#include "effects.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

DelayEffect::DelayEffect(float sampleRate)
    : sampleRate_(sampleRate)
    , delayTime_(0.5f)
    , decay_(0.5f)
    , mix_(0.3f)
    , writeFramePos_(0)
    , delaySamplesFrames_(0)
    , activeChannels_(2)
    , maxDelayFrames_(static_cast<size_t>(sampleRate_ * 2.0f))
    , pingPongEnabled_(false)
{
    if (maxDelayFrames_ == 0) {
        maxDelayFrames_ = 1;
    }
    delayBuffer_.resize(maxDelayFrames_ * activeChannels_, 0.0f);
    updateDelaySamples();
}

void DelayEffect::setDelayTime(float seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    delayTime_ = std::clamp(seconds, 0.01f, 2.0f); // Limit to 0.01-2 seconds
    updateDelaySamples();
}

void DelayEffect::setDecay(float decay) {
    std::lock_guard<std::mutex> lock(mutex_);
    decay_ = std::clamp(decay, 0.0f, 0.95f); // Limit to prevent infinite feedback
}

void DelayEffect::setMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void DelayEffect::setPingPong(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    pingPongEnabled_ = enabled;
}

bool DelayEffect::isPingPongEnabled() const {
    auto &guardMutex = const_cast<std::mutex &>(mutex_);
    std::lock_guard<std::mutex> lock(guardMutex);
    return pingPongEnabled_;
}

void DelayEffect::updateDelaySamples() {
    delaySamplesFrames_ = static_cast<size_t>(sampleRate_ * delayTime_);
    if (delaySamplesFrames_ >= maxDelayFrames_) {
        delaySamplesFrames_ = maxDelayFrames_ - 1;
        delayTime_ = static_cast<float>(delaySamplesFrames_) / sampleRate_;
    }
}

void DelayEffect::process(float* buffer, int numFrames, int numChannels) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (delaySamplesFrames_ == 0 || mix_ == 0.0f || numChannels <= 0) {
        return; // No delay effect
    }
    if (static_cast<size_t>(numChannels) > activeChannels_) {
        activeChannels_ = static_cast<size_t>(numChannels);
        delayBuffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
        writeFramePos_ = 0;
    }

    size_t bufferFrames = maxDelayFrames_;
    for (int frame = 0; frame < numFrames; ++frame) {
        size_t readFrame = (writeFramePos_ + bufferFrames - delaySamplesFrames_) % bufferFrames;
        size_t baseRead = readFrame * activeChannels_;
        size_t baseWrite = writeFramePos_ * activeChannels_;
        for (int channel = 0; channel < numChannels; ++channel) {
            int sampleIndex = frame * numChannels + channel;
            float input = buffer[sampleIndex];

            size_t readIndex = baseRead + channel;
            if (readIndex >= delayBuffer_.size()) {
                readIndex = baseRead + (channel % activeChannels_);
            }
            float delayed = delayBuffer_[readIndex];

            float feedback = delayed * decay_;
            if (pingPongEnabled_ && numChannels > 1) {
                int otherChannel = (channel + 1) % numChannels;
                size_t altReadIndex = baseRead + otherChannel;
                if (altReadIndex < delayBuffer_.size()) {
                    feedback = delayBuffer_[altReadIndex] * decay_;
                }
            }

            size_t writeIndex = baseWrite + channel;
            if (writeIndex >= delayBuffer_.size()) {
                writeIndex = baseWrite + (channel % activeChannels_);
            }
            delayBuffer_[writeIndex] = input + feedback;

            buffer[sampleIndex] = input * (1.0f - mix_) + delayed * mix_;
        }
        writeFramePos_ = (writeFramePos_ + 1) % bufferFrames;
    }
}

void DelayEffect::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(delayBuffer_.begin(), delayBuffer_.end(), 0.0f);
    writeFramePos_ = 0;
}

AmpModelEffect::AmpModelEffect(float sampleRate)
    : sampleRate_(sampleRate)
    , driveDb_(12.0f)
    , tone_(0.5f)
    , outputDb_(0.0f)
    , toneCutoff_(2000.0f)
{
    updateToneCutoff();
}

void AmpModelEffect::OnePoleLPF::setCutoff(float cutoff, float sampleRate) {
    if (sampleRate <= 0.0f) {
        a = 0.0f;
        return;
    }
    float x = std::exp(-2.0f * kPi * cutoff / sampleRate);
    a = x;
}

float AmpModelEffect::OnePoleLPF::process(float input) {
    z = (1.0f - a) * input + a * z;
    return z;
}

void AmpModelEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
    updateToneCutoff();
}

