/*
    EedTemplateProcessor.cpp  —  SKELETON. Copy to Source/, rename Template ->
    your device. Not compiled where it sits (see DeviceTemplate/README.md).
*/

#include "EedTemplateProcessor.h"
#include "EedTemplateEditor.h"
#include "EedDeviceRegistry.h"

// ---------------------------------------------------------------------------
// the dialable contract — THE step that matters
// ---------------------------------------------------------------------------
// This single schema is what the model is taught, what the server validates
// against, and what setParamValue maps. A knob with no ParamSpec here is not
// dialable, and by house rule does not ship.
//
// Real-world units only. The model reasons in dB / Hz / ms; making it reason in
// normalised 0..1 is how exactness dies.
const echojay::ParamSchema& EedTemplateProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kAmount, "%",
          (double) echojay::TemplateEngine::kMinAmount,
          (double) echojay::TemplateEngine::kMaxAmount,
          0.0,
          "how much of the effect is applied", false },

        // { "attack_ms", "ms", 0.1, 200.0, 10.0, "how fast it clamps down", false },
        // { "listen",    "",   0.0,   1.0,  0.0, "solo the detector",       true  },
    });
    return s;
}

bool EedTemplateProcessor::setParamValue (const juce::String& id, double value)
{
    // `value` arrives ALREADY clamped to the schema range — never re-clamp here.
    // Return false for an advertised id you have not implemented; it is then
    // reported as skipped rather than silently swallowed.
    if (id == kAmount) { engine_.setAmount ((float) value); return true; }
    return false;
}

double EedTemplateProcessor::getParamValue (const juce::String& id) const
{
    if (id == kAmount) return (double) engine_.getAmount();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedTemplateProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    engine_.reset();
}

void EedTemplateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

    engine_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedTemplateProcessor::createEditor()
{
    return new EedTemplateEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
// No edit to ChainHost, EchoJayAPI, the add-menu or any central list: they all
// read from the registry. Adding this file (and its CMake lines) is the whole job.
namespace
{
    BuiltinDevice makeTemplateDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Template";
        d.category        = "Utility";   // EQ | Dynamics | Utility | Stereo |
                                         // Modulation | Harmonic | Time
        d.descriptiveName = "EchoJay template device (built in)";

        // ASCII ONLY. juce::String's const char* constructor reads its input as
        // ASCII, so a UTF-8 em-dash here ships double-encoded mojibake into the
        // AI prompt. Use a plain hyphen.
        d.summary         = "One line telling the model WHEN to reach for this "
                            "device, not what it is.";

        // Frozen once shipped: both are written into saved chain XML, so
        // changing either orphans every session already using the device.
        // Check the uid is free first:  grep -rn "0x456A" Source
        d.identifier      = "echojay:builtin:template";
        d.uid             = 0x456A5454;   // 'EjTT'

        d.aliases         = { "EchoJayTemplate" };
        d.schema          = EedTemplateProcessor::schema();
        d.create          = [] { return std::make_unique<EedTemplateProcessor>(); };
        return d;
    }

    // This line, and nothing else, integrates the device. See the force-load
    // block in CMakeLists.txt for why it survives static linking.
    const BuiltinDeviceRegistrar templateRegistrar { makeTemplateDevice() };
}
