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
./build/jtune_autotune --key D --scale minor --strength 0.9 --algorithm loiacono
```

## Live UI

Run a terminal live UI with input/output pitch history graph:

```bash
./build/jtune_live_ui --key C --scale major --strength 1.0
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
- Set key/scale/strength and DSP settings
- Open `Settings...` for comprehensive Loiacono transform/audio options
  (`compute mode`, `algorithm`, `window`, `normalization`, `window length`,
  `leakiness`, `base A`, `freq range`, `multiple`, `bins`, and stream flags)
- Press `Start`

Arguments:
- `--key`: `C C# Db D D# Eb E F F# Gb G G# Ab A A# Bb B`
- `--scale`: `major` or `minor`
- `--strength`: `0.0..1.0`
- `--sample-rate`: default `48000`
- `--buffer-frames`: default `512`
- `--min-midi` / `--max-midi`: detection range
- `--multiple`: transform window multiple for detection
- `--voiced-threshold`: higher = less aggressive correction in noisy input

## Notes

- Mono input/output path in this first C++ version.
- This implementation is intended as a practical low-latency prototype and can be extended with higher-quality formant-preserving pitch shift.
