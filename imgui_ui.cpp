#include "autotune_core.h"
#include "effects.hpp"
#include "Loiacono/pitch_tracker.h"
#include "pitch_tracker.h"
#include "pitch_system.hpp"

#include "RtAudio.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <QSettings>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct DeviceEntry {
    unsigned int id = 0;
    std::string label;
    unsigned int inputChannels = 0;
    unsigned int outputChannels = 0;
    bool defaultInput = false;
    bool defaultOutput = false;
};

struct AudioState {
    explicit AudioState(const jtune::AutotuneOptions& options)
        : processor(std::make_unique<jtune::ConstantQAutotuneProcessor>(options)),
          effects(static_cast<float>(options.sampleRate))
    {
        outputPitchTracker = std::make_unique<jtune::RollingPitchTracker>(
            jtune::PitchTrackerOptions{
                options.sampleRate,
                options.minMidi,
                options.maxMidi,
                options.multiple,
                std::clamp(options.binCount / 2, 48, 160),
                std::max(options.analysisHop * 2, 192),
                0.20f,
                options.freqMinHz,
                options.freqMaxHz,
                options.leakiness,
                options.baseAFrequencyHz,
                options.computeMode,
                options.windowMode,
                options.normalizationMode,
                options.windowLengthMode,
                options.algorithmMode});
    }

    std::unique_ptr<jtune::ConstantQAutotuneProcessor> processor;
    EffectsProcessor effects;
    std::unique_ptr<jtune::RollingPitchTracker> outputPitchTracker;
    std::mutex pendingMutex;
    std::unique_ptr<jtune::ConstantQAutotuneProcessor> pendingProcessor;
    std::unique_ptr<jtune::ConstantQAutotuneProcessor> retiredProcessor;
    uint64_t pendingGeneration = 0;
    std::atomic<uint64_t> appliedGeneration{0};
    std::atomic<float> inputPeak{0.0f};
    std::atomic<float> outputPeak{0.0f};
    std::atomic<float> pitchRatio{1.0f};
    std::atomic<float> detectedPitchHz{0.0f};
    std::atomic<float> targetPitchHz{0.0f};
    std::atomic<float> outputPitchHz{0.0f};
    std::atomic<float> callbackCpu{0.0f};
    std::atomic<float> callbackMilliseconds{0.0f};
    std::atomic<float> dspLatencyMilliseconds{0.0f};
    std::atomic<bool> droneEnabled{false};
    std::atomic<float> droneFrequencyHz{195.9977f};
    std::atomic<float> droneLevel{0.12f};
    std::atomic<float> inputGain{1.0f};
    std::atomic<float> outputGain{1.0f};
    std::atomic<uint64_t> streamWarnings{0};
    std::mutex historyMutex;
    std::deque<float> inputPitchHistory;
    std::deque<float> outputPitchHistory;
    std::deque<std::vector<float>> spectrumHistory;
    size_t historyLimit = 500;
    uint64_t lastSpectrumRevision = 0;
    double callbackBudgetSeconds = 0.0;
    double dronePhase = 0.0;
    float droneEnvelope = 0.0f;
    float currentInputGain = 1.0f;
    float currentOutputGain = 1.0f;
    unsigned int sampleRate = 48000;
};

