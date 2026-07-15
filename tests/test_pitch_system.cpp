#include "pitch_system.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    auto& registry = jtune::pitchSystemRegistry();
    assert(registry.definitions().size() == 16);
    for (const auto& definition : registry.definitions()) {
        assert(definition.validate().empty());
        assert(!definition.id.empty());
        assert(!definition.sources.empty());
        assert(!definition.limitations.empty());
        assert(!definition.sourceHash.empty());
    }

    const std::string scl = R"(! exact 12 edo
Test 12 EDO
12
100.0
200.0
300.0
400.0
500.0
600.0
700.0
800.0
900.0
1000.0
1100.0
2/1
)";
    auto parsed = jtune::parseScalaScl(scl, "test.scl");
    assert(parsed.errors.empty());
    assert(parsed.definition);
    assert(parsed.definition->targets.size() == 12);
    assert(parsed.definition->periodBehavior == jtune::PeriodBehavior::Octave);
    assert(parsed.definition->correctionEligible);

    const std::string kbm = R"(12
0
127
60
69
440.0
12
0
1
2
3
4
5
6
7
8
9
10
11
)";
    auto kbmErrors = jtune::applyScalaKbm(*parsed.definition, kbm, "test.kbm");
    assert(kbmErrors.empty());
    jtune::PitchContext context;
    context.referenceMidi = 69;
    context.referenceHz = 440.0;
    jtune::PitchSystemEvaluator evaluator(*parsed.definition);
    assert(std::abs(*evaluator.frequencyForMidi(69, context) - 440.0) < 1e-9);
    assert(std::abs(*evaluator.frequencyForMidi(81, context) - 880.0) < 1e-6);
    const auto scalaJson = jtune::exportPitchSystemJson(*parsed.definition);
    const auto scalaRoundTrip = jtune::parseMeasuredJson(scalaJson, "scala-roundtrip.json");
    assert(scalaRoundTrip.definition);
    assert(scalaRoundTrip.definition->mapping);
    assert(scalaRoundTrip.definition->mapping->degrees == parsed.definition->mapping->degrees);
    const double cSharp4 = 440.0 * std::pow(2.0, (61.0 - 69.0) / 12.0);
    const auto chromaticTarget = evaluator.nearest(cSharp4, context);
    assert(chromaticTarget);
    assert(std::abs(chromaticTarget->frequencyHz - cSharp4) < 1e-6); // no implicit C-major mask

    const auto* ionian = jtune::pitchCollectionById("12edo.ionian");
    assert(ionian && ionian->degreeCount == 12);
    context.tonicMidi = 60; // C
    context.enabledDegrees = &ionian->enabledDegrees;
    const auto cIonianTarget = evaluator.nearest(cSharp4, context);
    assert(cIonianTarget);
    assert(std::abs(cIonianTarget->frequencyHz - cSharp4) > 1.0);
    const double fSharp4 = 440.0 * std::pow(2.0, (66.0 - 69.0) / 12.0);
    context.tonicMidi = 62; // D Ionian includes F#
    const auto dIonianTarget = evaluator.nearest(fSharp4, context);
    assert(dIonianTarget);
    assert(std::abs(dIonianTarget->frequencyHz - fSharp4) < 1e-6);
    context.enabledDegrees = nullptr;

    const auto degreeList = jtune::parsePitchDegreeList("0, 3,7,10");
    assert(degreeList && degreeList->size() == 4);
    assert(!jtune::parsePitchDegreeList("0,3,3"));

    // Reference/drone pitches must be supplied by the active mathematical system,
    // including its user-selected reference frequency and applied octave shift.
    const auto* edo19 = registry.byId("org.jtune.edo.19");
    assert(edo19);
    jtune::PitchContext edo19Context;
    edo19Context.referenceMidi = 69;
    edo19Context.referenceHz = 432.0;
    edo19Context.octaveShift = -1;
    jtune::PitchSystemEvaluator edo19Evaluator(*edo19);
    const auto edo19DefaultDrone = edo19Evaluator.frequencyForMidi(
        edo19Context.referenceMidi, edo19Context);
    assert(edo19DefaultDrone);
    assert(std::abs(*edo19DefaultDrone - edo19Context.referenceHz / 2.0) < 1e-9);
    const auto edo19Drone = edo19Evaluator.frequencyForMidi(70, edo19Context);
    assert(edo19Drone);
    const double expectedEdo19Drone = 432.0 * std::pow(2.0, 1.0 / 19.0) / 2.0;
    assert(std::abs(*edo19Drone - expectedEdo19Drone) < 1e-9);
    assert(std::abs(*edo19Drone - 432.0 * std::pow(2.0, (70.0 - 69.0) / 12.0) / 2.0) > 0.1);

    // Simulate live control changes while retaining the same drone key. Each
    // result must be recalculated from the current system and context.
    edo19Context.referenceHz = 440.0;
    const auto retunedReferenceDrone = edo19Evaluator.frequencyForMidi(70, edo19Context);
    assert(retunedReferenceDrone);
    assert(std::abs(*retunedReferenceDrone - 440.0 * std::pow(2.0, 1.0 / 19.0) / 2.0) < 1e-9);
    assert(std::abs(*retunedReferenceDrone - *edo19Drone) > 1.0);

    const auto* edo12 = registry.byId("org.jtune.edo.12");
    assert(edo12);
    jtune::PitchSystemEvaluator edo12Evaluator(*edo12);
    const auto retunedSystemDrone = edo12Evaluator.frequencyForMidi(70, edo19Context);
    assert(retunedSystemDrone);
    assert(std::abs(*retunedSystemDrone - 440.0 * std::pow(2.0, 1.0 / 12.0) / 2.0) < 1e-9);
    assert(std::abs(*retunedSystemDrone - *retunedReferenceDrone) > 0.1);

    const std::string measured = R"({
      "id":"example.measured.instrument","version":"1.0.0",
      "display_name":"Example measured instrument","model_type":"measured",
      "period_ratio":0,"author_or_community":"Test fixture",
      "limitations":"Test data only","appropriate_use":"Automated tests only",
      "sources":[{"citation":"Synthetic fixture","url":"","license":"CC0-1.0"}],
      "targets":[
        {"id":"key-a","name":"A","frequency_hz":220.0,"midi_note":57,"uncertainty_cents":0.2},
        {"id":"key-b","name":"B","frequency_hz":331.0,"midi_note":64,"uncertainty_cents":0.3}
      ]
    })";
    auto measuredParsed = jtune::parseMeasuredJson(measured, "fixture.json");
    assert(measuredParsed.errors.empty());
    assert(measuredParsed.definition);
    jtune::PitchSystemEvaluator measuredEvaluator(*measuredParsed.definition);
    assert(std::abs(*measuredEvaluator.frequencyForMidi(57, context) - 220.0) < 1e-9);
    assert(!measuredEvaluator.frequencyForMidi(58, context));

    const auto exported = jtune::exportPitchSystemJson(*measuredParsed.definition);
    auto roundTrip = jtune::parseMeasuredJson(exported, "roundtrip.json");
    assert(roundTrip.errors.empty());
    assert(roundTrip.definition->id == measuredParsed.definition->id);

    const auto malformed = jtune::parseScalaScl("broken\n2\n100.0\n", "broken.scl");
    assert(!malformed.definition);
    assert(!malformed.errors.empty());

    const std::string historical = R"({
      "tuningSystem":{"id":"ibnsina_1037","displayName":"Ibn Sina 17-Tone","version":"v1"},
      "selectedStartingNote":{"idName":"yegah","displayName":"yegah"},
      "pitchClasses":[
        {"noteName":"hisar","noteNameDisplay":"hisar","cents":111.3085691},
        {"noteName":"nawa","noteNameDisplay":"nawa","cents":1200.0}
      ]
    })";
    auto historicalParsed = jtune::parseDiarmaqarPitchClasses(historical, "archive.json");
    assert(historicalParsed.errors.empty());
    assert(historicalParsed.definition);
    assert(historicalParsed.definition->id == "org.diarmaqar.ibnsina_1037.yegah");
    assert(historicalParsed.definition->modelType == jtune::PitchModelType::Theoretical);
    assert(!historicalParsed.definition->reviewed);
    assert(!historicalParsed.definition->correctionEligible);
    assert(historicalParsed.definition->sources.front().license == "CC BY-NC-SA 4.0");

    auto sparse = jtune::parseScalaScl(scl, "sparse.scl");
    const std::string sparseKbm = R"(3
