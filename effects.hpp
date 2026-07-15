#pragma once

#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdint>

class DelayEffect {
public:
    DelayEffect(float sampleRate = 44100.0f);
    
    void setDelayTime(float seconds);
    void setDecay(float decay); // 0.0 to 1.0
    void setMix(float mix); // 0.0 (dry) to 1.0 (wet)
    void setPingPong(bool enabled);
    bool isPingPongEnabled() const;
    
    void process(float* buffer, int numFrames, int numChannels);
    void reset();
    
    float getDelayTime() const { return delayTime_; }
    float getDecay() const { return decay_; }
    float getMix() const { return mix_; }
    
private:
    float sampleRate_;
    float delayTime_; // in seconds
    float decay_;     // feedback amount (0.0 to 1.0)
    float mix_;       // dry/wet mix (0.0 to 1.0)
    
    std::vector<float> delayBuffer_;
    size_t writeFramePos_;
    size_t delaySamplesFrames_;
    size_t activeChannels_;
    size_t maxDelayFrames_;
    bool pingPongEnabled_;
    
    mutable std::mutex mutex_;
    
    void updateDelaySamples();
};

class AmpModelEffect {
public:
    AmpModelEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setDriveDb(float driveDb); // 0 to 30 dB
    void setTone(float tone); // 0.0 to 1.0
    void setOutputDb(float outputDb); // -12 to +12 dB

    float getDriveDb() const { return driveDb_; }
    float getTone() const { return tone_; }
    float getOutputDb() const { return outputDb_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    struct OnePoleLPF {
        float a = 0.0f;
        float z = 0.0f;
        void setCutoff(float cutoff, float sampleRate);
        float process(float input);
        void reset() { z = 0.0f; }
    };

    float sampleRate_;
    float driveDb_;
    float tone_;
    float outputDb_;
    float toneCutoff_;
    std::vector<OnePoleLPF> toneFilters_;

    void ensureChannels(size_t channels);
    void updateToneCutoff();
};

class SoftClipperEffect {
public:
    SoftClipperEffect();

    void setDrive(float drive); // 1.0 to 20.0
    void setMix(float mix); // 0.0 to 1.0

    float getDrive() const { return drive_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);

private:
    float drive_;
    float mix_;
};

class FuzzEffect {
public:
    FuzzEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setDrive(float drive); // 1.0 to 40.0
    void setTone(float tone); // 0.0 to 1.0
    void setMix(float mix); // 0.0 to 1.0

    float getDrive() const { return drive_; }
    float getTone() const { return tone_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    float sampleRate_;
    float drive_;
    float tone_;
    float mix_;
    float toneCoeff_;
    std::vector<float> toneState_;

    void ensureChannels(size_t channels);
    void updateTone();
};

class TremoloEffect {
public:
    TremoloEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setRate(float rateHz); // 0.1 to 12.0
    void setDepth(float depth); // 0.0 to 1.0

    float getRate() const { return rateHz_; }
    float getDepth() const { return depth_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    float sampleRate_;
    float rateHz_;
    float depth_;
    float phase_;
};

class ChorusEffect {
public:
    ChorusEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setRate(float rateHz); // 0.05 to 8.0
    void setDepthMs(float depthMs); // 0.1 to 20.0
    void setMix(float mix); // 0.0 to 1.0

    float getRate() const { return rateHz_; }
    float getDepthMs() const { return depthMs_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    float sampleRate_;
    float rateHz_;
    float depthMs_;
    float mix_;
    float phase_;
    size_t activeChannels_;
    size_t maxDelayFrames_;
    size_t writeFramePos_;
    std::vector<float> buffer_;

    void ensureChannels(size_t channels);
    float readDelay(float delayFrames, int channel) const;
};

class PhaserEffect {
public:
    PhaserEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setRate(float rateHz); // 0.05 to 8.0
    void setDepth(float depth); // 0.0 to 1.0
    void setFeedback(float feedback); // 0.0 to 0.95
    void setMix(float mix); // 0.0 to 1.0

    float getRate() const { return rateHz_; }
    float getDepth() const { return depth_; }
    float getFeedback() const { return feedback_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    struct ChannelState {
        std::array<float, 4> x1{};
        std::array<float, 4> y1{};
        float feedbackSample = 0.0f;
    };

    float sampleRate_;
    float rateHz_;
    float depth_;
    float feedback_;
    float mix_;
    float phase_;
    std::vector<ChannelState> channelStates_;

    void ensureChannels(size_t channels);
};

class BitCrusherEffect {
public:
    BitCrusherEffect();

