#include "autotune_core.h"
#include "pitch_tracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
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

std::vector<float> runProcessor(const std::vector<float>& input, const jtune::AutotuneOptions& opts)
{
    jtune::ConstantQAutotuneProcessor proc(opts);
    std::vector<float> out(input.size(), 0.0f);
    for (size_t i = 0; i < input.size(); ++i) {
        out[i] = proc.processSample(input[i]);
    }
    return out;
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
    if (est.empty()) {
        return estimateMedianPitchHzForAlgorithm(signal, opts, dutAlgorithm);
    }
    if (est.size() == 1) return est[0];
    std::sort(est.begin(), est.end());
    return 0.5 * (est[0] + est[1]);
}

const char* algoName(int mode)
{
    switch (mode) {
    case 0: return "loiacono";
    case 1: return "fft";
    case 2: return "goertzel";
    default: return "unknown";
    }
}

bool runCaseForAlgorithm(int algorithmMode)
{
    jtune::AutotuneOptions base;
    base.sampleRate = 48000;
    base.keyRoot = 0;
    base.minor = false;
    base.minMidi = 50;
    base.maxMidi = 80;
    base.multiple = 24;
    base.voicedThreshold = 0.20f;
    base.binCount = 192;
    base.analysisHop = 96;
    base.freqMinHz = 100.0;
    base.freqMaxHz = 3000.0;
    base.algorithmMode = algorithmMode;
    base.resynthMode = jtune::TimeDomain;

    const auto chirp = generateChirp(base.sampleRate, 4.0, 275.0, 282.0);

    auto passthroughOpts = base;
    passthroughOpts.strength = 0.0f;
    const auto raw = runProcessor(chirp, passthroughOpts);

    auto tunedOpts = base;
    tunedOpts.strength = 1.0f;
    const auto tuned = runProcessor(chirp, tunedOpts);

    const double rawHz = estimateMedianPitchHzCrossValidated(raw, tunedOpts, algorithmMode);
    const double tunedHz = estimateMedianPitchHzCrossValidated(tuned, tunedOpts, algorithmMode);

    if (!(rawHz > 0.0) || !(tunedHz > 0.0)) {
        std::cerr << "cross-validated pitch estimate failed for " << algoName(algorithmMode)
                  << " raw=" << rawHz << " tuned=" << tunedHz << "\n";
        return false;
    }

    const double shiftHz = tunedHz - rawHz;
    const double rawMidi = hzToMidi(rawHz);
    const int targetMidi = nearestScaleMidiForKey(rawMidi, base.keyRoot, base.minor);
    const double targetHz = midiToHz(static_cast<double>(targetMidi));
    double semitones = 12.0 * std::log2(targetHz / rawHz);
    semitones = std::clamp(semitones, -6.0, 6.0);
    const double expectedShiftHz = rawHz * std::pow(2.0, semitones / 12.0) - rawHz;
    const double rawTargetErrHz = std::abs(rawHz - targetHz);
    const double tunedTargetErrHz = std::abs(tunedHz - targetHz);

    bool pitchOk = false;
    if (std::abs(expectedShiftHz) < 1.5) {
        pitchOk = std::abs(shiftHz) < 2.0;
    } else {
        const bool moved = std::abs(shiftHz) > 2.0;
        const bool improved = tunedTargetErrHz <= std::max(2.0, rawTargetErrHz * 0.75);
        pitchOk = moved && improved;
    }

    std::cout << "algo=" << algoName(algorithmMode)
              << " raw_hz=" << rawHz
              << " tuned_hz=" << tunedHz
              << " shift_hz=" << shiftHz
              << " expected_shift_hz=" << expectedShiftHz
              << " pitch_ok=" << (pitchOk ? "true" : "false") << "\n";

    return pitchOk;
}

}  // namespace

int main()
{
    bool ok = true;
    for (int algorithmMode : {0, 1, 2}) {
        ok = runCaseForAlgorithm(algorithmMode) && ok;
    }
    return ok ? 0 : 1;
}
