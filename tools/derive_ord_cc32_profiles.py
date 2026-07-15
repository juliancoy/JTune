#!/usr/bin/env python3
"""Derive conservative region/maqam candidates from the trusted ORD-CC32 artifact."""

import argparse
import csv
import hashlib
import io
import json
import math
import pickle
import re
import zipfile
from collections import defaultdict
from pathlib import Path

import numpy as np

EXPECTED_MD5 = "9f8b9cdadf88ddb93d50a90d4ddc544e"
DOI = "10.5281/zenodo.15682346"
EXCLUDED = ("CD 6/11.", "CD 6/12.", "CD 8/04.", "CD 15/01.", "CD 15/05.", "CD 18/04.")


def slug(value):
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def circular_distance(a, b):
    distance = abs(a - b)
    return min(distance, 1200.0 - distance)


def smooth(histogram):
    kernel = np.array([1, 2, 3, 2, 1], dtype=float)
    kernel /= kernel.sum()
    padded = np.concatenate((histogram[-2:], histogram, histogram[:2]))
    return np.convolve(padded, kernel, mode="valid")


def select_targets(histogram, target_count=7):
    values = smooth(histogram.astype(float))
    candidates = []
    for index, value in enumerate(values):
        if value >= values[index - 1] and value >= values[(index + 1) % len(values)]:
            candidates.append((float(value), index * 20.0))
    selected = [(float(values[0]), 0.0)]
    for strength, cents in sorted(candidates, reverse=True):
        if all(circular_distance(cents, existing) >= 80.0 for _, existing in selected):
            selected.append((strength, cents))
        if len(selected) >= target_count:
            break
    selected.sort(key=lambda item: item[1])
    return selected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    digest = hashlib.md5(args.archive.read_bytes()).hexdigest()
    if digest != EXPECTED_MD5:
        raise SystemExit(f"refusing untrusted pickle archive: MD5 {digest}, expected {EXPECTED_MD5}")

    with zipfile.ZipFile(args.archive) as archive:
        rows = list(csv.DictReader(io.TextIOWrapper(archive.open("allfiles_metadata.csv"), encoding="utf-8")))
        groups = defaultdict(list)
        for row in rows:
            mode = (row.get("mode") or "").strip()
            region = (row.get("region") or "").strip()
            path = row["path"]
            if not mode or mode == "NA" or not region or any(item in path for item in EXCLUDED):
                continue
            if row.get("vocal-only", "").strip().lower() == "true":
                continue
            pickle_name = path + ".pickle"
            try:
                # Safe only because the exact published artifact checksum was
                # verified above. Pickle input from any other source is refused.
                item = pickle.loads(archive.read(pickle_name))
            except KeyError:
                continue
            histogram = item.get("oct_warped_hist_mean_wrt_tonic")
            if histogram is None or len(histogram) != 60:
                continue
            histogram = np.asarray(histogram, dtype=float)
            if not np.all(np.isfinite(histogram)) or histogram.sum() <= 0:
                continue
            groups[(region, mode)].append(histogram / histogram.sum())

    args.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for (region, mode), histograms in sorted(groups.items()):
        if len(histograms) < 2:
            continue
        stack = np.vstack(histograms)
        mean = stack.mean(axis=0)
        targets = []
        peak_values = select_targets(mean)
        maximum = max(value for value, _ in peak_values)
        for index, (strength, cents) in enumerate(peak_values):
            if cents < 1.0:  # degree zero is implicit in JTune's periodic model
                continue
            bin_index = int(round(cents / 20.0)) % 60
            per_recording_peak = stack[:, bin_index]
            uncertainty = max(10.0, min(60.0, 20.0 + 200.0 * float(per_recording_peak.std())))
            targets.append({
                "id": f"observed-degree-{index}",
                "name": f"Observed center {cents:.0f} cents",
                "cents": cents,
                "ratio": math.pow(2.0, cents / 1200.0),
                "uncertainty_cents": round(uncertainty, 2),
                "confidence": round(min(0.95, (strength / maximum) * min(1.0, len(histograms) / 5.0)), 3),
            })
        targets.append({"id": "octave", "name": "Octave", "cents": 1200.0,
                        "ratio": 2.0, "uncertainty_cents": 10.0, "confidence": 0.95})
        profile = {
            "id": f"org.jtune.ord-cc32.{slug(region)}.{slug(mode)}",
            "version": "zenodo-15682346",
            "display_name": f"Cairo 1932 observed — {mode} / {region} (n={len(histograms)})",
            "tradition": "1932 Cairo Congress historical performance",
            "region": region,
            "scope": f"Tonic-aligned aggregate for recordings tagged {mode} from {region}",
            "model_type": "measured",
            "period_behavior": "octave",
            "period_ratio": 2.0,
            "author_or_community": "ORD-CC32; Baris Bozkurt and Arthur Diniz De Souza",
            "measurement_date": "1932 recordings; features published 2025-06-17",
            "reviewed": False,
            "correction_eligible": False,
            "limitations": "Historical corpus-derived candidate, not Saudi, contemporary, universal, or a complete model of sayr/modulation. Peak centers are computational estimates from tonic-aligned histograms.",
            "appropriate_use": "Historical Cairo-1932 comparison and cautious correction only within the matching region/maqam label.",
            "sources": [{"citation": f"Bozkurt (2025), ORD-CC32, {DOI}",
                         "url": f"https://doi.org/{DOI}", "license": "CC BY-NC 4.0"}],
            "targets": targets,
        }
        output = args.output / f"{slug(region)}-{slug(mode)}.json"
        output.write_text(json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        manifest.append({"file": output.name, "id": profile["id"], "recordings": len(histograms),
                         "correction_eligible": profile["correction_eligible"]})

    (args.output / "manifest.json").write_text(json.dumps({
        "source_doi": DOI, "artifact_md5": EXPECTED_MD5,
        "correction_enabled_by_explicit_user_acceptance": False,
        "profiles": manifest,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(manifest)} profiles to {args.output}")


if __name__ == "__main__":
    main()
