#pragma once

#include <JuceHeader.h>
#include <optional>
#include "EJMatchTab.h"           // MatchHost, MatchSides, MatchSlotPair, CaptureSnapshot
#include "EJMatchWindowSides.h"   // matchSidesForWindow, matchWindowMemory
#include "EJMatchProposal.h"             // computeMatchProposal, MatchProposalFor::Plan
#include "EJMatchAttributionCapture.h"   // attribInsFromCapture, attributeBandMove
#include "EJMatchBlock.h"                // buildMatchBlock, context.match v1
#include "EJMatchReport.h"               // the match_report payload

// =============================================================================
//  THE MATCH PAGE OVER MATCHHOST: which capture and which window it reads.
// =============================================================================
//
// One place turns the host's answers into the page's, so paint, the press and
// the hand-off cannot disagree about which capture or which window they are on.
// Since fix 8a that place is matchPageFor in EJMatchPage.h, which A's "keep the
// reason on screen" check calls too; this header builds on the view it returns.
// It is a header, not a static in EJMatchTab.cpp, so a pin can drive it with a
// MatchHost of its own.
//
// THE MIX SIDE'S SERIES IS THE SLOT'S OWN CAPTURE (round 5a; A's 7db71816,
// Kathy's R4-2). MatchHost::captureForSlot answers for the slot on screen: a
// Snapshot slot gets the capture at its own index, never the newest one, and
// every other kind gets nothing. latestCapture() is not read here: an older
// snapshot in the mix slot windowed from the newest capture would be two
// moments presented as one.
//
// NO SUBSTITUTE. A slot with no capture of its own (Live, a workspace review,
// anything not a Snapshot) gives the window no mix series, so in "loudest30"
// that side has no bands and `why` says so (EJMatchWindowSides.h). A Live mix
// slot keeps 4a's line, "Capture at least 30 s of the chorus to make a plan."
//
// THE HAND-OFF (round 5d), through MatchHost only: the AI MATCH press posts the
// match_report (C2 section 4) with postToSessionChat, and "Make a plan" sends
// the context.match block (C3 rev 3 section 2) with draftPlan and opens the
// answer with openSessionChatAt. Both are built from ONE MatchHandOff: the
// sides over the window, 4b's uncapped plan proposal, and 4c's attribution from
// the mix slot's own capture, so the report and the plan cannot describe two
// different comparisons.
// =============================================================================

