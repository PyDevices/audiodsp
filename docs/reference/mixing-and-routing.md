# Mixing and routing

What the modules that combine, split and steer streams take and return.
`audiomixer.Mixer` plays several sources at once; `audioroute` fans a stream
out, re-points a wire and scales stereo width. To change what plays without rewiring, see
[Change what plays without rewiring](../guides/swap-what-plays.md).

## `audiomixer`

Generated from [`audiomixer.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiomixer/audiomixer.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiomixer
```

### Mixer voices

`Mixer.voice` returns these. The module has no public name for the type, so it is listed here under the private name the stub gives it.

```{eval-rst}
.. autoclass:: audiomixer._MixerVoice
```

## `audioroute`

Generated from [`audioroute.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioroute/audioroute.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audioroute
   :exclude-members: MidSide
```
