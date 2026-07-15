#include "pitch_system.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <sstream>

namespace jtune {
namespace {

constexpr double kCentsPerOctave = 1200.0;

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> dataLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (!line.empty() && line.front() != '!') lines.push_back(line);
    }
    return lines;
}

std::optional<double> positiveDouble(const std::string& value)
{
    try {
        size_t used = 0;
        const double result = std::stod(value, &used);
        if (used != value.size() || !(result > 0.0) || !std::isfinite(result)) return std::nullopt;
        return result;
    } catch (...) { return std::nullopt; }
}

std::optional<int> integer(const std::string& value)
{
    try {
        size_t used = 0;
        const int result = std::stoi(value, &used);
        if (used != value.size()) return std::nullopt;
        return result;
    } catch (...) { return std::nullopt; }
}

double targetRatio(const PitchTarget& target)
{
    if (target.ratio > 0.0) return target.ratio;
    return std::pow(2.0, target.cents / kCentsPerOctave);
}

PitchSystemDefinition edo(int divisions)
{
    PitchSystemDefinition d;
    d.id = "org.jtune.edo." + std::to_string(divisions);
    d.version = "1.0.0";
    d.displayName = std::to_string(divisions) + "-EDO";
    d.scope = "Equal division of the octave";
    d.modelType = PitchModelType::Mathematical;
    d.periodBehavior = PeriodBehavior::Octave;
    d.periodRatio = 2.0;
    d.authorOrCommunity = "JTune mathematical generator";
    d.limitations = "A mathematical temperament, not a claim about any culture or performance practice.";
    d.appropriateUse = "Exact equal division of a 2/1 octave.";
    d.correctionEligible = true;
    d.sourceHash = "builtin:edo:" + std::to_string(divisions) + ":1.0.0";
    d.correctionRangeCents = 200.0;
    d.sources.push_back({"Definition: octave divided into equal logarithmic steps.", "", "CC0-1.0"});
    for (int i = 1; i <= divisions; ++i) {
        PitchTarget t;
        t.id = "degree-" + std::to_string(i);
        t.name = std::to_string(i) + "/" + std::to_string(divisions) + " octave";
        t.cents = kCentsPerOctave * static_cast<double>(i) / static_cast<double>(divisions);
        t.ratio = std::pow(2.0, static_cast<double>(i) / static_cast<double>(divisions));
        d.targets.push_back(t);
    }
    return d;
}

QString modelName(PitchModelType type)
{
    switch (type) {
    case PitchModelType::Mathematical: return "mathematical";
    case PitchModelType::Theoretical: return "theoretical";
    case PitchModelType::Measured: return "measured";
    case PitchModelType::Adaptive: return "adaptive";
    case PitchModelType::Hybrid: return "hybrid";
    }
    return "mathematical";
}

PitchModelType parseModel(const QString& value)
{
    if (value == "theoretical") return PitchModelType::Theoretical;
    if (value == "measured") return PitchModelType::Measured;
    if (value == "adaptive") return PitchModelType::Adaptive;
    if (value == "hybrid") return PitchModelType::Hybrid;
    return PitchModelType::Mathematical;
}

MelodicDirection parseDirection(const QString& value)
{
    if (value == "ascending") return MelodicDirection::Ascending;
    if (value == "descending") return MelodicDirection::Descending;
    if (value == "stable") return MelodicDirection::Stable;
    return MelodicDirection::Unknown;
}

QString directionName(MelodicDirection value)
{
    switch (value) {
    case MelodicDirection::Ascending: return "ascending";
    case MelodicDirection::Descending: return "descending";
    case MelodicDirection::Stable: return "stable";
    case MelodicDirection::Unknown: return "unknown";
    }
    return "unknown";
}

int positiveModulo(int value, int modulus)
{
    const int result = value % modulus;
    return result < 0 ? result + modulus : result;
}

} // namespace

