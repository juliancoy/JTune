#include "loiacono_pitch_shift.h"

#include <algorithm>
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
        output[i] = synthesize.synthShiftedSample(analyze, 0.15f, 0.08f);
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

    return 0;
}
