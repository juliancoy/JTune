# JTune

Real-time C++ autotune app using the existing `RtAudio` dependency from the `Loiacono` submodule.

## Build

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
./build/jtune_autotune --key D --scale minor --strength 0.9
```

Arguments:
- `--key`: `C C# Db D D# Eb E F F# Gb G G# Ab A A# Bb B`
- `--scale`: `major` or `minor`
- `--strength`: `0.0..1.0`
- `--sample-rate`: default `48000`
- `--buffer-frames`: default `256`
- `--min-midi` / `--max-midi`: detection range
- `--multiple`: transform window multiple for detection
- `--voiced-threshold`: higher = less aggressive correction in noisy input

## Notes

- Mono input/output path in this first C++ version.
- This implementation is intended as a practical low-latency prototype and can be extended with higher-quality formant-preserving pitch shift.
