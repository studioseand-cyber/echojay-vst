/*
    EedLimiterProcessor.cpp  —  see EedLimiterProcessor.h.
*/

#include "EedLimiterProcessor.h"
#include "EedLimiterEditor.h"
#include "EedDeviceRegistry.h"

EedLimiterProcessor::EedLimiterProcessor()
{
    core_.setMode (echojay::DynamicsMode::Limit);
    core_.setDetectorMode (echojay::DetectorMode::Peak);

    // A limiter's ceiling is a hard number: a soft knee would start reducing
    // BELOW the ceiling, which is a compressor, not a limiter. The whole promise
    // of the device is "nothing above this line", so the corner is hard.
    core_.setKneeDb (0.0f);

    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedLimiterProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kCeilingDb, "dB", -24.0, 0.0, -0.3,
          "the level nothing is allowed above; -0.3 leaves headroom for the "
          "inter-sample peaks a lossy encoder will reconstruct", false },

        { kReleaseMs, "ms", 1.0, 1000.0, 50.0,
          "how fast it lets go after a peak; short is louder and more audible, "
          "long is smoother and can duck sustained material", false },

        { kLookaheadMs, "ms", 0.0, kMaxLookaheadMs, 2.0,
          "how far ahead it looks so it can catch a transient cleanly; this is "
          "added latency, reported to the host. 0 is zero-latency and slightly "
          "grittier on sharp transients. Ignored in clip mode, which has nothing "
          "to look ahead for", false },

        // ---- the depth pass ------------------------------------------------
        { kMode, "", 0.0, (double) (kNumModes - 1), 0.0,
          "how it holds the ceiling: transparent is the clean lookahead limiter "
          "(use it on a master), punchy is faster with a touch of drive so a loud "
          "mix feels dense rather than just loud, clip is a hard ceiling - the "
          "loudest and most aggressive, and it ignores release and lookahead",
          false, { "transparent", "punchy", "clip" } },

        { kTruePeak, "", 0.0, 1.0, 0.0,
          "measure the peak BETWEEN samples, not just at them, so the ceiling "
          "still holds after a converter or a lossy encoder reconstructs the "
          "waveform. Turn it on for anything being mastered or exported; it costs "
          "a little loudness and no latency", true },

        { kScHpfHz, "Hz", 0.0, 500.0, 0.0,
          "high-pass on the detector only, so sub-bass rumble stops driving the "
          "limiter. CAUTION: anything the detector cannot hear can exceed the "
          "ceiling, so leave this at 0 (off) on a master unless you mean it", false },
    });
    return s;
}

bool EedLimiterProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kCeilingDb) { core_.setThresholdDb ((float) value); return true; }
    if (id == kScHpfHz)   { core_.setSidechainHpfHz (value);      return true; }
    if (id == kTruePeak)  { core_.setTruePeak (value >= 0.5);     return true; }

    // Release and lookahead are both stored and then re-derived, because `clip`
    // overrides what the core runs for each of them.
    if (id == kReleaseMs)   { releaseMs_   = value; applyLookahead(); return true; }
    if (id == kLookaheadMs) { lookaheadMs_ = value; applyLookahead(); return true; }

    if (id == kMode)
    {
        const int i = juce::jlimit (0, kNumModes - 1, (int) std::lround (value));
        mode_ = (Mode) i;

        // `punchy` is the shared core's punch character — a faster attack and
        // recovery and gentle drive as it works. The other two are uncoloured:
        // a limiter's job is to be inaudible unless asked otherwise.
        core_.setCharacter (mode_ == Mode::Punchy ? echojay::CharacterMode::Punch
                                                 : echojay::CharacterMode::Clean);
        applyLookahead();
        return true;
    }
    return false;
}

double EedLimiterProcessor::getParamValue (const juce::String& id) const
{
    if (id == kCeilingDb)   return (double) core_.getThresholdDb();
    if (id == kReleaseMs)   return releaseMs_;
    if (id == kLookaheadMs) return lookaheadMs_;
    if (id == kMode)        return (double) (int) mode_;
    if (id == kTruePeak)    return core_.isTruePeak() ? 1.0 : 0.0;
    if (id == kScHpfHz)     return core_.getSidechainHpfHz();
    return 0.0;
}

