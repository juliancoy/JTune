# Worldwide Pitch-System Implementation Plan

## Implementation status (2026-07-13)

The JTune engineering scope in this plan is implemented:

- `TuningWizard` and its unsupported generators have been removed from JTune.
- `PitchSystemDefinition`, `PitchTarget`, `PitchContext`, provenance records,
  keyboard mappings, contextual evaluation, stable IDs, uncertainty, paired
  targets, beat-rate metadata, direction, register scope, and limitations exist
  in the shared JTune pitch-system module.
- Strict Scala `.scl`, same-basename `.kbm`, and provenance-rich measured JSON
  import are implemented, including JSON export and round-trip tests.
- Missing mappings remain unavailable; non-octave, non-periodic, and
  register-specific definitions are not forced into octave replication.
- The correction engine uses the evaluator, preserves rapid intentional pitch
  motion, applies target hysteresis, and has no major/minor pitch mask.
- Qt and ImGui import definitions, persist stable IDs and reference settings,
  show provenance/license/review/limitations, and apply changes off the audio
  callback path.
- CLI, batch WAV/chirp tools, Qt control API, and standalone control server use
  stable pitch-system IDs and explicit reference settings.
- The complete local suite passes, including parser/evaluator, streaming,
  resynthesis parity, frequency-domain quality, and GPU consistency tests.

The following are release gates, not facts software can manufacture:

- No culturally named built-in ships until a redistributable measured or
  precisely named theoretical dataset is supplied.
- No definition is marked reviewed until the required tradition-specific
  practitioner and domain review is documented.
- The repository contains synthetic test fixtures and checksum-pinned,
  reproducible ORD-CC32 historical Cairo-1932 derivatives. The latter remain
  reference-only because no tradition-specific review record is bundled.
- User acceptance never substitutes for technical, domain, or practitioner
  review; measured definitions requesting correction without documented review
  are rejected.

Those gates are deliberately unmet rather than filled with invented averages.

## Purpose

JTune and JSynth need a pitch-system implementation that can represent musical
practice without forcing every tradition into a Western key, mode, octave, or
fixed twelve-note keyboard model.

The filename of this document follows the requested name. In product language,
use **pitch system**, **tuning practice**, or the specific tradition and model.
Do not label music or people as "ethnic scales."

## Honest current state

- JTune can select a static 128-entry frequency table and independently assign a
  frequency to a MIDI reference note.
- The major/minor target filter has been removed from JTune's correction engine.
- Qt and ImGui share the same reduced catalog.
- Generic gamelan, makam, raga, and ancient Greek entries were removed from the
  visible catalogs because their implementations were invented or mislabeled.
- Those obsolete generators still exist in both codebases and must be deleted,
  replaced, or quarantined as explicitly experimental code.
- The current `TuningWizard` design assumes that every system can be flattened
  into one frequency per MIDI note. That assumption is inadequate.
- No measured gamelan dataset, complete makam model, raga grammar, adaptive
  intonation model, or culturally grounded validation suite is implemented.
- The existing catalog has no machine-readable provenance, license, geographic
  scope, theoretical lineage, measurement method, uncertainty, or limitations.

## Non-negotiable principles

1. Never invent missing pitches and present them as traditional practice.
2. Never use a broad cultural name for one local or theoretical model.
3. Every built-in system must identify its source, model, author or community,
   license, and known limitations.
4. Distinguish measured practice from prescriptive theory.
5. Distinguish tuning from mode, melodic grammar, ornament, and performance
   behavior.
6. Do not assume octave equivalence, twelve pitch classes, equal temperament,
   enharmonic equivalence, fixed absolute pitch, or one pitch per note name.
7. Do not call an average precise. Preserve measurement distributions and
   uncertainty when the source provides them.
8. Preserve original terminology and script alongside a careful translation.
9. Prefer sources created with practitioners and culture bearers. Academic
   publication alone does not establish cultural authority.
10. A system is not complete merely because it builds or produces frequencies.

## Phase 1: Audit and remove false claims

