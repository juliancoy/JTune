# JTune

Offline autotune app built around the `Loiacono` transform implementation included as a Git submodule.

## Setup

```bash
git submodule update --init --recursive
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip
pip install -e .
```

## Usage

```bash
jtune input.wav output/tuned.wav --key D --scale minor --strength 1.0
```

Parameters:
- `--strength`: `0.0` keeps original pitch, `1.0` fully snaps to key.
- `--min-midi` / `--max-midi`: vocal range to analyze.
- `--frame-ms`: pitch analysis frame size.
- `--voiced-threshold`: confidence threshold before correction is applied.

## Notes

- This is an offline processor for `.wav` files.
- Pitch detection uses `vendor/Loiacono/loiacono.py`.
- Correction is scale-aware (major/minor) and key-aware (`--key`).
