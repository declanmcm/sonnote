# Sonnote: A composition software based on pure intervals

**Author:** Declan McMahon  
**License:** MIT  
**Version:** 1.0.0  

## Overview

**Sonnote** is a standalone program using Vulkan and JACK to provide a composition tool based on pure intervals.

## Compilation

```
cmake -S . -B build (-DCMAKE_BUILD_TYPE=RelWithDebInfo)
cmake --build build -j
./build/app
```

---

# User guide

Almost everything in the note window is driven by the mouse cursor plus a
single key: hover over the thing you want to act on and press the key. "The
closest note" below means the note under the cursor's time that is nearest
the cursor vertically.

## The screen

- **Menu bar** (top): `File` menu, `Rec` button, `Automation (Tab)` button.
- **Track list**: one row per track.
- **Control panel**: BPM, reference frequency, jump / split sizes, harmonic
  ranges, the current track's sound (oscillators, Pd patch, VST3 instrument
  and effects, tuning).
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
| **Space** | Play / stop |
| **p** | Move the playhead to the cursor |
| **u** | Set the loop end at the cursor (press again at the same spot to clear it) |
| **k** | Metronome on / off |
| **t** | Tuner on / off |
| **n** | Freeze on / off |

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
- **BPM** slider: the tempo.

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
- **Inst / FX**: VST3 instrument and effects, each with an **Edit** button
  for the plugin window.
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

## Files

The `File` menu: **Save project** / **Load project** (`session.son`),
**Export WAV** (to the name in *Wav name*), **Export MIDI** (`export.mid`).

## Escape and quitting

Esc closes / cancels, in order: text editing, a clip edit / note move /
selection box, an open menu, the note selection, open plugin windows,
automation mode. With nothing left to close, Esc (or closing the window)
asks before quitting: **Enter** saves to `session.son` and quits, **Esc**
quits without saving, any other key or a click cancels.
