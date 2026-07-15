#include "effects.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {

std::vector<float> makeSignal(int frames, int channels)
{
    std::vector<float> signal(static_cast<size_t>(frames * channels));
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = 0.35f * std::sin(2.0f * 3.14159265358979323846f
                                             * 440.0f * frame / 48000.0f);
        for (int channel = 0; channel < channels; ++channel) {
            signal[static_cast<size_t>(frame * channels + channel)] = sample;
        }
    }
    return signal;
}

void requireFiniteAndChanged(EffectsProcessor& effects)
{
    auto input = makeSignal(4096, 2);
    auto output = input;
    effects.process(output.data(), 4096, 2);
    assert(std::all_of(output.begin(), output.end(), [](float value) {
        return std::isfinite(value);
    }));
    assert(output != input);
}

} // namespace

int main()
{
    EffectsProcessor effects(48000.0f);

    effects.setAmpEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setAmpEnabled(false);

    effects.setSoftClipEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setSoftClipEnabled(false);

    effects.setFuzzEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setFuzzEnabled(false);

    effects.setTremoloEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setTremoloEnabled(false);

    effects.setChorusEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setChorusEnabled(false);

    effects.setPhaserEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setPhaserEnabled(false);

    effects.setBitCrusherEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setBitCrusherEnabled(false);

    effects.setGranulatorEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setGranulatorEnabled(false);

    effects.setReverbEnabled(true);
    requireFiniteAndChanged(effects);
    effects.setReverbEnabled(false);

    effects.setDelayEnabled(true);
    effects.setDelayTime(0.01f);
    requireFiniteAndChanged(effects);
    effects.setDelayPingPong(true);
    effects.setEffectOrder(EffectsProcessor::EffectOrder::DelayReverbDriveMod);
    requireFiniteAndChanged(effects);
}
