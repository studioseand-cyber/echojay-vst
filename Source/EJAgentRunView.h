#pragma once
// =============================================================================
//  EJAgentRunView — ITEM 0 (Sean, 14 Oct 2026): THE AGENT RUN IS A MESSAGE.
//
//  "Like Claude does it." One agent run renders as ONE row in the transcript:
//  the opening sentence, the step list ticking in place, the buttons on the
//  lines, and the closing sentence that the row collapses to when it is over.
//  docs/MOCKUP_AGENT_IN_CHAT_APPROVED.md is the specification; this header is
//  its geometry and its words.
//
//  WHY A MODEL AND NOT A DIRECT READ OF THE CLIENT. The row has TWO producers
//  and they must not drift:
//
//    1. the LIVE client, while the run is going (EJAgentClient::steps() etc.);
//    2. the SNAPSHOT persisted on the message (ChatMsg::agentData), which is
//       what a reopened chat has - and, once item 0b lands, what a run that
//       finished with no window attached leaves behind.
//
//  Both produce a `RunView`, and everything downstream - the chat's measure
//  pass, its paint pass, its hit testing, and the rig's published state - reads
//  only that. The alternative was the chat measuring from the client and
//  painting from the message, which is the "two authorities for one height"
//  fault this file's neighbours already record twice (the ASK chips, then the
//  ops card, both from one pass missing a term).
//
//  ONE LAYOUT FUNCTION. layoutRun() is pure over the model and returns every
//  rect, including the button rects the editor turns into click zones. The
//  measure pass and the paint pass call the same function with the same width,
//  so a height and a pixel cannot disagree; a guard calls it with no window at
//  all.
//
//  DOWNGRADE SAFETY (Sean's standing rule). The snapshot is a NEW key on the
//  persisted message. An old build never reads it, and the row's prose lives in
//  ChatMsg::content as it always has - so a project saved by this build opens in
//  ship_2026-10-09a showing the run's closing sentence as an ordinary reply,
//  which is true rather than merely harmless.
// =============================================================================
#include <JuceHeader.h>
#include "EJPendingClaim.h"   // nothing claims "added" while the add is still waiting
#include <vector>
#include "EJAgentPanelLayout.h"
#include "EJAgentClient.h"

namespace echojay::agentui
{

// ---- the model -----------------------------------------------------------------
// `status` mirrors EJAgentClient::Step::Status by VALUE, deliberately: the
// snapshot is JSON on a saved message, so an enum's numbering is part of the
// file format. statusToken()/statusFromToken() put a NAME on the wire instead,
// and the int is only ever an in-memory convenience.
/** ---- IS THIS ROW THE RUN THIS WINDOW IS HOLDING? (his 14:15, 21 Oct 2026) ------------------

    The answer decides whether a card draws its CHIPS, its Stop and its Undo, or is drawn as
    history - so getting it wrong silently removes every control from a card that is still waiting
    for him. It got it wrong on his 14:15 reopen, and the log's order says how: the adopt recorded
    the row's integer id, the chat replay then reissued every restored row's id, and the integer
    named nothing.

      rowKey / liveKey   the run's own key, minted once, carried in the snapshot. THE identity.
      rowId / liveId     the per-window integer, fast and correct for a row this window wrote,
                         and reissued by a replay - so it can only ever ADD to the answer.
      hasSession         whether the client is holding a run at all.

    TRUE means live. A key match wins; the integer is kept as the fast path, because a row this
    window just created has an id before anything has written a snapshot for it to carry a key in. */
inline bool rowIsLiveRun (const juce::String& rowKey, const juce::String& liveKey,
                          int rowId, int liveId, bool hasSession)
{
    if (! hasSession) return false;
    if (rowId != 0 && rowId == liveId) return true;
    return rowKey.trim().isNotEmpty() && rowKey.trim() == liveKey.trim();
}

/** Declared here, defined at the foot of the file (it forwards to the protocol's own copy, so
    the client and this view compare prose with the SAME function - see its note down there). */
inline juce::String normalisedProse (const juce::String& s);

struct RunStep
{
    juce::String id;
    juce::String label;         // the step in plain words (the server's summary)
    juce::String detail;        // the landed line, or the user's error line
    juce::String modelDetail;   // the technical message, shown only when Details is open
    juce::String right;         // the right-hand status word ("applied", "auto-applied", ...)
    juce::String askReason;     // Auto: why this one still waits. Never invented here.
    juce::StringArray chips;    // a question's answer chips (this step is the question)
    /** One note per chip, parallel to `chips`, as the server sent it ("I can set it", "you'll set
        it by ear"). Empty for a chip with no note, and the whole array is empty for an ask that
        carried none - which is the geometry every chip had before 18 Oct. */
    juce::StringArray chipNotes;
    /** Parallel again: TRUE where the server marked the choice as one the user sets by hand. */
    juce::Array<bool>  chipHandSet;
    juce::String answered;      // "You said: ..." once a chip is tapped
    int  status = 0;            // EJAgentClient::Step::Status
    bool applySkip = false;     // Ask mode, awaiting a decision -> [Apply] [Skip] on the line
    /** His 21:52: a build awaiting his decision is a PLAN, so the pair reads [Go] [Cancel] and the
        slots are this row's own numbered lines. Set only for a build whose every slot names a
        plugin - the unreadable ones never become a decision at all (his 21:48). */
    bool buildPlan = false;
    bool canUndo = false;
    bool hasDetails = false;    // a failed line with a technical message behind it
    bool expanded = false;      // Details open (by step id, so a relayout cannot collapse it)
    bool autoApplied = false;
};

/** ---- DOES THIS RUN'S CARD BELONG IN THE CHAT THAT IS OPEN? (Sean's 18:18, 20 Oct 2026) ------

    THE THIRD TIME IN ONE DAY A RUN WENT INVISIBLE, so the decision is a function with a name rather
    than a condition inside a 200-line sync method, and it is legged both ways.

      runChatId   the chat the LIVE RUN was started in - from the run itself (EJAgentClient::start is
                  handed it and keeps it), never from a member of the editor. A row does not survive
                  a chat switch, so anything the editor remembers about "the last row" describes the
                  last run and not this one: that is the 18:18 fault exactly.
      openChatId  the chat on screen now.

    TRUE means a row opens here. The two rules it has to satisfy at once:
      - a NEW run typed in this chat was started with this chat's id -> its card is drawn (his 16:06
        and 18:18: "blank again. No card, no spinner, no Stop");
      - an OLDER run still waiting elsewhere carries that chat's id -> refused here, and its card
        stays where it belongs (his 11:59: survival is not capture).

    AN UNKNOWN EITHER SIDE MEANS DRAW IT. A run with no chat id (nothing told us) and a window with
    no chat open are both states in which refusing would hide a live run - and an invisible run is
    the worse failure of the two by a long way: he cannot even stop it. */
/** ---- RETIRED AS THE BIRTH GATE (stage D, 21 Oct 2026) ------------------------------------
    The product no longer calls this to decide whether to OPEN a row. Its rule - "an unknown either
    side means draw it" - was right for a window with no chat open and wrong for a window about to
    become a different chat, and it could not tell them apart: that is his 19:51 card at the top of
    a chat it had nothing to do with. A run's entry now exists in its own chat from the moment the
    run starts (stages A and B), so NOT drawing it in another chat hides nothing, which is what made
    the old default necessary.

    Kept as a pure, legged decision because the two legs around it still say something true about
    chat identity, and the rules it used to serve are now held end to end in tools/chat_log_guard
    (ST: a stopped run's entry cannot appear in another chat; MO: nothing posts to the chat on
    screen). Nothing in Source calls it. */
inline bool rowBelongsInChat (const juce::String& runChatId, const juce::String& openChatId)
{
    if (runChatId.trim().isEmpty() || openChatId.trim().isEmpty()) return true;
    return runChatId.trim() == openChatId.trim();
}

/** ---- IS ANYTHING ON THIS CARD STILL MOVING? (his 14:08 ruling, 21 Oct 2026) ----------------
    TRUE when the "Thinking..." line belongs under the rows as they stand: there are none yet (the
    gap between Send and the first step), or the last one has SETTLED - ticked, failed, declined,
    not run, undone or stopped. FALSE while the last row is itself Running or waiting on an answer,
    because that row is already the indicator and two moving lines would claim the same work twice.

    A free function with a name, for the reason rowBelongsInChat is one: it is a decision about what
    he sees, and the guard holds it in both directions. */
inline bool lastRowHasSettled (const std::vector<RunStep>& steps)
{
    using St = EJAgentClient::Step::Status;
    if (steps.empty()) return true;
    const int st = steps.back().status;
    return st != (int) St::Running && st != (int) St::AwaitingApproval
        && st != (int) St::Pending;
}

struct RunView
{
    juce::String sessionId;
    // A key minted when the row is created and never reissued. The sessionId is EMPTY until the
    // server answers /start, so it cannot identify the row in the window where the row is born -
    // and matching on "an assistant entry with the same (empty) content" picked the wrong one.
    // This is the same lesson as fix 6's editor identity: an address, or a blank, is not an identity.
    juce::String runKey;
    juce::String opening;       // the first sentence. Lives in ChatMsg::content; carried here for the guard.
    juce::String closing;       // the closing sentence once the run is over

