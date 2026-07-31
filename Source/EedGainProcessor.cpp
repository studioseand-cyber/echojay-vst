/*
    EedGainProcessor.cpp  —  see EedGainProcessor.h.
*/

#include "EedGainProcessor.h"
#include "EedGainEditor.h"
#include "EedDeviceRegistry.h"

#include <cmath>

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

        // ---- the depth pass (DEVICE_DEPTH_PLAN.md, Utility) ----------------
        { kMode, "",
          0.0, (double) (echojay::kNumGainModes - 1),
          (double) (int) echojay::GainMode::Stereo,
          "stereo is plain level + pan; mid_side adds independent gain on the "
          "mid (centre) and side (stereo difference) via mid_db/side_db - "
          "raise side_db to widen, lower it to narrow, lower mid_db to make "
          "room in the centre. level_db and pan still apply after it",
          false, { "stereo", "mid_side" } },

        { kMidDb, "dB",
          (double) echojay::GainEngine::kMinDb,
          (double) echojay::GainEngine::kMaxMsDb,
          0.0,
          "mid_side mode only: gain on the MID (centre) signal; -60 removes "
          "the centre entirely, leaving only the sides", false },

        { kSideDb, "dB",
          (double) echojay::GainEngine::kMinDb,
          (double) echojay::GainEngine::kMaxMsDb,
          0.0,
          "mid_side mode only: gain on the SIDE (stereo difference) signal; "
          "positive widens (+6 is double, as wide as Stereo Width's 200), "
          "negative narrows, -60 collapses to mono", false },

        { kMono, "",
          0.0, 1.0, 0.0,
          "sum the output to mono (both channels become the centre); the "
          "quick compatibility check, and a fade rather than a click", true },

        { kPhaseLeft, "",
          0.0, 1.0, 0.0,
          "invert the LEFT channel's polarity; use it to fix a flipped "
          "cable/mic - with mono on, inverting one side of an identical pair "
          "cancels to silence", true },

        { kPhaseRight, "",
          0.0, 1.0, 0.0,
          "invert the RIGHT channel's polarity", true },
    });
    return s;
}

bool EedGainProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kLevelDb) { engine_.setLevelDb ((float) value); return true; }
    if (id == kPan)     { engine_.setPan     ((float) value); return true; }

    if (id == kMode)
    {
        engine_.setMode (echojay::gainModeFromIndex ((int) std::lround (value)));
        return true;
    }
    if (id == kMidDb)      { engine_.setMidDb      ((float) value); return true; }
    if (id == kSideDb)     { engine_.setSideDb     ((float) value); return true; }
    if (id == kMono)       { engine_.setMono       (value >= 0.5);  return true; }
    if (id == kPhaseLeft)  { engine_.setPhaseLeft  (value >= 0.5);  return true; }
    if (id == kPhaseRight) { engine_.setPhaseRight (value >= 0.5);  return true; }
    return false;
}

double EedGainProcessor::getParamValue (const juce::String& id) const
{
    if (id == kLevelDb)    return (double) engine_.getLevelDb();
    if (id == kPan)        return (double) engine_.getPan();
    if (id == kMode)       return (double) (int) engine_.getMode();
    if (id == kMidDb)      return (double) engine_.getMidDb();
    if (id == kSideDb)     return (double) engine_.getSideDb();
    if (id == kMono)       return engine_.getMono()       ? 1.0 : 0.0;
    if (id == kPhaseLeft)  return engine_.getPhaseLeft()  ? 1.0 : 0.0;
    if (id == kPhaseRight) return engine_.getPhaseRight() ? 1.0 : 0.0;
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
        d.summary         = "Exact level trim and constant-power pan, plus the common "
                            "utility jobs: mid/side gain (mid_side mode), mono sum and "
                            "per-channel polarity flip. Reach for it to gain-stage a "
                            "chain, place a source in the field, widen or narrow via "
                            "side_db, or check mono compatibility; it is dialled to "
                            "precise dB rather than approximated.";
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