- Inventory every generator and catalog entry in JTune and JSynth.
- For each entry, record whether it is:
  - mathematically defined;
  - historically sourced;
  - measured from performance or instruments;
  - a modern reconstruction;
  - an experiment;
  - unsupported.
- Remove unsupported generators rather than merely hiding them.
- Correct misleading implementations, including:
  - quarter-comma meantone represented as 31-EDO;
  - Kirnberger represented by arbitrary cent offsets;
  - identical fabricated Slendro and Pelog tables;
  - generic ratio maps labeled Rast, Bayati, or Yaman;
  - ancient Greek octave species represented as modern church modes.
- Review the harmonic-series, Farey, sparse pentatonic, Bohlen-Pierce, and EDO
  keyboard mappings. Label constructed systems as constructed systems.
- Add tests that reject catalog entries without provenance.

Deliverable: no selectable entry makes a stronger claim than its implementation
and evidence support.

## Phase 2: Replace the static-table architecture

Create a shared pitch-system library used by JTune and JSynth.

### Core entities

`PitchSystemDefinition`

- stable identifier and version;
- display name and names in original scripts;
- tradition, region, locality, and scope;
- model type: mathematical, theoretical, measured, adaptive, or hybrid;
- source citations and dataset license;
- author, researcher, practitioner, ensemble, instrument, and measurement date;
- period behavior: octave, non-octave period, register-specific, or none;
- reference-pitch requirements;
- pitch definitions and uncertainty;
- keyboard or controller mapping;
- limitations and appropriate-use statement.

`PitchTarget`

- stable pitch identifier rather than an assumed Western note name;
- nominal ratio, cents value, frequency, or measured distribution;
- register and instrument applicability;
- ascending, descending, or contextual variants;
- tolerance and confidence;
- optional paired pitch or beating target;
- optional functional role within a named modal system.

`PitchContext`

- reference pitch and tonic;
- current register;
- melodic direction;
- preceding and following pitch targets;
- active phrase or mode state;
- instrument or voice profile;
- user-selected theoretical or measured model.

`PitchSystemEvaluator`

- returns zero, one, or multiple valid targets for a detected pitch and context;
- reports why a target was selected;
- never silently fills undefined pitches;
- supports fixed, measured, contextual, and adaptive implementations.

### Required behavior

- Support arbitrary numbers of pitches per period.
- Support non-octave systems and unequal register mappings.
- Support sparse pitch sets without mapping excluded notes to fake pitches.
- Support multiple candidates for one nominal degree.
- Support asymmetric ascending and descending behavior.
- Support measured frequency tables without octave replication.
- Support paired tunings and target beat rates.
- Keep pitch detection reference separate from correction targets.
- Make the correction range configurable per system instead of globally forcing
  a two-semitone Western limit.

Deliverable: correction no longer depends on a `vector<float>[128]` as its
fundamental representation.

## Phase 3: Open interchange and user-supplied precision

- Implement Scala `.scl` parsing for interval definitions.
- Implement `.kbm` parsing for keyboard mapping and reference pitch.
- Preserve the distinction between a scale definition and its mapping.
- Add an explicit-frequency format for measured, non-periodic systems.
- Add JSON support for provenance, uncertainty, contextual variants, paired
  pitches, and dataset licensing.
- Validate files strictly and show actionable errors.
- Never infer a missing reference note, tonic, period, or mapping without asking
  the user or labeling the default.
- Support export without discarding information.

Deliverable: musicians and researchers can load exact data without modifying C++.

## Phase 4: Tradition-specific implementations

These workstreams must remain separate. One implementation cannot stand in for
all of them.

### Indonesian gamelan

- Begin with one openly licensed, named, measured ensemble dataset.
- Store every measured instrument key by ensemble, instrument, register, and
  pitch designation.
- Preserve stretched or compressed octaves instead of octave-folding.
- Represent paired pengumbang/pengisep pitches and measured or intended ombak.
- Do not average instruments unless the UI explicitly requests a statistical
  summary.
- Distinguish Javanese, Balinese, and Sundanese practices and more specific local
  traditions.
