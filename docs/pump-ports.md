# Porting the audio pump to a new platform

The loop that pulls an effect graph block by block is portable C and it is
already in your build. What it cannot know is what a thread is on your
platform, what a mutex is, where the clock comes from and where the audio
goes. You supply those four, and nothing else.

Write one C file. Define `audioif_port_driver()` from
[`../src/shared/audioif_port.h`](../src/shared/audioif_port.h), return an
`audioif_port_ops_t` filled in with whatever your platform has, and put the
table in the **same translation unit** as your MicroPython module
registration — that last part is not style, and [How it binds](#how-it-binds-and-the-trap-it-dodges)
says why. Then ask the firmware you built what it got:

```python
>>> import audiopump
>>> audiopump.driver()
'rp2'
```

A driver that did not bind says `'none'` instead of quietly pumping nothing,
so that one line is the whole acceptance test for the wiring.

## The smallest port is no port at all

**Every hook may be NULL, and a table of nothing but NULLs is a complete and
correct port.** One thread, no hardware, and the lock compiles down to a load
and a branch. `spawn()` then adopts the graph, returns `-2` and sets service
mode; `audiopump.service()` advances the loop on the calling thread. That is
how the WebAssembly build has always run, and it is what the CPython wheel
gets.

So start with nothing filled in. Confirm the graph plays through `service()`
and the bytes are what the same graph produces anywhere else, then add hooks
until it plays by itself.

## The hooks

Sixteen function pointers and a name. The name is the only field that may not
be NULL.

### The mutex — `lock_take`, `lock_give`

Recursive, and priority-inheriting where the OS has it. The pump runs above
the interpreter on a board, so an interpreter holding a swap has to be lifted
to finish it.

The contract these two serve is in
[`../src/shared/audioif_pump_lock.h`](../src/shared/audioif_pump_lock.h) and it
is small: the pump holds the lock for one block pull, the control path holds
it around its final swap only, and nothing is ever held across an allocation
or across anything that can raise. Recursive because a node's pull
legitimately re-enters helpers that lock, and neither side should have to know
about the other.

NULL gives you no exclusion at all, which is the right answer when nothing
else runs.

### The clock — `now_us`

Monotonic microseconds. NULL means no clock: every duration the engine
publishes reads zero, which is honest on a port that cannot time itself.

### The thread — `thread_start`, `thread_wake`, `thread_release`, `self_id`, `status_tid`, `stack_free`

`thread_start` is the one hook whose absence changes the *shape* of the
engine. With it, `spawn()` returns a thread identifier and the loop runs by
itself; without it, `spawn()` returns `-2` and the loop is service-mode only.

| hook | what it must guarantee |
|---|---|
| `thread_start` | Calls `entry` once, on the new thread; `entry` returns when the loop is done. Writes `*where` — the core it pinned to, or `-1` for "a thread, no affinity" — which is what `spawn()` hands back to Python. |
| `thread_wake` | Wakes a pump that is parked or waiting to notice `stop`. |
| `thread_release` | Joins or deletes the thread and frees what it held. Called only after the engine has seen the loop finish, or has given up waiting, and must be safe when no thread was ever started. |
| `self_id` | Any value that is stable per thread and different between the two threads that matter; `0` means "nobody". It only ever answers *am I on the pump thread*. |
| `status_tid` | What the status block's thread word reports. A different question from `self_id`: on esp32 the useful answer is which core the pump landed on, not the task handle. |
| `stack_free` | Stack the pump thread never used, in whatever unit the port counts. |

### Waiting — `park_spin`, `sleep_us`

`park_spin` is the pump's own wait while parked. It must return when
`thread_wake()` is called, and it may return early — a spin is a legal
implementation, a sleep that misses a wake is not.

**Fill it in even if your port never parks.** It is also the wait the loop
takes when the output ring is full, so it is what makes `backpressure()` True
— and a driver that starts a thread and leaves `park_spin` NULL gets a
free-running pump that drops blocks instead of waiting for room. That is the
one hook whose absence is reported to Python: `audiopump.backpressure()` is
True on a threaded port that has it and True in service mode, where the loop
hands the thread back on a full ring and the caller's next `service()` is the
wake, and False only in that unfinished case.

`sleep_us` is the control side's wait: pacing, and the bounded waits in
teardown. It **must not raise**, because one of its callers is a finaliser.

### The sink — `sink_open`, `sink_close`, `sink_ready`, `sink_write`, `sink_dma_bytes`, `sink_rx_bytes`

One write, whatever it is written to. On a desktop that is the file
`sink_open()` opened; on a board it is a peripheral your own Python surface
opened, and `sink_open` is NULL there. `sink_ready` is true when a hardware
sink is open and a block may be written to it.

`sink_dma_bytes` and `sink_rx_bytes` are what the hardware actually clocked
out and in. They are the only way an underrun gets measured at all — bytes the
DMA sent that the pump never wrote *are* the silence a listener heard — and
they read zero on a port with no DMA to ask.

### Letting go — `teardown`

Called from the engine's teardown, which a soft reset reaches through a
finaliser. Drop whatever the VM cannot: a channel, a timer, a handle. It must
not raise and it must be safe to call twice.

### What the four shipped ports fill in

| hook | esp32 | win32 | pthread | none |
|---|---|---|---|---|
| `lock_take`/`lock_give` | `xSemaphoreTakeRecursive` on a static priority-inheriting mutex | `CRITICAL_SECTION` | recursive `pthread_mutex` | — |
| `now_us` | `esp_timer_get_time` | `QueryPerformanceCounter` | `clock_gettime` | — |
| `thread_start` | `xTaskCreatePinnedToCore[WithCaps]` | `_beginthreadex` | `pthread_create` | — |
| `thread_wake` | `xTaskNotifyGive` | — | — | — |
| `thread_release` | `vTaskDelete[WithCaps]` | `WaitForSingleObject` + `CloseHandle` | `pthread_join` | — |
| `self_id` / `status_tid` | task handle / **core** | thread id | `pthread_self` | — |
| `stack_free` | `uxTaskGetStackHighWaterMark` | — | — | — |
| `park_spin` | `ulTaskNotifyTake` | `SwitchToThread` | `sched_yield` | — |
| `sleep_us` | `vTaskDelay` / `esp_rom_delay_us` | waitable timer | `nanosleep` | — |
| `sink_open`/`close` | — (the channel is its own surface) | `_open` + `_O_BINARY` | `open` | — |
| `sink_ready`/`sink_write` | `i2s_channel_write` | `_write` | `write` | — |
| `sink_dma_bytes`/`rx_bytes` | the ISR counters | — | — | — |
| `teardown` | close the channel | close the pace timer | — | — |

All four are one file, so reading it beside this page is the fastest way in.

## How it binds, and the trap it dodges

Weak symbols. The engine defines `audioif_port_driver()` weakly and your
driver defines it strongly, so the linker keeps yours.

The trap is real and it is why the rule about the translation unit exists: in
a **static archive** the weak default can satisfy the reference first, your
object is never pulled into the link at all, and the default wins *silently* —
a firmware that compiles, links and plays nothing, with nothing in the build
log to read. Putting the hook table beside `MP_REGISTER_MODULE` dodges it,
because the firmware's module table holds an undefined reference to your
module object, so your object is always pulled in, archive or not.

That is an argument rather than a proof, so it is checked at run time on every
build. Four of the six are proven by running them:

| build | `audiopump.driver()` | `threaded()` | `spawn()` returns | proof |
|---|---|---|---|---|
| unix | `pthread` | True | −1, a real thread id | run |
| unix, driver not linked | `none` | False | −2, `service()` drives | run |
| windows | `win32` | True | −1 | run |
| webassembly | `none` | False | −2 | run |
| ESP32-P4 | `esp32` | — | — | run, at an earlier commit |
| ESP32-S3 | — | — | — | link map |

On the **Waveshare ESP32-P4 panel**, an earlier commit of this engine did bind
on silicon: `audiopump.driver()` said `esp32`, the five storm digests came
back as that board's published values to the byte, and the mutex was real — a
control call waited 2171 µs with the pump running and 0 µs without.

**No image of the current line has been flashed, on either chip.** Both
compile and link, and there the check is the link map: the weak
`.text.audioif_port_driver` from `audioif_port.c.obj` is in the **discarded**
sections and the symbol resolves to the driver's object on both.

One more thing worth knowing before you key your driver off a macro. A user C
module is compiled without `ESP_PLATFORM`, and a POSIX branch *links* on
esp32 because the IDF's newlib has `pthread.h` — so the wrong macro gets you
an unpinned pump on a default stack with no sink and nothing saying so. That
cost this spike a whole firmware once, and it is most of the reason
`audiopump.driver()` exists at all. Define your own macro from your build
glue.

## The portability gate

Nothing in this repository's C may include a platform header, and that is
enforced rather than promised:

```sh
python3 tools/check_portable.py
```

[`../tools/check_portable.py`](../tools/check_portable.py) fails on
`freertos/`, `esp_*`, `driver/`, `sdkconfig`, `pthread.h`, `windows.h`,
`unistd.h`, `sys/time.h`, and on the two dozen call names those headers bring
— `clock_gettime`, `nanosleep` and the rest. Comments and string literals are
stripped before the scan, so a file may explain the rule without breaking it,
which is how the header above can name every one of them.

Three runs stand behind it: clean across 280 files; **three** hits with a
`<pthread.h>` and a `clock_gettime()` planted in the lock, so the gate is
armed rather than merely quiet; and **one** hit on the state of the repository
before the split — `src/audiomp3/MP3Decoder.c` had carried `#include
<unistd.h>` all along for `SEEK_SET`, which is ISO C and is `<stdio.h>` now.
