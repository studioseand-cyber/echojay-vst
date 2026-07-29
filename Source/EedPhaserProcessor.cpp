/*
    EedPhaserProcessor.cpp  —  see EedPhaserProcessor.h.
*/

#include "EedPhaserProcessor.h"
#include "EedPhaserEditor.h"
#include "EedDeviceRegistry.h"
#include "EedModTempo.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedPhaserProcessor::schema()
{
    using L = echojay::LfoCore;
    using P = echojay::PhaserEngine;

    static const echojay::ParamSchema s ({
        { kRateHz, "Hz",
          (double) L::kMinRateHz, (double) L::kMaxRateHz, 0.3,
          "how fast the notches sweep; ignored while sync is on", false },

        { kSync, "",
          0.0, 1.0, 0.0,
          "lock the rate to the host tempo and use sync_division instead", true },

        { kDivision, "",
          0.0, (double) (L::kNumDivisions - 1), (double) L::kDefaultDivision,
          "tempo division while sync is on: 0 = 4/1 slowest, 6 = 1/4, "
          "12 = 1/32 fastest", false },

        { kDepth, "%",
          (double) L::kMinDepth, (double) L::kMaxDepth, 70.0,
          "how far the notches travel, up to two octaves either side of "
          "center_freq at 100", false },

        { kStages, "",
          (double) P::kMinStages, (double) P::kMaxStages, 6.0,
          "how many allpass stages, i.e. how many notches: 2-4 is a gentle "
          "sweep, 8-12 is thick and vocal", false },

        { kFeedback, "%",
          (double) P::kMinFeedback, (double) P::kMaxFeedback, 30.0,
          "how much of the phased signal is fed back; higher makes the notches "
          "sharper and more resonant, negative shifts where they sit", false },

        // US spelling deliberately: this is the id the model is taught and will
        // send. A knob's id is a wire format, not prose.
        { kCentreFreq, "Hz",
          (double) P::kMinCentreHz, (double) P::kMaxCentreHz, 800.0,
          "the frequency the sweep is centred on; lower is a deeper, "
          "warmer phase, higher is thinner and more obvious", false },

        { kMix, "%",
          (double) P::kMinMix, (double) P::kMaxMix, 100.0,
          "how much of the phased signal is heard; the notches come from it "
          "meeting the dry signal, so 100 is the full effect and 0 is bypass",
          false },
    });
    return s;
}

bool EedPhaserProcessor::setParamValue (const juce::String& id, double value)
{
    auto& lfo = engine_.lfo();

    if (id == kRateHz)     { lfo.setRateHz              ((float) value); return true; }
    if (id == kSync)       { lfo.setTempoSync           (value >= 0.5);  return true; }
    if (id == kDivision)   { lfo.setDivisionIndex       ((int) std::lround (value)); return true; }
    if (id == kDepth)      { lfo.setDepthPercent        ((float) value); return true; }
    if (id == kStages)     { engine_.setStages          ((int) std::lround (value)); return true; }
    if (id == kFeedback)   { engine_.setFeedbackPercent ((float) value); return true; }
    if (id == kCentreFreq) { engine_.setCentreHz        ((float) value); return true; }
    if (id == kMix)        { engine_.setMixPercent      ((float) value); return true; }
    return false;
}

double EedPhaserProcessor::getParamValue (const juce::String& id) const
{
    const auto& lfo = engine_.lfo();

    if (id == kRateHz)     return (double) lfo.getRateHz();
    if (id == kSync)       return lfo.getTempoSync() ? 1.0 : 0.0;
    if (id == kDivision)   return (double) lfo.getDivisionIndex();
    if (id == kDepth)      return (double) lfo.getDepthPercent();
    if (id == kStages)     return (double) engine_.getStages();
    if (id == kFeedback)   return (double) engine_.getFeedbackPercent();
    if (id == kCentreFreq) return (double) engine_.getCentreHz();
    if (id == kMix)        return (double) engine_.getMixPercent();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedPhaserProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    engine_.reset();
}

void EedPhaserProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    echojay::pushHostTempo (*this, engine_.lfo());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedPhaserProcessor::createEditor()
{
    return new EedPhaserEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makePhaserDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Phaser";
        d.category        = "Modulation";
        d.descriptiveName = "EchoJay phaser (built in)";
        // ASCII ONLY - see the note in DeviceTemplate/README.md.
        d.summary         = "Sweeping notches from a chain of allpass filters. Reach "
                            "for it to give a sustained part motion and a hollow, "
                            "swirling character - keys, guitars, pads - where a chorus "
                            "would thicken instead of animate.";
        d.identifier      = "echojay:builtin:phaser";
        d.uid             = 0x456A5048;   // 'EjPH' - frozen once shipped
        d.aliases         = { "EchoJayPhaser", "Phaser" };
        d.schema          = EedPhaserProcessor::schema();
        d.create          = [] { return std::make_unique<EedPhaserProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar phaserRegistrar { makePhaserDevice() };
}