const std::vector<PitchCollectionDefinition>& builtInPitchCollections()
{
    static const std::vector<PitchCollectionDefinition> collections = {
        {"all", "All defined targets", 0, {}, "No pitch-collection filter"},
        {"12edo.ionian", "Ionian (Major)", 12, {0, 2, 4, 5, 7, 9, 11}, "12-degree diatonic mode"},
        {"12edo.dorian", "Dorian", 12, {0, 2, 3, 5, 7, 9, 10}, "12-degree diatonic mode"},
        {"12edo.phrygian", "Phrygian", 12, {0, 1, 3, 5, 7, 8, 10}, "12-degree diatonic mode"},
        {"12edo.lydian", "Lydian", 12, {0, 2, 4, 6, 7, 9, 11}, "12-degree diatonic mode"},
        {"12edo.mixolydian", "Mixolydian", 12, {0, 2, 4, 5, 7, 9, 10}, "12-degree diatonic mode"},
        {"12edo.aeolian", "Aeolian (Natural Minor)", 12, {0, 2, 3, 5, 7, 8, 10}, "12-degree diatonic mode"},
        {"12edo.locrian", "Locrian", 12, {0, 1, 3, 5, 6, 8, 10}, "12-degree diatonic mode"},
        {"12edo.major-pentatonic", "Major pentatonic", 12, {0, 2, 4, 7, 9}, "12-degree pentatonic collection"},
        {"12edo.minor-pentatonic", "Minor pentatonic", 12, {0, 3, 5, 7, 10}, "12-degree pentatonic collection"},
        {"custom", "Custom active degrees", 0, {}, "User-selected degrees from the active pitch system"},
    };
    return collections;
}

