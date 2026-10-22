#pragma once
// =============================================================================
//  EJAgentTools — the EXECUTOR INTERFACE the agent client drives.
//
//  The client (EJAgentClient) owns the loop: it talks to B's /api/agent/start
//  and /step, decides when a tool call may run (approval, order, stop), and
//  posts compact results. It never touches a rack, a meter or a Link itself.
//  Everything that does is behind this interface, which Session A implements
//  over ChainHost / the Link transport / the level tallies in its own file
//  (the plan's "plugin executes" half, AGENT_MODE_PLAN.md 1.1 and 7).
//
//  Threading: every call is made on the MESSAGE THREAD and completes through
//  `done` on the message thread, synchronously or later. The client tags each
//  call with a generation and ignores a completion from a stopped session, so
//  an implementation may complete after stop() without checking anything.
//
//  Results are COMPACT (plan section 2): a look(analysis) is ~250 tokens, a
//  look(rack) one line per slot, a check(level) three numbers. The executor
//  defines the summary; the client sends it verbatim.
//
//  wait_for_playback is NOT delegated: the client runs it (the UX lines, the
//  timeout, the early stop) and reads the executor's PlaybackReading on a
//  timer. The two hooks it needs are beginPlaybackWindow() and readPlayback().
//
//  Undo: a `do` that landed returns an undoToken. The client offers Undo per
//  step (undoStep) and "undo all" back to the checkpoint it captured at the
//  session's start (captureCheckpoint / undoToCheckpoint). The tokens are
//  opaque to the client; A's implementation keys them on the plugin-wide
//  UndoHistory (EchoJayUndoHistory.h: a CHAIN entry per applied line is the
//  design in LINK_REMOTE_CONTROL_PLAN.md section 9).
// =============================================================================
#include <juce_core/juce_core.h>
#include <functional>
#include <limits>
#include "EJAgentProtocol.h"

namespace echojay::agent
{

struct ToolOutcome
{
    bool         ok = true;
    juce::var    result;                 // compact, goes to the server as `result`
    juce::String errorCode, errorMessage; // when ! ok: goes to the server as `error`
    juce::String undoToken;              // a `do` that landed and can be undone on its own; "" otherwise
    juce::String landedLine;             // optional: what the checklist row says once done ("EQ: bell 300 Hz -2 dB")

    /** ---- ITEM 3 (Sean, 13 Oct 2026): TOOL ERRORS ARE NOT USER TEXT -------------------------------------
        Sean saw, in red, in his chat: "repeat this same call with confirm: true", followed by a dump of
        every channel and uid on the session. Both are mine, both are CORRECT - they are instructions to a
        model that has to decide what to do next, and item 6(d) added the channel list on purpose so a loop
        would stop. The mistake was that one string had two audiences and I only designed for one of them.

        So `errorMessage` stays exactly as it is - the model's text, as detailed as it needs to be - and
        `userMessage` is the one plain line a person reads. Empty means "the first sentence of errorMessage
        will do", which is true of most failures and keeps every existing call site honest rather than
        silently showing a human a model's instructions. */
    juce::String userMessage;

    /** The one line to show a person. Never the imperative clauses, never a list of uids. */
    juce::String forTheUser() const
    {
        if (userMessage.isNotEmpty()) return userMessage;
        // The first sentence, and nothing after it: every multi-audience message this build produces puts
        // the plain statement first and the model's instructions after, so this is a safe default rather
        // than a guess at parsing. Capped, so a single long sentence cannot become a wall either.
        auto first = errorMessage.upToFirstOccurrenceOf (". ", false, false).trim();
        if (first.isEmpty()) first = errorCode.isNotEmpty() ? errorCode : juce::String ("that step did not work");
        if (first.length() > 160) first = first.substring (0, 157).trim() + "...";
        return first.endsWithChar ('.') || first.endsWithChar ('?') ? first : first + ".";
    }

