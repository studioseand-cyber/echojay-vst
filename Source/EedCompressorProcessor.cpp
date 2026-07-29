/*
    EedCompressorProcessor.cpp  —  see EedCompressorProcessor.h.
*/

#include "EedCompressorProcessor.h"
#include "EedCompressorEditor.h"
#include "EedDeviceRegistry.h"

EedCompressorProcessor::EedCompressorProcessor()
{
    core_.setMode (echojay::DynamicsMode::Compress);
    core_.setDetectorMode (echojay::DetectorMode::Rms);
    core_.setRmsWindowMs (10.0);
    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedCompressorProcessor::schema()
{
    // Descriptions are written for the MODEL, not for a manual: each one says
    // what turning the knob does to the sound, because that is what a request
    // like "make the vocal sit better" has to be translated against.
    static const echojay::ParamSchema s ({
        { kThresholdDb, "dB", -60.0, 0.0, -18.0,
          "level where compression starts; lower means more of the signal is "
          "compressed", false },

        { kRatio, "", 1.0, 20.0, 4.0,
          "how hard it compresses above the threshold; 2 is gentle glue, 4 is "
          "control, 10+ is limiting", false },

        { kAttackMs, "ms", 0.1, 200.0, 10.0,
          "how fast it clamps down; short keeps transients in check, long lets "
          "them through and sounds punchier", false },

        { kReleaseMs, "ms", 5.0, 2000.0, 120.0,
          "how fast it lets go; short is more audible pumping, long is smoother "
          "level riding", false },

        { kKneeDb, "dB", 0.0, 24.0, 6.0,
          "width of the soft transition around the threshold; 0 is a hard corner, "
          "wide is gradual and transparent", false },

        { kMakeupDb, "dB", -12.0, 24.0, 0.0,
          "output gain to put back what the compression took off", false },

        { kMix, "%", 0.0, 100.0, 100.0,
          "blend of compressed against dry; below 100 is parallel compression, "
          "which adds density without losing transients", false },
    });
    return s;
}

bool EedCompressorProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kThresholdDb) { core_.setThresholdDb ((float) value); return true; }
    if (id == kRatio)       { core_.setRatio       ((float) value); return true; }
    if (id == kAttackMs)    { core_.setAttackMs    (value);         return true; }
    if (id == kReleaseMs)   { core_.setReleaseMs   (value);         return true; }
    if (id == kKneeDb)      { core_.setKneeDb      ((float) value); return true; }
    if (id == kMakeupDb)    { core_.setMakeupDb    ((float) value); return true; }
    // The schema speaks percent because that is what a user and a model both
    // say; the core speaks 0..1. Converted in exactly one place.
    if (id == kMix)         { core_.setMix ((float) (value * 0.01)); return true; }
    return false;
}

double EedCompressorProcessor::getParamValue (const juce::String& id) const
{
    if (id == kThresholdDb) return (double) core_.getThresholdDb();
    if (id == kRatio)       return (double) core_.getRatio();
    if (id == kAttackMs)    return core_.getAttackMs();
    if (id == kReleaseMs)   return core_.getReleaseMs();
    if (id == kKneeDb)      return (double) core_.getKneeDb();
    if (id == kMakeupDb)    return (double) core_.getMakeupDb();
    if (id == kMix)         return (double) core_.getMix() * 100.0;
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedCompressorProcessor::prepareToPlay (double sampleRate, int)
{
    core_.prepare (sampleRate);
    core_.reset();
}

void EedCompressorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    core_.process (buffer.getWritePointer (0),
                   numCh > 1 ? buffer.getWritePointer (1) : nullptr,
                   buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedCompressorProcessor::createEditor()
{
    return new EedCompressorEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeCompressorDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Compressor";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay compressor (built in)";
        d.summary         = "Stereo-linked RMS compressor with soft knee, makeup and "
                            "parallel mix. Reach for it to control level, glue a bus "
                            "or add density; every setting is dialled in real units "
                            "rather than approximated.";
        d.identifier      = "echojay:builtin:compressor";
        d.uid             = 0x456A4350;   // 'EjCP' - frozen once shipped
        d.aliases         = { "EchoJayCompressor", "EchoJay Comp", "EchoJay Compression" };
        d.schema          = EedCompressorProcessor::schema();
        d.create          = [] { return std::make_unique<EedCompressorProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar compressorRegistrar { makeCompressorDevice() };
}
