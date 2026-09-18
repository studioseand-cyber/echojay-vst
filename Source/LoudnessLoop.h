#pragma once
// LoudnessLoop (18 Sep 2026 ruling G, the interactive closed loop): deterministic inside V2, no server round-trip.
//
//  Arm      a chain whose EchoJay Limiter settings carry a LUFS target ("... to the -8 LUFS target ...") arms the loop
//           on land. Bubble: "Chain built. Play the loudest part of the song ..."
//  Measure  BS.1770 gated integrated LUFS at the chain OUTPUT (ChainHost chainOutTally_, K-weighted, -70 abs /
//           -10 rel gates); only hops above -40 LUFS count toward the 10 s (LevelTally::setCountFloor), so a stop,
//           a gap or a fade is not counted; wall clock 60 s with nothing counted -> "still waiting for audio".
//  Pass 1   trim = clamp(target - measured, +-6 dB) on the limiter's input_db (the limiter eases it over 50 ms).
//  Pass 2   same, then HOLD: "Hitting -8.2 LUFS, target -8. Peaks -0.1 dBTP, limiter working 3-5 dB." Never a
//           third pass, never a re-trim on a later play, never while a knob is being dragged (EJKnobGesture).
//  Edges    total push needed > +12 dB -> no clamp, the bubble says what it would take and offers "push it";
//           average GR in pass 2 > 6 dB -> the bubble OFFERS a clipper before the limiter or a -1 dB target.
//  Verbs    "a bit louder" / "a bit softer" -> target +-1, one pass; "check the level again" -> re-arm; "undo" ->
//           restore the pre-loop input_db.
// The tick is a public function (tickNow) so a harness drives it against its own audio; the plugin drives it
// from a 250 ms juce::Timer.
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedLimiterProcessor.h"
#include "EJKnobGesture.h"
#include <cmath>
#include <functional>
#include <limits>

class LoudnessLoop : private juce::Timer
{
public:
    enum class State { idle, waitAudio, measuring, hold };
    struct Bubble { juce::String text; float progress = -1.0f; bool replace = false; bool final = false; };

    static constexpr float kCountFloorLufs   = -40.0f;
    static constexpr float kNeedSeconds      = 10.0f;
    static constexpr float kPassClampDb      = 6.0f;
    static constexpr float kTotalPushLimitDb = 12.0f;
    static constexpr float kGrOfferDb        = 6.0f;
    static constexpr int   kWaitWallMs       = 60000;
    static constexpr int   kTickMs           = 250;

    explicit LoudnessLoop (ChainHost& host) : host_ (host) {}
    ~LoudnessLoop() override { stopTimer(); }

    std::function<void (const Bubble&)> onBubble;                          // message thread
    std::function<bool()>               isPlaying  { [] { return true; } };
    std::function<bool()>               knobGestureOpen { [] { return echojay::knobGestureOpen(); } };
    std::function<juce::int64()>        nowMs      { [] { return juce::Time::currentTimeMillis(); } };

