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
struct RunStep
{
    juce::String id;
    juce::String label;         // the step in plain words (the server's summary)
    juce::String detail;        // the landed line, or the user's error line
    juce::String modelDetail;   // the technical message, shown only when Details is open
    juce::String right;         // the right-hand status word ("applied", "auto-applied", ...)
    juce::String askReason;     // Auto: why this one still waits. Never invented here.
    juce::StringArray chips;    // a question's answer chips (this step is the question)
    juce::String answered;      // "You said: ..." once a chip is tapped
    int  status = 0;            // EJAgentClient::Step::Status
    bool applySkip = false;     // Ask mode, awaiting a decision -> [Apply] [Skip] on the line
    bool canUndo = false;
    bool hasDetails = false;    // a failed line with a technical message behind it
    bool expanded = false;      // Details open (by step id, so a relayout cannot collapse it)
    bool autoApplied = false;
};

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
    bool finished = false;      // Done | Stopped | Failed
    bool stepsOpen = true;      // false = collapsed to the closing sentence + "N changes"
    bool showApplyAll = false;
    bool showStop = false;
    bool showUndoAll = false;
    bool showGotIt = false;
    bool showRetry = false;
    bool onePlanLine = false;   // item 2: one line means ONE apply, named "Apply"
    int  changeCount = 0;       // how many steps landed (the "N changes" button)
    std::vector<RunStep> steps;

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
        case S::Declined:         return "skipped";
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
        case St::Done:             return s.autoApplied ? "auto-applied" : "applied";
        case St::Failed:           return "not applied";
        case St::Declined:         return "skipped";
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
    v.stepsOpen  = stepsOpen || ! v.finished || anyFailed;
    v.closing    = v.finished ? (c.state() == S::Failed ? c.errorMessage() : c.doneSummary()) : juce::String();
    v.showStop   = v.running;
    v.showUndoAll = v.finished && c.canUndoAll();
    v.showRetry   = c.state() == S::Failed && c.errorRetryable();
    v.showGotIt   = c.state() == S::Listening;
    v.onePlanLine = c.planLines().size() <= 1;
    v.showApplyAll = c.state() == S::AwaitingApproval;

    if (c.askPending())
    {
        const auto ask = c.pendingAsk();
        v.askQuestion = ask.question;
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
    const bool askNow  = c.askPending();
    // The pending ask hangs off the LAST step, which is the tool call that asked it. With no steps at all
    // (a talk-only round that asks straight away) it gets a row of its own so the chips have somewhere to
    // live - a question with no visible chips is how the rig stalled five rows on 15 Oct.
    const int lastIdx = (int) c.steps().size() - 1;
    int i = -1;
    for (const auto& s : c.steps())
    {
        ++i;
        RunStep r;
        r.id          = s.id;
        r.label       = s.label.isNotEmpty() ? s.label : juce::String ("step");
        r.detail      = s.detail;
        r.modelDetail = s.modelDetail;
        r.status      = (int) s.status;
        r.autoApplied = s.autoApplied;
        r.askReason   = s.askReason;
        r.canUndo     = undoNow && s.canUndo();
        r.hasDetails  = s.status == St::Failed && s.modelDetail.isNotEmpty() && s.modelDetail != s.detail;
        r.expanded    = expanded.contains (s.id);
        r.applySkip   = s.status == St::AwaitingApproval && c.state() == S::AwaitingApproval;
        const bool askHere = askNow && i == lastIdx;
        if (askHere) r.chips = c.pendingAsk().labels;
        r.right       = rightWordFor (s, askHere);
        if (s.status == St::Done || s.status == St::Undone) ++v.changeCount;
        v.steps.push_back (std::move (r));
    }
    if (askNow && c.steps().empty())
    {
        RunStep r;
        r.id     = "ask";
        r.label  = c.pendingAsk().question.isNotEmpty() ? c.pendingAsk().question : juce::String ("a question");
        r.chips  = c.pendingAsk().labels;
        r.right  = "your answer?";
        r.status = (int) St::AwaitingApproval;
        v.steps.push_back (std::move (r));
        v.askQuestion.clear();        // the row IS the question; two copies would be two authorities
    }
    // Undone steps are no longer changes to undo. Counting them would make "3 changes" outlive the undo.
    v.changeCount = 0;
    for (const auto& s : v.steps) if (s.status == (int) St::Done) ++v.changeCount;
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
    o->setProperty ("notice",    v.notice);
    o->setProperty ("ask",       v.askQuestion);
    o->setProperty ("running",   v.running);
    o->setProperty ("finished",  v.finished);
    o->setProperty ("stepsOpen", v.stepsOpen);
    o->setProperty ("applyAll",  v.showApplyAll);
    o->setProperty ("stop",      v.showStop);
    o->setProperty ("undoAll",   v.showUndoAll);
    o->setProperty ("gotIt",     v.showGotIt);
    o->setProperty ("retry",     v.showRetry);
    o->setProperty ("onePlanLine", v.onePlanLine);
    o->setProperty ("changes",   v.changeCount);
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
        so->setProperty ("status", statusToken (s.status));
        if (s.applySkip)   so->setProperty ("applySkip", true);
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
    r.running    = (bool) o->getProperty ("running");
    r.finished   = (bool) o->getProperty ("finished");
    r.stepsOpen  = o->hasProperty ("stepsOpen") ? (bool) o->getProperty ("stepsOpen") : true;
    r.showApplyAll = (bool) o->getProperty ("applyAll");
    r.showStop   = (bool) o->getProperty ("stop");
    r.showUndoAll= (bool) o->getProperty ("undoAll");
    r.showGotIt  = (bool) o->getProperty ("gotIt");
    r.showRetry  = (bool) o->getProperty ("retry");
    r.onePlanLine= (bool) o->getProperty ("onePlanLine");
    r.changeCount= (int)  o->getProperty ("changes");
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
                s.status      = statusFromToken (so->getProperty ("status").toString());
                s.applySkip   = (bool) so->getProperty ("applySkip");
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
};

