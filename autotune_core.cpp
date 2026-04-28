#include "autotune_core.h"

#include "loiacono_pitch_shift.h"
#include "loiacono_rolling.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace jtune {
namespace {

constexpr double kTwoPi = 2.0 * M_PI;
constexpr double kPi = M_PI;

double midiToHz(double midi, double baseA)
{
    return baseA * std::pow(2.0, (midi - 69.0) / 12.0);
}

double hzToMidi(double hz, double baseA)
{
    return 69.0 + 12.0 * std::log2(hz / baseA);
}

double hannWindow(double phase)
{
    const double p = std::clamp(phase, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kTwoPi * p);
}

}  // namespace

ConstantQAutotuneProcessor::ConstantQAutotuneProcessor(const AutotuneOptions& opts)
    : opts_(opts),
      analyze_(nullptr),
      synthesize_(std::make_unique<loiacono::LoiaconoSynthesize>())
{
    opts_.binCount = std::clamp(opts_.binCount, 32, 2400);
    opts_.multiple = std::clamp(opts_.multiple, 2, 240);
    opts_.analysisHop = std::max(16, opts_.analysisHop);
    opts_.freqMinHz = std::clamp(opts_.freqMinHz, 20.0, 12000.0);
    opts_.freqMaxHz = std::clamp(opts_.freqMaxHz, 40.0, 20000.0);
    if (opts_.freqMinHz >= opts_.freqMaxHz - 10.0) {
        opts_.freqMaxHz = opts_.freqMinHz + 10.0;
    }
    opts_.leakiness = std::clamp(opts_.leakiness, 0.99, 1.0);
    opts_.baseAFrequencyHz = std::clamp(opts_.baseAFrequencyHz, 400.0, 500.0);
    opts_.resynthMode = std::clamp(opts_.resynthMode, 0, 1);
    opts_.ratioSmoothing = std::clamp(opts_.ratioSmoothing, 0.01f, 1.0f);
    opts_.amplitudeSmoothing = std::clamp(opts_.amplitudeSmoothing, 0.01f, 1.0f);
    opts_.phasePull = std::clamp(opts_.phasePull, 0.0f, 1.0f);
    opts_.flowGrainMs = std::clamp(opts_.flowGrainMs, 5, 80);
    opts_.flowOverlap = std::clamp(opts_.flowOverlap, 0.10f, 0.95f);
    opts_.flowBaseDelayMs = std::clamp(opts_.flowBaseDelayMs, 5, 200);
    opts_.flowDriftCorrection = std::clamp(opts_.flowDriftCorrection, 0.0f, 0.2f);

    loiacono::AnalyzeConfig analyzeCfg;
    analyzeCfg.sampleRate = opts_.sampleRate;
    analyzeCfg.freqMinHz = opts_.freqMinHz;
    analyzeCfg.freqMaxHz = opts_.freqMaxHz;
    analyzeCfg.binCount = opts_.binCount;
    analyzeCfg.multiple = opts_.multiple;
    analyzeCfg.computeMode = opts_.computeMode;
    analyzeCfg.windowMode = opts_.windowMode;
    analyzeCfg.normalizationMode = opts_.normalizationMode;
    analyzeCfg.windowLengthMode = opts_.windowLengthMode;
    analyzeCfg.algorithmMode = opts_.algorithmMode;
    analyzeCfg.leakiness = opts_.leakiness;
    analyzeCfg.baseAFrequencyHz = opts_.baseAFrequencyHz;
    analyzeCfg.phaseEnabled = true;
    analyze_ = std::make_unique<loiacono::LoiaconoAnalyze>(analyzeCfg);

    const size_t n = static_cast<size_t>(std::max(1, analyze_->numBins()));
    spectrum_.assign(n, 0.0f);
    phase_.assign(n, 0.0f);
    shiftedMagnitudes_.assign(n, 0.0f);
    shiftedPhases_.assign(n, 0.0f);
    synthesize_->reset(analyze_->numBins());

    flowRing_.assign(static_cast<size_t>(1 << 15), 0.0f);
    flowGrainSize_ =
        std::max(64, static_cast<int>((static_cast<double>(opts_.sampleRate) * static_cast<double>(opts_.flowGrainMs)) / 1000.0));
    flowSynthesisHop_ = std::max(16, static_cast<int>(static_cast<float>(flowGrainSize_) * (1.0f - opts_.flowOverlap)));
    flowBaseDelay_ =
        std::max(flowGrainSize_, static_cast<int>((static_cast<double>(opts_.sampleRate) * static_cast<double>(opts_.flowBaseDelayMs)) / 1000.0));
    if (flowBaseDelay_ > static_cast<int>(flowRing_.size()) - 8) {
        flowBaseDelay_ = static_cast<int>(flowRing_.size()) - 8;
    }
    flowGrains_.assign(12, ActiveFlowGrain{});

    analysisCountdown_ = opts_.analysisHop;
}