    // A chain whose EchoJay Limiter settings text names a LUFS target arms the loop. Returns the target or NaN.
    static float targetFromSettingsText (const juce::String& settings)
    {
        const auto low = settings.toLowerCase();
        const int at = low.indexOf ("lufs target");
        if (at < 0) return std::numeric_limits<float>::quiet_NaN();
        int e = at; while (e > 0 && low[e - 1] == ' ') --e;
        int b = e; while (b > 0 && (juce::CharacterFunctions::isDigit (low[b - 1]) || low[b - 1] == '.' || low[b - 1] == '-')) --b;
        const auto num = low.substring (b, e);
        if (num.isEmpty() || num == "-" ) return std::numeric_limits<float>::quiet_NaN();
        return (float) num.getDoubleValue();
    }
    bool armFromChain (int passes = 2)
    {
        for (int i = host_.getNumSlots() - 1; i >= 0; --i)
        {
            const auto info = host_.getSlotInfo (i);
            if (info.name != "EchoJay Limiter") continue;
            const float t = targetFromSettingsText (info.settings);
            if (std::isfinite (t)) { arm (t, i, passes); return true; }
        }
        return false;
    }
    void arm (float targetLufs, int limiterSlot, int passes = 2)
    {
        auto* lim = limiter (limiterSlot);
        if (lim == nullptr) return;
        target_ = targetLufs; slot_ = limiterSlot; passesLeft_ = passes; pass_ = 0;
        if (! haveUndo_) { preLoopInputDb_ = lim->inputDb(); haveUndo_ = true; }
        startPass();
        emit ("Chain built. Play the loudest part of the song - the chorus or the drop - and I'll set the level after about ten seconds of audio.", 0.0f, false, false);
        if (! juce::MessageManager::getInstanceWithoutCreating() || ! isTimerRunning()) startTimer (kTickMs);
    }
    void nudgeTarget (float deltaDb)
    {
        if (slot_ < 0 || limiter (slot_) == nullptr) return;
        target_ += deltaDb; passesLeft_ = 1; pass_ = 0; startPass();
        emit (juce::String ("Target now ") + fmt (target_) + " LUFS. Play the loudest part again - one more pass.", 0.0f, false, false);
        if (! isTimerRunning()) startTimer (kTickMs);
    }
    void recheck() { if (slot_ >= 0 && limiter (slot_) != nullptr) { passesLeft_ = 2; pass_ = 0; startPass(); emit ("Checking the level again - play the loudest part.", 0.0f, false, false); if (! isTimerRunning()) startTimer (kTickMs); } }
    bool undo()
    {
        auto* lim = slot_ >= 0 ? limiter (slot_) : nullptr;
        if (lim == nullptr || ! haveUndo_) return false;
        writeInputDb (preLoopInputDb_);
        state_ = State::hold; stopTimer();
        emit (juce::String ("Restored the limiter input gain to ") + fmtSigned (preLoopInputDb_) + " dB (before the loop).", -1.0f, false, true);
        return true;
    }

    State state() const noexcept { return state_; }
    bool  isArmed() const noexcept { return state_ == State::waitAudio || state_ == State::measuring; }
    bool  everArmed() const noexcept { return slot_ >= 0; }
    int   pass() const noexcept { return pass_; }
    float target() const noexcept { return target_; }
    float lastMeasured() const noexcept { return measured_[1] > -200.0f ? measured_[1] : measured_[0]; }
    float measuredPass (int p) const noexcept { return p >= 1 && p <= 2 ? measured_[p - 1] : std::numeric_limits<float>::quiet_NaN(); }
    float countedSeconds() const { return host_.getChainOutLevels().heardAboveSeconds; }
    juce::String lastBubble() const { return lastBubble_; }