0
127
60
60
261.625565
12
0
x
7
)";
    assert(jtune::applyScalaKbm(*sparse.definition, sparseKbm, "sparse.kbm").empty());
    jtune::PitchSystemEvaluator sparseEvaluator(*sparse.definition);
    jtune::PitchContext sparseContext;
    sparseContext.referenceMidi = 60;
    sparseContext.referenceHz = 261.625565;
    assert(sparseEvaluator.frequencyForMidi(60, sparseContext));
    assert(!sparseEvaluator.frequencyForMidi(61, sparseContext));

    int ordProfiles = 0;
    for (const auto& profile : registry.definitions()) {
        if (profile.tradition != "1932 Cairo Congress historical performance") continue;
        assert(profile.modelType == jtune::PitchModelType::Measured);
        assert(!profile.reviewed);
        assert(!profile.correctionEligible);
        assert(profile.tradition == "1932 Cairo Congress historical performance");
        assert(!profile.region.empty());
        assert(profile.sources.front().license == "CC BY-NC 4.0");
        ++ordProfiles;
    }
    assert(ordProfiles == 10);

    auto unreviewedMeasured = *measuredParsed.definition;
    unreviewedMeasured.correctionEligible = true;
    const auto unreviewedErrors = unreviewedMeasured.validate();
    assert(std::find_if(unreviewedErrors.begin(), unreviewedErrors.end(), [](const auto& error) {
        return error.find("documented review") != std::string::npos;
    }) != unreviewedErrors.end());

    unreviewedMeasured.reviewed = true;
    unreviewedMeasured.reviews.push_back({"Test reviewer", "Synthetic test role",
        "Parser gate only", "2026-07-14", ""});
    assert(unreviewedMeasured.validate().empty());

    auto invalidPair = unreviewedMeasured;
    invalidPair.targets.front().pairedTargetId = "missing-target";
    assert(!invalidPair.validate().empty());

    std::cout << "pitch-system parsers, provenance, mapping, and evaluator validated\n";
    return 0;
}