    /** ---- ONE SENTENCE, DRAWN ONCE (16 Oct 2026, Sean in Logic) --------------------------------
        He read a MTransient run in Logic: *"the closing appears twice, as a bubble ABOVE the ASK row
        and again inside the run"* - and named the cause from my own evidence: A3-live's drawn.txt has
        `opening` and `closing` as the SAME sentence. On a short run they are: the model streams one
        sentence, which becomes the message's content (the bubble, drawn above the rows), and the
        done-summary is that same sentence, which the row draws as its closing underneath.

        So when they are the same sentence, the row does not draw it again. The copy that stays is the
        message's own content - it is what the transcript holds and what an older build reads - and
        the one that goes is the repeat. Compared on the trimmed text, because the only thing that
        makes them differ is whitespace from the stream.

        Called by the editor once the opening is known (it comes from the ChatMsg, not from the JSON),
        so every reader of the view - the paint pass, the measure pass, the rig's published row - gets
        the same answer from the same place. */
    void dropClosingIfItRepeatsTheOpening()
    {
        if (closing.trim().isEmpty() || opening.trim().isEmpty()) return;
        const auto a = closing.trim(), b = opening.trim();
        if (a == b) { closing.clear(); return; }
        // ---- AND WHEN THE BUBBLE HAS A SENTENCE OF ITS OWN IN FRONT (17 Oct 2026, item 6) -------
        //
        // Last night's cut compared the two for EQUALITY, and he read the closing twice again this
        // morning: *"the bubble still repeats the closing when the bubble has extra text before
        // it"*. The bubble was "Done - I've set it up on the kick." followed by the summary, so the
        // strings differed by one sentence and the repeat survived the test that was meant to catch
        // it.
        //
        // What must not happen is the same sentence appearing twice on screen, so that is what is
        // asked: does the bubble already END WITH the closing (the ordinary shape - prose, then the
        // summary), or CONTAIN it as a whole line? Either way the row drops its copy and the bubble
        // keeps the words, which is the one he reads first.
        //
        // Deliberately not "contains the text anywhere": a closing of "Done." would then be dropped
        // because the bubble happens to contain that word. The test is a whole line or the tail.
        if (b.endsWith (a)) { closing.clear(); return; }
        for (const auto& line : juce::StringArray::fromLines (b))
            if (line.trim() == a) { closing.clear(); return; }
        // ---- AND WHEN THE SUMMARY IS THE ANSWER'S BEGINNING, CUT (18 Oct 2026) -----------------
        //
        // He read it a third time: *"the closing still shows twice, and the card copy is still cut
        // off mid-word (...RCompresso)"*. His 13:56 log says why, exactly: the round's talk carried
        // the whole answer (1,077 characters) and the done frame's summary carried the SAME answer
        // truncated mid-word - "...Pro Audio DSP DSM V3, RCompresso" - so the two strings were
        // neither equal, nor one the tail of the other, nor a whole line of it. Every test above
        // missed it and the row drew the cut copy underneath the full one.
        //
        // A summary that is the opening's PREFIX is the opening, shortened. It says nothing new, so
        // it goes - and because this is the one owner of "are these the same sentence", the record
        // and the card agree about it without a second rule.
        if (b.startsWith (a)) { closing.clear(); return; }
    }

    /** ---- THE ANSWER, ONCE, IN THE CARD (18 Oct 2026) ------------------------------------------
        *"Show it once, in full, inside the card."*  The prose reached the screen twice because two
        surfaces drew it: the message's own bubble (ChatMsg::content, which is `opening` here) above
        the card, and the card's closing inside it. Dropping the repeat was only half of what he
        asked for - the copy that survived was the BUBBLE, not the card.

        So the card takes the whole answer: the opening, plus whatever the closing adds that the
        opening does not already say. The caller then draws no bubble for a row that has a card
        (EchoJayEditor's two passes), which makes "shows twice" unreachable rather than tested for.
        `content` is untouched: it is still what the record holds and what an older build reads. */
    void foldProseIntoTheCard()
    {
        // ---- AN UNDONE RUN'S PROSE IS NOT FOLDED BACK IN (22 Oct 2026) -------------------------
        //
        // His #14 ruling - *"after 'Everything the agent changed has been undone' the run still shows
        // 'Pre gain on the Mix Bus is set to 0 dB', a stale sentence"* - is honoured in build(),
        // which REPLACES the closing once every change is undone. Rig row C5 kept failing anyway,
        // and the reason is here: the card's closing is folded from the OPENING plus the closing, and
        // the opening is the model's own prose, which says the thing the undo just made untrue. The
        // replacement owned one field and the fold wrote the stale sentence back into it.
        //
        //     closing = "The pre gain on the Mix Bus is set to 3 dB.\n\nEverything this run changed
        //                has been undone, so what it said above no longer describes the rack"
        //
        // So the rule belongs to whichever code decides WHAT THE CARD SAYS, which is this function.
        // `closingBeforeUndo` is set by exactly that replacement and is carried in the snapshot, so
        // it is the mark that the prose above has been voided - paired with changeCount, which is the
        // same condition the replacement itself used, so a run that lands something new afterwards
        // folds normally again instead of latching on a field that survived in the record.
        if (closingBeforeUndo.isNotEmpty() && changeCount == 0) return;
        const auto b = opening.trim();
        if (b.isEmpty()) return;                     // a card with no prose is unchanged
        dropClosingIfItRepeatsTheOpening();          // the same-sentence rule, one owner
        const auto a = closing.trim();
        if (a.isEmpty())      { closing = b; return; }
        if (a.startsWith (b)) { closing = a; return; }   // already folded: never fold twice
        closing = b + "\n\n" + a;
    }
    juce::String statusWord;    // "Working" | "Waiting for your OK" | ... (the mockup's words)
    juce::String modeBadge;     // "ASK" | "AUTO"
    juce::String notice;        // one amber line: a refused send, an undo-all outcome
    juce::String askQuestion;   // the pending question's text (its chips are on its step row)
    juce::String playbackLine;
    double playbackFrac = 0.0;
    bool   playbackReached = false;
    bool   playbackActive = false;
    int  state = 0;             // EJAgentClient::State
    bool running = false;       // a live session: the spinner animates, Stop is on the row
    /** ---- B'S STATUS LINE, UNDER "Thinking..." (his 15:28 request, 21 Oct 2026) --------------
        *"under the "Thinking..." line, show the latest status text B sends during the wait ...
        replacing the previous one as each arrives. It's one line, in the same muted style as
        agent-off's progress lines. It disappears when the next step appears or an ask is raised.
        With no status yet, just "Thinking...".*

        Carried on the view (and so in the entry's payload, which is where he asked it be drawn
        from) and set from the client's own latest frame. Empty = no status yet, and then the row is
        just "Thinking...". It is never recorded as history: the client clears it on a step, an ask
        and the end, so a reopened card shows "Thinking..." until B sends the next one. */
    juce::String statusText;
    bool finished = false;      // Done | Stopped | Failed
    bool stepsOpen = true;      // false = collapsed to the closing sentence + "N changes"
    bool showApplyAll = false;
    bool showStop = false;
    bool showUndoAll = false;
    bool showGotIt = false;
    /** His 08:10 point 2: the BUTTON says "Got it" too until the run can promise nothing else will
        need playback. One rule for the line and the button - two surfaces saying different things
        about the same fact is what he read at 08:08 and 08:10. */
    bool canStopPlaying = false;
    bool showRetry = false;
    bool onePlanLine = false;   // item 2: one line means ONE apply, named "Apply"
    /** TRUE when the steps are collapsed but a FAILED line is still shown: Sean's #13 asked for Hide
        steps to work on a failed run, and 11b's rule says a failure must not be folded away. Both. */
    bool failuresOnly = false;
    /** How many steps the run has, for the control's label when NOTHING was changed (Sean's #18). */
    int  stepCount = 0;
    /** The closing sentence as the server wrote it, kept when an undo has replaced it (Sean's #14) -
        so the record still holds what the run said it had done, and only the DISPLAY moves on. */
    juce::String closingBeforeUndo;
    /** The line under an ask with nothing to tap: "Type your answer below." Never invented choices -
        the plugin cannot know what B meant to offer - but never a dead end either (16 Oct 2026). */
    juce::String askTypeHint;
    /** ---- A CARD THAT IS OVER (18 Oct 2026) ---------------------------------------------------
        TRUE when this view was rebuilt from the chat RECORD rather than from a live client. The card
        draws the same: every step, the "You said" lines, the read-backs, the red failures, the
        closing - and Details and Hide steps still work, because those are questions about the
        PICTURE, not about the run.

        What a record must not offer is a control that would act on a run that no longer exists:
        Apply, Skip, Undo, Stop, Retry, and a tappable chip. Those are not drawn. A chip's answer
        still shows, on the step it answered, which is where "You said" already lives. */
    bool fromRecord = false;
    int  changeCount = 0;       // how many steps landed (the "N changes" button)
    std::vector<RunStep> steps;

    /** ---- THIS CARD IS HISTORY (19 Oct 2026; the rule itself is from 18 Oct) -----------------
        A row whose run is not the one this window is running draws in full - that is the whole
        point of keeping the snapshot - but the controls that would ACT on it are not offered,
        because the run they would act on is over.

        *"Both are read-only views, so on a restored card they must sit exactly where they sat live
        and open the same. Only the buttons that act stay locked: Undo, Apply, Skip, Retry, Stop,
        chips."*  So Details and the change list are deliberately NOT touched here.

        Moved out of the editor (agentRunViewFor) into the view on 19 Oct for one reason: his guard
        leg compares a live layout with a restored one, and a guard that re-implemented this
        transformation would be comparing its own idea of a record with the product's. */
    void makeRecord()
    {
        fromRecord     = true;
        running        = false;
        finished       = true;
        showStop       = false;
        showRetry      = false;
        showApplyAll   = false;
        showUndoAll    = false;
        showGotIt      = false;
        canStopPlaying = false;
        playbackActive = false;
        for (auto& s : steps) { s.applySkip = false; s.canUndo = false; }
        if (statusWord.isEmpty()) statusWord = "Done";
    }

