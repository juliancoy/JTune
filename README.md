# JTune

Real-time C++ autotune app using the existing `RtAudio` dependency from the `Loiacono` submodule.

## Build

```bash
git submodule update --init --recursive
./build.sh
```

Default directories:
- Ninja builds go in `build/` (default).
- Non-Ninja CMake trees should go in `build-cmake/`, for example:

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
```

## Run

```bash
./build/jtune_autotune --strength 0.9 --algorithm loiacono
# or load an exact Scala definition and optional same-basename .kbm mapping
./build/jtune_autotune --tuning-file my-system.scl --reference-note 69 --reference-hz 432
```

## Live UI

Run a terminal live UI with input/output pitch history graph:

```bash
./build/jtune_live_ui --strength 1.0
```

Controls:
- `q`: quit

Useful options:
- `--history-size 240`
- `--bin-count 128`
- `--analysis-hop 256`
- `--algorithm loiacono|fft|goertzel`
- `--input-device ID --output-device ID`

## Qt GUI

Run the desktop Qt GUI (controls + live input/output pitch graph):

```bash
./build/JTune
```

In the GUI:
- Select API and devices
- Select a pitch system (temperament/tuning practice)
- Optionally select a compatible pitch collection and tonic, or edit individual active degrees
- Set the independent reference note/frequency, strength, and DSP settings
- Import strict `.scl`/same-basename `.kbm` definitions or provenance-rich measured `.json`
- Inspect stable ID, version, source, license, appropriate use, review status, and limitations
- Open `Settings...` for comprehensive Loiacono transform/audio options
  (`compute mode`, `algorithm`, `window`, `normalization`, `window length`,
  `leakiness`, `reference pitch`, `freq range`, `multiple`, `bins`, and stream flags)
- Press `Start`

Arguments:
- `--pitch-system`: stable built-in or imported definition ID
- `--tuning-file`: `.scl` or measured `.json`; a same-basename `.kbm` is loaded automatically
- `--reference-note`, `--reference-hz`, and `--octave-shift`: explicit mapping controls
- `--key` and `--scale` have been removed; they never silently filter targets
- `--strength`: `0.0..1.0`
- `--pitch-collection`: `all`, `12edo.ionian`, `12edo.dorian`,
  `12edo.phrygian`, `12edo.lydian`, `12edo.mixolydian`, `12edo.aeolian`,
  `12edo.locrian`, or `custom`
- `--tonic-note`: MIDI note used as collection degree zero
- `--degrees`: comma-separated custom degree offsets, such as `0,2,4,5,7,9,11`
- `--sample-rate`: default `48000`
- `--buffer-frames`: default `512`
- `--min-midi` / `--max-midi`: detection range
- `--multiple`: transform window multiple for detection
- `--voiced-threshold`: higher = less aggressive correction in noisy input

## Dear ImGui live effects UI

Run the low-latency Dear ImGui control surface with:

```bash
./build/jtune_imgui
```

Select input and output devices, choose a pitch system and an independent
reference note/frequency, then adjust
the correction and resynthesis parameters, and press `Start audio`. Parameter
changes made while streaming take effect when `Apply DSP` is pressed; the new
processor is prepared on the UI thread and swapped at an audio-buffer boundary.
The live dashboard plots detected, corrected, and target pitch positions and
breaks the current latency estimate into driver, DSP-lookback, and callback
components. The stream defaults to two driver buffers and exposes the actual
buffer count and period reported after opening.
Imported pitch-system paths and stable IDs are persisted. The ImGui panel shows
provenance, license, review state, appropriate use, and limitations.

Built-ins are mathematical definitions only. Culturally named measured or
theoretical datasets are accepted only through the provenance-bearing format;
JTune does not invent a generic gamelan, makam, or raga table.
Use `Hard tune preset` for full-wet, immediate note locking. It also holds the
last valid correction through short vocal-confidence dropouts so consonants and
breathy frames do not briefly leak the original glide.
The reference-drone controls generate a click-free post-effect tone for pitch
practice. They start at G3 (196 Hz); choose a note, octave, and level, then use
`Start G3 drone`. The button opens the selected audio stream automatically if
it is not already running.

The effects section contains the complete JSynth effects chain: delay with
ping-pong mode, amp modeling and tone filtering, soft clipping, fuzz, tremolo,
chorus, phaser, bit crushing, granulation, and reverb. Each processor exposes
its JSynth parameters, and the chain can place drive/modulation before or after
delay/reverb.

The signal path is intentionally `input -> autotune -> effects -> reference
drone -> output`. Chorus and flanging-style modulation, delay, reverb, amp
modeling, fuzz, and clipping all remain after pitch correction so their added
harmonics, echoes, and modulation cannot mislead pitch detection. The reference
drone is mixed after the rack so it remains a clean tuning reference.

## Notes

- Mono input/output path in this first C++ version.
- This implementation is intended as a practical low-latency prototype and can be extended with higher-quality formant-preserving pitch shift.

## Tests

After building, run the complete suite with:

```bash
ctest --test-dir build --output-on-failure
```

The `streaming_autotune` test sends a four-second synthetic off-scale chirp to
the same 256-frame `processBuffer` path used by the live audio callback. It
independently checks output-pitch accuracy and correction latency, and reports
real-time factor, callback percentiles and jitter, deadline misses, and the
first correction decision and sustained corrected-output times. Settled output
must remain within 5 median cents and 15 p95 cents of the target note.
