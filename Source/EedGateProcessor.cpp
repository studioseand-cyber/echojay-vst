/*
    EedGateProcessor.cpp  —  see EedGateProcessor.h.
*/

#include "EedGateProcessor.h"
#include "EedGateEditor.h"
#include "EedDeviceRegistry.h"

EedGateProcessor::EedGateProcessor()
{
    core_.setDetectorMode (echojay::DetectorMode::Peak);
    core_.setMaxLookaheadMs (kMaxLookaheadMs);

    // Gate vs Duck is a schema param now, so the mode comes from the defaults
    // like everything else rather than being set twice.
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

        // ---- the depth pass ------------------------------------------------
        { kMode, "", 0.0, 1.0, 0.0,
          "which side of the threshold gets attenuated: gate pulls down what is "
          "QUIETER than the threshold (cleaning up bleed and noise), duck pulls "
          "down what is LOUDER than it by range_db (holding something back "
          "whenever it gets loud). Everything else means the same in both",
          false, { "gate", "duck" } },

        { kScHpfHz, "Hz", 0.0, 500.0, 0.0,
          "high-pass on the DETECTOR only, never on the audio: with sc_lpf_hz it "
          "makes the trigger frequency-selective, so a tom mic can open on the "
          "tom and ignore the snare bleeding into it. 0 is off", false },

        { kScLpfHz, "Hz", 200.0, 20000.0, 20000.0,
          "low-pass on the detector only, the other half of a selective trigger; "
          "set it just above the source you want to open on. 20000 is wide open",
          false },

        { kLookaheadMs, "ms", 0.0, kMaxLookaheadMs, 0.0,
          "how far ahead it looks, so the gate is already opening when the "
          "transient arrives instead of clipping the front off it; this is added "
          "latency, reported to the host. 1-3 ms is enough for most sources", false },
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
    if (id == kScHpfHz)      { core_.setSidechainHpfHz (value);       return true; }
    if (id == kScLpfHz)      { core_.setSidechainLpfHz (value);       return true; }

    if (id == kMode)
    {
        core_.setMode (value >= 0.5 ? echojay::DynamicsMode::Duck
                                    : echojay::DynamicsMode::Gate);
        return true;
    }

    if (id == kLookaheadMs)
    {
        lookaheadMs_ = value;
        core_.setLookaheadMs (value);
        setLatencySamples (core_.lookaheadSamples());
        return true;
    }
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
    if (id == kMode)         return isDucking() ? 1.0 : 0.0;
    if (id == kScHpfHz)      return core_.getSidechainHpfHz();
    if (id == kScLpfHz)      return core_.getSidechainLpfHz();
    if (id == kLookaheadMs)  return lookaheadMs_;
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedGateProcessor::prepareToPlay (double sampleRate, int)
{
    core_.prepare (sampleRate);
    core_.reset();

    // The ring was just resized for this rate, so the sample count the host needs
    // has changed even though the millisecond value has not.
    setLatencySamples (core_.lookaheadSamples());
}

void EedGateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // BYPASS STILL DELAYS: once a lookahead is dialled the host is compensating
    // for the reported latency whether or not the device is bypassed, so
    // returning the signal early would shift this track against the session.
    if (isBypassed())
    {
        core_.processDelayOnly (l, r, buffer.getNumSamples());
        return;
    }

    core_.process (l, r, buffer.getNumSamples());
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
        d.summary         = "Stereo-linked gate with hold, hysteresis, lookahead and a "
                            "frequency-selective sidechain, attenuating by a set range "
                            "rather than hard muting. Reach for it to clean up bleed "
                            "between hits, tighten a drum or remove a noise floor "
                            "between phrases - or switch mode to duck, and it holds "
                            "something down whenever the signal gets loud instead.";
        d.identifier      = "echojay:builtin:gate";
        d.uid             = 0x456A4754;   // 'EjGT' - frozen once shipped
        d.aliases         = { "EchoJayGate", "EchoJay Noise Gate", "EchoJay Ducker" };
        d.schema          = EedGateProcessor::schema();
        d.create          = [] { return std::make_unique<EedGateProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar gateRegistrar { makeGateDevice() };
}