const PitchCollectionDefinition* pitchCollectionById(const std::string& id)
{
    const auto& values = builtInPitchCollections();
    const auto it = std::find_if(values.begin(), values.end(), [&](const auto& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

std::optional<std::vector<int>> parsePitchDegreeList(const std::string& text)
{
    std::vector<int> values;
    std::istringstream input(text);
    std::string token;
    while (std::getline(input, token, ',')) {
        token = trim(token);
        const auto value = integer(token);
        if (!value || *value < 0 || std::find(values.begin(), values.end(), *value) != values.end())
            return std::nullopt;
        values.push_back(*value);
    }
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    return values;
}

std::vector<std::string> PitchSystemDefinition::validate() const
{
    std::vector<std::string> errors;
    if (id.empty()) errors.push_back("stable id is required");
    if (version.empty()) errors.push_back("version is required");
    if (displayName.empty()) errors.push_back("display name is required");
    if (sources.empty()) errors.push_back("at least one source is required");
    for (const auto& source : sources) {
        if (source.citation.empty()) errors.push_back("source citation is required");
        if (source.license.empty()) errors.push_back("source license is required");
    }
    if (limitations.empty()) errors.push_back("limitations are required");
    if (appropriateUse.empty()) errors.push_back("appropriate-use statement is required");
    if (targets.empty()) errors.push_back("at least one pitch target is required");
    if (reviewed && reviews.empty())
        errors.push_back("reviewed definitions require at least one documented review record");
    if (correctionEligible && modelType == PitchModelType::Measured && !reviewed)
        errors.push_back("measured definitions require documented review before correction can be enabled");
    if (correctionRangeCents < 0.0 || !std::isfinite(correctionRangeCents))
        errors.push_back("correction range must be finite and non-negative");
    if ((periodBehavior == PeriodBehavior::Octave || periodBehavior == PeriodBehavior::NonOctave) &&
        (!(periodRatio > 1.0) || !std::isfinite(periodRatio)))
        errors.push_back("period ratio must be finite and greater than one");
    if (mapping) {
        if (mapping->mapSize < 0 || mapping->firstMidi < 0 || mapping->lastMidi > 127 ||
            mapping->firstMidi > mapping->lastMidi || mapping->referenceMidi < 0 ||
            mapping->referenceMidi > 127 || !(mapping->referenceHz > 0.0))
            errors.push_back("keyboard mapping header is invalid");
        if (mapping->mapSize != static_cast<int>(mapping->degrees.size()))
            errors.push_back("keyboard mapping size does not match degree count");
    }
    std::vector<std::string> targetIds;
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        if (target.id.empty()) errors.push_back("target " + std::to_string(i) + " has no id");
        if (std::find(targetIds.begin(), targetIds.end(), target.id) != targetIds.end())
            errors.push_back("duplicate target id: " + target.id);
        targetIds.push_back(target.id);
        const int representations = (target.ratio > 0.0 ? 1 : 0) +
            (target.absoluteHz > 0.0 ? 1 : 0) + (target.cents != 0.0 ? 1 : 0);
        if (representations == 0) errors.push_back("target " + target.id + " has no pitch value");
        if (target.confidence < 0.0 || target.confidence > 1.0)
            errors.push_back("target " + target.id + " confidence is outside 0..1");
        if (target.uncertaintyCents < 0.0 || !std::isfinite(target.uncertaintyCents))
            errors.push_back("target " + target.id + " uncertainty is invalid");
        if ((target.registerMin >= 0 || target.registerMax >= 0) &&
            (target.registerMin < 0 || target.registerMax < target.registerMin || target.registerMax > 127))
            errors.push_back("target " + target.id + " register range is invalid");
    }
    for (const auto& target : targets) {
        if (!target.pairedTargetId.empty() &&
            std::find(targetIds.begin(), targetIds.end(), target.pairedTargetId) == targetIds.end())
            errors.push_back("target " + target.id + " references missing paired target " + target.pairedTargetId);
    }
    for (const auto& review : reviews) {
        if (review.reviewer.empty() || review.scope.empty() || review.date.empty())
            errors.push_back("review records require reviewer, scope, and date");
    }
    return errors;
}

PitchSystemEvaluator::PitchSystemEvaluator(const PitchSystemDefinition& definition)
    : definition_(definition) {}

std::optional<double> PitchSystemEvaluator::frequencyForMidi(int midiNote, const PitchContext& context) const
{
    if (midiNote < 0 || midiNote > 127) return std::nullopt;
    if (definition_.periodBehavior == PeriodBehavior::None ||
        definition_.periodBehavior == PeriodBehavior::RegisterSpecific) {
        for (const auto& target : definition_.targets) {
            if (target.absoluteHz > 0.0 && target.registerMin == midiNote && target.registerMax == midiNote)
                return target.absoluteHz * std::pow(2.0, context.octaveShift);
        }
        return std::nullopt;
    }

    const int degreeCount = static_cast<int>(definition_.targets.size());
    if (degreeCount <= 0) return std::nullopt;
    const KeyboardMapping mapping = definition_.mapping.value_or(KeyboardMapping{});
    const int referenceMidi = context.referenceMidi;
    auto floorDiv = [](int value, int divisor) {
        int quotient = value / divisor;
        if (value % divisor < 0) --quotient;
        return quotient;
    };
    auto scaleRatioForDegree = [&](int scaleDegree) -> double {
        const int periods = floorDiv(scaleDegree, degreeCount);
        int local = scaleDegree - periods * degreeCount;
        double ratio = local == 0 ? 1.0 : targetRatio(definition_.targets[static_cast<size_t>(local - 1)]);
        return ratio * std::pow(definition_.periodRatio, periods);
    };
    if (definition_.mapping && mapping.mapSize > 0) {
        if (midiNote < mapping.firstMidi || midiNote > mapping.lastMidi) return std::nullopt;
        auto mappedDegree = [&](int note) -> std::optional<int> {
            const int delta = note - mapping.middleMidi;
            const int cycle = floorDiv(delta, mapping.mapSize);
            const int slot = delta - cycle * mapping.mapSize;
            if (slot < 0 || slot >= static_cast<int>(mapping.degrees.size())) return std::nullopt;
            const int degree = mapping.degrees[static_cast<size_t>(slot)];
            if (degree < 0) return std::nullopt;
            const int formalPeriod = mapping.formalOctaveDegree > 0 ? mapping.formalOctaveDegree : degreeCount;
            return degree + cycle * formalPeriod;
        };
        const auto noteDegree = mappedDegree(midiNote);
        const auto referenceDegree = mappedDegree(mapping.referenceMidi);
        if (!noteDegree || !referenceDegree) return std::nullopt;
        const double mappedHz = mapping.referenceHz *
            scaleRatioForDegree(*noteDegree) / scaleRatioForDegree(*referenceDegree);
        const double userReferenceScale = context.referenceHz / mapping.referenceHz;
        return mappedHz * userReferenceScale * std::pow(2.0, context.octaveShift);
    }
    auto ratioAt = [&](int note) -> std::optional<double> {
        const int delta = note - referenceMidi;
        return scaleRatioForDegree(delta);
    };
    const auto ratio = ratioAt(midiNote);
    if (!ratio) return std::nullopt;
    return context.referenceHz * (*ratio) * std::pow(2.0, context.octaveShift);
}

std::vector<EvaluatedTarget> PitchSystemEvaluator::candidates(double detectedHz,
                                                               const PitchContext& context) const
{
    std::vector<EvaluatedTarget> result;
    if (!(detectedHz > 0.0) || !std::isfinite(detectedHz)) return result;
    auto eligible = [&](const PitchTarget& target, int midi) {
        if (target.confidence <= 0.0) return false;
        if (target.registerMin >= 0 && (midi < target.registerMin || midi > target.registerMax)) return false;
        if (!target.instrument.empty() && !context.instrumentOrVoice.empty() &&
            target.instrument != context.instrumentOrVoice) return false;
        if (target.direction != MelodicDirection::Unknown &&
            context.direction != MelodicDirection::Unknown &&
            target.direction != context.direction) return false;
        return true;
    };
    if (definition_.periodBehavior == PeriodBehavior::None ||
        definition_.periodBehavior == PeriodBehavior::RegisterSpecific) {
        for (const auto& target : definition_.targets) {
            if (!(target.absoluteHz > 0.0)) continue;
            const int midi = target.registerMin;
            if (!eligible(target, midi)) continue;
            const double frequency = target.absoluteHz * std::pow(2.0, context.octaveShift);
            const double distance = 1200.0 * std::log2(frequency / detectedHz);
            const double range = (definition_.correctionRangeCents > 0.0
                ? definition_.correctionRangeCents : 3600.0) + 1200.0 * std::abs(context.octaveShift);
            if (std::abs(distance) > range) continue;
            result.push_back({target, frequency, distance,
                "eligible measured target " + target.id + " in " + definition_.id});
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return std::abs(a.distanceCents) < std::abs(b.distanceCents);
        });
        return result;
    }
    for (int midi = 0; midi < 128; ++midi) {
        if (context.enabledDegrees && !context.enabledDegrees->empty()) {
            const int offset = positiveModulo(midi - context.tonicMidi,
                                              std::max(1, static_cast<int>(definition_.targets.size())));
            if (std::find(context.enabledDegrees->begin(), context.enabledDegrees->end(), offset) ==
                context.enabledDegrees->end()) continue;
        }
        const auto frequency = frequencyForMidi(midi, context);
        if (!frequency || !(*frequency > 0.0)) continue;
        const double distance = 1200.0 * std::log2(*frequency / detectedHz);
        const double range = (definition_.correctionRangeCents > 0.0
            ? definition_.correctionRangeCents : 3600.0) + 1200.0 * std::abs(context.octaveShift);
        if (std::abs(distance) > range) continue;
        EvaluatedTarget evaluated;
        const int degreeCount = std::max(1, static_cast<int>(definition_.targets.size()));
        int degree = (midi - context.referenceMidi) % degreeCount;
        if (degree < 0) degree += degreeCount;
        if (degree == 0) {
            evaluated.target.id = "unison";
            evaluated.target.name = "1/1";
            evaluated.target.ratio = 1.0;
        } else {
            evaluated.target = definition_.targets[static_cast<size_t>(degree - 1)];
        }
        if (!eligible(evaluated.target, midi)) continue;
        evaluated.frequencyHz = *frequency;
        evaluated.distanceCents = distance;
        evaluated.reason = "nearest eligible target in " + definition_.id;
        result.push_back(std::move(evaluated));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::abs(a.distanceCents) < std::abs(b.distanceCents);
    });
    return result;
}