int audioCallback(void* outputBuffer,
                  void* inputBuffer,
                  unsigned int nFrames,
                  double,
                  RtAudioStreamStatus status,
                  void* userData)
{
    auto* state = static_cast<AudioState*>(userData);
    auto* output = static_cast<float*>(outputBuffer);
    const auto* input = static_cast<const float*>(inputBuffer);
    if (!state || !output) return 0;

    const auto begin = Clock::now();
    if (status != 0) state->streamWarnings.fetch_add(1, std::memory_order_relaxed);

    if (state->pendingMutex.try_lock()) {
        if (state->pendingProcessor) {
            state->processor.swap(state->pendingProcessor);
            // Hand the old processor back to the UI thread. Destroying its
            // analysis state here would make the real-time callback stall.
            state->retiredProcessor = std::move(state->pendingProcessor);
            state->appliedGeneration.store(state->pendingGeneration, std::memory_order_release);
        }
        state->pendingMutex.unlock();
    }

    float inputPeak = 0.0f;
    float outputPeak = 0.0f;
    const float targetInputGain = state->inputGain.load(std::memory_order_relaxed);
    const float targetOutputGain = state->outputGain.load(std::memory_order_relaxed);
    const float inputGainStep = (targetInputGain - state->currentInputGain)
        / static_cast<float>(std::max(1u, nFrames));
    const float outputGainStep = (targetOutputGain - state->currentOutputGain)
        / static_cast<float>(std::max(1u, nFrames));
    for (unsigned int frame = 0; frame < nFrames; ++frame) {
        state->currentInputGain += inputGainStep;
        const float in = (input ? input[frame] : 0.0f) * state->currentInputGain;
        output[frame] = state->processor->processSample(in);
        inputPeak = std::max(inputPeak, std::abs(in));
    }

    // Pitch correction must see the clean input. Time-based, modulation, and
    // nonlinear effects create extra periodicities that destabilize detection,
    // so the entire creative-effects rack deliberately follows autotune.
    state->effects.process(output, static_cast<int>(nFrames), 1);
    for (unsigned int frame = 0; frame < nFrames; ++frame) {
        const bool droneOn = state->droneEnabled.load(std::memory_order_relaxed);
        const float droneTarget = droneOn
            ? state->droneLevel.load(std::memory_order_relaxed)
            : 0.0f;
        // Roughly 10 ms exponential attack/release avoids clicks when the
        // reference drone is toggled or its pitch changes.
        const float envelopeStep = std::min(1.0f, 100.0f / static_cast<float>(state->sampleRate));
        state->droneEnvelope += (droneTarget - state->droneEnvelope) * envelopeStep;
        const double droneHz = std::clamp(
            static_cast<double>(state->droneFrequencyHz.load(std::memory_order_relaxed)),
            20.0,
            5000.0);
        output[frame] += state->droneEnvelope * static_cast<float>(std::sin(state->dronePhase));
        state->dronePhase += 2.0 * 3.14159265358979323846 * droneHz / static_cast<double>(state->sampleRate);
        if (state->dronePhase >= 2.0 * 3.14159265358979323846) {
            state->dronePhase -= 2.0 * 3.14159265358979323846;
        }
        state->currentOutputGain += outputGainStep;
        output[frame] = std::clamp(output[frame] * state->currentOutputGain, -1.0f, 1.0f);
        outputPeak = std::max(outputPeak, std::abs(output[frame]));
        // Analyze the final sample sent to the device, after the wet/dry DSP,
        // post-effect drone, gain, and limiter. This is measured output telemetry,
        // not detected input multiplied by the requested correction ratio.
        if (state->outputPitchTracker->processSample(output[frame])) {
            state->outputPitchHz.store(
                static_cast<float>(state->outputPitchTracker->pitchHz()),
                std::memory_order_relaxed);
        }
    }

    state->inputPeak.store(inputPeak, std::memory_order_relaxed);
    state->outputPeak.store(outputPeak, std::memory_order_relaxed);
    state->pitchRatio.store(state->processor->currentPitchRatio(), std::memory_order_relaxed);
    state->detectedPitchHz.store(state->processor->currentDetectedPitchHz(), std::memory_order_relaxed);
    state->targetPitchHz.store(state->processor->currentTargetPitchHz(), std::memory_order_relaxed);
    const uint64_t spectrumRevision = state->processor->spectrumRevision();
    if (spectrumRevision != state->lastSpectrumRevision && state->historyMutex.try_lock()) {
        std::vector<float> spectrum;
        state->processor->copyCurrentSpectrum(spectrum);
        state->inputPitchHistory.push_back(state->processor->currentDetectedPitchHz());
        state->outputPitchHistory.push_back(state->outputPitchHz.load(std::memory_order_relaxed));
        state->spectrumHistory.push_back(std::move(spectrum));
        while (state->inputPitchHistory.size() > state->historyLimit) state->inputPitchHistory.pop_front();
        while (state->outputPitchHistory.size() > state->historyLimit) state->outputPitchHistory.pop_front();
        while (state->spectrumHistory.size() > state->historyLimit) state->spectrumHistory.pop_front();
        state->lastSpectrumRevision = spectrumRevision;
        state->historyMutex.unlock();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    const float cpu = state->callbackBudgetSeconds > 0.0
        ? static_cast<float>(elapsed / state->callbackBudgetSeconds)
        : 0.0f;
    state->callbackCpu.store(cpu, std::memory_order_relaxed);
    state->callbackMilliseconds.store(static_cast<float>(elapsed * 1000.0), std::memory_order_relaxed);
    return 0;
}

class AudioEngine {
public:
    ~AudioEngine() { stop(); }

    bool start(const jtune::AutotuneOptions& options,
               unsigned int bufferFrames,
               unsigned int bufferCount,
               unsigned int inputDevice,
               unsigned int outputDevice,
               std::string& error)
    {
        stop();
        try {
            audio_ = std::make_unique<RtAudio>();
            state_ = std::make_unique<AudioState>(options);
            state_->callbackBudgetSeconds =
                static_cast<double>(bufferFrames) / static_cast<double>(options.sampleRate);
            state_->sampleRate = options.sampleRate;
            state_->dspLatencyMilliseconds.store(
                options.resynthMode == jtune::TimeDomain
                    ? static_cast<float>(std::max(options.flowGrainMs, options.flowBaseDelayMs))
                    : 0.0f,
                std::memory_order_relaxed);

            RtAudio::StreamParameters inputParams;
            inputParams.deviceId = inputDevice;
            inputParams.nChannels = 1;
            inputParams.firstChannel = 0;
            RtAudio::StreamParameters outputParams;
            outputParams.deviceId = outputDevice;
            outputParams.nChannels = 1;
            outputParams.firstChannel = 0;

            unsigned int actualFrames = bufferFrames;
            RtAudio::StreamOptions streamOptions;
            streamOptions.flags = RTAUDIO_MINIMIZE_LATENCY;
            streamOptions.numberOfBuffers = bufferCount;
            streamOptions.streamName = "J-Tune ImGui";
            audio_->openStream(&outputParams,
                               &inputParams,
                               RTAUDIO_FLOAT32,
                               options.sampleRate,
                               &actualFrames,
                               &audioCallback,
                               state_.get(),
                               &streamOptions);
            state_->callbackBudgetSeconds =
                static_cast<double>(actualFrames) / static_cast<double>(options.sampleRate);
            audio_->startStream();
            actualBufferFrames_ = actualFrames;
            actualBufferCount_ = streamOptions.numberOfBuffers;
            driverLatencyFrames_ = std::max<long>(0, audio_->getStreamLatency());
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "RtAudio could not open the selected duplex stream";
        }
        stop();
        return false;
    }

    void stop()
    {
        if (audio_) {
            try {
                if (audio_->isStreamRunning()) audio_->stopStream();
                if (audio_->isStreamOpen()) audio_->closeStream();
            } catch (...) {
            }
        }
        state_.reset();
        audio_.reset();
        actualBufferFrames_ = 0;
        actualBufferCount_ = 0;
        driverLatencyFrames_ = 0;
    }

    bool isRunning() const
    {
        return audio_ && audio_->isStreamRunning();
    }

    bool apply(const jtune::AutotuneOptions& options, uint64_t generation, std::string& error)
    {
        if (!state_) {
            error = "Start the audio stream before applying live DSP changes";
            return false;
        }
        try {
            auto next = std::make_unique<jtune::ConstantQAutotuneProcessor>(options);
            std::unique_ptr<jtune::ConstantQAutotuneProcessor> retired;
            {
                std::lock_guard<std::mutex> lock(state_->pendingMutex);
                retired = std::move(state_->retiredProcessor);
                state_->pendingProcessor = std::move(next);
                state_->pendingGeneration = generation;
            }
            state_->dspLatencyMilliseconds.store(
                options.resynthMode == jtune::TimeDomain
                    ? static_cast<float>(std::max(options.flowGrainMs, options.flowBaseDelayMs))
                    : 0.0f,
                std::memory_order_relaxed);
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Could not create the updated autotune processor";
        }
        return false;
    }

    void collectRetiredProcessor()
    {
        if (!state_) return;
        std::unique_ptr<jtune::ConstantQAutotuneProcessor> retired;
        if (state_->pendingMutex.try_lock()) {
            retired = std::move(state_->retiredProcessor);
            state_->pendingMutex.unlock();
        }
    }

    void setDrone(bool enabled, float frequencyHz, float level)
    {
        if (!state_) return;
        state_->droneFrequencyHz.store(frequencyHz, std::memory_order_relaxed);
        state_->droneLevel.store(std::clamp(level, 0.0f, 0.5f), std::memory_order_relaxed);
        state_->droneEnabled.store(enabled, std::memory_order_release);
    }

    AudioState* state() const { return state_.get(); }
    unsigned int actualBufferFrames() const { return actualBufferFrames_; }
    unsigned int actualBufferCount() const { return actualBufferCount_; }
    bool driverLatencyAvailable() const { return driverLatencyFrames_ > 0; }
    double driverLatencyMilliseconds(unsigned int sampleRate) const
    {
        return sampleRate > 0
            ? 1000.0 * static_cast<double>(driverLatencyFrames_) / static_cast<double>(sampleRate)
            : 0.0;
    }

private:
    std::unique_ptr<RtAudio> audio_;
    std::unique_ptr<AudioState> state_;
    unsigned int actualBufferFrames_ = 0;
    unsigned int actualBufferCount_ = 0;
    long driverLatencyFrames_ = 0;
};

std::vector<DeviceEntry> enumerateDevices(std::string& error)
{
    std::vector<DeviceEntry> devices;
    try {
        RtAudio audio;
        for (unsigned int id : audio.getDeviceIds()) {
            const auto info = audio.getDeviceInfo(id);
            DeviceEntry entry;
            entry.id = id;
            entry.inputChannels = info.inputChannels;
            entry.outputChannels = info.outputChannels;
            entry.defaultInput = info.isDefaultInput;
            entry.defaultOutput = info.isDefaultOutput;
            entry.label = "[" + std::to_string(id) + "] " + info.name;
            devices.push_back(std::move(entry));
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "Could not enumerate audio devices";
    }
    return devices;
}

int defaultDeviceIndex(const std::vector<DeviceEntry>& devices, bool input)
{
    for (size_t i = 0; i < devices.size(); ++i) {
        if ((input && devices[i].defaultInput && devices[i].inputChannels > 0) ||
            (!input && devices[i].defaultOutput && devices[i].outputChannels > 0)) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        if ((input && devices[i].inputChannels > 0) || (!input && devices[i].outputChannels > 0)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool deviceCombo(const char* label,
                 const std::vector<DeviceEntry>& devices,
                 int& selected,
                 bool input,
                 bool enabled)
{
    bool changed = false;
    if (!enabled) ImGui::BeginDisabled();
    const char* preview = selected >= 0 && selected < static_cast<int>(devices.size())
        ? devices[static_cast<size_t>(selected)].label.c_str()
        : "No compatible device";
    if (ImGui::BeginCombo(label, preview)) {
        for (size_t i = 0; i < devices.size(); ++i) {
            const bool compatible = input ? devices[i].inputChannels > 0 : devices[i].outputChannels > 0;
            if (!compatible) continue;
            const bool isSelected = selected == static_cast<int>(i);
            if (ImGui::Selectable(devices[i].label.c_str(), isSelected)) {
                selected = static_cast<int>(i);
                changed = true;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!enabled) ImGui::EndDisabled();
    return changed;
}

void drawScaleNotes(const jtune::AutotuneOptions& options)
{
    static constexpr const char* notes[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int note = 0; note < 12; ++note) {
        if (note > 0 && note % 6 != 0) ImGui::SameLine();
        const bool active = true;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.55f, 0.43f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.68f, 0.53f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.18f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.18f, 0.22f, 1.0f));
        }
        ImGui::Button(notes[note], ImVec2(48.0f, 30.0f));
        ImGui::PopStyleColor(2);
    }
}

void setupStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.WindowPadding = ImVec2(14.0f, 14.0f);
    style.ItemSpacing = ImVec2(9.0f, 8.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.06f, 0.075f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.42f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.42f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.13f, 0.55f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.75f, 0.59f, 1.0f);
}

std::string pitchLabel(float hz)
{
    if (!(hz > 0.0f) || !std::isfinite(hz)) return "--";
    static constexpr const char* names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const double midi = 69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0);
    const int note = static_cast<int>(std::lround(midi));
    const int pitchClass = ((note % 12) + 12) % 12;
    const int octave = note / 12 - 1;
    const double cents = 100.0 * (midi - static_cast<double>(note));
    char text[64];
    std::snprintf(text, sizeof(text), "%s%d  %+.0f ct  %.1f Hz", names[pitchClass], octave, cents, hz);
    return text;
}

struct PitchSpectrumHistorySnapshot {
    std::vector<float> inputPitch;
    std::vector<float> outputPitch;
    std::vector<std::vector<float>> spectra;
    size_t capacity = 500;
};

PitchSpectrumHistorySnapshot snapshotPitchSpectrumHistory(AudioState* state)
{
    PitchSpectrumHistorySnapshot snapshot;
    if (!state) return snapshot;
    std::lock_guard<std::mutex> lock(state->historyMutex);
    snapshot.inputPitch.assign(state->inputPitchHistory.begin(), state->inputPitchHistory.end());
    snapshot.outputPitch.assign(state->outputPitchHistory.begin(), state->outputPitchHistory.end());
    snapshot.spectra.assign(state->spectrumHistory.begin(), state->spectrumHistory.end());
    snapshot.capacity = std::max<size_t>(2, state->historyLimit);
    return snapshot;
}

bool drawPitchSpectrumHistory(const char* id,
                              const PitchSpectrumHistorySnapshot& history,
                              const jtune::AutotuneOptions& options,
                              float height = 180.0f,
                              bool interactive = true)
{
    ImGui::TextUnformatted("Pitch and frequency history");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(240.0f, ImGui::GetContentRegionAvail().x), height);
    const float left = origin.x + 34.0f;
    const float right = origin.x + size.x - 8.0f;
    const float top = origin.y + 8.0f;
    const float bottom = origin.y + size.y - 20.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(18, 22, 28, 255), 5.0f);

    const int minMidi = options.minMidi;
    const int maxMidi = std::max(minMidi + 1, options.maxMidi);
    auto midiToY = [&](double midi) {
        const double t = std::clamp((midi - minMidi) / static_cast<double>(maxMidi - minMidi), 0.0, 1.0);
        return bottom - static_cast<float>(t) * (bottom - top);
    };
    auto hzToY = [&](float hz) {
        return midiToY(69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0));
    };

    float peak = 0.0f;
    for (const auto& column : history.spectra) {
        for (float amplitude : column) peak = std::max(peak, amplitude);
    }
    peak = std::max(peak, 1.0e-9f);
    const size_t visible = std::min(history.spectra.size(), history.capacity);
    const size_t first = history.spectra.size() - visible;
    const size_t renderColumns = std::min<size_t>(visible, 180);
    const int renderRows = 72;
    const double logMin = std::log(std::max(1.0, options.freqMinHz));
    const double logSpan = std::log(std::max(options.freqMinHz * 1.01, options.freqMaxHz)) - logMin;
    auto colorFor = [&](float amplitude) {
        float t = std::log1p(9.0f * std::max(0.0f, amplitude) / peak) / std::log(10.0f);
        t = t < 0.05f ? 0.0f : std::pow(t, 0.6f);
        const float r = std::clamp(1.5f * t - 0.5f, 0.0f, 1.0f);
        const float g = std::clamp(1.5f - std::abs(3.0f * t - 1.5f), 0.0f, 1.0f);
        const float b = std::clamp(1.0f - 1.5f * t, 0.0f, 1.0f);
        return IM_COL32(static_cast<int>(255.0f * r), static_cast<int>(255.0f * g),
                        static_cast<int>(255.0f * b), 220);
    };
    for (size_t rendered = 0; rendered < renderColumns; ++rendered) {
        const size_t relative = renderColumns > 1 ? rendered * (visible - 1) / (renderColumns - 1) : 0;
        const auto& column = history.spectra[first + relative];
        if (column.empty()) continue;
        const double slot = static_cast<double>(history.capacity - visible + relative);
        const float x0 = left + static_cast<float>(slot / (history.capacity - 1)) * (right - left);
        const float nextSlot = static_cast<float>(visible > 1 ? visible - 1 : 1) /
            static_cast<float>(std::max<size_t>(1, renderColumns - 1));
        const float x1 = std::min(right, x0 + nextSlot * (right - left) / static_cast<float>(history.capacity - 1) + 1.0f);
        for (int row = 0; row < renderRows; ++row) {
            const double midi = maxMidi - (static_cast<double>(row) + 0.5) / renderRows * (maxMidi - minMidi);
            const double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
            if (hz < options.freqMinHz || hz > options.freqMaxHz) continue;
            const double binT = (std::log(hz) - logMin) / logSpan;
            const size_t bin = static_cast<size_t>(std::clamp(
                std::lround(binT * static_cast<double>(column.size() - 1)),
                0L, static_cast<long>(column.size() - 1)));
            const float y0 = top + static_cast<float>(row) * (bottom - top) / renderRows;
            const float y1 = top + static_cast<float>(row + 1) * (bottom - top) / renderRows + 1.0f;
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), colorFor(column[bin]));
        }
    }

    for (int i = 0; i <= 4; ++i) {
        const float y = top + static_cast<float>(i) * (bottom - top) / 4.0f;
        draw->AddLine(ImVec2(left, y), ImVec2(right, y), IM_COL32(70, 78, 92, 120));
    }
    auto drawPitch = [&](const std::vector<float>& series, ImU32 color) {
        const size_t count = std::min(series.size(), history.capacity);
        const size_t start = series.size() - count;
        bool active = false;
        ImVec2 previous;
        for (size_t i = 0; i < count; ++i) {
            const float hz = series[start + i];
            if (!(hz > 0.0f) || !std::isfinite(hz)) { active = false; continue; }
            const double slot = static_cast<double>(history.capacity - count + i);
            const ImVec2 point(left + static_cast<float>(slot / (history.capacity - 1)) * (right - left), hzToY(hz));
            if (active) draw->AddLine(previous, point, color, 2.0f);
            previous = point;
            active = true;
        }
    };
    drawPitch(history.inputPitch, IM_COL32(79, 195, 247, 255));
    drawPitch(history.outputPitch, IM_COL32(255, 167, 38, 255));
    draw->AddRect(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(90, 100, 118, 220));
    draw->AddText(ImVec2(origin.x + 3.0f, top - 2.0f), IM_COL32(210, 220, 235, 255), std::to_string(maxMidi).c_str());
    draw->AddText(ImVec2(origin.x + 3.0f, bottom - 10.0f), IM_COL32(210, 220, 235, 255), std::to_string(minMidi).c_str());
    draw->AddText(ImVec2(left, bottom + 3.0f), IM_COL32(79, 195, 247, 255), "Input");
    draw->AddText(ImVec2(left + 48.0f, bottom + 3.0f), IM_COL32(255, 167, 38, 255), "Output");
    ImGui::InvisibleButton(id, size);
    const bool clicked = interactive && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (interactive && ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open the expanded live spectrum");
    return clicked;
}