    /** ---- SEAN'S #10 (16 Oct 2026): THE SAME SPLIT ON THE SUCCESS PATH --------------------------
        He read this, in a step's details, in Logic: "Report it as ADDED, with its settings not
        confirmed. Do NOT call the add itself 'not applied', and do NOT state any figure". Item 3
        gave FAILURES two audiences (errorMessage / userMessage) and the fault came back through the
        door it did not cover: `landedLine` is one string read by the model AND painted on the row.

        The clean half is the sentences addressed to a person; the model's half is every sentence
        addressed to IT. The split is named here, in one place, so a call site cannot write a
        two-audience string without a filter standing behind it (userHalfOfLanded below).            */
    juce::String landedForUser;          // optional: the row's words when landedLine also talks to the model

    /** The landed line a person reads. Explicit when the call site wrote one; otherwise every
        sentence addressed to the model is dropped and the log says it happened. */
    juce::String landedForTheUser() const
    {
        if (landedForUser.isNotEmpty()) return landedForUser;
        return userHalfOfLanded (landedLine);
    }

    /** Drops the sentences written FOR THE MODEL out of a landed line. The markers are the
        imperative openings our own strings use - "Report ...", "Do NOT ...", "do not name ..." -
        and the test is deliberately narrow: a sentence is only dropped when it OPENS with one, so a
        figure or a plugin name can never be filtered away by accident. Public and static so the
        guard can hold it against both directions (a model sentence dropped, a user sentence kept). */
    static juce::String userHalfOfLanded (const juce::String& line)
    {
        if (line.isEmpty()) return line;
        static const char* modelOpeners[] = {
            "report ", "do not ", "do nots", "don't ", "never ", "say ", "tell the user", "call ",
            "repeat ", "use the ", "you were not given", "and do not ", "and do nots", "do NOT "
        };
        juce::StringArray kept;
        // Split on sentence ends only; a decimal point ("-14.00") is not a sentence end because the
        // next character is a digit. Walked rather than tokenised for exactly that reason.
        int start = 0;
        const int n = line.length();
        for (int i = 0; i < n; ++i)
        {
            const auto c = line[i];
            const bool end = (c == '.' || c == '!' || c == '?')
                          && (i + 1 >= n || line[i + 1] == ' ' || line[i + 1] == '\n');
            if (! end && i + 1 < n) continue;
            auto sentence = line.substring (start, juce::jmin (n, i + 1)).trim();
            start = i + 1;
            if (sentence.isEmpty()) continue;
            const auto low = sentence.toLowerCase();
            bool forModel = false;
            for (const char* m : modelOpeners)
                if (low.startsWith (juce::String (m).toLowerCase())) { forModel = true; break; }
            if (forModel) continue;
            kept.add (sentence);
        }
        auto out = kept.joinIntoString (" ").trim();
        return out.isNotEmpty() ? out : line;    // never blank a row: if every sentence looked like
                                                 // the model's, the line was not ours to split.
    }