std::optional<EvaluatedTarget> PitchSystemEvaluator::nearest(double detectedHz,
                                                              const PitchContext& context) const
{
    auto values = candidates(detectedHz, context);
    if (values.empty()) return std::nullopt;
    return values.front();
}

ParseResult parseScalaScl(const std::string& text, const std::string& sourceName)
{
    ParseResult result;
    const auto lines = dataLines(text);
    if (lines.size() < 2) {
        result.errors.push_back(sourceName + ": SCL requires a description and note count");
        return result;
    }
    const auto count = integer(lines[1]);
    if (!count || *count <= 0 || *count > 4096) {
        result.errors.push_back(sourceName + ": invalid SCL note count");
        return result;
    }
    if (lines.size() != static_cast<size_t>(*count + 2)) {
        result.errors.push_back(sourceName + ": SCL note count does not match interval lines");
        return result;
    }

    PitchSystemDefinition d;
    d.id = "user.scala." + sourceName;
    std::replace(d.id.begin(), d.id.end(), ' ', '-');
    d.version = "1.0.0";
    d.displayName = lines[0];
    d.scope = "Imported Scala scale";
    d.modelType = PitchModelType::Mathematical;
    d.correctionEligible = true;
    d.correctionRangeCents = 200.0;
    d.periodBehavior = PeriodBehavior::NonOctave;
    d.limitations = "Meaning depends on the accompanying keyboard mapping and source documentation.";
    d.appropriateUse = "Use with an explicit reference pitch and, when applicable, a .kbm mapping.";
    d.sources.push_back({"Imported from " + sourceName, sourceName, "User-supplied; redistribution not granted"});

    double previous = 1.0;
    for (int i = 0; i < *count; ++i) {
        const std::string token = lines[static_cast<size_t>(i + 2)];
        double ratio = 0.0;
        double cents = 0.0;
        const auto slash = token.find('/');
        if (slash != std::string::npos) {
            const auto numerator = positiveDouble(token.substr(0, slash));
            const auto denominator = positiveDouble(token.substr(slash + 1));
            if (!numerator || !denominator) {
                result.errors.push_back(sourceName + ": invalid ratio at degree " + std::to_string(i + 1));
                continue;
            }
            ratio = *numerator / *denominator;
            cents = 1200.0 * std::log2(ratio);
        } else if (token.find('.') != std::string::npos) {
            const auto parsed = positiveDouble(token);
            if (!parsed) {
                result.errors.push_back(sourceName + ": invalid cents at degree " + std::to_string(i + 1));
                continue;
            }
            cents = *parsed;
            ratio = std::pow(2.0, cents / 1200.0);
        } else {
            const auto parsed = positiveDouble(token);
            if (!parsed) {
                result.errors.push_back(sourceName + ": invalid integer ratio at degree " + std::to_string(i + 1));
                continue;
            }
            ratio = *parsed;
            cents = 1200.0 * std::log2(ratio);
        }
        if (ratio <= previous) result.errors.push_back(sourceName + ": intervals must be strictly ascending");
        previous = ratio;
        PitchTarget target;
        target.id = "degree-" + std::to_string(i + 1);
        target.name = token;
        target.ratio = ratio;
        target.cents = cents;
        d.targets.push_back(target);
    }
    if (!result.errors.empty()) return result;
    d.periodRatio = d.targets.back().ratio;
    d.periodBehavior = std::abs(d.periodRatio - 2.0) < 1e-9 ? PeriodBehavior::Octave : PeriodBehavior::NonOctave;
    result.definition = std::move(d);
    return result;
}

