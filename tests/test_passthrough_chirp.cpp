#include "autotune_core.h"

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

Metrics compareSignals(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double dot = 0.0;
    double ea = 0.0;
    double eb = 0.0;
    double err2 = 0.0;
    double maxAbs = 0.0;

    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(a[i]);
        const double y = static_cast<double>(b[i]);
        dot += x * y;
        ea += x * x;
        eb += y * y;
        const double e = x - y;
        err2 += e * e;
        maxAbs = std::max(maxAbs, std::abs(e));
    }

    Metrics m;
    m.corr = dot / std::sqrt(std::max(1e-12, ea * eb));
    m.rmse = std::sqrt(err2 / std::max<size_t>(1, n));
    m.maxAbsErr = maxAbs;
    return m;
}

}  // namespace

int main()
{
    const auto chirp = generateChirp(48000, 3.0, 220.0, 880.0);

    struct AlgoCase {
        const char* name;
        int mode;
    };
    const AlgoCase algoCases[] = {
        {"loiacono", 0},
        {"fft", 1},
        {"goertzel", 2},
    };

    for (const auto& c : algoCases) {
        jtune::AutotuneOptions opts;
        opts.sampleRate = 48000;
        opts.minMidi = 50;
        opts.maxMidi = 80;
        opts.multiple = 24;
        opts.voicedThreshold = 0.2f;
        opts.binCount = 192;
        opts.analysisHop = 96;
        opts.algorithmMode = c.mode;
        opts.strength = 0.0f;  // explicit passthrough mode

        jtune::ConstantQAutotuneProcessor proc(opts);
        std::vector<float> out(chirp.size(), 0.0f);
        for (size_t i = 0; i < chirp.size(); ++i) {
            out[i] = proc.processSample(chirp[i]);
        }

        const auto m = compareSignals(chirp, out);
        std::cout << "[" << c.name << "] corr=" << m.corr
                  << " rmse=" << m.rmse
                  << " max_abs_err=" << m.maxAbsErr << "\n";

        if (m.corr < 0.99999) {
            std::cerr << "Passthrough correlation too low for " << c.name << ": " << m.corr << "\n";
            return 1;
        }
        if (m.rmse > 1e-5) {
            std::cerr << "Passthrough RMSE too high for " << c.name << ": " << m.rmse << "\n";
            return 1;
        }
        if (m.maxAbsErr > 1e-4) {
            std::cerr << "Passthrough max abs error too high for " << c.name << ": " << m.maxAbsErr << "\n";
            return 1;
        }
    }

    return 0;
}