    // The tick. The plugin calls it from the timer; a harness calls it directly between audio blocks.
    void tickNow()
    {
        if (state_ != State::waitAudio && state_ != State::measuring) return;
        auto* lim = limiter (slot_);
        if (lim == nullptr) { state_ = State::hold; stopTimer(); emit ("The limiter is no longer in the chain - level loop stopped.", -1.0f, false, true); return; }
        const auto out = host_.getChainOutLevels();
        const float counted = out.heardAboveSeconds;
        // GR samples ride every tick while audio is counted (the limiter's own meter)
        if (counted > lastCounted_ + 0.05f)
        {
            const float gr = -lim->gainReductionDb();
            grMin_ = juce::jmin (grMin_, gr); grMax_ = juce::jmax (grMax_, gr); grSum_ += gr; ++grN_;
            state_ = State::measuring;
        }
        lastCounted_ = counted;
        const float progress = juce::jlimit (0.0f, 1.0f, counted / kNeedSeconds);
        if (counted < kNeedSeconds)
        {
            if (counted <= 0.0f && ! waitingSaid_ && nowMs() - passStartMs_ >= kWaitWallMs)
            { waitingSaid_ = true; emit ("Still waiting for audio - play the loudest part of the song and I'll set the level.", 0.0f, true, false); }
            else if (counted > 0.0f) emit (pass_ == 0 ? "Listening..." : "Checking...", progress, true, false);
            return;
        }
        if (knobGestureOpen()) return;                                   // never trim under a hand: try the next tick
        const float measured = out.levelDb;
        if (! std::isfinite (measured)) return;
        measured_[juce::jmin (1, pass_)] = measured; ++pass_;
        const float needed = target_ - measured;
        const float mixReads = measured - (float) lim->inputDb();          // the raw mix: what the limiter is fed before its push
        const float totalPush = target_ - mixReads;                         // everything the push would have to be, open-loop gain included
        if (pass_ == 1 && totalPush > kTotalPushLimitDb)
        {   // the edge: say what it would take, offer, change nothing (no silent clamp)
            state_ = State::hold; stopTimer();
            emit ("Your mix reads " + fmt (mixReads) + " LUFS; hitting " + fmt (target_) + " would need " + fmtSigned (totalPush) + " dB. I can push the compressor and saturator first - say 'push it'.", -1.0f, true, true);
            return;
        }
        const float trim = juce::jlimit (-kPassClampDb, kPassClampDb, needed);
        const double newDb = juce::jlimit (-12.0, 12.0, lim->inputDb() + (double) trim);
        writeInputDb (newDb);
        --passesLeft_;
        if (passesLeft_ > 0)
        {
            emit ("Measured " + fmt (measured) + ". Pushing " + fmtSigned (trim) + " dB - checking.", 1.0f, true, false);
            startPass();
            return;
        }
        // final: HOLD
        state_ = State::hold; stopTimer();
        const float peak = lim->outputPeakDbMax();
        const float grAvg = grN_ > 0 ? grSum_ / (float) grN_ : 0.0f;
        juce::String text = "Hitting " + fmt (measured) + " LUFS, target " + fmt (target_) + "."
            + (std::abs (trim) >= 0.05f ? " Trimmed " + fmtSigned (trim) + " dB." : juce::String())
            + " Peaks " + fmt (peak) + " dBTP, limiter working " + fmt (juce::jmax (0.0f, grMin_ == std::numeric_limits<float>::max() ? 0.0f : grMin_)) + "-" + fmt (juce::jmax (0.0f, grMax_)) + " dB.";
        if (grAvg > kGrOfferDb)
            text += " The limiter is averaging " + fmt (grAvg) + " dB of reduction: a clipper before it, or a " + fmt (target_ - 1.0f) + " target, would be gentler - say which.";
        emit (text, -1.0f, true, true);
    }

private:
    void timerCallback() override { tickNow(); }
    EedLimiterProcessor* limiter (int slot) const
    {
        if (slot < 0 || slot >= host_.getNumSlots()) return nullptr;
        if (host_.getSlotInfo (slot).name != "EchoJay Limiter") return nullptr;
        return dynamic_cast<EedLimiterProcessor*> (host_.getSlotProcessor (slot));
    }
    void startPass()
    {
        host_.setChainOutCountFloor (kCountFloorLufs);
        host_.resetChainOutLevels();
        if (auto* lim = limiter (slot_)) lim->resetOutputPeak();
        lastCounted_ = 0.0f; waitingSaid_ = false; passStartMs_ = nowMs();
        grMin_ = std::numeric_limits<float>::max(); grMax_ = 0.0f; grSum_ = 0.0f; grN_ = 0;
        state_ = State::waitAudio;
    }
    void writeInputDb (double db)
    {   // through the host so the card and the dial info follow; the limiter eases the step over 50 ms
        auto* p = new juce::DynamicObject(); p->setProperty ("input_db", db);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (p));
        host_.setSlotStructuredSettings (slot_, juce::var (w));
        if (auto* lim = limiter (slot_)) if (std::abs (lim->inputDb() - db) > 0.01) lim->setParamValue ("input_db", db);   // do-not-dial refuses the Assistant path: the user asked for this loop
    }
    void emit (const juce::String& text, float progress, bool replace, bool final)
    {
        lastBubble_ = text;
        if (onBubble) { Bubble b; b.text = text; b.progress = progress; b.replace = replace; b.final = final; onBubble (b); }
    }
    static juce::String fmt (float v) { return juce::String (v, 1).replace ("-0.0", "0.0"); }
    static juce::String fmtSigned (float v) { return (v >= 0.0f ? "+" : "") + fmt (v); }

    ChainHost& host_;
    State state_ = State::idle;
    int   slot_ = -1, passesLeft_ = 0, pass_ = 0;
    float target_ = -9.0f;
    float measured_[2] { -999.0f, -999.0f };
    double preLoopInputDb_ = 0.0; bool haveUndo_ = false;
    float lastCounted_ = 0.0f; bool waitingSaid_ = false; juce::int64 passStartMs_ = 0;
    float grMin_ = std::numeric_limits<float>::max(), grMax_ = 0.0f, grSum_ = 0.0f; int grN_ = 0;
    juce::String lastBubble_;
};