std::vector<std::string> applyScalaKbm(PitchSystemDefinition& definition,
                                       const std::string& text,
                                       const std::string& sourceName)
{
    std::vector<std::string> errors;
    const auto lines = dataLines(text);
    if (lines.size() < 7) return {sourceName + ": KBM requires seven header fields"};
    KeyboardMapping m;
    const auto mapSize = integer(lines[0]);
    const auto first = integer(lines[1]);
    const auto last = integer(lines[2]);
    const auto middle = integer(lines[3]);
    const auto reference = integer(lines[4]);
    const auto referenceHz = positiveDouble(lines[5]);
    const auto octaveDegree = integer(lines[6]);
    if (!mapSize || *mapSize < 0 || !first || !last || !middle || !reference || !referenceHz || !octaveDegree)
        return {sourceName + ": invalid KBM header"};
    if (*first < 0 || *last > 127 || *first > *last || *reference < 0 || *reference > 127)
        errors.push_back(sourceName + ": KBM MIDI range is invalid");
    if (lines.size() != static_cast<size_t>(7 + *mapSize))
        errors.push_back(sourceName + ": KBM map size does not match mapping lines");
    m.mapSize = *mapSize; m.firstMidi = *first; m.lastMidi = *last; m.middleMidi = *middle;
    m.referenceMidi = *reference; m.referenceHz = *referenceHz; m.formalOctaveDegree = *octaveDegree;
    for (int i = 0; i < *mapSize && static_cast<size_t>(7 + i) < lines.size(); ++i) {
        if (lines[static_cast<size_t>(7 + i)] == "x" || lines[static_cast<size_t>(7 + i)] == "X") m.degrees.push_back(-1);
        else {
            const auto degree = integer(lines[static_cast<size_t>(7 + i)]);
            if (!degree) errors.push_back(sourceName + ": invalid KBM degree at line " + std::to_string(8 + i));
            else m.degrees.push_back(*degree);
        }
    }
    if (errors.empty()) definition.mapping = std::move(m);
    return errors;
}

