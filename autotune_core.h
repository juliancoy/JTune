#pragma once

#include <memory>
#include <cstdint>
#include <vector>

class LoiaconoRolling;
namespace loiacono {
class LoiaconoAnalyze;
class LoiaconoSynthesize;
}

namespace jtune {

enum ResynthMode {
    FrequencyDomain = 0,
    TimeDomain = 1,
};

struct AutotuneOptions {
    unsigned int sampleRate = 48000;
    int keyRoot = 0;  // C
    bool minor = false;
    float strength = 1.0f;
    float wetMix = 1.0f;  // 0=dry passthrough, 1=fully tuned
    int minMidi = 40;
    int maxMidi = 84;

    int multiple = 24;
    int binCount = 128;
    int analysisHop = 256;

    float voicedThreshold = 0.25f;
    double freqMinHz = 100.0;
    double freqMaxHz = 3000.0;
    double leakiness = 0.9995;
    double baseAFrequencyHz = 440.0;

    int computeMode = 1;       // LoiaconoRolling::ComputeMode::MultiThread
    int windowMode = 0;        // RectangularWindow
    int normalizationMode = 2; // Energy
    int windowLengthMode = 2;  // PeriodMultiple
    int algorithmMode = 0;     // Loiacono

    // Resynthesis path: FrequencyDomain or TimeDomain.
    int resynthMode = TimeDomain;

    // FrequencyDomain resynthesis controls.
    float ratioSmoothing = 0.15f;      // 0..1, higher = faster ratio tracking
    float amplitudeSmoothing = 0.15f;  // 0..1, higher = faster bin amplitude updates
    float phasePull = 0.08f;           // 0..1, pull oscillator phase toward measured phase

    // TimeDomain controls.
    int flowGrainMs = 20;          // grain length in ms
    float flowOverlap = 0.75f;     // overlap fraction [0.1, 0.95]
    int flowBaseDelayMs = 40;      // analysis delay in ms
    float flowDriftCorrection = 0.01f; // cursor drift correction [0, 0.2]
};

class ConstantQAutotuneProcessor {
public:
    explicit ConstantQAutotuneProcessor(const AutotuneOptions& opts);
    ~ConstantQAutotuneProcessor();

    float processSample(float inSample);
    void processBuffer(const float* in, float* out, unsigned int nFrames);

    float currentPitchRatio() const { return smoothedRatio_; }

private:
    bool inScale(int midiNote) const;
    int nearestScaleMidi(double detectedMidi) const;

    AutotuneOptions opts_;
    std::unique_ptr<loiacono::LoiaconoAnalyze> analyze_;
    std::unique_ptr<loiacono::LoiaconoSynthesize> synthesize_;

    std::vector<float> spectrum_;
    std::vector<float> phase_;
    std::vector<float> shiftedMagnitudes_;
    std::vector<float> shiftedPhases_;
    float inputEnv_ = 0.0f;
    float outputEnv_ = 0.0f;

    // TimeDomain state.
    std::vector<float> flowRing_;
    uint64_t flowSampleCounter_ = 0;
    int flowGrainSize_ = 1024;
    int flowBaseDelay_ = 2048;
    int flowSynthesisHop_ = 256;
    int flowSamplesSinceSpawn_ = 0;
    double flowAnalysisCursorAbs_ = 0.0;
    bool flowInitialized_ = false;
    struct ActiveFlowGrain {
        double readStartAbs = 0.0;
        int age = 0;
        bool active = false;
    };
    std::vector<ActiveFlowGrain> flowGrains_;

    float smoothedRatio_ = 1.0f;
    int analysisCountdown_ = 0;
};

}  // namespace jtune