    static ToolOutcome success (juce::var result, const juce::String& undoToken = {}, const juce::String& landedLine = {})
    { ToolOutcome o; o.ok = true; o.result = std::move (result); o.undoToken = undoToken; o.landedLine = landedLine; return o; }
    /** A success with both audiences written for deliberately: `landedLine` to the model, `userLine`
        to the person. The pair `failureFor` already is, for the other half of a step's life. */
    static ToolOutcome successFor (juce::var result, const juce::String& undoToken,
                                   const juce::String& landedLine, const juce::String& userLine)
    { ToolOutcome o; o.ok = true; o.result = std::move (result); o.undoToken = undoToken;
      o.landedLine = landedLine; o.landedForUser = userLine; return o; }
    static ToolOutcome failure (const juce::String& code, const juce::String& message)
    { ToolOutcome o; o.ok = false; o.errorCode = code; o.errorMessage = message; return o; }
    /** A failure with both audiences written for deliberately: `message` to the model, `userLine` to the
        person. Used wherever the model's text carries an instruction or a list. */
    static ToolOutcome failureFor (const juce::String& code, const juce::String& message,
                                   const juce::String& userLine)
    { ToolOutcome o; o.ok = false; o.errorCode = code; o.errorMessage = message; o.userMessage = userLine; return o; }
    /** A failure that carries STRUCTURE for the server to act on (17 Oct 2026, D22).

        A plugin that failed to load before is neither applied nor refused: the step comes back not
        applied with the plain sentence, and the result object carries the material the question
        needs - `tryAnywayOffer`, the raw reason for Details, and the loadable alternatives of the
        same role - so the ask can be drawn with [Try anyway] beside them. A bare `failure` would
        have left the model to invent the options. */
    static ToolOutcome failureWithResult (const juce::String& code, const juce::String& message,
                                          juce::var result)
    { ToolOutcome o; o.ok = false; o.errorCode = code; o.errorMessage = message;
      o.userMessage = message; o.result = std::move (result); return o; }
};

struct PlaybackReading
{
    bool  transportKnown = false;    // the host publishes a transport at all (LoudnessLoop's rule: unknown never blocks)
    bool  playing        = false;    // the transport says it is running (meaningful only when transportKnown)
    float heardAboveSeconds = 0.0f;  // gated audio counted SINCE beginPlaybackWindow()
    float integratedLufs    = std::numeric_limits<float>::quiet_NaN();
    float loudestShortTermLufs = std::numeric_limits<float>::quiet_NaN();
    float truePeakDbtp      = std::numeric_limits<float>::quiet_NaN();
    // ---- WHICH WINDOW EACH FIGURE IS OF (19 Oct 2026, Sean at 12:33) ---------------------------
    // *"check playback still says loudest 3 s -17.5 LUFS under integrated -14.1. If integrated is
    // measured over a longer window than the last 8 s, put both on the same window, or name each
    // window in the payload so B can label it truthfully."*
    //
    // On the open rack both come from one tally reset at the same instant, so they ARE one window.
    // On a Link they are two instruments: the frame's integrated is meterEngine's, kept since the
    // rack last changed, while the loudest is the level tally's max. The comments here used to say
    // "over the window" of both, which is the whole of the lie.
    float loudestWindowSeconds = 0.0f;         // what the loudest figure describes, in seconds
    bool  integratedIsLongerWindow = false;    // TRUE when the integrated figure is of a longer span
    // ---- PLAIN dBFS RMS, FOR THE PROFILE'S CURVE (19 Oct 2026, B's section 86(2)) --------------
    // *"The profile's curve needs the slot input as sine-referenced RMS dBFS, plus the peak.
    // check(playback) sends LUFS only, so the server uses the loudest-3-second LUFS as a stand-in
    // and logs [agent-first-listen] proxy: true."*  These end the stand-in. They are PLAIN RMS off
    // the same hops the LUFS figures come from - a separate ring in LevelTally, because the chain
    // taps are K-weighted and handing LUFS over under a dBFS name is the mislabelling that cost
    // today's morning.
    bool  plainRmsKnown = false;
    float loudestRmsDbfs = 0.0f, typicalRmsDbfs = 0.0f, loudestPeakDbfs = 0.0f;
};

/** WHETHER A STEP CAN BE PUT BACK, AND WHETHER AUTO MAY RUN IT WITHOUT ASKING (Sean's Auto ruling).
 *
 *  ONE FUNCTION, TWO READERS. Auto has to know BEFORE a step runs whether it can be undone; the outcome has
 *  to know AFTER it ran whether to carry an undo token. A second, hand-written list for the pre-flight
 *  question would drift from the tokens inside a round - the argument SHORT90's codec makes for itself,
 *  "one writer, one reader, both here". So ExecutorDo::undoabilityOf is the only thing that decides either,
 *  and a guard leg holds it against the token each op actually returns.
 */
struct Undoability
{
    bool undoable = false;            // the plugin-wide history can put this back
    bool preApprovedInAuto = false;   // not undoable, but Auto may still run it - for a STATED reason
    juce::String whyNot;              // shown on the line when it waits in Auto
    bool mayRunInAuto() const noexcept { return undoable || preApprovedInAuto; }
};

class ToolExecutor
{
public:
    virtual ~ToolExecutor() = default;
    using Done = std::function<void (ToolOutcome)>;

