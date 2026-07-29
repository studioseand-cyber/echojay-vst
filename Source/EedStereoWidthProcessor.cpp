/*
    EedStereoWidthProcessor.cpp  —  see EedStereoWidthProcessor.h.
*/

#include "EedStereoWidthProcessor.h"
#include "EedStereoWidthEditor.h"
#include "EedDeviceRegistry.h"

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
// Ranges here are the SAME numbers StereoEngine clamps to. The schema is what
// the model is taught and what the server validates against; the engine is what
// enforces it in DSP.
//
// Descriptions are ASCII only and say what TURNING the knob does, in the units
// the model will send. They ship verbatim into the AI prompt.
const echojay::ParamSchema& EedStereoWidthProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kWidth, "%",
          (double) echojay::StereoEngine::kMinWidthPct,
          (double) echojay::StereoEngine::kMaxWidthPct,
          100.0,
          "stereo width: 0 is mono, 100 is unchanged, 200 is double-wide "
          "(the mono fold-down is identical at every setting)", false },

        { kBassMonoHz, "Hz",
          (double) echojay::StereoEngine::kMinBassMonoHz,
          (double) echojay::StereoEngine::kMaxBassMonoHz,
          0.0,
          "everything below this frequency is collapsed to mono; 0 is off. "
          "Typical 80-150 for a tight low end", false },

        { kOutputTrimDb, "dB",
          (double) echojay::StereoEngine::kMinTrimDb,
          (double) echojay::StereoEngine::kMaxTrimDb,
          0.0,
          "output level trim, for matching loudness after a width change", false },
    });
    return s;
}

bool EedStereoWidthProcessor::setParamValue (const juce::String& id, double value)
{
    // `value` arrives ALREADY clamped to the schema range — never re-clamp here.
    if (id == kWidth)        { engine_.setWidthPercent ((float) value); return true; }
    if (id == kBassMonoHz)   { engine_.setBassMonoHz   ((float) value); return true; }
    if (id == kOutputTrimDb) { engine_.setTrimDb       ((float) value); return true; }
    return false;
}

double EedStereoWidthProcessor::getParamValue (const juce::String& id) const
{
    if (id == kWidth)        return (double) engine_.getWidthPercent();
    if (id == kBassMonoHz)   return (double) engine_.getBassMonoHz();
    if (id == kOutputTrimDb) return (double) engine_.getTrimDb();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
EedStereoWidthProcessor::EedStereoWidthProcessor()
{
    // The engine's own idle values and the schema's defaults are two separate
    // statements of intent; make the schema win, here, once. Doing it in the
    // DERIVED constructor is deliberate — the base cannot, because the virtual
    // setParamValue it would need does not exist yet while the base is running.
    resetParamsToDefaults();
}

void EedStereoWidthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    // Snap to the current targets so a freshly restored session starts AT its
    // values rather than gliding up to them across the first block.
    engine_.reset();
}

void EedStereoWidthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel the host gave us that has no input behind it,
    // otherwise it carries whatever was left in the buffer.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedStereoWidthProcessor::createEditor()
{
    return new EedStereoWidthEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeStereoWidthDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Stereo Width";
        d.category        = "Stereo";
        d.descriptiveName = "EchoJay stereo width + bass mono (built in)";

        // ASCII ONLY: juce::String's const char* constructor reads its input as
        // ASCII, so a UTF-8 dash here ships double-encoded mojibake into the AI
        // prompt. Plain hyphens throughout.
        d.summary         = "Corrective stereo control. Reach for it to widen or narrow "
                            "an image by an exact percentage, or to lock the low end to "
                            "mono under a set frequency. The mono fold-down is preserved "
                            "exactly, so it is safe on a master bus.";

        // Frozen once shipped: both are written into saved chain XML.
        d.identifier      = "echojay:builtin:stereowidth";
        d.uid             = 0x456A5357;   // 'EjSW'

        d.aliases         = { "EchoJayStereoWidth", "EchoJay Width", "EchoJay Stereo",
                              "EchoJay Imager" };
        d.schema          = EedStereoWidthProcessor::schema();
        d.create          = [] { return std::make_unique<EedStereoWidthProcessor>(); };
        return d;
    }

    // File-scope static: this line, and nothing else, integrates the device.
    // See CMakeLists.txt's force-load block for why it survives static linking.
    const BuiltinDeviceRegistrar stereoWidthRegistrar { makeStereoWidthDevice() };
}
