/*
    EedDelayProcessor.cpp  —  see EedDelayProcessor.h.
*/

#include "EedDelayProcessor.h"
#include "EedDelayEditor.h"
#include "EedDeviceRegistry.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
// Ranges here are the SAME numbers DelayEngine clamps to — the schema is what
// the model is taught and what the server validates against, the engine is what
// enforces it in DSP. Written once, next to each other, so a change to one that
// misses the other is obvious in review.
//
// Defaults are a usable delay on insert, not a null one: 350 ms at 35% feedback
// and 30% mix is an eighth-ish slap that a user hears immediately. A device that
// does nothing until six knobs are moved reads as broken.
const echojay::ParamSchema& EedDelayProcessor::schema()
{
    using E = echojay::DelayEngine;

    static const echojay::ParamSchema s ({
        { kTimeMs, "ms",
          (double) E::kMinTimeMs, (double) E::kMaxTimeMs, 350.0,
          "echo time when sync is off; ignored while sync is on", false },

        { kSync, "",
          0.0, 1.0, 0.0,
          "lock the echo time to the host tempo instead of using time_ms", true },

        // Advertised as an index rather than a note name because the contract is
        // numeric end to end: the model sets a number, the server clamps a
        // number. The names are spelled out here so the choice is still musical
        // rather than a guess. T = triplet, D = dotted.
        { kDivision, "",
          0.0, (double) (E::kNumDivisions - 1), 6.0,
          "note length when sync is on, shortest to longest: "
          "0=1/32 1=1/16T 2=1/32D 3=1/16 4=1/8T 5=1/16D 6=1/8 7=1/4T 8=1/8D "
          "9=1/4 10=1/2T 11=1/4D 12=1/2 13=1/2D 14=1/1", false },

        { kFeedback, "%",
          (double) E::kMinFeedbackPct, (double) E::kMaxFeedbackPct, 35.0,
          "how much of each echo is fed back; 0 is a single repeat, "
          "100 is a very long tail that still decays", false },

        { kMix, "%",
          (double) E::kMinMixPct, (double) E::kMaxMixPct, 30.0,
          "wet/dry balance; 0 is bypass-clean dry, 100 is echoes only", false },

        { kFilterHpHz, "Hz",
          (double) E::kMinHpHz, (double) E::kMaxHpHz, 120.0,
          "high-pass inside the feedback loop; raise it to stop repeats "
          "building up mud under the source", false },

        { kFilterLpHz, "Hz",
          (double) E::kMinLpHz, (double) E::kMaxLpHz, 6000.0,
          "low-pass inside the feedback loop; lower it to make each repeat "
          "darker than the last, the way a tape echo does", false },

        { kPingPong, "",
          0.0, 1.0, 0.0,
          "bounce the echoes left-right instead of keeping each side on its own "
          "channel. Works with ANY mode, so this is how to get a bouncing tape or "
          "analog echo; mode=pingpong is the clean digital bounce on its own", true },

        { kStereoOffset, "%",
          (double) E::kMinOffsetPct, (double) E::kMaxOffsetPct, 0.0,
          "offset the right channel's time as a percentage of the left's; "
          "small values widen the echoes, large ones make them stumble", false },

        { kModRateHz, "Hz",
          (double) E::kMinModRateHz, (double) E::kMaxModRateHz, 0.4,
          "speed of the delay-time wobble", false },

        { kModDepthMs, "ms",
          (double) E::kMinModDepthMs, (double) E::kMaxModDepthMs, 0.0,
          "how far the delay time wobbles; a little detunes the repeats "
          "like tape, a lot is a chorus", false },

        // ---- the depth pass (DEVICE_DEPTH_PLAN.md, Time) -------------------
        // The marquee control. Everything it does happens INSIDE the feedback
        // loop, which is why the description can promise a progression: the first
        // repeat is clean and each one after it is further gone.
        { kMode, "",
          0.0, (double) (echojay::kNumDelayModes - 1), 0.0,
          "the character of the repeats, applied inside the feedback loop so it "
          "builds up over successive echoes: digital is clean and transparent, tape "
          "saturates and adds wow and flutter so each repeat is darker and drifts "
          "slightly, analog is a bucket-brigade line - darker still, band-limited "
          "and a little gritty, pingpong is the clean digital sound bounced hard "
          "left-right. To bounce a tape or analog echo, set mode and turn ping_pong "
          "on as well",
          false, { "digital", "tape", "analog", "pingpong" } },

        { kDiffusion, "%",
          (double) E::kMinDiffusionPct, (double) E::kMaxDiffusionPct, 0.0,
          "smear the repeats toward a reverby wash instead of discrete echoes; 0 is "
          "a clean delay, high turns the tail into something closer to a reverb "
          "while keeping the rhythm of the delay time", false },

        { kDuck, "%",
          (double) E::kMinDuckPct, (double) E::kMaxDuckPct, 0.0,
          "how much the repeats duck under the DRY signal, so the echoes stay out of "
          "the way while the source plays and come up in the gaps. The standard trick "
          "for a delay on a lead vocal; 0 is off, 40-70 is the usual range", false },
    });
    return s;
}