- Distinguish Slendro and Pelog from pathet, repertoire, and instrument layout.
- Add additional ensembles as separate named datasets, never as replacements for
  the first.

Completion requires review by practitioners familiar with the represented
ensemble or tradition.

### Turkish makam

- Implement a named theoretical model first, such as a precisely documented
  Arel-Ezgi-Uzdilek representation, without calling it universal Turkish
  practice.
- Represent commas, accidentals, named tones, tetrachords/pentachords, tonic
  (`durak`), dominant (`güçlü`), melodic direction (`seyir`), and extensions.
- Keep ascending and descending forms distinct where required.
- Add performed-intonation datasets as measured models separate from theory.
- Do not merge Turkish makam with Arabic maqam or Persian dastgah.

### Arabic maqam

- Select a documented regional and pedagogical model before adding pitches.
- Represent ajnas, tonic, ghammaz, modulations, path, and contextual pitch
  behavior rather than treating a maqam as seven static notes.
- Add Rast, Bayati, and other maqamat only with model-specific definitions and
  citations.
- Keep regional practice and measured performance data separately selectable.

### Persian dastgah

- Treat dastgah and radif as their own workstream.
- Represent variable tones, shahed, ist, melodic function, gusheh context, and
  repositioning where supported by the selected source.
- Do not relabel Persian material as Arabic or Turkish.

### Hindustani raga

- Make Sa and its frequency the primary reference rather than A4.
- Represent aroha, avaroha, vadi, samvadi, characteristic phrases, omitted
  degrees, register behavior, and intonational variants.
- Treat shruti theories as named theories, not a single settled 22-tone table.
- Do not quantize gamaka or meend to static targets during transitions.
- Begin with one raga and one documented model plus measured performances.

### Carnatic raga

- Keep Carnatic and Hindustani models separate.
- Represent tonic-relative svaras, melakarta/janya relationships, arohana,
  avarohana, characteristic prayogas, and gamaka context.
- Correction must distinguish stable pitch regions from intentional movement.
- Begin with one raga supported by annotated, licensed recordings and expert
  review.

### Additional systems

- Add African, East Asian, Southeast Asian, Oceanic, Indigenous American, and
  other practices only through their own scoped workstreams.
- Do not create a miscellaneous "world" bucket that erases differences.
- Prioritize based on available community collaboration and licensed evidence,
  not perceived exoticism.

## Phase 5: Pitch tracking and correction behavior

- Separate pitch detection, target inference, and resynthesis into testable
  components.
- Track continuous pitch contours before choosing targets.
- Classify stable tones, transitions, vibrato, slides, and ornaments.
- Avoid correcting intentional continuous motion as if it were an error.
- Allow model-specific target attraction strength and transition rules.
- Support hysteresis so nearby microtonal targets do not chatter.
- Retain uncertainty from both pitch detection and the source tuning model.
- When context is insufficient, pass through or present alternatives rather than
  making an undocumented choice.
- Make latency and temporal smoothing explicit because phrase direction and
  ornament recognition require time context.

Deliverable: correction respects the selected musical model instead of merely
snapping to the nearest frequency.

## Phase 6: Psychoacoustic and audio validation

- Test frequency accuracy in cents and hertz across the full supported range.
- Test glides, vibrato, ornaments, noisy attacks, breath, inharmonic instruments,
  and polyphonic leakage.
- For paired gamelan pitches, test achieved beat frequency and phase behavior.
- Evaluate octave-stretch and non-octave resynthesis without forcing harmonics
  into 12-TET assumptions.
- Conduct controlled listening tests with preregistered questions where useful.
- Do not convert listener preference into a claim of cultural correctness.
- Separate perceptual audibility, correction quality, and cultural validity.
- Document participant population, playback system, protocol, and uncertainty.
- Seek ethics review when research involves human participants or publishable
  behavioral conclusions.

## Phase 7: Qt interface

- Replace the flat combo box with a searchable browser grouped by model type,
  tradition, region, and measured ensemble.
- Show provenance, license, scope, model type, uncertainty, and limitations
  before activation.