void AmpModelEffect::setDriveDb(float driveDb) {
    driveDb_ = std::clamp(driveDb, 0.0f, 30.0f);
}

void AmpModelEffect::setTone(float tone) {
    tone_ = std::clamp(tone, 0.0f, 1.0f);
    updateToneCutoff();
}

void AmpModelEffect::setOutputDb(float outputDb) {
    outputDb_ = std::clamp(outputDb, -12.0f, 12.0f);
}

void AmpModelEffect::ensureChannels(size_t channels) {
    if (toneFilters_.size() == channels) return;
    toneFilters_.assign(channels, OnePoleLPF{});
    for (auto &filter : toneFilters_) {
        filter.setCutoff(toneCutoff_, sampleRate_);
    }
}

void AmpModelEffect::updateToneCutoff() {
    float minCut = 600.0f;
    float maxCut = 8000.0f;
    toneCutoff_ = minCut * std::pow(maxCut / minCut, tone_);
    for (auto &filter : toneFilters_) {
        filter.setCutoff(toneCutoff_, sampleRate_);
    }
}

void AmpModelEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0) return;
    ensureChannels(static_cast<size_t>(numChannels));

    float drive = std::pow(10.0f, driveDb_ / 20.0f);
    float outGain = std::pow(10.0f, outputDb_ / 20.0f);

    for (int frame = 0; frame < numFrames; ++frame) {
        for (int channel = 0; channel < numChannels; ++channel) {
            int index = frame * numChannels + channel;
            float input = buffer[index];
            float saturated = std::tanh(input * drive);
            float toned = toneFilters_[static_cast<size_t>(channel)].process(saturated);
            float processed = toned * outGain;
            buffer[index] = processed;
        }
    }
}

void AmpModelEffect::reset() {
    for (auto &filter : toneFilters_) {
        filter.reset();
    }
}

SoftClipperEffect::SoftClipperEffect()
    : drive_(2.0f)
    , mix_(0.5f)
{
}

void SoftClipperEffect::setDrive(float drive) {
    drive_ = std::clamp(drive, 1.0f, 20.0f);
}

void SoftClipperEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void SoftClipperEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0) return;
    if (mix_ <= 0.0f) return;
    float wet = mix_;
    float dry = 1.0f - wet;
    for (int i = 0, total = numFrames * numChannels; i < total; ++i) {
        float input = buffer[i];
        float x = input * drive_;
        float clipped = x / (1.0f + std::fabs(x));
        buffer[i] = input * dry + clipped * wet;
    }
}

FuzzEffect::FuzzEffect(float sampleRate)
    : sampleRate_(std::max(1.0f, sampleRate))
    , drive_(12.0f)
    , tone_(0.55f)
    , mix_(0.45f)
    , toneCoeff_(0.0f)
{
    updateTone();
}

void FuzzEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
    updateTone();
}

void FuzzEffect::setDrive(float drive) {
    drive_ = std::clamp(drive, 1.0f, 40.0f);
}

void FuzzEffect::setTone(float tone) {
    tone_ = std::clamp(tone, 0.0f, 1.0f);
    updateTone();
}

void FuzzEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void FuzzEffect::ensureChannels(size_t channels) {
    if (toneState_.size() == channels) return;
    toneState_.assign(channels, 0.0f);
}

void FuzzEffect::updateTone() {
    const float minCut = 500.0f;
    const float maxCut = 9000.0f;
    const float cutoff = minCut * std::pow(maxCut / minCut, tone_);
    toneCoeff_ = 1.0f - std::exp(-2.0f * kPi * cutoff / sampleRate_);
}

void FuzzEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0 || mix_ <= 0.0f) return;
    ensureChannels(static_cast<size_t>(numChannels));
    const float wet = mix_;
    const float dry = 1.0f - wet;

    for (int frame = 0; frame < numFrames; ++frame) {
        for (int channel = 0; channel < numChannels; ++channel) {
            const int index = frame * numChannels + channel;
            const float input = buffer[index];
            const float driven = input * drive_;
            const float folded = std::tanh(driven) - 0.18f * std::tanh(driven * 3.0f);
            float &state = toneState_[static_cast<size_t>(channel)];
            state += toneCoeff_ * (folded - state);
            buffer[index] = input * dry + state * wet;
        }
    }
}

void FuzzEffect::reset() {
    std::fill(toneState_.begin(), toneState_.end(), 0.0f);
}

TremoloEffect::TremoloEffect(float sampleRate)
    : sampleRate_(sampleRate)
    , rateHz_(4.0f)
    , depth_(0.5f)
    , phase_(0.0f)
{
}

void TremoloEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
}

void TremoloEffect::setRate(float rateHz) {
    rateHz_ = std::clamp(rateHz, 0.1f, 12.0f);
}

void TremoloEffect::setDepth(float depth) {
    depth_ = std::clamp(depth, 0.0f, 1.0f);
}

void TremoloEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0) return;
    if (depth_ <= 0.0f) return;
    float phaseInc = 2.0f * kPi * rateHz_ / sampleRate_;
    for (int frame = 0; frame < numFrames; ++frame) {
        float lfo = 0.5f * (1.0f + std::sin(phase_));
        float gain = (1.0f - depth_) + depth_ * lfo;
        for (int channel = 0; channel < numChannels; ++channel) {
            int index = frame * numChannels + channel;
            buffer[index] *= gain;
        }
        phase_ += phaseInc;
        if (phase_ > 2.0f * kPi) {
            phase_ -= 2.0f * kPi;
        }
    }
}

void TremoloEffect::reset() {
    phase_ = 0.0f;
}

ChorusEffect::ChorusEffect(float sampleRate)
    : sampleRate_(std::max(1.0f, sampleRate))
    , rateHz_(0.8f)
    , depthMs_(8.0f)
    , mix_(0.25f)
    , phase_(0.0f)
    , activeChannels_(2)
    , maxDelayFrames_(static_cast<size_t>(std::max(1.0f, sampleRate_ * 0.08f)))
    , writeFramePos_(0)
{
    buffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
}

void ChorusEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
    maxDelayFrames_ = static_cast<size_t>(std::max(1.0f, sampleRate_ * 0.08f));
    buffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
    writeFramePos_ = 0;
}

void ChorusEffect::setRate(float rateHz) {
    rateHz_ = std::clamp(rateHz, 0.05f, 8.0f);
}

void ChorusEffect::setDepthMs(float depthMs) {
    depthMs_ = std::clamp(depthMs, 0.1f, 20.0f);
}

void ChorusEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void ChorusEffect::ensureChannels(size_t channels) {
    if (channels == activeChannels_) return;
    activeChannels_ = std::max<size_t>(1, channels);
    buffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
    writeFramePos_ = 0;
}

float ChorusEffect::readDelay(float delayFrames, int channel) const {
    const float readPos = static_cast<float>(writeFramePos_) - delayFrames;
    float wrapped = std::fmod(readPos, static_cast<float>(maxDelayFrames_));
    if (wrapped < 0.0f) wrapped += static_cast<float>(maxDelayFrames_);
    const size_t index0 = static_cast<size_t>(wrapped) % maxDelayFrames_;
    const size_t index1 = (index0 + 1) % maxDelayFrames_;
    const float frac = wrapped - static_cast<float>(index0);
    const size_t ch = static_cast<size_t>(channel);
    const float a = buffer_[index0 * activeChannels_ + ch];
    const float b = buffer_[index1 * activeChannels_ + ch];
    return a + (b - a) * frac;
}

void ChorusEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0 || mix_ <= 0.0f) return;
    ensureChannels(static_cast<size_t>(numChannels));
    const float wet = mix_;
    const float dry = 1.0f - wet;
    const float phaseInc = 2.0f * kPi * rateHz_ / sampleRate_;
    const float baseDelayFrames = sampleRate_ * 0.012f;
    const float depthFrames = sampleRate_ * depthMs_ / 1000.0f;

    for (int frame = 0; frame < numFrames; ++frame) {
        for (int channel = 0; channel < numChannels; ++channel) {
            const int index = frame * numChannels + channel;
            const float input = buffer[index];
            const float channelPhase = phase_ + static_cast<float>(channel) * kPi * 0.5f;
            const float lfo = 0.5f + 0.5f * std::sin(channelPhase);
            const float delayed = readDelay(baseDelayFrames + depthFrames * lfo, channel);
            buffer_[writeFramePos_ * activeChannels_ + static_cast<size_t>(channel)] = input;
            buffer[index] = input * dry + delayed * wet;
        }
        writeFramePos_ = (writeFramePos_ + 1) % maxDelayFrames_;
        phase_ += phaseInc;
        if (phase_ > 2.0f * kPi) phase_ -= 2.0f * kPi;
    }
}

void ChorusEffect::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writeFramePos_ = 0;
    phase_ = 0.0f;
}

PhaserEffect::PhaserEffect(float sampleRate)
    : sampleRate_(std::max(1.0f, sampleRate))
    , rateHz_(0.6f)
    , depth_(0.6f)
    , feedback_(0.25f)
    , mix_(0.35f)
    , phase_(0.0f)
{
}

void PhaserEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
}

void PhaserEffect::setRate(float rateHz) {
    rateHz_ = std::clamp(rateHz, 0.05f, 8.0f);
}

void PhaserEffect::setDepth(float depth) {
    depth_ = std::clamp(depth, 0.0f, 1.0f);
}

void PhaserEffect::setFeedback(float feedback) {
    feedback_ = std::clamp(feedback, 0.0f, 0.95f);
}

void PhaserEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void PhaserEffect::ensureChannels(size_t channels) {
    if (channelStates_.size() == channels) return;
    channelStates_.assign(channels, ChannelState{});
}

void PhaserEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0 || mix_ <= 0.0f) return;
    ensureChannels(static_cast<size_t>(numChannels));
    const float phaseInc = 2.0f * kPi * rateHz_ / sampleRate_;
    const float wet = mix_;
    const float dry = 1.0f - wet;

    for (int frame = 0; frame < numFrames; ++frame) {
        const float sweep = 0.5f + 0.5f * std::sin(phase_);
        const float center = 250.0f + depth_ * sweep * 2600.0f;
        const float wc = std::tan(kPi * std::clamp(center, 40.0f, sampleRate_ * 0.45f) / sampleRate_);
        const float coeff = (1.0f - wc) / (1.0f + wc);

        for (int channel = 0; channel < numChannels; ++channel) {
            const int index = frame * numChannels + channel;
            const float input = buffer[index];
            ChannelState &state = channelStates_[static_cast<size_t>(channel)];
            float x = input + state.feedbackSample * feedback_;
            for (size_t stage = 0; stage < state.x1.size(); ++stage) {
                const float y = -coeff * x + state.x1[stage] + coeff * state.y1[stage];
                state.x1[stage] = x;
                state.y1[stage] = y;
                x = y;
            }
            state.feedbackSample = x;
            buffer[index] = input * dry + x * wet;
        }

        phase_ += phaseInc;
        if (phase_ > 2.0f * kPi) phase_ -= 2.0f * kPi;
    }
}

void PhaserEffect::reset() {
    for (auto &state : channelStates_) {
        state = ChannelState{};
    }
    phase_ = 0.0f;
}

BitCrusherEffect::BitCrusherEffect()
    : bitDepth_(8.0f)
    , sampleRateReduction_(4.0f)
    , mix_(0.5f)
    , holdCounter_(0)
{
}

void BitCrusherEffect::setBitDepth(float bitDepth) {
    bitDepth_ = std::clamp(bitDepth, 1.0f, 16.0f);
}

void BitCrusherEffect::setSampleRateReduction(float reduction) {
    sampleRateReduction_ = std::clamp(reduction, 1.0f, 64.0f);
}

void BitCrusherEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void BitCrusherEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0 || mix_ <= 0.0f) return;
    if (heldSamples_.size() != static_cast<size_t>(numChannels)) {
        heldSamples_.assign(static_cast<size_t>(numChannels), 0.0f);
        holdCounter_ = 0;
    }

    const int holdFrames = std::max(1, static_cast<int>(std::round(sampleRateReduction_)));
    const float levels = std::max(1.0f, std::pow(2.0f, std::round(bitDepth_)) - 1.0f);
    const float wet = mix_;
    const float dry = 1.0f - wet;

    for (int frame = 0; frame < numFrames; ++frame) {
        if (holdCounter_ <= 0) {
            for (int channel = 0; channel < numChannels; ++channel) {
                const int index = frame * numChannels + channel;
                const float clipped = std::clamp(buffer[index], -1.0f, 1.0f);
                heldSamples_[static_cast<size_t>(channel)] =
                    std::round((clipped + 1.0f) * 0.5f * levels) / levels * 2.0f - 1.0f;
            }
            holdCounter_ = holdFrames;
        }

        for (int channel = 0; channel < numChannels; ++channel) {
            const int index = frame * numChannels + channel;
            buffer[index] = buffer[index] * dry + heldSamples_[static_cast<size_t>(channel)] * wet;
        }
        --holdCounter_;
    }
}

void BitCrusherEffect::reset() {
    holdCounter_ = 0;
    std::fill(heldSamples_.begin(), heldSamples_.end(), 0.0f);
}

GranulatorEffect::GranulatorEffect(float sampleRate)
    : sampleRate_(std::max(1.0f, sampleRate))
    , grainSizeMs_(60.0f)
    , texture_(0.35f)
    , mix_(0.25f)
    , activeChannels_(2)
    , maxDelayFrames_(static_cast<size_t>(std::max(1.0f, sampleRate_ * 2.0f)))
    , writeFramePos_(0)
    , grainPhase_(0)
    , grainSizeFrames_(1)
    , grainStartA_(0)
    , grainStartB_(0)
    , randomState_(0x6d2b79f5u)
{
    buffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
    updateGrainSize();
    grainStartA_ = chooseGrainOffset();
    grainStartB_ = chooseGrainOffset();
}

void GranulatorEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
    maxDelayFrames_ = static_cast<size_t>(std::max(1.0f, sampleRate_ * 2.0f));
    buffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
    writeFramePos_ = 0;
    grainPhase_ = 0;
    updateGrainSize();
    grainStartA_ = chooseGrainOffset();
    grainStartB_ = chooseGrainOffset();
}

void GranulatorEffect::setGrainSizeMs(float grainSizeMs) {
    grainSizeMs_ = std::clamp(grainSizeMs, 10.0f, 250.0f);
    updateGrainSize();
}

void GranulatorEffect::setTexture(float texture) {
    texture_ = std::clamp(texture, 0.0f, 1.0f);
}

void GranulatorEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void GranulatorEffect::ensureChannels(size_t channels) {
    if (channels == activeChannels_) return;
    activeChannels_ = std::max<size_t>(1, channels);
    buffer_.assign(maxDelayFrames_ * activeChannels_, 0.0f);
    writeFramePos_ = 0;
    grainPhase_ = 0;
}

void GranulatorEffect::updateGrainSize() {
    grainSizeFrames_ = std::max(1, static_cast<int>(sampleRate_ * grainSizeMs_ / 1000.0f));
    grainSizeFrames_ = std::min(grainSizeFrames_, static_cast<int>(std::max<size_t>(1, maxDelayFrames_ / 2)));
}

size_t GranulatorEffect::chooseGrainOffset() {
    randomState_ = randomState_ * 1664525u + 1013904223u;
    const float random = static_cast<float>((randomState_ >> 8) & 0x00ffffffu) /
                         static_cast<float>(0x01000000u);
    const float baseFrames = static_cast<float>(grainSizeFrames_) * (1.0f + texture_ * 6.0f);
    const float spreadFrames = static_cast<float>(grainSizeFrames_) * texture_ * 8.0f;
    const size_t offset = static_cast<size_t>(std::max(1.0f, baseFrames + random * spreadFrames));
    return std::min(offset, maxDelayFrames_ - 1);
}

float GranulatorEffect::readGrain(size_t startFrame, int phase, int channel, int numChannels) const {
    const size_t offset = (startFrame + static_cast<size_t>(phase)) % maxDelayFrames_;
    const size_t readFrame = (writeFramePos_ + maxDelayFrames_ - offset) % maxDelayFrames_;
    return buffer_[readFrame * activeChannels_ + static_cast<size_t>(channel % numChannels)];
}

void GranulatorEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0 || mix_ <= 0.0f) return;
    ensureChannels(static_cast<size_t>(numChannels));
    const float wet = mix_;
    const float dry = 1.0f - wet;

    for (int frame = 0; frame < numFrames; ++frame) {
        const float phaseNorm = static_cast<float>(grainPhase_) /
                                static_cast<float>(std::max(1, grainSizeFrames_));
        const float windowA = 0.5f - 0.5f * std::cos(2.0f * kPi * phaseNorm);
        const int phaseB = (grainPhase_ + grainSizeFrames_ / 2) % grainSizeFrames_;
        const float phaseNormB = static_cast<float>(phaseB) /
                                 static_cast<float>(std::max(1, grainSizeFrames_));
        const float windowB = 0.5f - 0.5f * std::cos(2.0f * kPi * phaseNormB);

        for (int channel = 0; channel < numChannels; ++channel) {
            const int index = frame * numChannels + channel;
            const float input = buffer[index];
            const float wetSample =
                (readGrain(grainStartA_, grainPhase_, channel, numChannels) * windowA +
                 readGrain(grainStartB_, phaseB, channel, numChannels) * windowB) /
                std::max(0.001f, windowA + windowB);
            buffer_[writeFramePos_ * activeChannels_ + static_cast<size_t>(channel)] = input;
            buffer[index] = input * dry + wetSample * wet;
        }

        writeFramePos_ = (writeFramePos_ + 1) % maxDelayFrames_;
        ++grainPhase_;
        if (grainPhase_ >= grainSizeFrames_) {
            grainPhase_ = 0;
            grainStartA_ = chooseGrainOffset();
            grainStartB_ = chooseGrainOffset();
        }
    }
}

void GranulatorEffect::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writeFramePos_ = 0;
    grainPhase_ = 0;
}