struct RunRects
{
    int totalHeight = 0;
    juce::Rectangle<int> header, statusText, modeBadge, closing, changesBtn;
    juce::Rectangle<int> notice, askQuestion, playbackRing, playbackLine;
    std::vector<RowRects> rows;             // one per step, in order (empty when collapsed)
    std::vector<RunZone>  zones;
};

namespace runmetrics
{
    static constexpr int headerH   = 20;
    static constexpr int gap       = 6;
    static constexpr int lineBtnW  = 52;    // Apply / Skip on a line
    static constexpr int rightW    = 76;    // the right-hand status word column
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
    if (v.stepsOpen)
    {
        std::vector<RowSpec> specs;
        std::vector<int> kinds;                  // 0 none, 1 right word, 2 Apply/Skip, 3 Details, 4 Undo
        specs.reserve (v.steps.size());
        for (const auto& s : v.steps)
        {
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
            else if (s.right.isNotEmpty())        { r.rightButtonW = rightW;          kind = 1; }
            specs.push_back (r);
            kinds.push_back (kind);
        }
        if (! specs.empty())
        {
            y += layoutRows (specs, x0, y, width, L.rows);
            // the chips of a question row sit UNDER that row, indented past the glyph column
            for (size_t i = 0; i < L.rows.size() && i < v.steps.size(); ++i)
            {
                const int kind = kinds[i];
                const auto& rr = L.rows[i];
                if (kind == 2)
                {
                    const int bw = lineBtnW;
                    L.zones.push_back ({ { rr.button.getX(), rr.button.getY(), bw, rr.button.getHeight() },
                                         RunHit::ApplyLine, (int) i, {} });
                    L.zones.push_back ({ { rr.button.getX() + bw + Metrics::btnGap, rr.button.getY(), bw, rr.button.getHeight() },
                                         RunHit::SkipLine, (int) i, {} });
                }
                else if (kind == 3) L.zones.push_back ({ rr.button, RunHit::Details, (int) i, {} });
                else if (kind == 4) L.zones.push_back ({ rr.button, RunHit::Undo,    (int) i, {} });
            }
            // chips: laid after the rows so their y is below everything the rows claimed
            for (size_t i = 0; i < v.steps.size(); ++i)
            {
                if (v.steps[i].chips.isEmpty() || ! v.steps[i].answered.isEmpty()) continue;
                const int cx = x0 + Metrics::glyphW + Metrics::glyphGap;
                std::vector<juce::Rectangle<int>> chipRects;
                const int h = layoutChips (v.steps[i].chips, cx, y, juce::jmax (1, width - (cx - x0)), chipRects);
                for (size_t k = 0; k < chipRects.size(); ++k)
                    L.zones.push_back ({ chipRects[k], RunHit::Chip, (int) i, v.steps[i].chips[(int) k] });
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
    if (v.finished && v.changeCount > 0)
    {
        const auto label = juce::String (v.changeCount) + (v.changeCount == 1 ? " change" : " changes");
        const int w = juce::jmax (Metrics::chipMinW, chipFont().getStringWidth (label) + 30);
        L.changesBtn = { x0, y, w, changesH };
        L.zones.push_back ({ L.changesBtn, RunHit::ToggleSteps, -1, v.stepsOpen ? "Hide steps" : label });
    }
    if (v.showApplyAll)
    {
        if (! v.onePlanLine) { footer.add ("Apply all");  footerKinds.push_back (RunHit::ApplyAll); }
        else                 { footer.add ("Apply");      footerKinds.push_back (RunHit::ApplyAll); }
        footer.add ("Skip all"); footerKinds.push_back (RunHit::SkipAll);
    }
    if (v.showGotIt)  { footer.add ("Got it, you can stop"); footerKinds.push_back (RunHit::GotIt); }
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

} // namespace echojay::agentui