    /** AUTO MODE'S PRE-FLIGHT (Sean's ruling, 14 Oct 2026). See Undoability above: the client asks this
        BEFORE it decides whether an ask_first step waits for a press. The default is the SAFE answer - not
        undoable, so Auto asks - because an executor that has not been taught the question must not be read
        as saying "go ahead". `goal` is the user's own words, which is what makes the undo carve-out safe. */
    virtual Undoability undoabilityOf (const ToolCall& call, const juce::String& goal)
    {
        juce::ignoreUnused (call, goal);
        Undoability u;
        u.whyNot = "this build cannot tell whether that can be put back";
        return u;
    }

    // ---- ITEM 2 (Sean, 13 Oct 2026): A PLAN LINE HAS TO SAY WHOSE CHANNEL IT IS ON --------------------
    // Sean pressed Apply on the plan and the executor then refused with the cross-channel ask - two presses
    // for one decision, the second one asking about a track the card had never named. The card can only name
    // it if something can turn the call's `channel` argument into a track NAME, and only the executor holds
    // the registry. "" means "the channel you already have open", which needs no words.
    virtual juce::String describeOtherChannel (const ToolCall& call) { juce::ignoreUnused (call); return {}; }
    /** "Only suggest plugins EchoJay can auto-dial", his Settings toggle, asked FRESH on every
        request (his 09:49: an unmapped plugin went in with it on, because neither agent body
        carried it - `autoDial` existed only on the chat road). Defaulted false so an executor that
        does not know the question behaves exactly as before. */
    virtual bool autoDialOnly() { return false; }
    /** The genre the header's selector shows, asked fresh on every request. Empty when nothing is
        set, and then the key is absent rather than "" - B's loudness default keys on it (B, 21 Oct:
        it arrived empty because only the chat road sent it). */
    virtual juce::String genreName() { return {}; }

    // ---- WHAT A BUILD WOULD REMOVE, SAID BEFORE THE PRESS (16 Oct 2026, Sean's step 7.4) -------------
    // His report: "make the kick punchier" on a Link already holding Transient Master and Renaissance
    // Axx proposed "build 3 slots: EchoJay EQ, EchoJay Compressor, EchoJay Limiter", which REPLACES the
    // existing chain, with no explanation. His rule: "If a build would remove plugins, the line must say
    // what it removes."
    //
    // Only the executor can answer it - it is the one that can read the live rack the call addresses -
    // and the line is composed in the client, so this is the seam. Returns the names a `build` call would
    // take OFF that rack, in rack order, and nothing for any other op or when nothing would go.
    virtual juce::StringArray buildWouldRemove (const ToolCall& call) { juce::ignoreUnused (call); return {}; }

    /** The plugin names this machine has, for deciding whether a typed word is a NAME or a sentence.

        Sean's item 5, 17 Oct 2026: he answered "Which CL 1B do you have loaded?" by typing "cl1b"
        and the run was STOPPED, because the ask offered three chips, none of them spelled "cl1b",
        and a text matching no chip was read as a new instruction. His ruling: *"A name answers the
        ask (fuzzy match to installed plugins); only a sentence is a new instruction."* The client
        cannot tell a name from a sentence without knowing what is installed, so it asks here. */
    virtual juce::StringArray installedPluginNames() { return {}; }

    /** Shrink a result that is too big to send, keeping its shape (17 Oct 2026).

        Contract section 5 caps a result at 8 KB and the server rejects a larger one with
        `result_too_large`. The executor already trims every result it builds; this exists for the
        case the client meets at the last moment - an oversized result in hand, about to be posted -
        so that the ANSWER gets smaller instead of the run dying. The default drops the largest
        arrays and marks `truncated`; ExecutorRead overrides it with the gate it already owns. */
    /** How many bytes a result for THIS call may be, before the client compacts it (18 Oct 2026).

        Contract section 5 caps a result at 8 KB and that is still the answer for every call. The one
        exception is the server's own inventory census (B's FOR_A 76): it stamps its calls
        `tc_census_<n>`, does not trim their answers, and wants the whole plugin list in ONE step
        rather than eight. The question is asked of the EXECUTOR because the executor is what built
        the page and knows the ceiling it built it under; the client only enforces what it is told. */
    virtual int maxResultBytes (const juce::String& /*callId*/) { return 8192; }