float ReverbEffect::CombFilter::process(float input) {
    if (buffer.empty()) return input;
    float output = buffer[index];
    filterStore = output + (filterStore - output) * damp;
    buffer[index] = input + filterStore * feedback;
    index = (index + 1) % buffer.size();
    return output;
}

void ReverbEffect::CombFilter::reset() {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    index = 0;
    filterStore = 0.0f;
}

float ReverbEffect::AllpassFilter::process(float input) {
    if (buffer.empty()) return input;
    float bufOut = buffer[index];
    float output = -input + bufOut;
    buffer[index] = input + bufOut * feedback;
    index = (index + 1) % buffer.size();
    return output;
}

void ReverbEffect::AllpassFilter::reset() {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    index = 0;
}

ReverbEffect::ReverbEffect(float sampleRate)
    : sampleRate_(sampleRate)
    , roomSize_(0.5f)
    , damping_(0.3f)
    , mix_(0.25f)
    , activeChannels_(0)
{
}

void ReverbEffect::setSampleRate(float sampleRate) {
    sampleRate_ = std::max(1.0f, sampleRate);
    configureFilters();
}

void ReverbEffect::setRoomSize(float roomSize) {
    roomSize_ = std::clamp(roomSize, 0.0f, 1.0f);
    configureFilters();
}

void ReverbEffect::setDamping(float damping) {
    damping_ = std::clamp(damping, 0.0f, 1.0f);
    configureFilters();
}

void ReverbEffect::setMix(float mix) {
    mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void ReverbEffect::ensureChannels(size_t channels) {
    if (activeChannels_ == channels) return;
    activeChannels_ = channels;
    combs_.assign(channels, std::array<CombFilter, 4>{});
    allpasses_.assign(channels, std::array<AllpassFilter, 2>{});
    configureFilters();
}

void ReverbEffect::configureFilters() {
    if (activeChannels_ == 0) return;
    float scale = sampleRate_ / 44100.0f;
    std::array<int, 4> combSizes = { 1116, 1188, 1277, 1356 };
    std::array<int, 2> allpassSizes = { 556, 441 };
    float feedback = 0.7f + roomSize_ * 0.25f;
    float damp = damping_ * 0.8f;

    for (size_t ch = 0; ch < activeChannels_; ++ch) {
        for (size_t i = 0; i < combs_[ch].size(); ++i) {
            size_t size = static_cast<size_t>(std::max(1.0f, combSizes[i] * scale));
            combs_[ch][i].buffer.assign(size, 0.0f);
            combs_[ch][i].index = 0;
            combs_[ch][i].feedback = feedback;
            combs_[ch][i].damp = damp;
            combs_[ch][i].filterStore = 0.0f;
        }
        for (size_t i = 0; i < allpasses_[ch].size(); ++i) {
            size_t size = static_cast<size_t>(std::max(1.0f, allpassSizes[i] * scale));
            allpasses_[ch][i].buffer.assign(size, 0.0f);
            allpasses_[ch][i].index = 0;
            allpasses_[ch][i].feedback = 0.5f;
        }
    }
}

void ReverbEffect::process(float* buffer, int numFrames, int numChannels) {
    if (!buffer || numFrames <= 0 || numChannels <= 0) return;
    if (mix_ <= 0.0f) return;
    ensureChannels(static_cast<size_t>(numChannels));
    float wet = mix_;
    float dry = 1.0f - wet;

    for (int frame = 0; frame < numFrames; ++frame) {
        for (int channel = 0; channel < numChannels; ++channel) {
            int index = frame * numChannels + channel;
            float input = buffer[index];

            float acc = 0.0f;
            for (auto &comb : combs_[static_cast<size_t>(channel)]) {
                acc += comb.process(input);
            }
            for (auto &allpass : allpasses_[static_cast<size_t>(channel)]) {
                acc = allpass.process(acc);
            }
            float processed = acc * 0.25f;
            buffer[index] = input * dry + processed * wet;
        }
    }
}

void ReverbEffect::reset() {
    for (auto &channelCombs : combs_) {
        for (auto &comb : channelCombs) {
            comb.reset();
        }
    }
    for (auto &channelAllpasses : allpasses_) {
        for (auto &allpass : channelAllpasses) {
            allpass.reset();
        }
    }
}

EffectsProcessor::EffectsProcessor(float sampleRate)
    : delay_(sampleRate)
    , delayEnabled_(false)
    , amp_(sampleRate)
    , ampEnabled_(false)
    , softClip_()
    , softClipEnabled_(false)
    , fuzz_(sampleRate)
    , fuzzEnabled_(false)
    , tremolo_(sampleRate)
    , tremoloEnabled_(false)
    , chorus_(sampleRate)
    , chorusEnabled_(false)
    , phaser_(sampleRate)
    , phaserEnabled_(false)
    , bitCrusher_()
    , bitCrusherEnabled_(false)
    , granulator_(sampleRate)
    , granulatorEnabled_(false)
    , reverb_(sampleRate)
    , reverbEnabled_(false)
    , sampleRate_(sampleRate)
    , delayPingPongEnabled_(false)
    , effectOrder_(EffectOrder::DriveModDelayReverb)
{
}

void EffectsProcessor::setSampleRate(float sampleRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    sampleRate_ = sampleRate;
    // Reinitialize delay with new sample rate
    delay_.~DelayEffect();
    new (&delay_) DelayEffect(sampleRate);
    delay_.setPingPong(delayPingPongEnabled_);
    amp_.setSampleRate(sampleRate);
    fuzz_.setSampleRate(sampleRate);
    tremolo_.setSampleRate(sampleRate);
    chorus_.setSampleRate(sampleRate);
    phaser_.setSampleRate(sampleRate);
    granulator_.setSampleRate(sampleRate);
    reverb_.setSampleRate(sampleRate);
}

void EffectsProcessor::setDelayEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    delayEnabled_ = enabled;
    if (!enabled) {
        delay_.reset();
    }
}