    bool empty() const noexcept { return sessionId.isEmpty() && steps.empty() && closing.isEmpty(); }
};

// ---- the words on the wire -------------------------------------------------------
inline const char* statusToken (int s) noexcept
{
    using S = EJAgentClient::Step::Status;
    switch ((S) s)
    {
        case S::Pending:          return "pending";
        case S::AwaitingApproval: return "awaiting";
        case S::Running:          return "running";
        case S::Done:             return "done";
        case S::Failed:           return "failed";
        // ---- "declined" IS NOT "skipped" (C's point 45, 29 Oct 2026) ----------------------
        //
        // C1 now carries both, and B's b/declined-words adds `declined` to plan_result's status
        // set. Until this line, a step HE declined on a plan card went out as "skipped" - so once
        // that branch merges the server would say "...was skipped, because its step waited...",
        // which is a false reason for a choice he made deliberately. He did not skip it; he said no.
        case S::Declined:         return "declined";
        case S::NotRun:           return "not_run";
        case S::Undone:           return "undone";
        case S::Stopped:          return "stopped";
    }
    return "pending";
}

inline int statusFromToken (const juce::String& t) noexcept
{
    using S = EJAgentClient::Step::Status;
    if (t == "awaiting") return (int) S::AwaitingApproval;
    if (t == "running")  return (int) S::Running;
    if (t == "done")     return (int) S::Done;
    if (t == "failed")   return (int) S::Failed;
    // BOTH SPELLINGS COME BACK, because this token is also what a stored run record holds: every
    // record written before today says "skipped", and a reader that only knew "declined" would
    // silently turn every one of those steps into Pending on reload. New records say "declined".
    if (t == "declined") return (int) S::Declined;
    if (t == "skipped")  return (int) S::Declined;
    if (t == "not_run")  return (int) S::NotRun;
    if (t == "undone")   return (int) S::Undone;
    if (t == "stopped")  return (int) S::Stopped;
    return (int) S::Pending;
}

/** The mockup's status words, exactly. The header's right-hand status is the
    one place the user reads "what is this run doing", so it is a closed list
    rather than a sentence assembled at the call site. */
inline juce::String statusWordFor (EJAgentClient::State st) noexcept
{
    using S = EJAgentClient::State;
    switch (st)
    {
        case S::Idle:             return {};
        case S::Connecting:
        case S::Streaming:
        case S::Executing:
        case S::Posting:          return "Working";
        case S::AwaitingApproval: return "Waiting for your OK";
        case S::AwaitingAsk:      return "Waiting for your answer";
        case S::Listening:        return "Listening";
        case S::Done:             return "Done";
        case S::Stopped:          return "Stopped";
        case S::Failed:           return "Failed";
    }
    return {};
}

/** The right-hand word on a step row. The mockup's list, and nothing else:
    "applied", "auto-applied", "skipped", "not applied", "undone", "stopped",
    "your answer?", "working". A row with buttons shows the buttons instead,
    which is why this returns "" for an awaiting row in Ask mode. */
inline juce::String rightWordFor (const EJAgentClient::Step& s, bool askPendingHere) noexcept
{
    using St = EJAgentClient::Step::Status;
    if (askPendingHere) return "your answer?";
    switch (s.status)
    {
        case St::Pending:          return {};
        case St::AwaitingApproval: return {};                 // the buttons live there
        case St::Running:          return "working";
        // ---- ONLY A `do` APPLIES ANYTHING (Sean's #5, 16 Oct 2026) ------------------------------
        // "a stray green 'applied' label on the right of the run's status lines." It was not stray:
        // every finished step said "applied", including a look and a check - lines that read the
        // state and changed nothing. A read that claims to have applied something is the same class
        // of lie as a figure nobody can see. The green tick already says the step finished, so a
        // read gets no word at all, and "applied" means what it says.
        case St::Done:             return s.isDo() ? (s.autoApplied ? "auto-applied" : "applied") : juce::String();
        case St::Failed:           return "not applied";
        // ---- HE DID NOT SKIP IT, HE SAID NO (C's point 53, 29 Oct 2026) -------------------
        //
        // This row word stayed "skipped" after the wire token became "declined", so the plugin
        // contradicted its own server: the closing now says "you chose not to" while the row said
        // "skipped". Worse, "skipped" is the word E2 retired - it reads as something that happened
        // TO him rather than a decision he made, and the two are not the same event.
        case St::Declined:         return "declined";
        case St::NotRun:           return "not applied";
        case St::Undone:           return "undone";
        case St::Stopped:          return "stopped";
    }
    return {};
}

// ---- live client -> model ---------------------------------------------------------
/** The ONE reader of the client for rendering purposes. `expanded` is the
    editor's set of step ids whose Details are open (by id, never by index:
    the row list is rebuilt on every change and an index would move under the
    user - item 3's rule, 13 Oct). */
/** Declared here, defined below: runViewFrom needs it to tell a closing that repeats a failed
    step's own line from one that says something new (his 17:20 run). */

/** ---- ONE DRAWING OF ONE DIAL, WHEREVER IT IS DRAWN (his test 3 of 16:40, 23 Oct 2026) --------

    The step for a dial on playback: running while it dials, Done with its own result when it lands.
    Called from beside the add that armed it, and again at the end for any dial no add on this card
    claimed - `drawn` is what keeps those two from drawing the same dial twice.

    The RESULT is the dial's own sentence (B's done line, or ours when B sent none) and never a
    second wording of it: his 08:43 ruling, and the reason the client keeps the landed results. */
inline void pushDialStepFor (const juce::String& plugin, const EJAgentClient& c, RunView& v,
                             juce::StringArray& drawn)
{
    if (plugin.isEmpty() || drawn.contains (plugin)) return;
    const auto landed   = c.dialResults();
    const auto dialling = c.keepPlayingPlugin();
    const bool hasLanded = landed.containsKey (plugin);
    if (! hasLanded && dialling != plugin) return;      // no dial here: nothing to draw
    drawn.add (plugin);
    RunStep r;
    r.id = "dial:" + plugin;
    if (hasLanded)
    {
        r.label  = "The " + plugin + " set itself on the playback";
        r.detail = landed[plugin];
        r.right  = "set";
        r.status = (int) EJAgentClient::Step::Status::Done;
        ++v.changeCount;
    }
    else
    {
        r.label  = "Keep it playing while the " + plugin + " sets itself";
        r.detail = "I'll say when it's done.";
        r.right  = "setting itself";
        r.status = (int) EJAgentClient::Step::Status::Running;
    }
    v.steps.push_back (std::move (r));
}

inline RunView runViewFrom (const EJAgentClient& c, const juce::StringArray& expanded, bool stepsOpen)
{
    using S  = EJAgentClient::State;
    using St = EJAgentClient::Step::Status;
    RunView v;
    v.sessionId  = c.sessionId();
    v.state      = (int) c.state();
    v.statusWord = statusWordFor (c.state());
    v.modeBadge  = c.approvalMode() == EJAgentClient::ApprovalMode::Auto ? "AUTO" : "ASK";
    v.notice     = c.notice();
    v.running    = c.isActive();
    v.finished   = c.state() == S::Done || c.state() == S::Stopped || c.state() == S::Failed;
    // ---- A RUN THAT FAILED A LINE DOES NOT COLLAPSE (16 Oct 2026) ------------------------------
    // The mockup collapses a finished run to its closing sentence with an "N changes" control, and the
    // G4 fix made that the default at last. Row G7 then went red, and it was right to: Details lives
    // ON a step line, so collapsing put the only account of a FAILURE behind a press - a run that says
    // "1 change" with its failure folded away is the item-3 complaint ("Details did nothing") arriving
    // from the other side. A failed line keeps the steps open; everything else collapses.
    bool anyFailed = false;
    for (const auto& st : c.steps()) if (st.status == St::Failed) anyFailed = true;
    // ---- SEAN'S #13 (16 Oct 2026): HIDE STEPS WORKS ON A FAILED RUN TOO ------------------------
    // The rule above was written as "a failed run never collapses", which made the control a
    // no-op on exactly the runs he was pressing it on - a button that does nothing is item 2's
    // complaint, and his #13 named it. The rule it was protecting still holds: a failure must not
    // be folded away. So the press is HONOURED and the collapse keeps the failed lines on screen -
    // failuresOnly - rather than refusing to collapse at all.
    v.failuresOnly = ! stepsOpen && v.finished && anyFailed;
    v.stepsOpen  = stepsOpen || ! v.finished;
    v.closing    = v.finished ? (c.state() == S::Failed ? c.errorMessage() : c.doneSummary()) : juce::String();
    // ---- ONCE, NOT TWICE (19 Oct 2026, his 17:20 run) ------------------------------------------
    // *"a failed step's error shows twice ('slot 0 is not on the rack (1..1).' on two lines); show
    // it once."*  When the step's failure IS the run's failure, errorMessage() above and that step's
    // own `detail` are the same sentence, so the row printed it on the step line and again as the
    // closing. The step line is the one that belongs: it names WHICH step failed, which the closing
    // cannot. So the closing is kept only when it says something no failed step already says -
    // exactly the rule the Done frame applies to doneSummary against the round's talk.
    if (v.closing.isNotEmpty())
    {
        const auto key = normalisedProse (v.closing);
        for (const auto& st : c.steps())
            if (st.status == St::Failed && normalisedProse (st.detail) == key)
            {
                v.closing.clear();
                break;
            }
    }
    v.showStop   = v.running;
    v.showUndoAll = v.finished && c.canUndoAll();
    // RETRY FOR AN UNREADABLE PLAN TOO (his 21:48, 22 Oct 2026). The run itself has not failed -
    // the server is answering - but the only thing it offered named no plugin in two of its slots,
    // so the card says it could not read the plan and the one road out is to ask again.
    v.showRetry   = (c.state() == S::Failed && c.errorRetryable()) || c.planUnreadable();
    v.showGotIt   = c.state() == S::Listening;
    v.canStopPlaying = c.stopInviteAllowed();
    v.onePlanLine = c.planLines().size() <= 1;
    // ---- SEAN'S #4 (16 Oct 2026): ONE WAITING LINE HAS ONE PAIR OF BUTTONS ---------------------
    // "One pending line: show only the line's buttons. Two or more: add Apply all / Skip all under
    // the list, as in the mockup." The line's own Apply is the primary control (item 2's fix), so
    // a second Apply under it was two doors onto one decision - and the user had to guess whether
    // they differed. The footer pair returns the moment there is more than one decision to make.
    // The ONE exception, deliberate: an approval state with no line showing buttons at all would
    // have no road out, so the footer stays as the fallback rather than leaving a run stuck.
    int waitingLines = 0;
    for (const auto& st : c.steps())
        if (st.status == St::AwaitingApproval && c.state() == S::AwaitingApproval) ++waitingLines;
    v.showApplyAll = c.state() == S::AwaitingApproval && waitingLines != 1;

    if (c.askPending())
    {
        const auto ask = c.pendingAsk();
        v.askQuestion = ask.question;
        // ---- SEAN'S BLOCKER, 16 Oct 2026: AN ASK IS NEVER A DEAD END -------------------------------
        // The 09:19 ask arrived with no choices at all. The row showed the question and a faint
        // "your answer?" and there was nothing to press; typing was refused. The choices cannot be
        // invented here - only B knows what it meant to offer - but the road that IS open is named,
        // so the user can always answer: the composer takes it (EJAgentClient::interceptTyped).
        if (ask.labels.isEmpty())
            v.askTypeHint = "Type your answer in the box below and press Enter.";
        else if (ask.allowFreeText)
            v.askTypeHint = "Tap one, or type your own answer.";
    }
    if (c.playback().active)
    {
        const auto& pb = c.playback();
        v.playbackActive  = true;
        v.playbackLine    = pb.line;
        v.playbackReached = pb.reached;
        v.playbackFrac    = pb.minSeconds > 0.0 ? juce::jlimit (0.0, 1.0, pb.heardSeconds / pb.minSeconds) : 0.0;
    }

    const bool undoNow = c.undoAllowedNow();
    // ---- ONLY THE LAST UNDOABLE STEP CARRIES AN UNDO BUTTON (16 Oct 2026) ----------------------
    //
    // The matrix's D4 pressed a step's Undo and watched the OTHER step's value change. The sink is
    // why: a per-step undo pops the plugin-wide history ONCE - the last change - whatever token it
    // was given. For the last step that is exactly right; for any earlier one the button says
    // "undone" beside a line whose value is still on the rack and reverts something else.
    //
    // A history of this shape cannot undo a middle entry without a redo model, which does not exist
    // before beta. So the button is offered where its promise is true - the last step that handed
    // back a token - and everything earlier is covered by the run's own "Undo all", which walks back
    // to the checkpoint. A button that can only tell the truth, rather than a button everywhere.
    // ---- AND ONLY WHEN THERE IS EXACTLY ONE OF THEM (16 Oct 2026, the second finding) ----------
    //
    // "The last undoable step" was still not honest enough. The plugin-wide history COALESCES
    // consecutive edits (EchoJayUndoHistory: an entry whose `before` equals the last entry's `after`
    // is merged into it), so a run that set the pre-gain and then the master wet in one round has ONE
    // entry covering both. D4 pressed the last step's Undo and watched the pre-gain go back to 0.00
    // as well - correctly, because that is what the entry held, while the row said "undone" against
    // one line and nothing against the other.
    //
    // So the button is offered where its promise is exactly true: a run with ONE undoable step, whose
    // entry can only be that step's. Anything with two or more is undone as a whole, by "Undo all",
    // which is what it is for. A control that is right nine times in ten is the thing this product
    // keeps being burned by.
    int lastUndoable = -1, undoableCount = 0;
    {
        int k = -1;
        for (const auto& s : c.steps()) { ++k; if (s.canUndo()) { lastUndoable = k; ++undoableCount; } }
        if (undoableCount != 1) lastUndoable = -1;
    }
    const bool askNow  = c.askPending();
    // The pending ask hangs off the LAST step, which is the tool call that asked it. With no steps at all
    // (a talk-only round that asks straight away) it gets a row of its own so the chips have somewhere to
    // live - a question with no visible chips is how the rig stalled five rows on 15 Oct.
    const int lastIdx = (int) c.steps().size() - 1;
    // Which dials this card has already drawn. Shared by the two call sites (beside an add, and the
    // tail sweep), so a dial is drawn exactly once wherever its add happens to be.
    juce::StringArray drawnDials;
    int i = -1;
    for (const auto& s : c.steps())
    {
        ++i;
        const bool askHere = askNow && i == lastIdx;
        RunStep r;
        r.id          = s.id;
        // "Show the question or nothing" (Sean, 18 Oct): a pending ask whose call said nothing worth a
        // line shows its chips and its hint and no label, rather than the word "step" - which is the
        // same empty noise as the bare "ask" describeCall no longer passes on.
        r.label       = s.label.isNotEmpty() ? s.label
                                             : (askHere ? juce::String() : juce::String ("step"));
        // ---- AND A WAITING LINE DOES NOT SAY "ADDED" (28 Oct 2026, Sean's point 2) -------------
        //
        // SCEN RP4: while a card sat in awaiting_approval the screen read "I added a Tube-Tech CL 1B
        // compressor to the kick with threshold around -15 dB..." and nothing had been added. The
        // rule is one tense on a line that is waiting - the figures are the model's and they are
        // what the user is approving, so they are left exactly where they were. echojay has it as a
        // pure function because the plan card (A2) and the walk-through (A5) must obey it too.
        {
            const bool stillWaiting = (s.status == EJAgentClient::Step::Status::AwaitingApproval
                                       || s.status == EJAgentClient::Step::Status::Pending);
            r.label  = echojay::pendingClaimText (r.label,  stillWaiting);
            r.detail = echojay::pendingClaimText (s.detail, stillWaiting);
        }
        r.detail      = r.detail.isNotEmpty() ? r.detail : s.detail;
        r.modelDetail = s.modelDetail;
        r.status      = (int) s.status;
        r.autoApplied = s.autoApplied;
        r.askReason   = s.askReason;
        r.canUndo     = undoNow && s.canUndo() && i == lastUndoable;
        r.hasDetails  = s.status == St::Failed && s.modelDetail.isNotEmpty() && s.modelDetail != s.detail;
        r.expanded    = expanded.contains (s.id);
        r.applySkip   = s.status == St::AwaitingApproval && c.state() == S::AwaitingApproval;
        // ---- A BUILD AWAITING A DECISION IS DRAWN AS A PLAN (his 21:52, 22 Oct 2026) ---------
        //
        // *"A raw 'build N slots' step with Apply/Skip must not be offered for a build either ...
        //  show it as a plan needing Go, or refuse it plainly."*  Refusal belongs to a plan that
        // cannot be READ (21:48, and the step is already Failed by the time it gets here); a
        // readable one is work he asked for, so it is drawn as the plan it is.
        //
        // The slots come from `s.call` through the protocol's one reader, so this cannot offer a
        // chain the refusal would have refused, and there is no second copy of the chain to drift.
        // The heading is the SERVER's when it sent a plan frame this round - one author for the
        // words - and our own sentence only when it did not.
        //
        // AND ONLY WHEN THE SERVER SENT NO PLAN CARD - which is his ruling word for word ("if the
        // server sends a build op WITH NO PLAN"). B's own fix of 23 Oct makes every build a plan
        // first, so the ordinary road now arrives as a plan card whose items carry the server's own
        // per-step words ("elysia alpha mix for about 2 dB"). Re-heading those rows here would
        // overwrite the better sentence AND put the plan on screen twice - his test 8b, arriving
        // from the other side. This draws the plan only where there is none.
        if (r.applySkip && ! c.planCardThisRound())
        {
            juce::StringArray slots;
            if (echojay::agent::buildPlanSlots (s.call, slots))
            {
                r.buildPlan = true;
                r.label = "The plan: build this chain";
                juce::String lines;
                for (int k = 0; k < slots.size(); ++k)
                    lines << (k > 0 ? "\n" : "") << juce::String (k + 1) << ". " << slots[k];
                r.detail = r.detail.isNotEmpty() ? lines + "\n" + r.detail : lines;
            }
        }
        if (askHere)
        {
            const auto& pending = c.pendingAsk();
            // ---- THE QUESTION IS ASKED ONCE (18 Oct 2026, Sean in Logic) ----------------------
            // *"The ask question now shows twice: as the step line AND again in the question block
            // below it."*  Since describeCall started preferring the question over the server's
            // bare "ask" summary, the asking step's line IS the question - so the block underneath
            // is a second copy of it. The row wins (it carries the chips), exactly as the no-steps
            // branch below already decided for the same reason.
            // ---- AND ONCE IS ONCE WHEN THE TWO STRINGS DIFFER (22 Oct 2026, his test 8) -----
            //
            // *"the plan is shown TWICE. Once inside the agent step with the Go / Change / Cancel
            //  chips, then again in full as a plain message below the card."*
            //
            // The test above is exact equality on trimmed text, and these two strings stopped being
            // equal: the step's label is composed when the tool_call frame ARRIVES
            // (plainifyLine(describeCall)), while the client then PREPENDS the talk's own text to
            // the question (pendingAsk_.question = text + "\n" + ask.question) so a plan arrives as
            // one sentence pair. Same sentence, two spellings - and the exact test cannot see it.
            //
            // THE RULING IS UNCHANGED (18 Oct): "the row wins, it carries the chips". So:
            //   the label already says the question  -> drop the block;
            //   the question says MORE than the label -> the block's text becomes the ROW's, and the
            //     block goes. Nothing is lost and nothing is said twice, which is what he asked for
            //     ("if it's ours, render it once, inside the step").
            // ---- AND ONCE IS ONCE WITHOUT COMPARING PROSE AT ALL (his test 1, 16:24, 23 Oct) --
            //
            // *"the plan is STILL shown twice, in the card with chips and then in full as plain
            //  text below the card."*  Third day, same two fields - and that session's log settles
            //  where it is NOT. B's contract holds exactly as written: the plan card's tool_call
            //  carries the text and the message frame for it has EMPTY text and only `of` pointing
            //  at the card, which this client logs as "frame unknown event=message skipped". The
            //  plan's text appears in six log lines all session, three per card - the frame, the
            //  step and the ask - so nothing arrived twice and nothing was flushed to the chat.
            //
            // THE TEST ITSELF WAS THE FAULT, and it was a PROXY: prose standing in for identity.
            // The row's label is composed from the call when its frame arrives; the question is the
            // ask's own; they reach this view by different roads and in that session they differed
            // by ONE character - "4. bx clipper" on the row, "4. bx_clipper" in the question.
            // Containment after normalising dashes and spaces cannot see a difference in the MIDDLE
            // of a string, so both copies were kept. Normalising underscores too would buy a day:
            // any wording the two roads do not share breaks it again.
            //
            // IDENTITY CANNOT DRIFT. `askHere` means this step IS the pending ask (its id is the
            // ask's id), so the row and the block are two drawings of ONE thing - there is nothing
            // to compare. The block goes, always, and the row keeps the chips, which is his 18 Oct
            // ruling unchanged ("the row wins, it carries the chips").
            //
            // WHICH TEXT the row shows is then a presentation choice and not a test, and it is made
            // so that nothing can be LOST: the longer of the two, because one is sometimes the
            // fuller sentence (the talk's text prepended to the ask) and the other is sometimes the
            // more canonical spelling. A tie keeps the label, which is what the row already had.
            if (v.askQuestion.isNotEmpty())
            {
                if (v.askQuestion.trim().length() > r.label.trim().length())
                    r.label = v.askQuestion;     // the fuller sentence, on the row with the chips
                v.askQuestion.clear();           // one ask, one drawing of it, by construction
            }
            r.chips       = pending.labels;
            r.chipNotes   = pending.details;     // "the chips don't show their notes" (18 Oct)
            r.chipHandSet = pending.handSet;
        }
        r.right       = rightWordFor (s, askHere);
        if (s.status == St::Done || s.status == St::Undone) ++v.changeCount;
        const auto addedHere = echojay::agent::addedPluginName (s.call);
        v.steps.push_back (std::move (r));
        // ---- AND A DIAL'S STEP SITS WITH ITS ADD (his test 3 at 16:40, 23 Oct 2026) ------------
        //
        // *"On the card, each dial is its own step right after its add, running while I play, and it
        //  shows its result on that step."*
        //
        // The dial steps used to be appended after every other step, so on a four-plugin build the
        // dials sat in a block at the bottom and the card read as "four adds, then two dials" rather
        // than as the order the work happens in. The run's own lists still decide WHETHER there is a
        // dial (keepPlayingFor and the landed results, which is the 08:43 road); this only decides
        // where it is drawn, and the drawing follows the add it belongs to.
        if (addedHere.isNotEmpty())
            pushDialStepFor (addedHere, c, v, drawnDials);
    }
    // ---- B'S KEEP-PLAYING CONTRACT, POINT 2: THE DIAL IS ITS OWN STEP (22 Oct 2026) -----------
    //
    // *"Show the dial as its own step on the run card, with the plugin's name, until the dial lands
    //  or gives up"* (docs/CONTRACT_keep_playing_2026-10-22.md, and his overnight item 4).
    //
    // His test 9: the plan's second compressor was "set on the first playback for about 1 dB" and the
    // card showed NO step for that dial - so the only sign of it was the listen refusing to end, and
    // a card that holds him on playback without saying what for is a card that looks stuck. Point 1
    // (never say "you can stop") is in the client; this is the line he reads while it happens.
    //
    // THE STEP COMES FROM THE EXECUTOR'S OWN ANSWER - the same question point 1 asks before it
    // holds the stop prompt - so the step and the prompt cannot disagree about whether a dial is
    // still going. It is drawn while one is pending and gone when it lands, which is what "until the
    // dial lands or gives up" means; the step that ADDED the plugin keeps its own line either way.
    // The executor names ONE plugin: the first slot of this run that still has a dial pending. If
    // two dialled at once the card would name them one after the other rather than side by side,
    // which is a limit of the accessor and not of this drawing - and it is the honest limit to have,
    // because the alternative is this view keeping its own list of what is dialling (two sources of
    // truth for one fact, which is the family three of this week's faults came from).
    // It is pushed LAST, so the number it carries is one past the steps that did the work.
    //
    // ---- AND IT FINISHES WITH ITS RESULT (his test 4 at 08:43, 23 Oct 2026) ------------------
    //
    // *"the dial-on-playback result arrived as a loose message below the card ... the dial step
    //  finishes and shows its result, then the card says it's done. Nothing appears outside the
    //  card."*  So the step does NOT vanish when the dial lands: it becomes a Done step carrying
    //  the sentence the dial itself wrote. The landed ones are drawn first, in the order the run
    //  armed them, and the one still working last - which is the order he reads them in.
    // ...AND ANY DIAL WHOSE ADD IS NOT ON THIS CARD still gets its step here, at the end. A dial can
    // outlive the step that armed it (a run that was reloaded from the store, or a dial armed by a
    // build whose own step the server never sent), and a dial nobody can see is the fault his 16:40
    // ruling is about. Only the ones no add claimed, so nothing is drawn twice.
    // HELD IN A LOCAL, NOT ITERATED OFF THE CALL. dialResults() returns by value and getAllKeys()
    // returns a reference INTO it, so `for (auto& n : c.dialResults().getAllKeys())` walks a list
    // whose owner died at the end of that expression - which is exactly what it did: KP5 went from
    // green to "no dial step" the moment I wrote it that way.
    const auto landedNow = c.dialResults();
    for (const auto& name : landedNow.getAllKeys())
        pushDialStepFor (name, c, v, drawnDials);
    if (const auto dialling = c.keepPlayingPlugin(); dialling.isNotEmpty())
        pushDialStepFor (dialling, c, v, drawnDials);
    if (askNow && c.steps().empty())
    {
        RunStep r;
        r.id     = "ask";
        r.label  = c.pendingAsk().question.isNotEmpty() ? c.pendingAsk().question : juce::String ("a question");
        r.chips       = c.pendingAsk().labels;
        r.chipNotes   = c.pendingAsk().details;
        r.chipHandSet = c.pendingAsk().handSet;
        r.right  = "your answer?";
        r.status = (int) St::AwaitingApproval;
        v.steps.push_back (std::move (r));
        v.askQuestion.clear();        // the row IS the question; two copies would be two authorities
    }
    // ---- SOMETHING ON THE ROW THE MOMENT IT EXISTS (18 Oct 2026, Sean in Logic) ----------------
    //
    // *"I press Enter on the Kick's Link and see only a blank 'E' avatar for several seconds before
    // the answer appears. 'Thinking...' must show the moment I send."* Item 9 (17 Oct) put that line
    // in the stage row to cover the gap between Send and the first frame - and the row cleared it
    // immediately, because EJAgentClient::post() sets Connecting and calls changed() INSIDE start().
    // AgentRunBridge::agentChanged then handed over to a row that had nothing to hand over to: no
    // sessionId (the server has not answered), no steps, no closing - so empty() was true, layoutRun
    // returned a zero height, and the only thing drawn was the assistant avatar of a message with no
    // content. The handover was right; the row was not ready to receive it.
    //
    // A live run with nothing to show yet is not an empty run: it is a run WAITING ON THE SERVER,
    // which is a step like any other. So it gets one line, carrying the same word item 9 chose and
    // the pulsing glyph the mockup already uses for work in progress. It costs no new paint code, it
    // makes empty() false so the card has a height, and it goes away by itself the moment a real
    // step arrives - this block cannot outlive the thing it stands in for.
    // ---- AND UNDER THE LATEST STEP, EVERY TIME (his 14:08 ruling, 21 Oct 2026) ---------------
    //
    // *"While an agent run is working, a "Thinking..." line always sits under the latest step, from
    // the moment that step ticks until the next step appears, every time, not only at the start.
    // Today "Thinking..." shows before the first step, then disappears once a step completes, so the
    // card looks idle while it waits on the server (e.g. "read the rack" ticked, then nothing but
    // "Working" top right). ... The line goes when the run is waiting on Sean (an ask with buttons)
    // or has finished."*
    //
    // WHAT WAS WRONG: the condition was `v.steps.empty()`, so the line stood in for the FIRST step
    // and nothing else. Every later wait on the server - and a round trip is most of a run's elapsed
    // time - left a card of ticked lines with nothing moving on it. "Working" top right is true but
    // it is the same word whether the server is thinking or the run is stuck, which is the whole of
    // what he could not tell.
    //
    // THE THREE CONDITIONS, and each is one of his clauses:
    //   - the run is WORKING (c.isActive()), so a finished, stopped or failed run has no line;
    //   - it is NOT waiting on him: askNow covers both shapes of that - an ask riding the last step
    //     and an ask with no steps yet - and the ask's own buttons are what he is looking at;
    //   - the latest step has SETTLED. A step still Running is itself the indicator, and a second
    //     moving line under it would be two things claiming the same work. "From the moment that
    //     step ticks" is exactly this test.
    //
    // It is still the last row, it still carries the pulsing Running glyph, and it still cannot
    // outlive what it stands in for: the next step pushes it down the moment it arrives.
    else if (v.running && ! askNow && lastRowHasSettled (v.steps))
    {
        RunStep r;
        r.id     = "thinking";
        r.label  = juce::String::fromUTF8 ("Thinking\xe2\x80\xa6");
        r.status = (int) St::Running;
        // B'S LATEST WORD ABOUT THE WAIT, under the line and in B's own words (his 15:28). It rides
        // the same row as `detail`, which is the muted second line the row already draws - so this
        // is one field and no new paint code, and it disappears with the row it is under.
        r.detail = c.serverStatusLine().trim();
        v.statusText = r.detail;
        v.steps.push_back (std::move (r));
    }
    // ---- SEAN'S #18 (16 Oct 2026): A READ IS NOT A CHANGE --------------------------------------
    // *"A read-only run (a GR question) shows '4 changes'. Read-only steps aren't changes: show
    // 'N steps', or no count if nothing changed."* Undone steps are not changes either - counting
    // them would make "3 changes" outlive the undo - and nor is a look, a check or a question. The
    // count is the number of `do` steps that landed and have not been put back; `stepCount` is what
    // the control falls back to naming when that is zero.
    v.changeCount = 0;
    // THE PHANTOM ROW IS NOT A STEP (his 14:08 ruling, 21 Oct 2026). "Thinking..." is a stand-in for
    // work that has not reported yet, and now that it sits under EVERY latest step it would inflate
    // the count on every round trip - "4 steps" for three, the whole time the server is thinking.
    // Counted out by id, where it is pushed, rather than by subtracting one somewhere later.
    v.stepCount   = (int) v.steps.size();
    for (const auto& s : v.steps) if (s.id == "thinking") --v.stepCount;
    int i2 = -1;
    for (const auto& s : v.steps)
    {
        ++i2;
        if (s.status != (int) St::Done) continue;
        const bool wasDo = i2 < (int) c.steps().size() && c.steps()[(size_t) i2].isDo();
        if (wasDo) ++v.changeCount;
    }
    // ---- SEAN'S #14 (16 Oct 2026): AFTER AN UNDO, THE CLOSING CANNOT STILL CLAIM THE CHANGE ----
    // *"after 'Everything the agent changed has been undone' the run still shows 'Pre gain on the Mix
    // Bus is set to 0 dB', a stale sentence."* The closing is the server's done summary, written
    // before the undo existed; once every change has been put back it describes a state that is no
    // longer true. The sentence is REPLACED rather than edited, because nothing here can know what
    // the state is now - only that what the closing says about it has been undone.
    {
        bool anyUndone = false;
        for (const auto& s : v.steps) if (s.status == (int) St::Undone) anyUndone = true;
        if (anyUndone && v.changeCount == 0 && v.closing.isNotEmpty())
        {
            v.closingBeforeUndo = v.closing;
            v.closing = "Everything this run changed has been undone, so what it said above no longer "
                        "describes the rack.";
        }
    }
    return v;
}

// ---- model <-> JSON (the snapshot on the message) ---------------------------------
inline juce::var toJson (const RunView& v)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("sessionId", v.sessionId);
    o->setProperty ("runKey",    v.runKey);
    o->setProperty ("opening",   v.opening);
    o->setProperty ("closing",   v.closing);
    o->setProperty ("status",    v.statusWord);
    o->setProperty ("mode",      v.modeBadge);
    // ---- THE NOTICE IS NOT PART OF THE RECORD (Sean's 19:5x item 1, 20 Oct 2026) ------------
    //
    // *"The "agent is still working on build a mix bus" notice from his earlier blocked send is
    // still on this run's card, in orange, above the steps. A notice belongs to the send that
    // triggered it, in that chat, and goes away once it's been seen."*
    //
    // It is PER-SEND state - a refused message, an undo-all outcome - and serialising it made it
    // part of the run's permanent snapshot: written into agentData while it was up, then redrawn by
    // every later render of that card, including in a chat he had moved on from. start() clears the
    // live one, which is why the field looked harmless; the copy in the record outlived it.
    //
    // So it is NOT written. A card restored from its record has no notice, which is right: a notice
    // about a send is meaningless once that send is history. The LIVE one still shows, because
    // runViewFrom copies it from the client on every render.
    //
    // (The reader below still accepts it, so a snapshot written by an older build loses the line
    // rather than failing to parse.)
    o->setProperty ("ask",       v.askQuestion);
    if (v.askTypeHint.isNotEmpty()) o->setProperty ("askHint", v.askTypeHint);
    if (v.failuresOnly)             o->setProperty ("failuresOnly", true);
    o->setProperty ("running",   v.running);
    if (v.statusText.isNotEmpty()) o->setProperty ("statusText", v.statusText);   // his 15:28
    o->setProperty ("finished",  v.finished);
    o->setProperty ("stepsOpen", v.stepsOpen);
    o->setProperty ("applyAll",  v.showApplyAll);
    o->setProperty ("stop",      v.showStop);
    o->setProperty ("undoAll",   v.showUndoAll);
    o->setProperty ("gotIt",     v.showGotIt);
    o->setProperty ("canStop",   v.canStopPlaying);
    o->setProperty ("retry",     v.showRetry);
    o->setProperty ("onePlanLine", v.onePlanLine);
    o->setProperty ("changes",   v.changeCount);
    if (v.stepCount > 0)                  o->setProperty ("stepCount", v.stepCount);
    if (v.closingBeforeUndo.isNotEmpty()) o->setProperty ("closingBeforeUndo", v.closingBeforeUndo);
    o->setProperty ("state",     v.state);
    if (v.playbackActive)
    {
        o->setProperty ("playbackLine", v.playbackLine);
        o->setProperty ("playbackFrac", v.playbackFrac);
        o->setProperty ("playbackReached", v.playbackReached);
    }
    juce::Array<juce::var> steps;
    for (const auto& s : v.steps)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty ("id", s.id);
        so->setProperty ("label", s.label);
        if (s.detail.isNotEmpty())      so->setProperty ("detail", s.detail);
        if (s.modelDetail.isNotEmpty()) so->setProperty ("modelDetail", s.modelDetail);
        if (s.right.isNotEmpty())       so->setProperty ("right", s.right);
        if (s.askReason.isNotEmpty())   so->setProperty ("askReason", s.askReason);
        if (s.answered.isNotEmpty())    so->setProperty ("answered", s.answered);
        if (! s.chips.isEmpty())        so->setProperty ("chips", s.chips.joinIntoString ("\n"));
        // Additive, and read back positionally: a build that does not know these fields reads the
        // chips exactly as it did before, and this one pads a short list rather than mis-pairing it.
        if (! s.chipNotes.isEmpty())    so->setProperty ("chipNotes", s.chipNotes.joinIntoString ("\n"));
        if (! s.chipHandSet.isEmpty())
        {
            juce::String hand;
            for (int k = 0; k < s.chipHandSet.size(); ++k) hand << (s.chipHandSet[k] ? '1' : '0');
            so->setProperty ("chipHand", hand);
        }
        so->setProperty ("status", statusToken (s.status));
        if (s.applySkip)   so->setProperty ("applySkip", true);
        if (s.buildPlan)   so->setProperty ("buildPlan", true);
        if (s.canUndo)     so->setProperty ("canUndo", true);
        if (s.hasDetails)  so->setProperty ("hasDetails", true);
        if (s.expanded)    so->setProperty ("expanded", true);
        if (s.autoApplied) so->setProperty ("autoApplied", true);
        steps.add (juce::var (so));
    }
    o->setProperty ("steps", steps);
    return juce::var (o);
}

