/*
    EedExpanderProcessor.cpp  —  see EedExpanderProcessor.h.
*/

#include "EedExpanderProcessor.h"
#include "EedLatencyLog.h"
#include "EedExpanderEditor.h"
#include "EedDeviceRegistry.h"

EedExpanderProcessor::EedExpanderProcessor()
{
    core_.setMode (echojay::DynamicsMode::Expand);
    core_.setDetectorMode (echojay::DetectorMode::Peak);

    // A soft knee by default, and not dialable here: the expander publishes five
    // knobs by spec, and a hard-cornered expander is the version that sounds
    // like a gate. 6 dB is the width that keeps the slope engaging gradually.
    //
    // The constant is published (kFixedKneeDb) because the editor draws the
    // curve from it too; a literal here and a literal there is a picture that
    // stops matching the sound the first time one of them is changed.
    core_.setKneeDb (kFixedKneeDb);

    core_.setMaxLookaheadMs (kMaxLookaheadMs);

    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedExpanderProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kThresholdDb, "dB", -80.0, 0.0, -40.0,
          "level below which the signal is pushed further down; anything above "
          "it passes untouched", false },

        { kRatio, "", 1.0, 20.0, 2.0,
          "how steeply quiet material is pushed down; 2 is a gentle tidy-up, 8+ "
          "approaches gating", false },

        { kAttackMs, "ms", 0.05, 100.0, 2.0,
          "how fast it recovers to unity when the signal rises above the "
          "threshold", false },

        { kReleaseMs, "ms", 5.0, 3000.0, 200.0,
          "how fast it pushes down again once the signal falls away; long is "
          "natural, short is abrupt", false },

        { kRangeDb, "dB", 0.0, 60.0, 20.0,
          "the deepest reduction it is allowed to apply; this is what keeps an "
          "expander from fading quiet passages to nothing", false },

        // ---- the depth pass ------------------------------------------------
        { kScHpfHz, "Hz", 0.0, 500.0, 0.0,
          "high-pass on the DETECTOR only, never on the audio: low-frequency "
          "rumble stops holding the expander open through the pauses you are "
          "trying to clean up. 0 is off", false },

        { kLookaheadMs, "ms", 0.0, kMaxLookaheadMs, 0.0,
          "how far ahead it looks, so it is already back at unity when a note "
          "arrives instead of softening its front; this is added latency, reported "
          "to the host", false },
    });
    return s;
}

bool EedExpanderProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kThresholdDb) { core_.setThresholdDb ((float) value); return true; }
    if (id == kRatio)       { core_.setRatio       ((float) value); return true; }
    if (id == kAttackMs)    { core_.setAttackMs    (value);         return true; }
    if (id == kReleaseMs)   { core_.setReleaseMs   (value);         return true; }
    if (id == kRangeDb)     { core_.setRangeDb     ((float) value); return true; }
    if (id == kScHpfHz)     { core_.setSidechainHpfHz (value);      return true; }

    if (id == kLookaheadMs)
    {
        lookaheadMs_ = value;
        core_.setLookaheadMs (value);
        ejSetLatencyLogged (*this, core_.lookaheadSamples(), "EedExpanderProcessor #1");
        return true;
    }
    return false;
}

double EedExpanderProcessor::getParamValue (const juce::String& id) const
{
    if (id == kThresholdDb) return (double) core_.getThresholdDb();
    if (id == kRatio)       return (double) core_.getRatio();
    if (id == kAttackMs)    return core_.getAttackMs();
    if (id == kReleaseMs)   return core_.getReleaseMs();
    if (id == kRangeDb)     return (double) core_.getRangeDb();
    if (id == kScHpfHz)     return core_.getSidechainHpfHz();
    if (id == kLookaheadMs) return lookaheadMs_;
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedExpanderProcessor::prepareToPlay (double sampleRate, int)
{
    core_.prepare (sampleRate);
    core_.reset();

    // The ring was just resized for this rate, so the sample count the host needs
    // has changed even though the millisecond value has not.
    ejSetLatencyLogged (*this, core_.lookaheadSamples(), "EedExpanderProcessor #2");
}

void EedExpanderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // BYPASS STILL DELAYS: the host is compensating for the reported latency
    // whether or not the device is bypassed, so returning the signal early would
    // shift this track against the session every time bypass was toggled.
    if (isBypassed())
    {
        core_.processDelayOnly (l, r, buffer.getNumSamples());
        return;
    }

    core_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedExpanderProcessor::createEditor()
{
    return new EedExpanderEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeExpanderDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Expander";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay downward expander (built in)";
        d.summary         = "Stereo-linked downward expander with a range limit, a "
                            "detector high-pass and lookahead. Reach "
                            "for it to push a noise floor or room tone down without the "
                            "on/off character of a gate, or to restore dynamics to "
                            "over-compressed material.";
        d.identifier      = "echojay:builtin:expander";
        d.uid             = 0x456A5850;   // 'EjXP' - frozen once shipped
        d.aliases         = { "EchoJayExpander", "EchoJay Expansion",
                              "EchoJay Downward Expander" };
        d.schema          = EedExpanderProcessor::schema();
        d.create          = [] { return std::make_unique<EedExpanderProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar expanderRegistrar { makeExpanderDevice() };
}