void EffectsProcessor::setDelayTime(float seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.setDelayTime(seconds);
}

void EffectsProcessor::setDelayDecay(float decay) {
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.setDecay(decay);
}

void EffectsProcessor::setDelayMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.setMix(mix);
}

void EffectsProcessor::setDelayPingPong(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    delayPingPongEnabled_ = enabled;
    delay_.setPingPong(enabled);
}

void EffectsProcessor::setEffectOrder(EffectOrder order) {
    std::lock_guard<std::mutex> lock(mutex_);
    effectOrder_ = order;
}

void EffectsProcessor::setAmpEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    ampEnabled_ = enabled;
    if (!enabled) {
        amp_.reset();
    }
}

void EffectsProcessor::setAmpDriveDb(float driveDb) {
    std::lock_guard<std::mutex> lock(mutex_);
    amp_.setDriveDb(driveDb);
}

void EffectsProcessor::setAmpTone(float tone) {
    std::lock_guard<std::mutex> lock(mutex_);
    amp_.setTone(tone);
}

void EffectsProcessor::setAmpOutputDb(float outputDb) {
    std::lock_guard<std::mutex> lock(mutex_);
    amp_.setOutputDb(outputDb);
}

void EffectsProcessor::setSoftClipEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    softClipEnabled_ = enabled;
}

void EffectsProcessor::setSoftClipDrive(float drive) {
    std::lock_guard<std::mutex> lock(mutex_);
    softClip_.setDrive(drive);
}

void EffectsProcessor::setSoftClipMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    softClip_.setMix(mix);
}

void EffectsProcessor::setFuzzEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    fuzzEnabled_ = enabled;
    if (!enabled) {
        fuzz_.reset();
    }
}

void EffectsProcessor::setFuzzDrive(float drive) {
    std::lock_guard<std::mutex> lock(mutex_);
    fuzz_.setDrive(drive);
}

void EffectsProcessor::setFuzzTone(float tone) {
    std::lock_guard<std::mutex> lock(mutex_);
    fuzz_.setTone(tone);
}

void EffectsProcessor::setFuzzMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    fuzz_.setMix(mix);
}

void EffectsProcessor::setTremoloEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    tremoloEnabled_ = enabled;
    if (!enabled) {
        tremolo_.reset();
    }
}

void EffectsProcessor::setTremoloRate(float rateHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    tremolo_.setRate(rateHz);
}

void EffectsProcessor::setTremoloDepth(float depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    tremolo_.setDepth(depth);
}

void EffectsProcessor::setChorusEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    chorusEnabled_ = enabled;
    if (!enabled) {
        chorus_.reset();
    }
}

void EffectsProcessor::setChorusRate(float rateHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    chorus_.setRate(rateHz);
}

void EffectsProcessor::setChorusDepthMs(float depthMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    chorus_.setDepthMs(depthMs);
}

void EffectsProcessor::setChorusMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    chorus_.setMix(mix);
}

void EffectsProcessor::setPhaserEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    phaserEnabled_ = enabled;
    if (!enabled) {
        phaser_.reset();
    }
}

void EffectsProcessor::setPhaserRate(float rateHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    phaser_.setRate(rateHz);
}

void EffectsProcessor::setPhaserDepth(float depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    phaser_.setDepth(depth);
}

