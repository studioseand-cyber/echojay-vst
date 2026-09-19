#pragma once
#define EJ_LOUDNESSLOOP_ARMSOURCE 1   // 18d: armSource() / loudnessOption() / ceilingDb() exist on this build
#define EJ_LOUDNESSLOOP_V2 1          // 18e: the Level slot, max short-term, ask-before-apply, the verbs, EJLoudness lines
// LoudnessLoop v2 (18e, 19 Sep 2026): deterministic inside V2, no server round-trip.
//
//  Arm      a chain whose Level slot (or, for an older server's chain, whose EchoJay Limiter) carries target_lufs in
//           its structured params (text fallback) arms the loop at build finish. If the chain has a target but no
//           "EchoJay Level" slot, the editor inserts one before the last slot first (armLoudnessLoopIfTargeted).
//  Drive    THE LEVEL SLOT'S gain_db, never a limiter parameter. The last slot - any brand - only holds the ceiling.
//  Measure  the loudest 3 s (max short-term LUFS-S, LevelTally::maxShortTermDb) across a window of 10 s of counted
//           audio (hops above -40 LUFS) at the chain OUTPUT; the INPUT side (before the Level slot) is measured
//           over the same window for the window sanity check.
//  Sanity   counted audio >= 3 dB under the build-time integrated input -> "that sounded like a quiet section -
//           play the chorus and I'll try again", nothing applied, the window restarts.
//  Ask      "Measured -X LUFS (loudest 3 s). Push +Y dB to reach -8? say go" -> apply on go/apply -> re-measure ->
//           propose again; up to four rounds, then "stuck at -X: the limiter is working N dB average, up to M dB on
//           the hits - say push it or leave it". Numbers always, including at the +24 dB Level ceiling.
//  Track    after applying, keep measuring max short-term; a later section more than 1 dB over the target ->
//           "that section is running -X, 1.4 dB over the target - back off 1.4 dB? say go" (ask, never apply).
//  Verbs    go/apply, push it (raise the Level by the shortfall, clamped, one pass), a bit louder / softer (target
//           +-1, one pass), undo, leave it, check the level again. All client-side (PluginEditor::handleLoudnessVerb),
//           never a chat.
//  Log      every measurement, trim, branch, GR range and bubble as an "EJLoudness:" line (logLine hook).
// The tick is a public function (tickNow) so a harness drives it against its own audio; the plugin drives it from
// a 250 ms juce::Timer.
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedLimiterProcessor.h"
#include "EedLevelProcessor.h"
#include "EJKnobGesture.h"
#include <cmath>
#include <functional>
#include <limits>

class LoudnessLoop : private juce::Timer
{
public:
    enum class State { idle, waitAudio, measuring, proposed, hold, tracking };
    struct Bubble { juce::String text; float progress = -1.0f; bool replace = false; bool final = false; };

    static constexpr float kCountFloorLufs   = -40.0f;
    static constexpr float kNeedSeconds      = 10.0f;
    static constexpr float kPassClampDb      = 6.0f;
    static constexpr float kLevelMaxDb       = 24.0f;    // the Level slot's range (EedLevelProcessor)
    static constexpr float kQuietUnderDb     = 3.0f;     // window sanity: >= 3 dB under the build-time input = a quiet section
    static constexpr float kOverTargetDb     = 1.0f;     // tracking: > 1 dB over the target proposes a back-off
    static constexpr float kCloseEnoughDb    = 0.5f;     // a proposal under this is "on target"
    static constexpr int   kMaxRounds        = 4;
    static constexpr float kGrOfferDb        = 6.0f;
    static constexpr int   kWaitWallMs       = 60000;
    static constexpr int   kTickMs           = 250;

    explicit LoudnessLoop (ChainHost& host) : host_ (host) {}
    ~LoudnessLoop() override { stopTimer(); }

    std::function<void (const Bubble&)> onBubble;                          // message thread
    std::function<void (const juce::String&)> logLine;                      // "EJLoudness: ..." (item 5); the editor wires EchoJay_NSLog
    std::function<bool()>               isPlaying  { [] { return true; } };
    std::function<bool()>               knobGestureOpen { [] { return echojay::knobGestureOpen(); } };
    std::function<juce::int64()>        nowMs      { [] { return juce::Time::currentTimeMillis(); } };