inline RunView fromJson (const juce::var& v)
{
    RunView r;
    auto* o = v.getDynamicObject();
    if (o == nullptr) return r;
    r.sessionId  = o->getProperty ("sessionId").toString();
    r.runKey     = o->getProperty ("runKey").toString();
    r.opening    = o->getProperty ("opening").toString();
    r.closing    = o->getProperty ("closing").toString();
    r.statusWord = o->getProperty ("status").toString();
    r.modeBadge  = o->getProperty ("mode").toString();
    r.notice     = o->getProperty ("notice").toString();
    r.askQuestion= o->getProperty ("ask").toString();
    r.askTypeHint= o->getProperty ("askHint").toString();
    r.failuresOnly = (bool) o->getProperty ("failuresOnly");
    r.running    = (bool) o->getProperty ("running");
    r.statusText = o->getProperty ("statusText").toString();
    r.finished   = (bool) o->getProperty ("finished");
    r.stepsOpen  = o->hasProperty ("stepsOpen") ? (bool) o->getProperty ("stepsOpen") : true;
    r.showApplyAll = (bool) o->getProperty ("applyAll");
    r.showStop   = (bool) o->getProperty ("stop");
    r.showUndoAll= (bool) o->getProperty ("undoAll");
    r.showGotIt  = (bool) o->getProperty ("gotIt");
    r.canStopPlaying = (bool) o->getProperty ("canStop");
    r.showRetry  = (bool) o->getProperty ("retry");
    r.onePlanLine= (bool) o->getProperty ("onePlanLine");
    r.changeCount= (int)  o->getProperty ("changes");
    r.stepCount  = (int)  o->getProperty ("stepCount");
    r.closingBeforeUndo = o->getProperty ("closingBeforeUndo").toString();
    r.state      = (int)  o->getProperty ("state");
    if (o->hasProperty ("playbackLine"))
    {
        r.playbackActive  = true;
        r.playbackLine    = o->getProperty ("playbackLine").toString();
        r.playbackFrac    = (double) o->getProperty ("playbackFrac");
        r.playbackReached = (bool) o->getProperty ("playbackReached");
    }
    if (auto* arr = o->getProperty ("steps").getArray())
        for (const auto& sv : *arr)
            if (auto* so = sv.getDynamicObject())
            {
                RunStep s;
                s.id          = so->getProperty ("id").toString();
                s.label       = so->getProperty ("label").toString();
                s.detail      = so->getProperty ("detail").toString();
                s.modelDetail = so->getProperty ("modelDetail").toString();
                s.right       = so->getProperty ("right").toString();
                s.askReason   = so->getProperty ("askReason").toString();
                s.answered    = so->getProperty ("answered").toString();
                const auto chips = so->getProperty ("chips").toString();
                if (chips.isNotEmpty()) s.chips.addLines (chips);
                const auto notes = so->getProperty ("chipNotes").toString();
                if (notes.isNotEmpty()) s.chipNotes.addLines (notes);
                while (s.chipNotes.size() < s.chips.size()) s.chipNotes.add ({});
                const auto hand = so->getProperty ("chipHand").toString();
                for (int k = 0; k < s.chips.size(); ++k)
                    s.chipHandSet.add (k < hand.length() && hand[k] == '1');
                s.status      = statusFromToken (so->getProperty ("status").toString());
                s.applySkip   = (bool) so->getProperty ("applySkip");
                s.buildPlan   = (bool) so->getProperty ("buildPlan");
                s.canUndo     = (bool) so->getProperty ("canUndo");
                s.hasDetails  = (bool) so->getProperty ("hasDetails");
                s.expanded    = (bool) so->getProperty ("expanded");
                s.autoApplied = (bool) so->getProperty ("autoApplied");
                r.steps.push_back (std::move (s));
            }
    return r;
}

