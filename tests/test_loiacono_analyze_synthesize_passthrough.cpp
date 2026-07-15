#include "loiacono_pitch_shift.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
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

struct Metrics {
    double corr = 0.0;
    double rmse = 0.0;
    double maxAbsErr = 0.0;
};

double toneProjection(const std::vector<float>& signal,
                      unsigned int sampleRate,
                      double frequencyHz,
                      size_t offset)
{
    double real = 0.0;
    double imag = 0.0;
    for (size_t i = offset; i < signal.size(); ++i) {
        const double angle = 2.0 * kPi * frequencyHz * static_cast<double>(i) /
            static_cast<double>(sampleRate);
        real += static_cast<double>(signal[i]) * std::cos(angle);
        imag -= static_cast<double>(signal[i]) * std::sin(angle);
    }
    return std::hypot(real, imag);
}

double rms(const std::vector<float>& signal, size_t offset)
{
    if (offset >= signal.size()) return 0.0;
    double energy = 0.0;
    for (size_t i = offset; i < signal.size(); ++i) {
        const double sample = static_cast<double>(signal[i]);
        energy += sample * sample;
    }
    return std::sqrt(energy / static_cast<double>(signal.size() - offset));
}

double medianOfThree(double a, double b, double c)
{
    std::array<double, 3> values{a, b, c};
    std::sort(values.begin(), values.end());
    return values[1];
}

Metrics compareSignals(const std::vector<float>& a, const std::vector<float>& b, size_t offset)
{
    const size_t n = std::min(a.size(), b.size());
    if (offset >= n) return {};

    double dot = 0.0;
    double ea = 0.0;
    double eb = 0.0;
    double err2 = 0.0;
    double maxAbs = 0.0;

    for (size_t i = offset; i < n; ++i) {
        const double x = static_cast<double>(a[i]);
        const double y = static_cast<double>(b[i]);
        dot += x * y;
        ea += x * x;
        eb += y * y;
        const double e = x - y;
        err2 += e * e;
        maxAbs = std::max(maxAbs, std::abs(e));
    }

    const size_t compared = n - offset;
    Metrics m;
    m.corr = dot / std::sqrt(std::max(1e-12, ea * eb));
    m.rmse = std::sqrt(err2 / std::max<size_t>(1, compared));
    m.maxAbsErr = maxAbs;
    return m;
}

}  // namespace