    // A limiter settings text naming a LUFS target (an older server's chain). Returns the target or NaN.
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
    // The target the chain carries: the Level slot's params first, then the limiter's params (18d), then the text.
    struct ChainTarget { float lufs = std::numeric_limits<float>::quiet_NaN(); int levelSlot = -1; int limiterSlot = -1; juce::String source, option; float ceiling = -0.1f; };
    ChainTarget findTarget() const
    {
        ChainTarget t;
        const int n = host_.getNumSlots();
        auto readParams = [&] (int i, const char* key) -> juce::var
        {
            const auto st = host_.getSlotStructured (i);
            const auto params = st.getDynamicObject() != nullptr ? st.getProperty ("params", juce::var()) : juce::var();
            if (auto* po = params.getDynamicObject()) return po->getProperty (key);
            return {};
        };
        for (int i = n - 1; i >= 0; --i) if (isLimiterName (host_.getSlotInfo (i).name)) { t.limiterSlot = i; break; }
        if (t.limiterSlot < 0 && n > 0) t.limiterSlot = n - 1;   // any brand last: the last slot holds the ceiling
        for (int i = n - 1; i >= 0; --i) if (host_.getSlotInfo (i).name == "EchoJay Level") { t.levelSlot = i; break; }
        if (t.levelSlot >= 0)
        {
            if (auto* lv = dynamic_cast<EedLevelProcessor*> (host_.getSlotProcessor (t.levelSlot)))
            {
                const auto tv = readParams (t.levelSlot, "target_lufs");
                if (tv.isDouble() || tv.isInt() || tv.isInt64()) { t.lufs = (float) (double) tv; t.source = "level_params"; }
                else if (std::isfinite ((float) lv->targetLufs()) && lv->targetLufs() < -0.5) { t.lufs = (float) lv->targetLufs(); t.source = "level_device"; }
                t.option = EedLevelProcessor::optionName (lv->loudnessOption());
            }
        }
        if (! std::isfinite (t.lufs))
            for (int i = n - 1; i >= 0; --i)
            {
                const auto info = host_.getSlotInfo (i);
                if (info.name != "EchoJay Limiter") continue;
                const auto tv = readParams (i, "target_lufs");
                if (tv.isDouble() || tv.isInt() || tv.isInt64()) { t.lufs = (float) (double) tv; t.source = "limiter_params"; }
                const auto cv = readParams (i, "ceiling_db"); if (cv.isDouble() || cv.isInt()) t.ceiling = (float) (double) cv;
                const auto ov = readParams (i, "loudness_option"); if (ov.isString()) t.option = ov.toString();
                if (! std::isfinite (t.lufs)) { const float tt = targetFromSettingsText (info.settings); if (std::isfinite (tt)) { t.lufs = tt; t.source = "text"; } }
                break;
            }
        return t;
    }
    static bool isLimiterName (const juce::String& name)
    {
        const auto l = name.toLowerCase();
        return l.contains ("limit") || l.contains ("maxim") || l.contains ("clip") || l.contains ("ceiling");
    }
    // Arms when the chain carries a target AND a Level slot. Returns false (and says why on the log) otherwise.
    bool armFromChain (int /*passes*/ = 2)
    {
        const auto t = findTarget();
        if (! std::isfinite (t.lufs)) { log ("not armed: no target in the chain"); return false; }
        if (t.levelSlot < 0) { log ("not armed: no EchoJay Level slot (target " + fmt (t.lufs) + " from " + t.source + ")"); return false; }
        armSource_ = t.source; loudnessOption_ = t.option; ceilingDb_ = t.ceiling;
        arm (t.lufs, t.levelSlot, t.limiterSlot);
        return true;
    }
    juce::String armSource() const noexcept { return armSource_; }
    juce::String loudnessOption() const noexcept { return loudnessOption_; }
    float ceilingDb() const noexcept { return ceilingDb_; }

