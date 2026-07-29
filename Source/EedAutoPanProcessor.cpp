/*
    EedAutoPanProcessor.cpp  —  see EedAutoPanProcessor.h.
*/

#include "EedAutoPanProcessor.h"
#include "EedAutoPanEditor.h"
#include "EedDeviceRegistry.h"
#include "EedModTempo.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedAutoPanProcessor::schema()
{
    using L = echojay::LfoCore;

    static const echojay::ParamSchema s ({
        { kRateHz, "Hz",
          (double) L::kMinRateHz, (double) L::kMaxRateHz, 0.5,
          "speed the image sweeps across the field; ignored while sync is on", false },

        { kSync, "",
          0.0, 1.0, 0.0,
          "lock the rate to the host tempo and use sync_division instead", true },

        { kDivision, "",
          0.0, (double) (L::kNumDivisions - 1), (double) L::kDefaultDivision,
          "tempo division while sync is on: 0 = 4/1 slowest, 6 = 1/4, "
          "12 = 1/32 fastest", false },

        { kDepth, "%",
          (double) L::kMinDepth, (double) L::kMaxDepth, 70.0,
          "how far from centre the image travels; 100 reaches hard left "
          "and hard right, 0 leaves it centred", false },

        { kShape, "",
          0.0, (double) (L::kNumShapes - 1), 0.0,
          "waveform: 0 sine (smooth sweep), 1 triangle (linear), "
          "2 square (jump side to side), 3 saw", false },

        { kStereoPhase, "deg",
          (double) L::kMinPhaseDeg, (double) L::kMaxPhaseDeg, 0.0,
          "offset between the two channels' movement: 0 sweeps the whole image "
          "together, 90 rotates it, 180 turns it into a tremolo", false },
    });
    return s;
}

bool EedAutoPanProcessor::setParamValue (const juce::String& id, double value)
{
    auto& lfo = engine_.lfo();

    if (id == kRateHz)      { lfo.setRateHz         ((float) value); return true; }
    if (id == kSync)        { lfo.setTempoSync      (value >= 0.5);  return true; }
    if (id == kDivision)    { lfo.setDivisionIndex  ((int) std::lround (value)); return true; }
    if (id == kDepth)       { lfo.setDepthPercent   ((float) value); return true; }
    if (id == kShape)       { lfo.setShape          ((int) std::lround (value)); return true; }
    if (id == kStereoPhase) { lfo.setStereoPhaseDeg ((float) value); return true; }
    return false;
}

double EedAutoPanProcessor::getParamValue (const juce::String& id) const
{
    const auto& lfo = engine_.lfo();

    if (id == kRateHz)      return (double) lfo.getRateHz();
    if (id == kSync)        return lfo.getTempoSync() ? 1.0 : 0.0;
    if (id == kDivision)    return (double) lfo.getDivisionIndex();
    if (id == kDepth)       return (double) lfo.getDepthPercent();
    if (id == kShape)       return (double) lfo.getShape();
    if (id == kStereoPhase) return (double) lfo.getStereoPhaseDeg();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedAutoPanProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    engine_.reset();
}

void EedAutoPanProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

juce::AudioProcessorEditor* EedAutoPanProcessor::createEditor()
{
    return new EedAutoPanEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeAutoPanDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Auto Pan";
        d.category        = "Modulation";
        d.descriptiveName = "EchoJay auto pan (built in)";
        // ASCII ONLY - see the note in DeviceTemplate/README.md.
        d.summary         = "Sweeps a source across the stereo field, in Hz or locked "
                            "to the host tempo, at constant power so the level never "
                            "dips in the middle. Reach for it to give a static part "
                            "movement without changing its tone.";
        d.identifier      = "echojay:builtin:autopan";
        d.uid             = 0x456A4150;   // 'EjAP' - frozen once shipped
        d.aliases         = { "EchoJayAutoPan", "EchoJay Panner", "Auto Pan", "AutoPan" };
        d.schema          = EedAutoPanProcessor::schema();
        d.create          = [] { return std::make_unique<EedAutoPanProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar autoPanRegistrar { makeAutoPanDevice() };
}
