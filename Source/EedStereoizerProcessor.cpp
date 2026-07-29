/*
    EedStereoizerProcessor.cpp  —  see EedStereoizerProcessor.h.
*/

#include "EedStereoizerProcessor.h"
#include "EedStereoizerEditor.h"
#include "EedDeviceRegistry.h"

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
// Same numbers StereoEngine clamps to. Descriptions are ASCII only and ship
// verbatim into the AI prompt, so they say what turning the knob DOES, in the
// units the model will send back.
//
// Defaults are a WORKING widener rather than a bypass: this is an effect, not a
// utility, and a model that adds it and dials nothing should still hear it do
// its job. (Stereo Width, the corrective sibling, defaults to transparent.)
const echojay::ParamSchema& EedStereoizerProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kWidth, "%",
          (double) echojay::StereoEngine::kMinWidthPct,
          (double) echojay::StereoEngine::kMaxWidthPct,
          120.0,
          "stereo width: 0 is mono, 100 is unchanged, 200 is double-wide", false },

        { kHaasMs, "ms",
          (double) echojay::StereoEngine::kMinHaasMs,
          (double) echojay::StereoEngine::kMaxHaasMs,
          15.0,
          "Haas widening: how far the side image is displaced in time. 0 is off, "
          "10-25 is a broad natural spread, beyond 30 starts to read as a slap. "
          "Applied so the mono sum is unaffected", false },

        { kMonoMakerHz, "Hz",
          (double) echojay::StereoEngine::kMinBassMonoHz,
          (double) echojay::StereoEngine::kMaxBassMonoHz,
          150.0,
          "everything below this frequency is collapsed to mono, keeping the low "
          "end centred and tight; 0 is off", false },

        { kMix, "%",
          (double) echojay::StereoEngine::kMinMixPct,
          (double) echojay::StereoEngine::kMaxMixPct,
          100.0,
          "dry/wet blend: 0 is the untouched input, 100 is fully widened", false },
    });
    return s;
}

bool EedStereoizerProcessor::setParamValue (const juce::String& id, double value)
{
    // `value` arrives ALREADY clamped to the schema range — never re-clamp here.
    if (id == kWidth)       { engine_.setWidthPercent ((float) value); return true; }
    if (id == kHaasMs)      { engine_.setHaasMs       ((float) value); return true; }
    if (id == kMonoMakerHz) { engine_.setBassMonoHz   ((float) value); return true; }
    if (id == kMix)         { engine_.setMixPercent   ((float) value); return true; }
    return false;
}

double EedStereoizerProcessor::getParamValue (const juce::String& id) const
{
    if (id == kWidth)       return (double) engine_.getWidthPercent();
    if (id == kHaasMs)      return (double) engine_.getHaasMs();
    if (id == kMonoMakerHz) return (double) engine_.getBassMonoHz();
    if (id == kMix)         return (double) engine_.getMixPercent();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
EedStereoizerProcessor::EedStereoizerProcessor()
{
    // This device's defaults are NOT the engine's idle values (see the schema),
    // so the schema has to be the one that wins. Done in the DERIVED
    // constructor because the base cannot: the virtual setParamValue it would
    // need does not exist yet while the base is running.
    resetParamsToDefaults();
}

void EedStereoizerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    // Snap to the current targets, and clear the delay line, so a restored
    // session starts AT its values with no tail from the previous instance.
    engine_.reset();
}

void EedStereoizerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedStereoizerProcessor::createEditor()
{
    return new EedStereoizerEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeStereoizerDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Stereoizer";
        d.category        = "Stereo";
        d.descriptiveName = "EchoJay Haas stereo widener (built in)";

        // ASCII ONLY — a UTF-8 dash here ships mojibake into the AI prompt.
        d.summary         = "Creative widener. Reach for it to open up a narrow or mono "
                            "source - a synth, a guitar double, a pad - with Haas "
                            "displacement rather than plain level. The mono fold-down is "
                            "preserved exactly, so it does not comb when summed.";

        // Frozen once shipped: both are written into saved chain XML.
        d.identifier      = "echojay:builtin:stereoizer";
        d.uid             = 0x456A535A;   // 'EjSZ'

        // "Stereoiser" is the British spelling and a near-certain near-miss;
        // accepting it costs nothing and a rejected name is a device the model
        // simply cannot add.
        d.aliases         = { "EchoJayStereoizer", "EchoJay Stereoiser",
                              "EchoJay Widener", "EchoJay Haas" };
        d.schema          = EedStereoizerProcessor::schema();
        d.create          = [] { return std::make_unique<EedStereoizerProcessor>(); };
        return d;
    }

    // File-scope static: this line, and nothing else, integrates the device.
    const BuiltinDeviceRegistrar stereoizerRegistrar { makeStereoizerDevice() };
}
