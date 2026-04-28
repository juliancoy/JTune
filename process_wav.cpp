#include "autotune_core.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct WavData {
    unsigned int sampleRate = 0;
    std::vector<float> samples;
};

bool readWavMono(const std::filesystem::path& path, WavData& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char riff[4] = {};
    uint32_t riffSize = 0;
    char wave[4] = {};
    f.read(riff, 4);
    f.read(reinterpret_cast<char*>(&riffSize), 4);
    f.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") return false;

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    std::vector<char> data;

    while (f && !f.eof()) {
        char chunkId[4] = {};
        uint32_t chunkSize = 0;
        f.read(chunkId, 4);
        if (!f) break;
        f.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!f) break;

        const std::string id(chunkId, 4);
        if (id == "fmt ") {
            uint16_t blockAlign = 0;
            uint32_t byteRate = 0;
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&channels), 2);
            f.read(reinterpret_cast<char*>(&sampleRate), 4);
            f.read(reinterpret_cast<char*>(&byteRate), 4);
            f.read(reinterpret_cast<char*>(&blockAlign), 2);
            f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (chunkSize > 16) {
                f.seekg(static_cast<std::streamoff>(chunkSize - 16), std::ios::cur);
            }
        } else if (id == "data") {
            data.resize(chunkSize);
            if (chunkSize > 0) f.read(data.data(), static_cast<std::streamsize>(chunkSize));
        } else {
            f.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }
        if (chunkSize % 2 == 1) f.seekg(1, std::ios::cur);
    }

    if (sampleRate == 0 || channels == 0 || data.empty()) return false;
    out.sampleRate = sampleRate;

    if (audioFormat == 1 && bitsPerSample == 16) {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data.data());
        const size_t total = data.size() / sizeof(int16_t);
        const size_t frames = total / channels;
        out.samples.assign(frames, 0.0f);
        for (size_t i = 0; i < frames; ++i) {
            const int16_t s = pcm[i * channels];
            out.samples[i] = static_cast<float>(s / 32768.0f);
        }
        return true;
    }

    if (audioFormat == 3 && bitsPerSample == 32) {
        const float* pcm = reinterpret_cast<const float*>(data.data());
        const size_t total = data.size() / sizeof(float);
        const size_t frames = total / channels;
        out.samples.assign(frames, 0.0f);
        for (size_t i = 0; i < frames; ++i) {
            out.samples[i] = pcm[i * channels];
        }
        return true;
    }

    return false;
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

int parseResynth(const std::string& value)
{
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "timedomain" || v == "time-domain" || v == "time" || v == "flow" || v == "time-domain-flow") {
        return jtune::TimeDomain;
    }
    return jtune::FrequencyDomain;
}

}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path inPath;
    std::filesystem::path outPath;

    jtune::AutotuneOptions opts;
    opts.sampleRate = 48000;
    opts.keyRoot = 0;
    opts.minor = false;
    opts.strength = 1.0f;
    opts.wetMix = 1.0f;
    opts.minMidi = 40;
    opts.maxMidi = 84;
    opts.multiple = 40;
    opts.binCount = 256;
    opts.analysisHop = 96;
    opts.voicedThreshold = 0.20f;
    opts.freqMinHz = 100.0;
    opts.freqMaxHz = 3000.0;
    opts.leakiness = 0.9995;
    opts.baseAFrequencyHz = 440.0;
    opts.computeMode = 1;
    opts.windowMode = 0;
    opts.normalizationMode = 2;
    opts.windowLengthMode = 2;
    opts.algorithmMode = 0;
    opts.resynthMode = jtune::TimeDomain;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--in") inPath = next();
        else if (arg == "--out") outPath = next();
        else if (arg == "--sample-rate") opts.sampleRate = static_cast<unsigned int>(std::stoul(next()));
        else if (arg == "--key") opts.keyRoot = parseKeyRoot(next());
        else if (arg == "--scale") opts.minor = std::string(next()) == "minor";
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
        else if (arg == "--resynth") opts.resynthMode = parseResynth(next());
        else if (arg == "--help") {
            std::cout << "Usage: jtune_process_wav --in in.wav [--out out.wav] [options]\n";
            return 0;
        } else {
            std::cerr << "Unknown arg: " << arg << "\n";
            return 2;
        }
    }

    if (inPath.empty()) {
        std::cerr << "--in is required\n";
        return 2;
    }

    if (outPath.empty()) {
        const std::filesystem::path parent = inPath.has_parent_path() ? inPath.parent_path() : std::filesystem::path(".");
        const std::string stem = inPath.stem().string();
        const std::string ext = inPath.has_extension() ? inPath.extension().string() : std::string(".wav");
        outPath = parent / (stem + "_autotune" + ext);
    }

    WavData wav;
    if (!readWavMono(inPath, wav)) {
        std::cerr << "Failed reading wav: " << inPath << "\n";
        return 1;
    }

    opts.sampleRate = wav.sampleRate;
    jtune::ConstantQAutotuneProcessor proc(opts);
    std::vector<float> out(wav.samples.size(), 0.0f);
    for (size_t i = 0; i < wav.samples.size(); ++i) {
        out[i] = proc.processSample(wav.samples[i]);
    }

    std::filesystem::create_directories(outPath.parent_path());
    if (!writeWavMono16(outPath, out, wav.sampleRate)) {
        std::cerr << "Failed writing wav: " << outPath << "\n";
        return 1;
    }

    std::cout << "Wrote " << outPath << "\n";
    return 0;
}
