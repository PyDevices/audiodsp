# Render on several threads

You can render on as many threads as you like, as long as **no node is used
from two threads at once**. Give each thread its own synthesizer, effects
and mixer, and each renders exactly the bytes it would render alone, LFOs
included: synthio's block clock belongs to the render in progress on each
thread, not to the process.

On a free-threaded build (3.15t has wheels), the native extension declares
that it doesn't need the GIL, so those threads run in parallel and importing
audiodsp leaves the GIL off. Every call into a native node holds a lock on
that node, so reconfiguring a node while another thread renders it won't
crash the interpreter. What comes out of that is still undefined, though,
because it breaks the rule above. [tests/test_cpython_free_threading.py](../../tests/test_cpython_free_threading.py) is
the stress test that holds both, sixteen instruments on sixteen threads.