namespace echojay
{

/** The page view with the page's own window (fix 8a): matchPageFor
    (EJMatchPage.h) over the window remembered for the selected reference,
    from sides the caller already holds. A shorthand, not a second
    computation. */
inline MatchPageView matchPageViewOf (const MatchHost& host, const MatchSides& sides,
                                      const MatchSlotPair& slots)
{
    return matchPageFor (host, matchSelectedWindow (host), slots, &sides);
}

// =============================================================================
//  5d: THE REPORT AND "MAKE A PLAN"
// =============================================================================

inline constexpr const char* kMatchPlanCap          = "match_plan";   ///< C3 section 4
inline constexpr const char* kMatchReportKind       = "match_report"; ///< C2 section 4
inline constexpr const char* kMatchReportInChatLabel = "Report in chat";

/** The page's hand-off state lives in the panel's own properties: MatchPanel
    is declared in A's PluginEditor.h and gains no members. */
inline juce::Identifier matchPageReportInChatId() { return "ejMatchReportInChat"; }
inline juce::Identifier matchPagePlanLineId()     { return "ejMatchPlanLine"; }
inline juce::Identifier matchPagePlanBusyId()     { return "ejMatchPlanBusy"; }
inline juce::Identifier matchPageLastReportId()   { return "ejMatchLastReport"; }

// -----------------------------------------------------------------------------
// FIGURES THE BLOCK NEEDS THAT THE SIDES DO NOT CARRY
// -----------------------------------------------------------------------------

/** C3's reference.id. The page has the MEASURED reference (ReferenceResult),
    whose identity is its path; the library's RefEntry id is not reachable
    through MatchHost (REQUESTS_FOR_A_5 R5-2). So the id is "ref_" and 8 hex
    digits of the path's hash: stable per file, the same on every press. */
inline juce::String matchReferenceId (const juce::String& path)
{
    const auto hex = juce::String::toHexString ((juce::int64) path.hashCode64()).paddedLeft ('0', 16);
    return "ref_" + hex.substring (8);
}

// -----------------------------------------------------------------------------
// THE ONE COMPARISON BOTH SENDS ARE BUILT FROM
// -----------------------------------------------------------------------------

struct MatchHandOff
{
    MatchWindowMode mode = MatchWindowMode::Whole;
    MatchSide mix, ref;                         ///< over the window
    juce::String referenceName, referenceId;
    MatchProposal plan;                         ///< MatchProposalFor::Plan: uncapped (X1)
    /** C4's D per band, mix minus reference, on the six-band relatives; absent
        where the plan considered no bands (no six bands, or a refusal). */
    std::array<std::optional<float>, 6> D {};
    /** One per band with a D, from the mix slot's own capture. */
    std::array<std::optional<AttribResult>, 6> attribution {};
    MatchBusTap busTap = MatchBusTap::Output;
    bool mixIsCapture = false;                  ///< the mix slot holds a capture of its own
    std::optional<double> mixLoudest30, refLoudest30;
    std::optional<MatchCrestLog> crestLog;
};

/** Whether the plan proposal considered the bands at all. */
inline bool matchPlanConsideredBands (const MatchProposal& p)
{
    if (p.bandsUnavailableWhy.isNotEmpty()) return false;
    for (const auto& r : p.refusals)
        if (r.kind == MatchRefusalKind::BandsTooShort || r.kind == MatchRefusalKind::BandsNotAverage)
            return false;
    return true;
}

inline MatchHandOff matchHandOffOf (const MatchHost& host, const MatchPageView& v)
{
    MatchHandOff h;
    h.mode = v.w.mode;
    h.mix = v.w.mix;
    h.ref = v.w.ref;
    const auto* ref = host.selectedReference();
    if (ref != nullptr)
    {
        h.referenceName = ref->name;
        h.referenceId   = matchReferenceId (ref->path);
        h.refLoudest30  = matchLoudest30Lufs (ref->hopFrames);
    }
    h.plan = v.plan;

    std::array<float, 6> mr {}, rr {};
    if (matchPlanConsideredBands (h.plan)
        && matchBandRelatives (h.mix.macro, mr) && matchBandRelatives (h.ref.macro, rr))
        for (std::size_t b = 0; b < 6; ++b) h.D[b] = mr[b] - rr[b];

    h.mixIsCapture = v.mixCapture.has_value();
    if (! v.mixCapture) return h;

    const auto& c = *v.mixCapture;
    h.mixLoudest30 = matchLoudest30Lufs (c.hopFrames);
    h.busTap = matchBusTapOf (c);
    // The SAME hops on the bus and on every Link (C4 section 2): the whole
    // capture, or the window the page compared.
    const std::optional<HopRange> w = h.mode == MatchWindowMode::Whole
                                          ? std::optional<HopRange> (HopRange { 0, c.hopFrames.size() })
                                          : v.w.mixWindow;
    if (! w) return h;
    const auto ins = attribInsFromCapture (c, *w, h.D, host.liveLinks());
    for (std::size_t b = 0; b < 6; ++b)
        if (ins[b]) h.attribution[b] = attributeBandMove (*ins[b]);

    // The crest log over the same windows (C4 rev 4 section 6), for the card's
    // record only. hostBlockSize is not reachable (R5-1), so it stays 0: absent.
    MatchCrestLog log;
    log.mix = matchCrestLogSide (c.hopFrames, *w);
    if (ref != nullptr)
    {
        const std::optional<HopRange> rw = h.mode == MatchWindowMode::Whole
                                               ? std::optional<HopRange> (HopRange { 0, ref->hopFrames.size() })
                                               : v.w.refWindow;
        if (rw) log.ref = matchCrestLogSide (ref->hopFrames, *rw);
    }
    h.crestLog = log;
    return h;
}

/** A placed attribution: the band has a move and C4 put it somewhere. */
inline bool matchAttributionPlaced (const std::optional<AttribResult>& a)
{
    return a && (a->target == AttribTarget::Bus || a->target == AttribTarget::Track
                 || a->target == AttribTarget::Pair);
}

// -----------------------------------------------------------------------------
// THE REPORT (C2 section 4)
// -----------------------------------------------------------------------------

inline MatchReportIn matchReportInOf (const MatchHandOff& h, bool hasMatchPlan, bool agentOn)
{
    MatchReportIn in;
    in.referenceName = h.referenceName;
    in.window = h.mode;
    in.mix = h.mix;
    in.ref = h.ref;
    in.proposal = h.plan;
    for (std::size_t b = 0; b < 6; ++b)
    {
        in.bands[b].mixMinusRefDb = h.D[b];
        if (matchAttributionPlaced (h.attribution[b])) in.bands[b].attribution = h.attribution[b];
    }
    in.busTap = h.busTap;
    in.crestLog = h.crestLog;
    in.hasMatchPlan = hasMatchPlan;
    in.agentOn = agentOn;
    return in;
}

/** The payload as the host stores it: C2 section 4's object. */
inline juce::var matchReportVar (const MatchHandOff& h, bool hasMatchPlan, bool agentOn)
{
    const auto in  = matchReportInOf (h, hasMatchPlan, agentOn);
    const auto rep = buildMatchReport (in);
    const juce::String json = matchReportPayload (in, rep);
    juce::var payload = juce::JSON::parse (json);
    return payload;
}

/** The AI MATCH press's post: the report goes to the Whole session chat, and
    the page notes it. False, and nothing posted, if the payload did not build. */
inline bool matchPostReport (MatchHost& host, const MatchHandOff& h, juce::NamedValueSet& page)
{
    const juce::var payload = matchReportVar (h, host.hasCap (kMatchPlanCap), host.agentOn());
    if (! payload.isObject()) return false;
    host.postToSessionChat (kMatchReportKind, payload);
    page.set (matchPageReportInChatId(), true);
    return true;
}

// -----------------------------------------------------------------------------
// ONE REPORT PER COMPARISON (round 6b)
// -----------------------------------------------------------------------------
//
// A comparison is the mix slot's capture (its id, as captureForSlot returns
// it), the reference, the window and the capture's hop count. A press whose
// comparison is the one this page last posted plays the morph again and points
// at the report already in the chat instead of posting a second. Any of the
// four changed is a new comparison, and a new report.
//
// A LIVE MIX HAS NO IDENTITY. Its slot holds no capture, so nothing says two
// presses compared the same audio, and every such press posts.
//
// NO LINK. postToSessionChat returns no message id and MatchHost cannot open
// the chat at a message, so "Report in chat" is plain text (REQUESTS_FOR_A_6).

struct MatchReportIdentity
{
    juce::String captureId, referencePath;
    MatchWindowMode mode = MatchWindowMode::Whole;
    int hops = 0;
};

/** The comparison a press is about; nullopt when the mix slot holds no capture. */
inline std::optional<MatchReportIdentity> matchReportIdentityOf (const MatchPageView& v)
{
    if (! v.mixCapture) return std::nullopt;
    return MatchReportIdentity { v.mixCapture->id, v.refKey, v.w.mode, v.mixCapture->hopFramesHops };
}

/** One string per comparison, for the page's properties. JSON, so no field
    can run into the next. */
inline juce::String matchReportIdentityKey (const MatchReportIdentity& i)
{
    juce::Array<juce::var> a { juce::var (i.captureId), juce::var (i.referencePath),
                               juce::var (i.mode == MatchWindowMode::Whole ? "whole" : "loudest30"),
                               juce::var (i.hops) };
    return juce::JSON::toString (juce::var (a), true);
}

inline bool matchReportAlreadyPosted (const std::optional<MatchReportIdentity>& id,
                                      const juce::NamedValueSet& page)
{
    if (! id) return false;
    const auto* last = page.getVarPointer (matchPageLastReportId());
    return last != nullptr && last->isString() && last->toString() == matchReportIdentityKey (*id);
}

struct MatchPressActs { bool morph = false, post = false; };

/** What a press does. The morph plays whenever the proposal is playable, on
    every press; the report posts only for a comparison not already posted. */
inline MatchPressActs matchPressActs (bool playable, const std::optional<MatchReportIdentity>& id,
                                      const juce::NamedValueSet& page)
{
    MatchPressActs a;
    a.morph = playable;
    a.post  = playable && ! matchReportAlreadyPosted (id, page);
    return a;
}

enum class MatchReportPost { Posted, AlreadyInChat, NotBuilt };

/** The press's report half, after the morph has started: post a new report, or
    note the one already in the chat. */
inline MatchReportPost matchPressReport (MatchHost& host, const MatchHandOff& h,
                                         const std::optional<MatchReportIdentity>& id,
                                         juce::NamedValueSet& page)
{
    if (matchReportAlreadyPosted (id, page))
    {
        page.set (matchPageReportInChatId(), true);
        return MatchReportPost::AlreadyInChat;
    }
    if (! matchPostReport (host, h, page)) return MatchReportPost::NotBuilt;
    if (id) page.set (matchPageLastReportId(), matchReportIdentityKey (*id));
    else    page.remove (matchPageLastReportId());
    return MatchReportPost::Posted;
}

// -----------------------------------------------------------------------------
// THE PRESS (fix 8a): exactly what is on screen
// -----------------------------------------------------------------------------
//
// Sean's ruling of 30 Oct: the AI MATCH press uses the page view for the
// selected window and nothing else. One view, read once; its readiness decides
// the morph, its band steps are what the morph walks, and its hand-off is the
// report. A window that was asked for and not taken refuses with the page's
// own line (matchReadiness), and never falls back to the whole capture.
//
// MatchPanel::press() calls this and only animates what it returns, so a pin
// drives exactly what the button does.

struct MatchPress
{
    MatchReadiness ready;                 ///< the view's, never recomputed
    std::array<float, 6> moves {};        ///< the band steps the morph walks
    std::optional<MatchReportIdentity> id;
    MatchPressActs acts;
    std::optional<MatchReportPost> report;   ///< set when a post was tried
};

inline MatchPress matchPressOn (MatchHost& host, juce::NamedValueSet& page)
{
    const auto v = matchPageFor (host, matchSelectedWindow (host), host.matchSlots());
    MatchPress p;
    p.ready = v.ready;
    p.moves = matchBandMoves (v.prop);
    p.id    = matchReportIdentityOf (v);
    p.acts  = matchPressActs (p.ready.playable, p.id, page);
    if (p.acts.post)
        p.report = matchPressReport (host, matchHandOffOf (host, v), p.id, page);
    else if (p.acts.morph)
        page.set (matchPageReportInChatId(), true);   // already in the chat
    return p;
}

// -----------------------------------------------------------------------------
// THE BLOCK (C3 rev 3 section 2)
// -----------------------------------------------------------------------------

/** C3's finding figure for a directional finding: the SIDE field of the same
    name. Crest has none (C4 rev 4: no crest finding is ever written). */
inline const char* matchBlockFigure (MatchFigure f) noexcept
{
    switch (f)
    {
        case MatchFigure::LRA:         return "lra";
        case MatchFigure::PSR:         return "psr";
        case MatchFigure::PLR:         return "plr";
        case MatchFigure::Width:       return "width";
        case MatchFigure::Correlation: return "corr";
        case MatchFigure::Crest:
        case MatchFigure::Count:       break;
    }
    return nullptr;
}

/** C3 names one refusal kind, "too_short", in its example and its fixture. The
    other three are K's words, for B to confirm. */
inline const char* matchBlockRefusalKind (MatchRefusalKind k) noexcept
{
    switch (k)
    {
        case MatchRefusalKind::BandsTooShort:
        case MatchRefusalKind::DynamicsTooShort: return "too_short";
        case MatchRefusalKind::BandsNotAverage:  return "not_average";
        case MatchRefusalKind::GainBelowFloor:   return "below_floor";
    }
    return "";
}

inline juce::String matchBlockRefusalSide (const juce::String& side)
{
    if (side == "the capture")   return "mix";
    if (side == "the reference") return "ref";
    return "both";
}

/** One SIDE. bandsOk false: the six relatives do not exist, and C3 needs them. */
inline MatchBlockSide matchBlockSideOf (const MatchSide& s, std::optional<double> loudest30, bool& bandsOk)
{
    MatchBlockSide o;
    if (s.integrated > -99.0f) o.integrated = s.integrated;
    o.loudest30 = loudest30;
    if (s.truePeak > -99.0f) o.truePeak = s.truePeak;
    if (s.overs >= 0) o.overs = s.overs;
    if (s.lra > 0.0f) o.lra = s.lra;
    if (s.psr > -99.0f) o.psr = s.psr;
    if (s.plr > -99.0f) o.plr = s.plr;
    o.width = s.width;          // always measured (MatchSide)
    o.corr  = s.correlation;
    std::array<float, 6> rel {};
    bandsOk = s.hasMacro && matchBandRelatives (s.macro, rel);
    for (std::size_t b = 0; b < 6; ++b) o.bandsRel[b] = rel[b];
    return o;
}

inline const char* matchBlockTarget (AttribTarget t) noexcept
{
    switch (t)
    {
        case AttribTarget::Track: return "track";
        case AttribTarget::Pair:  return "pair";
        case AttribTarget::Bus:   return "bus";
        case AttribTarget::None:
        case AttribTarget::Unavailable: break;
    }
    return "";
}

/** C4's placement as C3's attribution. */
inline MatchBlockAttribution matchBlockAttributionOf (const AttribResult& a)
{
    MatchBlockAttribution o;
    o.target = matchBlockTarget (a.target);
    for (const auto& l : a.links) o.links.push_back ({ l.uid, l.name, l.share });
    if (a.target == AttribTarget::Pair) o.combinedShare = a.combinedShare;
    o.reason = a.reason;
    return o;
}

/** The block's input from the hand-off.

    SIGNS. C3's measuredDb is "the difference it came from", mix minus
    reference, as its fixtures carry it (a gain of +1.6 came from -1.6). The
    proposal's own measuredDb is reference minus mix for a band and a gain, so
    both are negated here. A ceiling's measuredDb stays the mix's true peak,
    which is what it came from (K's reading; no fixture has a ceiling). */
inline MatchBlockIn matchBlockInOf (const MatchHandOff& h, bool& bandsOk)
{
    MatchBlockIn in;
    in.referenceId = h.referenceId;
    in.referenceName = h.referenceName;
    in.window = h.mode == MatchWindowMode::Whole ? "whole" : "loudest30";
    bool okMix = false, okRef = false;
    in.mix = matchBlockSideOf (h.mix, h.mixLoudest30, okMix);
    in.ref = matchBlockSideOf (h.ref, h.refLoudest30, okRef);
    bandsOk = okMix && okRef;

    for (const auto& m : h.plan.moves)
    {
        MatchBlockMove o;
        switch (m.kind)
        {
            case MatchMoveKind::Ceiling:
                o.kind = "ceiling"; o.valueDb = m.valueDb; o.measuredDb = m.measuredDb;
                break;
            case MatchMoveKind::Gain:
                o.kind = "gain"; o.valueDb = m.valueDb; o.measuredDb = -m.measuredDb;
                break;
            case MatchMoveKind::Band:
            {
                o.kind = "band"; o.band = m.band;
                o.measuredDb = -m.measuredDb;   // D
                o.valueDb = m.valueDb;          // -D on the bus: the plan is uncapped
                const auto& a = h.attribution[(std::size_t) m.band];
                if (matchAttributionPlaced (a) && a->valueDb)
                {
                    o.valueDb = *a->valueDb;    // C4 section 5's x on a track or pair
                    o.attribution = matchBlockAttributionOf (*a);
                }
                break;
            }
        }
        in.moves.push_back (o);
    }
    for (const auto& f : h.plan.findings)
        if (const char* name = matchBlockFigure (f.figure))
            in.findings.push_back ({ name, f.mixValue, f.refValue, f.difference });
    for (const auto& r : h.plan.refusals)
        in.refusals.push_back ({ matchBlockRefusalKind (r.kind), matchBlockRefusalSide (r.side), r.message });
    return in;
}

inline juce::String matchPlanNeedsBandsSentence()
{
    return "Make a plan needs the six bands measured on both sides of this comparison.";
}

/** The block "Make a plan" sends, through C3's own rules (buildMatchBlock). */
inline MatchBlockRead matchPlanBlockOf (const MatchHandOff& h)
{
    bool bandsOk = false;
    const auto in = matchBlockInOf (h, bandsOk);
    if (! bandsOk)
    {
        MatchBlockRead r;
        r.unreadableWhy = matchPlanNeedsBandsSentence();
        return r;
    }
    return buildMatchBlock (in);
}

inline int matchBlockMoveCount (const MatchBlockRead& r)
{
    if (! r.readable) return 0;
    const juce::var moves = r.block.getProperty ("moves", juce::var());
    return moves.isArray() ? moves.size() : 0;
}

// -----------------------------------------------------------------------------
// WHEN "MAKE A PLAN" IS OFFERED (C2, C3 sections 3 and 4)
// -----------------------------------------------------------------------------

/** All four: a v2 build, one that declares match_plan, something to plan (C3
    section 3: only refusals, or nothing over a floor, is no plan), and a
    capture of its own in the mix slot (DESIGN D4: a Live side can't be
    windowed, so the plan needs a capture). */
inline bool matchMakePlanOffered (bool isV2, bool hasMatchPlan, bool somethingToPlan, bool mixIsCapture) noexcept
{
    return isV2 && hasMatchPlan && somethingToPlan && mixIsCapture;
}

/** Something to plan, from the plan proposal and the windowed sides alone, so
    paint can ask it every frame without building the block or the
    attribution. It holds whenever the block would carry a move: the plan
    proposes one, both sides have their six bands (C3 refuses a block without
    them), and in "loudest30" both sides were windowed (C3 needs both
    loudest30 figures there). The send re-checks on the block itself. */
inline bool matchSomethingToPlan (const MatchProposal& plan, const MatchWindowedSides& w)
{
    std::array<float, 6> r {};
    return ! plan.moves.empty()
        && w.mix.hasMacro && matchBandRelatives (w.mix.macro, r)
        && w.ref.hasMacro && matchBandRelatives (w.ref.macro, r)
        && (w.mode == MatchWindowMode::Whole || w.windowed);
}

inline bool matchMakePlanOfferedOn (const MatchHost& host, const MatchPageView& v)
{
    return matchMakePlanOffered (host.isV2(), host.hasCap (kMatchPlanCap),
                                 matchSomethingToPlan (v.plan, v.w), v.mixCapture.has_value());
}

// -----------------------------------------------------------------------------
// "MAKE A PLAN": the send, and what the page shows of the answer
// -----------------------------------------------------------------------------

using MatchPlanAnswer = std::function<void (bool ok, juce::String planId, juce::String why)>;

/** Sends the block with draftPlan. False, with whyNot, when there is nothing
    the server could read; draftPlan is then never called. */
inline bool matchSendPlan (MatchHost& host, const MatchHandOff& h, MatchPlanAnswer answer, juce::String& whyNot)
{
    const auto r = matchPlanBlockOf (h);
    if (! r.readable)
    {
        whyNot = r.unreadableWhy == matchPlanNeedsBandsSentence()
                     ? r.unreadableWhy
                     : "Make a plan couldn't send this comparison: " + r.unreadableWhy + ".";
        return false;
    }
    if (matchBlockMoveCount (r) == 0)
    {
        whyNot = "There is nothing over a floor here to plan.";
        return false;
    }
    host.draftPlan (r.block, std::move (answer));
    return true;
}

/** The answer: on ok the Whole session chat opens at the plan and the page
    shows nothing more; otherwise the page shows the callback's sentence. */
inline juce::String matchPlanAnswered (MatchHost& host, bool ok, const juce::String& planId, const juce::String& why)
{
    if (ok && planId.isNotEmpty())
    {
        host.openSessionChatAt (planId);
        return {};
    }
    if (why.isNotEmpty()) return why;
    return ok ? "The plan came back without an id, so there is nothing to open."
              : "The plan could not be drafted.";
}

// -----------------------------------------------------------------------------
// WHERE THE HAND-OFF SITS: the row under the window switch
// -----------------------------------------------------------------------------

inline constexpr int kMatchPlanButtonW   = 96;
inline constexpr int kMatchReportNoteW   = 104;
inline constexpr int kMatchHandOffGap    = 6;

struct MatchHandOffRects { juce::Rectangle<int> reportNote, planButton, planLine; };

inline MatchHandOffRects matchHandOffRects (juce::Rectangle<int> graph)
{
    MatchHandOffRects o;
    const auto sw = matchWindowSwitchRect (graph);
    if (sw.isEmpty()) return o;
    const auto inner = graph.reduced (kMatchWindowSwitchPad);
    const int y = sw.getBottom() + kMatchHandOffGap;
    if (y + 2 * kMatchWindowSwitchH + kMatchHandOffGap > inner.getBottom()) return o;
    o.planButton = { sw.getRight() - kMatchPlanButtonW, y, kMatchPlanButtonW, kMatchWindowSwitchH };
    o.reportNote = { o.planButton.getX() - kMatchHandOffGap - kMatchReportNoteW, y,
                     kMatchReportNoteW, kMatchWindowSwitchH };
    o.planLine   = { inner.getX(), y + kMatchWindowSwitchH + kMatchHandOffGap,
                     sw.getRight() - inner.getX(), kMatchWindowSwitchH };
    return o;
}

/** Every string the hand-off can put on the page. MA5 reads it. */
inline juce::StringArray matchHandOffPageStrings()
{
    return { kMatchReportInChatLabel, kMatchMakePlanLabel, matchPlanNeedsBandsSentence(),
             "There is nothing over a floor here to plan.",
             "The plan came back without an id, so there is nothing to open.",
             "The plan could not be drafted.", "Make a plan couldn't send this comparison: " };
}

} // namespace echojay
