#include "autotune_core.h"
#include "pitch_system.hpp"

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
    opts_.referenceMidiNote = std::clamp(opts_.referenceMidiNote, 0, 127);
    opts_.octaveShift = std::clamp(opts_.octaveShift, -2, 2);
    opts_.baseAFrequencyHz = std::clamp(opts_.baseAFrequencyHz, 1.0, 20000.0);
    const double analysisA4Hz = opts_.baseAFrequencyHz *
        std::pow(2.0, (69.0 - static_cast<double>(opts_.referenceMidiNote)) / 12.0);
    opts_.resynthMode = std::clamp(opts_.resynthMode, 0, 1);
    opts_.ratioSmoothing = std::clamp(opts_.ratioSmoothing, 0.01f, 1.0f);
    opts_.correctionHoldMs = std::clamp(opts_.correctionHoldMs, 0, 500);
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
    analyzeCfg.baseAFrequencyHz = analysisA4Hz;
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

double ConstantQAutotuneProcessor::targetFrequency(double detectedHz)
{
    const auto& registry = pitchSystemRegistry();
    const PitchSystemDefinition* definition = registry.byId(opts_.pitchSystemId);
    if (!definition || !definition->correctionEligible) {
        currentTargetId_.clear();
        currentTargetReason_ = definition ? "definition is reference-only" : "pitch-system id not found";
        currentTargetConfidence_ = 0.0f;
        currentTargetUncertaintyCents_ = 0.0f;
        return detectedHz;
    }
    PitchContext context;
    context.referenceMidi = opts_.referenceMidiNote;
    context.referenceHz = opts_.baseAFrequencyHz;
    context.octaveShift = opts_.octaveShift;
    context.tonicMidi = opts_.tonicMidiNote;
    const PitchCollectionDefinition* collection = pitchCollectionById(opts_.pitchCollectionId);
    const std::vector<int>* enabledDegrees = nullptr;
    if (collection && collection->id == "custom") enabledDegrees = &opts_.customEnabledDegrees;
    else if (collection && collection->degreeCount == static_cast<int>(definition->targets.size()))
        enabledDegrees = &collection->enabledDegrees;
    context.enabledDegrees = enabledDegrees;
    double movementCents = 0.0;
    if (previousContextPitchHz_ > 0.0)
        movementCents = 1200.0 * std::log2(detectedHz / previousContextPitchHz_);
    if (movementCents > 2.0) context.direction = MelodicDirection::Ascending;
    else if (movementCents < -2.0) context.direction = MelodicDirection::Descending;
    else context.direction = MelodicDirection::Stable;
    context.previousTargetId = previousTargetId_;
    previousContextPitchHz_ = detectedHz;

    // Fast intentional movement (slides, meend, gamaka attacks) is not a pitch
    // error. Leave it continuous rather than quantizing every analysis frame.
    if (opts_.preserveRapidPitchMotion && std::abs(movementCents) >= 80.0) {
        currentTargetId_.clear();
        currentTargetReason_ = "rapid intentional pitch motion preserved";
        currentTargetConfidence_ = 0.0f;
        currentTargetUncertaintyCents_ = 0.0f;
        return detectedHz;
    }

    const auto candidates = PitchSystemEvaluator(*definition).candidates(detectedHz, context);
    if (candidates.empty()) {
        currentTargetId_.clear();
        currentTargetReason_ = "no eligible target for the current context";
        currentTargetConfidence_ = 0.0f;
        currentTargetUncertaintyCents_ = 0.0f;
        return detectedHz;
    }
    const EvaluatedTarget* selected = &candidates.front();
    if (!previousTargetId_.empty()) {
        const auto previous = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return candidate.target.id == previousTargetId_;
        });
        if (previous != candidates.end() &&
            std::abs(previous->distanceCents) <= std::abs(selected->distanceCents) + opts_.targetHysteresisCents)
            selected = &*previous;
    }
    previousTargetId_ = selected->target.id;
    currentTargetId_ = selected->target.id;
    currentTargetReason_ = selected->reason;
    currentTargetConfidence_ = static_cast<float>(selected->target.confidence);
    currentTargetUncertaintyCents_ = static_cast<float>(selected->target.uncertaintyCents);
    return selected->frequencyHz;
}