    void arm (float targetLufs, int levelSlot, int limiterSlot)
    {
        auto* lv = level (levelSlot);
        if (lv == nullptr) { log ("arm refused: slot " + juce::String (levelSlot) + " is not EchoJay Level"); return; }
        target_ = targetLufs; slot_ = levelSlot; limiterSlot_ = limiterSlot; round_ = 0; pendingTrim_ = 0.0f;
        if (! haveUndo_) { preLoopGainDb_ = (float) lv->gainDb(); haveUndo_ = true; }
        const auto in = host_.getChainInLevels();
        buildInputLufs_ = in.known ? in.levelDb : std::numeric_limits<float>::quiet_NaN();
        log ("armed: target " + fmt (target_) + " LUFS (" + armSource_ + (loudnessOption_.isNotEmpty() ? ", " + loudnessOption_ : juce::String()) + "), Level slot " + juce::String (slot_)
             + " gain " + fmtSigned ((float) lv->gainDb()) + " dB, limiter slot " + juce::String (limiterSlot_) + " (" + limiterName() + "), build-time input " + fmt (buildInputLufs_) + " LUFS");
        startWindow();
        emit ("Chain built. Play the loudest part of the song - the chorus or the drop - and I'll measure it, then ask before I set the level.", 0.0f, false, false);
        if (! juce::MessageManager::getInstanceWithoutCreating() || ! isTimerRunning()) startTimer (kTickMs);
    }

    // ---- the verbs (all deterministic; PluginEditor::handleLoudnessVerb routes the words) ----
    bool go()                       // "go" / "apply": apply the proposed trim, then measure again and propose again
    {
        if (state_ != State::proposed || level (slot_) == nullptr) return false;
        if (pendingKind_ == PendingKind::backOff || pendingKind_ == PendingKind::push)
        {
            applyTrim (pendingTrim_, pendingKind_ == PendingKind::backOff ? "back-off applied" : "push applied");
        }
        else applyTrim (pendingTrim_, "applied on go");
        ++round_;
        pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        startWindow();
        emit ("Applied. Play the loudest part again - I'll check it.", 0.0f, true, false);
        if (! isTimerRunning()) startTimer (kTickMs);
        return true;
    }
    bool pushIt()                   // raise the Level by the shortfall, clamped, one pass, then measure
    {
        auto* lv = level (slot_); if (lv == nullptr || ! std::isfinite (lastMeasured())) return false;
        const float shortfall = target_ - lastMeasured();
        const float trim = juce::jlimit (-kPassClampDb, kPassClampDb, shortfall);
        applyTrim (trim, "push it");
        round_ = 0; pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        startWindow();
        emit ("Pushed " + fmtSigned (trim) + " dB on the Level slot. Play the loudest part again - I'll check it.", 0.0f, true, false);
        if (! isTimerRunning()) startTimer (kTickMs);
        return true;
    }
    void nudgeTarget (float deltaDb)   // "a bit louder" / "a bit softer": target +-1, one pass
    {
        if (level (slot_) == nullptr) return;
        target_ += deltaDb; round_ = 0; pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        log ("target nudged " + fmtSigned (deltaDb) + " dB -> " + fmt (target_) + " LUFS");
        startWindow();
        emit ("Target now " + fmt (target_) + " LUFS. Play the loudest part again - one more pass.", 0.0f, false, false);
        if (! isTimerRunning()) startTimer (kTickMs);
    }
    void recheck()                 // "check the level again"
    {
        if (level (slot_) == nullptr) return;
        round_ = 0; pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        startWindow();
        emit ("Checking the level again - play the loudest part.", 0.0f, false, false);
        if (! isTimerRunning()) startTimer (kTickMs);
    }
    bool undo()
    {
        auto* lv = level (slot_);
        if (lv == nullptr || ! haveUndo_) return false;
        writeGainDb (preLoopGainDb_);
        state_ = State::hold; stopTimer();
        log ("undo: Level gain restored to " + fmtSigned (preLoopGainDb_) + " dB");
        emit ("Restored the Level slot to " + fmtSigned (preLoopGainDb_) + " dB (before the loop).", -1.0f, false, true);
        return true;
    }
    void leaveIt()
    {
        state_ = State::hold; stopTimer(); pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        log ("leave it: holding at " + fmt (lastMeasured()) + " LUFS, Level " + fmtSigned (currentGainDb()) + " dB");
        emit ("Leaving it at " + fmt (lastMeasured()) + " LUFS, Level " + fmtSigned (currentGainDb()) + " dB.", -1.0f, false, true);
    }