void drawTuner(float detectedHz, float outputHz, float targetHz)
{
    ImGui::TextUnformatted("Tuning position");
    const std::string inputText = pitchLabel(detectedHz);
    const std::string tunedText = pitchLabel(outputHz);
    const std::string targetText = pitchLabel(targetHz);
    ImGui::TextColored(ImVec4(0.38f, 0.70f, 1.0f, 1.0f), "Input   %s", inputText.c_str());
    ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.62f, 1.0f), "Output (measured)   %s", tunedText.c_str());
    ImGui::TextColored(ImVec4(1.0f, 0.73f, 0.25f, 1.0f), "Target  %s", targetText.c_str());

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(240.0f, ImGui::GetContentRegionAvail().x), 92.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,
                        ImVec2(origin.x + size.x, origin.y + size.y),
                        IM_COL32(20, 23, 31, 255),
                        6.0f);
    const float left = origin.x + 20.0f;
    const float right = origin.x + size.x - 20.0f;
    const float centerY = origin.y + 51.0f;
    auto centsToX = [&](double cents) {
        const double normalized = (std::clamp(cents, -100.0, 100.0) + 100.0) / 200.0;
        return left + static_cast<float>(normalized) * (right - left);
    };
    for (int cents : {-100, -50, 0, 50, 100}) {
        const float x = centsToX(cents);
        const ImU32 color = cents == 0 ? IM_COL32(255, 187, 64, 220) : IM_COL32(88, 94, 110, 180);
        draw->AddLine(ImVec2(x, origin.y + 25.0f), ImVec2(x, origin.y + 72.0f), color, cents == 0 ? 2.0f : 1.0f);
        char tick[16];
        std::snprintf(tick, sizeof(tick), "%+d", cents);
        draw->AddText(ImVec2(x - 10.0f, origin.y + 7.0f), IM_COL32(155, 160, 174, 255), tick);
    }
    draw->AddLine(ImVec2(left, centerY), ImVec2(right, centerY), IM_COL32(100, 106, 122, 255), 1.0f);

    if (detectedHz > 0.0f && targetHz > 0.0f) {
        const double inputCents = 1200.0 * std::log2(static_cast<double>(detectedHz / targetHz));
        const double tunedCents = outputHz > 0.0f
            ? 1200.0 * std::log2(static_cast<double>(outputHz / targetHz))
            : inputCents;
        const float inputX = centsToX(inputCents);
        const float tunedX = centsToX(tunedCents);
        draw->AddLine(ImVec2(inputX, centerY), ImVec2(tunedX, centerY), IM_COL32(89, 190, 151, 180), 3.0f);
        draw->AddCircleFilled(ImVec2(inputX, centerY), 7.0f, IM_COL32(92, 173, 255, 255));
        draw->AddCircleFilled(ImVec2(tunedX, centerY), 7.0f, IM_COL32(64, 230, 158, 255));
        draw->AddText(ImVec2(inputX - 10.0f, centerY + 13.0f), IM_COL32(92, 173, 255, 255), "IN");
        draw->AddText(ImVec2(tunedX - 18.0f, centerY + 13.0f), IM_COL32(64, 230, 158, 255), "OUT");
    } else {
        draw->AddText(ImVec2(left, centerY + 12.0f), IM_COL32(155, 160, 174, 255), "Waiting for a voiced pitch...");
    }
    ImGui::Dummy(size);
}

