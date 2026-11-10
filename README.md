# Sonnote: A composition software based on pure intervals

**Author:** Declan McMahon  
**License:** MIT  
**Version:** 1.0.0  

## Overview

**Sonnote** is a standalone program using Vulkan and JACK to provide a composition tool based on pure intervals.

## Platform support

Linux only, on X11 or Wayland (VST3 plugin windows open through XWayland).
Audio goes through JACK, so either JACK itself or PipeWire's JACK layer
(`pipewire-jack`, run as `pw-jack ./build/app` if it isn't the default).

## Dependencies

### Bundled as a git submodule

- [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) (MIT), in
  `extern/vst3sdk`, pinned to `v3.8.1_build_84`. Only its `base`, `cmake`,
  `pluginterfaces` and `public.sdk` parts are needed.

`json.hpp` ([nlohmann/json](https://github.com/nlohmann/json)), `stb_image.h`
and `stb_truetype.h` ([nothings/stb](https://github.com/nothings/stb)) are
single headers committed in `src/`.

### System packages

Everything in one line:

**Arch**

```
sudo pacman -S --needed cmake gcc pkgconf sdl2-compat vulkan-icd-loader vulkan-headers pipewire-jack libsndfile freetype2 fftw pd libx11 libglvnd tk
```

plus the Vulkan driver for your GPU: `vulkan-radeon` (AMD), `vulkan-intel`
(Intel) or `nvidia-utils` (NVIDIA). If you use `jack2` instead of PipeWire,
leave out `pipewire-jack` (pacman would offer to replace `jack2` with it).

**Ubuntu / Debian**

```
sudo apt install cmake g++ pkg-config libsdl2-dev libvulkan-dev mesa-vulkan-drivers libjack-jackd2-dev pipewire-jack libsndfile1-dev libfreetype-dev libfftw3-dev libpd-dev libx11-dev libgl-dev tk
```

(`mesa-vulkan-drivers` covers AMD and Intel GPUs; NVIDIA needs its own driver.)

What each package is for:

| Needed for | Arch | Debian / Ubuntu |
|---|---|---|
| Building | `cmake gcc pkgconf` | `cmake g++ pkg-config` |
| Window and graphics | `sdl2-compat vulkan-icd-loader vulkan-headers` | `libsdl2-dev libvulkan-dev` |
| Vulkan driver | `vulkan-radeon`, `vulkan-intel` or `nvidia-utils` | `mesa-vulkan-drivers` (or the NVIDIA driver) |
| Audio | `pipewire-jack` (or `jack2`), `libsndfile` | `libjack-jackd2-dev pipewire-jack libsndfile1-dev` |
| Fonts, tuner | `freetype2 fftw` | `libfreetype-dev libfftw3-dev` |
| Pd patches ([libpd](https://github.com/libpd/libpd)) | `pd` (includes libpd) | `libpd-dev` |
| Plugin windows, OpenGL | `libx11 libglvnd` | `libx11-dev libgl-dev` |
| Pd editor (optional) | `tk` | `tk` |

libpd needs to be built with multi-instance support (`PD_MULTI`); without it
Sonnote still runs but Pd patches don't play.

Optional: the Vulkan validation layer (`vulkan-validation-layers` on Arch,
`vulkan-validationlayers` on Debian / Ubuntu) is used when installed, which
prints Vulkan warnings to the terminal. Set `SONNOTE_VALIDATION=0` to turn it
off.

## Installation

```
git clone https://github.com/declanmcm/sonnote.git
cd sonnote
git submodule update --init
git -C extern/vst3sdk submodule update --init base cmake pluginterfaces public.sdk
```

(Don't use `--recursive`: it also downloads the SDK's `vstgui4`, `doc` and
`tutorials`, which Sonnote doesn't need.)

## Compilation

```
cmake -S . -B build (-DCMAKE_BUILD_TYPE=RelWithDebInfo)
cmake --build build -j
./build/app
```

It can be launched from any folder: Sonnote finds its own folder from the
executable and works from there, so relative paths (`recordings/`, `export.mid`,
and those typed into the save and load prompts) start there too.

---

# User guide

Almost everything in the note window is driven by the mouse cursor plus a
single key: hover over the thing you want to act on and press the key. "The
closest note" below means the note under the cursor's time that is nearest
the cursor vertically.

## The screen

- **Menu bar** (top): `File` menu, `Rec` button, `Automation (Tab)` button,
  `Tempo` and `Master gain` buttons (see *Global lanes*), `FX chain (b)`
  button (toggles the signal chain bar at the bottom).
- **Track list**: one row per track.
- **Control panel**: BPM, reference frequency, jump / split sizes, harmonic
  ranges, the current track's sound (oscillators, Pd patch, VST3 instrument,
  tuning).
- **Note window**: x is time, y is pitch in octaves (up = higher). Below the
  notes are the **perc strip** (one lane per perc track) and the **audio
  strip** (one lane per audio track).

## Navigating

| Action | How |
|---|---|
| Pan | Mouse wheel (both axes), or drag with the right mouse button |
| Zoom | Hold **Left Ctrl** and use the mouse wheel |
| Scroll the perc / audio strip lanes | Mouse wheel over the strip |
| Grow / shrink the strip under the cursor | **Up / Down** arrows |

## Playback

| Key | Action |
|---|---|
| **Space** | Play / pause. The playhead stays where it stopped and playing carries on from there |
| **p** | Move the playhead to the closest beat or rhythm split, also while playing |
| **Shift + p** | Move the playhead exactly to the cursor, without snapping |
| **u** | Set the loop end at the cursor (press again at the same spot to clear it) |
| **k** | Metronome on / off |
| **t** | Tuner on / off |
| **n** | Freeze on / off |

A loop goes back to where **p** last put the playhead, however many times
you've paused since.

## Writing notes

Notes are written on the current track (or, if the current track isn't a
Standard track, the first Standard track).

- **Place a note:** left-click to set the start (and pitch), left-click again
  to set the end. The red dot shows where the click will land.
- Times snap to beats and rhythm splits; pitches snap to the harmonic lines
  (see *Harmonics and foci*).

| Key (over a note) | Action |
|---|---|
| **d** | Delete the closest note |
| **f** | Pick the note up to move it: it follows the cursor's pitch, keeping its start and length. Click to put it down |
| **l** | Change the note's length: picks up the nearer end; click to set the new end |
| **j** | Transpose the note by the *Jump size* interval (e.g. `3/2`) |
| **g** | Type a gain in dB for the note (or the selected notes), Enter to apply, Esc to cancel |
| **1–9, 0** | Move the note to track 1–10 (Standard tracks only). This also selects that track |
| **e** | Measure an interval (see below) |

### Selecting, moving, copying and deleting

| Key / mouse | Action |
|---|---|
| **Shift + left-drag** | Draw a box: notes overlapping it (in time) whose pitch is inside it are added to the selection |
| **a** | Toggle selection of the closest note (selected notes get a yellow outline) |
| **Esc** | Clear the selection |
| **m** | Pick up the selected notes (or the closest note, if none are selected) to move them in time. White outlines show where they'll land; **click** to put them down, **Esc** to cancel. Pitches and spacing are kept |
| **r** | Delete all selected notes (and their foci, as with **d**) |
| **c** | Copy the selected notes (or, if none are selected, the closest note) |
| **v** | Paste the copied notes with the earliest one starting at the cursor's beat. Pitches are kept exactly |

**Moving a section of music:** Shift-drag a box around it (more boxes or
**a** to add to it), hover anywhere, press **m**, move the cursor by the
amount you want to shift it, and click. To duplicate it instead, use **c**
and then **v** at the destination. Foci aren't moved with the notes.

### Undo / redo

| Key | Action |
|---|---|
| **Ctrl+Z** | Undo |
| **Ctrl+Shift+Z** or **Ctrl+Y** | Redo |

The last 5 edits can be undone: placing, deleting (`d`, `r`), moving (`f`/`l`/`m`),
transposing (`j`), re-tracking and gain changes of notes; foci; splits;
pasting; perc blips; clip edits and recorded clips. Tempo, track settings,
automation and selection aren't covered. Deleting or reordering tracks, and
loading a project, clear the history.

## Measuring intervals (e)

1. Hover a note or a harmonic line and press **e** to pick the first point
   (marked with a dot).
2. Move to another note or harmonic line: the interval from the first point
   is shown at the bottom right as `ratio (ratio reduced to within an
   octave)`.
3. Press **e** again to fix the second point; the reading stays until the next
   measurement.

Whichever is nearer the cursor is picked: the harmonic line it has snapped
to, or the closest note. Harmonic ends are highlighted orange, and the
reading turns orange when either end is a harmonic (pale yellow for note to
note).

The white reading at the bottom right is the harmonic currently under the
cursor relative to the focus. When hovering a note that was placed on a
focus's harmonic, a green line shows its *origin* (the focus note) and the
interval to it is shown at the bottom left.

## Harmonics and foci

- **Right-click a note** to make it a **focus** for the length of that note
  (drawn as an orange line). While a focus is active, its harmonic lines are
  drawn across the window and new notes snap to them.
- **h** switches between snapping to the foci's harmonics and snapping to the
  harmonics of the *fundamental* (the common root of the notes sounding at the
  time).
- **-** (minus) toggles *inverse harmony*: the harmonic lines go down from the
  focus (8/9, 4/5, ...) instead of up (9/8, 5/4, ...).
- **Harm. Range** (n): the intervals `i / 2^n` for `2^n ≤ i < 2^(n+1)`
  are drawn in every octave. `2` gives 5/4, 6/4, 7/4; `3` gives 9/8 ... 15/8.
- **Alt. H. Range**: extra interval families, as numbers or ranges (e.g.
  `3` or `3-7`). Each number N adds (N+1)/N, (N+2)/N, ... up to the octave.
- **Ref. Freq.**: the frequency of the 1/1 ratio.

## Rhythm

- **s** twice: the first press marks one end, the second splits the span
  between them into *Split size* equal parts (rhythm splits that notes snap
  to).
- **x**: remove the rhythm split under the cursor.
- **BPM** slider: the tempo, until the Tempo lane has points (see *Global
  lanes*). After that this spot shows the tempo at the playhead.

## Tracks

- Click a row (or press **1–9, 0**) to make it the current track. Double-click
  the name to rename it.
- **M** mute, **S** solo, **R** arm for recording (audio tracks), **x** delete
  (click twice to confirm).
- **+ Standard / + Audio / + Perc** add a track.
- **Shift+Up / Shift+Down** move the current track up / down the list.
- Each row has a gain slider (or, in automation mode, the `Auto:` parameter
  picker).

## Sound (control panel, current track)

- **OSC1–3**: click to change the waveform; Attack / Decay / Sustain /
  Release / Gain / Hold sliders and a Freq box.
- **Pd patch**: name of a Pure Data patch to play the track; **Edit in Pd**
  opens it (the patch reloads when its Pd window is closed).
- **Inst**: VST3 instrument of the current track, with an **Edit** button that
  opens its window. Its effects are in the signal chain bar.
- **FX chain (b)** in the menu bar: toggles the signal chain bar at the bottom,
  showing the current track's instrument and effects in processing order.
  Each card has a slider per parameter, so plugins without a window are usable
  too (the wheel scrolls them, **Find** filters them), and **UI** opens the
  plugin's window. **<** and **>** reorder effects, and **x** clicked twice
  removes one along with its automation. To add an effect, click it in the last
  card, or type in its **Find** box and press Enter, which takes the first
  match, or with no match tries the text as a name or a path to a `.vst3`.
  With Rec on, moving a slider during playback records automation, as moving
  the control in the plugin's window does. Shift + wheel scrolls along the
  chain if it doesn't fit.
- **Tuning**: how microtonal pitches are sent to a VST3 instrument (click to
  cycle).

## Percussion

In the perc strip, **left-click** in a lane to add a blip at the cursor's beat;
**d** over a blip deletes it.

## Audio clips and recording

1. Arm an audio track with **R** on its row. While armed, the track
   **monitors** its input: the live input plays through the track (its Pd
   patch or VST3 effects, gain, mute / solo) all the time, playing or not,
   without recording anything. Use headphones to avoid feedback.
2. Turn on **Rec** in the menu bar.
3. Press **Space**: input is recorded into the armed tracks until playback
   stops. A new clip replaces whatever part of existing clips it covers.

Over a clip in the audio strip: **l** trims its nearer edge, **m** moves it
(click to finish, Esc to cancel), **d** deletes it.

## Automation

Press **Tab** (or the `Automation (Tab)` button) to switch the note window to
automation for the current track; Tab again to go back.

- Use the track's **Auto:** button to choose what to automate: *Velocity*, or
  a plugin parameter (a picker opens; type to filter, Enter to pick).
- **Velocity:** drag a note's stem to set its velocity, or drag between stems
  to paint across them.
- **Parameters:** click / drag to draw, drag a point to move it, **d** to
  delete the point under the cursor.
- With **Rec** on, moving a control in a plugin window during playback records
  it into that parameter's lane.

## Global lanes

Two lanes belong to the whole song rather than a track. The `Tempo` and `Master
gain` buttons show them in automation mode; press the button again to go back
to the track's lane, or **Tab** to go back to the notes.

| Key / mouse | Action |
|---|---|
| **Click / drag** | Draw points |
| **Drag a point** | Move it |
| **g** | Type an exact value for the point under the cursor, or for a new point there. Enter applies, Esc cancels |
| **d** | Delete the point under the cursor |

- **Tempo** runs from 20 to 260 BPM. New points snap to beats, rhythm splits
  and whole BPM; hold **Shift** not to. Between two points the tempo changes
  smoothly; for a sudden change, put two points close together. Before the
  first point and after the last it stays level.
- **Master gain** scales everything but the metronome: the top of the lane is
  0 dB and the bottom is silence.

The note window's x axis is time, so a tempo change moves the notes on screen:
they keep their beats. This happens when you let go of the mouse. Clips also
keep their start and end beats, but they aren't stretched, so a clip can end
early if the tempo speeds up under it. MIDI export has the tempo too, with
ramps as small steps.

## Files

The `File` menu: **Save project** / **Load project** ask for a file (starting
from the last one used, or `session.son`): type a name or path, **Tab**
completes folders and `.son` files, **Enter** confirms, **Esc** cancels. Saving
adds `.son` to a name with no extension and asks before replacing another file.
**Export WAV** (to the name in *Wav name*), **Export MIDI** (`export.mid`).

## Escape and quitting

Esc closes / cancels, in order: text editing, a clip edit / note move /
selection box, an open menu, the note selection, open plugin windows,
automation mode. With nothing left to close, Esc (or closing the window)
asks before quitting: **Enter** asks for a file name, then saves and quits (Esc
there goes back), **Esc** quits without saving, any other key or a click cancels.
