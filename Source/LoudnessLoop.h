#pragma once
#define EJ_LOUDNESSLOOP_ARMSOURCE 1   // 18d: armSource() / loudnessOption() / ceilingDb() exist on this build
#define EJ_LOUDNESSLOOP_V2 1          // 18e: the Level slot, max short-term, ask-before-apply, the verbs, EJLoudness lines
#define EJ_LOUDNESSLOOP_PILLS 1       // 18f: bubbles carry their verbs as pills; the quiet-window and back-off asks
#define EJ_LOUDNESSLOOP_MANNERS 1     // 18g: explicit Listen / Check, +-1 dB with step scaling and 3 proposals, Done, GR estimate, ceiling-readback safety net
#define EJ_LOUDNESSLOOP_MANNERS21 1  // 21 Sep: Go is a level verb ("Applied +X dB (Level now +Y). How's it sounding?"); NO automatic check after it - nothing measures until Check; a proposal after any apply carries [Undo]
#define EJ_LOUDNESSLOOP_VERBS18H 1    // 18h: a level verb applies and asks "How's it sounding?" (no auto-check); Push it only when short; peak GR estimate
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
#include <vector>
#include <algorithm>
#include "ChainHost.h"
#include "EedLimiterProcessor.h"
#include "EedLevelProcessor.h"
#include "EedDeviceRegistry.h"
#include "EJKnobGesture.h"
#include <cmath>
#include <functional>
#include <limits>

class LoudnessLoop : private juce::Timer
{
public:
    // 18g: armed = waiting for Listen (no window runs on the first audio). 21 Sep: the after-Go waiting state is gone - after Go the
    // loop HOLDS like after any other level verb; the user presses Check (the "one automatic check if audio continues" branch was removed, not gated)
    enum class State { idle, waitAudio, measuring, proposed, hold, tracking, quietAsked, armed };
    // 18f: kind decides the editor's treatment - progress bubbles ("Listening..." / "Checking...") replace the previous
    // PROGRESS bubble only; every other bubble is history and stays. pills = the verbs the bubble offers, in order; a pill
    // tap runs the same handler as the typed words (PluginEditor::handleLoudnessVerb).
    struct Bubble
    {
        enum class Kind { progress, arm, proposal, result, stuck, quiet, backoff, info };
        juce::String text; float progress = -1.0f; bool replace = false; bool final = false; Kind kind = Kind::info; juce::StringArray pills;
    };
    // 18h (item 3): [Push it] is offered only when the loop is SHORT of the target (shortPills), never on an on-target result
    static juce::StringArray resultPills()   { return { "Undo", "A bit louder", "A bit softer", "Done" }; }
    static juce::StringArray shortPills()    { return { "Push it", "Undo", "A bit louder", "A bit softer", "Done" }; }
    // 18h (item 4): after a level verb (A bit louder / softer / Push it / Undo): "Applied +-X dB (Level now +Y). How's it sounding?"
    static juce::StringArray afterVerbPills(){ return { "Check", "A bit louder", "A bit softer", "Undo", "Done" }; }
    static juce::StringArray armPills()      { return { "Listen" }; }
    static juce::StringArray checkPills()    { return { "Check" }; }
    static juce::StringArray proposalPills() { return { "Go", "Leave it" }; }
    static juce::StringArray proposalAfterApplyPills() { return { "Go", "Leave it", "Undo" }; }   // 21 Sep: a proposal that follows ANY prior apply
    static juce::StringArray stuckPills()    { return { "Push it", "Leave it" }; }
    static juce::StringArray quietPills()    { return { "Listen again", "This is the loudest part" }; }
    static juce::StringArray backoffPills()  { return { "Back off", "Leave it" }; }

    static constexpr float kCountFloorLufs   = -40.0f;
    static constexpr float kNeedSeconds      = 10.0f;
    static constexpr float kPassClampDb      = 6.0f;
    static constexpr float kLevelMaxDb       = 24.0f;    // the Level slot's range (EedLevelProcessor)
    static constexpr float kQuietUnderDb     = 3.0f;     // window sanity: >= 3 dB under the build-time input = a quiet section
    static constexpr float kOverTargetDb     = 1.0f;     // tracking: > 1 dB over the target proposes a back-off
    static constexpr float kCloseEnoughDb    = 1.0f;     // 18g: on target = within +-1.0 dB (was 0.5)
    // 22 Sep 2026 (item 5): every proposal is bounded by the limiter's GR on the hits, per loudness option
    // 21m item 3 (22 Sep 2026): the cap is on the TYPICAL reduction on the hits (mean over the top 20 % of 100 ms blocks in the
    // loudest 3 s), not the single worst peak. Calibration (22 Sep, Sean's Mix Bus, Pushed): the worst-peak cap of 6 stopped at
    // -10.8 with the worst hit at 4.8 dB (+1.1 offered); Sean pushed +2..+3 by hand and judged it right - a worst peak of ~8-9 dB
    // at his setting. The typical figure was not logged by 21l, so these are the ruled starting points: Pushed 8 sits above his
    // setting, Commercial 6 at or just below it; the first 21m log carries both figures for the re-calibration.
    static float grCapDb (const juce::String& option) noexcept
    {
        if (option == "pushed")  return 8.0f;
        if (option == "dynamic") return 3.0f;
        if (option == "keep")    return 1.0f;
        return 6.0f;   // commercial (and the default)
    }
    static constexpr float kOpeningHeadroomDb = 3.0f;
    static constexpr float kOpeningFloorDb    = -6.0f;   // ruling 5b (22 Sep 2026): the opening gain never goes below -6.0 dB   // 22 Sep 2026 (item 5): peaks into the limiter never open more than 3 dB over the ceiling
    static constexpr int   kMaxProposals     = 3;        // 18g: at most 3 proposals, then the result bubble (was 4 rounds)
    static constexpr float kRatioMin         = 0.5f, kRatioMax = 2.0f;   // 18g: achieved/commanded clamp for the step scaling
    static constexpr float kGrOfferDb        = 6.0f;
    static constexpr int   kWaitWallMs       = 60000;
    static constexpr int   kTickMs           = 250;

    explicit LoudnessLoop (ChainHost& host) : host_ (host) {}
    ~LoudnessLoop() override { stopTimer(); }

