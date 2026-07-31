/*
    EedSaturationProcessor.cpp  —  see EedSaturationProcessor.h.
*/

#include "EedSaturationProcessor.h"
#include "EedSaturationEditor.h"
#include "EedDeviceRegistry.h"

using echojay::harmonic::HarmonicCore;

EedSaturationProcessor::EedSaturationProcessor()
{
    // Everything, including the oversampling factor, comes from the schema's
    // defaults — one source of truth for what a fresh device is. The factor is
    // live-switchable (the core preallocates for 8x), and every change
    // republishes its latency.
    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedSaturationProcessor::schema()
{
    // Ranges here are the SAME numbers HarmonicCore works in. The schema is what
    // the model is taught and what the server validates against; the engine is
    // what enforces it in DSP. Written once, next to each other, so a change to
    // one that misses the other is obvious in review.
    static const echojay::ParamSchema s ({
        { kDriveDb, "dB",
          (double) HarmonicCore::kMinDriveDb, (double) HarmonicCore::kMaxDriveDb, 6.0,
          "how hard the signal is pushed into the curve; output is level "
          "compensated, so this adds harmonics rather than volume", false, {} },

        { kType, "", 0.0, (double) (echojay::harmonic::kCurveCount - 1), 0.0,
          "the saturation curve: tube is asymmetric and warm (2nd harmonic), "
          "tape is soft and compressing, diode is the hardest and most "
          "aggressive, soft is a clean symmetric limit",
          false, { "tube", "tape", "diode", "soft" } },

        // ---- the depth pass ------------------------------------------------
        // The single biggest character control after `type`, so its description
        // carries the even/odd vocabulary a request like "warmer" or "more
        // aggressive" translates against.
        { kEmphasis, "", 0.0, (double) (echojay::harmonic::kEmphasisCount - 1),
          (double) (int) echojay::harmonic::Emphasis::Both,
          "which harmonics dominate, on top of the selected curve: even is "
          "asymmetric and warm/tubey (strong 2nd), odd is symmetric and "
          "aggressive (3rd, console/transistor edge), both keeps the curve's "
          "own natural blend (the default - exactly the device as shipped)",
          false, { "even", "odd", "both" } },

        { kBias, "%",
          (double) HarmonicCore::kMinBias, (double) HarmonicCore::kMaxBias, 0.0,
          "DC offset INTO the curve: either direction pushes the signal onto a "
          "more asymmetric region and brings in even harmonics, without adding "
          "DC to the output; 0 is off", false, {} },

        { kHpfHz, "Hz",
          (double) HarmonicCore::kMinHpfHz, (double) HarmonicCore::kMaxHpfHz, 0.0,
          "keep the sub clean: what sits below this frequency bypasses the "
          "shaper and rejoins after it, so bass stays tight while the mids and "
          "highs get driven. 60-120 on a bass-heavy bus; 0 saturates the full "
          "band", false, {} },

        { kOversample, "", 0.0, 2.0, 1.0,
          "anti-aliasing quality against CPU: 8x is the cleanest on bright "
          "material, 2x the cheapest; 4x (the default) suits almost everything. "
          "Latency follows the setting (30, 45 or 48 samples) and is reported "
          "to the host", false, { "2x", "4x", "8x" } },

        { kToneDb, "dB",
          (double) HarmonicCore::kMinToneDb, (double) HarmonicCore::kMaxToneDb, 0.0,
          "tone tilt around 700 Hz: positive brightens, negative darkens, "
          "0 is exactly flat", false, {} },

        { kMix, "%", 0.0, 100.0, 100.0,
          "dry/wet blend; the dry path is delay matched, so a partial mix does "
          "not comb filter", false, {} },

        { kOutputDb, "dB",
          (double) HarmonicCore::kMinOutDb, (double) HarmonicCore::kMaxOutDb, 0.0,
          "final output trim", false, {} },
    });
    return s;
}

namespace
{
    // oversample choice index <-> the factor the core runs at.
    int factorForOversampleIndex (int index) noexcept
    {
        return index <= 0 ? 2 : (index >= 2 ? 8 : 4);
    }

    int oversampleIndexForFactor (int factor) noexcept
    {
        return factor >= 8 ? 2 : (factor >= 4 ? 1 : 0);
    }
}

bool EedSaturationProcessor::setParamValue (const juce::String& id, double value)
{
    // `value` arrives ALREADY clamped to the schema range — never re-clamp here.
    if (id == kDriveDb)  { core_.setDriveDb    ((float) value); return true; }
    if (id == kToneDb)   { core_.setToneDb     ((float) value); return true; }
    if (id == kMix)      { core_.setMixPercent ((float) value); return true; }
    if (id == kOutputDb) { core_.setOutputDb   ((float) value); return true; }
    if (id == kBias)     { core_.setBias       ((float) value); return true; }
    if (id == kHpfHz)    { core_.setHpfHz      ((float) value); return true; }
    if (id == kType)
    {
        core_.setCurve (echojay::harmonic::curveFromIndex ((int) std::lround (value)));
        return true;
    }
    if (id == kEmphasis)
    {
        core_.setEmphasis (echojay::harmonic::emphasisFromIndex ((int) std::lround (value)));
        return true;
    }
    if (id == kOversample)
    {
        core_.setOversampling (factorForOversampleIndex ((int) std::lround (value)));

        // The number the DAW needs to keep this track in time with every other
        // one. Pushed here as well as from prepareToPlay, because an AI move
        // can change the quality while the graph is running.
        setLatencySamples (core_.latencySamples());
        return true;
    }
    return false;
}

double EedSaturationProcessor::getParamValue (const juce::String& id) const
{
    if (id == kDriveDb)    return (double) core_.getDriveDb();
    if (id == kToneDb)     return (double) core_.getToneDb();
    if (id == kMix)        return (double) core_.getMixPercent();
    if (id == kOutputDb)   return (double) core_.getOutputDb();
    if (id == kBias)       return (double) core_.getBias();
    if (id == kHpfHz)      return (double) core_.getHpfHz();
    if (id == kType)       return (double) (int) core_.getCurve();
    if (id == kEmphasis)   return (double) (int) core_.getEmphasis();
    if (id == kOversample) return (double) oversampleIndexForFactor (core_.getOversampling());
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedSaturationProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    core_.prepare (sampleRate, samplesPerBlock);
    core_.reset();
    spectrumTap_.clear();

    // The anti-aliasing filters cost 30/45/48 samples depending on the
    // `oversample` setting. Publishing it is what lets the host slide the track
    // back into place; a saturator that quietly runs 1 ms early is a phase
    // problem nobody thinks to look for.
    setLatencySamples (core_.latencySamples());
}

void EedSaturationProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel with no input behind it, or it carries whatever
    // was left in the buffer.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    core_.process (l, r, buffer.getNumSamples());

    // After processing: the bars show what came OUT. Deliberately racy and
    // non-blocking, the contract VizTap.h documents.
    spectrumTap_.write (l, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedSaturationProcessor::createEditor()
{
    return new EedSaturationEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeSaturationDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Saturation";
        d.category        = "Harmonic";
        d.descriptiveName = "EchoJay oversampled saturation (built in)";
        // ASCII ONLY in registry text: juce::String's const char* constructor
        // reads its input as ASCII, so a UTF-8 em-dash here ships double-encoded
        // mojibake into the AI prompt. Plain hyphens.
        d.summary         = "Oversampled waveshaping with four curves plus pro depth: an "
                            "even/odd harmonic emphasis, a bias offset, a keep-the-sub-clean "
                            "high-pass and selectable 2x/4x/8x oversampling. Reach for it to "
                            "add weight and harmonic density - to thicken a thin source, glue "
                            "a bus, or push a part forward without raising its level. Tube for "
                            "warmth, tape for glue, diode for grit, soft for a clean ceiling; "
                            "then `emphasis` even for warmer, odd for more aggressive.";
        d.identifier      = "echojay:builtin:saturation";
        d.uid             = 0x456A5341;   // 'EjSA' - frozen once shipped
        d.aliases         = { "EchoJaySaturation", "EchoJay Saturator",
                              "EchoJay Distortion", "EchoJay Drive" };
        d.schema          = EedSaturationProcessor::schema();
        d.create          = [] { return std::make_unique<EedSaturationProcessor>(); };
        return d;
    }

    // This line, and nothing else, integrates the device. See the force-load
    // block in CMakeLists.txt for why it survives static linking.
    const BuiltinDeviceRegistrar saturationRegistrar { makeSaturationDevice() };
}
