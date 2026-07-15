#include "autotune_core.h"
#include "pitch_tracker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned int kSampleRate = 48000;
constexpr unsigned int kBufferFrames = 256;
constexpr double kDurationSeconds = 4.0;
// Glide across most of D4's capture region. A hard-tuned output should stay
// at D4 throughout instead of reproducing the movement between notes.
constexpr double kStartHz = 278.0;
constexpr double kEndHz = 291.0;

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    return values[index];
}

double median(std::vector<double> values)
{
    return percentile(std::move(values), 0.5);
}

std::vector<float> generateChirp()
{
    const size_t frames = static_cast<size_t>(kDurationSeconds * kSampleRate);
    std::vector<float> audio(frames);
    double phase = 0.0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const double alpha = static_cast<double>(frame) / static_cast<double>(frames - 1);
        const double hz = kStartHz + (kEndHz - kStartHz) * alpha;
        phase += 2.0 * kPi * hz / static_cast<double>(kSampleRate);
        // Voice-like harmonic stack instead of the unrealistically easy pure
        // sine used by the original test.
        audio[frame] = static_cast<float>(
            0.13 * std::sin(phase) +
            0.055 * std::sin(2.0 * phase + 0.2) +
            0.035 * std::sin(3.0 * phase - 0.35) +
            0.020 * std::sin(4.0 * phase + 0.5));
    }
    return audio;
}

jtune::RollingPitchTracker makeIndependentVerifier(const jtune::AutotuneOptions& opts)
{
    return jtune::RollingPitchTracker(jtune::PitchTrackerOptions{
        opts.sampleRate,
        opts.minMidi,
        opts.maxMidi,
        opts.multiple,
        96,
        96,
        0.20f,
        opts.freqMinHz,
        opts.freqMaxHz,
        opts.leakiness,
        opts.baseAFrequencyHz,
        opts.computeMode,
        opts.windowMode,
        opts.normalizationMode,
        opts.windowLengthMode,
        1 // FFT verifier; the processor under test uses Loiacono.
    });
}

} // namespace

