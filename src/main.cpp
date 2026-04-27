#include "RtAudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

float midiToHz(double midi)
{
    return static_cast<float>(440.0 * std::pow(2.0, (midi - 69.0) / 12.0));
}

struct AutotuneOptions {
    unsigned int sampleRate = 48000;
    unsigned int bufferFrames = 256;
    int keyRoot = 0;  // C
    bool minor = false;
    float strength = 1.0f;
    int minMidi = 40;
    int maxMidi = 84;
    int multiple = 24;
    float voicedThreshold = 2.2f;
};

int parseKeyRoot(const std::string& key)
{
    if (key == "C") return 0;
    if (key == "C#" || key == "Db") return 1;
    if (key == "D") return 2;
    if (key == "D#" || key == "Eb") return 3;
    if (key == "E") return 4;
    if (key == "F") return 5;
    if (key == "F#" || key == "Gb") return 6;
    if (key == "G") return 7;
    if (key == "G#" || key == "Ab") return 8;
    if (key == "A") return 9;
    if (key == "A#" || key == "Bb") return 10;
    if (key == "B") return 11;
    return -1;
}

bool inScale(int midiNote, int keyRoot, bool minor)
{
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int minorScale[7] = {0, 2, 3, 5, 7, 8, 10};
    int pc = ((midiNote % 12) + 12) % 12;
    const int* intervals = minor ? minorScale : major;
    for (int i = 0; i < 7; ++i) {
        if (((intervals[i] + keyRoot) % 12) == pc) return true;
    }
    return false;
}

int nearestScaleMidi(double detectedMidi, int keyRoot, bool minor)
{
    int center = static_cast<int>(std::lround(detectedMidi));
    int best = center;
    double bestDist = 1e9;
    for (int n = center - 24; n <= center + 24; ++n) {
        if (!inScale(n, keyRoot, minor)) continue;
        double d = std::abs(static_cast<double>(n) - detectedMidi);
        if (d < bestDist) {
            bestDist = d;
            best = n;
        }
    }
    return best;
}

struct DelayPitchShifter {
    explicit DelayPitchShifter(size_t sizeSamples)
        : ring(sizeSamples, 0.0f), ringSize(sizeSamples)
    {
        const float grain = 1024.0f;
        phaseA = 0.0f;
        phaseB = grain * 0.5f;
        sourceA = 0.0f;
        sourceB = 0.0f;
        grainSize = grain;
        baseDelay = 2048.0f;
    }

    float process(float in, float rate)
    {
        ring[writePos] = in;

        auto readInterp = [&](float pos) {
            float wrapped = std::fmod(pos, static_cast<float>(ringSize));
            if (wrapped < 0.0f) wrapped += static_cast<float>(ringSize);
            int i0 = static_cast<int>(wrapped);
            int i1 = (i0 + 1) % static_cast<int>(ringSize);
            float frac = wrapped - static_cast<float>(i0);
            return ring[static_cast<size_t>(i0)] * (1.0f - frac) + ring[static_cast<size_t>(i1)] * frac;
        };

        auto env = [&](float phase) {
            float x = phase / grainSize;
            return 1.0f - std::abs(2.0f * x - 1.0f);
        };

        const float write = static_cast<float>(writePos);
        if (phaseA == 0.0f) sourceA = write - baseDelay;
        if (phaseB == 0.0f) sourceB = write - baseDelay;

        float out = 0.0f;
        out += readInterp(sourceA) * env(phaseA);
        out += readInterp(sourceB) * env(phaseB);

        sourceA += rate;
        sourceB += rate;
        phaseA += 1.0f;
        phaseB += 1.0f;

        if (phaseA >= grainSize) {
            phaseA -= grainSize;
            sourceA = write - baseDelay;
        }
        if (phaseB >= grainSize) {
            phaseB -= grainSize;
            sourceB = write - baseDelay;
        }

        writePos = (writePos + 1) % ringSize;
        return out;
    }

    std::vector<float> ring;
    size_t ringSize;
    size_t writePos = 0;
    float phaseA = 0.0f;
    float phaseB = 0.0f;
    float sourceA = 0.0f;
    float sourceB = 0.0f;
    float grainSize = 1024.0f;
    float baseDelay = 2048.0f;
};