// ---- geometry -----------------------------------------------------------------------
/** Every clickable thing the row draws. The editor turns these into its painted
    click zones; `stepIndex` is -1 for the row-wide controls. ONE list, so a
    button that is drawn is a button that can be pressed - the dead-Details
    button of item 3 was exactly a drawn control with no reachable handler. */
enum class RunHit
{
    None = 0, ApplyLine, SkipLine, Details, Chip, ApplyAll, SkipAll, Stop, UndoAll, GotIt, Retry, ToggleSteps, Undo
};

struct RunZone
{
    juce::Rectangle<int> rect;
    RunHit kind = RunHit::None;
    int    stepIndex = -1;
    juce::String label;      // a chip's label; "" otherwise
    juce::String note;       // a chip's note, drawn under its name; "" otherwise
    bool handSet = false;    // a chip the user sets by hand: its note is tinted
};

struct RunRects
{
    int totalHeight = 0;
    juce::Rectangle<int> header, statusText, modeBadge, closing, changesBtn;
    juce::Rectangle<int> notice, askQuestion, playbackRing, playbackLine;
    std::vector<RowRects> rows;             // one per LAID row, in order (empty when fully collapsed)
    /** Which step each laid row belongs to. The rows were one-to-one with the steps until the
        failures-only collapse (Sean's #13) laid a SUBSET, at which point an index into one was no
        longer an index into the other - the class of fault that moved Details onto the wrong line
        on 13 Oct. Every reader goes through this map. */
    std::vector<int>      rowStep;
    juce::Rectangle<int>  askHint;
    std::vector<RunZone>  zones;
};

