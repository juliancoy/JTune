#include "autotune_core.h"
#include "pitch_system.hpp"

#include "RtAudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

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

struct RuntimeOptions {
    jtune::AutotuneOptions dsp;
    unsigned int bufferFrames = 256;
    bool listDevices = false;
    bool hasInputDevice = false;
    bool hasOutputDevice = false;
    unsigned int inputDevice = 0;
    unsigned int outputDevice = 0;
    RtAudio::Api api = RtAudio::UNSPECIFIED;
};

struct AppState {
    explicit AppState(const RuntimeOptions& opts)
        : options(opts), processor(opts.dsp)
    {
    }

    RuntimeOptions options;
    jtune::ConstantQAutotuneProcessor processor;
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
    if (!out) return 0;

    state->processor.processBuffer(in, out, nFrames);
    return 0;
}

bool parseApi(const std::string& value, RtAudio::Api& outApi)
{
    if (value == "unspecified") {
        outApi = RtAudio::UNSPECIFIED;
        return true;
    }
    if (value == "alsa") {
        outApi = RtAudio::LINUX_ALSA;
        return true;
    }
    if (value == "pulse") {
        outApi = RtAudio::LINUX_PULSE;
        return true;
    }
    if (value == "jack") {
        outApi = RtAudio::UNIX_JACK;
        return true;
    }
    return false;
}

bool parseAlgorithm(const std::string& value, int& outMode)
{
    if (value == "loiacono") {
        outMode = 0;
        return true;
    }
    if (value == "fft") {
        outMode = 1;
        return true;
    }
    if (value == "goertzel") {
        outMode = 2;
        return true;
    }
    return false;
}

bool parseResynth(const std::string& value, int& outMode)
{
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (v == "frequencydomain" || v == "frequency-domain" || v == "frequency" || v == "freq") {
        outMode = jtune::FrequencyDomain;
        return true;
    }
    if (v == "timedomain" || v == "time-domain" || v == "time" || v == "flow" || v == "time-domain-flow") {
        outMode = jtune::TimeDomain;
        return true;
    }
    return false;
}

void printDevices(RtAudio& audio)
{
    const auto ids = audio.getDeviceIds();
    std::cout << "Audio devices (" << ids.size() << "):\n";
    for (unsigned int id : ids) {
        auto info = audio.getDeviceInfo(id);
        std::cout << "  [" << id << "] " << info.name
                  << " in=" << info.inputChannels
                  << " out=" << info.outputChannels;
        if (info.isDefaultInput) std::cout << " default-in";
        if (info.isDefaultOutput) std::cout << " default-out";
        std::cout << "\n";
    }
}

void printUsage(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [--pitch-system ID] [--tuning-file FILE]"
              << " [--reference-note 69] [--reference-hz 440] [--octave-shift -2..2] [--strength 1.0]"
              << " [--sample-rate 48000] [--buffer-frames 256] [--min-midi 40] [--max-midi 84]"
              << " [--multiple 24] [--voiced-threshold 1.1] [--bin-count 128] [--analysis-hop 256]"
              << " [--freq-min 100] [--freq-max 3000] [--algorithm loiacono|fft|goertzel]"
              << " [--resynth FrequencyDomain|TimeDomain] [--ratio-smoothing 0.15] [--amp-smoothing 0.15] [--phase-pull 0.08]"
              << " [--flow-grain-ms 20] [--flow-overlap 0.75] [--flow-delay-ms 40] [--flow-drift 0.01]"
              << " [--list-devices] [--input-device ID] [--output-device ID]"
              << " [--api unspecified|alsa|pulse|jack]\n";
}

