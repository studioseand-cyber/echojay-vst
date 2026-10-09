/*
    EedLimiterProcessor.cpp  —  see EedLimiterProcessor.h.
*/

#include "EedLimiterProcessor.h"
#include <atomic>
#include <cmath>
#include "EedLatencyLog.h"
#include "EedLimiterEditor.h"
#include "EedDeviceRegistry.h"

EedLimiterProcessor::EedLimiterProcessor()
{
    // The shared DynamicsCore is kept for the editor's picture only (dwell histogram, detector level): Limit mode,
    // peak detector, hard knee, so what it draws matches a brick wall. Its gain is never applied.
    core_.setMode (echojay::DynamicsMode::Limit);
    core_.setDetectorMode (echojay::DetectorMode::Peak);
    core_.setKneeDb (0.0f);

    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedLimiterProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kCeilingDb, "dB", -24.0, 0.0, 0.0,
          "the level nothing is allowed above, true-peak accurate; 0.0 is the default (Pro-L 2's), "
          "-1.0 or lower leaves headroom for a lossy encoder", false },

        { kInputDb, "dB", -12.0, 12.0, 0.0,
          "gain into the limiter, the loudness push: on a bus or master aimed at a loudness target this is "
          "the measured integrated LUFS to the genre target (e.g. -15.5 measured, -8 target -> +7.5), "
          "clamped to +12; 0 leaves the level alone", false },

        { kReleaseMs, "ms", 1.0, 1000.0, 400.0,
          "how fast the sustained reduction lets go; 400 is the default (Pro-L 2's); short is louder and "
          "more audible, long is steadier and can duck sustained material", false },

        { kAttackMs, "ms", 10.0, 2000.0, 275.0,
          "how quickly sustained level builds up reduction; 275 is the default (Pro-L 2's); shorter grabs "
          "sustained material sooner (denser), longer leaves transients and bodies alone (punchier)", false },

        { kLookaheadMs, "ms", 0.0, kMaxLookaheadMs, 0.18,
          "how far ahead it looks, and how smoothly the gain moves; 0.18 is the default (Pro-L 2's). More is "
          "smoother and softer on transients. The latency is fixed and reported to the host whatever this is", false },

        { kLinkPct, "%", 0.0, 100.0, 75.0,
          "channel linking on transients: 100 reduces both channels equally (the image never shifts), 0 treats "
          "them independently; 75 is the default (Pro-L 2's)", false },

        { kReleaseLinkPct, "%", 0.0, 100.0, 100.0,
          "channel linking on the sustained reduction: 100 (the default) keeps one reduction for both channels", false },

        // ---- the depth pass ------------------------------------------------
        { kMode, "", 0.0, (double) (kNumModes - 1), 0.0,
          "how it holds the ceiling: transparent is the clean lookahead limiter "
          "(use it on a master), punchy is faster with a touch of drive so a loud "
          "mix feels dense rather than just loud, clip is a hard ceiling - the "
          "loudest and most aggressive, and it ignores release and lookahead",
          false, { "transparent", "punchy", "clip" } },

        { kTruePeak, "", 0.0, 1.0, 1.0,
          "measure the peak BETWEEN samples, not just at them, so the ceiling "
          "still holds after a converter or a lossy encoder reconstructs the "
          "waveform. On by default (8 Oct 2026); off is sample-peak limiting", true },

        { kScHpfHz, "Hz", 0.0, 500.0, 0.0,
          "high-pass on the detector only, so sub-bass rumble stops driving the "
          "limiter. CAUTION: anything the detector cannot hear can exceed the "
          "ceiling, so leave this at 0 (off) on a master unless you mean it", false },
    });
    return s;
}