    std::function<void (const Bubble&)> onBubble;                          // message thread
    std::function<void (const juce::String&)> logLine;                      // "EJLoudness: ..." (item 5); the editor wires EchoJay_NSLog
    std::function<bool()>               isPlaying  { [] { return true; } };
    std::function<void (float beforeDb, float afterDb)> onGainWritten;      // 21n item 3: every loop write of the Level gain (an undo entry)
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
    // COMPATIBILITY (19e on the live 88x7asebn server, which puts target_lufs on the EchoJay Limiter and no Level slot):
    // a chain that carries a target but no "EchoJay Level" slot gets one inserted immediately before its last slot, the
    // target and option copied onto it, and the loop arms from THAT slot. The limiter's own input_db is left as the
    // server set it; the loop only ever moves the Level slot. Returns the index of the Level slot or -1.
    int ensureLevelSlot()
    {
        auto t = findTarget();
        if (! std::isfinite (t.lufs)) return -1;
        if (t.levelSlot >= 0) return t.levelSlot;
        const int n = host_.getNumSlots();
        if (n <= 0) return -1;
        const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        if (dev == nullptr) { log ("cannot insert EchoJay Level: not registered"); return -1; }
        const int at = n - 1;
        const auto err = host_.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*dev), at);
        if (err.isNotEmpty()) { log ("could not insert EchoJay Level: " + err); return -1; }
        auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 0.0); pp->setProperty ("target_lufs", (double) t.lufs);
        const juce::StringArray opts { "commercial", "pushed", "dynamic", "keep" };
        pp->setProperty ("loudness_option", juce::jmax (0, opts.indexOf (t.option)));
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
        host_.setSlotStructuredSettings (at, juce::var (w));
        host_.setSlotSettings (at, "Level 0.0 dB - the level loop drives this slot toward the " + fmt (t.lufs) + " LUFS target");
        log ("inserted EchoJay Level at slot " + juce::String (at) + " (target " + fmt (t.lufs) + " from " + t.source + "; the chain had none)");
        return at;
    }
    // Arms when the chain carries a target: from the Level slot, inserting one first when the target sits on the limiter
    // (an 18d-shaped chain). Returns false (and says why on the log) otherwise.
    bool armFromChain (int /*passes*/ = 2)
    {
        ensureLevelSlot();
        auto t = findTarget();   // 18g: the safety net may move the limiter slot
        if (! std::isfinite (t.lufs)) { log ("not armed: no target in the chain"); return false; }
        if (t.levelSlot < 0) { log ("not armed: no EchoJay Level slot could be placed (target " + fmt (t.lufs) + " from " + t.source + ")"); return false; }
        armSource_ = t.source; loudnessOption_ = t.option; ceilingDb_ = t.ceiling;
        substituteLimiterIfNoCeilingReadback (t);   // 18g (item 5): the ceiling must be CONFIRMED before the loop drives into it
        arm (t.lufs, t.levelSlot, t.limiterSlot);
        return true;
    }
    // 18g (item 5, safety net): the last limiter is a third-party slot whose ceiling control has NO dial readback (no map, or the
    // ceiling was not among the applied controls) -> it is replaced by EchoJay Limiter holding the chain's ceiling, said in one line.
    // A third-party limiter whose ceiling READ BACK stays (the loop estimates its GR).
    bool substituteLimiterIfNoCeilingReadback (ChainTarget& t)
    {
        if (t.limiterSlot < 0 || t.limiterSlot >= host_.getNumSlots()) return false;
        if (dynamic_cast<EedLimiterProcessor*> (host_.getSlotProcessor (t.limiterSlot)) != nullptr) return false;
        const auto infos = host_.getDialInfos();
        bool ceilingReadBack = false;
        if (t.limiterSlot < (int) infos.size())
            for (const auto& a : infos[(size_t) t.limiterSlot].applied) if (a.containsIgnoreCase ("ceil")) ceilingReadBack = true;
        if (ceilingReadBack) return false;
        const auto oldName = host_.getSlotInfo (t.limiterSlot).name;
        // the ceiling the chain asked for: the slot's ceiling-like control value, else the chain default -0.1
        float ceiling = std::isfinite (t.ceiling) ? t.ceiling : -0.1f;
        { const auto st = host_.getSlotStructured (t.limiterSlot);
          if (auto* co = st.getProperty ("controls", juce::var()).getDynamicObject())
              for (const auto& kv : co->getProperties()) if (kv.name.toString().containsIgnoreCase ("ceil") && (kv.value.isDouble() || kv.value.isInt())) ceiling = (float) (double) kv.value; }
        const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (dev == nullptr) { log ("ceiling readback absent on " + oldName + " but EchoJay Limiter is not registered - left as is"); return false; }
        const int at = t.limiterSlot;
        host_.removeSlot (at);
        const auto err = host_.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*dev), at);
        if (err.isNotEmpty()) { log ("could not substitute EchoJay Limiter for " + oldName + ": " + err); return false; }
        auto* pp = new juce::DynamicObject(); pp->setProperty ("ceiling_db", (double) ceiling); pp->setProperty ("true_peak", 1);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
        host_.setSlotStructuredSettings (at, juce::var (w));
        host_.setSlotSettings (at, "ceiling " + fmt (ceiling) + " dBTP, true peak on - holds the ceiling (substituted for " + oldName + ": its ceiling could not be confirmed)");
        t.limiterSlot = at; ceilingDb_ = ceiling;   // the Level slot sits before it and is untouched by the remove/insert at 'at'
        log ("substituted EchoJay Limiter for " + oldName + " at slot " + juce::String (at) + ": its ceiling had no dial readback; ceiling " + fmt (ceiling) + " dBTP");
        emit (oldName + "'s ceiling could not be confirmed, so EchoJay Limiter holds the ceiling instead (" + fmt (ceiling) + " dBTP).", -1.0f, false, false, Bubble::Kind::info);
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
        levelPtr_ = lv; limiterPtr_ = (limiterSlot >= 0 && limiterSlot < host_.getNumSlots()) ? host_.getSlotProcessor (limiterSlot) : nullptr;   // 21m item 1: identity, not index
        if (! haveUndo_) { preLoopGainDb_ = (float) lv->gainDb(); haveUndo_ = true; }
        const auto in = host_.getChainInLevels();
        buildInputLufs_ = in.known ? in.levelDb : std::numeric_limits<float>::quiet_NaN();
        // 22 Sep 2026 (item 5): opening gain at build = min (estimate, ceiling + 3 dB - build-time true peak of the loudest
        // section), so peaks into the limiter never open more than 3 dB over the ceiling.
        if (in.known && in.truePeakDb > -150.0f && std::isfinite (ceilingDb_))
        {
            const float maxOpen = juce::jmax (kOpeningFloorDb, ceilingDb_ + kOpeningHeadroomDb - in.truePeakDb);   // ruling 5b: floored at -6.0
            if ((float) lv->gainDb() > maxOpen)
            {
                const float was = (float) lv->gainDb();
                writeGainDb (juce::jlimit (-kLevelMaxDb, kLevelMaxDb, maxOpen));
                log ("opening gain capped: " + fmtSigned (was) + " -> " + fmtSigned ((float) lv->gainDb()) + " dB (ceiling " + fmt (ceilingDb_) + " + 3 - build-time true peak " + fmt (in.truePeakDb) + " dBTP" + (maxOpen <= kOpeningFloorDb + 0.001f ? ", floored at -6.0" : "") + ")");
                if ((float) lv->gainDb() < 0.0f)   // ruling 5b: the chain card says why the Level opened below zero
                    host_.setSlotSettings (slot_, "Level " + fmtSigned ((float) lv->gainDb()) + " dB: the mix already peaks above the ceiling");
            }
        }
        log ("armed: target " + fmt (target_) + " LUFS (" + armSource_ + (loudnessOption_.isNotEmpty() ? ", " + loudnessOption_ : juce::String()) + "), Level slot " + juce::String (slot_)
             + " gain " + fmtSigned ((float) lv->gainDb()) + " dB, limiter slot " + juce::String (limiterSlot_) + " (" + limiterName() + "), build-time input " + fmt (buildInputLufs_) + " LUFS");
        // 18g (item 1): NO window runs on the first audio. The user cues the loudest section and taps Listen (or types it).
        state_ = State::armed; proposals_ = 0; lastCommanded_ = 0.0f; prevMeasured_ = std::numeric_limits<float>::quiet_NaN();
        emit ("Cue the loudest section, press play, then tap Listen.", -1.0f, false, false, Bubble::Kind::arm, armPills());
        if (! juce::MessageManager::getInstanceWithoutCreating() || ! isTimerRunning()) startTimer (kTickMs);   // the tick feeds the Level card's GR while armed
    }
    // 18g (item 1): Listen starts the measuring window - from armed, from a Check prompt, from the quiet-window question, or
    // after Done / Leave it (a fresh listen). Check is the same window after Go when the audio had stopped.
    bool listen()
    {
        if (levelNow() == nullptr) return false;
        if (state_ == State::waitAudio || state_ == State::measuring || state_ == State::proposed) return false;
        quietMeasured_ = std::numeric_limits<float>::quiet_NaN(); continueAfterGo_ = false; proposals_ = 0; lastCommanded_ = 0.0f;   // a fresh listen is a fresh sequence
        startWindow();
        emit ("Listening...", 0.0f, true, false, Bubble::Kind::progress);
        if (! isTimerRunning()) startTimer (kTickMs);
        return true;
    }
    bool check()
    {   // 18h (item 4): Check measures ONCE and reports with the result pills (no proposal) - from the after-verb hold, from the
        // "Tap Check" prompt, or from the watch; not while a window already runs or a proposal is open
        if (levelNow() == nullptr) return false;
        if (state_ == State::waitAudio || state_ == State::measuring || state_ == State::proposed || state_ == State::armed) return false;
        reportOnly_ = ! continueAfterGo_; continueAfterGo_ = false;   // 21 Sep: after Go the Check continues the sequence (a proposal may follow); after any other verb it reports
        startWindow();
        emit ("Checking...", 0.0f, true, false, Bubble::Kind::progress);
        if (! isTimerRunning()) startTimer (kTickMs);
        return true;
    }
    // 18g (item 3): Done ends the watch phase - nothing is measured or proposed after it.
    bool done()
    {
        if (slot_ < 0) return false;
        state_ = State::hold; stopTimer(); pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none; continueAfterGo_ = false;
        log ("done: Level " + fmtSigned (currentGainDb()) + " dB, last measured " + fmt (lastMeasured()) + " LUFS - the loop is finished");
        emit ("Done - Level " + fmtSigned (currentGainDb()) + " dB, last measured " + fmt (lastMeasured()) + " LUFS. Say listen to measure again.", -1.0f, false, true, Bubble::Kind::result);
        return true;
    }

    // ---- the verbs (all deterministic; PluginEditor::handleLoudnessVerb routes the words) ----
    bool go()                       // "go" / "apply": apply the proposed trim, then measure again and propose again
    {
        if (state_ != State::proposed || levelNow() == nullptr) return false;
        if (pendingKind_ == PendingKind::backOff || pendingKind_ == PendingKind::push)
        {
            applyTrim (pendingTrim_, pendingKind_ == PendingKind::backOff ? "back-off applied" : "push applied");
        }
        else applyTrim (pendingTrim_, "applied on go");
        applied_ = true;   // 21 Sep: a USER apply - every proposal from here carries [Undo] (the arm-time exact apply is not one)
        ++round_;
        pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        // 21 Sep 2026 (Sean's ruling): Go is a level verb like every other - the Level moved, ONE bubble asks how it sounds and NOTHING
        // measures until the user presses Check. The next Check continues the sequence (round_, proposals_, the step scaling), so
        // it may propose again; that proposal carries [Undo]. (The 18g "one automatic check if audio continues" branch is removed.)
        continueAfterGo_ = true; reportOnly_ = false;
        state_ = State::hold; stopTimer();
        emit ("Applied " + fmtSigned (appliedDelta_) + " dB (Level now " + fmtSigned (currentGainDb()) + "). How's it sounding?", -1.0f, false, false, Bubble::Kind::info, afterVerbPills());
        return true;
    }
    // 18f: the quiet-window answers
    bool listenAgain()              // "listen again" (the quiet-window pill): the same window as Listen
    {
        if (state_ != State::quietAsked) return false;
        return listen();
    }
    bool loudestPart()              // "this is the loudest part": proceed with the held measurement -> the normal proposal
    {
        if (state_ != State::quietAsked || levelNow() == nullptr || ! std::isfinite (quietMeasured_)) return false;
        const float m = quietMeasured_; quietMeasured_ = std::numeric_limits<float>::quiet_NaN();
        log ("quiet window accepted as the loudest part: proceeding with " + fmt (m) + " LUFS");
        proposeFrom (m, host_.getChainOutLevels().truePeakDb);
        return true;
    }
    bool backOff()                  // "back off": apply the pending back-off (the same as go on a back-off proposal)
    {
        if (state_ != State::proposed || pendingKind_ != PendingKind::backOff) return false;
        return go();
    }
    bool pushIt()                   // raise the Level by the shortfall, clamped, one pass, then measure
    {
        auto* lv = levelNow(); if (lv == nullptr || ! std::isfinite (lastMeasured())) return false;
        const float shortfall = target_ - lastMeasured();
        const float trim = juce::jlimit (-kPassClampDb, kPassClampDb, shortfall);
        applyTrim (trim, "push it"); applied_ = true;
        afterVerb (trim);
        return true;
    }
    // 18h (item 4): every level verb ends here - the Level moved, ONE bubble asks how it sounds, nothing measures until Check
    void afterVerb (float deltaDb)
    {
        round_ = 0; proposals_ = 0; pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none; continueAfterGo_ = false; lastCommanded_ = 0.0f;
        state_ = State::hold; stopTimer();
        emit ("Applied " + fmtSigned (deltaDb) + " dB (Level now " + fmtSigned (currentGainDb()) + "). How's it sounding?", -1.0f, false, false, Bubble::Kind::info, afterVerbPills());
    }
    // 22 Sep 2026 (item 2, client half): a typed complaint the server classified loop_verb ("too squashed", "over limited", "distorted",
    // "pumping", "too loud") is the softer step TWICE - one move of -2 dB, one bubble
    bool backOffComplaint() { if (levelNow() == nullptr) return false; log ("complaint -> softer x2"); nudgeTarget (-2.0f); return true; }
    void nudgeTarget (float deltaDb)   // "a bit louder" / "a bit softer": target +-1 AND the Level moves by it now (one pass), then one check
    {
        if (levelNow() == nullptr) return;
        target_ += deltaDb;
        log ("target nudged " + fmtSigned (deltaDb) + " dB -> " + fmt (target_) + " LUFS");
        // 18g/18h: the nudge IS the move - the Level moves by +-1 now; 18h: no automatic check, the bubble asks how it sounds
        applyTrim (deltaDb, deltaDb > 0 ? "a bit louder" : "a bit softer"); applied_ = true;
        afterVerb (deltaDb);
    }
    void recheck()                 // "check the level again" = Check (18h): one window, a report with the result pills
    {
        if (levelNow() == nullptr) return;
        if (state_ == State::waitAudio || state_ == State::measuring) return;
        round_ = 0; proposals_ = 0; lastCommanded_ = 0.0f; pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none; continueAfterGo_ = false; reportOnly_ = true;
        startWindow();
        emit ("Checking the level again - play the loudest part.", -1.0f, false, false, Bubble::Kind::info);
        if (! isTimerRunning()) startTimer (kTickMs);
    }
    bool undo()
    {
        auto* lv = levelNow();
        if (lv == nullptr || ! haveUndo_) return false;
        const float delta = preLoopGainDb_ - (float) lv->gainDb();
        writeGainDb (preLoopGainDb_);
        log ("undo: Level gain restored to " + fmtSigned (preLoopGainDb_) + " dB (delta " + fmtSigned (delta) + ")");
        afterVerb (delta);   // 18h: "Applied -X dB (Level now +Y). How's it sounding?"
        return true;
    }
    void leaveIt()
    {
        const bool watching = pendingKind_ == PendingKind::backOff;   // 18g (item 3): leaving a back-off keeps the watch; Done ends it
        pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none;
        if (watching) { state_ = State::tracking; startTracking(); }
        else { state_ = State::hold; stopTimer(); }
        log ("leave it: holding at " + fmt (lastMeasured()) + " LUFS, Level " + fmtSigned (currentGainDb()) + " dB" + (watching ? ", still watching" : ""));
        emit ("Leaving it at " + fmt (lastMeasured()) + " LUFS, Level " + fmtSigned (currentGainDb()) + " dB." + (watching ? " Still watching for a louder section." : ""), -1.0f, false, true, Bubble::Kind::result, pillsFor (target_ - lastMeasured()));
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
    float currentGainDb() const { auto* lv = levelNow(); return lv != nullptr ? (float) lv->gainDb() : 0.0f; }
    float countedSeconds() const { return host_.getChainOutLevels().heardAboveSeconds; }
    float grAvg() const noexcept { return grN_ > 0 ? grSum_ / (float) grN_ : 0.0f; }
    float grMax() const noexcept { return juce::jmax (0.0f, grMax_); }
    juce::String lastBubble() const { return lastBubble_; }
    Bubble::Kind lastKind() const noexcept { return lastKind_; }
    juce::StringArray lastPills() const { return lastPills_; }
    int bubbleCount() const noexcept { return bubbleCount_; }   // non-progress bubbles emitted (a verb must produce exactly one)
    // 18f: the editor's verb line ("verb \"go\" state 3") rides the SAME EJLoudness stream as the loop's own lines, so a
    // pill and the typed word can be compared as one sequence (the guard captures logLine; NSLog was outside it).
    void note (const juce::String& s) const { log (s); }
    // 18g
    int   proposals() const noexcept { return proposals_; }
    float lastCommandedDb() const noexcept { return lastCommanded_; }
    float grEstimateDb() const noexcept { return estN_ > 0 ? estSum_ / (float) estN_ : std::numeric_limits<float>::quiet_NaN(); }   // Level OUT minus chain OUT (LUFS-S), mean over the window
    // 18h: the peak estimate - Level OUT true peak minus chain OUT true peak, both max-held over the window (the hits)
    float grPeakEstimateDb() const { auto* lv = levelNow(); if (lv == nullptr) return std::numeric_limits<float>::quiet_NaN(); const float a = lv->outputLevels().truePeakDb, b = host_.getChainOutLevels().truePeakDb; return (a > -150.0f && b > -150.0f) ? a - b : std::numeric_limits<float>::quiet_NaN(); }
    // 21m item 3: the hits, block by block. typical = mean reduction (Level OUT TP - chain OUT TP) over the top 20 % of the last
    // 30 hops ranked by Level OUT true peak (the hits); worst = the largest single-block reduction; typicalLvTp = the mean Level
    // OUT true peak of those top blocks (what the cap projects the trim onto). NaN until both tallies carry hops.
    struct HitsMeasure { float typicalDb = std::numeric_limits<float>::quiet_NaN(), worstDb = std::numeric_limits<float>::quiet_NaN(), typicalLvTpDb = std::numeric_limits<float>::quiet_NaN(); int blocks = 0; };
    HitsMeasure hitsMeasure() const
    {
        HitsMeasure m; auto* lv = levelNow(); if (lv == nullptr) return m;
        const auto a = lv->outputLevels(), b = host_.getChainOutLevels();
        const int n = juce::jmin (a.hopTruePeakCount, b.hopTruePeakCount); if (n <= 0) return m;
        std::vector<int> order; for (int k = 0; k < n; ++k) order.push_back (k);
        const int offA = a.hopTruePeakCount - n, offB = b.hopTruePeakCount - n;   // newest-aligned
        std::sort (order.begin(), order.end(), [&] (int x, int y) { return a.hopTruePeakDb[(size_t) (offA + x)] > a.hopTruePeakDb[(size_t) (offA + y)]; });
        const int top = juce::jmax (1, (int) std::lround (n * 0.2));
        // the single loudest block is the transient the worst figure reports; it is left OUT of the typical mean whenever the top
        // share has three or more blocks, so one hit cannot drag "typical" (and the cap's projection) up to itself
        const int first = top >= 3 ? 1 : 0, used = top - first;
        double sumRed = 0.0, sumTp = 0.0; float worst = -1.0e9f;
        for (int k = 0; k < n; ++k) { const float red = a.hopTruePeakDb[(size_t) (offA + k)] - b.hopTruePeakDb[(size_t) (offB + k)]; if (red > worst) worst = red; }
        for (int i = first; i < top; ++i) { const int k = order[(size_t) i]; sumRed += a.hopTruePeakDb[(size_t) (offA + k)] - b.hopTruePeakDb[(size_t) (offB + k)]; sumTp += a.hopTruePeakDb[(size_t) (offA + k)]; }
        m.typicalDb = juce::jmax (0.0f, (float) (sumRed / used)); m.worstDb = juce::jmax (0.0f, worst); m.typicalLvTpDb = (float) (sumTp / used); m.blocks = n;
        return m;
    }
    static juce::StringArray pillsFor (float needed) { return needed > kCloseEnoughDb ? shortPills() : resultPills(); }   // 18h: Push it only when SHORT
    bool  grIsEstimated() const { return echoJayLimiter() == nullptr; }
    int   levelSlot() const noexcept { return slot_; }
    int   limiterSlot() const noexcept { return limiterSlot_; }

    // The tick. The plugin calls it from the timer; a harness calls it directly between audio blocks.
    void tickNow()
    {
        auto* lv = levelNow();
        if (lv == nullptr) { if (state_ != State::idle && state_ != State::hold) { state_ = State::hold; stopTimer(); levelPtr_ = nullptr; log ("stopped: the Level slot is no longer in the chain"); emit ("The Level slot is no longer in the chain - level loop stopped.", -1.0f, false, true, Bubble::Kind::info); } return; }
        // the Level card's GR row: the EchoJay Limiter's real GR, else the estimate (18g item 4)
        if (auto* lim = echoJayLimiter()) lv->setDownstreamGrDb (-lim->gainReductionDb(), false);
        else if (estN_ > 0) lv->setDownstreamGrDb (grEstimateDb(), true);
        if (state_ != State::waitAudio && state_ != State::measuring && state_ != State::tracking) return;   // armed / proposed / hold / quietAsked: nothing runs on its own
        const auto out = host_.getChainOutLevels();
        const float counted = out.heardAboveSeconds;
        if (counted > lastCounted_ + 0.05f)
        {
            if (auto* lim = echoJayLimiter())
            { const float gr = -lim->gainReductionDb(); grMin_ = juce::jmin (grMin_, gr); grMax_ = juce::jmax (grMax_, gr); grSum_ += gr; ++grN_; }
            {   // 18g (item 4): the ESTIMATE, Level OUT minus chain OUT (short-term LUFS) over the window. Accumulated on every
                // limiter (a guard compares it with the EchoJay Limiter's real GR); it is REPORTED only for a third-party one.
                const float lvOut = lv->outputLevels().shortTermDb, chOut = out.shortTermDb;
                if (std::isfinite (lvOut) && std::isfinite (chOut)) { const float e = lvOut - chOut; estSum_ += e; ++estN_; estMax_ = juce::jmax (estMax_, e); }
            }
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
                emit ("That section is louder than the one I levelled on (" + fmt (m) + " LUFS, " + fmt (m - target_) + " dB over the target) - back off " + fmtSigned (pendingTrim_) + " dB?", -1.0f, false, false, Bubble::Kind::backoff, backoffPills());
            }
            return;
        }
        const float progress = juce::jlimit (0.0f, 1.0f, counted / kNeedSeconds);
        if (counted < kNeedSeconds)
        {
            if (counted <= 0.0f && ! waitingSaid_ && nowMs() - passStartMs_ >= kWaitWallMs)
            { waitingSaid_ = true; emit ("Still waiting for audio - play the loudest part of the song and I'll measure it.", 0.0f, true, false, Bubble::Kind::progress); }
            else if (counted > 0.0f) emit (round_ == 0 && pendingKind_ == PendingKind::none ? "Listening..." : "Checking...", progress, true, false, Bubble::Kind::progress);
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
        {   // 18f: ASK rather than restart - the measurement is held until "listen again" or "this is the loudest part"
            log ("window quiet: input max short-term " + fmt (lastInputWindow_) + " LUFS is " + fmt (buildInputLufs_ - lastInputWindow_) + " dB under the build-time integrated input " + fmt (buildInputLufs_) + " - asking, nothing applied");
            quietMeasured_ = measured; state_ = State::quietAsked; stopTimer();
            emit ("This is quieter than the section the chain was built on (" + fmt (lastInputWindow_) + " now vs " + fmt (buildInputLufs_) + " at build). Is this the loudest part of the song?", -1.0f, false, false, Bubble::Kind::quiet, quietPills());
            return;
        }
        {   // 21m ruling 2 (gain staging): at every Listen/Check window close, each slot except the Level and the last limiter is trimmed to
            // unity (-(out - in) short-term, +-12 dB, kept slots untouched) BEFORE the proposal - the loop then works on a unity chain
            auto sumTrims = [this] { float t = 0.0f; for (int i = 0; i < host_.getNumSlots(); ++i) t += host_.getSlotTrimDb (i); return t; };
            const float trimsBefore = sumTrims();
            juce::StringArray tl; const int changed = host_.measureUnityTrims (slot_, limiterSlot_, &tl);
            for (const auto& l : tl) log ("unity trim: " + l);
            trimDeltaDb_ = sumTrims() - trimsBefore;   // what the chain output moved by, after this window measured it
            if (changed > 0) log ("unity trims changed: " + juce::String (changed) + " (chain output moves " + juce::String (trimDeltaDb_, 1) + " dB)"); }
        // the loop's opening gain assumes a UNITY chain: the window measured the un-trimmed chain, so the figures it proposes from carry the trims just applied
        proposeFrom (measured + trimDeltaDb_, out.truePeakDb + trimDeltaDb_);
    }
    // 18f: the decision after a measurement (also reached from loudestPart()) - proposal / on-target / stuck / at the limit
    void proposeFrom (float measured, float truePeakDb)
    {
        auto* lv = levelNow(); if (lv == nullptr) return;
        const float needed = target_ - measured;
        const float cur = (float) lv->gainDb();
        // 18g (item 2): from the second pass the step is scaled by achieved/commanded of the previous pass (clamped 0.5-2.0),
        // so a limiter that gives back 0.6 dB per dB gets a 1/0.6 step, not four shrinking ones.
        float ratio = 1.0f;
        if (std::abs (lastCommanded_) >= 0.05f && std::isfinite (prevMeasured_))
        {
            const float achieved = measured - prevMeasured_;
            ratio = juce::jlimit (kRatioMin, kRatioMax, achieved / lastCommanded_);
            log ("step scaling: commanded " + fmtSigned (lastCommanded_) + " dB, achieved " + fmtSigned (achieved) + " dB -> ratio " + juce::String (ratio, 2) + " (clamped 0.5-2.0)");
        }
        float trim = juce::jlimit (-kPassClampDb, kPassClampDb, needed / ratio);
        // 22 Sep 2026 (item 5): the proposal is bounded by the limiter's GR on the hits for this loudness option - if the
        // target needs more, the CAPPED level is proposed and said so ("-9.4 is as loud as this goes with the limiter
        // working <=4 dB. Push to -8 anyway?") with [Push it anyway] [Leave it].
        // predicted GR on the hits after the trim = (Level OUT true peak, max-held) + trim - the limiter's ceiling: the peaks
        // above the ceiling are what the limiter takes; a trim that keeps the peaks under the ceiling costs no GR at all
        // 21m item 3: the cap projects the trim onto the TYPICAL hit level (the mean Level OUT true peak of the top 20 % of blocks),
        // not the single worst peak; the worst peak is measured and logged beside it
        bool capped = false; const float cap = grCapDb (loudnessOption_); const auto hm = hitsMeasure(); const float worstTP = lv->outputLevels().truePeakDb;
        const float lvTP = std::isfinite (hm.typicalLvTpDb) ? hm.typicalLvTpDb : worstTP;
        // the base the trim is projected onto: the MEASURED typical reduction when the hits are already being limited (a clipper's
        // intersample overshoot makes "true peak - ceiling" read high), else the typical hit's distance to the ceiling
        const float base = (std::isfinite (hm.typicalDb) && hm.typicalDb > 0.5f) ? hm.typicalDb : (lvTP - ceilingDb_);
        if (trim > 0.0f && lvTP > -150.0f && std::isfinite (ceilingDb_))
        {
            const float predictedHits = juce::jmax (0.0f, base + trim);
            if (predictedHits > cap + 0.05f) { const float capTrim = juce::jmax (0.0f, cap - base); log ("GR cap: typical hit true peak " + fmt (lvTP) + " dBTP (top 20 % of " + juce::String (hm.blocks) + " blocks; worst peak " + fmt (worstTP) + " dBTP, worst reduction " + fmt (hm.worstDb) + " dB) + trim " + fmtSigned (trim) + " - ceiling " + fmt (ceilingDb_) + " = " + fmt (predictedHits) + " dB typical on the hits > cap " + fmt (cap) + " (" + loudnessOption_ + ") -> trim " + fmtSigned (capTrim)); trim = capTrim; capped = true; }
        }
        bool atCeiling = false;
        if (cur + trim > kLevelMaxDb) { trim = kLevelMaxDb - cur; atCeiling = true; }
        if (cur + trim < -kLevelMaxDb) { trim = -kLevelMaxDb - cur; atCeiling = true; }
        log ("measured: max short-term " + fmt (measured) + " LUFS (input window " + fmt (lastInputWindow_) + "), target " + fmt (target_) + ", needed " + fmtSigned (needed) + " dB, Level " + fmtSigned (cur) + " dB, trim " + fmtSigned (trim) + (atCeiling ? " (Level ceiling)" : "") + ", limiter GR avg " + fmt (grAvg()) + " max " + fmt (grMax()) + " dB, round " + juce::String (round_));
        const juce::String grText = grText_();
        // 22 Sep 2026 (ruling 4): the true-peak values THEMSELVES, not only their difference
        { const auto hm2 = hitsMeasure(); log ("hits: typical " + fmt (hm2.typicalDb) + " dB over the top 20 % of " + juce::String (hm2.blocks) + " blocks, worst " + fmt (hm2.worstDb) + " dB"); }   // 21m item 3: both figures, for the re-calibration
        log ("true peak: Level OUT TP " + fmt (lv->outputLevels().truePeakDb) + " dBTP, chain OUT TP " + fmt (host_.getChainOutLevels().truePeakDb) + " dBTP, hits " + fmt (juce::jmax (0.0f, grPeakEstimateDb())) + " dB" + (grIsEstimated() ? " (third-party limiter: the hits figure is the report)" : " (EchoJay Limiter: its own GR reading " + fmt (grMax()) + " dB is the report)"));
        if (capped)
        {
            state_ = State::proposed; pendingTrim_ = trim; pendingKind_ = PendingKind::propose; ++proposals_;
            const float cappedLevel = measured + trim;
            juce::StringArray pills { "Push it anyway", "Leave it" }; if (applied_) pills.add ("Undo");
            emit ("Measured " + fmt (measured) + " LUFS (loudest 3 s). " + fmt (cappedLevel) + " is as loud as this goes with the limiter working <=" + juce::String ((int) std::round (cap)) + " dB. Push to " + fmt (target_) + " anyway? " + grText, -1.0f, false, false, Bubble::Kind::proposal, pills);
            return;
        }
        if (reportOnly_)
        {   // 18h (item 4): a Check REPORTS - on target, or the distance - with the result pills (Push it only when short), then watches
            reportOnly_ = false; state_ = State::tracking; startTracking();
            const bool on = std::abs (needed) <= kCloseEnoughDb;
            emit ("Hitting " + fmt (measured) + " LUFS (loudest 3 s), target " + fmt (target_) + (on ? " - on target." : " - " + fmt (std::abs (needed)) + " dB " + (needed > 0 ? "under." : "over.")) + " Peaks " + fmt (truePeakDb) + " dBTP, " + grText, -1.0f, false, true, Bubble::Kind::result, pillsFor (needed));
            return;
        }
        if (std::abs (needed) <= kCloseEnoughDb)
        {
            state_ = State::tracking; startTracking();
            emit ("Hitting " + fmt (measured) + " LUFS (loudest 3 s), target " + fmt (target_) + " - on target. Peaks " + fmt (truePeakDb) + " dBTP, " + grText + " I'll keep watching for a louder section.", -1.0f, false, true, Bubble::Kind::result, resultPills());
            return;
        }
        if (proposals_ >= kMaxProposals)
        {   // 18g (item 2): three proposals made - the result bubble, the watch continues (flagged: the ruling's words "within a dB"
            // are used only when that is true; otherwise the distance is stated)
            state_ = State::tracking; startTracking();
            const bool within = std::abs (needed) <= kCloseEnoughDb;
            emit ("Hitting " + fmt (measured) + " LUFS" + (within ? ", within a dB - leaving it." : " after " + juce::String (proposals_) + " passes, " + fmt (std::abs (needed)) + " dB " + (needed > 0 ? "under" : "over") + " the target - leaving it.") + " " + grText, -1.0f, false, true, Bubble::Kind::result, pillsFor (needed));
            return;
        }
        if (atCeiling && std::abs (trim) < 0.05f)
        {
            state_ = State::hold; stopTimer();
            emit ("Hitting " + fmt (measured) + " LUFS, target " + fmt (target_) + " - the Level slot is at its " + fmtSigned (cur) + " dB limit; " + grText + " Push it drives the compressor and saturator harder.", -1.0f, false, true, Bubble::Kind::stuck, stuckPills());
            return;
        }
        pendingTrim_ = trim; pendingKind_ = PendingKind::propose; state_ = State::proposed; stopTimer(); ++proposals_;
        emit ("Measured " + fmt (measured) + " LUFS (loudest 3 s). Push " + fmtSigned (trim) + " dB to reach " + fmt (target_) + "?" + (atCeiling ? " (that is the Level slot's limit)" : "") + " " + grText, -1.0f, false, false, Bubble::Kind::proposal, applied_ ? proposalAfterApplyPills() : proposalPills());
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
    // 21m item 1: the Level slot by identity - the index is looked up from the instance every time
    int resolveLevelSlot() const
    {
        if (levelPtr_ == nullptr) return -1;
        for (int i = 0; i < host_.getNumSlots(); ++i) if (host_.getSlotProcessor (i) == levelPtr_ && host_.getSlotInfo (i).name == "EchoJay Level") return i;
        return -1;
    }
    int resolveLimiterSlot() const
    {
        if (limiterPtr_ == nullptr) return -1;
        for (int i = 0; i < host_.getNumSlots(); ++i) if (host_.getSlotProcessor (i) == limiterPtr_) return i;
        return -1;
    }
    EedLevelProcessor* levelNow() const { slot_ = resolveLevelSlot(); if (limiterPtr_ != nullptr) limiterSlot_ = resolveLimiterSlot(); return level (slot_); }
    EedLimiterProcessor* echoJayLimiter() const
    {
        if (limiterSlot_ < 0 || limiterSlot_ >= host_.getNumSlots()) return nullptr;
        return dynamic_cast<EedLimiterProcessor*> (host_.getSlotProcessor (limiterSlot_));
    }
    juce::String limiterName() const { return limiterSlot_ >= 0 && limiterSlot_ < host_.getNumSlots() ? host_.getSlotInfo (limiterSlot_).name : juce::String ("none"); }
    juce::String grText_() const
    {
        if (echoJayLimiter() == nullptr)   // 18g (item 4): the estimate, never "not an EchoJay device"
        {   // 18h (item 5): the loudness difference misses a limiter that gives the level back (auto make-up / level matching) or works
            // only on the hits; the PEAK difference (Level OUT true peak - chain OUT true peak, both max-held over the window) shows them
            // 22 Sep 2026 (ruling 4): the short-term-loudness "average" is dropped - the loop reports the hits only, the
            // max-held true-peak difference (Level OUT - chain OUT); with the EchoJay Limiter present its own GR reading replaces it
            if (estN_ == 0) return "limiter GR not measured yet.";
            const auto hm = hitsMeasure();
            if (std::isfinite (hm.typicalDb)) return "limiter working ~" + fmt (hm.typicalDb) + " dB on the hits (worst peak " + fmt (hm.worstDb) + ").";   // 21m item 3
            const float pk = grPeakEstimateDb();
            return std::isfinite (pk) ? "limiter catching up to ~" + fmt (juce::jmax (0.0f, pk)) + " dB on the hits." : "limiter GR not measured yet.";
        }
        {   const auto hm = hitsMeasure();   // 21m item 3: the same block measure; the EchoJay Limiter's own max GR is the worst peak
            if (std::isfinite (hm.typicalDb)) return "limiter working " + fmt (hm.typicalDb) + " dB on the hits (worst peak " + fmt (juce::jmax (hm.worstDb, grMax())) + ")."; }
        return "limiter catching up to " + fmt (grMax()) + " dB on the hits.";
    }
    void startWindow()
    {
        host_.setChainOutCountFloor (kCountFloorLufs);
        host_.resetChainOutLevels();
        host_.resetChainOutShortTermMax(); host_.resetChainInShortTermMax();
        if (auto* lim = echoJayLimiter()) lim->resetOutputPeak();
        if (auto* lv = levelNow()) lv->resetMeters();   // 18h: the Level's IN/OUT meters (and their peak holds) describe THIS window
        lastCounted_ = 0.0f; waitingSaid_ = false; passStartMs_ = nowMs();
        grMin_ = std::numeric_limits<float>::max(); grMax_ = 0.0f; grSum_ = 0.0f; grN_ = 0;
        estSum_ = 0.0f; estN_ = 0; estMax_ = 0.0f;
        state_ = State::waitAudio;
    }
    void startTracking()
    {
        host_.resetChainOutLevels(); host_.resetChainOutShortTermMax(); lastCounted_ = 0.0f;
        if (! isTimerRunning() && juce::MessageManager::getInstanceWithoutCreating() != nullptr) startTimer (kTickMs);
    }
    void applyTrim (float trim, const char* why)
    {
        auto* lv = levelNow(); if (lv == nullptr) return;
        lastCommanded_ = trim; prevMeasured_ = lastMeasured_;   // 18g (item 2): the next proposal scales its step by achieved/commanded
        const float newDb = juce::jlimit (-kLevelMaxDb, kLevelMaxDb, (float) lv->gainDb() + trim);
        appliedDelta_ = newDb - (float) lv->gainDb();   // 21 Sep: the delta the after-verb bubble reports
        log (juce::String (why) + ": Level " + fmtSigned ((float) lv->gainDb()) + " -> " + fmtSigned (newDb) + " dB (trim " + fmtSigned (trim) + ")");
        writeGainDb (newDb);
    }
public:
    void writeGainDb (float db)   // 21n item 3: public - the undo dispatcher restores a loop entry through the same write
    {   // through the host so the card and the dial info follow
        const float before = currentGainDb();
        if (onGainWritten && std::abs (before - db) > 0.01f) onGainWritten (before, db);
        auto* p = new juce::DynamicObject(); p->setProperty ("gain_db", (double) db);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (p));
        host_.setSlotStructuredSettings (slot_, juce::var (w));
        if (auto* lv = levelNow()) if (std::abs (lv->gainDb() - db) > 0.01) lv->setParamValue ("gain_db", db);
        host_.setSlotSettings (slot_, "Level " + fmtSigned (db) + " dB (set by the level loop toward the " + fmt (target_) + " LUFS target)");
    }
    void emit (const juce::String& text, float progress, bool replace, bool final, Bubble::Kind kind = Bubble::Kind::info, juce::StringArray pills = {})
    {
        lastBubble_ = text; lastKind_ = kind; lastPills_ = pills;
        if (kind != Bubble::Kind::progress) ++bubbleCount_;
        log ("bubble: " + text + (pills.isEmpty() ? juce::String() : " [" + pills.joinIntoString (" | ") + "]"));
        if (onBubble) { Bubble b; b.text = text; b.progress = progress; b.replace = replace; b.final = final; b.kind = kind; b.pills = pills; onBubble (b); }
    }
    void log (const juce::String& s) const { if (logLine) logLine ("EJLoudness: " + s); }
    static juce::String fmt (float v) { return std::isfinite (v) ? juce::String (v, 1).replace ("-0.0", "0.0") : juce::String ("n/a"); }
    static juce::String fmtSigned (float v) { return (v >= 0.0f ? "+" : "") + fmt (v); }

    ChainHost& host_;
    float trimDeltaDb_ = 0.0f;   // 21m ruling 2: the sum of unity-trim changes at the last window close
    State state_ = State::idle;
    mutable int slot_ = -1, limiterSlot_ = -1; int round_ = 0;   // slot_/limiterSlot_ are caches re-resolved from the instances (21m item 1), hence mutable
    // 22 Sep 2026 (21m item 1): the loop tracks its Level slot and its limiter by IDENTITY (the processor instances), never by
    // index - an insert/remove/reorder of OTHER slots moves the indices, the instances stay; the indices are re-resolved on every call
    EedLevelProcessor* levelPtr_ = nullptr; juce::AudioProcessor* limiterPtr_ = nullptr;
    float target_ = -9.0f;
    juce::String armSource_, loudnessOption_;
    float ceilingDb_ = -0.1f;
    float lastMeasured_ = std::numeric_limits<float>::quiet_NaN(), lastInputWindow_ = std::numeric_limits<float>::quiet_NaN(), buildInputLufs_ = std::numeric_limits<float>::quiet_NaN();
    float pendingTrim_ = 0.0f; PendingKind pendingKind_ = PendingKind::none;
    float preLoopGainDb_ = 0.0f; bool haveUndo_ = false;
    float lastCounted_ = 0.0f; bool waitingSaid_ = false; juce::int64 passStartMs_ = 0;
    float grMin_ = std::numeric_limits<float>::max(), grMax_ = 0.0f, grSum_ = 0.0f; int grN_ = 0;
    juce::String lastBubble_; Bubble::Kind lastKind_ = Bubble::Kind::info; juce::StringArray lastPills_; int bubbleCount_ = 0;
    float quietMeasured_ = std::numeric_limits<float>::quiet_NaN();   // the measurement held while the quiet-window question is open
    // 18g
    int   proposals_ = 0;                                              // proposals made since arm (cap kMaxProposals)
    float lastCommanded_ = 0.0f, prevMeasured_ = std::numeric_limits<float>::quiet_NaN();   // the previous pass, for the step scaling
    bool  continueAfterGo_ = false;                                    // 21 Sep: the Check after Go continues the sequence (may propose); after other verbs a Check reports
    bool  applied_ = false; float appliedDelta_ = 0.0f;                // 21 Sep: any apply so far -> proposals carry [Undo]; the last apply's real delta (after the clamp)
    bool  reportOnly_ = false;                                         // 18h: a Check reports (result pills), never proposes
    float estSum_ = 0.0f, estMax_ = 0.0f; int estN_ = 0;               // third-party limiter GR estimate over the window
};