    void setBitDepth(float bitDepth); // 1.0 to 16.0
    void setSampleRateReduction(float reduction); // 1.0 to 64.0
    void setMix(float mix); // 0.0 to 1.0

    float getBitDepth() const { return bitDepth_; }
    float getSampleRateReduction() const { return sampleRateReduction_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    float bitDepth_;
    float sampleRateReduction_;
    float mix_;
    int holdCounter_;
    std::vector<float> heldSamples_;
};

class GranulatorEffect {
public:
    GranulatorEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setGrainSizeMs(float grainSizeMs); // 10.0 to 250.0
    void setTexture(float texture); // 0.0 to 1.0
    void setMix(float mix); // 0.0 to 1.0

    float getGrainSizeMs() const { return grainSizeMs_; }
    float getTexture() const { return texture_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    float sampleRate_;
    float grainSizeMs_;
    float texture_;
    float mix_;
    size_t activeChannels_;
    size_t maxDelayFrames_;
    size_t writeFramePos_;
    int grainPhase_;
    int grainSizeFrames_;
    size_t grainStartA_;
    size_t grainStartB_;
    uint32_t randomState_;
    std::vector<float> buffer_;

    void ensureChannels(size_t channels);
    void updateGrainSize();
    size_t chooseGrainOffset();
    float readGrain(size_t startFrame, int phase, int channel, int numChannels) const;
};

class ReverbEffect {
public:
    ReverbEffect(float sampleRate = 44100.0f);

    void setSampleRate(float sampleRate);
    void setRoomSize(float roomSize); // 0.0 to 1.0
    void setDamping(float damping); // 0.0 to 1.0
    void setMix(float mix); // 0.0 to 1.0

    float getRoomSize() const { return roomSize_; }
    float getDamping() const { return damping_; }
    float getMix() const { return mix_; }

    void process(float* buffer, int numFrames, int numChannels);
    void reset();

private:
    struct CombFilter {
        std::vector<float> buffer;
        size_t index = 0;
        float feedback = 0.0f;
        float damp = 0.0f;
        float filterStore = 0.0f;

        float process(float input);
        void reset();
    };

    struct AllpassFilter {
        std::vector<float> buffer;
        size_t index = 0;
        float feedback = 0.5f;

        float process(float input);
        void reset();
    };

    float sampleRate_;
    float roomSize_;
    float damping_;
    float mix_;
    size_t activeChannels_;
    std::vector<std::array<CombFilter, 4>> combs_;
    std::vector<std::array<AllpassFilter, 2>> allpasses_;

    void ensureChannels(size_t channels);
    void configureFilters();
};

class EffectsProcessor {
public:
    EffectsProcessor(float sampleRate = 44100.0f);
    
    void setSampleRate(float sampleRate);
    
    // Delay effect controls
    void setDelayEnabled(bool enabled);
    void setDelayTime(float seconds);
    void setDelayDecay(float decay);
    void setDelayMix(float mix);
    void setDelayPingPong(bool enabled);

    // Amp modeling
    void setAmpEnabled(bool enabled);
    void setAmpDriveDb(float driveDb);
    void setAmpTone(float tone);
    void setAmpOutputDb(float outputDb);

    bool isAmpEnabled() const { return ampEnabled_; }
    float getAmpDriveDb() const { return amp_.getDriveDb(); }
    float getAmpTone() const { return amp_.getTone(); }
    float getAmpOutputDb() const { return amp_.getOutputDb(); }

    // Soft clipping
    void setSoftClipEnabled(bool enabled);
    void setSoftClipDrive(float drive);
    void setSoftClipMix(float mix);

    bool isSoftClipEnabled() const { return softClipEnabled_; }
    float getSoftClipDrive() const { return softClip_.getDrive(); }
    float getSoftClipMix() const { return softClip_.getMix(); }

    // Fuzz
    void setFuzzEnabled(bool enabled);
    void setFuzzDrive(float drive);
    void setFuzzTone(float tone);
    void setFuzzMix(float mix);

    bool isFuzzEnabled() const { return fuzzEnabled_; }
    float getFuzzDrive() const { return fuzz_.getDrive(); }
    float getFuzzTone() const { return fuzz_.getTone(); }
    float getFuzzMix() const { return fuzz_.getMix(); }