namespace runmetrics
{
    static constexpr int headerH   = 20;
    static constexpr int gap       = 6;
    static constexpr int lineBtnW  = 52;    // Apply / Skip on a line
    static constexpr int rightW    = 76;    // the right-hand status word column
    /** The narrowest card width at which the right-hand word keeps its own column (his 17:3x item
        1). Below it the word moves to the row's second line and the step text takes the full width -
        240 px leaves a step line about 150 px beside a 76 px column plus the glyph, which is where
        the squeeze he photographed begins. */
    static constexpr int kRightWordMinWidth = 240;
    static constexpr int detailsW  = 58;
    static constexpr int footerH   = 30;
    static constexpr int changesH  = 24;
}

/** Lays the whole run row out from (x0, y0) across `width`. Pure over the
    model: the chat's measure pass, its paint pass and a guard all call this. */
inline RunRects layoutRun (const RunView& v, int x0, int y0, int width)
{
    using namespace runmetrics;
    RunRects L;
    if (width <= 0 || v.empty()) return L;
    int y = y0;

    // ---- header: EchoJay + the mode badge on the left, the status on the right ----
    L.header     = { x0, y, width, headerH };
    const int statusW = juce::jmin (width / 2, juce::jmax (40, chipFont().getStringWidth (v.statusWord) + 6));
    L.statusText = v.statusWord.isNotEmpty() ? juce::Rectangle<int> (x0 + width - statusW, y, statusW, headerH)
                                             : juce::Rectangle<int>();
    const int badgeW = v.modeBadge.isNotEmpty() ? detailFont().getStringWidth (v.modeBadge) + 10 : 0;
    L.modeBadge  = badgeW > 0 ? juce::Rectangle<int> (x0, y + 3, badgeW, headerH - 6) : juce::Rectangle<int>();
    y += headerH + 2;

    if (v.notice.isNotEmpty())
    {
        const int h = wrappedTextHeight (v.notice, detailFont(), width);
        L.notice = { x0, y, width, h };
        y += h + gap;
    }

    // ---- the steps ------------------------------------------------------------------
    if (v.stepsOpen || v.failuresOnly)
    {
        std::vector<RowSpec> specs;
        std::vector<int> kinds;                  // 0 none, 1 right word, 2 Apply/Skip, 3 Details, 4 Undo
        specs.reserve (v.steps.size());
        for (size_t si = 0; si < v.steps.size(); ++si)
        {
            const auto& s = v.steps[si];
            // the collapsed-with-a-failure state shows the failed lines and nothing else
            if (! v.stepsOpen && s.status != (int) EJAgentClient::Step::Status::Failed) continue;
            L.rowStep.push_back ((int) si);
            RowSpec r;
            r.label  = s.label;
            r.detail = s.detail;
            if (s.askReason.isNotEmpty())
                r.detail = r.detail.isNotEmpty() ? (r.detail + "\n" + s.askReason) : s.askReason;
            if (s.answered.isNotEmpty())
                r.detail = r.detail.isNotEmpty() ? (r.detail + "\n" + s.answered) : s.answered;
            if (s.expanded && s.modelDetail.isNotEmpty() && s.modelDetail != s.detail)
                r.detail = r.detail.isNotEmpty() ? (r.detail + "\n" + s.modelDetail) : s.modelDetail;
            int kind = 0;
            if (s.applySkip)                      { r.rightButtonW = lineBtnW * 2 + Metrics::btnGap; kind = 2; }
            else if (s.canUndo)                   { r.rightButtonW = Metrics::undoW;  kind = 4; }
            else if (s.hasDetails)                { r.rightButtonW = detailsW;        kind = 3; }
            else if (s.right.isNotEmpty())
            {
                // ---- THE WORD GOES UNDER THE LINE WHEN THE WINDOW IS NARROW (his 17:3x item 1) --
                //
                // *"the "your answer?" label on the right of a waiting step takes a whole column, so
                // in a narrow window the step text is squeezed into a thin strip on the left ... Put
                // that label on its own line, or drop it when the window is narrow; the step text
                // must always use the card's width."*
                //
                // Above the threshold it stays in its column, which is where the mockup puts it.
                // Below it the column is NOT reserved - the step text takes the whole width - and the
                // word becomes the row's own second line, where it reads as what it is. The threshold
                // is the narrowest width at which a step line still has room for a few words beside a
                // 76 px column; under that the column was taking more than half of some rows.
                if (width >= kRightWordMinWidth) { r.rightButtonW = rightW; kind = 1; }
                else
                {
                    r.detail = r.detail.isNotEmpty() ? (r.detail + "\n" + s.right) : s.right;
                    kind = 0;
                }
            }
            specs.push_back (r);
            kinds.push_back (kind);
        }
        if (! specs.empty())
        {
            y += layoutRows (specs, x0, y, width, L.rows);
            // the chips of a question row sit UNDER that row, indented past the glyph column
            for (size_t i = 0; i < L.rows.size() && i < L.rowStep.size(); ++i)
            {
                const int kind = kinds[i];
                const int si   = L.rowStep[i];
                const auto& rr = L.rows[i];
                // A RECORD KEEPS DETAILS (a question about the picture) and loses the actions.
                if (v.fromRecord && (kind == 2 || kind == 4)) continue;
                if (kind == 2)
                {
                    const int bw = lineBtnW;
                    // A BUILD PLAN SAYS GO AND CANCEL (his 21:52). The ZONE carries the word, so the
                    // painter and the rig read the same label from the same place - a second switch
                    // in the painter would be two authors for one button.
                    const bool plan = v.steps[(size_t) si].buildPlan;
                    L.zones.push_back ({ { rr.button.getX(), rr.button.getY(), bw, rr.button.getHeight() },
                                         RunHit::ApplyLine, si, plan ? "Go" : "Apply" });
                    L.zones.push_back ({ { rr.button.getX() + bw + Metrics::btnGap, rr.button.getY(), bw, rr.button.getHeight() },
                                         RunHit::SkipLine, si, plan ? "Cancel" : "Skip" });
                }
                else if (kind == 3) L.zones.push_back ({ rr.button, RunHit::Details, si, {} });
                else if (kind == 4) L.zones.push_back ({ rr.button, RunHit::Undo,    si, {} });
            }
            // chips: laid after the rows so their y is below everything the rows claimed
            for (size_t i = 0; i < L.rowStep.size(); ++i)
            {
                const int si = L.rowStep[i];
                const auto& st = v.steps[(size_t) si];
                if (v.fromRecord) continue;      // a record's chips are history, not buttons
                if (st.chips.isEmpty() || ! st.answered.isEmpty()) continue;
                const int cx = x0 + Metrics::glyphW + Metrics::glyphGap;
                std::vector<juce::Rectangle<int>> chipRects;
                const int h = layoutChips (st.chips, st.chipNotes, cx, y,
                                           juce::jmax (1, width - (cx - x0)), chipRects);
                for (size_t k = 0; k < chipRects.size(); ++k)
                {
                    const int ki = (int) k;
                    L.zones.push_back ({ chipRects[k], RunHit::Chip, si, st.chips[ki],
                                         ki < st.chipNotes.size() ? st.chipNotes[ki] : juce::String(),
                                         ki < st.chipHandSet.size() && st.chipHandSet[ki] });
                }
                y += h;
            }
            y += gap;
        }
    }

    if (v.askQuestion.isNotEmpty())
    {
        const int h = wrappedTextHeight (v.askQuestion, bodyFont(), width);
        L.askQuestion = { x0, y, width, h };
        y += h + gap;
    }
    if (v.askTypeHint.isNotEmpty())
    {
        const int h = wrappedTextHeight (v.askTypeHint, detailFont(), width);
        L.askHint = { x0, y, width, h };
        y += h + gap;
    }

    if (v.playbackActive)
    {
        const int lineW = juce::jmax (1, width - Metrics::indicatorW - Metrics::glyphGap);
        const int lineH = juce::jmax (Metrics::indicatorW, wrappedTextHeight (v.playbackLine, bodyFont(), lineW));
        L.playbackRing = { x0, y, Metrics::indicatorW, Metrics::indicatorW };
        L.playbackLine = { x0 + Metrics::indicatorW + Metrics::glyphGap, y, lineW, lineH };
        y += lineH + gap;
    }

    // ---- the closing sentence, and what sits under it -------------------------------
    if (v.closing.isNotEmpty())
    {
        const int h = wrappedTextHeight (v.closing, bodyFont(), width);
        L.closing = { x0, y, width, h };
        y += h + gap;
    }

    // ---- the row's own controls ------------------------------------------------------
    juce::StringArray footer;
    std::vector<RunHit> footerKinds;
    // ---- THE COLLAPSE CONTROL (Sean's #2 and #13) ---------------------------------------------
    // It was gated on changeCount > 0, so a run that failed or was stopped without landing anything
    // had no control at all - and a run that HAD landed something but failed a line had one that
    // could not act (the view forced the steps open). Both were "the button does nothing". Now:
    // any finished run with steps can be collapsed, and the label says what pressing it will do.
    if (v.finished && ! v.steps.empty())
    {
        // "N changes" when it changed something; "N steps" when it only read (Sean's #18).
        const auto label = v.changeCount > 0
                               ? juce::String (v.changeCount) + (v.changeCount == 1 ? " change" : " changes")
                               : v.stepCount > 0
                                     ? juce::String (v.stepCount) + (v.stepCount == 1 ? " step" : " steps")
                                     : juce::String ("Show steps");
        const int w = juce::jmax (Metrics::chipMinW, chipFont().getStringWidth (juce::String ("Hide steps")) + 30);
        L.changesBtn = { x0, y, w, changesH };
        L.zones.push_back ({ L.changesBtn, RunHit::ToggleSteps, -1, v.stepsOpen ? "Hide steps" : label });
    }
    if (v.showApplyAll)
    {
        if (! v.onePlanLine) { footer.add ("Apply all");  footerKinds.push_back (RunHit::ApplyAll); }
        else                 { footer.add ("Apply");      footerKinds.push_back (RunHit::ApplyAll); }
        footer.add ("Skip all"); footerKinds.push_back (RunHit::SkipAll);
    }
    if (v.showGotIt)  { footer.add (v.canStopPlaying ? "Got it, you can stop" : "Got it");
                        footerKinds.push_back (RunHit::GotIt); }
    if (v.showStop)   { footer.add ("Stop");     footerKinds.push_back (RunHit::Stop); }
    if (v.showRetry)  { footer.add ("Retry");    footerKinds.push_back (RunHit::Retry); }
    if (v.showUndoAll){ footer.add ("Undo all"); footerKinds.push_back (RunHit::UndoAll); }

    if (! footer.isEmpty())
    {
        // right-aligned, natural widths, shrinking evenly rather than leaving the row
        const auto f = chipFont();
        std::vector<int> widths;
        int total = 0;
        for (const auto& lab : footer) { const int w = juce::jmax (Metrics::chipMinW, f.getStringWidth (lab) + 24); widths.push_back (w); total += w; }
        total += Metrics::btnGap * ((int) widths.size() - 1);
        const int changesW = L.changesBtn.isEmpty() ? 0 : L.changesBtn.getWidth() + Metrics::btnGap;
        const int avail = juce::jmax (Metrics::chipMinW, width - changesW);
        if (total > avail && ! widths.empty())
        {
            const int each = juce::jmax (24, (avail - Metrics::btnGap * ((int) widths.size() - 1)) / (int) widths.size());
            for (auto& w : widths) w = each;
        }
        const int by = L.changesBtn.isEmpty() ? y : L.changesBtn.getY();
        int x = x0 + width;
        for (int i = (int) widths.size() - 1; i >= 0; --i)
        {
            x -= widths[(size_t) i];
            L.zones.push_back ({ { x, by, widths[(size_t) i], Metrics::btnH }, footerKinds[(size_t) i], -1, footer[i] });
            x -= Metrics::btnGap;
        }
        if (L.changesBtn.isEmpty()) y += Metrics::btnH + gap;
    }
    if (! L.changesBtn.isEmpty()) y += changesH + gap;

    L.totalHeight = juce::jmax (0, y - y0);
    return L;
}

