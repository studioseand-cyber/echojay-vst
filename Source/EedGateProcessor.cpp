/*
    EedGateProcessor.cpp  —  see EedGateProcessor.h.
*/

#include "EedGateProcessor.h"
#include "EedGateEditor.h"
#include "EedDeviceRegistry.h"

EedGateProcessor::EedGateProcessor()
{
    core_.setMode (echojay::DynamicsMode::Gate);
    core_.setDetectorMode (echojay::DetectorMode::Peak);
    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedGateProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kThresholdDb, "dB", -80.0, 0.0, -40.0,
          "level the signal must reach for the gate to open; anything quieter is "
          "attenuated", false },

        { kRangeDb, "dB", 0.0, 80.0, 40.0,
          "how far the gate pulls down when closed; 0 is no gating at all, 20 is "
          "ducking bleed, 60+ is effectively silence", false },

        { kAttackMs, "ms", 0.05, 100.0, 1.0,
          "how fast it opens; too slow clips the front off every note", false },

        { kHoldMs, "ms", 0.0, 1000.0, 20.0,
          "minimum time it stays open after the signal drops; keeps a decay or a "
          "tail intact instead of chopping it", false },

        { kReleaseMs, "ms", 5.0, 3000.0, 150.0,
          "how fast it closes once the hold expires; short is abrupt, long fades "
          "the tail out naturally", false },

        { kHysteresisDb, "dB", 0.0, 24.0, 6.0,
          "how far below the threshold the signal must fall before the gate "
          "closes; this is what stops it chattering on a signal sitting right at "
          "the threshold", false },
    });
    return s;
}

bool EedGateProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kThresholdDb)  { core_.setThresholdDb  ((float) value); return true; }
    if (id == kRangeDb)      { core_.setRangeDb      ((float) value); return true; }
    if (id == kAttackMs)     { core_.setAttackMs     (value);         return true; }
    if (id == kHoldMs)       { core_.setHoldMs       (value);         return true; }
    if (id == kReleaseMs)    { core_.setReleaseMs    (value);         return true; }
    if (id == kHysteresisDb) { core_.setHysteresisDb ((float) value); return true; }
    return false;
}

double EedGateProcessor::getParamValue (const juce::String& id) const
{
    if (id == kThresholdDb)  return (double) core_.getThresholdDb();
    if (id == kRangeDb)      return (double) core_.getRangeDb();
    if (id == kAttackMs)     return core_.getAttackMs();
    if (id == kHoldMs)       return core_.getHoldMs();
    if (id == kReleaseMs)    return core_.getReleaseMs();
    if (id == kHysteresisDb) return (double) core_.getHysteresisDb();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedGateProcessor::prepareToPlay (double sampleRate, int)
{
    core_.prepare (sampleRate);
    core_.reset();
}

void EedGateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    core_.process (buffer.getWritePointer (0),
                   numCh > 1 ? buffer.getWritePointer (1) : nullptr,
                   buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedGateProcessor::createEditor()
{
    return new EedGateEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeGateDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Gate";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay noise gate (built in)";
        d.summary         = "Stereo-linked gate with hold and hysteresis, attenuating "
                            "by a set range rather than hard muting. Reach for it to "
                            "clean up bleed between hits, tighten a drum or remove a "
                            "noise floor between phrases.";
        d.identifier      = "echojay:builtin:gate";
        d.uid             = 0x456A4754;   // 'EjGT' - frozen once shipped
        d.aliases         = { "EchoJayGate", "EchoJay Noise Gate" };
        d.schema          = EedGateProcessor::schema();
        d.create          = [] { return std::make_unique<EedGateProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar gateRegistrar { makeGateDevice() };
}