double ConstantQAutotuneProcessor::referenceA4Frequency() const
{
    return opts_.baseAFrequencyHz *
        std::pow(2.0, (69.0 - static_cast<double>(opts_.referenceMidiNote)) / 12.0);
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
        ++spectrumRevision_;
        analyze_->getPhase(phase_);
        const float spectrumPeak = spectrum_.empty()
            ? 0.0f
            : *std::max_element(spectrum_.begin(), spectrum_.end());
        double spectrumSum = 0.0;
        for (float magnitude : spectrum_) spectrumSum += std::max(0.0f, magnitude);
        const double spectrumMean = spectrumSum / static_cast<double>(std::max<size_t>(1, spectrum_.size()));
        const double spectralCrest = static_cast<double>(spectrumPeak) / std::max(1e-12, spectrumMean);
        auto pitch = analyze_->transform().detectRootPitch(
            spectrum_,
            opts_.freqMinHz,
            opts_.freqMaxHz,
            referenceA4Frequency());

        // Preserve the last ratio through short confidence gaps. Resetting to
        // 1.0 on every missed vocal frame audibly leaked the uncorrected slide.
        float targetRatio = smoothedRatio_;
        bool hasCorrection = false;
        if (pitch.freqHz > 0.0
            && pitch.confidence >= static_cast<double>(opts_.voicedThreshold)
            && (opts_.resynthMode != FrequencyDomain || spectralCrest >= 3.0)) {
            detectedPitchHz_ = static_cast<float>(pitch.freqHz);
            const double detectedMidi = hzToMidi(pitch.freqHz, referenceA4Frequency());
            const int detectedMidiRounded = static_cast<int>(std::lround(detectedMidi));
            if (detectedMidiRounded >= opts_.minMidi && detectedMidiRounded <= opts_.maxMidi) {
                const double targetHz = targetFrequency(pitch.freqHz);
                targetPitchHz_ = static_cast<float>(targetHz);
                double semitones = (12.0 * std::log2(targetHz / pitch.freqHz)) * static_cast<double>(opts_.strength);
                const auto* activeDefinition = pitchSystemRegistry().byId(opts_.pitchSystemId);
                const double correctionCents = activeDefinition && activeDefinition->correctionRangeCents > 0.0
                    ? activeDefinition->correctionRangeCents : 200.0;
                const double maxShift = correctionCents / 100.0 + 12.0 * std::abs(opts_.octaveShift);
                semitones = std::clamp(semitones, -maxShift, maxShift);
                targetRatio = static_cast<float>(std::pow(2.0, semitones / 12.0));
                hasCorrection = true;
            }
        }
        if (hasCorrection) {
            unvoicedAnalysisSamples_ = 0;
            frequencyVoiced_ = true;
        } else {
            unvoicedAnalysisSamples_ += opts_.analysisHop;
            const int holdSamples = static_cast<int>(
                static_cast<double>(opts_.sampleRate) * static_cast<double>(opts_.correctionHoldMs) / 1000.0);
            if (unvoicedAnalysisSamples_ > holdSamples) {
                targetRatio = 1.0f;
                detectedPitchHz_ = 0.0f;
                targetPitchHz_ = 0.0f;
                frequencyVoiced_ = false;
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
                    // Advance at the requested pitch ratio so each grain
                    // carries the full correction. Keep the cursor within one
                    // grain of the fixed-delay point using occasional wrapped
                    // jumps; overlapping grains crossfade those jumps. The old
                    // continuous drift pull relaxed the cursor toward 1:1 and
                    // audibly let sustained slides pass between target notes.
                    g.readStartAbs = flowAnalysisCursorAbs_;
                    flowAnalysisCursorAbs_ +=
                        static_cast<double>(smoothedRatio_) * static_cast<double>(flowSynthesisHop_);
                    const double desired =
                        static_cast<double>(flowSampleCounter_) - static_cast<double>(flowBaseDelay_);
                    const double offset = flowAnalysisCursorAbs_ - desired;
                    // Wrap by one detected fundamental period. Unlike an
                    // arbitrary grain-sized jump, this lands at essentially
                    // the same waveform phase and avoids periodic pitch
                    // splatter while keeping the source cursor bounded.
                    const double wrapSamples = detectedPitchHz_ > 0.0f
                        ? static_cast<double>(opts_.sampleRate) / static_cast<double>(detectedPitchHz_)
                        : static_cast<double>(flowGrainSize_);
                    if (offset > wrapSamples) {
                        flowAnalysisCursorAbs_ -= wrapSamples;
                    } else if (offset < -wrapSamples) {
                        flowAnalysisCursorAbs_ += wrapSamples;
                    }
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
        tuned = synthesize_->synthShiftedSample(
            *analyze_, opts_.amplitudeSmoothing, opts_.phasePull, opts_.analysisHop);
        // The oscillator bank has no valid amplitudes before the first
        // analysis frame. Fade it in instead of switching from silence (or
        // the ratio-1 bypass) in one sample.
        const float attackStep = 1.0f / std::max(1.0f, static_cast<float>(opts_.sampleRate) * 0.01f);
        const float releaseStep = 1.0f / std::max(1.0f, static_cast<float>(opts_.sampleRate) * 0.02f);
        const float blendTarget = synthesize_->hasShift() && frequencyVoiced_ ? 1.0f : 0.0f;
        if (frequencySynthBlend_ < blendTarget) {
            frequencySynthBlend_ = std::min(blendTarget, frequencySynthBlend_ + attackStep);
        } else {
            frequencySynthBlend_ = std::max(blendTarget, frequencySynthBlend_ - releaseStep);
        }
        tuned = inSample * (1.0f - frequencySynthBlend_) + tuned * frequencySynthBlend_;
    }
    tuned = std::clamp(tuned, -1.0f, 1.0f);

    inputEnv_ = inputEnv_ * 0.995f + std::abs(inSample) * 0.005f;
    outputEnv_ = outputEnv_ * 0.995f + std::abs(tuned) * 0.005f;
    const float gain = std::clamp(inputEnv_ / std::max(1e-5f, outputEnv_), 0.25f, 4.0f);
    tuned *= gain;

    flowSampleCounter_++;
    return std::clamp(dry * inSample + wet * tuned, -1.0f, 1.0f);
}

void ConstantQAutotuneProcessor::processBuffer(const float* in, float* out, unsigned int nFrames)
{
    for (unsigned int i = 0; i < nFrames; ++i) {
        const float sample = in ? in[i] : 0.0f;
        out[i] = processSample(sample);
    }
}

}  // namespace jtune
