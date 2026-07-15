# JTune pitch-system interchange

JTune accepts Scala `.scl` files, optional same-basename `.kbm` mappings, and a
provenance-bearing JSON format for measured or contextual definitions.

Pitch collections are a separate runtime layer. A pitch system defines target
frequencies; an optional collection filters its degree offsets relative to an
independent tonic. `All defined targets` is the default. Twelve-degree modal
presets are offered only for compatible systems, while custom active degrees
use the selected system's own degree numbering.

Parsing is strict: declared counts must match, intervals must ascend, mappings
may explicitly contain `x` for an unavailable key, and definitions without a
source citation, license, limitations, or appropriate-use statement are rejected.

`correction_eligible` is deliberately opt-in. Measured JSON is rejected when it
requests correction without `reviewed: true` and at least one complete review
record. Historical and theoretical archive imports
remain available for visualization and comparison but do not drive automatic
pitch correction.

JTune also imports pitch-class JSON responses from the Digital Arabic Maqām
Archive. Download a specific, cited historical reconstruction rather than an
unqualified “Arabic scale,” for example:

```bash
curl -o ibnsina-yegah.json \
  'https://diarmaqar.net/api/tuning-systems/ibnsina_1037/yegah/pitch-classes?pitchClassDataType=cents'
```

Import the resulting JSON in the GUI. Its archive version, starting note,
source URL, and CC BY-NC-SA 4.0 license are retained, and correction is disabled.

For measured historical performance rather than theoretical reconstruction,
JTune includes a checksum-pinned ORD-CC32 derivation pipeline and generated
region/maqam profiles under `data/ord-cc32`. These are explicitly labeled as
1932 Cairo Congress observations and must not be described as Saudi or modern.

## Measured/contextual JSON

```json
{
  "id": "org.example.collection.instrument.measurement",
  "version": "1.0.0",
  "display_name": "Named instrument measurement",
  "original_name": "",
  "tradition": "",
  "region": "",
  "locality": "",
  "scope": "One named instrument measured on one date",
  "model_type": "measured",
  "period_behavior": "register_specific",
  "period_ratio": 0,
  "author_or_community": "Named contributor",
  "ensemble": "Named ensemble",
  "instrument": "Named instrument",
  "measurement_date": "YYYY-MM-DD",
  "reviewed": false,
  "correction_eligible": false,
  "source_hash": "sha256:... (filled automatically for imported files)",
  "correction_range_cents": 200,
  "limitations": "State exactly what this data cannot represent.",
  "appropriate_use": "State exactly what this data can represent.",
  "sources": [
    {
      "citation": "Full citation or measurement record",
      "url": "https://example.org/source",
      "license": "An explicit redistribution license"
    }
  ],
  "reviews": [
    {
      "reviewer": "Named reviewer or documented review body",
      "qualification_or_relationship": "Relationship to the represented practice",
      "scope": "Exactly what was reviewed",
      "date": "YYYY-MM-DD",
      "evidence_url": "https://example.org/review-record"
    }
  ],
  "targets": [
    {
      "id": "stable-target-id",
      "name": "Translated or display name",
      "original_name": "Name in the source language/script",
      "frequency_hz": 440.0,
      "ratio": 0,
      "cents": 0,
      "uncertainty_cents": 0.5,
      "confidence": 0.95,
      "register_min": 69,
      "register_max": 69,
      "direction": "unknown",
      "function": "",
      "instrument": "Instrument/register identifier",
      "paired_target_id": "",
      "beat_rate_hz": 0
    }
  ]
}
```

Do not add a review record merely to unlock correction. For a measured or
culturally scoped definition, it must document the relevant technical, domain,
and practice review. User consent to load a file is not such a review.

`model_type` is `mathematical`, `theoretical`, `measured`, `adaptive`, or
`hybrid`. `period_behavior` is `octave`, `non_octave`, `register_specific`, or
`none`. `direction` is `ascending`, `descending`, `stable`, or `unknown`.

An explicit frequency is not octave-replicated when the definition is
register-specific or non-periodic. Missing keys remain missing; JTune does not
invent a frequency.

## Stable persistence

Applications persist the definition's stable `id`, `version`, imported source
path, reference MIDI note, reference frequency, and octave shift. Combo-box
indices are never the identity of a pitch system.

## Cultural data gate

A culturally named built-in requires a redistributable source, reproducible raw
measurements or a precisely identified theory, limitations, appropriate-use
scope, and documented review by people competent in that particular practice.
The file format makes those fields representable; it does not turn an unreviewed
file into an authoritative model.