void EffectsProcessor::setPhaserFeedback(float feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    phaser_.setFeedback(feedback);
}

void EffectsProcessor::setPhaserMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    phaser_.setMix(mix);
}

void EffectsProcessor::setBitCrusherEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    bitCrusherEnabled_ = enabled;
    if (!enabled) {
        bitCrusher_.reset();
    }
}

void EffectsProcessor::setBitCrusherBitDepth(float bitDepth) {
    std::lock_guard<std::mutex> lock(mutex_);
    bitCrusher_.setBitDepth(bitDepth);
}

void EffectsProcessor::setBitCrusherSampleRateReduction(float reduction) {
    std::lock_guard<std::mutex> lock(mutex_);
    bitCrusher_.setSampleRateReduction(reduction);
}

void EffectsProcessor::setBitCrusherMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    bitCrusher_.setMix(mix);
}

void EffectsProcessor::setGranulatorEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    granulatorEnabled_ = enabled;
    if (!enabled) {
        granulator_.reset();
    }
}

void EffectsProcessor::setGranulatorGrainSizeMs(float grainSizeMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    granulator_.setGrainSizeMs(grainSizeMs);
}

void EffectsProcessor::setGranulatorTexture(float texture) {
    std::lock_guard<std::mutex> lock(mutex_);
    granulator_.setTexture(texture);
}

void EffectsProcessor::setGranulatorMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    granulator_.setMix(mix);
}

void EffectsProcessor::setReverbEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    reverbEnabled_ = enabled;
    if (!enabled) {
        reverb_.reset();
    }
}

void EffectsProcessor::setReverbRoomSize(float roomSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    reverb_.setRoomSize(roomSize);
}

void EffectsProcessor::setReverbDamping(float damping) {
    std::lock_guard<std::mutex> lock(mutex_);
    reverb_.setDamping(damping);
}

void EffectsProcessor::setReverbMix(float mix) {
    std::lock_guard<std::mutex> lock(mutex_);
    reverb_.setMix(mix);
}

void EffectsProcessor::process(float* buffer, int numFrames, int numChannels) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!(ampEnabled_ || softClipEnabled_ || fuzzEnabled_ || tremoloEnabled_ ||
          chorusEnabled_ || phaserEnabled_ || bitCrusherEnabled_ ||
          granulatorEnabled_ || delayEnabled_ || reverbEnabled_)) {
        return;
    }

    switch (effectOrder_) {
    case EffectOrder::DriveModDelayReverb:
        if (ampEnabled_) amp_.process(buffer, numFrames, numChannels);
        if (softClipEnabled_) softClip_.process(buffer, numFrames, numChannels);
        if (fuzzEnabled_) fuzz_.process(buffer, numFrames, numChannels);
        if (tremoloEnabled_) tremolo_.process(buffer, numFrames, numChannels);
        if (phaserEnabled_) phaser_.process(buffer, numFrames, numChannels);
        if (chorusEnabled_) chorus_.process(buffer, numFrames, numChannels);
        if (bitCrusherEnabled_) bitCrusher_.process(buffer, numFrames, numChannels);
        if (granulatorEnabled_) granulator_.process(buffer, numFrames, numChannels);
        if (delayEnabled_) delay_.process(buffer, numFrames, numChannels);
        if (reverbEnabled_) reverb_.process(buffer, numFrames, numChannels);
        break;
    case EffectOrder::DelayReverbDriveMod:
        if (delayEnabled_) delay_.process(buffer, numFrames, numChannels);
        if (reverbEnabled_) reverb_.process(buffer, numFrames, numChannels);
        if (ampEnabled_) amp_.process(buffer, numFrames, numChannels);
        if (softClipEnabled_) softClip_.process(buffer, numFrames, numChannels);
        if (fuzzEnabled_) fuzz_.process(buffer, numFrames, numChannels);
        if (tremoloEnabled_) tremolo_.process(buffer, numFrames, numChannels);
        if (phaserEnabled_) phaser_.process(buffer, numFrames, numChannels);
        if (chorusEnabled_) chorus_.process(buffer, numFrames, numChannels);
        if (bitCrusherEnabled_) bitCrusher_.process(buffer, numFrames, numChannels);
        if (granulatorEnabled_) granulator_.process(buffer, numFrames, numChannels);
        break;
    }
}

void EffectsProcessor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.reset();
    amp_.reset();
    fuzz_.reset();
    tremolo_.reset();
    chorus_.reset();
    phaser_.reset();
    bitCrusher_.reset();
    granulator_.reset();
    reverb_.reset();
}