bool parseArgs(int argc, char** argv, RuntimeOptions& opts, bool& showHelp)
{
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
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
        } else if (a == "--pitch-system") {
            if (!needValue(a)) return false;
            opts.dsp.pitchSystemId = argv[++i];
        } else if (a == "--tuning-file") {
            if (!needValue(a)) return false;
            std::vector<std::string> errors;
            if (!jtune::pitchSystemRegistry().loadFile(argv[++i], errors)) {
                for (const auto& error : errors) std::cerr << error << '\n';
                return false;
            }
            opts.dsp.pitchSystemId = jtune::pitchSystemRegistry().definitions().back().id;
        } else if (a == "--reference-note") {
            if (!needValue(a)) return false; opts.dsp.referenceMidiNote = std::stoi(argv[++i]);
        } else if (a == "--reference-hz") {
            if (!needValue(a)) return false; opts.dsp.baseAFrequencyHz = std::stod(argv[++i]);
        } else if (a == "--octave-shift") {
            if (!needValue(a)) return false; opts.dsp.octaveShift = std::stoi(argv[++i]);
        } else if (a == "--pitch-collection") {
            if (!needValue(a)) return false; opts.dsp.pitchCollectionId = argv[++i];
        } else if (a == "--tonic-note") {
            if (!needValue(a)) return false; opts.dsp.tonicMidiNote = std::stoi(argv[++i]);
        } else if (a == "--degrees") {
            if (!needValue(a)) return false;
            const auto values = jtune::parsePitchDegreeList(argv[++i]);
            if (!values) { std::cerr << "Invalid comma-separated degree list\n"; return false; }
            opts.dsp.pitchCollectionId = "custom"; opts.dsp.customEnabledDegrees = *values;
        } else if (a == "--key" || a == "--scale") {
            std::cerr << a << " was removed; choose an explicit pitch system and reference pitch\n";
            return false;
        } else if (a == "--strength") {
            if (!needValue(a)) return false;
            opts.dsp.strength = std::clamp(std::stof(argv[++i]), 0.0f, 1.0f);
        } else if (a == "--sample-rate") {
            if (!needValue(a)) return false;
            opts.dsp.sampleRate = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (a == "--buffer-frames") {
            if (!needValue(a)) return false;
            opts.bufferFrames = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (a == "--min-midi") {
            if (!needValue(a)) return false;
            opts.dsp.minMidi = std::stoi(argv[++i]);
        } else if (a == "--max-midi") {
            if (!needValue(a)) return false;
            opts.dsp.maxMidi = std::stoi(argv[++i]);
        } else if (a == "--multiple") {
            if (!needValue(a)) return false;
            opts.dsp.multiple = std::stoi(argv[++i]);
        } else if (a == "--voiced-threshold") {
            if (!needValue(a)) return false;
            opts.dsp.voicedThreshold = std::stof(argv[++i]);
        } else if (a == "--bin-count") {
            if (!needValue(a)) return false;
            opts.dsp.binCount = std::stoi(argv[++i]);
        } else if (a == "--analysis-hop") {
            if (!needValue(a)) return false;
            opts.dsp.analysisHop = std::stoi(argv[++i]);
        } else if (a == "--freq-min") {
            if (!needValue(a)) return false;
            opts.dsp.freqMinHz = std::stod(argv[++i]);
        } else if (a == "--freq-max") {
            if (!needValue(a)) return false;
            opts.dsp.freqMaxHz = std::stod(argv[++i]);
        } else if (a == "--algorithm") {
            if (!needValue(a)) return false;
            if (!parseAlgorithm(argv[++i], opts.dsp.algorithmMode)) {
                std::cerr << "Invalid algorithm. Use: loiacono|fft|goertzel\n";
                return false;
            }
        } else if (a == "--resynth") {
            if (!needValue(a)) return false;
            if (!parseResynth(argv[++i], opts.dsp.resynthMode)) {
                std::cerr << "Invalid resynth. Use: FrequencyDomain|TimeDomain\n";
                return false;
            }
        } else if (a == "--ratio-smoothing") {
            if (!needValue(a)) return false;
            opts.dsp.ratioSmoothing = std::stof(argv[++i]);
        } else if (a == "--amp-smoothing") {
            if (!needValue(a)) return false;
            opts.dsp.amplitudeSmoothing = std::stof(argv[++i]);
        } else if (a == "--phase-pull") {
            if (!needValue(a)) return false;
            opts.dsp.phasePull = std::stof(argv[++i]);
        } else if (a == "--flow-grain-ms") {
            if (!needValue(a)) return false;
            opts.dsp.flowGrainMs = std::stoi(argv[++i]);
        } else if (a == "--flow-overlap") {
            if (!needValue(a)) return false;
            opts.dsp.flowOverlap = std::stof(argv[++i]);
        } else if (a == "--flow-delay-ms") {
            if (!needValue(a)) return false;
            opts.dsp.flowBaseDelayMs = std::stoi(argv[++i]);
        } else if (a == "--flow-drift") {
            if (!needValue(a)) return false;
            opts.dsp.flowDriftCorrection = std::stof(argv[++i]);
        } else if (a == "--list-devices") {
            opts.listDevices = true;
        } else if (a == "--input-device") {
            if (!needValue(a)) return false;
            opts.inputDevice = static_cast<unsigned int>(std::stoul(argv[++i]));
            opts.hasInputDevice = true;
        } else if (a == "--output-device") {
            if (!needValue(a)) return false;
            opts.outputDevice = static_cast<unsigned int>(std::stoul(argv[++i]));
            opts.hasOutputDevice = true;
        } else if (a == "--api") {
            if (!needValue(a)) return false;
            if (!parseApi(argv[++i], opts.api)) {
                std::cerr << "Invalid api. Use: unspecified|alsa|pulse|jack\n";
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            return false;
        }
    }

    if (opts.dsp.minMidi >= opts.dsp.maxMidi) {
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

    RuntimeOptions options;
    bool showHelp = false;
    if (!parseArgs(argc, argv, options, showHelp)) {
        if (showHelp) return 0;
        return 1;
    }

    try {
        RtAudio audio(options.api);
        if (audio.getDeviceCount() < 1) {
            std::cerr << "No audio devices found.\n";
            return 1;
        }
        if (options.listDevices) {
            printDevices(audio);
            return 0;
        }

        RtAudio::StreamParameters inputParams;
        RtAudio::StreamParameters outputParams;
        inputParams.deviceId = options.hasInputDevice ? options.inputDevice : audio.getDefaultInputDevice();
        outputParams.deviceId = options.hasOutputDevice ? options.outputDevice : audio.getDefaultOutputDevice();

        if (!options.hasInputDevice && !options.hasOutputDevice && inputParams.deviceId != outputParams.deviceId) {
            const unsigned int invalid = std::numeric_limits<unsigned int>::max();
            unsigned int fallback = invalid;
            std::string fallbackName;
            for (unsigned int id : audio.getDeviceIds()) {
                auto info = audio.getDeviceInfo(id);
                if (info.inputChannels > 0 && info.outputChannels > 0) {
                    if (info.isDefaultInput || info.isDefaultOutput) {
                        fallback = id;
                        fallbackName = info.name;
                        break;
                    }
                    if (fallback == invalid) {
                        fallback = id;
                        fallbackName = info.name;
                    }
                }
            }
            if (fallback != invalid) {
                inputParams.deviceId = fallback;
                outputParams.deviceId = fallback;
                std::cout << "Using duplex device [" << fallback << "] " << fallbackName << "\n";
            }
        }

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
            options.dsp.sampleRate,
            &frames,
            &audioCallback,
            &state);

        std::cout << "JTune C++ autotune running. Press Ctrl+C to stop.\n";
        std::cout << "pitch_system=" << options.dsp.pitchSystemId
                  << " reference=" << options.dsp.referenceMidiNote << '@' << options.dsp.baseAFrequencyHz << "Hz"
                  << " octave_shift=" << options.dsp.octaveShift
                  << " strength=" << options.dsp.strength
                  << " sr=" << options.dsp.sampleRate
                  << " buffer=" << frames
                  << " bins=" << options.dsp.binCount
                  << " hop=" << options.dsp.analysisHop
                  << " resynth=" << (options.dsp.resynthMode == jtune::TimeDomain ? "TimeDomain" : "FrequencyDomain")
                  << "\n";
        if (const auto* definition = jtune::pitchSystemRegistry().byId(options.dsp.pitchSystemId))
            std::cout << "pitch_system_version=" << definition->version
                      << " source_hash=" << definition->sourceHash
                      << " correction_eligible=" << (definition->correctionEligible ? "true" : "false")
                      << " limitations=" << definition->limitations << "\n";

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