/** The one place a step's glyph is drawn, so the transcript row and the
    (retiring) floating panel cannot disagree about what a tick looks like.
    `phase` animates the spinner; pass 0 for a still frame. */
inline void drawRunGlyph (juce::Graphics& g, juce::Rectangle<int> r, int status, float phase,
                          juce::Colour dim, juce::Colour amber, juce::Colour blue,
                          juce::Colour green, juce::Colour red)
{
    using St = EJAgentClient::Step::Status;
    const auto c = r.toFloat().withSizeKeepingCentre (12.0f, 12.0f);
    switch ((St) status)
    {
        case St::Pending:          g.setColour (dim);   g.drawEllipse (c, 1.2f); break;
        case St::AwaitingApproval: g.setColour (amber); g.drawEllipse (c, 1.2f); break;
        case St::Running:
        {
            juce::Path p;
            p.addCentredArc (c.getCentreX(), c.getCentreY(), 6.0f, 6.0f, 0.0f, phase,
                             phase + juce::MathConstants<float>::pi * 1.3f, true);
            g.setColour (blue);
            g.strokePath (p, juce::PathStrokeType (1.6f));
            break;
        }
        case St::Done:
        {
            juce::Path p;
            p.startNewSubPath (c.getX() + 1.5f, c.getCentreY() + 0.5f);
            p.lineTo (c.getX() + 5.0f, c.getBottom() - 1.5f);
            p.lineTo (c.getRight() - 1.0f, c.getY() + 1.5f);
            g.setColour (green);
            g.strokePath (p, juce::PathStrokeType (1.8f));
            break;
        }
        case St::Failed:
            g.setColour (red);
            g.drawLine (c.getX() + 2.0f, c.getY() + 2.0f, c.getRight() - 2.0f, c.getBottom() - 2.0f, 1.6f);
            g.drawLine (c.getRight() - 2.0f, c.getY() + 2.0f, c.getX() + 2.0f, c.getBottom() - 2.0f, 1.6f);
            break;
        case St::Declined: case St::NotRun:
            g.setColour (dim);
            g.drawLine (c.getX() + 2.0f, c.getCentreY(), c.getRight() - 2.0f, c.getCentreY(), 1.6f);
            break;
        case St::Undone:
            g.setColour (amber);
            g.drawEllipse (c, 1.2f);
            g.drawLine (c.getX() + 3.0f, c.getCentreY(), c.getRight() - 3.0f, c.getCentreY(), 1.2f);
            break;
        case St::Stopped:
            g.setColour (dim);
            g.fillRect (c.reduced (2.5f));
            break;
    }
}

