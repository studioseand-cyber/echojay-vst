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
//  Measure  the INTEGRATED loudness of the window (LevelTally::levelDb, K-weighted, so it IS LUFS) across a window
//           of 10 s of counted audio (hops above -40 LUFS) at the chain OUTPUT, post limiter. The loudest 3 s
//           (maxShortTermDb) is still measured and is the SAFETY CHECK, never the target: 7 Oct 2026 ruling, after
//           a bus that measured -11.4 loudest-3-s was proposed +3.4 dB when the right answer was +8. The INPUT side
//           (before the Level slot) is measured over the same window for the window sanity check.
//  Sanity   counted audio >= 3 dB under the build-time integrated input -> "that sounded like a quiet section -
//           play the chorus and I'll try again", nothing applied, the window restarts.
//  Ask      "Measured -X LUFS integrated. Push +Y dB to reach -8? say go" -> apply on go/apply -> re-measure ->
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
#include "EchoJayReadingGate.h"   // 21p item 1: the one validity gate
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
    // 21r item 1: THE ARM BUBBLE'S TEXT AS A CONSTANT, and the pills that belong to the loop's CURRENT state.
    // Pills are not persisted with a chat message - a reload rebuilds the bubbles from the workspace and they come
    // back bare, which is why the Listen button vanished on 21q. They are re-attached from the LIVE loop instead,
    // which is also the honest rule: a stale bubble from a previous build must never carry a live verb.
    static juce::String armBubbleText()      { return "Cue the loudest section, press play, then tap Listen."; }
    /** 08c F2: THE ARM BUBBLE AS IT IS WRITTEN, aim included, with ONE author. The aim words went onto the emit
        and not onto this, so liveBubbleText() returned a different string from the one in the chat - and
        everything that finds the arm bubble by its text (the Listen reattachment, the one-bubble-per-build check)
        found nothing. Two authors for one string, which is the fault I closed twice this morning on the chains
        list's edge and the Link card's layout. */
    juce::String armBubbleTextNow() const    { return armBubbleText() + " (" + aimWords() + ")"; }
    juce::StringArray livePills() const      { return state_ == State::armed ? armPills() : juce::StringArray(); }
    juce::String      liveBubbleText() const { return state_ == State::armed ? armBubbleTextNow() : juce::String(); }
    /** 08c F2 (ruled): THE BUILD SAYS WHAT IT IS DOING, in words, on the arm bubble and on the Level card. A
        -12 "dynamic" build and a -8 "pushed" build behave completely differently - 3 dB of allowed GR against 12 -
        and on 8 Oct the only place that appeared was a log line, so "it is quieter than this morning" could not be
        answered from the screen. */
    juce::String aimWords() const
    {
        if (aim_ == Aim::matchInput) return "matched to input";
        juce::String w = fmt (target_) + " LUFS";
        if (loudnessOption_.equalsIgnoreCase ("dynamic"))     w += ", keeping dynamics";
        else if (loudnessOption_.equalsIgnoreCase ("pushed")) w += ", pushed";
        else if (loudnessOption_.isNotEmpty())                w += ", " + loudnessOption_;
        return w;
    }
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
    // 7 Oct 2026 RULING (Sean, 20:09, on measurements): these were below normal mastering practice. His reference
    // is Pro-L 2 at +8.2 dB on a -8 LUFS master, where 4-6 dB of real GR on the loud sections is ordinary, and the
    // bx_limiter in his own chain showed 1.3-1.7 dB where the old model predicted 6.3. A cap of 6 dB on what we
    // will even OFFER is a cap below what the work needs.
    static float grCapDb (const juce::String& option) noexcept
    {
        if (option == "pushed")  return 12.0f;
        if (option == "dynamic") return 3.0f;    // unchanged: "dynamic" is a promise to keep the dynamics
        if (option == "keep")    return 1.0f;    // unchanged: "keep" is a promise to change nothing
        // 08c F2: "match" asks for out = in, so the chain is not being pushed up to anything and the GR a
        // correct match produces is whatever the chain's own plugins produce. 3 dB, as "dynamic": a match that
        // needs more GR than that is not a match, it is a chain that squashes, and the cap says so.
        if (option == "match")   return 3.0f;
        return 10.0f;  // commercial (and the default) - was 6.0
    }
    // THE THIRD-PARTY GR ESTIMATE (7 Oct 2026). Where the limiter's GR is readable - the EchoJay Limiter - it is
    // MEASURED and no model is used at all. For a third-party limiter the old fallback was "every dB of true peak
    // above the ceiling becomes gain reduction", which is what a sample-peak brickwall does and not what a
    // lookahead true-peak limiter shows: at +4 dB Sean's chain had about 7.0 dB of excess over the ceiling against
    // 1.5 dB of real GR, a ratio of about 0.21. So the estimate is scaled and CLAMPED, and the clamp is the point -
    // an estimate that cannot exceed 3 dB cannot hold a master 5 dB down on a guess, and the loud-window
    // correction, which reads the real thing, is what finishes the job. PROVISIONAL on Sean's calibration points:
    // every window logs estimate vs measured so the next point arrives for free.
    static constexpr float kThirdPartyGrFactor = 0.25f;
    static constexpr float kThirdPartyGrMaxDb  = 3.0f;
    static constexpr int   kMaxProposals     = 3;        // 18g: at most 3 proposals, then the result bubble (was 4 rounds)
    static constexpr float kRatioMin         = 0.5f, kRatioMax = 2.0f;   // 18g: achieved/commanded clamp for the step scaling
    static constexpr float kGrOfferDb        = 6.0f;
    static constexpr int   kWaitWallMs       = 60000;
    // LISTEN ALWAYS RESOLVES (7 Oct 2026 ruling, Sean 20:00). He cued the loudest section, played, tapped Listen
    // twice, and then nothing happened indefinitely. There was no deadline anywhere: three returns in the tick -
    // not enough counted audio, a sensor that never reports, and an open knob gesture - each waited for ever, and
    // the only thing ever said while waiting was one line at kWaitWallMs (SIXTY seconds), said once.
    // The window now resolves within kResolveMs of PLAYBACK: measured from the Listen tap, and extended ONCE to
    // kResolveMs after the first counted audio, so the clock means playback rather than the time he spent cueing.
    static constexpr int   kResolveMs        = 20000;
    // The loud window is a SAFETY CHECK on the decision, never the target (the target is integrated). This is how
    // far above the target the projected loudest 3 s may sit before the log calls it out.
    static constexpr float kShortTermWatchDb = 6.0f;
    static constexpr int   kTickMs           = 250;

    explicit LoudnessLoop (ChainHost& host) : host_ (host) {}
    ~LoudnessLoop() override { stopTimer(); }

    std::function<void (const Bubble&)> onBubble;                          // message thread
    std::function<void (const juce::String&)> logLine;                      // "EJLoudness: ..." (item 5); the editor wires EchoJay_NSLog
    std::function<bool()>               isPlaying  { [] { return true; } };
    // 21p item 1: does the host publish a transport at all? Default FALSE = unknown, and unknown never blocks - a
    // host with no play head must not be read as "stopped" forever. Only a transport the host says is stopped does.
    std::function<bool()>               transportKnown { [] { return false; } };
    std::function<void (float beforeDb, float afterDb)> onGainWritten;      // 21n item 3: every loop write of the Level gain (an undo entry)
    std::function<bool()>               knobGestureOpen { [] { return echojay::knobGestureOpen(); } };
    // 7 Oct 2026 (item 2c): EchoJay'S OWN BUS GAIN, which the loop could not see. applyBusGainSmoothed runs
    // BETWEEN chainHost.process and the meter tap (PluginProcessor.cpp), so the loop lands the chain output
    // PRE bus gain while the user reads POST. The loop does not correct for it - the landing is a closed-loop
    // measurement at the chain output - but it must NAME it, because a non-zero trim means the figure the loop
    // reports and the figure on the meters differ by exactly that, for ever, and nothing said so.
    std::function<float()>              busGainDb  { [] { return 0.0f; } };
    // ---- 08c item F2 (9 Oct 2026 ruling): WHAT IS THIS BUILD AIMING AT? ------------------------------
    // Sean's rule: a CHANNEL or a BUS build VOLUME-MATCHES by default - the chain must not change the level,
    // so out = in - while a MIX BUS or MASTER build hits a loudness TARGET. The loop cannot know which it is
    // looking at, so the processor tells it: true only for FullMix, MasterBus and MusicBus.
    // Defaulted to TRUE so anything that forgets to set it behaves exactly as the shipped loop did.
    std::function<bool()>               aimIsTarget { [] { return true; } };
    // ---- LEVELLING V2 (10 Oct 2026 ruling): THERE IS NO ECHOJAY LEVEL SLOT ----------------------------
    // The loop drives a gain that ALREADY EXISTS, resolved by the processor that owns the rack:
    //   match  -> the rack's own post-rack OUT gain (a Link's setGainDb; the V2's setBusGainDb)
    //   target -> the FINAL limiter slot's own input_db (limiter v2: -12..+12, 20 ms ramp in the engine)
    // Sean's reasons, from test 1: a Level slot could sit anywhere (his sat 5th of 6, BEFORE Vocal Reverb, where
    // no gain can make out = in for the chain), and one loop bound to one ChainHost could not reach a rack owned
    // by another process at all. A stage resolved by the rack's owner has neither problem.
    struct Stage
    {
        juce::String               name;                               // "rack_out" | "limiter_in"
        std::function<bool()>      ready   { [] { return false; } };
        std::function<float()>     readDb  { [] { return 0.0f; } };
        std::function<void(float)> writeDb { [] (float) {} };
    };
    /** Wired by the processor. `match` picks which stage; the loop asks on every arm and never caches across one. */
    std::function<Stage(bool)>          resolveStage;
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
    struct ChainTarget { float lufs = std::numeric_limits<float>::quiet_NaN(); int levelSlot = -1; int limiterSlot = -1;
                     juce::String source, option;
                     juce::String optionSource;   // 9 Oct: WHICH field the option came from, or "absent"
                     float ceiling = -0.1f; };
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
        // "any brand last: the last slot holds the ceiling" - BUT NEVER THE ECHOJAY LEVEL. 9 Oct 2026: with the
        // Level correctly placed LAST on a chain that does not end in a limiter, this fallback nominated the
        // LEVEL as the limiter, the ceiling safety net below could not read a ceiling off it (of course - it is
        // a gain stage), and it REPLACED the Level with an EchoJay Limiter. A one-op EQ build came out as
        // "EchoJay EQ | EchoJay Limiter" with no Level at all. The fault is older than tonight's position fix,
        // which only made it visible: before it the fallback nominated whatever sat last instead, and the
        // substitution would have eaten that.
        if (t.limiterSlot < 0)
            for (int i = n - 1; i >= 0; --i)
                if (host_.getSlotInfo (i).name != "EchoJay Level") { t.limiterSlot = i; break; }
        for (int i = n - 1; i >= 0; --i) if (host_.getSlotInfo (i).name == "EchoJay Level") { t.levelSlot = i; break; }
        if (t.levelSlot >= 0)
        {
            if (auto* lv = dynamic_cast<EedLevelProcessor*> (host_.getSlotProcessor (t.levelSlot)))
            {
                // `lv` is now only the TYPE CHECK - this slot really is an EchoJay Level - because nothing below
                // reads the device any more. Kept for that, and named so it does not look like an oversight.
                juce::ignoreUnused (lv);
                // 9 Oct 2026 (fix 3, SECOND ATTEMPT - the first was wrong). The device's own params cannot
                // answer "did the server send a target": a READBACK path writes every param back through
                // setParamValue, so any "was it set" flag on the device is true whatever arrived.
                //   EJParamApply: slot 0 readback -> gain_db 0 dB, target_lufs -9 LUFS, loudness_option commercial
                // ...on a build whose only param was option:"match". So the STRUCTURED PARAMS are the authority,
                // because they are the only record of what the server actually wrote - and now that writeGainDb
                // merges instead of replacing, they survive the first landing.
                // AND THE DEVICE IS NOT CONSULTED AT ALL. I first kept it as a narrow fallback for a slot with
                // no params object, then could not state what it would mean: the device's DEFAULTS are -9.0 and
                // commercial, so "the server asked for -9 commercial" and "nobody asked for anything" are the
                // same reading. A fallback that cannot distinguish its two cases is not a fallback. Every chain
                // the product has ever built writes target_lufs into the params (18e's shape, and
                // ensureLevelSlot's), so the device path covered nothing real - only the defaults, which is
                // exactly how a volume-match build came to chase -9 LUFS.
                const auto tv = readParams (t.levelSlot, "target_lufs");
                if (tv.isDouble() || tv.isInt() || tv.isInt64()) { t.lufs = (float) (double) tv; t.source = "level_params"; }
                // ---- 9 Oct 2026 (CONTRACT_LEVEL_PARAMS, fix 2): `option` IS THE FIELD, and we never read it --
                // B writes `settings_structured.params.option` = "match" | "pushed" | "dynamic". The plugin read
                // the Level device's NUMERIC `loudness_option` and nothing else, so `option` - not in the
                // device's schema - was skipped by applyStructured and never seen. B's legacy `loudness_option`
                // is a STRING ("commercial", "pushed", "explicit"...), which lround()s to 0 = commercial. So we
                // wrote a field B does not read and read a field B does not write. Precedence now:
                //   1. params.option          - the contract's field, the authority
                //   2. params.loudness_option as a STRING - B's legacy word
                //   3. the device's numeric option, ONLY if something actually set it
                //   4. empty - nothing was asked, and armFromChain applies the contract's fallback
                const auto ov = readParams (t.levelSlot, "option");
                const auto lo = readParams (t.levelSlot, "loudness_option");
                if (ov.isString() && ov.toString().isNotEmpty())       { t.option = ov.toString().toLowerCase(); t.optionSource = "params.option"; }
                else if (lo.isString() && lo.toString().isNotEmpty())  { t.option = lo.toString().toLowerCase(); t.optionSource = "params.loudness_option (legacy word)"; }
                else                                                   { t.option = {}; t.optionSource = "absent"; }
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
    // ---- LEVELLING V2 (10 Oct 2026 ruling): NO LEVEL SLOT IS EVER INSERTED --------------------------
    // What a build needs instead:
    //   a MATCH build: nothing at all. It drives the rack's own OUT gain and is not pushing into anything.
    //   a TARGET build: a limiter LAST, because a target drives level INTO a ceiling and there must be one. If
    //     the chain already ends in a limiter it is used as it stands; otherwise an EchoJay Limiter is inserted
    //     last at the ruled -0.1 dBTP.
    // This also retires the question left open on 9 Oct - whether to substitute a limiter for whatever sat last.
    // Nothing is ever substituted now: a match build needs no limiter, and a target build gets a REAL one added
    // rather than a plugin the user asked for being replaced.
    /** Returns the final limiter's slot index for a target build (inserting one if needed), or -1. */
    int ensureLimiterForTarget()
    {
        const int n = host_.getNumSlots();
        if (n > 0 && ChainHost::isLimiterLikeName (host_.getSlotInfo (n - 1).name))
            return n - 1;                                   // the chain already ends in one
        const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (dev == nullptr) { log ("a target build needs a limiter last, but EchoJay Limiter is not registered"); return -1; }
        const auto err = host_.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*dev), n);
        if (err.isNotEmpty()) { log ("could not insert the final EchoJay Limiter: " + err); return -1; }
        const int at = host_.getNumSlots() - 1;
        auto* pp = new juce::DynamicObject();
        pp->setProperty ("ceiling_db", (double) ChainHost::kFinalCeilingDb);   // the ruled -0.1, held
        pp->setProperty ("true_peak", 1);
        pp->setProperty ("input_db", 0.0);                                     // the stage the loop will drive
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
        host_.setSlotStructuredSettings (at, juce::var (w));
        host_.setSlotSettings (at, "ceiling " + fmt (ChainHost::kFinalCeilingDb)
                                   + " dBTP, true peak on - added because this build aims at a loudness target "
                                     "and a target needs a ceiling to drive into");
        log ("inserted EchoJay Limiter last at slot " + juce::String (at) + ", ceiling "
             + fmt (ChainHost::kFinalCeilingDb) + " dBTP (a target build with no limiter)");
        return at;
    }

    // Arms when the chain carries a target: from the Level slot, inserting one first when the target sits on the limiter
    // (an 18d-shaped chain). Returns false (and says why on the log) otherwise.
    bool armFromChain (int /*passes*/ = 2)
    {
        // LEVELLING V2: a chain written before 10 Oct may still carry an EchoJay Level slot. Normalise it FIRST
        // - its gain moves to the stage and the slot goes - so everything below reasons about one shape. The
        // stage is not resolved yet, so this runs again after it is (see below); the first call only reports.
        // 08c F2: THE AIM IS DECIDED FIRST, because it decides whether a Level slot is owed at all. The order
        // used to be "insert a slot if there is a target, then read the target", which could never serve a match
        // build - it has no target to trigger the insertion.
        {
            const auto probe = findTarget();
            const bool byType = aimIsTarget ? aimIsTarget() : true;   // Sean: only FullMix and MasterBus hit a target
            if (probe.option.isNotEmpty())
            {
                // THE SERVER SAID WHAT IT WANTED, so it decides - including "pushed" on a channel, which B's
                // contract allows ("a channel build where the user asked for a level change").
                aim_ = probe.option.equalsIgnoreCase ("match") || probe.option.equalsIgnoreCase ("keep")
                           ? Aim::matchInput : Aim::hitTarget;
                log ("aim from the chain: option \"" + probe.option + "\" via " + probe.optionSource
                     + " -> " + juce::String (aim_ == Aim::matchInput ? "volume match" : "hit the target"));
            }
            else if (std::isfinite (probe.lufs))
            {
                // NO OPTION BUT A TARGET. B's contract says treat it as "pushed"; Sean's ruling says a channel or
                // bus volume-matches. They only collide here, on a chain built before 9 Oct or by an older
                // server. SEAN'S RULING WINS, because he is the one who decides, and the disagreement is LOGGED
                // rather than settled silently - a rule that quietly loses is worse than one that is argued.
                aim_ = byType ? Aim::hitTarget : Aim::matchInput;
                log (juce::String ("aim: no option in the chain but a target of ") + fmt (probe.lufs)
                     + " LUFS is present. CONTRACT_LEVEL_PARAMS would read that as \"pushed\"; Sean's 9 Oct rule "
                       "is that only a mix bus or master hits a target. This channel "
                     + (byType ? "IS one, so the target stands" : "is NOT one, so it volume-matches instead")
                     + " - flagged, not settled.");
            }
            else
            {
                // NEITHER. The contract's fallback and Sean's rule agree: match.
                aim_ = Aim::matchInput;
                log ("aim: no option and no target in the chain -> volume match (CONTRACT_LEVEL_PARAMS fallback)");
            }
        }
        // LEVELLING V2: what the aim needs. A match build needs NOTHING added; a target build needs a limiter
        // last, because it drives level into a ceiling.
        int limiterForTarget = -1;
        if (aim_ == Aim::hitTarget) limiterForTarget = ensureLimiterForTarget();
        auto t = findTarget();   // the limiter may have just been added
        // The contract agreed with B: option "match" | "pushed" | "dynamic", and "match" is accepted WITHOUT a
        // target. So a missing target is a refusal only when this build was supposed to hit one.
        if (aim_ == Aim::matchInput && ! std::isfinite (t.lufs))
            t.lufs = 0.0f;   // a placeholder: in match mode the real target is re-read from the input each window
        if (! std::isfinite (t.lufs)) { log ("not armed: no target in the chain"); return false; }
        if (aim_ == Aim::hitTarget && limiterForTarget < 0)
        { log ("not armed: a target build needs a limiter last and none could be placed"); return false; }
        armSource_ = t.source; ceilingDb_ = t.ceiling;
        // 9 Oct 2026: the option the loop RUNS on is the aim it decided, not a word the chain may not carry. In
        // match mode that word is "match" whatever the chain said, so grCapDb and aimWords agree with the aim.
        // Ruling 3 (Sean, 9 Oct): COMMERCIAL STAYS DISTINCT FROM PUSHED - 10 dB of allowed GR against 12, as
        // ruled on 7 Oct. So a MISSING option falls back to "commercial", which is what the Level device has
        // always defaulted to and the conservative cap of the two; falling back to "pushed" (as I first wrote
        // it) would have retired the distinction by the back door on every chain that carries no option.
        loudnessOption_ = aim_ == Aim::matchInput ? juce::String ("match")
                                                  : (t.option.isNotEmpty() ? t.option : juce::String ("commercial"));
        substituteLimiterIfNoCeilingReadback (t);   // 18g (item 5): the ceiling must be CONFIRMED before the loop drives into it
        // LEVELLING V2: THE STAGE. Resolved by the processor that owns this rack - a Link resolves its own, the
        // V2 resolves its own - so the loop never reaches across a lease. match -> the rack's OUT gain; target ->
        // the final limiter's input_db.
        if (! resolveStage) { log ("not armed: no gain stage is wired on this processor"); return false; }
        stage_ = resolveStage (aim_ == Aim::matchInput);
        if (! stageReady())
        { log ("not armed: the " + (stage_.name.isNotEmpty() ? stage_.name : juce::String ("gain stage")) + " is not available"); return false; }
        log ("stage: " + stage_.name + ", currently " + fmtSigned (currentGainDb()) + " dB");
        migrateLevelSlotIfPresent();   // now the stage exists, an old Level slot's gain has somewhere to go
        writeRecord (-1.0f);   // the aim is recorded at rack level before the first landing
        arm (t.lufs, -1, aim_ == Aim::hitTarget ? limiterForTarget : t.limiterSlot);
        return true;
    }
    // 18g (item 5, safety net): the last limiter is a third-party slot whose ceiling control has NO dial readback (no map, or the
    // ceiling was not among the applied controls) -> it is replaced by EchoJay Limiter holding the chain's ceiling, said in one line.
    // A third-party limiter whose ceiling READ BACK stays (the loop estimates its GR).
    bool substituteLimiterIfNoCeilingReadback (ChainTarget& t)
    {
        if (t.limiterSlot < 0 || t.limiterSlot >= host_.getNumSlots()) return false;
        if (dynamic_cast<EedLimiterProcessor*> (host_.getSlotProcessor (t.limiterSlot)) != nullptr) return false;
        // THE BELT TO findTarget'S BRACES (9 Oct 2026): this function REMOVES the slot it substitutes, so a
        // wrong limiterSlot does not merely mis-report - it destroys a slot. It must therefore refuse the one
        // slot the loop cannot do without, whatever nominated it, rather than trust that nothing ever will.
        // The closing comment here used to read "the Level slot sits before it and is untouched", which was an
        // assumption about placement and stopped being true the moment the Level was placed last.
        if (dynamic_cast<EedLevelProcessor*> (host_.getSlotProcessor (t.limiterSlot)) != nullptr)
        {
            log ("ceiling readback absent at slot " + juce::String (t.limiterSlot) + " but that slot is the "
                 "EchoJay Level - REFUSED: substituting there would delete the slot the loop drives");
            return false;
        }
        // AND IT MUST PLAUSIBLY BE A LIMITER AT ALL (9 Oct 2026). I flagged this as "not mine to widen" an hour
        // ago and the gate made the decision for me: a one-op EQ build came out as "EchoJay Limiter | EchoJay
        // Level". The intent of the old rule - "any brand last: the last slot holds the ceiling" - is sound when
        // the last slot IS a limiter of a brand we cannot read. It is nonsense when the chain has no limiter at
        // all, and then this function deletes a plugin the user asked for and puts a limiter in its place.
        // A chain with no limiter simply has no ceiling to confirm; the loop already copes (the GR estimate
        // path), and the loud-window check is the safety either way.
        // STILL FOR SEAN: whether a chain with no limiter should have one INSERTED before the loop drives level
        // into it. That is a product question, it is not this function's to answer, and refusing is the
        // conservative answer until he rules.
        const auto nomName = host_.getSlotInfo (t.limiterSlot).name;
        if (! ChainHost::isLimiterLikeName (nomName))
        {
            log ("no limiter in this chain - the last slot is \"" + nomName + "\", which is not limiter-like, so "
                 "there is no ceiling to confirm and NOTHING is substituted. The loop runs on the GR estimate and "
                 "the loud-window check.");
            return false;
        }
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
        t.limiterSlot = at; ceilingDb_ = ceiling;   // never the Level: refused above, whatever nominated it
        log ("substituted EchoJay Limiter for " + oldName + " at slot " + juce::String (at) + ": its ceiling had no dial readback; ceiling " + fmt (ceiling) + " dBTP");
        emit (oldName + "'s ceiling could not be confirmed, so EchoJay Limiter holds the ceiling instead (" + fmt (ceiling) + " dBTP).", -1.0f, false, false, Bubble::Kind::info);
        return true;
    }
    juce::String armSource() const noexcept { return armSource_; }
    juce::String loudnessOption() const noexcept { return loudnessOption_; }
    float ceilingDb() const noexcept { return ceilingDb_; }

    void arm (float targetLufs, int levelSlot, int limiterSlot)
    {
        // LEVELLING V2: there is no Level slot to resolve. `levelSlot` is kept in the signature (callers and
        // harnesses pass -1) so the 18e call shape survives, and the stage is already resolved by armFromChain.
        juce::ignoreUnused (levelSlot);
        target_ = targetLufs; slot_ = -1; limiterSlot_ = limiterSlot; round_ = 0; pendingTrim_ = 0.0f;
        levelPtr_ = nullptr;
        limiterPtr_ = (limiterSlot >= 0 && limiterSlot < host_.getNumSlots()) ? host_.getSlotProcessor (limiterSlot) : nullptr;   // 21m item 1: identity, not index
        if (! haveUndo_) { preLoopGainDb_ = currentGainDb(); haveUndo_ = true; }
        const auto in = host_.getChainInLevels();
        buildInputLufs_ = in.known ? in.levelDb : std::numeric_limits<float>::quiet_NaN();
        // ---- THE PEAK-HEADROOM CAP IS GONE (7 Oct 2026 ruling, Sean 19:52) ----------------------------------
        // What was here: opening gain = min (estimate, ceiling + 3 dB - build-time true peak), so "peaks into the
        // limiter never open more than 3 dB over the ceiling". It was a BLIND distortion guard, applied before
        // anything had been heard. The intent was right and 3 dB was the wrong number for a master: on Sean's mix
        // bus it computed -0.1 + 3.0 - 2.9 = 0.0 dB and turned a +4.4 dB Level into ZERO, which is why every
        // mix-bus build came out quiet. A hot bus got no push at all, whatever the target said.
        // SAFETY NOW COMES FROM TWO THINGS INSTEAD, both with evidence behind them: the limiter's own ceiling,
        // and the one automatic correction after the first loud window, which reads ACTUAL GR and backs off only
        // when the loud sections exceed grCapDb. A cap for TRACK chains was offered and not taken: its only stated
        // purpose was blind safety before any listening, and the GR check is that same safety with a measurement.
        //
        // THE OPENING FIGURE, and it is an ESTIMATE until something has been heard - said so, in the log.
        // Closed loop where we can: the output integrated with the Level where it is now tells us the whole chain's
        // behaviour in one number, past the pre-chain gain, the slot gains and the bus trim alike (item 2c). Open
        // loop only when nothing has been heard yet, and then it is the build-time input plus nothing - never a
        // prediction of what the plugins will do.
        // ---- 08c item F2 (9 Oct 2026): THE OPENING NEEDS ITS OWN WINDOW ---------------------------------
        // The arm timing was never the fault - armLoudnessLoopIfTargeted already runs from the dial-settled path.
        // The READING was. `getChainOutLevels().levelDb` is integrated over everything heard since the last
        // reset, so on 8 Oct it spanned minutes of the chain as it stood BEFORE its dials landed: the opening
        // solved for -15.3 while the settled chain actually delivered -14.6 once the API-2500 took its 2.4 dB.
        // An integrated figure that predates the chain it describes is not a measurement of that chain.
        //
        // So the tallies are RESET here and the landing is OWED, not written: it happens on the first tick where
        // both the input and the output have a reading of THIS chain. That is also exactly Sean's rule - "once
        // dialStateSettled fires, measure chain IN and OUT integrated over the same window".
        host_.resetChainOutLevels();
        // THE LOOP'S OWN INPUT WINDOW, never the song's. getChainInLevels() is the song's integrated reading and
        // Sean's standing rule is that a loop reset does not clear it, so ChainHost carries a second tally off
        // the same tap for this (08c F2). Resetting the song's reading here would have been a regression dressed
        // up as a fix.
        host_.resetChainInLoopLevels();
        openingOwed_ = true; openingResetSeen_ = false;
        log (juce::String ("opening gain OWED: the chain tallies are reset, so the landing is measured on this ")
             + "chain rather than on an integrated figure that predates its dials. Aim: " + aimWords()
             + ". No peak-headroom cap (7 Oct ruling): the ceiling and the loud-window GR check are the safety."
             + busGainNote());
        log ("armed: target " + fmt (target_) + " LUFS (" + armSource_ + (loudnessOption_.isNotEmpty() ? ", " + loudnessOption_ : juce::String()) + "), Level slot " + juce::String (slot_)
             + " gain " + fmtSigned (currentGainDb()) + " dB, limiter slot " + juce::String (limiterSlot_) + " (" + limiterName() + "), build-time input " + fmt (buildInputLufs_) + " LUFS");
        // 18g (item 1): NO window runs on the first audio. The user cues the loudest section and taps Listen (or types it).
        state_ = State::armed; proposals_ = 0; lastCommanded_ = 0.0f; prevMeasured_ = std::numeric_limits<float>::quiet_NaN();
        emit (armBubbleTextNow(), -1.0f, false, false, Bubble::Kind::arm, armPills());
        if (! juce::MessageManager::getInstanceWithoutCreating() || ! isTimerRunning()) startTimer (kTickMs);   // the tick feeds the Level card's GR while armed
    }
    // 18g (item 1): Listen starts the measuring window - from armed, from a Check prompt, from the quiet-window question, or
    // after Done / Leave it (a fresh listen). Check is the same window after Go when the audio had stopped.
    bool listen()
    {
        if (! stageReady()) return false;
        // A LISTEN TAP WHILE A PROPOSAL IS OPEN RE-SHOWS THE CARD (7 Oct 2026). It used to be refused outright,
        // which is indistinguishable from the plugin ignoring the tap - and a proposal the user has scrolled past
        // is exactly when they tap Listen again. A fresh window would throw away a measurement they can still act
        // on, so the card comes back instead.
        if (state_ == State::proposed && pendingKind_ != PendingKind::none)
        {
            log ("Listen while a proposal is open: re-showing it rather than starting a new window");
            reShowProposal();
            return true;
        }
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
        if (! stageReady()) return false;
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
        if (state_ != State::proposed || ! stageReady()) return false;
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
        if (state_ != State::quietAsked || ! stageReady() || ! std::isfinite (quietMeasured_)) return false;
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
        if (! stageReady() || ! std::isfinite (lastMeasured())) return false;
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
    bool backOffComplaint() { if (! stageReady()) return false; log ("complaint -> softer x2"); nudgeTarget (-2.0f); return true; }
    void nudgeTarget (float deltaDb)   // "a bit louder" / "a bit softer": target +-1 AND the Level moves by it now (one pass), then one check
    {
        if (! stageReady()) return;
        target_ += deltaDb;
        log ("target nudged " + fmtSigned (deltaDb) + " dB -> " + fmt (target_) + " LUFS");
        // 18g/18h: the nudge IS the move - the Level moves by +-1 now; 18h: no automatic check, the bubble asks how it sounds
        applyTrim (deltaDb, deltaDb > 0 ? "a bit louder" : "a bit softer"); applied_ = true;
        afterVerb (deltaDb);
    }
    void recheck()                 // "check the level again" = Check (18h): one window, a report with the result pills
    {
        if (! stageReady()) return;
        if (state_ == State::waitAudio || state_ == State::measuring) return;
        round_ = 0; proposals_ = 0; lastCommanded_ = 0.0f; pendingTrim_ = 0.0f; pendingKind_ = PendingKind::none; continueAfterGo_ = false; reportOnly_ = true;
        startWindow();
        emit ("Checking the level again - play the loudest part.", -1.0f, false, false, Bubble::Kind::info);
        if (! isTimerRunning()) startTimer (kTickMs);
    }
    bool undo()
    {
        if (! stageReady() || ! haveUndo_) return false;
        const float delta = preLoopGainDb_ - currentGainDb();
        writeGainDb (preLoopGainDb_);
        log ("undo: " + stageName() + " restored to " + fmtSigned (preLoopGainDb_) + " dB (delta " + fmtSigned (delta) + ")");
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
    /** Levelling v2: the stage's gain, whichever stage this rack uses. */
    float currentGainDb() const { return stage_.ready && stage_.ready() ? stage_.readDb() : 0.0f; }
    /** Is there a gain to drive at all? Replaces "is there an EchoJay Level slot in the chain". */
    bool  stageReady() const { return stage_.ready && stage_.ready(); }
    /** LEVELLING V2: THE TAP THE GR MODEL USED TO READ. It was the EchoJay Level slot's own OUT meter - the
        signal entering whatever came next. With no Level slot, the equivalent and better tap is the final
        limiter's own INPUT meter, which reads the GAINED input (L's hand-off), i.e. the signal actually arriving
        at the ceiling. Empty when there is no EchoJay Limiter last, which is every MATCH build - and a match
        build pushes into nothing, so there is no reduction to model. Every caller already handles an unknown
        reading; what they must not do is read a tap that is not there. */
    echojay::LevelTally::Snapshot stageTap() const
    {
        if (auto* lim = echoJayLimiter()) return lim->inputLevels();
        return {};
    }
    juce::String stageName() const { return stage_.name; }
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
    // Levelling v2: the Level slot's own OUT tap is gone, and with it this estimate's minuend. It returns NaN,
    // which every caller already handles (the cap falls back to the limiter's MEASURED gain reduction). That is
    // not a loss: a target build always ends in a limiter whose GR is readable, and a match build is not pushing
    // into anything, so there is nothing to estimate. Kept as a named seam rather than deleted so the callers
    // keep saying which figure they used.
    float grPeakEstimateDb() const { return std::numeric_limits<float>::quiet_NaN(); }
    // 21m item 3: the hits, block by block. typical = mean reduction (Level OUT TP - chain OUT TP) over the top 20 % of the last
    // 30 hops ranked by Level OUT true peak (the hits); worst = the largest single-block reduction; typicalLvTp = the mean Level
    // OUT true peak of those top blocks (what the cap projects the trim onto). NaN until both tallies carry hops.
    struct HitsMeasure { float typicalDb = std::numeric_limits<float>::quiet_NaN(), worstDb = std::numeric_limits<float>::quiet_NaN(), typicalLvTpDb = std::numeric_limits<float>::quiet_NaN(); int blocks = 0; };
    HitsMeasure hitsMeasure() const
    {
        HitsMeasure m; if (! stageReady()) return m;
        const auto a = stageTap(), b = host_.getChainOutLevels();
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
        if (! stageReady())
        {
            if (state_ != State::idle && state_ != State::hold)
            {
                state_ = State::hold; stopTimer(); levelPtr_ = nullptr;
                log ("stopped: the gain stage this loop drives (" + stageName() + ") is gone");
                emit ("The gain I was levelling with is no longer there - level loop stopped.",
                      -1.0f, false, true, Bubble::Kind::info);
            }
            return;
        }
        // Levelling v2: there is no Level CARD to carry a GR row. A target build always ends in a limiter, so the
        // REAL gain reduction is readable there and no model is needed; a match build pushes into nothing, so
        // there is no reduction to report. The estimate is kept for the proposal's own cap arithmetic below.
        // 08c F2: A LANDING IS ONLY TRUE OF THE CHAIN IT WAS MEASURED ON. If the chain's gain has moved since -
        // an edit, an Apply, a trim, Sean's two EQ bells on 8 Oct - the figure on the card describes a chain that
        // no longer exists, so say so once and offer to do it again rather than stand on it.
        if (landingIsStale() && ! staleSaid_)
        {
            staleSaid_ = true;
            log ("landing STALE: the chain's gain changed after the Level was set (" + aimWords()
                 + "), so the figure on the card is true of a chain that no longer exists");
            emit (aim_ == Aim::matchInput
                      ? juce::String ("The chain changed after I matched the level. Tap Listen and I'll match it again.")
                      : "The chain changed after I set the level. Tap Listen and I'll check it against " + aimWords() + ".",
                  -1.0f, false, false, Bubble::Kind::info, { "Listen" });
        }
        // 08c F2: the owed opening landing runs while ARMED - `armed` is exactly the state it has to fire in,
        // which is why it sits past the early return below. The stale check above it deliberately runs FIRST:
        // after the opening landing the loop is STILL armed, waiting for Listen, so a staleness check that only
        // ran in the measuring states could never see the case it exists for (Sean's two EQ bells at 20:05).
        if (state_ == State::armed) { tryOpeningLanding(); return; }
        if (state_ != State::waitAudio && state_ != State::measuring && state_ != State::tracking) return;   // proposed / hold / quietAsked: nothing runs on its own
        const auto out = host_.getChainOutLevels();
        const float counted = out.heardAboveSeconds;
        if (counted > lastCounted_ + 0.05f)
        {
            if (auto* lim = echoJayLimiter())
            { const float gr = -lim->gainReductionDb(); grMin_ = juce::jmin (grMin_, gr); grMax_ = juce::jmax (grMax_, gr); grSum_ += gr; ++grN_; }
            {   // 18g (item 4), levelling v2: the ESTIMATE, limiter IN minus chain OUT (short-term LUFS) over the window. On every
                // limiter (a guard compares it with the EchoJay Limiter's real GR); it is REPORTED only for a third-party one.
                const float lvOut = stageTap().shortTermDb, chOut = out.shortTermDb;
                if (std::isfinite (lvOut) && std::isfinite (chOut)) { const float e = lvOut - chOut; estSum_ += e; ++estN_; estMax_ = juce::jmax (estMax_, e); }
            }
            if (state_ == State::waitAudio)
            {
                state_ = State::measuring;
                firstAudioMs_ = nowMs();   // the deadline is extended ONCE, from here: 20 s of PLAYBACK
                log ("state -> measuring: first audio after " + juce::String ((int) ((firstAudioMs_ - passStartMs_) / 1000))
                     + " s of waiting; the resolve deadline now runs " + juce::String (kResolveMs / 1000) + " s from here");
            }
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
            // THE DEADLINE, FIRST. Not enough loud audio is the ordinary reason a window never finishes, and it
            // used to be the reason it waited for ever. Now it resolves and says which of the four things happened.
            if (pastResolveDeadline()) { resolveUnfinished (counted, out); return; }
            if (counted <= 0.0f && ! waitingSaid_ && nowMs() - passStartMs_ >= kWaitWallMs)
            { waitingSaid_ = true; emit ("Still waiting for audio - play the loudest part of the song and I'll measure it.", 0.0f, true, false, Bubble::Kind::progress); }
            else if (counted > 0.0f) emit (round_ == 0 && pendingKind_ == PendingKind::none ? "Listening..." : "Checking...", progress, true, false, Bubble::Kind::progress);
            return;
        }
        // A KNOB GESTURE MUST NOT BE ABLE TO BLOCK A MEASUREMENT FOR EVER. knobGestureEnded only decrements, so a
        // single missed mouse-up leaves the count at 1 for the rest of the session and this return fired on every
        // tick. Past the deadline the window resolves anyway, and the log says the gesture was open.
        if (knobGestureOpen())
        {
            if (! pastResolveDeadline()) return;
            if (! resolvedLate_) { resolvedLate_ = true; log ("resolving although a knob gesture is still open - past the "
                                   + juce::String (kResolveMs / 1000) + " s deadline, and a gesture never blocks a measurement"); }
        }
        // ---- THE TARGET IS INTEGRATED, NOT THE LOUDEST 3 SECONDS (7 Oct 2026 ruling, Sean 20:09) ----------
        // What was here: `measured = out.maxShortTermDb`, so every proposal made the LOUDEST 3 SECONDS equal the
        // target. A "-8 commercial" target means the song's INTEGRATED loudness is about -8, with the loud
        // sections sitting above it, so the loop was aiming low by the whole short-term-to-integrated distance -
        // about 4.5 dB on Sean's mix bus, which is exactly what his ear found (+8 was right, the loop proposed
        // +3.4). Both chain tallies are K-weighted, so levelDb IS integrated LUFS, and startWindow() resets the
        // out tally, so this is the integrated loudness OF THIS WINDOW at the chain output, post limiter.
        const float measured = out.levelDb;
        if (! out.known || ! std::isfinite (measured))
        {
            if (pastResolveDeadline()) { resolveUnfinished (counted, out); return; }
            return;
        }
        // 08c F2: IN MATCH MODE THE TARGET IS THIS WINDOW'S INPUT. Re-read here rather than only at arm, so a
        // build whose dials land after the arm - the 8 Oct fault, where a compressor took 2.4 dB after the opening
        // had been computed - is matched against what the input actually is NOW. Everything below is unchanged.
        if (aim_ == Aim::matchInput)
        {
            const auto inNow = host_.getChainInLoopLevels();
            if (inNow.known && std::isfinite (inNow.levelDb))
            {
                if (std::abs (inNow.levelDb - target_) > 0.05f)
                    log ("match: the aim is the chain input, re-read this window: " + fmt (target_) + " -> "
                         + fmt (inNow.levelDb) + " LUFS");
                target_ = inNow.levelDb;
            }
            else
            {
                log ("match: the chain INPUT has no reading this window, so there is nothing to match to - "
                     "holding the Level where it is");
                if (pastResolveDeadline()) { resolveUnfinished (counted, out); return; }
                return;
            }
        }
        // The loud window is kept as a SAFETY CHECK, as ruled - never as the target. It rides the log.
        if (std::isfinite (out.maxShortTermDb))
            log ("window: integrated " + fmt (measured) + " LUFS (the target), loudest 3 s "
                 + fmt (out.maxShortTermDb) + " LUFS (+" + fmt (out.maxShortTermDb - measured)
                 + " over it, the safety check), " + fmt (counted) + " s counted" + busGainNote());
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
        {   // 21t-m item 2 (29 Sep 2026 ruling): LISTEN MEASURES AND ASKS. IT WRITES NOTHING.
            // What was here: measureUnityTrims(), which wrote a pre-trim AND a post-trim on every slot but the
            // Level and the last limiter, and logged "match trims, compare-only: the chain output does NOT
            // move". Sean's 18:13:39.890 window wrote Saturate pre -4.8 dB and PuigTec pre -3.2 dB, and
            // setSlotPreTrimDb is IN - applied unconditionally by SlotPreTrim, in the audio path. The chain
            // output DID move, by those pre-gains, and he heard it drop before he had agreed to anything.
            // The rule is now: the only level write on a bus is the Level slot, after Go, by exactly the
            // proposed amount. A measurement that changes what it is measuring is not a measurement.
            trimDeltaDb_ = 0.0f;
            log ("listen: measured, nothing written - the only level write is the Level slot, after Go"); }
        {   // 21p item 2: the per-slot picture the Listen card shows, and the flags by name
            for (const auto& l : host_.listenCardLines()) log ("slot picture: " + l);
        }
        // 21p item 1: the loop's OWN reading goes through the same gate. A window that passed the count floor can
        // still be silence at the chain output (a muted send, a stopped transport between ticks): it is "no reading".
        {
            const auto g = echojay::ReadingGate::check (measured, out.truePeakDb, rollingOrUnknown(), out.heardSeconds);
            if (! g.valid)
            {
                log ("no reading: " + g.why + " - nothing applied");
                state_ = State::hold; stopTimer();
                emit ("No reading - " + g.why + ". Play the loudest part and tap Listen again.", -1.0f, false, false, Bubble::Kind::info, { "Listen" });
                return;
            }
        }
        // the loop's opening gain assumes a UNITY chain: the window measured the un-trimmed chain, so the figures it proposes from carry the trims just applied
        // R2 (21s-b, 24 Sep 2026): ONE FIGURE EVERYWHERE. The proposal used to be computed from the window PLUS the
        // trim change (measured + trimDeltaDb_), while Done reported the raw window - so one Listen printed -11.3
        // at the proposal and -14.9 at Done, 3.6 dB apart, and it read as the level having dropped after a push.
        // Now the match trims are compare-only (they are not in the path during a measurement), so the window IS
        // the chain output and there is nothing to correct for.
        proposeFrom (measured, out.truePeakDb);
    }
    // 18f: the decision after a measurement (also reached from loudestPart()) - proposal / on-target / stuck / at the limit
    void proposeFrom (float measured, float truePeakDb)
    {
        if (! stageReady()) return;
        const float needed = target_ - measured;
        const float cur = currentGainDb();
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
        bool capped = false; const float cap = grCapDb (loudnessOption_); const auto hm = hitsMeasure(); const float worstTP = stageTap().truePeakDb;
        const float lvTP = std::isfinite (hm.typicalLvTpDb) ? hm.typicalLvTpDb : worstTP;
        // the base the trim is projected onto: the MEASURED typical reduction when the hits are already being limited (a clipper's
        // intersample overshoot makes "true peak - ceiling" read high), else the typical hit's distance to the ceiling
        // THE BASE THE CAP PROJECTS ONTO (7 Oct 2026 ruling). Three tiers, measurement first:
        //  1. hm.typicalDb - the MEASURED reduction (Level OUT true peak minus chain OUT true peak over the top
        //     20 % of blocks). Real for any limiter, third-party included, once it is actually working.
        //  2. the EchoJay Limiter's OWN GR reading, when the measure above has not filled yet.
        //  3. only then an estimate, and a scaled, clamped one: 0.25 x the excess over the ceiling, never more
        //     than 3 dB. The old fallback was the raw excess, which predicted 6.3 dB against a real 1.3-1.7 on
        //     Sean's bx_limiter and is what held every master down.
        const float excess = juce::jmax (0.0f, lvTP - ceilingDb_);
        float base; const char* baseFrom;
        if (std::isfinite (hm.typicalDb) && hm.typicalDb > 0.5f)                { base = hm.typicalDb;                        baseFrom = "measured (typical over the hits)"; }
        else if (! grIsEstimated() && std::isfinite (grAvg()) && grAvg() > 0.5f) { base = grAvg();                             baseFrom = "measured (the EchoJay Limiter's own GR)"; }
        else                                                                     { base = juce::jmin (kThirdPartyGrMaxDb, kThirdPartyGrFactor * excess); baseFrom = "ESTIMATED (0.25 x excess, clamped at 3 dB)"; }
        log ("GR base " + fmt (base) + " dB from " + juce::String (baseFrom) + " - excess over the ceiling "
             + fmt (excess) + " dB, cap " + fmt (cap) + " dB (" + loudnessOption_ + "); measured typical "
             + fmt (hm.typicalDb) + ", worst " + fmt (hm.worstDb) + ", limiter GR avg " + fmt (grAvg())
             + " max " + fmt (grMax()) + " (this line is the calibration record: estimate vs measured, every window)");
        if (trim > 0.0f && lvTP > -150.0f && std::isfinite (ceilingDb_))
        {
            const float predictedHits = juce::jmax (0.0f, base + trim);
            if (predictedHits > cap + 0.05f) { const float capTrim = juce::jmax (0.0f, cap - base); log ("GR cap: typical hit true peak " + fmt (lvTP) + " dBTP (top 20 % of " + juce::String (hm.blocks) + " blocks; worst peak " + fmt (worstTP) + " dBTP, worst reduction " + fmt (hm.worstDb) + " dB) + trim " + fmtSigned (trim) + " - ceiling " + fmt (ceilingDb_) + " = " + fmt (predictedHits) + " dB typical on the hits > cap " + fmt (cap) + " (" + loudnessOption_ + ") -> trim " + fmtSigned (capTrim)); trim = capTrim; capped = true; }
        }
        bool atCeiling = false;
        if (cur + trim > kLevelMaxDb) { trim = kLevelMaxDb - cur; atCeiling = true; }
        if (cur + trim < -kLevelMaxDb) { trim = -kLevelMaxDb - cur; atCeiling = true; }
        log ("measured: INTEGRATED " + fmt (measured) + " LUFS at the chain output (input window max short-term " + fmt (lastInputWindow_) + "), target " + fmt (target_) + ", needed " + fmtSigned (needed) + " dB, Level " + fmtSigned (cur) + " dB, trim " + fmtSigned (trim) + (atCeiling ? " (Level ceiling)" : "") + ", limiter GR avg " + fmt (grAvg()) + " max " + fmt (grMax()) + " dB, round " + juce::String (round_));
        const juce::String grText = grText_();
        // 22 Sep 2026 (ruling 4): the true-peak values THEMSELVES, not only their difference
        { const auto hm2 = hitsMeasure(); log ("hits: typical " + fmt (hm2.typicalDb) + " dB over the top 20 % of " + juce::String (hm2.blocks) + " blocks, worst " + fmt (hm2.worstDb) + " dB"); }   // 21m item 3: both figures, for the re-calibration
        log ("true peak: limiter IN TP " + fmt (stageTap().truePeakDb) + " dBTP, chain OUT TP " + fmt (host_.getChainOutLevels().truePeakDb) + " dBTP, hits " + fmt (juce::jmax (0.0f, grPeakEstimateDb())) + " dB" + (grIsEstimated() ? " (third-party limiter: the hits figure is the report)" : " (EchoJay Limiter: its own GR reading " + fmt (grMax()) + " dB is the report)"));
        if (capped)
        {
            state_ = State::proposed; pendingTrim_ = trim; pendingKind_ = PendingKind::propose; ++proposals_;
            const float cappedLevel = measured + trim;
            juce::StringArray pills { "Push it anyway", "Leave it" }; if (applied_) pills.add ("Undo");
            emit ("Measured " + fmt (measured) + " LUFS integrated. " + fmt (cappedLevel) + " is as loud as this goes with the limiter working <=" + juce::String ((int) std::round (cap)) + " dB. Push to " + fmt (target_) + " anyway? " + grText, -1.0f, false, false, Bubble::Kind::proposal, pills);
            return;
        }
        if (reportOnly_)
        {   // 18h (item 4): a Check REPORTS - on target, or the distance - with the result pills (Push it only when short), then watches
            reportOnly_ = false; state_ = State::tracking; startTracking();
            const bool on = std::abs (needed) <= kCloseEnoughDb;
            emit ("Hitting " + fmt (measured) + " LUFS integrated, target " + fmt (target_) + (on ? " - on target." : " - " + fmt (std::abs (needed)) + " dB " + (needed > 0 ? "under." : "over.")) + " Peaks " + fmt (truePeakDb) + " dBTP, " + grText, -1.0f, false, true, Bubble::Kind::result, pillsFor (needed));
            return;
        }
        if (std::abs (needed) <= kCloseEnoughDb)
        {
            state_ = State::tracking; startTracking();
            emit ("Hitting " + fmt (measured) + " LUFS integrated, target " + fmt (target_) + " - on target. Peaks " + fmt (truePeakDb) + " dBTP, " + grText + " I'll keep watching for a louder section.", -1.0f, false, true, Bubble::Kind::result, resultPills());
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
        emit ("Measured " + fmt (measured) + " LUFS integrated. Push " + fmtSigned (trim) + " dB to reach " + fmt (target_) + "?" + (atCeiling ? " (that is the Level slot's limit)" : "") + " " + grText, -1.0f, false, false, Bubble::Kind::proposal, applied_ ? proposalAfterApplyPills() : proposalPills());
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
    // ---- THE RESOLVE DEADLINE (7 Oct 2026 ruling) -------------------------------------------------------
    // 20 s from the Listen tap, extended ONCE to 20 s after the first counted audio, so "20 s" means 20 s of
    // playback and not 20 s of the user cueing the chorus.
    /** 08c F2: a landing is only true of the chain it was measured on. Recorded with the chain's VALUE revision -
        the counter that moves on every gain write, trim, wet and loop move (ChainHost: bumpChainValue) - because
        that is exactly "has the chain's gain changed since". A structural edit bumps it too, which is correct:
        adding a plugin changes the level as surely as turning one up. */
    void noteLanded()
    {
        landedOnce_ = true; staleSaid_ = false;
        landedAtValueRev_ = host_.getChainValueRevision();
    }
    /** True when a landing was made and the chain's gain has moved since. Sean's two EQ bells at 20:05 on 8 Oct
        are the case: the figure was true of the chain as it stood, and then the chain changed under it. */
    bool landingIsStale() const
    {
        return landedOnce_ && landedAtValueRev_ >= 0
            && host_.getChainValueRevision() != landedAtValueRev_;
    }
    /** 9 Oct 2026: the Level slot's params as they stand, as a NEW object we may add to. Every write to this
        slot goes through here so no writer can drop a field another writer owns. */
    juce::DynamicObject* levelParamsCopy() const
    {
        if (slot_ >= 0)
        {
            const auto st = host_.getSlotStructured (slot_);
            if (st.getDynamicObject() != nullptr)
                if (auto* po = st.getProperty ("params", juce::var()).getDynamicObject())
                    return new juce::DynamicObject (*po);
        }
        return new juce::DynamicObject();
    }
    /** LEVELLING V2: the record, at rack level. `landedDb < 0 && !landed` writes the aim before any landing;
        after a landing it carries the gain that was set. One writer, so the card, the save and a reopen all read
        the same thing. */
    void writeRecord (float landedDb, bool landed = false)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("option", loudnessOption_);
        if (aim_ == Aim::hitTarget && std::isfinite (target_)) o->setProperty ("target_lufs", (double) target_);
        o->setProperty ("stage", stage_.name);
        o->setProperty ("aim_words", aimWords());
        if (landed) o->setProperty ("landed_db", (double) landedDb);
        host_.setLevellingRecord (juce::var (o));
    }

    /** LEVELLING V2 MIGRATION: an EchoJay Level slot from a project or a saved chain written before 10 Oct. Its
        gain moves to the stage and the slot is removed. The log carries the chain output BEFORE and AFTER,
        because "no audible change" is true for a Level that was already last and FALSE for one that sat before a
        non-linear slot - Sean's sat before Vocal Reverb - and claiming otherwise would be the lie. Returns the
        dB moved, or NaN when there was nothing to migrate. */
    float migrateLevelSlotIfPresent()
    {
        int at = -1;
        for (int i = host_.getNumSlots() - 1; i >= 0; --i)
            if (host_.getSlotInfo (i).name == "EchoJay Level") { at = i; break; }
        if (at < 0) return std::numeric_limits<float>::quiet_NaN();
        float gain = 0.0f;
        if (auto* lv = dynamic_cast<EedLevelProcessor*> (host_.getSlotProcessor (at)))
            gain = currentGainDb();
        const int n = host_.getNumSlots();
        const bool wasLast = (at == n - 1);
        const auto before = host_.getChainOutLevels();
        if (stageReady()) stage_.writeDb (currentGainDb() + gain);
        host_.removeSlot (at);
        log ("migrated the EchoJay Level slot away: " + fmtSigned (gain) + " dB moved from slot "
             + juce::String (at + 1) + " of " + juce::String (n) + " to " + stage_.name
             + (wasLast ? " - it was already LAST, so this is level-identical"
                        : " - it was NOT last, so the sound DOES change here: a gain ahead of a non-linear slot "
                          "was in the wrong place, which is why this redesign exists")
             + ". Chain output before: "
             + (before.known ? fmt (before.levelDb) + " LUFS" : juce::String ("not yet measured")));
        return gain;
    }

    /** 08c F2: THE OWED OPENING LANDING. Run from the tick while the loop is armed, so it fires as soon as this
        chain - the settled one - has been heard at both taps. Match mode sets the Level so out = in; target mode
        so out = the target. Either way it is one write, from one window, and `noteLanded` stamps the chain it was
        true of. */
    void tryOpeningLanding()
    {
        if (! openingOwed_) return;
        if (! stageReady()) return;
        const auto outNow = host_.getChainOutLevels();
        const auto inNow  = host_.getChainInLoopLevels();   // the loop's window, not the song's reading
        const bool needIn = (aim_ == Aim::matchInput);
        // THE RESET IS DEFERRED TO THE AUDIO THREAD (LevelTally::reset only raises a flag; the clear and the
        // publish happen in push()). So `known` can still be TRUE on the OLD window for as long as no audio has
        // arrived - and reading it then would land on exactly the stale integrated figure this whole item exists
        // to stop. The reset is therefore OBSERVED rather than assumed: heardSeconds has to be seen near zero
        // once before any reading is believed. With no audio playing that never happens, which is correct - there
        // is nothing to measure, and the arm bubble is already asking for playback.
        if (! openingResetSeen_)
        {
            const bool outCleared = outNow.heardSeconds < 1.0f;
            const bool inCleared  = ! needIn || inNow.heardSeconds < 1.0f;
            if (outCleared && inCleared)
            {
                openingResetSeen_ = true;
                log ("opening gain: the chain tallies have cleared, so the next reading is of THIS chain");
            }
            return;
        }
        if (! (outNow.known && std::isfinite (outNow.levelDb))) return;
        if (needIn && ! (inNow.known && std::isfinite (inNow.levelDb))) return;

        const float cur = currentGainDb();
        if (needIn) target_ = inNow.levelDb;            // the aim, measured
        const float want = juce::jlimit (-kLevelMaxDb, kLevelMaxDb, cur + (target_ - outNow.levelDb));
        if (needIn)
            log ("opening gain (VOLUME MATCH): chain in " + fmt (inNow.levelDb) + " LUFS, out "
                 + fmt (outNow.levelDb) + " LUFS over the same window -> Level " + fmtSigned (cur) + " -> "
                 + fmtSigned (want) + " dB, so the chain does not change the level" + busGainNote());
        else
            log ("opening gain (closed loop, fresh window): output integrated " + fmt (outNow.levelDb)
                 + " LUFS at Level " + fmtSigned (cur) + " dB -> " + fmtSigned (want) + " dB for "
                 + aimWords() + ". No peak-headroom cap (7 Oct ruling): the ceiling and the loud-window GR "
                 "check are the safety." + busGainNote());
        openingOwed_ = false;
        if (std::abs (want - cur) >= 0.05f) writeGainDb (want);
        else noteLanded();   // nothing to write, but this chain HAS now been landed on
    }
    bool pastResolveDeadline() const
    {
        const juce::int64 from = firstAudioMs_ > 0 ? firstAudioMs_ : passStartMs_;
        return from > 0 && nowMs() - from >= (juce::int64) kResolveMs;
    }
    // EchoJay's own bus trim, named wherever a figure is reported, because the loop measures PRE bus gain and the
    // user reads POST (item 2c). Silent at 0.0 dB - there is nothing to say then.
    juce::String busGainNote() const
    {
        const float b = busGainDb ? busGainDb() : 0.0f;
        if (! std::isfinite (b) || std::abs (b) < 0.05f) return {};
        return ", and EchoJay's bus trim is " + fmtSigned (b) + " dB, so the meters read that much "
               + juce::String (b > 0.0f ? "above" : "below") + " these figures";
    }
    // "NO SIGNAL" IS DECIDED BY THE READING, NOT BY THE COUNTER. A chain does not fall silent the instant the
    // transport stops: the Level slot's gain smoother and a limiter's release keep a few hops above the counting
    // floor, so a genuinely silent 20 s window lands with counted at 0.2 s rather than exactly 0 - and
    // "I heard 0 of the 10 seconds I need" is the wrong thing to tell someone whose cable is unplugged. Under a
    // second of counted audio AND a current reading below the floor is no signal; anything else is a short window.
    bool noSignalNow (const echojay::LevelTally::Snapshot& out, float counted) const
    {
        if (counted > 1.0f) return false;
        const float now = std::isfinite (out.shortTermDb) ? out.shortTermDb
                        : (std::isfinite (out.rmsDb) ? out.rmsDb : -200.0f);
        return now <= kCountFloorLufs;
    }
    // THE ONE PLACE A WINDOW CAN END WITHOUT A MEASUREMENT, and it always names which of the four things happened.
    // Before this existed the loop simply returned from the tick and waited for ever: Sean tapped Listen twice,
    // got "Already listening", and then silence (20:00, 7 Oct).
    void resolveUnfinished (float counted, const echojay::LevelTally::Snapshot& out)
    {
        const bool stopped = transportKnown && transportKnown() && isPlaying && ! isPlaying();
        juce::String why, said;
        if (stopped)
        {
            why  = "the transport is stopped";
            said = "The transport is stopped, so there was nothing to measure. Start playback on the loudest part and tap Listen.";
        }
        else if (noSignalNow (out, counted))
        {
            // THE FIGURE, not a bare "no signal": his meters read RMS -61.8 and Momentary "--" at that moment, and
            // that reading IS the evidence for what to do next.
            // The figure the user can see on the meters: short-term when a 3 s window has closed, else the
            // decayed RMS, which is what read -61.8 dB on Sean's screenshot. Never a bare "no signal".
            const juce::String lvl = std::isfinite (out.shortTermDb) && out.shortTermDb > -150.0f
                                       ? fmt (out.shortTermDb) + " LUFS short-term"
                                       : (std::isfinite (out.rmsDb) && out.rmsDb > -150.0f
                                              ? fmt (out.rmsDb) + " dB RMS"
                                              : juce::String ("nothing above the noise floor"));
            why  = "no signal reached the plugin (" + lvl + ", nothing above the " + fmt (kCountFloorLufs) + " LUFS counting floor)";
            said = "No signal is reaching EchoJay - the chain output reads " + lvl + ". Check the track is playing and "
                   "routed through this plugin, then tap Listen.";
        }
        else if (counted < kNeedSeconds)
        {
            why  = "only " + fmt (counted) + " s of the " + juce::String ((int) kNeedSeconds) + " s needed was loud enough";
            said = "I heard " + juce::String ((int) std::round (counted)) + " of the " + juce::String ((int) kNeedSeconds)
                 + " seconds I need. Play a longer stretch of the loudest part and tap Listen.";
        }
        else
        {
            // Counted enough and still no figure: that is a SENSOR fault, not a user problem, and it is logged as one.
            why  = "the chain-output reading never became usable (known=" + juce::String (out.known ? "y" : "n")
                 + ", integrated " + fmt (out.levelDb) + ") - THIS IS A SENSOR FAULT, not something the user did";
            said = "I counted enough audio but could not get a reading from the chain output. That is a fault on my "
                   "side - tap Listen to try again.";
        }
        state_ = State::hold; stopTimer();
        log ("state -> hold: resolved at the " + juce::String (kResolveMs / 1000) + " s deadline without a measurement - "
             + why + "; counted " + fmt (counted) + " s" + busGainNote());
        emit (said, -1.0f, false, false, Bubble::Kind::info, { "Listen" });
    }
    void startWindow()
    {
        host_.setChainOutCountFloor (kCountFloorLufs);
        host_.resetChainOutLevels();
        // 08c F2: the loop's INPUT window is restarted with the output one, so a match compares the two over the
        // same span. The song's integrated reading (getChainInLevels) is untouched, as ruled.
        host_.resetChainInLoopLevels();
        // ---- 08c F2 (9 Oct, final): A WINDOW SUPERSEDES THE OWED OPENING, and this is the ruling it serves.
        // I tried the other way - a window that completes while the opening is owed TAKES it, one uncapped write,
        // then restarts - because it stops a quick Listen landing short by the per-pass clamp. The gate showed
        // what it costs: twelve legs across two guards went red, and the honest reading of them is not that they
        // are stale. It is that the user taps Listen, sees "Listening...", and ten seconds later the Level jumps
        // nine dB with no question asked. The opening at the ARM is invisible because a build is visibly setting
        // levels; mid-Listen it is a surprise, and "ask before applying" (18g) is a ruling about exactly that.
        //
        // So the opening fires ONLY while armed, from the arm's own fresh window, which is the audio the user
        // plays while cueing - what the arm bubble asks for in so many words ("Cue the loudest section, press
        // play, then tap Listen"). A user who taps Listen with nothing played gets the ordinary loop: measure,
        // propose, Go. That can take two passes, and two honest passes beat one silent jump.
        openingOwed_ = false;
        host_.resetChainOutShortTermMax(); host_.resetChainInShortTermMax();
        if (auto* lim = echoJayLimiter()) lim->resetOutputPeak();
        // Levelling v2: no Level slot, so no Level meters to reset. The chain IN/OUT tallies reset above ARE the
        // window, and for a target build the limiter's own peak hold is reset on the line above this one.
        lastCounted_ = 0.0f; waitingSaid_ = false; passStartMs_ = nowMs();
        firstAudioMs_ = 0; resolvedLate_ = false;   // the kResolveMs clocks (7 Oct ruling: Listen always resolves)
        log ("state -> listening: window open, needs " + juce::String ((int) kNeedSeconds) + " s above "
             + fmt (kCountFloorLufs) + " LUFS momentary, resolves within " + juce::String (kResolveMs / 1000)
             + " s of playback either way");
        grMin_ = std::numeric_limits<float>::max(); grMax_ = 0.0f; grSum_ = 0.0f; grN_ = 0;
        estSum_ = 0.0f; estN_ = 0; estMax_ = 0.0f;
        state_ = State::waitAudio;
    }
    // THE OPEN PROPOSAL, SHOWN AGAIN. Composed from the SAME state the original was (pendingTrim_, lastMeasured_,
    // target_), so the card cannot drift from the thing Go will apply - a second card with a different number on
    // it would be worse than no card.
    void reShowProposal()
    {
        if (! stageReady()) return;
        const juce::String grText = grText_();
        emit ("Still waiting on this: measured " + fmt (lastMeasured_) + " LUFS integrated. Push "
              + fmtSigned (pendingTrim_) + " dB to reach " + fmt (target_) + "? " + grText,
              -1.0f, false, false, Bubble::Kind::proposal, applied_ ? proposalAfterApplyPills() : proposalPills());
    }
    void startTracking()
    {
        host_.resetChainOutLevels(); host_.resetChainOutShortTermMax(); lastCounted_ = 0.0f;
        if (! isTimerRunning() && juce::MessageManager::getInstanceWithoutCreating() != nullptr) startTimer (kTickMs);
    }
    void applyTrim (float trim, const char* why)
    {
        if (! stageReady()) return;
        lastCommanded_ = trim; prevMeasured_ = lastMeasured_;   // 18g (item 2): the next proposal scales its step by achieved/commanded
        const float newDb = juce::jlimit (-kLevelMaxDb, kLevelMaxDb, currentGainDb() + trim);
        appliedDelta_ = newDb - currentGainDb();   // 21 Sep: the delta the after-verb bubble reports
        log (juce::String (why) + ": Level " + fmtSigned (currentGainDb()) + " -> " + fmtSigned (newDb) + " dB (trim " + fmtSigned (trim) + ")");
        writeGainDb (newDb);
    }
public:
    void writeGainDb (float db)   // 21n item 3: public - the undo dispatcher restores a loop entry through the same write
    {   // through the host so the card and the dial info follow
        const float before = currentGainDb();
        if (onGainWritten && std::abs (before - db) > 0.01f) onGainWritten (before, db);
        // 9 Oct 2026 (CONTRACT_LEVEL_PARAMS): MERGE, NEVER REPLACE. This built a fresh params object holding
        // gain_db alone, so the first landing DELETED `option` and `target_lufs` from the slot. The target
        // survived only because the device remembers it; `option` is a server field with no device param behind
        // it, so it was simply gone - and then every later findTarget read "absent" and the aim fell back to
        // whatever the ChannelType said, whatever the server had asked for.
        // LEVELLING V2: the write goes to the STAGE - the rack's OUT gain, or the final limiter's input_db -
        // and never to a Level slot, because there is not one. The stage is the processor's to resolve, so this
        // function no longer knows or cares which rack or which process owns it.
        if (! stageReady()) { log ("no gain stage to write " + fmtSigned (db) + " dB to"); return; }
        stage_.writeDb (db);
        // 8 Oct 2026: AND IT HAS TO SURVIVE A RELOAD. The host saves the state CACHE, and nothing captured after
        // a loop write - so Sean's landing of +8.1 dB held in the audio, was confirmed in the log, and came back
        // at 0.0 after the host relaunched. The Level is a built-in: its capture is a small JSON, so every write
        // takes it rather than us deciding which writes "count".
        // LEVELLING V2: the record carries the landed gain, at rack level. The old per-slot state capture is
        // gone with the slot; the stage's own control is persisted by whoever owns it (a Link's gainDb in its
        // state, the limiter's input_db in the slot's params), which is what makes the landing survive a reopen.
        writeRecord (db, true);
        // 08c F2: ONE AUTHOR FOR THE STAMP. Every gain the loop writes - the opening, an applied proposal, a
        // tracking nudge, an undo restore - re-records the chain it was true of. Stamping only at the opening
        // would make the loop's OWN next write look like somebody else's edit and fire the stale offer at the
        // user immediately after they pressed Go.
        noteLanded();
    }
    void emit (const juce::String& text, float progress, bool replace, bool final, Bubble::Kind kind = Bubble::Kind::info, juce::StringArray pills = {})
    {
        lastBubble_ = text; lastKind_ = kind; lastPills_ = pills;
        if (kind != Bubble::Kind::progress) ++bubbleCount_;
        log ("bubble: " + text + (pills.isEmpty() ? juce::String() : " [" + pills.joinIntoString (" | ") + "]"));
        if (onBubble) { Bubble b; b.text = text; b.progress = progress; b.replace = replace; b.final = final; b.kind = kind; b.pills = pills; onBubble (b); }
    }
    /// 21p item 1: rolling, or a host that has never said. Only a KNOWN-stopped transport fails the gate.
    bool rollingOrUnknown() const
    {
        const bool known = transportKnown ? transportKnown() : false;
        if (! known) return true;
        return isPlaying ? isPlaying() : true;
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
    // 08c F2: MATCH MODE IS A TARGET THAT IS RE-READ EVERY WINDOW. Rather than a second decision path beside
    // the whole proposal/cap/tracking machine, the aim changes where `target_` COMES FROM: in match mode it is
    // the chain INPUT's integrated loudness for this window, so "out = in" is just "hit the target" with the
    // target measured instead of given. Every mechanism below - the step scaling, the GR cap, the tracking
    // back-off, the bubbles - works unchanged, which is the only reason this is a safe change to make at once.
    enum class Aim { matchInput, hitTarget };
    Aim    aim_ = Aim::hitTarget;
    bool   openingOwed_ = false;       // 08c F2: the opening landing is measured, not written at arm
    bool   openingResetSeen_ = false;  // 08c F2: the deferred tally reset has been OBSERVED, not assumed
    Stage  stage_;                     // levelling v2: the gain this loop drives (rack OUT or limiter IN)
    bool   staleSaid_ = false;         // 08c F2: the stale-landing offer is made once per landing
    bool   landedOnce_ = false;        // a landing has been written: a later chain-gain change makes it stale
    int    landedAtValueRev_ = -1;     // the chain's VALUE revision when we landed
    float lastCounted_ = 0.0f; bool waitingSaid_ = false; juce::int64 passStartMs_ = 0;
    juce::int64 firstAudioMs_ = 0;   // 7 Oct 2026: the deadline's second clock - 0 until audio is counted
    bool resolvedLate_ = false;      // the "resolved with a gesture open" line is said once per window
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