- Provide separate controls for tonic/reference pitch and absolute frequency.
- Use culturally appropriate pitch names when supplied by the definition.
- Display the actual active targets rather than assuming MIDI note names.
- Visualize contextual, ascending/descending, paired, and unavailable targets.
- Add `.scl`, `.kbm`, measured-table, and rich JSON import/export.
- Add a dataset inspector with ratios, cents, hertz, register, source, and notes.
- Make experimental or unreviewed data visibly distinct and opt-in.

## Phase 8: ImGui interface

- Use the same shared browser model and identifiers as Qt.
- Provide a compact searchable selector without dropping provenance.
- Show tonic/reference controls appropriate to the selected definition.
- Display current target identity, frequency, model confidence, and selection
  reason in the live tuner.
- Visualize multiple valid targets and contextual transitions.
- Provide import/reload controls suitable for live performance.
- Ensure switching datasets is prepared off the audio thread and applied at a
  buffer boundary.
- Never reduce the ImGui interface to a less accurate tuning model than Qt.

## Phase 9: Persistence, API, and command line

- Persist stable system ID, version, source hash, reference settings, and model
  parameters—not a fragile combo-box index.
- Reject or clearly migrate sessions whose dataset is missing or changed.
- Expose the same model through Qt, ImGui, REST/control server, batch WAV
  processing, and command-line applications.
- Deprecate `--key` and `--scale`; replace them with explicit pitch-system,
  mapping, tonic/reference, and model-context arguments.
- Return active provenance and target-selection reasoning from APIs.

## Phase 10: Testing and review gates

### Automated tests

- Parser conformance tests for `.scl` and `.kbm`.
- Golden frequency tests derived from each cited source.
- Tests for non-octave, sparse, register-specific, paired, and contextual data.
- Tests proving no major/minor or C-based filter is applied implicitly.
- Tests for arbitrary reference notes and frequencies, including 432 Hz where
  explicitly selected.
- Round-trip persistence and import/export tests.
- Real-time allocation, thread-safety, latency, and processor-swap tests.
- Qt and ImGui tests proving both select the same stable definition.

### Human review

- Technical review by tuning and audio-DSP specialists.
- Source and terminology review by ethnomusicologists with relevant regional
  expertise.
- Practice review by musicians, tuners, instrument builders, teachers, or other
  culture bearers from the represented tradition.
- Accessibility and usability review by performing musicians.

No built-in culturally named model ships merely because automated tests pass.

## Provenance requirements for every built-in dataset

- Stable ID and semantic version.
- Full citation and direct source location.
- License permitting redistribution and software use.
- Named theory, ensemble, instrument, performer, or corpus.
- Geographic and historical scope.
- Measurement equipment and method when applicable.
- Reference pitch and environmental conditions when available.
- Raw measurements retained alongside transformed values.
- Transformation code and reproducible build step.
- Uncertainty and missing-data representation.
- Reviewer names or documented review process.
- Clear statement of what the dataset must not be generalized to represent.

## Order of implementation

1. Complete the audit and delete unsupported generators.
2. Build the shared contextual pitch-system library.
3. Add Scala and explicit measured-frequency imports.
4. Migrate 12-TET and other genuinely mathematical systems as reference tests.
5. Update correction, persistence, APIs, Qt, and ImGui to use stable definitions.
6. Add one named measured gamelan ensemble dataset.
7. Add one named Turkish makam theoretical model and one performed dataset.
8. Add one Hindustani or Carnatic raga model with annotated performance data.
9. Obtain domain and practitioner review for each.
10. Expand only by repeating the full provenance and review process.

## Definition of done

Worldwide pitch-system support is complete only when:

- no culturally named entry is invented, generic, or unsupported;
- every entry is reproducible from licensed cited evidence;
- the engine supports fixed, measured, non-octave, paired, and contextual pitch;
- Qt, ImGui, APIs, batch tools, and saved sessions use the same model;
- correction respects melodic movement and uncertainty;
- musicians can inspect exactly why every target was chosen;
- automated audio tests pass, including GPU consistency where enabled;
- relevant technical experts and practitioners have reviewed each shipped model;
- limitations are visible in the product rather than buried in source comments.
