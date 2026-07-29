/*
    EedExciterProcessor.cpp  —  see EedExciterProcessor.h.
*/

#include "EedExciterProcessor.h"
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

        { kMode, "", 0.0, 1.0, 0.0,
          "the curve used on the high band: tube is asymmetric and brings in a "
          "2nd harmonic (air, presence), tape is symmetric and softer (sheen "
          "without edge)",
          false, { "tube", "tape" } },

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
    if (id == kMix)      { engine_.setMixPercent ((float) value); return true; }
    if (id == kOutputDb) { engine_.setOutputDb   ((float) value); return true; }
    if (id == kMode)     { engine_.setMode ((int) std::lround (value)); return true; }
    return false;
}

double EedExciterProcessor::getParamValue (const juce::String& id) const
{
    if (id == kFreqHz)   return (double) engine_.getFreqHz();
    if (id == kAmount)   return (double) engine_.getAmount();
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

    setLatencySamples (engine_.latencySamples());
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
                            "the top end rather than amplifying what is already there.";
        d.identifier      = "echojay:builtin:exciter";
        d.uid             = 0x456A4558;   // 'EjEX' - frozen once shipped
        d.aliases         = { "EchoJayExciter", "EchoJay Enhancer", "EchoJay Air" };
        d.schema          = EedExciterProcessor::schema();
        d.create          = [] { return std::make_unique<EedExciterProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar exciterRegistrar { makeExciterDevice() };
}
