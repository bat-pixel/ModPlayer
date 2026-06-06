# ModPlayer

A Windows desktop player for Amiga ProTracker MOD files, featuring a real-time oscilloscope visualizer and two switchable playback backends.

![ModPlayer screenshot](screenshots/Screenshot%202026-06-06%20082736.png)

## Features

- **Dual playback backends** — switch at runtime between a hand-written native ProTracker engine and [libopenmpt](https://lib.openmpt.org/libopenmpt/)
- **Oscilloscope visualizer** — per-channel waveform display and peak meter, double-buffered at ~30 fps
- **Amiga stereo panning** — channels 1 & 4 hard-panned left (blue), channels 2 & 3 hard-panned right (green)
- **Per-channel HUD** — shows channel number, stereo side, current note (e.g. `A-2`), and volume (e.g. `v64`)
- **Position display** — two-row banner shows current order, row, and total orders in real time
- **Song looping** — restarts from order 0 when the song ends
- **Recursive library scan** — locates `MODS/` relative to the executable (searches up to 4 directory levels)
- **Effect log panel** — shows any unimplemented effects used by the loaded MOD

## Controls

| Key | Action |
|-----|--------|
| `Space` / `→` | Next track |
| `←` / `Backspace` | Previous track |
| `P` | Pause / resume |
| `B` | Toggle backend (Native ↔ libopenmpt) |

## Supported Effects (Native Backend)

| Effect | Name |
|--------|------|
| `0xy`  | Arpeggio |
| `1xx`  | Portamento Up |
| `2xx`  | Portamento Down |
| `3xx`  | Portamento to Note |
| `4xx`  | Vibrato (sine / ramp-down / square / random) |
| `5xx`  | Portamento to Note + Volume Slide |
| `6xx`  | Vibrato + Volume Slide |
| `7xx`  | Tremolo |
| `8xx`  | Set Panning (0x00 = full left, 0x80 = centre, 0xFF = full right) |
| `9xx`  | Sample Offset |
| `Axx`  | Volume Slide |
| `Bxx`  | Jump to Order |
| `Cxx`  | Set Volume |
| `Dxx`  | Pattern Break |
| `E1x`  | Fine Portamento Up |
| `E2x`  | Fine Portamento Down |
| `E3x`  | Glissando Control |
| `E4x`  | Set Vibrato Waveform |
| `E5x`  | Set Finetune |
| `E6x`  | Pattern Loop |
| `E7x`  | Set Tremolo Waveform |
| `E9x`  | Retrigger Note |
| `EAx`  | Fine Volume Slide Up |
| `EBx`  | Fine Volume Slide Down |
| `ECx`  | Note Cut |
| `EDx`  | Note Delay |
| `EEx`  | Pattern Delay |
| `Fxx`  | Set Speed / BPM |

The libopenmpt backend inherits full format support from libopenmpt (MOD, XM, S3M, IT, and many more).

## Building

**Requirements:** Visual Studio 2022, [vcpkg](https://vcpkg.io/) (x64-windows triplet)

**Dependencies (installed via vcpkg):**
- `libopenmpt`
- `mpg123`
- `libvorbis`

Open `ModPlayer/ModPlayer.vcxproj` in Visual Studio and build in Release or Debug configuration for x64.

## Tests

A standalone console test suite lives in `tests/test_audio.cpp`. Compile it alongside `ModPlayer/ModParser.cpp` and `ModPlayer/ModMixer.cpp` (no GUI dependencies). Run from the repo root:

```
tests\bin\test_audio.exe
```

Or pass a specific MOD file as the first argument:

```
tests\bin\test_audio.exe "MODS\1987\mod.Ackerlight"
```

The suite verifies parser correctness, non-silent output, amplitude bounds, NaN/Inf detection, output determinism, stereo panning, and visualiser data — 22 tests, all passing.

## MOD Files

Place MOD files anywhere inside the `MODS/` directory (subdirectories are scanned recursively). Files are recognised by a `.mod` extension or a `mod.` prefix. The application searches for `MODS/` starting from the executable's directory and walking up to 4 levels, so it works from any standard Visual Studio build output path.