/** Carry what the user ANSWERED from the previous snapshot of this run onto the rebuilt one.

    18 Oct 2026, Sean in Logic: *"On a second ask, 'You said: cl1b' appears under the NEW pending ask
    before I've answered it."*  Both callers - the window's row and the home's record - matched the two
    snapshots BY STEP ID, and his log has the server using one id for two asks in the same run:

        12:05:11  frame tool_call id=tc_pick_look_ask name=talk ... "ask"
        12:06:38  frame tool_call id=tc_pick_look_ask name=talk ... "ask"

    So the second ask found the first one's record and inherited its answer. An id the server reuses is
    not an identity; the POSITION in an append-only list is. Both are required - same index AND same id
    - so a list that ever did shift carries nothing rather than the wrong thing.

    It is one function because it was two copies of one rule, in two files, and a rule in two places is
    a rule that gets fixed in one. */
/** ---- THE SAME ANSWER, WRITTEN TWO WAYS (18 Oct 2026, his 15:0x regression) ----------------
    *"the closing is now saved twice, once with an em dash and once with a hyphen"*.  The record's
    write matched on the exact text, and the stored copy had come back from the server's prose while
    the new one had been through ours, so an em dash against a hyphen was enough to miss it and
    append a second copy.

    So the record compares NORMALISED text: every kind of dash reads as one, runs of whitespace read
    as one space, and the ends are trimmed. Those are the two things that differ between one copy of
    a sentence and another here; nothing else is touched, because a comparison that ignored more
    would start merging answers that really are different. */
/** ---- ONE COMPARATOR, AND IT NOW LIVES BESIDE THE PROTOCOL (23 Oct 2026) --------------------
    The body moved to EJAgentProtocol.h so the CLIENT can ask the same question this view asks: his
    test 0 at 08:10 had the plan drawn once on the card and once as the chat row's own prose, and
    the rule that drops the second copy has to be the same rule on both surfaces or they disagree
    again. This forwarder keeps every `agentui::normalisedProse` call site exactly as it was. */
inline juce::String normalisedProse (const juce::String& s) { return echojay::agent::normalisedProse (s); }

inline void carryAnswersForward (RunView& now, const RunView& before)
{
    for (size_t i = 0; i < now.steps.size() && i < before.steps.size(); ++i)
        if (now.steps[i].id == before.steps[i].id
            && now.steps[i].answered.isEmpty() && before.steps[i].answered.isNotEmpty())
            now.steps[i].answered = before.steps[i].answered;
}

/** The invariants a laid-out RUN must satisfy; "" when every rect passes, else the first violation.

    18 Oct 2026. checkRows (EJAgentPanelLayout.h) has covered the STEP rows since the panel existed,
    and nothing has ever checked the rest of the card - so the closing sentence, the notice and the
    ask question could each have been given a rect shorter than their text with no test able to see
    it. Sean read a cut-off closing in Logic and the cause turned out to be the row's WIDTH rather
    than its height, which is the other half of the same blindness: the width the card is laid out at
    must be the width it is drawn into, so a leg passes BOTH and compares them. */
inline juce::String checkRun (const RunView& v, const RunRects& L, int x0, int width)
{
    if (v.empty()) return L.totalHeight == 0 ? juce::String() : "an empty run laid out to a height";
    auto fits = [&] (const juce::Rectangle<int>& r, const juce::String& text, const juce::Font& f,
                     const char* what) -> juce::String
    {
        if (text.isEmpty()) return {};
        if (r.isEmpty()) return juce::String (what) + ": text with no rect";
        if (r.getX() < x0 || r.getRight() > x0 + width) return juce::String (what) + ": leaves the column";
        if (r.getHeight() < wrappedTextHeight (text, f, r.getWidth())) return juce::String (what) + ": truncated";
        return {};
    };
    if (auto why = fits (L.closing,     v.closing,     bodyFont(),   "closing");     why.isNotEmpty()) return why;
    if (auto why = fits (L.askQuestion, v.askQuestion, bodyFont(),   "ask");         why.isNotEmpty()) return why;
    if (auto why = fits (L.notice,      v.notice,      detailFont(), "notice");      why.isNotEmpty()) return why;
    if (auto why = fits (L.askHint,     v.askTypeHint, detailFont(), "ask hint");    why.isNotEmpty()) return why;
    for (const auto& z : L.zones)
    {
        if (z.rect.isEmpty()) continue;
        if (z.rect.getX() < x0 || z.rect.getRight() > x0 + width) return "a control leaves the column";
        // a chip's note, measured through the same two functions that drew it
        if (z.note.isNotEmpty())
        {
            const auto na = chipNoteArea (z.rect, true);
            if (na.isEmpty()) return "a chip's note: text with no rect";
            if (na.getHeight() < wrappedTextHeight (z.note, detailFont(), na.getWidth()))
                return "a chip's note: truncated";
        }
    }
    if (L.totalHeight <= 0) return "a run with content laid out to no height";
    return {};
}

} // namespace echojay::agentui
