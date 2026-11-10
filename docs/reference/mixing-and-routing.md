# Mixing and routing

What the modules that combine, split and steer streams take and return.
`audiomixer.Mixer` plays several sources at once; `audioroute` fans a stream
out, re-points a wire and scales stereo width. To change what plays without rewiring, see
[Change what plays without rewiring](../guides/swap-what-plays.md).

## `audiomixer`

```{eval-rst}
.. automodule:: audiomixer
```

## `audioroute`

```{eval-rst}
.. automodule:: audioroute
   :exclude-members: MidSide
```