    virtual juce::var compactResult (juce::var v)
    {
        auto bytes = [] (const juce::var& x) { return (int) juce::JSON::toString (x, true).getNumBytesAsUTF8(); };
        if (bytes (v) <= 8192) return v;
        if (auto* o = v.getDynamicObject())
            for (int pass = 0; pass < 12 && bytes (v) > 8192; ++pass)
            {
                juce::Identifier biggest; int n = 0;
                for (const auto& p : o->getProperties())
                    if (auto* a = p.value.getArray()) if (a->size() > n) { n = a->size(); biggest = p.name; }
                if (n <= 1) break;
                auto* a = o->getProperty (biggest).getArray();
                a->removeRange (a->size() / 2, a->size() - a->size() / 2);
                o->setProperty ("truncated", true);
            }
        return v;
    }

    // The opening read sent as `context` on /api/agent/start: channel (role,
    // name, uid), the rack in one line per slot, the inventory shortlist.
    virtual juce::var startContext() = 0;

    virtual void look  (const ToolCall& call, Done done) = 0;
    // "do", the only mutating tool. args.op (contract 2.2): build | add | remove | replace | move | set | set_wet |
    // bypass | set_level | set_io | set_master_wet | set_pre_gain, plus open_editor {slot} (free: opens the Link's
    // floating window for that slot, LINK_REMOTE_CONTROL_PLAN.md section 4). The result is the ack
    // {status, slot?, applied, rack} - a "stale" ack goes back as ok:false error.code "stale".
    virtual void doOp  (const ToolCall& call, Done done) = 0;
    virtual void check (const ToolCall& call, Done done) = 0;   // never called for what:"playback"

    // wait_for_playback support
    virtual void beginPlaybackWindow() = 0;          // "heard" counts from now (the loop-owned tally, not the song's)
    virtual PlaybackReading readPlayback() = 0;
    /** ---- THE KEEP-PLAYING CONTRACT'S ONE QUESTION (B, 22 Oct 2026) ------------------------
        The name of a plugin from this run that still needs the playback - a keep_playing dial
        that has not landed yet - or empty when nothing is waiting on it. The client asks before
        it tells the user they can stop, which is the whole of the contract's point 1.

        Defaulted, not pure: every other implementer (the guards' stubs, the read-only executor)
        answers "nothing is waiting" without being touched, and only the road that arms dials has
        anything to say. */
    virtual juce::String keepPlayingPlugin() { return {}; }

    // undo
    virtual juce::String captureCheckpoint (const juce::String& label) = 0;   // "" = no checkpoint available
    virtual void undoStep (const juce::String& undoToken, Done done) = 0;
    virtual void undoToCheckpoint (const juce::String& checkpoint, Done done) = 0;
};

// -----------------------------------------------------------------------------
//  The stand-in until A's executor lands. HONEST, not helpful: every tool answers
//  not_in_phase so the model is told the truth, playback never counts a second
//  (so the wait TIMES OUT instead of pretending), and there is no checkpoint.
//  The client's loop, cards and stop path are fully exercisable against it.
// -----------------------------------------------------------------------------
class StubExecutor : public ToolExecutor
{
public:
    juce::var startContext() override
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("executor", "stub");
        return juce::var (o);
    }
    void look  (const ToolCall&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "this build has no agent executor wired (stub)")); }
    void doOp  (const ToolCall&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "this build has no agent executor wired (stub)")); }
    void check (const ToolCall&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "this build has no agent executor wired (stub)")); }
    void beginPlaybackWindow() override {}
    PlaybackReading readPlayback() override { return {}; }
    juce::String keepPlayingPlugin() override { return {}; }
    juce::String captureCheckpoint (const juce::String&) override { return {}; }
    void undoStep (const juce::String&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "no executor: nothing was applied, so nothing can be undone")); }
    void undoToCheckpoint (const juce::String&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "no executor: nothing was applied, so nothing can be undone")); }
};

} // namespace echojay::agent