ParseResult parseMeasuredJson(const std::string& text, const std::string& sourceName)
{
    ParseResult result;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.errors.push_back(sourceName + ": invalid JSON: " + error.errorString().toStdString());
        return result;
    }
    const auto root = document.object();
    PitchSystemDefinition d;
    d.id = root.value("id").toString().toStdString();
    d.version = root.value("version").toString().toStdString();
    d.displayName = root.value("display_name").toString().toStdString();
    d.originalName = root.value("original_name").toString().toStdString();
    d.tradition = root.value("tradition").toString().toStdString();
    d.region = root.value("region").toString().toStdString();
    d.locality = root.value("locality").toString().toStdString();
    d.scope = root.value("scope").toString().toStdString();
    d.modelType = parseModel(root.value("model_type").toString());
    d.periodRatio = root.value("period_ratio").toDouble(0.0);
    const QString periodBehavior = root.value("period_behavior").toString();
    d.periodBehavior = d.periodRatio > 1.0 ? PeriodBehavior::NonOctave : PeriodBehavior::None;
    if (std::abs(d.periodRatio - 2.0) < 1e-9) d.periodBehavior = PeriodBehavior::Octave;
    if (periodBehavior == "register_specific") d.periodBehavior = PeriodBehavior::RegisterSpecific;
    else if (periodBehavior == "none") d.periodBehavior = PeriodBehavior::None;
    d.authorOrCommunity = root.value("author_or_community").toString().toStdString();
    d.ensemble = root.value("ensemble").toString().toStdString();
    d.instrument = root.value("instrument").toString().toStdString();
    d.measurementDate = root.value("measurement_date").toString().toStdString();
    d.limitations = root.value("limitations").toString().toStdString();
    d.appropriateUse = root.value("appropriate_use").toString().toStdString();
    d.reviewed = root.value("reviewed").toBool(false);
    d.correctionEligible = root.value("correction_eligible").toBool(false);
    d.sourceHash = root.value("source_hash").toString().toStdString();
    d.correctionRangeCents = root.value("correction_range_cents").toDouble(0.0);
    for (const auto& item : root.value("sources").toArray()) {
        const auto object = item.toObject();
        d.sources.push_back({object.value("citation").toString().toStdString(),
                             object.value("url").toString().toStdString(),
                             object.value("license").toString().toStdString()});
    }
    for (const auto& item : root.value("reviews").toArray()) {
        const auto object = item.toObject();
        d.reviews.push_back({object.value("reviewer").toString().toStdString(),
                             object.value("qualification_or_relationship").toString().toStdString(),
                             object.value("scope").toString().toStdString(),
                             object.value("date").toString().toStdString(),
                             object.value("evidence_url").toString().toStdString()});
    }
    if (root.value("mapping").isObject()) {
        const auto object = root.value("mapping").toObject();
        KeyboardMapping mapping;
        mapping.mapSize = object.value("map_size").toInt(0);
        mapping.firstMidi = object.value("first_midi").toInt(0);
        mapping.lastMidi = object.value("last_midi").toInt(127);
        mapping.middleMidi = object.value("middle_midi").toInt(60);
        mapping.referenceMidi = object.value("reference_midi").toInt(69);
        mapping.referenceHz = object.value("reference_hz").toDouble(440.0);
        mapping.formalOctaveDegree = object.value("formal_octave_degree").toInt(0);
        for (const auto& degree : object.value("degrees").toArray())
            mapping.degrees.push_back(degree.toInt(-1));
        d.mapping = std::move(mapping);
    }
    for (const auto& item : root.value("targets").toArray()) {
        const auto object = item.toObject();
        PitchTarget target;
        target.id = object.value("id").toString().toStdString();
        target.name = object.value("name").toString().toStdString();
        target.originalName = object.value("original_name").toString().toStdString();
        target.ratio = object.value("ratio").toDouble(0.0);
        target.cents = object.value("cents").toDouble(0.0);
        target.absoluteHz = object.value("frequency_hz").toDouble(0.0);
        target.uncertaintyCents = object.value("uncertainty_cents").toDouble(0.0);
        target.confidence = object.value("confidence").toDouble(1.0);
        target.registerMin = object.contains("register_min") ? object.value("register_min").toInt(-1) : object.value("midi_note").toInt(-1);
        target.registerMax = object.contains("register_max") ? object.value("register_max").toInt(-1) : target.registerMin;
        target.direction = parseDirection(object.value("direction").toString());
        target.function = object.value("function").toString().toStdString();
        target.instrument = object.value("instrument").toString().toStdString();
        target.pairedTargetId = object.value("paired_target_id").toString().toStdString();
        target.beatRateHz = object.value("beat_rate_hz").toDouble(0.0);
        d.targets.push_back(std::move(target));
    }
    result.errors = d.validate();
    if (result.errors.empty()) result.definition = std::move(d);
    return result;
}

