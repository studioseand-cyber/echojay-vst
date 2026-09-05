/*
    EedExciterProcessor.cpp  —  see EedExciterProcessor.h.
*/

#include "EedExciterProcessor.h"
#include "EedLatencyLog.h"
#include "EedExciterEditor.h"
#include "EedDeviceRegistry.h"

using echojay::ExciterEngine;

EedExciterProcessor::EedExciterProcessor()
{
    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedExciterProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kFreqHz, "Hz",
          (double) ExciterEngine::kMinFreqHz, (double) ExciterEngine::kMaxFreqHz, 3000.0,
          "band split; only what is ABOVE this is shaped, so the low end stays "
          "clean", false, {} },

        { kAmount, "%",
          (double) ExciterEngine::kMinAmount, (double) ExciterEngine::kMaxAmount, 50.0,
          "how far the high band is driven into the curve; 0 passes the signal "
          "through untouched", false, {} },

        // ---- expanded by the depth pass: odd and even characters -----------
        // tube and tape keep their frozen indices; odd and even are appended,
        // so a saved state from before this pass restores unchanged.
        { kMode, "", 0.0, (double) (ExciterEngine::kNumModes - 1), 0.0,
          "the character of the generated highs: tube is asymmetric air and "
          "presence (2nd plus odd - the default), tape is a soft symmetric "
          "sheen without edge, odd is the edgiest and most aggressive (3rd "
          "only, a symmetric hard knee), even is the warmest (strongly "
          "asymmetric, 2nd dominant)",
          false, { "tube", "tape", "odd", "even" } },

        { kFocus, "%",
          (double) ExciterEngine::kMinFocus, (double) ExciterEngine::kMaxFocus, 0.0,
          "how tightly the excitement is confined to the band above the split: "
          "0 is the split as shipped (the default), higher trims the bleed "
          "below the crossover so the lows stay surgically clean while the "
          "highs are excited", false, {} },

        { kMix, "%", 0.0, 100.0, 100.0,
          "dry/wet blend; the dry path is delay matched, so a partial mix does "
          "not comb filter", false, {} },

        { kOutputDb, "dB",
          (double) ExciterEngine::kMinOutDb, (double) ExciterEngine::kMaxOutDb, 0.0,
          "final output trim", false, {} },
    });
    return s;
}

bool EedExciterProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kFreqHz)   { engine_.setFreqHz     ((float) value); return true; }
    if (id == kAmount)   { engine_.setAmount     ((float) value); return true; }
    if (id == kFocus)    { engine_.setFocus      ((float) value); return true; }
    if (id == kMix)      { engine_.setMixPercent ((float) value); return true; }
    if (id == kOutputDb) { engine_.setOutputDb   ((float) value); return true; }
    if (id == kMode)     { engine_.setMode ((int) std::lround (value)); return true; }
    return false;
}

double EedExciterProcessor::getParamValue (const juce::String& id) const
{
    if (id == kFreqHz)   return (double) engine_.getFreqHz();
    if (id == kAmount)   return (double) engine_.getAmount();
    if (id == kFocus)    return (double) engine_.getFocus();
    if (id == kMix)      return (double) engine_.getMixPercent();
    if (id == kOutputDb) return (double) engine_.getOutputDb();
    if (id == kMode)     return (double) engine_.getMode();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedExciterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    engine_.reset();

    ejSetLatencyLogged (*this, engine_.latencySamples(), "EedExciterProcessor #1");
}

void EedExciterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

juce::AudioProcessorEditor* EedExciterProcessor::createEditor()
{
    return new EedExciterEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeExciterDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Exciter";
        d.category        = "Harmonic";
        d.descriptiveName = "EchoJay band-split harmonic exciter (built in)";
        // ASCII ONLY in registry text - see the note in EedPhaseInvertProcessor.
        d.summary         = "Adds harmonics to the highs only, leaving everything below the "
                            "split untouched. Reach for it when a source needs air, presence "
                            "or bite and an EQ boost would only make it harsh - it generates "
                            "the top end rather than amplifying what is already there. Four "
                            "characters via `mode`: tube for presence, tape for sheen, odd "
                            "for edge, even for warmth; raise `focus` to keep the lows "
                            "surgically clean.";
        d.identifier      = "echojay:builtin:exciter";
        d.uid             = 0x456A4558;   // 'EjEX' - frozen once shipped
        d.aliases         = { "EchoJayExciter", "EchoJay Enhancer", "EchoJay Air" };
        d.schema          = EedExciterProcessor::schema();
        d.create          = [] { return std::make_unique<EedExciterProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar exciterRegistrar { makeExciterDevice() };
}
