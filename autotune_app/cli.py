import argparse
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

REPO_ROOT = Path(__file__).resolve().parent.parent
LOIACONO_PATH = REPO_ROOT / "vendor" / "Loiacono"
if str(LOIACONO_PATH) not in sys.path:
    sys.path.insert(0, str(LOIACONO_PATH))

from loiacono import Loiacono  # noqa: E402


def midi_to_hz(midi_note: float) -> float:
    return 440.0 * (2.0 ** ((midi_note - 69.0) / 12.0))


def hz_to_midi(freq_hz: float) -> float:
    return 69.0 + 12.0 * np.log2(freq_hz / 440.0)


def key_allowed_pitch_classes(key_root: int, scale: str) -> set[int]:
    major = {0, 2, 4, 5, 7, 9, 11}
    minor = {0, 2, 3, 5, 7, 8, 10}
    base = major if scale == "major" else minor
    return {(n + key_root) % 12 for n in base}


def nearest_target_midi(detected_midi: float, allowed_pitch_classes: set[int]) -> float:
    candidates = []
    center = int(round(detected_midi))
    for n in range(center - 24, center + 25):
        if n % 12 in allowed_pitch_classes:
            candidates.append(n)
    return min(candidates, key=lambda n: abs(n - detected_midi))


def estimate_midi(frame: np.ndarray, detector: Loiacono, midi_values: np.ndarray) -> tuple[float, float]:
    dtftlen = detector.DTFTLEN
    if frame.shape[0] < dtftlen:
        frame = np.pad(frame, (0, dtftlen - frame.shape[0]))
    elif frame.shape[0] > dtftlen:
        frame = frame[:dtftlen]

    detector.run(frame)
    spectrum = detector.spectrum
    idx = int(np.argmax(spectrum))
    return midi_values[idx], float(spectrum[idx])


def autotune_mono(
    y: np.ndarray,
    sr: int,
    strength: float,
    min_midi: int,
    max_midi: int,
    multiple: int,
    key_root: int,
    scale: str,
    frame_ms: float,
    hop_ratio: float,
    voiced_threshold: float,
) -> np.ndarray:
    frame_len = max(512, int(sr * frame_ms / 1000.0))
    hop_len = max(128, int(frame_len * hop_ratio))
    if hop_len >= frame_len:
        hop_len = frame_len // 2

    allowed = key_allowed_pitch_classes(key_root, scale)
    window = np.hanning(frame_len)
    output = np.zeros_like(y, dtype=np.float64)
    norm = np.zeros_like(y, dtype=np.float64)
    midi_values = np.arange(min_midi, max_midi + 1, dtype=float)
    freq_hz = midi_to_hz(midi_values)
    fprime = freq_hz / float(sr)
    detector = Loiacono(
        fprime=fprime,
        dtftlen=int(np.ceil(multiple / fprime[0])),
        multiple=multiple,
    )

    for start in range(0, max(1, len(y) - frame_len + 1), hop_len):
        frame = y[start : start + frame_len]
        if frame.shape[0] < frame_len:
            break

        detected_midi, confidence = estimate_midi(
            frame=frame,
            detector=detector,
            midi_values=midi_values,
        )

        target_midi = nearest_target_midi(detected_midi, allowed)
        semitones = (target_midi - detected_midi) * strength

        if confidence < voiced_threshold or abs(semitones) < 0.05:
            tuned = frame
        else:
            tuned = librosa.effects.pitch_shift(frame.astype(np.float32), sr=sr, n_steps=float(semitones))
            tuned = tuned[:frame_len]
            if tuned.shape[0] < frame_len:
                tuned = np.pad(tuned, (0, frame_len - tuned.shape[0]))

        output[start : start + frame_len] += tuned * window
        norm[start : start + frame_len] += window

    norm = np.where(norm < 1e-8, 1.0, norm)
    out = output / norm
    return np.clip(out, -1.0, 1.0).astype(np.float32)


def parse_key(key: str) -> int:
    keys = {
        "C": 0,
        "C#": 1,
        "Db": 1,
        "D": 2,
        "D#": 3,
        "Eb": 3,
        "E": 4,
        "F": 5,
        "F#": 6,
        "Gb": 6,
        "G": 7,
        "G#": 8,
        "Ab": 8,
        "A": 9,
        "A#": 10,
        "Bb": 10,
        "B": 11,
    }
    if key not in keys:
        raise ValueError(f"Unsupported key '{key}'. Use values like C, C#, D, Eb, F#, A, Bb.")
    return keys[key]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="JTune: autotune audio using Loiacono-based pitch detection.")
    parser.add_argument("input", type=Path, help="Input wav file")
    parser.add_argument("output", type=Path, help="Output wav file")
    parser.add_argument("--key", default="C", help="Key root, e.g. C, F#, Bb")
    parser.add_argument("--scale", default="major", choices=["major", "minor"], help="Target scale")
    parser.add_argument("--strength", type=float, default=1.0, help="0.0 keeps original pitch, 1.0 fully snaps")
    parser.add_argument("--min-midi", type=int, default=40, help="Lowest pitch to detect")
    parser.add_argument("--max-midi", type=int, default=84, help="Highest pitch to detect")
    parser.add_argument("--multiple", type=int, default=24, help="Loiacono transform period multiple")
    parser.add_argument("--frame-ms", type=float, default=80.0, help="Analysis frame size in ms")
    parser.add_argument("--hop-ratio", type=float, default=0.25, help="Hop length as fraction of frame size")
    parser.add_argument("--voiced-threshold", type=float, default=4.0, help="Detection confidence threshold")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    y, sr = librosa.load(args.input, sr=None, mono=False)

    if y.ndim == 1:
        channels = [y]
    else:
        channels = [y[i] for i in range(y.shape[0])]

    key_root = parse_key(args.key)
    tuned_channels = [
        autotune_mono(
            y=ch,
            sr=sr,
            strength=args.strength,
            min_midi=args.min_midi,
            max_midi=args.max_midi,
            multiple=args.multiple,
            key_root=key_root,
            scale=args.scale,
            frame_ms=args.frame_ms,
            hop_ratio=args.hop_ratio,
            voiced_threshold=args.voiced_threshold,
        )
        for ch in channels
    ]

    if len(tuned_channels) == 1:
        out = tuned_channels[0]
    else:
        out = np.stack(tuned_channels, axis=1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    sf.write(args.output, out, sr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