    State state() const noexcept { return state_; }
    bool  isArmed() const noexcept { return state_ == State::waitAudio || state_ == State::measuring; }
    bool  everArmed() const noexcept { return slot_ >= 0; }
    bool  hasProposal() const noexcept { return state_ == State::proposed; }
    float pendingTrimDb() const noexcept { return pendingTrim_; }
    int   round() const noexcept { return round_; }
    float target() const noexcept { return target_; }
    float lastMeasured() const noexcept { return lastMeasured_; }
    float lastInputWindow() const noexcept { return lastInputWindow_; }
    float buildInputLufs() const noexcept { return buildInputLufs_; }
    float currentGainDb() const { auto* lv = level (slot_); return lv != nullptr ? (float) lv->gainDb() : 0.0f; }
    float countedSeconds() const { return host_.getChainOutLevels().heardAboveSeconds; }
    float grAvg() const noexcept { return grN_ > 0 ? grSum_ / (float) grN_ : 0.0f; }
    float grMax() const noexcept { return juce::jmax (0.0f, grMax_); }
    juce::String lastBubble() const { return lastBubble_; }
    int   levelSlot() const noexcept { return slot_; }
    int   limiterSlot() const noexcept { return limiterSlot_; }

    // The tick. The plugin calls it from the timer; a harness calls it directly between audio blocks.
    void tickNow()
    {
        if (state_ != State::waitAudio && state_ != State::measuring && state_ != State::tracking) return;
        auto* lv = level (slot_);
        if (lv == nullptr) { state_ = State::hold; stopTimer(); log ("stopped: the Level slot is no longer in the chain"); emit ("The Level slot is no longer in the chain - level loop stopped.", -1.0f, false, true); return; }
        const auto out = host_.getChainOutLevels();
        const float counted = out.heardAboveSeconds;
        if (counted > lastCounted_ + 0.05f)
        {
            if (auto* lim = echoJayLimiter())
            { const float gr = -lim->gainReductionDb(); grMin_ = juce::jmin (grMin_, gr); grMax_ = juce::jmax (grMax_, gr); grSum_ += gr; ++grN_; }
            if (state_ == State::waitAudio) state_ = State::measuring;
        }
        lastCounted_ = counted;
        if (state_ == State::tracking)
        {   // after applying: a later, louder section proposes a back-off - ask, never apply
            if (counted < kNeedSeconds || ! std::isfinite (out.maxShortTermDb)) return;
            const float m = out.maxShortTermDb;
            if (m > target_ + kOverTargetDb)
            {
                lastMeasured_ = m; pendingTrim_ = target_ - m; pendingKind_ = PendingKind::backOff; state_ = State::proposed; stopTimer();
                log ("tracking: max short-term " + fmt (m) + " LUFS is " + fmt (m - target_) + " dB over the target -> propose back-off " + fmtSigned (pendingTrim_) + " dB");
                emit ("That section is running " + fmt (m) + " LUFS, " + fmt (m - target_) + " dB over the target. Back off " + fmt (-pendingTrim_) + " dB? say go, or leave it.", -1.0f, false, false);
            }
            return;
        }
        const float progress = juce::jlimit (0.0f, 1.0f, counted / kNeedSeconds);
        if (counted < kNeedSeconds)
        {
            if (counted <= 0.0f && ! waitingSaid_ && nowMs() - passStartMs_ >= kWaitWallMs)
            { waitingSaid_ = true; emit ("Still waiting for audio - play the loudest part of the song and I'll measure it.", 0.0f, true, false); }
            else if (counted > 0.0f) emit (round_ == 0 && pendingKind_ == PendingKind::none ? "Listening..." : "Checking...", progress, true, false);
            return;
        }
        if (knobGestureOpen()) return;
        const float measured = out.maxShortTermDb;
        if (! std::isfinite (measured)) return;
        const auto in = host_.getChainInLevels();
        lastInputWindow_ = in.maxShortTermDb;
        lastMeasured_ = measured;
        // ---- window sanity: a quiet section is not the chorus ----
        if (std::isfinite (buildInputLufs_) && std::isfinite (lastInputWindow_) && lastInputWindow_ <= buildInputLufs_ - kQuietUnderDb)
        {
            log ("window rejected: input max short-term " + fmt (lastInputWindow_) + " LUFS is " + fmt (buildInputLufs_ - lastInputWindow_) + " dB under the build-time integrated input " + fmt (buildInputLufs_) + " - nothing applied");
            startWindow();
            emit ("That sounded like a quiet section (" + fmt (lastInputWindow_) + " LUFS in, the mix measured " + fmt (buildInputLufs_) + " at build) - play the chorus and I'll try again.", 0.0f, true, false);
            return;
        }
        const float needed = target_ - measured;
        const float cur = (float) lv->gainDb();
        float trim = juce::jlimit (-kPassClampDb, kPassClampDb, needed);
        bool atCeiling = false;
        if (cur + trim > kLevelMaxDb) { trim = kLevelMaxDb - cur; atCeiling = true; }
        if (cur + trim < -kLevelMaxDb) { trim = -kLevelMaxDb - cur; atCeiling = true; }
        log ("measured: max short-term " + fmt (measured) + " LUFS (input window " + fmt (lastInputWindow_) + "), target " + fmt (target_) + ", needed " + fmtSigned (needed) + " dB, Level " + fmtSigned (cur) + " dB, trim " + fmtSigned (trim) + (atCeiling ? " (Level ceiling)" : "") + ", limiter GR avg " + fmt (grAvg()) + " max " + fmt (grMax()) + " dB, round " + juce::String (round_));
        const juce::String grText = grText_();
        if (std::abs (needed) <= kCloseEnoughDb)
        {
            state_ = State::tracking; startTracking();
            emit ("Hitting " + fmt (measured) + " LUFS (loudest 3 s), target " + fmt (target_) + " - on target. Peaks " + fmt (out.truePeakDb) + " dBTP, " + grText + " I'll keep watching for a louder section.", -1.0f, true, true);
            return;
        }
        if (round_ >= kMaxRounds)
        {
            state_ = State::hold; stopTimer();
            emit ("Stuck at " + fmt (measured) + " LUFS after " + juce::String (round_) + " rounds, target " + fmt (target_) + ": " + grText + " Say push it or leave it.", -1.0f, true, true);
            return;
        }
        if (atCeiling && std::abs (trim) < 0.05f)
        {
            state_ = State::hold; stopTimer();
            emit ("Hitting " + fmt (measured) + " LUFS, target " + fmt (target_) + " - the Level slot is at its " + fmtSigned (cur) + " dB limit; " + grText + " Say push it (the compressor and saturator can do more) or leave it.", -1.0f, true, true);
            return;
        }
        pendingTrim_ = trim; pendingKind_ = PendingKind::propose; state_ = State::proposed; stopTimer();
        emit ("Measured " + fmt (measured) + " LUFS (loudest 3 s). Push " + fmtSigned (trim) + " dB to reach " + fmt (target_) + "? say go" + (atCeiling ? " (that is the Level slot's limit)" : "") + ". " + grText, -1.0f, true, false);
    }

private:
    enum class PendingKind { none, propose, push, backOff };
    void timerCallback() override { tickNow(); }
    EedLevelProcessor* level (int slot) const
    {
        if (slot < 0 || slot >= host_.getNumSlots()) return nullptr;
        if (host_.getSlotInfo (slot).name != "EchoJay Level") return nullptr;
        return dynamic_cast<EedLevelProcessor*> (host_.getSlotProcessor (slot));
    }
    EedLimiterProcessor* echoJayLimiter() const
    {
        if (limiterSlot_ < 0 || limiterSlot_ >= host_.getNumSlots()) return nullptr;
        return dynamic_cast<EedLimiterProcessor*> (host_.getSlotProcessor (limiterSlot_));
    }
    juce::String limiterName() const { return limiterSlot_ >= 0 && limiterSlot_ < host_.getNumSlots() ? host_.getSlotInfo (limiterSlot_).name : juce::String ("none"); }
    juce::String grText_() const
    {
        if (echoJayLimiter() == nullptr) return "limiter GR unknown (" + limiterName() + " is not an EchoJay device).";
        return "limiter working " + fmt (grAvg()) + " dB average, up to " + fmt (grMax()) + " dB on the hits.";
    }
    void startWindow()
    {
        host_.setChainOutCountFloor (kCountFloorLufs);
        host_.resetChainOutLevels();
        host_.resetChainOutShortTermMax(); host_.resetChainInShortTermMax();
        if (auto* lim = echoJayLimiter()) lim->resetOutputPeak();
        lastCounted_ = 0.0f; waitingSaid_ = false; passStartMs_ = nowMs();
        grMin_ = std::numeric_limits<float>::max(); grMax_ = 0.0f; grSum_ = 0.0f; grN_ = 0;
        state_ = State::waitAudio;
    }
    void startTracking()
    {
        host_.resetChainOutLevels(); host_.resetChainOutShortTermMax(); lastCounted_ = 0.0f;
        if (! isTimerRunning() && juce::MessageManager::getInstanceWithoutCreating() != nullptr) startTimer (kTickMs);
    }
    void applyTrim (float trim, const char* why)
    {
        auto* lv = level (slot_); if (lv == nullptr) return;
        const float newDb = juce::jlimit (-kLevelMaxDb, kLevelMaxDb, (float) lv->gainDb() + trim);
        log (juce::String (why) + ": Level " + fmtSigned ((float) lv->gainDb()) + " -> " + fmtSigned (newDb) + " dB (trim " + fmtSigned (trim) + ")");
        writeGainDb (newDb);
    }
    void writeGainDb (float db)
    {   // through the host so the card and the dial info follow
        auto* p = new juce::DynamicObject(); p->setProperty ("gain_db", (double) db);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (p));
        host_.setSlotStructuredSettings (slot_, juce::var (w));
        if (auto* lv = level (slot_)) if (std::abs (lv->gainDb() - db) > 0.01) lv->setParamValue ("gain_db", db);
        host_.setSlotSettings (slot_, "Level " + fmtSigned (db) + " dB (set by the level loop toward the " + fmt (target_) + " LUFS target)");
    }
    void emit (const juce::String& text, float progress, bool replace, bool final)
    {
        lastBubble_ = text;
        log ("bubble: " + text);
        if (onBubble) { Bubble b; b.text = text; b.progress = progress; b.replace = replace; b.final = final; onBubble (b); }
    }
    void log (const juce::String& s) const { if (logLine) logLine ("EJLoudness: " + s); }
    static juce::String fmt (float v) { return std::isfinite (v) ? juce::String (v, 1).replace ("-0.0", "0.0") : juce::String ("n/a"); }
    static juce::String fmtSigned (float v) { return (v >= 0.0f ? "+" : "") + fmt (v); }

    ChainHost& host_;
    State state_ = State::idle;
    int   slot_ = -1, limiterSlot_ = -1, round_ = 0;
    float target_ = -9.0f;
    juce::String armSource_, loudnessOption_;
    float ceilingDb_ = -0.1f;
    float lastMeasured_ = std::numeric_limits<float>::quiet_NaN(), lastInputWindow_ = std::numeric_limits<float>::quiet_NaN(), buildInputLufs_ = std::numeric_limits<float>::quiet_NaN();
    float pendingTrim_ = 0.0f; PendingKind pendingKind_ = PendingKind::none;
    float preLoopGainDb_ = 0.0f; bool haveUndo_ = false;
    float lastCounted_ = 0.0f; bool waitingSaid_ = false; juce::int64 passStartMs_ = 0;
    float grMin_ = std::numeric_limits<float>::max(), grMax_ = 0.0f, grSum_ = 0.0f; int grN_ = 0;
    juce::String lastBubble_;
};