ConstantQAutotuneProcessor::~ConstantQAutotuneProcessor() = default;

bool ConstantQAutotuneProcessor::inScale(int midiNote) const
{
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int minor[7] = {0, 2, 3, 5, 7, 8, 10};
    const int pc = ((midiNote % 12) + 12) % 12;
    const int* intervals = opts_.minor ? minor : major;
    for (int i = 0; i < 7; ++i) {
        if (((intervals[i] + opts_.keyRoot) % 12) == pc) return true;
    }
    return false;
}

int ConstantQAutotuneProcessor::nearestScaleMidi(double detectedMidi) const
{
    const int center = static_cast<int>(std::lround(detectedMidi));
    int best = center;
    double bestDist = 1e9;
    for (int n = center - 24; n <= center + 24; ++n) {
        if (!inScale(n)) continue;
        const double d = std::abs(static_cast<double>(n) - detectedMidi);
        if (d < bestDist) {
            bestDist = d;
            best = n;
        }
    }
    return best;
}

float ConstantQAutotuneProcessor::processSample(float inSample)
{
    const float wet = std::clamp(opts_.wetMix, 0.0f, 1.0f);
    const float dry = 1.0f - wet;
    const int flowRingSize = static_cast<int>(flowRing_.size());
    const int flowWritePos = static_cast<int>(flowSampleCounter_ % static_cast<uint64_t>(flowRingSize));
    flowRing_[static_cast<size_t>(flowWritePos)] = inSample;

    if (opts_.strength <= 1e-6f) {
        smoothedRatio_ = 1.0f;
        flowSampleCounter_++;
        return inSample;
    }

    analyze_->processSample(inSample);

    if (--analysisCountdown_ <= 0) {
        analysisCountdown_ = opts_.analysisHop;

        analyze_->getSpectrum(spectrum_);
        analyze_->getPhase(phase_);
        auto pitch = analyze_->transform().detectRootPitch(
            spectrum_,
            opts_.freqMinHz,
            opts_.freqMaxHz,
            opts_.baseAFrequencyHz);

        float targetRatio = 1.0f;
        if (pitch.freqHz > 0.0 && pitch.confidence >= static_cast<double>(opts_.voicedThreshold)) {
            const double detectedMidi = hzToMidi(pitch.freqHz, opts_.baseAFrequencyHz);
            const int detectedMidiRounded = static_cast<int>(std::lround(detectedMidi));
            if (detectedMidiRounded >= opts_.minMidi && detectedMidiRounded <= opts_.maxMidi) {
                const int targetMidi = nearestScaleMidi(detectedMidi);
                const double targetHz = midiToHz(targetMidi, opts_.baseAFrequencyHz);
                double semitones = (12.0 * std::log2(targetHz / pitch.freqHz)) * static_cast<double>(opts_.strength);
                // Keep the correction range conservative to reduce octave/alias jumps.
                semitones = std::clamp(semitones, -2.0, 2.0);
                targetRatio = static_cast<float>(std::pow(2.0, semitones / 12.0));
            }
        }
        smoothedRatio_ = smoothedRatio_ * (1.0f - opts_.ratioSmoothing) + targetRatio * opts_.ratioSmoothing;

        synthesize_->shiftFromAnalysis(*analyze_, spectrum_, phase_, smoothedRatio_, opts_.freqMinHz, opts_.freqMaxHz);
        shiftedMagnitudes_ = synthesize_->shiftedMagnitudes();
        shiftedPhases_ = synthesize_->shiftedPhases();
    }

    float tuned = 0.0f;
    if (opts_.resynthMode == TimeDomain) {
        auto sampleAt = [&](double idxAbs) -> float {
            idxAbs = std::fmod(idxAbs, static_cast<double>(flowRingSize));
            if (idxAbs < 0.0) idxAbs += static_cast<double>(flowRingSize);
            const int i0 = static_cast<int>(std::floor(idxAbs));
            const int i1 = (i0 + 1) % flowRingSize;
            const double frac = idxAbs - static_cast<double>(i0);
            const float s0 = flowRing_[static_cast<size_t>(i0)];
            const float s1 = flowRing_[static_cast<size_t>(i1)];
            return static_cast<float>(s0 * (1.0 - frac) + s1 * frac);
        };

        if (!flowInitialized_) {
            flowInitialized_ = true;
            flowAnalysisCursorAbs_ = static_cast<double>(flowSampleCounter_) - static_cast<double>(flowBaseDelay_);
            flowSamplesSinceSpawn_ = 0;
            for (auto& g : flowGrains_) g = ActiveFlowGrain{};
        }

        flowSamplesSinceSpawn_++;
        if (flowSamplesSinceSpawn_ >= flowSynthesisHop_) {
            flowSamplesSinceSpawn_ = 0;
            for (auto& g : flowGrains_) {
                if (!g.active) {
                    g.active = true;
                    g.age = 0;
                    g.readStartAbs = flowAnalysisCursorAbs_;
                    flowAnalysisCursorAbs_ += static_cast<double>(smoothedRatio_) * static_cast<double>(flowSynthesisHop_);
                    const double desired = static_cast<double>(flowSampleCounter_) - static_cast<double>(flowBaseDelay_);
                    flowAnalysisCursorAbs_ += static_cast<double>(opts_.flowDriftCorrection) * (desired - flowAnalysisCursorAbs_);
                    break;
                }
            }
        }

        const double maxReadableAbs = static_cast<double>(flowSampleCounter_) - 1.0;
        double accum = 0.0;
        double wsum = 0.0;
        for (auto& g : flowGrains_) {
            if (!g.active) continue;
            double readAbs = g.readStartAbs + static_cast<double>(g.age);
            if (readAbs > maxReadableAbs) readAbs = maxReadableAbs;
            const float s = sampleAt(readAbs);
            const double p = static_cast<double>(g.age) / static_cast<double>(std::max(1, flowGrainSize_ - 1));
            const double w = hannWindow(p);
            accum += static_cast<double>(s) * w;
            wsum += w;
            g.age++;
            if (g.age >= flowGrainSize_) g.active = false;
        }
        tuned = static_cast<float>(accum / std::max(1e-9, wsum));
    } else {
        tuned = synthesize_->synthShiftedSample(*analyze_, opts_.amplitudeSmoothing, opts_.phasePull);
    }
    tuned = std::clamp(tuned, -1.0f, 1.0f);

    inputEnv_ = inputEnv_ * 0.995f + std::abs(inSample) * 0.005f;
    outputEnv_ = outputEnv_ * 0.995f + std::abs(tuned) * 0.005f;
    const float gain = std::clamp(inputEnv_ / std::max(1e-5f, outputEnv_), 0.25f, 4.0f);
    tuned *= gain;

    flowSampleCounter_++;
    return dry * inSample + wet * tuned;
}

void ConstantQAutotuneProcessor::processBuffer(const float* in, float* out, unsigned int nFrames)
{
    for (unsigned int i = 0; i < nFrames; ++i) {
        const float sample = in ? in[i] : 0.0f;
        out[i] = processSample(sample);
    }
}

}  // namespace jtune