void drawLatency(float driverMs,
                 bool driverAvailable,
                 float dspMs,
                 float callbackMs,
                 float bufferPeriodMs,
                 unsigned int bufferCount)
{
    const float knownMs = (driverAvailable ? std::max(0.0f, driverMs) : 0.0f) +
        std::max(0.0f, dspMs) + std::max(0.0f, callbackMs);
    ImGui::TextUnformatted("Current latency");
    if (driverAvailable) {
        ImGui::Text("Estimated audio path: %.1f ms", knownMs);
        ImGui::TextDisabled("Driver %.1f  +  DSP %.1f  +  callback %.2f ms", driverMs, dspMs, callbackMs);
    } else {
        ImGui::Text("Known audio-path floor: %.1f ms", knownMs);
        ImGui::TextDisabled("Driver unavailable  +  DSP %.1f  +  callback %.2f ms", dspMs, callbackMs);
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(240.0f, ImGui::GetContentRegionAvail().x), 28.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(29, 33, 43, 255), 5.0f);
    const float scaleMs = std::max(10.0f, std::ceil(knownMs / 10.0f) * 10.0f);
    float x = origin.x;
    auto segment = [&](float ms, ImU32 color) {
        const float width = size.x * std::max(0.0f, ms) / scaleMs;
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(std::min(origin.x + size.x, x + width), origin.y + size.y), color, 4.0f);
        x += width;
    };
    if (driverAvailable) segment(driverMs, IM_COL32(74, 143, 231, 255));
    segment(dspMs, IM_COL32(50, 190, 135, 255));
    segment(callbackMs, IM_COL32(241, 172, 62, 255));
    ImGui::Dummy(size);
    ImGui::TextDisabled("Buffer period %.2f ms  |  driver buffers %u", bufferPeriodMs, bufferCount);
}

void drawGainMeter(const char* id, const char* label, float& gainDb, float peak)
{
    const ImVec2 meterStart = ImGui::GetCursorScreenPos();
    ImGui::ProgressBar(std::clamp(peak, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), "");
    const ImVec2 cursorAfterMeter = ImGui::GetCursorScreenPos();

    // Put the interactive gain control in the same rectangle as the meter.
    // Transparent frame colors leave the live level visible underneath it.
    ImGui::SetCursorScreenPos(meterStart);
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 18));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 28));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(255, 255, 255, 190));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(255, 255, 255, 255));
    ImGui::SetNextItemWidth(-1.0f);
    const std::string format = std::string(label) + "  %+.1f dB";
    ImGui::SliderFloat("##gain", &gainDb, -60.0f, 60.0f, format.c_str());
    ImGui::PopStyleColor(5);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(cursorAfterMeter);
}

void glfwErrorCallback(int error, const char* description)
{
    std::cerr << "GLFW error " << error << ": " << description << '\n';
}

} // namespace