bool EedDelayProcessor::setParamValue (const juce::String& id, double value)
{
    // `value` arrives ALREADY clamped to the schema range — never re-clamp here.
    if (id == kTimeMs)       { engine_.setTimeMs ((float) value);           return true; }
    if (id == kSync)         { engine_.setSync (value >= 0.5);              return true; }
    if (id == kDivision)     { engine_.setDivision ((int) std::lround (value)); return true; }
    if (id == kFeedback)     { engine_.setFeedbackPct ((float) value);      return true; }
    if (id == kMix)          { engine_.setMixPct ((float) value);           return true; }
    if (id == kFilterHpHz)   { engine_.setHighpassHz ((float) value);       return true; }
    if (id == kFilterLpHz)   { engine_.setLowpassHz ((float) value);        return true; }
    if (id == kPingPong)     { engine_.setPingPong (value >= 0.5);          return true; }
    if (id == kStereoOffset) { engine_.setStereoOffsetPct ((float) value);  return true; }
    if (id == kModRateHz)    { engine_.setModRateHz ((float) value);        return true; }
    if (id == kModDepthMs)   { engine_.setModDepthMs ((float) value);       return true; }
    if (id == kDiffusion)    { engine_.setDiffusionPct ((float) value);     return true; }
    if (id == kDuck)         { engine_.setDuckPct ((float) value);          return true; }

    if (id == kMode)
    {
        engine_.setMode (echojay::delayModeFromIndex ((int) std::lround (value)));
        return true;
    }
    return false;
}

double EedDelayProcessor::getParamValue (const juce::String& id) const
{
    if (id == kTimeMs)       return (double) engine_.getTimeMs();
    if (id == kSync)         return engine_.getSync() ? 1.0 : 0.0;
    if (id == kDivision)     return (double) engine_.getDivision();
    if (id == kFeedback)     return (double) engine_.getFeedbackPct();
    if (id == kMix)          return (double) engine_.getMixPct();
    if (id == kFilterHpHz)   return (double) engine_.getHighpassHz();
    if (id == kFilterLpHz)   return (double) engine_.getLowpassHz();
    if (id == kPingPong)     return engine_.getPingPong() ? 1.0 : 0.0;
    if (id == kStereoOffset) return (double) engine_.getStereoOffsetPct();
    if (id == kModRateHz)    return (double) engine_.getModRateHz();
    if (id == kModDepthMs)   return (double) engine_.getModDepthMs();
    if (id == kMode)         return (double) (int) engine_.getMode();
    if (id == kDiffusion)    return (double) engine_.getDiffusionPct();
    if (id == kDuck)         return (double) engine_.getDuckPct();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedDelayProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    // Snap to the current targets so a freshly restored session starts AT its
    // values rather than sweeping into them from wherever it was left.
    engine_.reset();
}

void EedDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

double EedDelayProcessor::getTailLengthSeconds() const
{
    const double t  = engine_.effectiveTimeMs() * 0.001;
    const double fb = (double) engine_.getFeedbackPct() * 0.01;

    if (fb <= 0.001) return t;          // one repeat and it is over
    if (fb >= 0.999) return 30.0;       // very long, but softClip guarantees finite

    // Repeats needed to fall 60 dB, each one costing 20*log10(fb) dB. Capped at
    // 30 s: past that the host is reserving render time for something inaudible.
    const double repeats = 60.0 / (-20.0 * std::log10 (fb));
    return juce::jlimit (t, 30.0, t * repeats);
}

juce::AudioProcessorEditor* EedDelayProcessor::createEditor()
{
    return new EedDelayEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeDelayDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Delay";
        d.category        = "Time";
        d.descriptiveName = "EchoJay stereo delay (built in)";

        // ASCII ONLY: juce::String's const char* constructor reads its input as
        // ASCII, so a UTF-8 dash here ships mojibake into the AI prompt.
        d.summary         = "Tempo-syncable stereo echo with four modes (digital, tape, "
                            "analog, pingpong), a filtered feedback loop, diffusion and "
                            "ducking from the dry signal. Reach for it to add depth or "
                            "rhythmic space; set mode for the character of the repeats, "
                            "time_ms directly (or sync on plus sync_division for a note "
                            "length), and duck to keep the echoes off a lead vocal.";
        d.identifier      = "echojay:builtin:delay";
        d.uid             = 0x456A444C;   // 'EjDL' - frozen once shipped
        d.aliases         = { "EchoJayDelay", "EchoJay Echo", "EchoJay Ping Pong" };
        d.schema          = EedDelayProcessor::schema();
        d.create          = [] { return std::make_unique<EedDelayProcessor>(); };
        return d;
    }

    // File-scope static: this line, and nothing else, integrates the device.
    // See CMakeLists.txt's force-load block for why it survives static linking.
    const BuiltinDeviceRegistrar delayRegistrar { makeDelayDevice() };
}
