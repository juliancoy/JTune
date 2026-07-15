#include "autotune_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned int kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDurationSeconds = 1.5;

std::vector<float> generateInput()
{
    const size_t frames = static_cast<size_t>(kSampleRate * kDurationSeconds);
    std::vector<float> input(frames);
    double phase = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double alpha = static_cast<double>(i) / static_cast<double>(frames - 1);
        const double hz = 280.0 + 8.0 * alpha;
        phase += 2.0 * kPi * hz / static_cast<double>(kSampleRate);
        input[i] = static_cast<float>(
            0.16 * std::sin(phase) +
            0.06 * std::sin(2.0 * phase + 0.2) +
            0.03 * std::sin(3.0 * phase - 0.3));
    }
    return input;
}

jtune::AutotuneOptions makeOptions(int mode)
{
    jtune::AutotuneOptions opts;
    opts.sampleRate = kSampleRate;
    opts.strength = 1.0f;
    opts.wetMix = 1.0f;
    opts.minMidi = 50;
    opts.maxMidi = 80;
    opts.multiple = 24;
    opts.binCount = 192;
    opts.analysisHop = 96;
    opts.voicedThreshold = 0.20f;
    opts.freqMinHz = 100.0;
    opts.freqMaxHz = 3000.0;
    opts.algorithmMode = 0;
    opts.resynthMode = mode;
    return opts;
}

struct RunResult {
    double elapsedSeconds = 0.0;
    double outputRms = 0.0;
    float finalRatio = 1.0f;
    float detectedHz = 0.0f;
    float targetHz = 0.0f;
    bool finite = true;
};

RunResult runMode(const std::vector<float>& input, int mode)
{
    jtune::ConstantQAutotuneProcessor processor(makeOptions(mode));
    std::vector<float> output(input.size());

    const auto start = Clock::now();
    processor.processBuffer(input.data(), output.data(), static_cast<unsigned int>(input.size()));
    const auto end = Clock::now();

    RunResult result;
    result.elapsedSeconds = std::chrono::duration<double>(end - start).count();
    result.finalRatio = processor.currentPitchRatio();
    result.detectedHz = processor.currentDetectedPitchHz();
    result.targetHz = processor.currentTargetPitchHz();

    double energy = 0.0;
    const size_t settle = output.size() / 2;
    for (size_t i = settle; i < output.size(); ++i) {
        result.finite = result.finite && std::isfinite(output[i]);
        const double sample = static_cast<double>(output[i]);
        energy += sample * sample;
    }
    result.outputRms = std::sqrt(energy / static_cast<double>(output.size() - settle));
    return result;
}

double median(std::array<double, 3> values)
{
    std::sort(values.begin(), values.end());
    return values[1];
}

}  // namespace

int main()
{
    const auto input = generateInput();
    std::array<double, 3> frequencyTimes{};
    std::array<double, 3> timeTimes{};
    RunResult frequency;
    RunResult time;

    // Alternate order so cache warmth and transient system load do not
    // consistently favor either implementation. Use medians to reject a
    // single scheduling outlier.
    for (size_t trial = 0; trial < frequencyTimes.size(); ++trial) {
        if (trial % 2 == 0) {
            frequency = runMode(input, jtune::FrequencyDomain);
            time = runMode(input, jtune::TimeDomain);
        } else {
            time = runMode(input, jtune::TimeDomain);
            frequency = runMode(input, jtune::FrequencyDomain);
        }
        frequencyTimes[trial] = frequency.elapsedSeconds;
        timeTimes[trial] = time.elapsedSeconds;
    }

    const double frequencyMedian = median(frequencyTimes);
    const double timeMedian = median(timeTimes);
    const double slower = std::max(frequencyMedian, timeMedian);
    const double faster = std::max(1e-9, std::min(frequencyMedian, timeMedian));
    const double parityRatio = slower / faster;
    const double levelParityRatio =
        std::max(frequency.outputRms, time.outputRms) /
        std::max(1e-9, std::min(frequency.outputRms, time.outputRms));
    const double frequencyRtf = frequencyMedian / kDurationSeconds;
    const double timeRtf = timeMedian / kDurationSeconds;

    std::cout << std::fixed << std::setprecision(4)
              << "resynthesis.frequency_seconds=" << frequencyMedian << '\n'
              << "resynthesis.time_seconds=" << timeMedian << '\n'
              << "resynthesis.slower_to_faster_ratio=" << parityRatio << '\n'
              << "resynthesis.frequency_rtf=" << frequencyRtf << '\n'
              << "resynthesis.time_rtf=" << timeRtf << '\n'
              << "resynthesis.frequency_rms=" << frequency.outputRms << '\n'
              << "resynthesis.time_rms=" << time.outputRms << '\n'
              << "resynthesis.rms_ratio=" << levelParityRatio << '\n';

    bool ok = true;
    if (!frequency.finite || !time.finite ||
        frequency.outputRms < 0.001 || time.outputRms < 0.001) {
        std::cerr << "A resynthesis mode produced invalid or silent output.\n";
        ok = false;
    }
    if (levelParityRatio > 2.0) {
        std::cerr << "Resynthesis output levels lack rough parity: "
                  << levelParityRatio << "x difference.\n";
        ok = false;
    }
    if (std::abs(frequency.finalRatio - 1.0f) < 0.001f ||
        std::abs(time.finalRatio - 1.0f) < 0.001f) {
        std::cerr << "The parity fixture did not exercise pitch correction.\n";
        ok = false;
    }
    if (std::abs(frequency.finalRatio - time.finalRatio) > 1e-5f ||
        std::abs(frequency.detectedHz - time.detectedHz) > 1e-3f ||
        std::abs(frequency.targetHz - time.targetHz) > 1e-3f) {
        std::cerr << "Resynthesis modes did not receive equivalent correction decisions.\n";
        ok = false;
    }
    if (frequencyRtf >= 1.0 || timeRtf >= 1.0) {
        std::cerr << "A resynthesis mode is slower than real time.\n";
        ok = false;
    }
    // These are intentionally different algorithms. "Rough parity" means
    // neither may consume over 6x the CPU time of the other on identical work.
    if (parityRatio > 6.0) {
        std::cerr << "Resynthesis modes lack rough performance parity: "
                  << parityRatio << "x difference.\n";
        ok = false;
    }

    return ok ? 0 : 1;
}
