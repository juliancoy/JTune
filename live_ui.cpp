#include "autotune_core.h"
#include "pitch_system.hpp"
#include "pitch_tracker.h"

#include "RtAudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

double hzToMidi(double hz)
{
    if (hz <= 0.0) return 0.0;
    return 69.0 + 12.0 * std::log2(hz / 440.0);
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
    unsigned int bufferFrames = 512;
    int historySize = 240;
    bool listDevices = false;
    bool hasInputDevice = false;
    bool hasOutputDevice = false;
    unsigned int inputDevice = 0;
    unsigned int outputDevice = 0;
    RtAudio::Api api = RtAudio::UNSPECIFIED;
};

struct UiState {
    std::mutex mutex;
    std::deque<float> inputHz;
    std::deque<float> outputHz;
    int maxHistory = 240;
    float latestInHz = 0.0f;
    float latestOutHz = 0.0f;
    float latestRatio = 1.0f;
};

struct AppState {
    AppState(const RuntimeOptions& opts, UiState* ui)
        : options(opts),
          processor(opts.dsp),
          inTracker({opts.dsp.sampleRate,
                     opts.dsp.minMidi,
                     opts.dsp.maxMidi,
                     opts.dsp.multiple,
                     std::clamp(opts.dsp.binCount / 2, 48, 160),
                     std::max(opts.dsp.analysisHop * 2, 192),
                     0.20f,
                     opts.dsp.freqMinHz,
                     opts.dsp.freqMaxHz,
                     opts.dsp.leakiness,
                     opts.dsp.baseAFrequencyHz,
                     opts.dsp.computeMode,
                     opts.dsp.windowMode,
                     opts.dsp.normalizationMode,
                     opts.dsp.windowLengthMode,
                     opts.dsp.algorithmMode}),
          outTracker({opts.dsp.sampleRate,
                      opts.dsp.minMidi,
                      opts.dsp.maxMidi,
                      opts.dsp.multiple,
                      std::clamp(opts.dsp.binCount / 2, 48, 160),
                      std::max(opts.dsp.analysisHop * 2, 192),
                      0.20f,
                      opts.dsp.freqMinHz,
                      opts.dsp.freqMaxHz,
                      opts.dsp.leakiness,
                      opts.dsp.baseAFrequencyHz,
                      opts.dsp.computeMode,
                      opts.dsp.windowMode,
                      opts.dsp.normalizationMode,
                      opts.dsp.windowLengthMode,
                      opts.dsp.algorithmMode}),
          passthrough(opts.dsp.strength <= 1e-6f),
          uiState(ui)
    {
    }

    RuntimeOptions options;
    jtune::ConstantQAutotuneProcessor processor;
    jtune::RollingPitchTracker inTracker;
    jtune::RollingPitchTracker outTracker;
    bool passthrough = false;
    int trackerCounter = 0;
    int trackerDecimation = 4;
    UiState* uiState = nullptr;
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

    for (unsigned int i = 0; i < nFrames; ++i) {
        const float sampleIn = in ? in[i] : 0.0f;
        const float sampleOut = state->processor.processSample(sampleIn);
        out[i] = sampleOut;

        bool inUpdated = false;
        bool outUpdated = false;
        state->trackerCounter++;
        if (state->trackerCounter >= state->trackerDecimation) {
            state->trackerCounter = 0;
            inUpdated = state->inTracker.processSample(sampleIn);
            if (state->passthrough) {
                outUpdated = inUpdated;
            } else {
                outUpdated = state->outTracker.processSample(sampleOut);
            }
        }
        if (inUpdated || outUpdated) {
            std::lock_guard<std::mutex> lock(state->uiState->mutex);
            state->uiState->latestInHz = static_cast<float>(state->inTracker.pitchHz());
            state->uiState->latestOutHz = state->passthrough
                ? state->uiState->latestInHz
                : static_cast<float>(state->outTracker.pitchHz());
            state->uiState->latestRatio = state->processor.currentPitchRatio();
            state->uiState->inputHz.push_back(state->uiState->latestInHz);
            state->uiState->outputHz.push_back(state->uiState->latestOutHz);
            while (static_cast<int>(state->uiState->inputHz.size()) > state->uiState->maxHistory) {
                state->uiState->inputHz.pop_front();
            }
            while (static_cast<int>(state->uiState->outputHz.size()) > state->uiState->maxHistory) {
                state->uiState->outputHz.pop_front();
            }
        }
    }

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
              << " [--history-size 240] [--list-devices] [--input-device ID] [--output-device ID]"
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
            if (!needValue(a)) return false; opts.dsp.pitchSystemId = argv[++i];
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
            if (!needValue(a)) return false; const auto values = jtune::parsePitchDegreeList(argv[++i]);
            if (!values) return false; opts.dsp.pitchCollectionId = "custom"; opts.dsp.customEnabledDegrees = *values;
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
        } else if (a == "--history-size") {
            if (!needValue(a)) return false;
            opts.historySize = std::clamp(std::stoi(argv[++i]), 60, 2000);
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

class TerminalRawMode {
public:
    TerminalRawMode()
    {
        if (!isatty(STDIN_FILENO)) return;
        active_ = true;
        tcgetattr(STDIN_FILENO, &old_);
        termios raw = old_;
        raw.c_lflag &= static_cast<unsigned int>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        oldFlags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, oldFlags_ | O_NONBLOCK);
    }

    ~TerminalRawMode()
    {
        if (!active_) return;
        tcsetattr(STDIN_FILENO, TCSANOW, &old_);
        fcntl(STDIN_FILENO, F_SETFL, oldFlags_);
        std::cout << "\x1b[0m\x1b[?25h\n";
    }

private:
    bool active_ = false;
    termios old_{};
    int oldFlags_ = 0;
};

