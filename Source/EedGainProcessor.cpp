/*
    EedGainProcessor.cpp  —  see EedGainProcessor.h.
*/

#include "EedGainProcessor.h"
#include "EedGainEditor.h"
#include "EedDeviceRegistry.h"

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedGainProcessor::schema()
{
    // Ranges here are the SAME numbers GainEngine clamps to. The schema is what
    // the model is taught and what the server validates against; the engine is
    // what enforces it in DSP. They are written once, next to each other, so a
    // change to one that misses the other is obvious in review.
    static const echojay::ParamSchema s ({
        { kLevelDb, "dB",
          (double) echojay::GainEngine::kMinDb,
          (double) echojay::GainEngine::kMaxDb,
          0.0,
          "output level; -60 is silence, 0 is unity", false },

        { kPan, "",
          -1.0, 1.0, 0.0,
          "stereo position: -1 hard left, 0 centre, +1 hard right "
          "(constant power, centre is unity)", false },
    });
    return s;
}

bool EedGainProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kLevelDb) { engine_.setLevelDb ((float) value); return true; }
    if (id == kPan)     { engine_.setPan     ((float) value); return true; }
    return false;
}

double EedGainProcessor::getParamValue (const juce::String& id) const
{
    if (id == kLevelDb) return (double) engine_.getLevelDb();
    if (id == kPan)     return (double) engine_.getPan();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedGainProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    // Snap to the current targets so a freshly restored session starts AT its
    // values rather than ramping up to them from unity on the first block.
    engine_.reset();

    // Clear the meters, or a device that has just been re-prepared shows the
    // level of whatever was playing before the transport stopped.
    inPeak_.set (0.0f);
    inRms_.set (0.0f);
    outPeak_.set (0.0f);
    outRms_.set (0.0f);
}

// Peak is the loudest single sample on ANY channel — that is what clips. RMS is
// pooled across channels rather than taken per channel, because a meter is
// answering "how loud is this", and that is one number for the signal, not one
// per wire.
void EedGainProcessor::publishLevels (const juce::AudioBuffer<float>& buffer, int numCh,
                                      echojay::viz::FloatTap& peakTap,
                                      echojay::viz::FloatTap& rmsTap)
{
    const int n = buffer.getNumSamples();
    if (n <= 0 || numCh <= 0) return;

    float  peak     = 0.0f;
    double meanSqSum = 0.0;

    for (int ch = 0; ch < numCh; ++ch)
    {
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, n));

        const double r = (double) buffer.getRMSLevel (ch, 0, n);
        meanSqSum += r * r;
    }

    peakTap.set (peak);
    rmsTap.set ((float) std::sqrt (meanSqSum / (double) numCh));
}

void EedGainProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel the host gave us that has no input behind it,
    // otherwise it carries whatever was left in the buffer.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // A bypassed device stops metering rather than publishing in == out. The
    // editor dims both meters and stops reading them, the same choice the GR
    // meter and the goniometer make: a live-looking picture of processing that
    // is not happening is worse than no picture.
    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    // The input side, measured BEFORE the engine rewrites the buffer in place —
    // there is no second copy of it afterwards.
    publishLevels (buffer, numCh, inPeak_, inRms_);

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());

    publishLevels (buffer, numCh, outPeak_, outRms_);
}

juce::AudioProcessorEditor* EedGainProcessor::createEditor()
{
    return new EedGainEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeGainDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Gain";
        d.category        = "Utility";
        d.descriptiveName = "EchoJay level + pan (built in)";
        d.summary         = "Exact level trim and constant-power pan. Reach for it to "
                            "gain-stage a chain or place a source in the stereo field; "
                            "it is dialled to precise dB rather than approximated.";
        d.identifier      = "echojay:builtin:gain";
        d.uid             = 0x456A474E;   // 'EjGN' — frozen once shipped
        d.aliases         = { "EchoJayGain", "EchoJay Level", "EchoJay Trim" };
        d.schema          = EedGainProcessor::schema();
        d.create          = [] { return std::make_unique<EedGainProcessor>(); };
        return d;
    }

    // File-scope static: this line, and nothing else, integrates the device.
    // See CMakeLists.txt's force-load block for why it survives static linking.
    const BuiltinDeviceRegistrar gainRegistrar { makeGainDevice() };
}
