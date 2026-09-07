# Quake PAPP MP3 decoder

This directory vendors the fixed-point OpenCore MP3 decoder used by the
ESPHome `micro-mp3` component. It is built directly into `quake.papp` so the
Quake build does not depend on an ESPHome component cache.

The decoder sources retain their upstream Apache License 2.0 notices. The
complete license and attribution files are `LICENSE` and `NOTICE` in this
directory.

Quake music files are loaded from `id1/music/trackNN.mp3`, for example
`id1/music/track02.mp3`. OGG files are not decoded by this PAPP build.