ParseResult parseDiarmaqarPitchClasses(const std::string& text, const std::string& sourceName)
{
    ParseResult result;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.errors.push_back(sourceName + ": invalid DiArMaqAr JSON: " + error.errorString().toStdString());
        return result;
    }
    const auto root = document.object();
    const auto system = root.value("tuningSystem").toObject();
    const auto selected = root.value("selectedStartingNote").toObject();
    const auto pitchClasses = root.value("pitchClasses").toArray();
    if (system.isEmpty() || selected.isEmpty() || pitchClasses.isEmpty()) {
        result.errors.push_back(sourceName + ": not a DiArMaqAr pitch-classes response");
        return result;
    }

    PitchSystemDefinition d;
    const std::string archiveId = system.value("id").toString().toStdString();
    const std::string startingNote = selected.value("idName").toString().toStdString();
    d.id = "org.diarmaqar." + archiveId + "." + startingNote;
    d.version = system.value("version").toString().toStdString();
    d.displayName = "Historical reference — " + system.value("displayName").toString().toStdString();
    d.tradition = "Arabic music theory history";
    d.scope = "Historical tuning reconstruction starting on " +
        selected.value("displayName").toString().toStdString();
    d.modelType = PitchModelType::Theoretical;
    d.periodBehavior = PeriodBehavior::Octave;
    d.periodRatio = 2.0;
    d.authorOrCommunity = "Digital Arabic Maqām Archive, AUB Music Intelligence Lab";
    d.limitations =
        "Historical/theoretical reconstruction. It does not establish modern, regional, Saudi, "
        "vocal, or performer-specific intonation and is disabled for corrective tuning.";
    d.appropriateUse =
        "Historical study, visualization, interval comparison, and research reproduction only.";
    d.reviewed = false;
    d.correctionEligible = false;
    d.sources.push_back({
        "Digital Arabic Maqām Archive pitch-class API: " + archiveId,
        "https://diarmaqar.net/api/tuning-systems/" + archiveId + "/" + startingNote + "/pitch-classes",
        "CC BY-NC-SA 4.0"});

    for (const auto& value : pitchClasses) {
        const auto object = value.toObject();
        const double cents = object.value("cents").toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(cents) || cents <= 0.0 || cents > 1200.0 + 1e-6) continue;
        PitchTarget target;
        target.id = object.value("noteName").toString().toStdString();
        target.name = object.value("noteNameDisplay").toString().toStdString();
        target.cents = cents;
        target.ratio = std::pow(2.0, cents / kCentsPerOctave);
        target.confidence = 1.0;
        d.targets.push_back(std::move(target));
    }
    std::sort(d.targets.begin(), d.targets.end(), [](const auto& a, const auto& b) {
        return a.cents < b.cents;
    });
    if (d.targets.empty() || std::abs(d.targets.back().cents - 1200.0) > 1e-3) {
        PitchTarget octave;
        octave.id = "octave";
        octave.name = "octave";
        octave.cents = 1200.0;
        octave.ratio = 2.0;
        d.targets.push_back(std::move(octave));
    }
    result.errors = d.validate();
    if (result.errors.empty()) result.definition = std::move(d);
    return result;
}

