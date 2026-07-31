/*
    EedTremoloProcessor.cpp  —  see EedTremoloProcessor.h.
*/

#include "EedTremoloProcessor.h"
#include "EedTremoloEditor.h"
#include "EedDeviceRegistry.h"
#include "EedModTempo.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
// Ranges here are the SAME numbers LfoCore and TremoloEngine clamp to. The
// schema is what the model is taught and what the server validates against; the
// engines are what enforce it in DSP. Written once, next to each other.
const echojay::ParamSchema& EedTremoloProcessor::schema()
{
    using L = echojay::LfoCore;

    static const echojay::ParamSchema s ({
        { kRateHz, "Hz",
          (double) L::kMinRateHz, (double) L::kMaxRateHz, 4.0,
          "speed of the level wobble; ignored while sync is on", false },

        { kSync, "",
          0.0, 1.0, 0.0,
          "lock the rate to the host tempo and use sync_division instead", true },

        { kDivision, "",
          0.0, (double) (L::kNumDivisions - 1), (double) L::kDefaultDivision,
          "tempo division while sync is on: 0 = 4/1 slowest, 6 = 1/4, "
          "12 = 1/32 fastest", false },

        { kDepth, "%",
          (double) L::kMinDepth, (double) L::kMaxDepth, 50.0,
          "how far the level drops at the trough; 100 reaches silence, "
          "the peak stays at unity", false },

        // A named choice since the depth pass (it was a bare 0..3): the value
        // is still the index, so old sessions and old numeric moves land
        // unchanged, and the two new shapes simply append.
        { kShape, "",
          0.0, (double) (L::kNumShapes - 1), 0.0,
          "waveform of the wobble: sine is smooth, triangle linear, square "
          "chops, saw ramps, harmonic is a rounder sine that lingers at the "
          "extremes, random holds a new random level each cycle "
          "(sample-and-hold; see smoothing_ms to make it glide)",
          false, { "sine", "triangle", "square", "saw", "harmonic", "random" } },

        { kStereoPhase, "deg",
          (double) L::kMinPhaseDeg, (double) L::kMaxPhaseDeg, 0.0,
          "how far the right channel's wobble lags the left; 0 is mono, "
          "180 makes the channels alternate", false },

        { kMix, "%",
          (double) echojay::TremoloEngine::kMinMix,
          (double) echojay::TremoloEngine::kMaxMix, 100.0,
          "how much of the modulated signal is heard; 0 is bypass", false },

        // ---- the depth pass (DEVICE_DEPTH_PLAN.md, Modulation) -------------
        { kMode, "",
          0.0, (double) (echojay::kNumTremoloModes - 1),
          (double) (int) echojay::TremoloMode::Sine,
          "the tremolo circuit: sine is the clean level wobble the device "
          "shipped with, optical smooths the gain like an amp's photocell so "
          "even a square breathes with soft edges, bias is harmonic tremolo - "
          "lows and highs wobble in OPPOSITE phase so the tone swirls while "
          "the level barely moves, the classic Fender-style sound and much "
          "richer than plain level tremolo",
          false, { "sine", "optical", "bias" } },

        { kSmoothing, "ms",
          (double) L::kMinSmoothingMs, (double) L::kMaxSmoothingMs, 1.5,
          "rounds the waveform's corners: a few ms is what keeps square from "
          "clicking, and 50-200 makes the random shape glide between its "
          "steps instead of jumping; 0 is hard edges", false },
    });
    return s;
}

bool EedTremoloProcessor::setParamValue (const juce::String& id, double value)
{
    auto& lfo = engine_.lfo();

    if (id == kRateHz)      { lfo.setRateHz         ((float) value); return true; }
    if (id == kSync)        { lfo.setTempoSync      (value >= 0.5);  return true; }
    // Discrete params arrive as a number from a dial or from the AI; rounding
    // here is what makes "shape": 1.6 mean triangle rather than nothing.
    if (id == kDivision)    { lfo.setDivisionIndex  ((int) std::lround (value)); return true; }
    if (id == kDepth)       { lfo.setDepthPercent   ((float) value); return true; }
    if (id == kShape)       { lfo.setShape          ((int) std::lround (value)); return true; }
    if (id == kStereoPhase) { lfo.setStereoPhaseDeg ((float) value); return true; }
    if (id == kMix)         { engine_.setMixPercent ((float) value); return true; }
    if (id == kSmoothing)   { lfo.setSmoothingMs    ((float) value); return true; }
    if (id == kMode)
    {
        engine_.setMode (echojay::tremoloModeFromIndex ((int) std::lround (value)));
        return true;
    }
    return false;
}

double EedTremoloProcessor::getParamValue (const juce::String& id) const
{
    const auto& lfo = engine_.lfo();

    if (id == kRateHz)      return (double) lfo.getRateHz();
    if (id == kSync)        return lfo.getTempoSync() ? 1.0 : 0.0;
    if (id == kDivision)    return (double) lfo.getDivisionIndex();
    if (id == kDepth)       return (double) lfo.getDepthPercent();
    if (id == kShape)       return (double) lfo.getShape();
    if (id == kStereoPhase) return (double) lfo.getStereoPhaseDeg();
    if (id == kMix)         return (double) engine_.getMixPercent();
    if (id == kMode)        return (double) (int) engine_.getMode();
    if (id == kSmoothing)   return (double) lfo.getSmoothingMs();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedTremoloProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    // Snap to the current targets so a freshly restored session starts AT its
    // values rather than ramping into them on the first block.
    engine_.reset();
}

void EedTremoloProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel the host gave us that has no input behind it,
    // otherwise it carries whatever was left in the buffer.
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

juce::AudioProcessorEditor* EedTremoloProcessor::createEditor()
{
    return new EedTremoloEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeTremoloDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Tremolo";
        d.category        = "Modulation";
        d.descriptiveName = "EchoJay tremolo (built in)";
        // ASCII ONLY: juce::String's const char* constructor reads its input as
        // ASCII, so a UTF-8 dash here would ship mojibake into the AI prompt.
        d.summary         = "Rhythmic level modulation, in Hz or locked to the host "
                            "tempo. Reach for it to make a static part move - a held "
                            "chord, a pad, a guitar - or, with a square shape at a "
                            "note division, to chop a sustained sound into a pattern. "
                            "Three circuits via mode: sine (clean), optical "
                            "(soft-edged, amp-like), bias (harmonic tremolo - the "
                            "tone swirls instead of the level chopping).";
        d.identifier      = "echojay:builtin:tremolo";
        d.uid             = 0x456A5452;   // 'EjTR' - frozen once shipped
        d.aliases         = { "EchoJayTremolo", "EchoJay Trem", "Tremolo" };
        d.schema          = EedTremoloProcessor::schema();
        d.create          = [] { return std::make_unique<EedTremoloProcessor>(); };
        return d;
    }

    // File-scope static: this line, and nothing else, integrates the device.
    // See CMakeLists.txt's force-load block for why it survives static linking.
    const BuiltinDeviceRegistrar tremoloRegistrar { makeTremoloDevice() };
}