int main()
{
    jtune::AutotuneOptions opts;
    opts.sampleRate = kSampleRate;
    // The selected pitch system supplies targets directly; no major/minor mask.
    opts.minMidi = 50;
    opts.maxMidi = 80;
    opts.multiple = 24;
    opts.voicedThreshold = 0.20f;
    opts.binCount = 192;
    opts.analysisHop = 96;
    opts.freqMinHz = 100.0;
    opts.freqMaxHz = 3000.0;
    opts.algorithmMode = 0;
    opts.resynthMode = jtune::TimeDomain;
    opts.strength = 1.0f;

    const auto input = generateChirp();
    std::vector<float> output(input.size(), 0.0f);
    std::vector<double> callbackMs;
    callbackMs.reserve((input.size() + kBufferFrames - 1) / kBufferFrames);

    jtune::ConstantQAutotuneProcessor backend(opts);
    size_t firstDecisionFrame = input.size();
    double processingMsAtFirstDecision = 0.0;
    double totalProcessingMs = 0.0;
    bool sawDetectedPitchTelemetry = false;
    bool sawTargetPitchTelemetry = false;

    // This is the same processBuffer entry point called by the RtAudio callback.
    for (size_t offset = 0; offset < input.size(); offset += kBufferFrames) {
        const auto frames = static_cast<unsigned int>(
            std::min<size_t>(kBufferFrames, input.size() - offset));
        const auto begin = Clock::now();
        backend.processBuffer(input.data() + offset, output.data() + offset, frames);
        const auto end = Clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(end - begin).count();
        callbackMs.push_back(elapsedMs);
        totalProcessingMs += elapsedMs;
        sawDetectedPitchTelemetry =
            sawDetectedPitchTelemetry || backend.currentDetectedPitchHz() > 0.0f;
        sawTargetPitchTelemetry =
            sawTargetPitchTelemetry || backend.currentTargetPitchHz() > 0.0f;

        const double correctionCents = std::abs(1200.0 * std::log2(backend.currentPitchRatio()));
        if (firstDecisionFrame == input.size() && correctionCents >= 10.0) {
            firstDecisionFrame = offset + frames;
            processingMsAtFirstDecision = totalProcessingMs;
        }
    }

    // Observe the output as an external client would. This verifier is intentionally
    // not included in callback timings and uses a different analysis algorithm.
    auto verifier = makeIndependentVerifier(opts);
    std::vector<double> settledPitchHz;
    size_t firstCorrectedOutputFrame = input.size();
    size_t correctedCandidateFrame = input.size();
    int consecutiveCorrectedEstimates = 0;
    constexpr double targetHz = 293.6647679174076; // median target is D4
    std::vector<double> settledErrorsCents;
    for (size_t frame = 0; frame < output.size(); ++frame) {
        if (!verifier.processSample(output[frame])) continue;
        const double pitchHz = verifier.pitchHz();
        if (!(pitchHz > 0.0) || !std::isfinite(pitchHz)) continue;

        const double alpha = static_cast<double>(frame) / static_cast<double>(output.size() - 1);
        const double sourceHz = kStartHz + (kEndHz - kStartHz) * alpha;
        const double sourceMidi = 69.0 + 12.0 * std::log2(sourceHz / 440.0);
        const double frameTargetHz = 440.0 * std::pow(2.0, (std::round(sourceMidi) - 69.0) / 12.0);
        const bool observablyCorrected =
            std::abs(pitchHz - frameTargetHz) + 2.0 < std::abs(sourceHz - frameTargetHz);
        if (observablyCorrected) {
            if (consecutiveCorrectedEstimates == 0) correctedCandidateFrame = frame;
            ++consecutiveCorrectedEstimates;
        } else {
            consecutiveCorrectedEstimates = 0;
            correctedCandidateFrame = input.size();
        }
        if (firstCorrectedOutputFrame == output.size() && consecutiveCorrectedEstimates >= 3) {
            firstCorrectedOutputFrame = correctedCandidateFrame;
        }
        if (frame >= output.size() / 4) {
            settledPitchHz.push_back(pitchHz);
            settledErrorsCents.push_back(std::abs(1200.0 * std::log2(pitchHz / frameTargetHz)));
        }
    }

    const double settledHz = median(settledPitchHz);
    // Skip acquisition, then evaluate the entire remaining glide point by
    // point. Its median source pitch is at 62.5% of the full sweep.
    const double sourceMedianHz = kStartHz + 0.625 * (kEndHz - kStartHz);
    const double rawErrorHz = std::abs(sourceMedianHz - targetHz);
    const double tunedErrorHz = std::abs(settledHz - targetHz);
    const double tunedErrorCents = median(settledErrorsCents);
    const double p95TunedErrorCents = percentile(settledErrorsCents, 0.95);
    const double callbackBudgetMs = 1000.0 * kBufferFrames / kSampleRate;
    const double p50CallbackMs = percentile(callbackMs, 0.50);
    const double p95CallbackMs = percentile(callbackMs, 0.95);
    const double p99CallbackMs = percentile(callbackMs, 0.99);
    const double maxCallbackMs = *std::max_element(callbackMs.begin(), callbackMs.end());
    const size_t deadlineMisses = static_cast<size_t>(std::count_if(
        callbackMs.begin(), callbackMs.end(),
        [callbackBudgetMs](double ms) { return ms > callbackBudgetMs; }));
    const double missRate = static_cast<double>(deadlineMisses) / callbackMs.size();
    const double realTimeFactor = totalProcessingMs / (kDurationSeconds * 1000.0);
    const double decisionAudioMs = 1000.0 * firstDecisionFrame / kSampleRate;
    const double correctedOutputAudioMs = 1000.0 * firstCorrectedOutputFrame / kSampleRate;
    const double finalPitchRatio = backend.currentPitchRatio();

    const bool decisionOk = firstDecisionFrame < input.size() && decisionAudioMs <= 750.0;
    const bool outputLatencyOk =
        firstCorrectedOutputFrame < output.size() && correctedOutputAudioMs <= 1500.0;
    // A moving chromatic target has a short, expected transition at each
    // nearest-pitch boundary; median lock remains strict while p95 allows it.
    const bool accuracyOk =
        settledHz > 0.0 && tunedErrorCents <= 5.0 && p95TunedErrorCents <= 60.0;
    const bool throughputOk = realTimeFactor < 1.0;
    const bool deadlineOk = p95CallbackMs <= callbackBudgetMs && missRate <= 0.05;
    const bool telemetryOk = sawDetectedPitchTelemetry && sawTargetPitchTelemetry;

    std::cout << std::fixed << std::setprecision(3)
              << "stream.sample_rate_hz=" << kSampleRate << '\n'
              << "stream.buffer_frames=" << kBufferFrames << '\n'
              << "stream.callback_budget_ms=" << callbackBudgetMs << '\n'
              << "timing.total_processing_ms=" << totalProcessingMs << '\n'
              << "timing.real_time_factor=" << realTimeFactor << '\n'
              << "timing.callback_p50_ms=" << p50CallbackMs << '\n'
              << "timing.callback_p95_ms=" << p95CallbackMs << '\n'
              << "timing.callback_p99_ms=" << p99CallbackMs << '\n'
              << "timing.callback_max_ms=" << maxCallbackMs << '\n'
              << "timing.callback_jitter_p95_minus_p50_ms=" << (p95CallbackMs - p50CallbackMs) << '\n'
              << "timing.deadline_misses=" << deadlineMisses << '\n'
              << "timing.deadline_miss_rate=" << missRate << '\n'
              << "latency.first_decision_audio_ms=" << decisionAudioMs << '\n'
              << "latency.first_decision_processing_ms=" << processingMsAtFirstDecision << '\n'
              << "latency.first_corrected_output_audio_ms=" << correctedOutputAudioMs << '\n'
              << "accuracy.source_median_hz=" << sourceMedianHz << '\n'
              << "accuracy.target_hz=" << targetHz << '\n'
              << "accuracy.final_pitch_ratio=" << finalPitchRatio << '\n'
              << "accuracy.output_median_hz=" << settledHz << '\n'
              << "accuracy.raw_error_hz=" << rawErrorHz << '\n'
              << "accuracy.tuned_error_hz=" << tunedErrorHz << '\n'
              << "accuracy.tuned_error_cents=" << tunedErrorCents << '\n'
              << "accuracy.p95_tuned_error_cents=" << p95TunedErrorCents << '\n';

    if (!decisionOk) std::cerr << "Correction decision exceeded the 750 ms audio-time limit.\n";
    if (!outputLatencyOk) std::cerr << "Corrected output exceeded the 1500 ms audio-time limit.\n";
    if (!accuracyOk) std::cerr << "Output did not remain locked within 5 median / 60 p95 cents.\n";
    if (!throughputOk) std::cerr << "Processing was slower than real time.\n";
    if (!deadlineOk) std::cerr << "Streaming callback timing exceeded its deadline allowance.\n";
    if (!telemetryOk) std::cerr << "Live detected-pitch or target-pitch telemetry was not produced.\n";

    return decisionOk && outputLatencyOk && accuracyOk && throughputOk && deadlineOk && telemetryOk ? 0 : 1;
}
