#include "autotune_core.h"
#include "pitch_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> generateChirp(unsigned int sampleRate, double seconds, double f0Hz, double f1Hz)
{
    const size_t total = static_cast<size_t>(seconds * static_cast<double>(sampleRate));
    std::vector<float> out(total, 0.0f);

    double phase = 0.0;
    for (size_t n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        const double alpha = std::clamp(t / seconds, 0.0, 1.0);
        const double f = f0Hz + (f1Hz - f0Hz) * alpha;
        phase += 2.0 * kPi * f / static_cast<double>(sampleRate);
        out[n] = static_cast<float>(0.2 * std::sin(phase));
    }

    return out;
}

bool writeWavMono16(const std::filesystem::path& path,
                    const std::vector<float>& samples,
                    unsigned int sampleRate)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const uint16_t numChannels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36u + dataBytes;

    auto writeU16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto writeU32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };

    f.write("RIFF", 4);
    writeU32(riffSize);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    writeU32(16);
    writeU16(1);
    writeU16(numChannels);
    writeU32(sampleRate);
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);

    f.write("data", 4);
    writeU32(dataBytes);

    for (float s : samples) {
        const float clamped = std::clamp(s, -1.0f, 1.0f);
        const int16_t pcm = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        f.write(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
    }

    return static_cast<bool>(f);
}

int parseKeyRoot(const std::string& value)
{
    const std::string k = value;
    if (k == "C") return 0;
    if (k == "C#" || k == "Db") return 1;
    if (k == "D") return 2;
    if (k == "D#" || k == "Eb") return 3;
    if (k == "E") return 4;
    if (k == "F") return 5;
    if (k == "F#" || k == "Gb") return 6;
    if (k == "G") return 7;
    if (k == "G#" || k == "Ab") return 8;
    if (k == "A") return 9;
    if (k == "A#" || k == "Bb") return 10;
    if (k == "B") return 11;
    return 0;
}

int parseAlgorithm(const std::string& value)
{
    if (value == "fft") return 1;
    if (value == "goertzel") return 2;
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path outDir = "tests/sounds";

    jtune::AutotuneOptions opts;
    opts.sampleRate = 48000;
    opts.strength = 1.0f;
    opts.wetMix = 1.0f;
    opts.minMidi = 40;
    opts.maxMidi = 84;
    opts.multiple = 40;
    opts.binCount = 256;
    opts.analysisHop = 96;
    opts.voicedThreshold = 0.05f;
    opts.freqMinHz = 100.0;
    opts.freqMaxHz = 3000.0;
    opts.leakiness = 0.9995;
    opts.baseAFrequencyHz = 440.0;
    opts.computeMode = 1;
    opts.windowMode = 0;
    opts.normalizationMode = 2;
    opts.windowLengthMode = 2;
    opts.algorithmMode = 0;

    double seconds = 4.0;
    double f0Hz = 275.0;
    double f1Hz = 282.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--out-dir") outDir = next();
        else if (arg == "--sample-rate") opts.sampleRate = static_cast<unsigned int>(std::stoul(next()));
        else if (arg == "--pitch-system") opts.pitchSystemId = next();
        else if (arg == "--tuning-file") {
            std::vector<std::string> errors;
            if (!jtune::pitchSystemRegistry().loadFile(next(), errors)) { for (const auto& e : errors) std::cerr << e << '\n'; return 2; }
            opts.pitchSystemId = jtune::pitchSystemRegistry().definitions().back().id;
        }
        else if (arg == "--reference-note") opts.referenceMidiNote = std::stoi(next());
        else if (arg == "--reference-hz") opts.baseAFrequencyHz = std::stod(next());
        else if (arg == "--octave-shift") opts.octaveShift = std::stoi(next());
        else if (arg == "--pitch-collection") opts.pitchCollectionId = next();
        else if (arg == "--tonic-note") opts.tonicMidiNote = std::stoi(next());
        else if (arg == "--degrees") { const auto values = jtune::parsePitchDegreeList(next()); if (!values) return 2; opts.pitchCollectionId = "custom"; opts.customEnabledDegrees = *values; }
        else if (arg == "--key" || arg == "--scale") { std::cerr << arg << " was removed\n"; return 2; }
        else if (arg == "--strength") opts.strength = std::stof(next());
        else if (arg == "--wet") opts.wetMix = std::stof(next());
        else if (arg == "--min-midi") opts.minMidi = std::stoi(next());
        else if (arg == "--max-midi") opts.maxMidi = std::stoi(next());
        else if (arg == "--multiple") opts.multiple = std::stoi(next());
        else if (arg == "--bins") opts.binCount = std::stoi(next());
        else if (arg == "--hop") opts.analysisHop = std::stoi(next());
        else if (arg == "--voiced-threshold") opts.voicedThreshold = std::stof(next());
        else if (arg == "--freq-min") opts.freqMinHz = std::stod(next());
        else if (arg == "--freq-max") opts.freqMaxHz = std::stod(next());
        else if (arg == "--leakiness") opts.leakiness = std::stod(next());
        else if (arg == "--base-a") opts.baseAFrequencyHz = std::stod(next());
        else if (arg == "--algorithm") opts.algorithmMode = parseAlgorithm(next());
        else if (arg == "--seconds") seconds = std::stod(next());
        else if (arg == "--f0") f0Hz = std::stod(next());
        else if (arg == "--f1") f1Hz = std::stod(next());
        else if (arg == "--help") {
            std::cout
                << "Usage: jtune_export_chirp [options]\n"
                << "  --out-dir PATH\n"
                << "  --sample-rate N\n"
                << "  --pitch-system ID | --tuning-file FILE\n"
                << "  --reference-note MIDI --reference-hz HZ --octave-shift -2..2\n"
                << "  --strength 0..1\n"
                << "  --wet 0..1\n"
                << "  --algorithm loiacono|fft|goertzel\n"
                << "  --seconds N --f0 HZ --f1 HZ\n";
            return 0;
        } else {
            std::cerr << "Unknown arg: " << arg << "\n";
            return 2;
        }
    }

    if (opts.minMidi >= opts.maxMidi) {
        std::cerr << "min-midi must be < max-midi\n";
        return 2;
    }

    std::filesystem::create_directories(outDir);

    const auto input = generateChirp(opts.sampleRate, seconds, f0Hz, f1Hz);

    jtune::ConstantQAutotuneProcessor proc(opts);
    std::vector<float> tuned(input.size(), 0.0f);
    for (size_t i = 0; i < input.size(); ++i) {
        tuned[i] = proc.processSample(input[i]);
    }

    const auto inPath = outDir / "chirp_input.wav";
    const auto outPath = outDir / "chirp_input_autotune.wav";

    if (!writeWavMono16(inPath, input, opts.sampleRate)) {
        std::cerr << "Failed writing " << inPath << "\n";
        return 1;
    }
    if (!writeWavMono16(outPath, tuned, opts.sampleRate)) {
        std::cerr << "Failed writing " << outPath << "\n";
        return 1;
    }

    std::cout << "Wrote:\n  " << inPath << "\n  " << outPath << "\n";
    return 0;
}
