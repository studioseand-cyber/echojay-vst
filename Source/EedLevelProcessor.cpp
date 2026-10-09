#include "EedLevelProcessor.h"
#include "EedLevelEditor.h"
#include "EedDeviceRegistry.h"
#include <cmath>

const echojay::ParamSchema& EedLevelProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kGainDb, "dB", kMinDb, kMaxDb, 0.0,
          "clean gain before the final limiter; the level loop sets this to reach the LUFS target, never the limiter", false },
        { kTargetLufs, "LUFS", -30.0, 0.0, -9.0,
          "the integrated LUFS target the level loop drives toward (informational; set by the server's loudness pass)", false },
        { kLoudnessOption, "", 0.0, 4.0, 0.0,
          "which loudness the target came from; \"match\" means the aim is this chain's own input level",
          false, { "commercial", "pushed", "dynamic", "keep", "match" } },
    });
    return s;
}
bool EedLevelProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kGainDb)     { gainDb_.store (juce::jlimit (kMinDb, kMaxDb, value), std::memory_order_relaxed); return true; }
    if (id == kTargetLufs) { targetLufs_.store (juce::jlimit (-30.0, 0.0, value), std::memory_order_relaxed); return true; }
    if (id == kLoudnessOption) { option_.store (juce::jlimit (0, kOptionMatch, (int) std::lround (value)), std::memory_order_relaxed); return true; }
    return false;
}
double EedLevelProcessor::getParamValue (const juce::String& id) const
{
    if (id == kGainDb) return gainDb();
    if (id == kTargetLufs) return targetLufs();
    if (id == kLoudnessOption) return (double) loudnessOption();
    return 0.0;
}
void EedLevelProcessor::prepareToPlay (double sampleRate, int)
{
    in_.prepare (sampleRate); out_.prepare (sampleRate);
    curLin_ = (float) std::pow (10.0, gainDb() / 20.0);
    ramp_   = (float) (1.0 - std::exp (-1.0 / (0.02 * sampleRate)));   // 20 ms toward the target, per sample
}
void EedLevelProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch) buffer.clear (ch, 0, buffer.getNumSamples());
    const int n = buffer.getNumSamples(); const int nch = juce::jmin (2, buffer.getNumChannels());
    if (n <= 0 || nch <= 0) return;
    in_.push (buffer.getReadPointer (0), nch > 1 ? buffer.getReadPointer (1) : nullptr, n);
    if (! isBypassed())
    {
        const float target = (float) std::pow (10.0, gainDb() / 20.0);
        float* l = buffer.getWritePointer (0); float* r = nch > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            curLin_ += (target - curLin_) * ramp_;
            l[i] *= curLin_; if (r != nullptr) r[i] *= curLin_;
        }
    }
    out_.push (buffer.getReadPointer (0), nch > 1 ? buffer.getReadPointer (1) : nullptr, n);
}
juce::AudioProcessorEditor* EedLevelProcessor::createEditor() { return new EedLevelEditor (*this); }

namespace
{
    BuiltinDevice makeLevelDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Level";
        d.category        = "Utility";
        d.descriptiveName = "EchoJay level stage for the loudness loop (built in)";
        d.summary         = "A clean gain stage the level loop drives, placed just before the final limiter of a loudness-target "
                            "chain. It meters its input and its output (short-term LUFS and true peak) so the level move is "
                            "visible on the card; the limiter after it only holds the ceiling.";
        d.identifier      = "echojay:builtin:level";
        d.uid             = 0x456A4C56;   // 'EjLV' - frozen once shipped
        d.aliases         = { "EchoJayLevel", "EchoJay Loudness Level" };
        d.schema          = EedLevelProcessor::schema();
        d.create          = [] { return std::make_unique<EedLevelProcessor>(); };
        return d;
    }
    const BuiltinDeviceRegistrar levelRegistrar { makeLevelDevice() };
}