void EedLimiterProcessor::applyLookahead()
{
    // CLIP is a hard ceiling, and that is entirely expressed by three zeroes: no
    // delay, no attack and no release. The gain then becomes the instantaneous
    // ceiling/peak ratio applied to the sample it was measured from, which IS
    // clipping — with the one improvement that it is stereo-LINKED, so a clipped
    // transient does not pull the image toward the quieter channel.
    //
    // The delay has to go with it. A lookahead means the gain is computed from a
    // sample the audio has not reached yet, which is exactly right for a limiter
    // with an attack and exactly wrong for an instantaneous one: the clip would
    // land milliseconds away from the peak that caused it.
    const bool clip = mode_ == Mode::Clip;

    delay_.setDelayMs (clip ? 0.0 : lookaheadMs_);

    if (clip)
    {
        core_.setAttackMs (0.0);
        core_.setReleaseMs (0.0);
    }
    else
    {
        // Attack derived from the lookahead: about a third of it, so the gain is
        // ~95% of the way to its target by the time the peak arrives. With no
        // lookahead there is nothing to hide behind, so it falls back to the
        // fastest attack the core will run.
        core_.setAttackMs (lookaheadMs_ > 0.0 ? juce::jmax (0.05, lookaheadMs_ / 3.0)
                                              : 0.05);
        core_.setReleaseMs (releaseMs_);
    }

    // The number the DAW needs to keep this track in time with every other one.
    setLatencySamples (delay_.delaySamples());
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedLimiterProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    core_.prepare (sampleRate_);
    core_.reset();

    // Sized ONCE, for the schema's maximum. Every later lookahead change is a
    // read-pointer move inside this buffer, never a reallocation.
    delay_.prepare (sampleRate_, kMaxLookaheadMs, 2);
    delay_.reset();

    applyLookahead();
}

void EedLimiterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
    const int n = buffer.getNumSamples();

    // BYPASS STILL DELAYS. The device is reporting latency to the host, and the
    // host is compensating for it whether or not the device is bypassed; if
    // bypass returned the signal early, toggling it would shift this track in
    // time against the rest of the session. So bypass skips the limiting and
    // keeps the delay.
    const bool byp = isBypassed();

    for (int i = 0; i < n; ++i)
    {
        // The detector reads the input BEFORE the delay — that is the whole
        // trick: it sees the peak while the audio carrying it is still in flight.
        const float scL = l[i];
        const float scR = r != nullptr ? r[i] : l[i];
        const float g   = byp ? 1.0f : core_.gainForSidechain (scL, scR);

        float frame[2] = { l[i], r != nullptr ? r[i] : 0.0f };
        delay_.process (frame, 2);

        if (byp)
        {
            // Delayed and otherwise untouched. In particular NOT shaped: the
            // core's last gain reduction is still sitting in its meter, so
            // asking for the character here would drive a bypassed device.
            l[i] = frame[0];
            if (r != nullptr) r[i] = frame[1];
            continue;
        }

        // `punchy`'s drive, applied where a gain element physically sits: after
        // the gain, on the way out. Identity in the other two modes and identity
        // in punchy while it is not reducing, so the ceiling is never coloured by
        // a limiter that is doing nothing. It can only ever pull a sample toward
        // zero, so it cannot break the wall it sits behind.
        l[i] = core_.shapeCharacter (frame[0] * g);
        if (r != nullptr) r[i] = core_.shapeCharacter (frame[1] * g);
    }
}

juce::AudioProcessorEditor* EedLimiterProcessor::createEditor()
{
    return new EedLimiterEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeLimiterDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Limiter";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay lookahead limiter (built in)";
        d.summary         = "Stereo-linked brick-wall limiter with lookahead, true-peak "
                            "detection and three modes (transparent, punchy, clip), "
                            "reporting its latency to the host. Reach for it last in a "
                            "chain to hold a hard ceiling, or to raise loudness without "
                            "clipping; transparent + true_peak is the mastering setting.";
        d.identifier      = "echojay:builtin:limiter";
        d.uid             = 0x456A4C4D;   // 'EjLM' - frozen once shipped
        d.aliases         = { "EchoJayLimiter", "EchoJay Brickwall",
                              "EchoJay Brick Wall Limiter" };
        d.schema          = EedLimiterProcessor::schema();
        d.create          = [] { return std::make_unique<EedLimiterProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar limiterRegistrar { makeLimiterDevice() };
}