std::string exportPitchSystemJson(const PitchSystemDefinition& d)
{
    QJsonObject root{{"id", QString::fromStdString(d.id)}, {"version", QString::fromStdString(d.version)},
        {"display_name", QString::fromStdString(d.displayName)}, {"original_name", QString::fromStdString(d.originalName)},
        {"tradition", QString::fromStdString(d.tradition)}, {"region", QString::fromStdString(d.region)},
        {"locality", QString::fromStdString(d.locality)}, {"scope", QString::fromStdString(d.scope)},
        {"model_type", modelName(d.modelType)}, {"period_ratio", d.periodRatio},
        {"period_behavior", d.periodBehavior == PeriodBehavior::Octave ? "octave" :
            d.periodBehavior == PeriodBehavior::NonOctave ? "non_octave" :
            d.periodBehavior == PeriodBehavior::RegisterSpecific ? "register_specific" : "none"},
        {"author_or_community", QString::fromStdString(d.authorOrCommunity)}, {"ensemble", QString::fromStdString(d.ensemble)},
        {"instrument", QString::fromStdString(d.instrument)}, {"measurement_date", QString::fromStdString(d.measurementDate)},
        {"limitations", QString::fromStdString(d.limitations)}, {"appropriate_use", QString::fromStdString(d.appropriateUse)},
        {"reviewed", d.reviewed}, {"correction_eligible", d.correctionEligible},
        {"source_hash", QString::fromStdString(d.sourceHash)},
        {"correction_range_cents", d.correctionRangeCents}};
    QJsonArray sources;
    for (const auto& s : d.sources) sources.append(QJsonObject{{"citation", QString::fromStdString(s.citation)},
        {"url", QString::fromStdString(s.url)}, {"license", QString::fromStdString(s.license)}});
    root["sources"] = sources;
    QJsonArray reviews;
    for (const auto& r : d.reviews) reviews.append(QJsonObject{
        {"reviewer", QString::fromStdString(r.reviewer)},
        {"qualification_or_relationship", QString::fromStdString(r.qualificationOrRelationship)},
        {"scope", QString::fromStdString(r.scope)}, {"date", QString::fromStdString(r.date)},
        {"evidence_url", QString::fromStdString(r.evidenceUrl)}});
    root["reviews"] = reviews;
    if (d.mapping) {
        QJsonArray degrees;
        for (int degree : d.mapping->degrees) degrees.append(degree);
        root["mapping"] = QJsonObject{{"map_size", d.mapping->mapSize},
            {"first_midi", d.mapping->firstMidi}, {"last_midi", d.mapping->lastMidi},
            {"middle_midi", d.mapping->middleMidi}, {"reference_midi", d.mapping->referenceMidi},
            {"reference_hz", d.mapping->referenceHz},
            {"formal_octave_degree", d.mapping->formalOctaveDegree}, {"degrees", degrees}};
    }
    QJsonArray targets;
    for (const auto& t : d.targets) targets.append(QJsonObject{{"id", QString::fromStdString(t.id)},
        {"name", QString::fromStdString(t.name)}, {"original_name", QString::fromStdString(t.originalName)},
        {"ratio", t.ratio}, {"cents", t.cents}, {"frequency_hz", t.absoluteHz},
        {"uncertainty_cents", t.uncertaintyCents}, {"confidence", t.confidence},
        {"register_min", t.registerMin}, {"register_max", t.registerMax},
        {"direction", directionName(t.direction)}, {"function", QString::fromStdString(t.function)},
        {"instrument", QString::fromStdString(t.instrument)},
        {"paired_target_id", QString::fromStdString(t.pairedTargetId)}, {"beat_rate_hz", t.beatRateHz}});
    root["targets"] = targets;
    return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

PitchSystemRegistry::PitchSystemRegistry()
{
    for (int divisions : {12, 19, 24, 31, 53, 72}) definitions_.push_back(edo(divisions));
#ifdef JTUNE_BUNDLED_PITCH_SYSTEM_DIR
    const std::filesystem::path directory(JTUNE_BUNDLED_PITCH_SYSTEM_DIR);
    if (std::filesystem::is_directory(directory)) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".json" && entry.path().filename() != "manifest.json")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            std::vector<std::string> errors;
            loadFile(file.string(), errors);
        }
    }
#endif
}

const PitchSystemDefinition* PitchSystemRegistry::byIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(definitions_.size()) ? &definitions_[static_cast<size_t>(index)] : nullptr;
}

const PitchSystemDefinition* PitchSystemRegistry::byId(const std::string& id) const
{
    const auto it = std::find_if(definitions_.begin(), definitions_.end(), [&](const auto& d) { return d.id == id; });
    return it == definitions_.end() ? nullptr : &*it;
}

bool PitchSystemRegistry::add(PitchSystemDefinition definition, std::vector<std::string>& errors)
{
    errors = definition.validate();
    if (byId(definition.id)) errors.push_back("duplicate pitch-system id: " + definition.id);
    if (!errors.empty()) return false;
    definitions_.push_back(std::move(definition));
    return true;
}

bool PitchSystemRegistry::loadFile(const std::string& path, std::vector<std::string>& errors)
{
    std::ifstream input(path);
    if (!input) { errors = {"cannot open " + path}; return false; }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ParseResult parsed;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".scl") parsed = parseScalaScl(text, path);
    else if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") {
        parsed = parseDiarmaqarPitchClasses(text, path);
        if (!parsed.definition) parsed = parseMeasuredJson(text, path);
    }
    else { errors = {"unsupported pitch-system file extension: " + path}; return false; }
    errors = parsed.errors;
    if (parsed.definition && path.size() >= 4 && path.substr(path.size() - 4) == ".scl") {
        const std::string kbmPath = path.substr(0, path.size() - 4) + ".kbm";
        std::ifstream kbmInput(kbmPath);
        if (kbmInput) {
            const std::string kbmText((std::istreambuf_iterator<char>(kbmInput)), std::istreambuf_iterator<char>());
            const auto kbmErrors = applyScalaKbm(*parsed.definition, kbmText, kbmPath);
            errors.insert(errors.end(), kbmErrors.begin(), kbmErrors.end());
        }
    }
    if (!errors.empty()) return false;
    if (parsed.definition && parsed.definition->sourceHash.empty()) {
        const QByteArray digest = QCryptographicHash::hash(
            QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex();
        parsed.definition->sourceHash = "sha256:" + digest.toStdString();
    }
    return parsed.definition && add(std::move(*parsed.definition), errors);
}

PitchSystemRegistry& pitchSystemRegistry()
{
    static PitchSystemRegistry registry;
    return registry;
}

} // namespace jtune
