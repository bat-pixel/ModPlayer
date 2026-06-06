# ModPlayer

A Windows desktop player for Amiga ProTracker MOD files with a directory browser, real-time Amiga demo effects synced to the music, oscilloscope visualiser, and two switchable playback backends.

## Features

- **libopenmpt backend (default)** — sounds identical to ffplay; handles MOD, XM, S3M, IT, and many more formats
- **Native ProTracker backend** — hand-written engine, switchable at runtime
- **Directory browser** — navigate the MODS folder hierarchy; single-click a file to play it
- **Clickable seek bar** — jump to any position in the song (libopenmpt backend)
- **5 Amiga demo effects** — all synced live to the music via per-channel volume and beat detection
- **Fullscreen borderless mode** — covers the primary monitor, hides the browser
- **Oscilloscope visualiser** — per-channel waveform + peak meter at ~30 fps
- **Amiga hard panning** — channels 1 & 4 left (blue), 2 & 3 right (green)
- **Recursive library scan** — finds `MODS/` up to 4 levels above the executable
- **Effect log** — flags any unimplemented effects in the loaded MOD

## Controls

| Key | Action |
|-----|--------|
| `Space` / `→` | Next track (flat list) |
| `←` / `Backspace` | Previous track |
| Single-click | Play file / navigate into folder (browser) |
| `Enter` | Play selected / navigate (browser keyboard) |
| `T` | Toggle browser panel |
| `F` / `F11` | Toggle fullscreen |
| `P` | Pause / resume |
| `B` | Toggle backend (libopenmpt ↔ Native) |
| `R` | Record player output → `tests/bin/player_recording.wav` (R again to stop) |
| `0` | Oscilloscope view (default) |
| `1` | Raster bars |
| `2` | 3D Starfield |
| `3` | Plasma |
| `4` | Copper scroller |
| `5` | Spectrum analyser |

## Demo Effects

All effects react live to the playing music:

| Effect | Music sync |
|--------|-----------|
| **Raster bars** | Bar position and height scale with per-channel volume; flash on each new row |
| **Starfield** | Warp speed driven by combined RMS energy; burst on beat; star colour tinted by L/R balance |
| **Plasma** | Frequency and speed scale with channel volumes; colour saturation from energy |
| **Copper scroller** | Scroll speed and sine-wave amplitude from music; copper gradient hue shifts with L/R |
| **Spectrum** | Fast-attack/slow-decay bars per frequency band; peak markers flash on beat |

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
| `E0x`  | LED Filter (E00 = off / bright, E01 = on / muffled) |
| `E1x`  | Fine Portamento Up |
| `E2x`  | Fine Portamento Down |
| `E3x`  | Glissando Control |
| `E4x`  | Set Vibrato Waveform |
| `E5x`  | Set Finetune |
| `E6x`  | Pattern Loop |
| `E7x`  | Set Tremolo Waveform |
| `E8x`  | Set Panning (0 = full left, 8 = centre, F = full right) |
| `E9x`  | Retrigger Note |
| `EAx`  | Fine Volume Slide Up |
| `EBx`  | Fine Volume Slide Down |
| `ECx`  | Note Cut |
| `EDx`  | Note Delay |
| `EEx`  | Pattern Delay |
| `Fxx`  | Set Speed / BPM |

Not implemented in the native backend: `EFx` (Funk Repeat — extremely rare, 56 files out of 4876 scanned). Switch to the libopenmpt backend with `B` for full compatibility.

## Building

**Requirements:** Visual Studio 2022, [vcpkg](https://vcpkg.io/) (x64-windows triplet)

**Dependencies (installed via vcpkg):**
- `libopenmpt`
- `mpg123`
- `libvorbis`

Open `ModPlayer/ModPlayer.vcxproj` in Visual Studio and build for x64 (Debug or Release).

## Tests and Audio Tools

All tools run from the repo root.

### Unit tests

```
tests\bin\test_audio.exe
tests\bin\test_audio.exe "MODS\Demos\Mod.Ackerlight 1.Mod"
```

22 tests: parser correctness, amplitude bounds, NaN detection, determinism, stereo panning, visualiser data.

### WAV rendering (native mixer)

```
tests\bin\test_audio.exe --wav out.wav "MODS\Demos\Mod.Ackerlight 1.Mod" 30
```

### WAV rendering (libopenmpt — identical to ffplay)

```
tests\bin\render_libopenmpt.exe "MODS\Demos\Mod.Ackerlight 1.Mod" --out out.wav --sec 30
```

### Audio comparison

```
python tests\compare_wav.py native.wav reference.wav --block-ms 200
python tests\zoom_wav.py native.wav reference.wav 12000
```

### Effect scanner (across entire MODS library)

```
python tests\scan_effects.py MODS
```

Reports effect usage counts across all files and highlights anything the native backend does not implement.

## MOD Files

Place MOD files anywhere under `MODS/` (subdirectories are scanned recursively). Files are recognised by a `.mod` extension or `mod.` prefix. `MODS/` is **not** tracked in git — it lives locally only.