int main()
{
    loiacono::AnalyzeConfig cfg;
    cfg.sampleRate = 48000;
    cfg.freqMinHz = 100.0;
    cfg.freqMaxHz = 3000.0;
    cfg.binCount = 192;
    cfg.multiple = 24;
    cfg.algorithmMode = 0;
    cfg.phaseEnabled = true;

    loiacono::LoiaconoAnalyze analyze(cfg);
    loiacono::LoiaconoSynthesize synthesize;
    synthesize.reset(analyze.numBins());

    const auto input = generateChirp(cfg.sampleRate, 3.0, 220.0, 880.0);
    std::vector<float> output(input.size(), 0.0f);
    std::vector<float> spectrum;
    std::vector<float> phase;

    int hopCountdown = 96;
    for (size_t i = 0; i < input.size(); ++i) {
        analyze.processSample(input[i]);
        if (--hopCountdown <= 0) {
            hopCountdown = 96;
            analyze.getSpectrum(spectrum);
            analyze.getPhase(phase);
            synthesize.shiftFromAnalysis(analyze, spectrum, phase, 1.0f, cfg.freqMinHz, cfg.freqMaxHz);
        }
        output[i] = synthesize.synthShiftedSample(analyze, 0.15f, 0.08f, 96);
    }

    const size_t settleOffset = static_cast<size_t>(cfg.sampleRate / 2);
    const auto m = compareSignals(input, output, settleOffset);
    std::cout << "[loiacono-analyze-synthesize] corr=" << m.corr
              << " rmse=" << m.rmse
              << " max_abs_err=" << m.maxAbsErr << "\n";

    if (m.corr < 0.98) {
        std::cerr << "Passthrough correlation too low: " << m.corr << "\n";
        return 1;
    }
    if (m.rmse > 0.05) {
        std::cerr << "Passthrough RMSE too high: " << m.rmse << "\n";
        return 1;
    }
    if (m.maxAbsErr > 0.30) {
        std::cerr << "Passthrough max abs error too high: " << m.maxAbsErr << "\n";
        return 1;
    }

    // Exercise the oscillator-bank path (ratio 1 is an intentional dry
    // bypass). A stable phase reference should concentrate substantially more
    // energy at the shifted frequency than at the original frequency.
    loiacono::LoiaconoAnalyze shiftedAnalyze(cfg);
    loiacono::LoiaconoSynthesize shiftedSynthesize;
    shiftedSynthesize.reset(shiftedAnalyze.numBins());
    constexpr double sourceHz = 440.0;
    constexpr float shiftRatio = 1.059463094f;
    // One second is long enough to expose synthesis work that accidentally
    // scales with the absolute sample index, but short enough for this to
    // remain a focused unit test. Equal-sized timing windows make the check
    // relative to the host CPU instead of relying on a wall-clock deadline.
    constexpr size_t timingWindowCount = 8;
    const auto tone = generateChirp(cfg.sampleRate, 1.0, sourceHz, sourceHz);
    std::vector<float> shifted(tone.size(), 0.0f);
    std::array<double, timingWindowCount> windowTimes{};
    const size_t timingWindowSamples = tone.size() / timingWindowCount;
    hopCountdown = 96;
    for (size_t i = 0; i < tone.size(); ++i) {
        const auto sampleStart = std::chrono::steady_clock::now();
        shiftedAnalyze.processSample(tone[i]);
        if (--hopCountdown <= 0) {
            hopCountdown = 96;
            shiftedAnalyze.getSpectrum(spectrum);
            shiftedAnalyze.getPhase(phase);
            shiftedSynthesize.shiftFromAnalysis(
                shiftedAnalyze, spectrum, phase, shiftRatio, cfg.freqMinHz, cfg.freqMaxHz);
        }
        shifted[i] = shiftedSynthesize.synthShiftedSample(shiftedAnalyze, 0.15f, 0.08f, 96);
        const auto sampleEnd = std::chrono::steady_clock::now();
        const size_t window = std::min(timingWindowCount - 1, i / timingWindowSamples);
        windowTimes[window] += std::chrono::duration<double>(sampleEnd - sampleStart).count();
    }
    const size_t shiftedSettleOffset = static_cast<size_t>(cfg.sampleRate / 4);
    const double targetHz = sourceHz * shiftRatio;
    const double targetEnergy = toneProjection(shifted, cfg.sampleRate, targetHz, shiftedSettleOffset);
    const double sourceEnergy = toneProjection(shifted, cfg.sampleRate, sourceHz, shiftedSettleOffset);
    const double lowerSideEnergy = toneProjection(shifted, cfg.sampleRate, targetHz - 30.0, shiftedSettleOffset);
    const double upperSideEnergy = toneProjection(shifted, cfg.sampleRate, targetHz + 30.0, shiftedSettleOffset);
    const double shiftedRms = rms(shifted, shiftedSettleOffset);
    const bool allFinite = std::all_of(shifted.begin(), shifted.end(), [](float sample) {
        return std::isfinite(sample);
    });
    const double earlyMedian = medianOfThree(windowTimes[1], windowTimes[2], windowTimes[3]);
    const double lateMedian = medianOfThree(windowTimes[5], windowTimes[6], windowTimes[7]);
    const double slowdown = lateMedian / std::max(1e-9, earlyMedian);
    std::cout << "[loiacono-shifted-tone] target/source="
              << targetEnergy / std::max(1e-12, sourceEnergy)
              << " target/lower-side=" << targetEnergy / std::max(1e-12, lowerSideEnergy)
              << " target/upper-side=" << targetEnergy / std::max(1e-12, upperSideEnergy)
              << " rms=" << shiftedRms
              << " late/early-runtime=" << slowdown << "\n";
    if (!allFinite) {
        std::cerr << "Shifted synthesis produced a non-finite sample\n";
        return 1;
    }
    if (shiftedRms < 0.005 || shiftedRms > 0.8) {
        std::cerr << "Shifted synthesis RMS is outside the useful range: " << shiftedRms << "\n";
        return 1;
    }
    if (targetEnergy < sourceEnergy * 4.0) {
        std::cerr << "Shifted synthesis does not concentrate energy at the target frequency\n";
        return 1;
    }
    if (targetEnergy < lowerSideEnergy * 4.0 || targetEnergy < upperSideEnergy * 4.0) {
        std::cerr << "Shifted synthesis has excessive off-target spectral energy\n";
        return 1;
    }
    if (slowdown > 1.8) {
        std::cerr << "Spectral synthesis cost grows with elapsed sample count: "
                  << slowdown << "x late/early\n";
        return 1;
    }

    return 0;
}