void renderGraph(const RuntimeOptions& opts, UiState& ui)
{
    winsize ws{};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    const int totalCols = std::max(80, static_cast<int>(ws.ws_col));
    const int totalRows = std::max(24, static_cast<int>(ws.ws_row));

    const int graphHeight = std::max(10, totalRows - 10);
    const int graphWidth = std::max(40, totalCols - 8);

    std::vector<float> in;
    std::vector<float> out;
    float latestIn = 0.0f;
    float latestOut = 0.0f;
    float ratio = 1.0f;
    {
        std::lock_guard<std::mutex> lock(ui.mutex);
        in.assign(ui.inputHz.begin(), ui.inputHz.end());
        out.assign(ui.outputHz.begin(), ui.outputHz.end());
        latestIn = ui.latestInHz;
        latestOut = ui.latestOutHz;
        ratio = ui.latestRatio;
    }

    const double midiMin = static_cast<double>(opts.dsp.minMidi);
    const double midiMax = static_cast<double>(opts.dsp.maxMidi);
    const double midiSpan = std::max(1.0, midiMax - midiMin);

    std::vector<std::string> canvas(static_cast<size_t>(graphHeight), std::string(static_cast<size_t>(graphWidth), ' '));

    auto drawSeries = [&](const std::vector<float>& series, char ch) {
        if (series.empty()) return;
        const size_t n = series.size();
        for (int x = 0; x < graphWidth; ++x) {
            const size_t idx = (n <= static_cast<size_t>(graphWidth))
                ? static_cast<size_t>(x) * n / static_cast<size_t>(graphWidth)
                : (n - static_cast<size_t>(graphWidth) + static_cast<size_t>(x));
            if (idx >= n) continue;
            const float hz = series[idx];
            if (hz <= 0.0f) continue;
            const double midi = hzToMidi(hz);
            const double t = std::clamp((midi - midiMin) / midiSpan, 0.0, 1.0);
            const int y = graphHeight - 1 - static_cast<int>(std::lround(t * static_cast<double>(graphHeight - 1)));
            char& cell = canvas[static_cast<size_t>(y)][static_cast<size_t>(x)];
            if (cell != ' ' && cell != ch) cell = '*';
            else cell = ch;
        }
    };

    drawSeries(in, 'i');
    drawSeries(out, 'o');

    std::cout << "\x1b[2J\x1b[H\x1b[?25l";
    std::cout << "JTune Live UI  (press q to quit)\n";
    std::cout << "input=i output=o overlap=*\n";
    std::cout << "in=" << latestIn << "Hz  out=" << latestOut << "Hz  ratio=" << ratio
              << "  pitch_system=" << opts.dsp.pitchSystemId
              << "  reference=" << opts.dsp.referenceMidiNote << '@' << opts.dsp.baseAFrequencyHz << "Hz"
              << "  octave_shift=" << opts.dsp.octaveShift
              << "  strength=" << opts.dsp.strength << "\n";
    if (const auto* definition = jtune::pitchSystemRegistry().byId(opts.dsp.pitchSystemId))
        std::cout << "model=" << definition->version << " hash=" << definition->sourceHash
                  << " correction=" << (definition->correctionEligible ? "enabled" : "reference-only") << "\n";

    for (int y = 0; y < graphHeight; ++y) {
        const double t = 1.0 - static_cast<double>(y) / static_cast<double>(graphHeight - 1);
        const double midi = midiMin + t * midiSpan;
        const int midiLabel = static_cast<int>(std::lround(midi));
        if (y == 0 || y == graphHeight / 2 || y == graphHeight - 1) {
            std::cout << (midiLabel < 100 ? " " : "") << midiLabel << " |" << canvas[static_cast<size_t>(y)] << "|\n";
        } else {
            std::cout << "    |" << canvas[static_cast<size_t>(y)] << "|\n";
        }
    }

    std::cout << "    +" << std::string(static_cast<size_t>(graphWidth), '-') << "+\n";
    std::cout.flush();
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

    UiState ui;
    ui.maxHistory = options.historySize;

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
            for (unsigned int id : audio.getDeviceIds()) {
                auto info = audio.getDeviceInfo(id);
                if (info.inputChannels > 0 && info.outputChannels > 0) {
                    fallback = id;
                    break;
                }
            }
            if (fallback != invalid) {
                inputParams.deviceId = fallback;
                outputParams.deviceId = fallback;
            }
        }

        inputParams.nChannels = 1;
        outputParams.nChannels = 1;
        inputParams.firstChannel = 0;
        outputParams.firstChannel = 0;

        AppState state(options, &ui);
        unsigned int frames = options.bufferFrames;

        audio.openStream(
            &outputParams,
            &inputParams,
            RTAUDIO_FLOAT32,
            options.dsp.sampleRate,
            &frames,
            &audioCallback,
            &state);

        audio.startStream();

        TerminalRawMode rawMode;

        while (g_running.load()) {
            char c = 0;
            const ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n > 0 && (c == 'q' || c == 'Q')) {
                g_running.store(false);
                break;
            }

            renderGraph(options, ui);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (audio.isStreamRunning()) audio.stopStream();
        if (audio.isStreamOpen()) audio.closeStream();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown error while running live UI stream.\n";
        return 1;
    }

    return 0;
}
