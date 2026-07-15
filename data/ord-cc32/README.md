# ORD-CC32 historical performance profiles

These profiles are reproducible derivatives of Barış Bozkurt's *Open Research
Dataset of the 1932 Cairo Congress of Arab Music* (ORD-CC32), DOI
`10.5281/zenodo.15682346`, licensed CC BY-NC 4.0.

They are reference-only historical Cairo Congress observations grouped by the dataset's region
and maqam annotations. They are not Saudi, contemporary, pan-Arab, or universal
standards. Profiles with only two or three recordings have especially limited
statistical coverage. Each target carries an uncertainty and confidence value.

The exact source artifact is identified in `manifest.json`. Reproduce the files
with:

```bash
python3 tools/derive_ord_cc32_profiles.py \
  CairoCong1932_features_plots_2025-06-17.zip data/ord-cc32
```

The script refuses pickle data unless the archive matches the published MD5.
The generated profiles remain visualization/reference-only. Correction may be
enabled only in a separately versioned derivative that includes documented
technical, domain, and practice-review records; user acceptance alone is not
review.

In the Qt GUI, select all profile JSON files at once with `Import .scl/.json`.
The manifest is documentation and should not be selected.
