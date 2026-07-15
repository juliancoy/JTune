#include "autotune_core.h"
#include "pitch_tracker.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double hzToMidi(double hz)
{
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

double midiToHz(double midi)
{
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

bool inScaleForKey(int midiNote, int keyRoot, bool minor)
{
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int minorScale[7] = {0, 2, 3, 5, 7, 8, 10};
    const int pc = ((midiNote % 12) + 12) % 12;
    const int* intervals = minor ? minorScale : major;
    for (int i = 0; i < 7; ++i) {
        if (((intervals[i] + keyRoot) % 12) == pc) return true;
    }
    return false;
}

int nearestScaleMidiForKey(double detectedMidi, int keyRoot, bool minor)
{
    const int center = static_cast<int>(std::lround(detectedMidi));
    int best = center;
    double bestDist = 1e9;
    for (int n = center - 24; n <= center + 24; ++n) {
        if (!inScaleForKey(n, keyRoot, minor)) continue;
        const double d = std::abs(static_cast<double>(n) - detectedMidi);
        if (d < bestDist) {
            bestDist = d;
            best = n;
        }
    }
    return best;
}

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

struct RunResult {
    std::vector<float> output;
    float finalRatio = 1.0f;
};

RunResult runAutotune(const std::vector<float>& input, const jtune::AutotuneOptions& opts)
{
    jtune::ConstantQAutotuneProcessor proc(opts);
    RunResult result;
    result.output.resize(input.size(), 0.0f);
    for (size_t i = 0; i < input.size(); ++i) {
        result.output[i] = proc.processSample(input[i]);
    }
    result.finalRatio = proc.currentPitchRatio();
    return result;
}

double estimateMedianPitchHzForAlgorithm(const std::vector<float>& signal,
                                         const jtune::AutotuneOptions& opts,
                                         int algorithmMode)
{
    const int trackerBins = std::clamp(opts.binCount / 2, 48, 200);
    const int trackerHop = std::max(opts.analysisHop, 96);
    jtune::RollingPitchTracker tracker(
        jtune::PitchTrackerOptions{
            opts.sampleRate,
            opts.minMidi,
            opts.maxMidi,
            opts.multiple,
            trackerBins,
            trackerHop,
            0.20f,
            opts.freqMinHz,
            opts.freqMaxHz,
            opts.leakiness,
            opts.baseAFrequencyHz,
            opts.computeMode,
            opts.windowMode,
            opts.normalizationMode,
            opts.windowLengthMode,
            algorithmMode});

    std::vector<double> hz;
    hz.reserve(signal.size() / 256);
    const size_t startCollect = signal.size() / 2;
    for (size_t i = 0; i < signal.size(); ++i) {
        if (tracker.processSample(signal[i]) && i >= startCollect) {
            const double pitch = tracker.pitchHz();
            if (pitch > 0.0) hz.push_back(pitch);
        }
    }
    if (hz.empty()) return 0.0;
    const auto mid = hz.begin() + static_cast<std::ptrdiff_t>(hz.size() / 2);
    std::nth_element(hz.begin(), mid, hz.end());
    return *mid;
}

double estimateMedianPitchHzCrossValidated(const std::vector<float>& signal,
                                           const jtune::AutotuneOptions& opts,
                                           int dutAlgorithm)
{
    std::vector<double> est;
    for (int a : {0, 1, 2}) {
        if (a == dutAlgorithm) continue;
        const double hz = estimateMedianPitchHzForAlgorithm(signal, opts, a);
        if (hz > 0.0 && std::isfinite(hz)) est.push_back(hz);
    }
    if (est.empty()) return estimateMedianPitchHzForAlgorithm(signal, opts, dutAlgorithm);
    if (est.size() == 1) return est[0];
    std::sort(est.begin(), est.end());
    return 0.5 * (est[0] + est[1]);
}

}  // namespace

int main()
{
    jtune::AutotuneOptions opts;
    opts.sampleRate = 48000;
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

    // C#4-ish chirp is corrected to the nearest 12-EDO target.
    const double f0 = 275.0;
    const double f1 = 282.0;
    const auto chirp = generateChirp(opts.sampleRate, 4.0, f0, f1);

    auto rawOpts = opts;
    rawOpts.strength = 0.0f;
    const auto raw = runAutotune(chirp, rawOpts);

    auto tunedOpts = opts;
    tunedOpts.strength = 1.0f;
    const auto tuned = runAutotune(chirp, tunedOpts);

    const double rawHz = estimateMedianPitchHzCrossValidated(raw.output, tunedOpts, tunedOpts.algorithmMode);
    const double tunedHz = estimateMedianPitchHzCrossValidated(tuned.output, tunedOpts, tunedOpts.algorithmMode);
    if (rawHz <= 0.0 || tunedHz <= 0.0) {
        std::cerr << "Pitch estimation failed. rawHz=" << rawHz << " tunedHz=" << tunedHz << "\n";
        return 1;
    }

    const double rawMidi = hzToMidi(rawHz);
    const double tunedMidi = hzToMidi(tunedHz);
    std::cout << "raw_ratio=" << raw.finalRatio << "\n";
    std::cout << "tuned_ratio=" << tuned.finalRatio << "\n";
    std::cout << "raw_hz=" << rawHz << " raw_midi=" << rawMidi << "\n";
    std::cout << "tuned_hz=" << tunedHz << " tuned_midi=" << tunedMidi << "\n";

    const double shiftHz = tunedHz - rawHz;
    const int targetMidi = static_cast<int>(std::lround(hzToMidi(rawHz)));
    const double targetHz = midiToHz(static_cast<double>(targetMidi));
    const double rawErrHz = std::abs(rawHz - targetHz);
    const double tunedErrHz = std::abs(tunedHz - targetHz);
    std::cout << "shift_hz=" << shiftHz << "\n";
    std::cout << "target_hz=" << targetHz
              << " raw_err_hz=" << rawErrHz
              << " tuned_err_hz=" << tunedErrHz << "\n";
    if (!(std::abs(shiftHz) > 2.0 && tunedErrHz <= std::max(2.0, rawErrHz * 0.75))) {
        std::cerr << "Autotune did not sufficiently improve pitch. rawHz=" << rawHz
                  << " tunedHz=" << tunedHz
                  << " targetHz=" << targetHz << "\n";
        return 1;
    }

    return 0;
}