struct LoiaconoLikeDetector {
    explicit LoiaconoLikeDetector(const AutotuneOptions& opts)
    {
        const int noteCount = opts.maxMidi - opts.minMidi + 1;
        midiNotes.reserve(static_cast<size_t>(noteCount));
        freqs.reserve(static_cast<size_t>(noteCount));
        windowLengths.reserve(static_cast<size_t>(noteCount));

        for (int midi = opts.minMidi; midi <= opts.maxMidi; ++midi) {
            float hz = midiToHz(static_cast<double>(midi));
            float fprime = hz / static_cast<float>(opts.sampleRate);
            int wlen = static_cast<int>(std::ceil(static_cast<float>(opts.multiple) / fprime));
            wlen = std::clamp(wlen, 256, 8192);
            midiNotes.push_back(midi);
            freqs.push_back(fprime);
            windowLengths.push_back(wlen);
            maxWindow = std::max(maxWindow, wlen);
        }

        history.assign(static_cast<size_t>(maxWindow), 0.0f);
    }

    void push(float sample)
    {
        history[writePos] = sample;
        writePos = (writePos + 1) % history.size();
        if (filled < static_cast<int>(history.size())) ++filled;
    }

    bool estimatePitch(double& midiOut, float& confidenceOut)
    {
        if (filled < 512) return false;

        float bestMag = 0.0f;
        float secondMag = 1e-6f;
        int bestIdx = -1;

        for (size_t i = 0; i < freqs.size(); ++i) {
            const int wlen = std::min(windowLengths[i], filled);
            const float f = freqs[i];
            const float norm = 1.0f / std::sqrt(static_cast<float>(wlen));

            double tr = 0.0;
            double ti = 0.0;
            const size_t start = (writePos + history.size() - static_cast<size_t>(wlen)) % history.size();

            for (int n = 0; n < wlen; ++n) {
                const size_t idx = (start + static_cast<size_t>(n)) % history.size();
                const float x = history[idx];
                const double angle = 2.0 * M_PI * static_cast<double>(f) * static_cast<double>(n);
                tr += static_cast<double>(x) * std::cos(angle) * norm;
                ti -= static_cast<double>(x) * std::sin(angle) * norm;
            }

            const float mag = static_cast<float>(std::sqrt(tr * tr + ti * ti));
            if (mag > bestMag) {
                secondMag = bestMag;
                bestMag = mag;
                bestIdx = static_cast<int>(i);
            } else if (mag > secondMag) {
                secondMag = mag;
            }
        }

        if (bestIdx < 0) return false;
        midiOut = static_cast<double>(midiNotes[static_cast<size_t>(bestIdx)]);
        confidenceOut = bestMag / std::max(secondMag, 1e-6f);
        return true;
    }

    std::vector<int> midiNotes;
    std::vector<float> freqs;
    std::vector<int> windowLengths;
    std::vector<float> history;
    size_t writePos = 0;
    int filled = 0;
    int maxWindow = 0;
};

struct AppState {
    explicit AppState(const AutotuneOptions& opts)
        : options(opts),
          shifter(static_cast<size_t>(opts.sampleRate * 2)),
          detector(opts)
    {
    }

    AutotuneOptions options;
    DelayPitchShifter shifter;
    LoiaconoLikeDetector detector;
    int callbacksSinceAnalysis = 0;
    float smoothedRate = 1.0f;
};

int audioCallback(void* outputBuffer,
                  void* inputBuffer,
                  unsigned int nFrames,
                  double,
                  RtAudioStreamStatus status,
                  void* userData)
{
    if (status) {
        std::cerr << "RtAudio stream status: " << status << "\n";
    }

    auto* state = static_cast<AppState*>(userData);
    auto* in = static_cast<const float*>(inputBuffer);
    auto* out = static_cast<float*>(outputBuffer);
    if (!in || !out) return 0;

    for (unsigned int i = 0; i < nFrames; ++i) {
        float sample = in[i];
        state->detector.push(sample);
        out[i] = state->shifter.process(sample, state->smoothedRate);
    }

    state->callbacksSinceAnalysis++;
    if (state->callbacksSinceAnalysis >= 4) {
        state->callbacksSinceAnalysis = 0;
        double detectedMidi = 0.0;
        float confidence = 0.0f;
        if (state->detector.estimatePitch(detectedMidi, confidence) && confidence >= state->options.voicedThreshold) {
            int targetMidi = nearestScaleMidi(detectedMidi, state->options.keyRoot, state->options.minor);
            double semitones = (static_cast<double>(targetMidi) - detectedMidi) * state->options.strength;
            semitones = std::clamp(semitones, -6.0, 6.0);
            float targetRate = static_cast<float>(std::pow(2.0, semitones / 12.0));
            state->smoothedRate = state->smoothedRate * 0.85f + targetRate * 0.15f;
        } else {
            state->smoothedRate = state->smoothedRate * 0.9f + 1.0f * 0.1f;
        }
    }

    return 0;
}