int main()
{
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1180, 960, "J-Tune Live Effects", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    setupStyle();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    jtune::AutotuneOptions options;
    options.sampleRate = 48000;
    options.wetMix = 1.0f;
    options.ratioSmoothing = 0.15f;
    QSettings pitchSettings("JTune", "JTuneImGui");
    std::string importedPitchPath = pitchSettings.value("pitch_system/file").toString().toStdString();
    if (!importedPitchPath.empty()) {
        std::vector<std::string> errors;
        jtune::pitchSystemRegistry().loadFile(importedPitchPath, errors);
    }
    options.pitchSystemId = pitchSettings.value("pitch_system/id", "org.jtune.edo.12").toString().toStdString();
    options.pitchCollectionId = pitchSettings.value("pitch_collection/id", "all").toString().toStdString();
    options.tonicMidiNote = std::clamp(pitchSettings.value("pitch_collection/tonic_midi", 60).toInt(), 0, 127);
    for (const QVariant& degree : pitchSettings.value("pitch_collection/custom_degrees").toList())
        options.customEnabledDegrees.push_back(degree.toInt());
    options.referenceMidiNote = std::clamp(pitchSettings.value("pitch_system/reference_midi", 69).toInt(), 0, 127);
    options.baseAFrequencyHz = pitchSettings.value("pitch_system/reference_hz", 440.0).toDouble();
    options.octaveShift = std::clamp(pitchSettings.value("pitch_system/octave_shift", 0).toInt(), -2, 2);
    auto savePitchSettings = [&]() {
        pitchSettings.setValue("pitch_system/file", QString::fromStdString(importedPitchPath));
        pitchSettings.setValue("pitch_system/id", QString::fromStdString(options.pitchSystemId));
        pitchSettings.setValue("pitch_system/reference_midi", options.referenceMidiNote);
        pitchSettings.setValue("pitch_system/reference_hz", options.baseAFrequencyHz);
        pitchSettings.setValue("pitch_system/octave_shift", options.octaveShift);
        pitchSettings.setValue("pitch_collection/id", QString::fromStdString(options.pitchCollectionId));
        pitchSettings.setValue("pitch_collection/tonic_midi", options.tonicMidiNote);
        QVariantList customDegrees;
        for (int degree : options.customEnabledDegrees) customDegrees << degree;
        pitchSettings.setValue("pitch_collection/custom_degrees", customDegrees);
        const auto* definition = jtune::pitchSystemRegistry().byId(options.pitchSystemId);
        if (definition) {
            pitchSettings.setValue("pitch_system/version", QString::fromStdString(definition->version));
            pitchSettings.setValue("pitch_system/source_hash", QString::fromStdString(definition->sourceHash));
        }
    };
    unsigned int bufferFrames = 256;
    unsigned int bufferCount = 2;
    AudioEngine engine;
    std::string status;
    auto devices = enumerateDevices(status);
    int inputDevice = defaultDeviceIndex(devices, true);
    int outputDevice = defaultDeviceIndex(devices, false);
    bool parametersDirty = false;
    bool expandedSpectrumOpen = false;
    uint64_t requestedGeneration = 0;
    bool droneEnabled = false;
    int droneMidiNote = options.referenceMidiNote;
    std::string dronePitchSystemId;
    int droneReferenceMidi = options.referenceMidiNote;
    bool dronePitchWasExplicitlySelected = false;
    float droneLevel = 0.12f;
    float appliedDroneFrequencyHz = 0.0f;
    float inputGainDb = 0.0f;
    float outputGainDb = 0.0f;
    bool delayEnabled = false;
    float delayTimeSeconds = 0.5f;
    float delayFeedback = 0.5f;
    float delayMix = 0.3f;
    bool delayPingPong = false;
    bool ampEnabled = false;
    float ampDriveDb = 12.0f;
    float ampTone = 0.5f;
    float ampOutputDb = 0.0f;
    bool softClipEnabled = false;
    float softClipDrive = 2.0f;
    float softClipMix = 0.5f;
    bool fuzzEnabled = false;
    float fuzzDrive = 8.0f;
    float fuzzTone = 0.5f;
    float fuzzMix = 0.5f;
    bool tremoloEnabled = false;
    float tremoloRate = 4.0f;
    float tremoloDepth = 0.5f;
    bool chorusEnabled = false;
    float chorusRate = 0.8f;
    float chorusDepthMs = 6.0f;
    float chorusMix = 0.3f;
    bool phaserEnabled = false;
    float phaserRate = 0.5f;
    float phaserDepth = 0.7f;
    float phaserFeedback = 0.2f;
    float phaserMix = 0.4f;
    bool bitCrusherEnabled = false;
    float bitCrusherDepth = 8.0f;
    float bitCrusherReduction = 4.0f;
    float bitCrusherMix = 0.4f;
    bool granulatorEnabled = false;
    float granulatorGrainMs = 60.0f;
    float granulatorTexture = 0.35f;
    float granulatorMix = 0.25f;
    bool reverbEnabled = false;
    float reverbRoomSize = 0.5f;
    float reverbDamping = 0.3f;
    float reverbMix = 0.25f;
    int effectOrder = 0;
    bool effectsDirty = true;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        engine.collectRetiredProcessor();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("J-Tune",
                     nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::TextUnformatted("J-TUNE  /  LIVE AUDIO EFFECTS");
        ImGui::SameLine();
        const bool running = engine.isRunning();
        if (!running) droneEnabled = false;
        ImGui::TextColored(running ? ImVec4(0.25f, 0.90f, 0.62f, 1.0f)
                                   : ImVec4(0.65f, 0.67f, 0.72f, 1.0f),
                           running ? "     STREAMING" : "     STOPPED");
        ImGui::Separator();

        ImGui::BeginChild("audio", ImVec2(0.0f, 205.0f), true);
        ImGui::TextUnformatted("Audio stream");
        deviceCombo("Input", devices, inputDevice, true, !running);
        deviceCombo("Output", devices, outputDevice, false, !running);

        if (running) ImGui::BeginDisabled();
        int sampleRate = static_cast<int>(options.sampleRate);
        static constexpr int sampleRates[] = {44100, 48000, 96000};
        if (ImGui::BeginCombo("Sample rate", std::to_string(sampleRate).c_str())) {
            for (int rate : sampleRates) {
                const bool selected = sampleRate == rate;
                if (ImGui::Selectable(std::to_string(rate).c_str(), selected)) {
                    options.sampleRate = static_cast<unsigned int>(rate);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        int buffer = static_cast<int>(bufferFrames);
        if (ImGui::SliderInt("Buffer frames", &buffer, 64, 1024, "%d", ImGuiSliderFlags_Logarithmic)) {
            bufferFrames = static_cast<unsigned int>(buffer);
        }
        int buffers = static_cast<int>(bufferCount);
        if (ImGui::SliderInt("Driver buffers", &buffers, 2, 8)) {
            bufferCount = static_cast<unsigned int>(buffers);
        }
        if (running) ImGui::EndDisabled();

        if (!running) {
            if (ImGui::Button("Start audio", ImVec2(130.0f, 30.0f))) {
                if (inputDevice < 0 || outputDevice < 0) {
                    status = "Select compatible input and output devices";
                } else {
                    status.clear();
                    if (engine.start(options,
                                     bufferFrames,
                                     bufferCount,
                                     devices[static_cast<size_t>(inputDevice)].id,
                                     devices[static_cast<size_t>(outputDevice)].id,
                                     status)) {
                        parametersDirty = false;
                        effectsDirty = true;
                        requestedGeneration = 0;
                        droneEnabled = false;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh devices", ImVec2(130.0f, 30.0f))) {
                status.clear();
                devices = enumerateDevices(status);
                inputDevice = defaultDeviceIndex(devices, true);
                outputDevice = defaultDeviceIndex(devices, false);
            }
        } else if (ImGui::Button("Stop audio", ImVec2(130.0f, 30.0f))) {
            droneEnabled = false;
            engine.stop();
            status = "Audio stream stopped";
        }
        if (!status.empty()) {
            ImGui::SameLine();
            ImGui::TextWrapped("%s", status.c_str());
        }
        ImGui::EndChild();

        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        float ratio = 1.0f;
        float detectedPitchHz = 0.0f;
        float targetPitchHz = 0.0f;
        float outputPitchHz = 0.0f;
        float cpu = 0.0f;
        float callbackMs = 0.0f;
        float dspLatencyMs = options.resynthMode == jtune::TimeDomain
            ? static_cast<float>(std::max(options.flowGrainMs, options.flowBaseDelayMs))
            : 0.0f;
        uint64_t warnings = 0;
        uint64_t appliedGeneration = 0;
        PitchSpectrumHistorySnapshot pitchSpectrumHistory;
        if (AudioState* state = engine.state()) {
            state->inputGain.store(std::pow(10.0f, inputGainDb / 20.0f), std::memory_order_relaxed);
            state->outputGain.store(std::pow(10.0f, outputGainDb / 20.0f), std::memory_order_relaxed);
            if (effectsDirty) {
                state->effects.setDelayEnabled(delayEnabled);
                state->effects.setDelayTime(delayTimeSeconds);
                state->effects.setDelayDecay(delayFeedback);
                state->effects.setDelayMix(delayMix);
                state->effects.setDelayPingPong(delayPingPong);
                state->effects.setAmpEnabled(ampEnabled);
                state->effects.setAmpDriveDb(ampDriveDb);
                state->effects.setAmpTone(ampTone);
                state->effects.setAmpOutputDb(ampOutputDb);
                state->effects.setSoftClipEnabled(softClipEnabled);
                state->effects.setSoftClipDrive(softClipDrive);
                state->effects.setSoftClipMix(softClipMix);
                state->effects.setFuzzEnabled(fuzzEnabled);
                state->effects.setFuzzDrive(fuzzDrive);
                state->effects.setFuzzTone(fuzzTone);
                state->effects.setFuzzMix(fuzzMix);
                state->effects.setTremoloEnabled(tremoloEnabled);
                state->effects.setTremoloRate(tremoloRate);
                state->effects.setTremoloDepth(tremoloDepth);
                state->effects.setChorusEnabled(chorusEnabled);
                state->effects.setChorusRate(chorusRate);
                state->effects.setChorusDepthMs(chorusDepthMs);
                state->effects.setChorusMix(chorusMix);
                state->effects.setPhaserEnabled(phaserEnabled);
                state->effects.setPhaserRate(phaserRate);
                state->effects.setPhaserDepth(phaserDepth);
                state->effects.setPhaserFeedback(phaserFeedback);
                state->effects.setPhaserMix(phaserMix);
                state->effects.setBitCrusherEnabled(bitCrusherEnabled);
                state->effects.setBitCrusherBitDepth(bitCrusherDepth);
                state->effects.setBitCrusherSampleRateReduction(bitCrusherReduction);
                state->effects.setBitCrusherMix(bitCrusherMix);
                state->effects.setGranulatorEnabled(granulatorEnabled);
                state->effects.setGranulatorGrainSizeMs(granulatorGrainMs);
                state->effects.setGranulatorTexture(granulatorTexture);
                state->effects.setGranulatorMix(granulatorMix);
                state->effects.setReverbEnabled(reverbEnabled);
                state->effects.setReverbRoomSize(reverbRoomSize);
                state->effects.setReverbDamping(reverbDamping);
                state->effects.setReverbMix(reverbMix);
                state->effects.setEffectOrder(effectOrder == 0
                ? EffectsProcessor::EffectOrder::DriveModDelayReverb
                : EffectsProcessor::EffectOrder::DelayReverbDriveMod);
                effectsDirty = false;
            }
            inputPeak = state->inputPeak.load(std::memory_order_relaxed);
            outputPeak = state->outputPeak.load(std::memory_order_relaxed);
            ratio = state->pitchRatio.load(std::memory_order_relaxed);
            detectedPitchHz = state->detectedPitchHz.load(std::memory_order_relaxed);
            targetPitchHz = state->targetPitchHz.load(std::memory_order_relaxed);
            outputPitchHz = state->outputPitchHz.load(std::memory_order_relaxed);
            cpu = state->callbackCpu.load(std::memory_order_relaxed);
            callbackMs = state->callbackMilliseconds.load(std::memory_order_relaxed);
            dspLatencyMs = state->dspLatencyMilliseconds.load(std::memory_order_relaxed);
            warnings = state->streamWarnings.load(std::memory_order_relaxed);
            appliedGeneration = state->appliedGeneration.load(std::memory_order_acquire);
            pitchSpectrumHistory = snapshotPitchSpectrumHistory(state);
        }

        ImGui::BeginChild("controls", ImVec2(0.0f, 420.0f), true);
        ImGui::BeginTable("control_table", 2, ImGuiTableFlags_SizingStretchSame);
        ImGui::TableNextColumn();

        ImGui::TextUnformatted("Pitch correction");
        ImGui::SameLine();
        if (ImGui::SmallButton("Hard tune preset")) {
            options.strength = 1.0f;
            options.wetMix = 1.0f;
            options.ratioSmoothing = 1.0f;
            options.correctionHoldMs = 100;
            options.voicedThreshold = 0.18f;
            options.analysisHop = 96;
            parametersDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Full wet correction, immediate targets, and vocal-dropout hold");
        }
        const auto definitions = jtune::pitchSystemRegistry().definitions();
        std::vector<std::string> tuningNames;
        tuningNames.reserve(definitions.size());
        for (const auto& definition : definitions) tuningNames.push_back(definition.displayName);
        std::vector<const char*> tuningLabels;
        tuningLabels.reserve(tuningNames.size());
        for (const auto& name : tuningNames) tuningLabels.push_back(name.c_str());
        int selectedPitchSystem = 0;
        for (int i = 0; i < static_cast<int>(definitions.size()); ++i)
            if (definitions[static_cast<size_t>(i)].id == options.pitchSystemId) selectedPitchSystem = i;
        if (ImGui::Combo("Tuning System", &selectedPitchSystem,
                         tuningLabels.data(), static_cast<int>(tuningLabels.size()))) {
            if (selectedPitchSystem >= 0 && selectedPitchSystem < static_cast<int>(definitions.size()))
                options.pitchSystemId = definitions[static_cast<size_t>(selectedPitchSystem)].id;
            parametersDirty = true;
        }
        static char pitchSystemPath[512] = {};
        ImGui::InputText("Pitch-system file", pitchSystemPath, sizeof(pitchSystemPath));
        ImGui::SameLine();
        if (ImGui::Button("Import")) {
            std::vector<std::string> errors;
            if (jtune::pitchSystemRegistry().loadFile(pitchSystemPath, errors)) {
                const auto& updated = jtune::pitchSystemRegistry().definitions();
                options.pitchSystemId = updated.back().id;
                importedPitchPath = pitchSystemPath;
                savePitchSettings();
                parametersDirty = true;
                status = "Imported " + updated.back().displayName;
            } else {
                status = errors.empty() ? "Pitch-system import failed" : errors.front();
            }
        }
        if (selectedPitchSystem >= 0 && selectedPitchSystem < static_cast<int>(definitions.size())) {
            const auto& selected = definitions[static_cast<size_t>(selectedPitchSystem)];
            ImGui::TextWrapped("Use: %s", selected.appropriateUse.c_str());
            ImGui::TextWrapped("Limits: %s", selected.limitations.c_str());
            ImGui::Text("Stable ID: %s  Version: %s", selected.id.c_str(), selected.version.c_str());
            ImGui::TextWrapped("Source hash: %s", selected.sourceHash.c_str());
            ImGui::Text("Reviewed: %s", selected.reviewed ? "yes" : "no");
            ImGui::TextColored(selected.correctionEligible
                    ? ImVec4(0.25f, 0.90f, 0.62f, 1.0f)
                    : ImVec4(1.0f, 0.73f, 0.25f, 1.0f),
                "Correction: %s", selected.correctionEligible ? "enabled" : "disabled (reference only)");
            for (const auto& source : selected.sources)
                ImGui::BulletText("%s | License: %s", source.citation.c_str(), source.license.c_str());
            for (const auto& review : selected.reviews)
                ImGui::BulletText("Reviewed by %s: %s (%s)", review.reviewer.c_str(),
                                  review.scope.c_str(), review.date.c_str());
        }
        const int activeDegreeCount = selectedPitchSystem >= 0 && selectedPitchSystem < static_cast<int>(definitions.size())
            ? static_cast<int>(definitions[static_cast<size_t>(selectedPitchSystem)].targets.size()) : 0;
        std::vector<const jtune::PitchCollectionDefinition*> compatibleCollections;
        for (const auto& collection : jtune::builtInPitchCollections())
            if (collection.degreeCount == 0 || collection.degreeCount == activeDegreeCount)
                compatibleCollections.push_back(&collection);
        int selectedCollection = 0;
        for (int i = 0; i < static_cast<int>(compatibleCollections.size()); ++i)
            if (compatibleCollections[static_cast<size_t>(i)]->id == options.pitchCollectionId) selectedCollection = i;
        std::vector<const char*> collectionLabels;
        for (const auto* collection : compatibleCollections) collectionLabels.push_back(collection->displayName.c_str());
        if (ImGui::Combo("Pitch Collection", &selectedCollection, collectionLabels.data(),
                         static_cast<int>(collectionLabels.size()))) {
            options.pitchCollectionId = compatibleCollections[static_cast<size_t>(selectedCollection)]->id;
            parametersDirty = true;
            savePitchSettings();
        }
        parametersDirty |= ImGui::SliderInt("Tonic MIDI note", &options.tonicMidiNote, 0, 127);
        if (options.pitchCollectionId == "custom" && activeDegreeCount > 0 &&
            ImGui::TreeNode("Active degrees")) {
            for (int degree = 0; degree < activeDegreeCount; ++degree) {
                bool enabled = std::find(options.customEnabledDegrees.begin(), options.customEnabledDegrees.end(), degree)
                    != options.customEnabledDegrees.end();
                std::string label = "Degree " + std::to_string(degree);
                if (ImGui::Checkbox(label.c_str(), &enabled)) {
                    if (enabled) options.customEnabledDegrees.push_back(degree);
                    else options.customEnabledDegrees.erase(std::remove(options.customEnabledDegrees.begin(),
                        options.customEnabledDegrees.end(), degree), options.customEnabledDegrees.end());
                    parametersDirty = true;
                    savePitchSettings();
                }
            }
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Built-ins are mathematically exact. Gamelan, makam, and raga require a named measured or theoretical dataset.");
        parametersDirty |= ImGui::SliderInt("Reference MIDI note", &options.referenceMidiNote, 0, 127);
        parametersDirty |= ImGui::SliderInt("Octave shift", &options.octaveShift, -2, 2, "%+d oct");
        float referenceHz = static_cast<float>(options.baseAFrequencyHz);
        if (ImGui::InputFloat("Reference frequency (Hz)", &referenceHz, 0.1f, 1.0f, "%.3f")) {
            options.baseAFrequencyHz = std::clamp(static_cast<double>(referenceHz), 1.0, 20000.0);
            parametersDirty = true;
        }
        parametersDirty |= ImGui::SliderFloat("Correction strength", &options.strength, 0.0f, 1.0f, "%.2f");
        parametersDirty |= ImGui::SliderFloat("Wet mix", &options.wetMix, 0.0f, 1.0f, "%.2f");
        parametersDirty |= ImGui::SliderFloat("Pitch response", &options.ratioSmoothing, 0.01f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("1.0 is a hard lock; lower values glide between targets");
        parametersDirty |= ImGui::SliderInt("Dropout hold", &options.correctionHoldMs, 0, 250, "%d ms");
        parametersDirty |= ImGui::SliderFloat("Voiced threshold", &options.voicedThreshold, 0.01f, 1.0f, "%.2f");
        parametersDirty |= ImGui::SliderInt("Lowest MIDI note", &options.minMidi, 0, 126);
        parametersDirty |= ImGui::SliderInt("Highest MIDI note", &options.maxMidi, 1, 127);
        if (options.minMidi >= options.maxMidi) options.minMidi = options.maxMidi - 1;

        ImGui::SeparatorText("Analysis");
        static constexpr const char* algorithms[] = {"Loiacono", "FFT", "Goertzel"};
        parametersDirty |= ImGui::Combo("Algorithm", &options.algorithmMode, algorithms, 3);
        parametersDirty |= ImGui::SliderInt("Bins", &options.binCount, 32, 512);
        parametersDirty |= ImGui::SliderInt("Analysis hop", &options.analysisHop, 16, 1024);
        parametersDirty |= ImGui::SliderInt("Window multiple", &options.multiple, 2, 96);
        parametersDirty |= ImGui::InputDouble("Minimum frequency", &options.freqMinHz, 1.0, 10.0, "%.1f Hz");
        parametersDirty |= ImGui::InputDouble("Maximum frequency", &options.freqMaxHz, 1.0, 10.0, "%.1f Hz");

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Resynthesis");
        int resynthesis = options.resynthMode;
        static constexpr const char* resynthesisNames[] = {"Frequency domain", "Time domain"};
        if (ImGui::Combo("Mode", &resynthesis, resynthesisNames, 2)) {
            options.resynthMode = resynthesis;
            parametersDirty = true;
        }
        if (options.resynthMode == jtune::TimeDomain) {
            parametersDirty |= ImGui::SliderInt("Grain length", &options.flowGrainMs, 5, 80, "%d ms");
            parametersDirty |= ImGui::SliderFloat("Grain overlap", &options.flowOverlap, 0.10f, 0.95f, "%.2f");
            parametersDirty |= ImGui::SliderInt("Base delay", &options.flowBaseDelayMs, 5, 200, "%d ms");
        } else {
            parametersDirty |= ImGui::SliderFloat("Amplitude response", &options.amplitudeSmoothing, 0.01f, 1.0f, "%.2f");
            parametersDirty |= ImGui::SliderFloat("Phase pull", &options.phasePull, 0.0f, 1.0f, "%.2f");
        }

        ImGui::SeparatorText("Post-autotune effects");
        ImGui::TextDisabled("Autotune -> effects rack -> clean reference drone -> output");
        effectsDirty |= ImGui::Checkbox("Delay", &delayEnabled);
        if (!delayEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Delay time", &delayTimeSeconds, 0.01f, 2.0f, "%.2f s");
        effectsDirty |= ImGui::SliderFloat("Delay feedback", &delayFeedback, 0.0f, 0.95f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Delay mix", &delayMix, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::Checkbox("Ping-pong", &delayPingPong);
        if (!delayEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Amp model / tone filter", &ampEnabled);
        if (!ampEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Amp drive", &ampDriveDb, 0.0f, 30.0f, "%.1f dB");
        effectsDirty |= ImGui::SliderFloat("Amp tone", &ampTone, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Amp output", &ampOutputDb, -12.0f, 12.0f, "%.1f dB");
        if (!ampEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Soft clip", &softClipEnabled);
        if (!softClipEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Clip drive", &softClipDrive, 1.0f, 20.0f, "%.2f x");
        effectsDirty |= ImGui::SliderFloat("Clip mix", &softClipMix, 0.0f, 1.0f, "%.2f");
        if (!softClipEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Fuzz", &fuzzEnabled);
        if (!fuzzEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Fuzz drive", &fuzzDrive, 1.0f, 40.0f, "%.1f x");
        effectsDirty |= ImGui::SliderFloat("Fuzz tone", &fuzzTone, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Fuzz mix", &fuzzMix, 0.0f, 1.0f, "%.2f");
        if (!fuzzEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Tremolo", &tremoloEnabled);
        if (!tremoloEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Tremolo rate", &tremoloRate, 0.1f, 12.0f, "%.2f Hz");
        effectsDirty |= ImGui::SliderFloat("Tremolo depth", &tremoloDepth, 0.0f, 1.0f, "%.2f");
        if (!tremoloEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Chorus", &chorusEnabled);
        if (!chorusEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Chorus rate", &chorusRate, 0.05f, 8.0f, "%.2f Hz");
        effectsDirty |= ImGui::SliderFloat("Chorus depth", &chorusDepthMs, 0.1f, 20.0f, "%.1f ms");
        effectsDirty |= ImGui::SliderFloat("Chorus mix", &chorusMix, 0.0f, 1.0f, "%.2f");
        if (!chorusEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Phaser", &phaserEnabled);
        if (!phaserEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Phaser rate", &phaserRate, 0.05f, 8.0f, "%.2f Hz");
        effectsDirty |= ImGui::SliderFloat("Phaser depth", &phaserDepth, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Phaser feedback", &phaserFeedback, 0.0f, 0.95f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Phaser mix", &phaserMix, 0.0f, 1.0f, "%.2f");
        if (!phaserEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Bit crusher", &bitCrusherEnabled);
        if (!bitCrusherEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Crusher bits", &bitCrusherDepth, 1.0f, 16.0f, "%.0f bit");
        effectsDirty |= ImGui::SliderFloat("Crusher reduction", &bitCrusherReduction, 1.0f, 64.0f, "%.1f x");
        effectsDirty |= ImGui::SliderFloat("Crusher mix", &bitCrusherMix, 0.0f, 1.0f, "%.2f");
        if (!bitCrusherEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Granulator", &granulatorEnabled);
        if (!granulatorEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Grain size", &granulatorGrainMs, 10.0f, 250.0f, "%.1f ms");
        effectsDirty |= ImGui::SliderFloat("Grain texture", &granulatorTexture, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Granulator mix", &granulatorMix, 0.0f, 1.0f, "%.2f");
        if (!granulatorEnabled) ImGui::EndDisabled();

        effectsDirty |= ImGui::Checkbox("Reverb", &reverbEnabled);
        if (!reverbEnabled) ImGui::BeginDisabled();
        effectsDirty |= ImGui::SliderFloat("Room size", &reverbRoomSize, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Reverb damping", &reverbDamping, 0.0f, 1.0f, "%.2f");
        effectsDirty |= ImGui::SliderFloat("Reverb mix", &reverbMix, 0.0f, 1.0f, "%.2f");
        if (!reverbEnabled) ImGui::EndDisabled();

        static constexpr const char* effectOrders[] = {
            "Drive / modulation / delay / reverb",
            "Delay / reverb / drive / modulation"};
        effectsDirty |= ImGui::Combo("Effect order", &effectOrder, effectOrders, 2);

        ImGui::SeparatorText("Selected notes");
        drawScaleNotes(options);

        ImGui::SeparatorText("Reference drone");
        struct DronePitch {
            int midiNote;
            float frequencyHz;
        };
        std::vector<DronePitch> dronePitches;
        const auto* droneDefinition = jtune::pitchSystemRegistry().byId(options.pitchSystemId);
        const bool droneSystemChanged = dronePitchSystemId != options.pitchSystemId;
        const bool droneReferenceChanged = droneReferenceMidi != options.referenceMidiNote;
        if (droneSystemChanged) dronePitchWasExplicitlySelected = false;
        if (!dronePitchWasExplicitlySelected && (droneSystemChanged || droneReferenceChanged)) {
            droneMidiNote = droneDefinition && droneDefinition->mapping
                ? droneDefinition->mapping->referenceMidi
                : options.referenceMidiNote;
        }
        dronePitchSystemId = options.pitchSystemId;
        droneReferenceMidi = options.referenceMidiNote;
        if (droneDefinition != nullptr) {
            jtune::PitchContext droneContext;
            droneContext.referenceMidi = options.referenceMidiNote;
            droneContext.referenceHz = options.baseAFrequencyHz;
            droneContext.octaveShift = options.octaveShift;
            const jtune::PitchSystemEvaluator droneEvaluator(*droneDefinition);
            for (int midi = 0; midi < 128; ++midi) {
                const auto frequency = droneEvaluator.frequencyForMidi(midi, droneContext);
                if (frequency && std::isfinite(*frequency) && *frequency >= 20.0 && *frequency <= 5000.0)
                    dronePitches.push_back({midi, static_cast<float>(*frequency)});
            }
        }

        if (!dronePitches.empty() && std::none_of(dronePitches.begin(), dronePitches.end(),
                [&](const DronePitch& pitch) { return pitch.midiNote == droneMidiNote; })) {
            const auto nearest = std::min_element(dronePitches.begin(), dronePitches.end(),
                [&](const DronePitch& a, const DronePitch& b) {
                    return std::abs(a.midiNote - droneMidiNote) < std::abs(b.midiNote - droneMidiNote);
                });
            droneMidiNote = nearest->midiNote;
        }

        auto selectedDrone = std::find_if(dronePitches.begin(), dronePitches.end(),
            [&](const DronePitch& pitch) { return pitch.midiNote == droneMidiNote; });
        const float droneHz = selectedDrone == dronePitches.end() ? 0.0f : selectedDrone->frequencyHz;
        bool droneChanged = false;
        char dronePreview[80] = "No mapped pitch";
        if (selectedDrone != dronePitches.end())
            std::snprintf(dronePreview, sizeof(dronePreview),
                          "Mapped key %d  %.2f Hz", droneMidiNote, droneHz);
        if (ImGui::BeginCombo("Drone pitch", dronePreview)) {
            for (const auto& pitch : dronePitches) {
                char label[80];
                std::snprintf(label, sizeof(label), "Mapped key %d  %.2f Hz", pitch.midiNote, pitch.frequencyHz);
                const bool selected = pitch.midiNote == droneMidiNote;
                if (ImGui::Selectable(label, selected)) {
                    droneMidiNote = pitch.midiNote;
                    dronePitchWasExplicitlySelected = true;
                    droneChanged = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        droneChanged |= ImGui::SliderFloat("Drone level", &droneLevel, 0.0f, 0.30f, "%.2f");
        if (droneDefinition != nullptr)
            ImGui::TextDisabled("Supplied by %s", droneDefinition->displayName.c_str());
        const std::string droneButtonLabel = droneEnabled
            ? "Stop drone"
            : "Start reference drone";
        if (dronePitches.empty()) ImGui::BeginDisabled();
        if (ImGui::Button(droneButtonLabel.c_str(), ImVec2(150.0f, 30.0f))) {
            if (droneEnabled) {
                droneEnabled = false;
                engine.setDrone(false, droneHz, droneLevel);
            } else {
                bool canStartDrone = engine.isRunning();
                if (!canStartDrone) {
                    if (inputDevice < 0 || outputDevice < 0) {
                        status = "Select compatible input and output devices";
                    } else {
                        status.clear();
                        canStartDrone = engine.start(
                            options,
                            bufferFrames,
                            bufferCount,
                            devices[static_cast<size_t>(inputDevice)].id,
                            devices[static_cast<size_t>(outputDevice)].id,
                            status);
                        if (canStartDrone) {
                            parametersDirty = false;
                            requestedGeneration = 0;
                            effectsDirty = true;
                        }
                    }
                }
                if (canStartDrone) {
                    droneEnabled = true;
                    engine.setDrone(true, droneHz, droneLevel);
                }
            }
        }
        if (dronePitches.empty()) ImGui::EndDisabled();
        if (dronePitches.empty() && droneEnabled) {
            droneEnabled = false;
            engine.setDrone(false, 0.0f, droneLevel);
        }
        const bool tuningChangedDroneFrequency = droneHz > 0.0f &&
            std::abs(droneHz - appliedDroneFrequencyHz) > 0.0001f;
        if ((droneChanged || tuningChangedDroneFrequency) && droneEnabled)
            engine.setDrone(true, droneHz, droneLevel);
        if (droneHz > 0.0f) appliedDroneFrequencyHz = droneHz;

        ImGui::EndTable();
        ImGui::EndChild();

        ImGui::BeginChild("dashboard", ImVec2(0.0f, -60.0f), true);
        ImGui::BeginTable("dashboard_table", 2, ImGuiTableFlags_SizingStretchSame);
        ImGui::TableNextColumn();
        if (drawPitchSpectrumHistory("##pitch_spectrum", pitchSpectrumHistory, options)) {
            expandedSpectrumOpen = true;
        }
        drawTuner(detectedPitchHz, outputPitchHz, targetPitchHz);
        ImGui::TableNextColumn();
        const unsigned int activeFrames = running ? engine.actualBufferFrames() : bufferFrames;
        const unsigned int activeBuffers = running ? engine.actualBufferCount() : bufferCount;
        const float bufferPeriodMs = options.sampleRate > 0
            ? 1000.0f * static_cast<float>(activeFrames) / static_cast<float>(options.sampleRate)
            : 0.0f;
        const float driverLatencyMs = running
            ? static_cast<float>(engine.driverLatencyMilliseconds(options.sampleRate))
            : 0.0f;
        drawLatency(driverLatencyMs,
                    running && engine.driverLatencyAvailable(),
                    dspLatencyMs,
                    callbackMs,
                    bufferPeriodMs,
                    activeBuffers);
        ImGui::SeparatorText("Volume");
        drawGainMeter("input_volume", "Input", inputGainDb, inputPeak);
        drawGainMeter("output_volume", "Output", outputGainDb, outputPeak);
        ImGui::ProgressBar(std::clamp(cpu, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), "Callback CPU");
        ImGui::Text("Stream warnings: %llu", static_cast<unsigned long long>(warnings));
        if (requestedGeneration > 0 && appliedGeneration < requestedGeneration) {
            ImGui::TextDisabled("DSP update queued for the next audio buffer");
        }
        ImGui::EndTable();
        ImGui::EndChild();

        if (parametersDirty) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Parameters changed");
        } else {
            ImGui::TextDisabled("Parameters are current");
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f);
        if (!running) ImGui::BeginDisabled();
        if (ImGui::Button("Apply DSP", ImVec2(150.0f, 34.0f))) {
            status.clear();
            const uint64_t nextGeneration = requestedGeneration + 1;
            if (engine.apply(options, nextGeneration, status)) {
                requestedGeneration = nextGeneration;
                parametersDirty = false;
                savePitchSettings();
            }
        }
        if (!running) ImGui::EndDisabled();

        ImGui::End();

        if (expandedSpectrumOpen) {
            ImGui::SetNextWindowSize(ImVec2(1100.0f, 700.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Expanded pitch spectrum", &expandedSpectrumOpen,
                             ImGuiWindowFlags_NoCollapse)) {
                ImGui::SetWindowFontScale(1.35f);
                ImGui::TextColored(ImVec4(0.31f, 0.76f, 0.97f, 1.0f),
                                   "INPUT  %.1f Hz", detectedPitchHz);
                ImGui::SameLine(260.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
                                   "OUTPUT — MEASURED  %.1f Hz", outputPitchHz);
                ImGui::SetWindowFontScale(1.0f);
                const float expandedHeight = std::max(360.0f, ImGui::GetContentRegionAvail().y - 8.0f);
                drawPitchSpectrumHistory("##expanded_pitch_spectrum",
                                         pitchSpectrumHistory,
                                         options,
                                         expandedHeight,
                                         false);
            }
            ImGui::End();
        }
        ImGui::Render();

        int displayWidth = 0;
        int displayHeight = 0;
        glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.035f, 0.04f, 0.052f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    engine.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
