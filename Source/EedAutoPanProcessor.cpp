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

        // A named choice since the depth pass (it was a bare 0..3): the value
        // is still the index, so old sessions and old numeric moves land
        // unchanged, and the two new shapes simply append.
        { kShape, "",
          0.0, (double) (L::kNumShapes - 1), 0.0,
          "waveform of the sweep: sine is smooth, triangle linear, square "
          "jumps side to side, saw ramps, harmonic is a rounder sine that "
          "lingers at the sides, random jumps to a new random position each "
          "cycle (sample-and-hold; see smoothing_ms to make it glide)",
          false, { "sine", "triangle", "square", "saw", "harmonic", "random" } },

        { kStereoPhase, "deg",
          (double) L::kMinPhaseDeg, (double) L::kMaxPhaseDeg, 0.0,
          "offset between the two channels' movement: 0 sweeps the whole image "
          "together, 90 rotates it, 180 turns it into a tremolo", false },

        // ---- the depth pass (DEVICE_DEPTH_PLAN.md, Modulation) -------------
        { kMode, "",
          0.0, (double) (echojay::kNumAutoPanModes - 1),
          (double) (int) echojay::AutoPanMode::ConstantPower,
          "the pan law: constant_power holds loudness steady across the sweep "
          "(the default, and the device as it shipped), linear is a plain "
          "crossfade that dips slightly mid-sweep, binaural adds a subtle "
          "inter-aural delay with the pan so the image reads as genuinely "
          "PLACED in space rather than faded - the most convincing movement "
          "on headphones",
          false, { "linear", "constant_power", "binaural" } },

        { kWidth, "%",
          (double) echojay::AutoPanEngine::kMinWidthPct,
          (double) echojay::AutoPanEngine::kMaxWidthPct, 100.0,
          "how far the stereo field extends: scales the whole sweep, so 50 "
          "confines the movement to the middle half of the field even at "
          "full depth; 100 is the full field", false },

        { kSmoothing, "ms",
          (double) L::kMinSmoothingMs, (double) L::kMaxSmoothingMs, 1.5,
          "rounds the waveform's corners: a few ms is what keeps square from "
          "clicking, and 50-200 makes the random shape drift between its "
          "positions instead of teleporting; 0 is hard edges", false },
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
    if (id == kWidth)       { engine_.setWidthPercent ((float) value); return true; }
    if (id == kSmoothing)   { lfo.setSmoothingMs    ((float) value); return true; }
    if (id == kMode)
    {
        engine_.setMode (echojay::autoPanModeFromIndex ((int) std::lround (value)));
        return true;
    }
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
    if (id == kMode)        return (double) (int) engine_.getMode();
    if (id == kWidth)       return (double) engine_.getWidthPercent();
    if (id == kSmoothing)   return (double) lfo.getSmoothingMs();
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
                            "movement without changing its tone; set mode binaural "
                            "for headphone-convincing placement, or shape random for "
                            "a new position each cycle.";
        d.identifier      = "echojay:builtin:autopan";
        d.uid             = 0x456A4150;   // 'EjAP' - frozen once shipped
        d.aliases         = { "EchoJayAutoPan", "EchoJay Panner", "Auto Pan", "AutoPan" };
        d.schema          = EedAutoPanProcessor::schema();
        d.create          = [] { return std::make_unique<EedAutoPanProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar autoPanRegistrar { makeAutoPanDevice() };
}