    // Tremolo
    void setTremoloEnabled(bool enabled);
    void setTremoloRate(float rateHz);
    void setTremoloDepth(float depth);

    bool isTremoloEnabled() const { return tremoloEnabled_; }
    float getTremoloRate() const { return tremolo_.getRate(); }
    float getTremoloDepth() const { return tremolo_.getDepth(); }

    // Chorus
    void setChorusEnabled(bool enabled);
    void setChorusRate(float rateHz);
    void setChorusDepthMs(float depthMs);
    void setChorusMix(float mix);

    bool isChorusEnabled() const { return chorusEnabled_; }
    float getChorusRate() const { return chorus_.getRate(); }
    float getChorusDepthMs() const { return chorus_.getDepthMs(); }
    float getChorusMix() const { return chorus_.getMix(); }

    // Phaser
    void setPhaserEnabled(bool enabled);
    void setPhaserRate(float rateHz);
    void setPhaserDepth(float depth);
    void setPhaserFeedback(float feedback);
    void setPhaserMix(float mix);

    bool isPhaserEnabled() const { return phaserEnabled_; }
    float getPhaserRate() const { return phaser_.getRate(); }
    float getPhaserDepth() const { return phaser_.getDepth(); }
    float getPhaserFeedback() const { return phaser_.getFeedback(); }
    float getPhaserMix() const { return phaser_.getMix(); }

    // Bit crushing
    void setBitCrusherEnabled(bool enabled);
    void setBitCrusherBitDepth(float bitDepth);
    void setBitCrusherSampleRateReduction(float reduction);
    void setBitCrusherMix(float mix);

    bool isBitCrusherEnabled() const { return bitCrusherEnabled_; }
    float getBitCrusherBitDepth() const { return bitCrusher_.getBitDepth(); }
    float getBitCrusherSampleRateReduction() const { return bitCrusher_.getSampleRateReduction(); }
    float getBitCrusherMix() const { return bitCrusher_.getMix(); }

    // Granulation
    void setGranulatorEnabled(bool enabled);
    void setGranulatorGrainSizeMs(float grainSizeMs);
    void setGranulatorTexture(float texture);
    void setGranulatorMix(float mix);

    bool isGranulatorEnabled() const { return granulatorEnabled_; }
    float getGranulatorGrainSizeMs() const { return granulator_.getGrainSizeMs(); }
    float getGranulatorTexture() const { return granulator_.getTexture(); }
    float getGranulatorMix() const { return granulator_.getMix(); }

    // Reverb
    void setReverbEnabled(bool enabled);
    void setReverbRoomSize(float roomSize);
    void setReverbDamping(float damping);
    void setReverbMix(float mix);

    bool isReverbEnabled() const { return reverbEnabled_; }
    float getReverbRoomSize() const { return reverb_.getRoomSize(); }
    float getReverbDamping() const { return reverb_.getDamping(); }
    float getReverbMix() const { return reverb_.getMix(); }
    
    bool isDelayEnabled() const { return delayEnabled_; }
    float getDelayTime() const { return delay_.getDelayTime(); }
    float getDelayDecay() const { return delay_.getDecay(); }
    float getDelayMix() const { return delay_.getMix(); }
    bool isDelayPingPongEnabled() const { return delay_.isPingPongEnabled(); }
    
    enum class EffectOrder {
        DriveModDelayReverb = 0,
        DelayReverbDriveMod = 1,
    };

    void setEffectOrder(EffectOrder order);
    EffectOrder getEffectOrder() const { return effectOrder_; }

    // Process audio buffer
    void process(float* buffer, int numFrames, int numChannels);
    
    void reset();
    
private:
    DelayEffect delay_;
    bool delayEnabled_;
    AmpModelEffect amp_;
    bool ampEnabled_;
    SoftClipperEffect softClip_;
    bool softClipEnabled_;
    FuzzEffect fuzz_;
    bool fuzzEnabled_;
    TremoloEffect tremolo_;
    bool tremoloEnabled_;
    ChorusEffect chorus_;
    bool chorusEnabled_;
    PhaserEffect phaser_;
    bool phaserEnabled_;
    BitCrusherEffect bitCrusher_;
    bool bitCrusherEnabled_;
    GranulatorEffect granulator_;
    bool granulatorEnabled_;
    ReverbEffect reverb_;
    bool reverbEnabled_;
    float sampleRate_;
    bool delayPingPongEnabled_;
    EffectOrder effectOrder_;
    std::mutex mutex_;
};