void printUsage(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [--key C] [--scale major|minor] [--strength 1.0]"
              << " [--sample-rate 48000] [--buffer-frames 256] [--min-midi 40] [--max-midi 84]"
              << " [--multiple 24] [--voiced-threshold 2.2]\n";
}

bool parseArgs(int argc, char** argv, AutotuneOptions& opts, bool& showHelp)
{
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto needValue = [&](const std::string& name) {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return false;
            }
            return true;
        };

        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            showHelp = true;
            return false;
        }
        if (a == "--key") {
            if (!needValue(a)) return false;
            int key = parseKeyRoot(argv[++i]);
            if (key < 0) {
                std::cerr << "Invalid key\n";
                return false;
            }
            opts.keyRoot = key;
        } else if (a == "--scale") {
            if (!needValue(a)) return false;
            std::string v = argv[++i];
            if (v == "major") opts.minor = false;
            else if (v == "minor") opts.minor = true;
            else {
                std::cerr << "Scale must be major or minor\n";
                return false;
            }
        } else if (a == "--strength") {
            if (!needValue(a)) return false;
            opts.strength = std::clamp(std::stof(argv[++i]), 0.0f, 1.0f);
        } else if (a == "--sample-rate") {
            if (!needValue(a)) return false;
            opts.sampleRate = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (a == "--buffer-frames") {
            if (!needValue(a)) return false;
            opts.bufferFrames = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (a == "--min-midi") {
            if (!needValue(a)) return false;
            opts.minMidi = std::stoi(argv[++i]);
        } else if (a == "--max-midi") {
            if (!needValue(a)) return false;
            opts.maxMidi = std::stoi(argv[++i]);
        } else if (a == "--multiple") {
            if (!needValue(a)) return false;
            opts.multiple = std::stoi(argv[++i]);
        } else if (a == "--voiced-threshold") {
            if (!needValue(a)) return false;
            opts.voicedThreshold = std::stof(argv[++i]);
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            return false;
        }
    }

    if (opts.minMidi >= opts.maxMidi) {
        std::cerr << "min-midi must be < max-midi\n";
        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    AutotuneOptions options;
    bool showHelp = false;
    if (!parseArgs(argc, argv, options, showHelp)) {
        if (showHelp) return 0;
        return 1;
    }

    try {
        RtAudio audio;
        if (audio.getDeviceCount() < 1) {
            std::cerr << "No audio devices found.\n";
            return 1;
        }

        RtAudio::StreamParameters inputParams;
        RtAudio::StreamParameters outputParams;
        inputParams.deviceId = audio.getDefaultInputDevice();
        outputParams.deviceId = audio.getDefaultOutputDevice();
        inputParams.nChannels = 1;
        outputParams.nChannels = 1;
        inputParams.firstChannel = 0;
        outputParams.firstChannel = 0;

        AppState state(options);
        unsigned int frames = options.bufferFrames;

        audio.openStream(
            &outputParams,
            &inputParams,
            RTAUDIO_FLOAT32,
            options.sampleRate,
            &frames,
            &audioCallback,
            &state
        );

        std::cout << "JTune C++ autotune running. Press Ctrl+C to stop.\n";
        std::cout << "key=" << options.keyRoot << " scale=" << (options.minor ? "minor" : "major")
                  << " strength=" << options.strength << " sr=" << options.sampleRate
                  << " buffer=" << frames << "\n";

        audio.startStream();
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (audio.isStreamRunning()) audio.stopStream();
        if (audio.isStreamOpen()) audio.closeStream();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown error while running RtAudio stream.\n";
        return 1;
    }

    return 0;
}
