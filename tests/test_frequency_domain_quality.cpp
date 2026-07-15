#include "autotune_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double rms(const std::vector<float>& x, size_t begin, size_t end)
{
    end = std::min(end, x.size());
    double energy = 0.0;
    for (size_t i = begin; i < end; ++i) energy += static_cast<double>(x[i]) * x[i];
    return std::sqrt(energy / static_cast<double>(std::max<size_t>(1, end - begin)));
}

double correlation(const std::vector<float>& a, const std::vector<float>& b, size_t begin, size_t end)
{
    end = std::min({end, a.size(), b.size()});
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (size_t i = begin; i < end; ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return ab / std::sqrt(std::max(1e-20, aa * bb));
}

double dominantFrequency(const std::vector<float>& x,
                         unsigned int sampleRate,
                         size_t begin,
                         size_t end,
                         double lowHz,
                         double highHz)
{
    double bestHz = 0.0;
    double bestEnergy = -1.0;
    for (double hz = lowHz; hz <= highHz; hz += 0.1) {
        double re = 0.0;
        double im = 0.0;
        for (size_t i = begin; i < end; ++i) {
            const double angle = 2.0 * kPi * hz * static_cast<double>(i) / sampleRate;
            re += x[i] * std::cos(angle);
            im -= x[i] * std::sin(angle);
        }
        const double energy = re * re + im * im;
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestHz = hz;
        }
    }
    return bestHz;
}

double toneAmplitude(const std::vector<float>& x,
                     unsigned int sampleRate,
                     size_t begin,
                     size_t end,
                     double hz)
{
    double re = 0.0;
    double im = 0.0;
    for (size_t i = begin; i < end; ++i) {
        const double angle = 2.0 * kPi * hz * static_cast<double>(i) / sampleRate;
        re += x[i] * std::cos(angle);
        im -= x[i] * std::sin(angle);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(std::max<size_t>(1, end - begin));
}

} // namespace

int main()
{
    constexpr unsigned int sampleRate = 48000;
    constexpr double seconds = 3.0;
    std::vector<float> input(static_cast<size_t>(sampleRate * seconds), 0.0f);

    // An isolated transient and deterministic unvoiced noise must survive the
    // voiced-only spectral model without pre-echo or tonalization.
    input[2400] = 0.7f;
    uint32_t noiseState = 0x12345678u;
    for (size_t i = 4800; i < 16800; ++i) {
        noiseState = noiseState * 1664525u + 1013904223u;
        input[i] = (static_cast<float>((noiseState >> 8) & 0xffffu) / 32768.0f - 1.0f) * 0.025f;
    }

    // Detuned harmonic voice analogue with a slow amplitude envelope. It
    // should move to the nearest 12-TET target while retaining harmonics and
    // dynamics.
    double phase = 0.0;
    for (size_t i = 24000; i < input.size(); ++i) {
        const double t = static_cast<double>(i - 24000) / sampleRate;
        const double vibrato = 0.7 * std::sin(2.0 * kPi * 5.0 * t);
        const double hz = 265.0 + vibrato;
        phase += 2.0 * kPi * hz / sampleRate;
        const double envelope = t < 1.0 ? 0.07 : 0.19;
        input[i] = static_cast<float>(envelope *
            (std::sin(phase) + 0.35 * std::sin(2.0 * phase + 0.2) +
             0.16 * std::sin(3.0 * phase - 0.4)));
    }

    jtune::AutotuneOptions opts;
    opts.sampleRate = sampleRate;
    opts.strength = 1.0f;
    opts.wetMix = 1.0f;
    opts.minMidi = 48;
    opts.maxMidi = 80;
    opts.multiple = 24;
    opts.binCount = 192;
    opts.analysisHop = 96;
    opts.voicedThreshold = 0.20f;
    opts.freqMinHz = 100.0;
    opts.freqMaxHz = 3000.0;
    opts.resynthMode = jtune::FrequencyDomain;

    jtune::ConstantQAutotuneProcessor processor(opts);
    std::vector<float> output(input.size());
    for (size_t i = 0; i < input.size(); ++i) output[i] = processor.processSample(input[i]);

    bool finiteAndBounded = true;
    double maxVoicedStep = 0.0;
    for (size_t i = 1; i < output.size(); ++i) {
        finiteAndBounded = finiteAndBounded && std::isfinite(output[i]) && std::abs(output[i]) <= 1.0f;
        if (i > 30000) maxVoicedStep = std::max(maxVoicedStep, std::abs(static_cast<double>(output[i] - output[i - 1])));
    }

    const double noiseCorr = correlation(input, output, 6000, 15000);
    const double preEchoRms = rms(output, 100, 2200);
    const double transientPeak = *std::max_element(output.begin() + 2300, output.begin() + 2500);
    const double quietRms = rms(output, 36000, 60000);
    const double loudRms = rms(output, 108000, 132000);
    const double dynamicsRatio = loudRms / std::max(1e-9, quietRms);
    const double outputPitch = dominantFrequency(output, sampleRate, 72000, 96000, 250.0, 282.0);
    const double fundamentalAmp = toneAmplitude(output, sampleRate, 72000, 96000, outputPitch);
    const double secondHarmonicRatio =
        toneAmplitude(output, sampleRate, 72000, 96000, outputPitch * 2.0) / std::max(1e-9, fundamentalAmp);
    const double thirdHarmonicRatio =
        toneAmplitude(output, sampleRate, 72000, 96000, outputPitch * 3.0) / std::max(1e-9, fundamentalAmp);
    constexpr double targetHz = 261.625565;

    std::cout << "frequency_quality noise_corr=" << noiseCorr
              << " pre_echo_rms=" << preEchoRms
              << " transient_peak=" << transientPeak
              << " pitch_hz=" << outputPitch
              << " dynamics_ratio=" << dynamicsRatio
              << " harmonic_ratios=" << secondHarmonicRatio << "," << thirdHarmonicRatio
              << " max_voiced_step=" << maxVoicedStep << "\n";
    std::cout << "detected_hz=" << processor.currentDetectedPitchHz()
              << " target_hz=" << processor.currentTargetPitchHz()
              << " ratio=" << processor.currentPitchRatio() << "\n";

    if (!finiteAndBounded) return 1;
    if (noiseCorr < 0.95) return 1;
    if (preEchoRms > 1e-5) return 1;
    if (transientPeak < 0.60) return 1;
    if (std::abs(outputPitch - targetHz) > 4.0 || std::abs(outputPitch - targetHz) >= std::abs(265.0 - targetHz)) return 1;
    if (dynamicsRatio < 1.8) return 1;
    if (secondHarmonicRatio < 0.08 || thirdHarmonicRatio < 0.02) return 1;
    if (maxVoicedStep > 0.08) return 1;
    return 0;
}