bool EedLimiterProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kCeilingDb) { core_.setThresholdDb ((float) value); engine_.setCeilingDb (value); return true; }
    if (id == kInputDb)   { inputDb_ = value; inputGain_ = (float) std::pow (10.0, value / 20.0); engine_.setInputGainDb (value); return true; }
    if (id == kScHpfHz)   { core_.setSidechainHpfHz (value); engine_.setSidechainHpfHz (value); return true; }
    if (id == kTruePeak)  { truePeakOn_ = value >= 0.5; core_.setTruePeak (truePeakOn_); engine_.setTruePeak (truePeakOn_); return true; }

    // Release, attack, lookahead, the links and mode are stored and then re-derived together into the engine's Tuning.
    if (id == kReleaseMs)      { releaseMs_      = value; applyLookahead(); return true; }
    if (id == kAttackMs)       { attackMs_       = value; applyLookahead(); return true; }
    if (id == kLookaheadMs)    { lookaheadMs_    = value; applyLookahead(); return true; }
    if (id == kLinkPct)        { linkPct_        = value; applyLookahead(); return true; }
    if (id == kReleaseLinkPct) { releaseLinkPct_ = value; applyLookahead(); return true; }

    if (id == kMode)
    {
        const int i = juce::jlimit (0, kNumModes - 1, (int) std::lround (value));
        mode_ = (Mode) i;
        applyLookahead();
        return true;
    }
    return false;
}

double EedLimiterProcessor::getParamValue (const juce::String& id) const
{
    if (id == kCeilingDb)   return (double) core_.getThresholdDb();
    if (id == kInputDb)     return inputDb_;
    if (id == kReleaseMs)      return releaseMs_;
    if (id == kAttackMs)       return attackMs_;
    if (id == kLookaheadMs)    return lookaheadMs_;
    if (id == kLinkPct)        return linkPct_;
    if (id == kReleaseLinkPct) return releaseLinkPct_;
    if (id == kMode)        return (double) (int) mode_;
    if (id == kTruePeak)    return truePeakOn_ ? 1.0 : 0.0;
    if (id == kScHpfHz)     return core_.getSidechainHpfHz();
    return 0.0;
}

void EedLimiterProcessor::applyLookahead()
{
    // Every mode runs the Transparent tuning (8 Oct 2026); punchy and clip keep their dial value, recorded as not yet
    // tuned. The knobs are LITERAL (see the header): at the schema defaults this is exactly limv2::transparent().
    echojay::limv2::Tuning t = echojay::limv2::transparent();
    const echojay::limv2::Tuning base = echojay::limv2::transparent();
    t.lookaheadMs   = base.lookaheadMs * juce::jlimit (0.0, kMaxLookaheadMs, lookaheadMs_) / 0.18;   // label 0.18 = the tuned window (0.06 ms); label/3
    t.slowReleaseMs = base.slowReleaseMs * juce::jlimit (1.0, 1000.0, releaseMs_) / 400.0;
    t.slowAttackMs  = base.slowAttackMs  * juce::jlimit (10.0, 2000.0, attackMs_) / 275.0;
    t.slowAttack2Ms = base.slowAttack2Ms * juce::jlimit (10.0, 2000.0, attackMs_) / 275.0;
    t.link          = juce::jlimit (0.0, 100.0, linkPct_) / 100.0;
    t.linkRelease   = juce::jlimit (0.0, 100.0, releaseLinkPct_) / 100.0;
    engine_.setTuning (t);

    // The editor's picture: the shared detector follows the window it used to (about a third of the lookahead).
    core_.setAttackMs (lookaheadMs_ > 0.0 ? juce::jmax (0.05, lookaheadMs_ / 3.0) : 0.05);
    core_.setReleaseMs (releaseMs_);

    // Fixed by construction (the engine's maximum, every setting), so this number only changes with the sample rate.
    ejSetLatencyLogged (*this, engine_.latencySamples(), "EedLimiterProcessor #1");
}

void EedLimiterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    EedDeviceProcessor::setStateInformation (data, sizeInBytes);   // full replace: defaults, then the saved values
    if (data == nullptr || sizeInBytes <= 0) return;
    const juce::var parsed = juce::JSON::parse (juce::String::createStringFromData (data, sizeInBytes));
    const juce::var params = parsed.isObject() ? parsed.getProperty ("params", juce::var()) : juce::var();
    if (! params.isObject()) return;
    const bool preV2 = ! params.hasProperty (kAttackMs);
    const bool oldDefaults = std::abs (lookaheadMs_ - kOldDefaultLookaheadMs) < 1e-9 && std::abs (releaseMs_ - kOldDefaultReleaseMs) < 1e-9;
    if (preV2 && oldDefaults)
    {
        // the old defaults from an old build mean "the limiter as it came": load the limiter as it comes NOW
        if (const auto* la = schema().find (kLookaheadMs)) lookaheadMs_ = la->def;
        if (const auto* re = schema().find (kReleaseMs))   releaseMs_   = re->def;
        applyLookahead();
    }
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedLimiterProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    core_.prepare (sampleRate_);
    core_.reset();
    inMeter_.prepare (sampleRate_); outMeter_.prepare (sampleRate_);   // 18e (item 4)

    // Sized ONCE, for the largest window the lookahead dial can ask for (5x the tuned window at 10 ms) with true
    // peak on; every later change is a window or coefficient change inside that storage, never an allocation.
    echojay::limv2::Tuning t = echojay::limv2::transparent();
    t.maxLookaheadMs = t.lookaheadMs * kMaxLookaheadMs / 0.18;   // the window the knob's 5 ms label asks for (1.67 ms)
    engine_.prepare (sampleRate_, t);
    meterTap_.prepare (sampleRate_); engine_.setMeterTap (&meterTap_);   // the panel's tap, sized here, never on the audio thread
    engine_.setFixedLatency (true);
    engine_.setCeilingDb (core_.getThresholdDb());
    engine_.setInputGainDb (inputDb_);
    engine_.setTruePeak (truePeakOn_);
    engine_.setSidechainHpfHz (core_.getSidechainHpfHz());
    engine_.reset();

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

    // BYPASS STILL DELAYS (see the header): the engine keeps its delay and its detector running and crossfades the
    // gain to unity over 10 ms, so toggling bypass neither shifts the track in time nor clicks.
    const bool byp = isBypassed();
    engine_.setBypassed (byp);

    {   // 18e (item 4): the INPUT meter reads the signal INTO the limiter (after input_db): a scaled copy at the dialled gain.
        // The shared detector is fed the same copy, for the editor's dwell histogram and detector level only.
        const float ig = byp ? 1.0f : inputGain_;
        float tl[512], tr[512];
        for (int off = 0; off < n; off += 512)
        {
            const int m = juce::jmin (512, n - off);
            for (int i = 0; i < m; ++i) { tl[i] = l[off + i] * ig; tr[i] = r != nullptr ? r[off + i] * ig : tl[i]; }
            inMeter_.push (tl, tr, m);
            if (! byp) for (int i = 0; i < m; ++i) (void) core_.gainForSidechain (tl[i], tr[i]);
        }
    }

    float* chs[2] = { l, r };
    engine_.process (chs, r != nullptr ? 2 : 1, n);

    float pk = 0.0f;
    for (int i = 0; i < n; ++i) pk = juce::jmax (pk, std::abs (l[i]), r != nullptr ? std::abs (r[i]) : 0.0f);
    wallGrDb_.store (byp ? 0.0f : engine_.gainReductionDb(), std::memory_order_relaxed);
    blockGrDb_.store (byp ? 0.0f : engine_.blockGainReductionDb(), std::memory_order_relaxed);
    if (pk > outPeakMax_.load (std::memory_order_relaxed)) outPeakMax_.store (pk, std::memory_order_relaxed);
    outMeter_.push (l, r, n);   // 18e (item 4): the OUTPUT meter
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
