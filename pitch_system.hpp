#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jtune {

enum class PitchModelType { Mathematical, Theoretical, Measured, Adaptive, Hybrid };
enum class PeriodBehavior { Octave, NonOctave, RegisterSpecific, None };
enum class MelodicDirection { Unknown, Ascending, Descending, Stable };

struct SourceReference {
    std::string citation;
    std::string url;
    std::string license;
};

struct ReviewRecord {
    std::string reviewer;
    std::string qualificationOrRelationship;
    std::string scope;
    std::string date;
    std::string evidenceUrl;
};

struct PitchTarget {
    std::string id;
    std::string name;
    std::string originalName;
    double ratio = 0.0;
    double cents = 0.0;
    double absoluteHz = 0.0;
    double uncertaintyCents = 0.0;
    double confidence = 1.0;
    int registerMin = -1;
    int registerMax = -1;
    MelodicDirection direction = MelodicDirection::Unknown;
    std::string function;
    std::string instrument;
    std::string pairedTargetId;
    double beatRateHz = 0.0;
};

struct KeyboardMapping {
    int mapSize = 0;
    int firstMidi = 0;
    int lastMidi = 127;
    int middleMidi = 60;
    int referenceMidi = 69;
    double referenceHz = 440.0;
    int formalOctaveDegree = 0;
    std::vector<int> degrees; // -1 means unmapped
};

struct PitchSystemDefinition {
    std::string id;
    std::string version;
    std::string displayName;
    std::string originalName;
    std::string tradition;
    std::string region;
    std::string locality;
    std::string scope;
    PitchModelType modelType = PitchModelType::Mathematical;
    PeriodBehavior periodBehavior = PeriodBehavior::Octave;
    double periodRatio = 2.0;
    std::vector<PitchTarget> targets; // degree zero is implicit 1/1 for periodic definitions
    std::optional<KeyboardMapping> mapping;
    std::vector<SourceReference> sources;
    std::vector<ReviewRecord> reviews;
    std::string sourceHash;
    std::string authorOrCommunity;
    std::string ensemble;
    std::string instrument;
    std::string measurementDate;
    std::string limitations;
    std::string appropriateUse;
    bool reviewed = false;
    // Historical reconstructions are valuable references but are not evidence
    // of a living performance standard. Correction must be explicitly enabled.
    bool correctionEligible = false;
    // Maximum automatic correction distance. Systems may narrow or widen this
    // based on documented practice; zero selects the conservative default.
    double correctionRangeCents = 0.0;

    std::vector<std::string> validate() const;
};

struct PitchContext {
    int referenceMidi = 69;
    double referenceHz = 440.0;
    int currentRegister = -1;
    MelodicDirection direction = MelodicDirection::Unknown;
    std::string previousTargetId;
    std::string nextTargetId;
    std::string phraseOrMode;
    std::string instrumentOrVoice;
    int octaveShift = 0;
    int tonicMidi = 60;
    // Empty means every defined degree. Non-empty values are degree offsets
    // from tonic in the active system, not assumed semitone pitch classes.
    const std::vector<int>* enabledDegrees = nullptr;
};

struct PitchCollectionDefinition {
    std::string id;
    std::string displayName;
    int degreeCount = 0;
    std::vector<int> enabledDegrees;
    std::string scope;
};

const std::vector<PitchCollectionDefinition>& builtInPitchCollections();
const PitchCollectionDefinition* pitchCollectionById(const std::string& id);
std::optional<std::vector<int>> parsePitchDegreeList(const std::string& text);

struct EvaluatedTarget {
    PitchTarget target;
    double frequencyHz = 0.0;
    double distanceCents = 0.0;
    std::string reason;
};

class PitchSystemEvaluator {
public:
    explicit PitchSystemEvaluator(const PitchSystemDefinition& definition);
    std::vector<EvaluatedTarget> candidates(double detectedHz, const PitchContext& context) const;
    std::optional<EvaluatedTarget> nearest(double detectedHz, const PitchContext& context) const;
    std::optional<double> frequencyForMidi(int midiNote, const PitchContext& context) const;

private:
    const PitchSystemDefinition& definition_;
};

struct ParseResult {
    std::optional<PitchSystemDefinition> definition;
    std::vector<std::string> errors;
};

ParseResult parseScalaScl(const std::string& text, const std::string& sourceName);
std::vector<std::string> applyScalaKbm(PitchSystemDefinition& definition,
                                       const std::string& text,
                                       const std::string& sourceName);
ParseResult parseMeasuredJson(const std::string& text, const std::string& sourceName);
ParseResult parseDiarmaqarPitchClasses(const std::string& text, const std::string& sourceName);
std::string exportPitchSystemJson(const PitchSystemDefinition& definition);

class PitchSystemRegistry {
public:
    PitchSystemRegistry();
    const std::vector<PitchSystemDefinition>& definitions() const { return definitions_; }
    const PitchSystemDefinition* byIndex(int index) const;
    const PitchSystemDefinition* byId(const std::string& id) const;
    bool add(PitchSystemDefinition definition, std::vector<std::string>& errors);
    bool loadFile(const std::string& path, std::vector<std::string>& errors);

private:
    std::vector<PitchSystemDefinition> definitions_;
};

PitchSystemRegistry& pitchSystemRegistry();

} // namespace jtune
