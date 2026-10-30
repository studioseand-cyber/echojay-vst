#pragma once

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <optional>
#include <vector>
#include "EJMatchProposal.h"      // MatchSide, MatchProposal, the floors
#include "EJMatchWindow.h"        // MatchWindowMode
#include "EJMatchAttribution.h"   // AttribResult and C4's sentences

// =============================================================================
//  THE MATCH REPORT: ITS SENTENCES, ITS LAYOUT AND ITS CARD PAYLOAD.
// =============================================================================
//
// DESIGN_MATCH_TAB rev 7 section 3 (the report and its three tiers),
// MATCH_SCREEN_CONTRACT section 5 (the card), C2 section 4 (the match_report
// payload, with C4's "share" where C2 wrote "ratio": C4 section 8 Q6, K's
// proposal) and C4 rev 4 sections 6 and 9 (the card lines and the crest log).
//
// BUILT FROM THE PROPOSAL STRUCTS, NEVER FROM A MODEL REPLY (screen contract
// section 5). Every sentence here is written by the plugin from the figures it
// measured, so a v1 plugin can never be shown one by the server.
//
// CREST DECIDES NOTHING AND REACHES NO USER (C4 rev 4 section 6, Sean, 28 Oct).
// There is no crest finding and no band move is withheld for one. Decision 4's
// sentence goes ONCE on the card whenever it proposes a band move, never under
// each move. The crest figures (V0, V1 as available, the host block size and
// frames per hop) go into the payload's crestLog for tuning, and nothing that
// renders reads it.
//
// PURE. No JUCE audio type, no global, no I/O. Layout takes a text-width
// function from the caller, so no font is read here.
// =============================================================================

namespace echojay
{

enum class MatchReportTier { CanDo, JustSoYouKnow, CantCompare };

inline const char* matchReportTierTitle (MatchReportTier t) noexcept
{
    switch (t)
    {
        case MatchReportTier::CanDo:         return "Can do:";
        case MatchReportTier::JustSoYouKnow: return "Just so you know:";
        case MatchReportTier::CantCompare:   return "Can't compare:";
    }
    return "";
}

/** The band labels the report prints (design section 3): the one table's
    (EJMatchProposal.h, 6a). */
inline juce::String matchReportBandLabel (int b) { return matchBandLabel (b); }

/** The label for one entry of the payload's bands[] (round 7b). bands[].name
    is B1's id on the wire (Sean, 29 Oct); the card shows the table's label, and
    A's paint branch reads it here. A name the table does not hold gives "",
    never the name itself. */
inline juce::String matchReportPayloadBandLabel (const juce::var& band)
{
    return matchBandLabel (matchBandIndexOfId (band.getProperty ("name", juce::var()).toString()));
}

inline juce::String matchReportArrow() { return juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")); }

// The button and offer labels. MA5 pins these.
inline const char* kMatchMakePlanLabel       = "Make a plan";
inline const char* kMatchLoudest30Label      = "Loudest 30 s instead";
inline const char* kMatchWholeSongLabel      = "Whole song instead";
inline const char* kMatchPickReferenceLabel  = "Pick another reference";
inline const char* kMatchTurnOnAgentLabel    = "Turn on Agent and do it all";

// -----------------------------------------------------------------------------
// INPUT
// -----------------------------------------------------------------------------

struct MatchReportBandIn
{
    /** C4's D: mix minus reference, dB; positive means the mix is hotter.
        Absent: the band was not measured, and the report prints no line. */
    std::optional<float> mixMinusRefDb;
    /** C4's placement. Absent: the move goes on the mix bus at -D. */
    std::optional<AttribResult> attribution;
};

/** One side's crest, for the log only (C4 rev 4 section 6). */
struct MatchCrestLogSide
{
    std::array<std::optional<float>, 6> v0 {};   ///< bandCrest: p95 of bandPower over its 3 s local mean
    std::array<std::optional<float>, 6> v1 {};   ///< bandCrestPeakOverMean: median bandPeak / bandPower
    std::optional<float> framesPerHop;           ///< this series' mean frames per hop
};

/** The crest log: both sides over the selected window, and the host block size
    the bias can be corrected from later. LOGGED, NEVER RENDERED. */
struct MatchCrestLog
{
    MatchCrestLogSide mix, ref;
    int hostBlockSize = 0;   ///< 0: not known
};

/** One side's log from its hop series over the window. */
template <typename Frame>
inline MatchCrestLogSide matchCrestLogSide (const std::vector<Frame>& frames, HopRange w)
{
    MatchCrestLogSide s;
    s.v0 = bandCrest (frames, w).db;
    s.v1 = bandCrestPeakOverMean (frames, w);
    s.framesPerHop = meanFramesPerHop (frames, w);
    return s;
}

struct MatchReportIn
{
    juce::String    referenceName;
    MatchWindowMode window = MatchWindowMode::Whole;
    MatchSide       mix, ref;
    /** Gain, ceiling, directional findings and refusals. Its band moves are not
        read: the bands come from `bands`, sized by C4. */
    MatchProposal   proposal;
    std::array<MatchReportBandIn, 6> bands {};
    /** Where P_bus came from (C2 section 5.1): matchBusTapOf the capture. */
    MatchBusTap busTap = MatchBusTap::Output;
    /** For the payload only. Absent: no log was taken. */
    std::optional<MatchCrestLog> crestLog;
    bool hasMatchPlan = false;   ///< the build declares match_plan (C3 section 4)
    bool agentOn = false;
};

// -----------------------------------------------------------------------------
// OUTPUT
// -----------------------------------------------------------------------------

enum class MatchReportLineKind { Move, UnderFloor, Finding, Refusal };

struct MatchReportLine
{
    MatchReportTier     tier = MatchReportTier::CanDo;
    MatchReportLineKind kind = MatchReportLineKind::Move;
    juce::String        label, text;
    juce::StringArray   detail;   ///< C4's sentences under the line
    int                 band = -1;
    std::optional<float> valueDb;   ///< Move only
    juce::String        moveKind;  ///< Move only: "eq", "gain" or "ceiling"
};

struct MatchReport
{
    juce::String heading;
    std::vector<MatchReportLine> lines;   ///< Can do, then Just so you know, then Can't compare
    bool caveat = false;                  ///< a Can do line names a track (C4 section 9)
    juce::String discriminator;           ///< decision 4's sentence: once, when any band move is proposed
    std::vector<juce::String> buttons;
};

// -----------------------------------------------------------------------------
// THE SENTENCES
// -----------------------------------------------------------------------------

inline juce::String matchReportHeading (const juce::String& name, MatchWindowMode w)
{
    return "Compared with \"" + name + "\" ("
         + (w == MatchWindowMode::Whole ? "whole song" : "loudest 30 s of each") + ")";
}

/** "2.8 dB hotter than the reference", "1.6 dB cooler than the reference". */
inline juce::String matchReportBandGap (float D)
{
    if (D == 0.0f) return "level with the reference";
    return juce::String (std::abs (D), 1) + " dB " + (D > 0.0f ? "hotter" : "cooler") + " than the reference";
}

inline juce::String matchReportUnderFloor()
{
    return "(under the " + juce::String (kMatchBandFloorDb, 1) + " dB measurement floor; leaving it)";
}

/** Plan decision 4, C4 rev 4 section 6: once on the card, whenever it proposes
    at least one band move. */
inline juce::String matchReportDecision4Sentence()
{
    return "These EQ moves were proposed without the check that tells body from peaks.";
}

inline juce::String matchReportMoveAction (const std::optional<AttribResult>& a, float valueDb)
{
    const juce::String size = juce::String (std::abs (valueDb), 1) + " dB "
                            + (valueDb < 0.0f ? "cut" : "lift");
    if (a && a->target == AttribTarget::Track && a->links.size() == 1)
        return "a " + size + " on " + a->links[0].name;
    if (a && a->target == AttribTarget::Pair && a->links.size() == 2)
        return "the same " + size + " on " + a->links[0].name + " and " + a->links[1].name;
    return "a " + size + " on the mix bus";
}

namespace detail
{
    inline juce::String compareDb (float mix, float ref, const char* unit, int places = 1)
    {
        return "(" + juce::String (mix, places) + " vs " + juce::String (ref, places) + unit + ")";
    }
}

/** A directional finding's line, or an empty label when the report leaves it
    out: PSR and PLR outside the whole window or with either side unknown, and
    the whole-file crest figure (crest reaches no user, C4 rev 4 section 6). */
inline MatchReportLine matchReportFindingLine (const MatchFinding& f, MatchWindowMode w,
                                               const MatchSide& mix, const MatchSide& ref)
{
    MatchReportLine l;
    l.tier = MatchReportTier::JustSoYouKnow;
    l.kind = MatchReportLineKind::Finding;
    const float d = f.difference;
    switch (f.figure)
    {
        case MatchFigure::PSR:
        case MatchFigure::PLR:
        {
            const bool psr = f.figure == MatchFigure::PSR;
            const float a = psr ? mix.psr : mix.plr, b = psr ? ref.psr : ref.plr;
            if (w != MatchWindowMode::Whole || a <= -99.0f || b <= -99.0f) return {};
            l.label = psr ? "Dynamics" : "Peak to loudness";
            l.text = juce::String (d < 0 ? "more squashed than the reference " : d > 0 ? "more open than the reference "
                                                                                       : "the same as the reference ")
                   + "(" + (psr ? "PSR " : "PLR ") + juce::String (f.mixValue, 1) + " vs "
                   + juce::String (f.refValue, 1) + ")";
            return l;
        }
        case MatchFigure::LRA:
            l.label = "Loudness range";
            l.text = juce::String (d < 0 ? "narrower than the reference " : d > 0 ? "wider than the reference "
                                                                                   : "the same as the reference ")
                   + detail::compareDb (f.mixValue, f.refValue, " LU");
            return l;
        case MatchFigure::Width:
            l.label = "Width";
            l.text = juce::String (d < 0 ? "narrower than the reference " : d > 0 ? "wider than the reference "
                                                                                   : "the same as the reference ")
                   + "(" + juce::String (juce::roundToInt (f.mixValue)) + "% vs "
                   + juce::String (juce::roundToInt (f.refValue)) + "%)";
            return l;
        case MatchFigure::Correlation:
            l.label = "Correlation";
            l.text = juce::String (d < 0 ? "lower than the reference " : d > 0 ? "higher than the reference "
                                                                                : "the same as the reference ")
                   + detail::compareDb (f.mixValue, f.refValue, "", 2);
            return l;
        case MatchFigure::Crest:
        case MatchFigure::Count:
            break;
    }
    return {};
}

inline const char* matchReportRefusalLabel (MatchRefusalKind k) noexcept
{
    switch (k)
    {
        case MatchRefusalKind::BandsTooShort:
        case MatchRefusalKind::BandsNotAverage:  return "Tone";
        case MatchRefusalKind::DynamicsTooShort: return "Loudness and dynamics";
        case MatchRefusalKind::GainBelowFloor:   return "Loudness";
    }
    return "";
}

// -----------------------------------------------------------------------------
// THE REPORT
// -----------------------------------------------------------------------------

inline MatchReport buildMatchReport (const MatchReportIn& in)
{
    MatchReport r;
    r.heading = matchReportHeading (in.referenceName, in.window);
    std::vector<MatchReportLine> canDo, know, cant;

    // Can do: the ceiling, then loudness, then the bands in band order.
    for (const auto& m : in.proposal.moves)
    {
        if (m.kind == MatchMoveKind::Ceiling)
        {
            MatchReportLine l;
            l.label = "Peaks"; l.moveKind = "ceiling"; l.valueDb = m.valueDb;
            l.text = "your mix peaks at " + juce::String (m.measuredDb, 1) + " dBTP"
                   + (in.ref.truePeak > -99.0f ? " (the reference " + juce::String (in.ref.truePeak, 1) + ")" : juce::String())
                   + " " + matchReportArrow() + " a ceiling at " + juce::String (m.valueDb, 1) + " dBTP";
            canDo.push_back (l);
        }
    }
    for (const auto& m : in.proposal.moves)
    {
        if (m.kind == MatchMoveKind::Gain)
        {
            MatchReportLine l;
            l.label = "Loudness"; l.moveKind = "gain"; l.valueDb = m.valueDb;
            l.text = "you're " + juce::String (std::abs (m.valueDb), 1) + " dB "
                   + (m.valueDb > 0.0f ? "quieter " : "louder ")
                   + detail::compareDb (in.mix.integrated, in.ref.integrated, " LUFS") + " "
                   + matchReportArrow() + " limiter to the reference's level";
            canDo.push_back (l);
        }
    }

    for (int b = 0; b < 6; ++b)
    {
        const auto& bi = in.bands[(size_t) b];
        if (! bi.mixMinusRefDb) continue;
        const float D = *bi.mixMinusRefDb;
        MatchReportLine l;
        l.band = b;
        l.label = matchReportBandLabel (b);
        if (std::abs (D) < kMatchBandFloorDb)
        {
            l.kind = MatchReportLineKind::UnderFloor;
            l.text = matchReportBandGap (D) + " " + matchReportArrow() + " " + matchReportUnderFloor();
            canDo.push_back (l);
            continue;
        }
        const auto& a = bi.attribution;
        const float value = (a && a->valueDb) ? *a->valueDb : -D;
        l.moveKind = "eq";
        l.valueDb = value;
        l.text = matchReportBandGap (D) + " " + matchReportArrow() + " " + matchReportMoveAction (a, value);
        if (a && a->reason.isNotEmpty()) l.detail.add (a->reason);
        if (a && a->coverage) l.detail.add (attribCoverageSentence (*a->coverage, b));
        r.discriminator = matchReportDecision4Sentence();
        if (a && (a->target == AttribTarget::Track || a->target == AttribTarget::Pair)) r.caveat = true;
        canDo.push_back (l);
    }

    // Just so you know: the directional findings.
    for (const auto& f : in.proposal.findings)
    {
        auto l = matchReportFindingLine (f, in.window, in.mix, in.ref);
        if (l.label.isNotEmpty()) know.push_back (l);
    }

    // Can't compare: the refusals, by their own messages.
    for (const auto& x : in.proposal.refusals)
    {
        MatchReportLine l;
        l.tier = MatchReportTier::CantCompare;
        l.kind = MatchReportLineKind::Refusal;
        l.label = matchReportRefusalLabel (x.kind);
        l.text = x.message;
        cant.push_back (l);
    }
    if (in.proposal.bandsUnavailableWhy.isNotEmpty())
    {
        MatchReportLine l;
        l.tier = MatchReportTier::CantCompare;
        l.kind = MatchReportLineKind::Refusal;
        l.label = "Tone";
        l.text = "No band proposal: " + in.proposal.bandsUnavailableWhy + ".";
        cant.push_back (l);
    }

    for (auto* v : { &canDo, &know, &cant })
        r.lines.insert (r.lines.end(), v->begin(), v->end());

    // The buttons. "Make a plan" needs match_plan AND something to plan
    // (C3 section 3: only refusals, or nothing over a floor, is no plan).
    bool anyMove = false;
    for (const auto& l : r.lines) anyMove = anyMove || l.kind == MatchReportLineKind::Move;
    if (in.hasMatchPlan && anyMove) r.buttons.push_back (kMatchMakePlanLabel);
    r.buttons.push_back (in.window == MatchWindowMode::Whole ? kMatchLoudest30Label : kMatchWholeSongLabel);
    r.buttons.push_back (kMatchPickReferenceLabel);
    return r;
}

/** The plan card's one agent offer (design section 3, "Who sees what"): only a
    build with match_plan, and only with the agent off. */
inline std::optional<juce::String> matchPlanCardAgentOffer (bool hasMatchPlan, bool agentOn)
{
    if (hasMatchPlan && ! agentOn) return juce::String (kMatchTurnOnAgentLabel);
    return std::nullopt;
}

/** Every string the user can see from one Match press: the report and the plan
    card's offer. MA5 reads this. */
inline juce::StringArray matchReportAllStrings (const MatchReport& r, const std::optional<juce::String>& offer)
{
    juce::StringArray s;
    s.add (r.heading);
    for (const auto& l : r.lines)
    {
        s.add (l.label); s.add (l.text);
        s.addArray (l.detail);
    }
    for (auto t : { MatchReportTier::CanDo, MatchReportTier::JustSoYouKnow, MatchReportTier::CantCompare })
        s.add (matchReportTierTitle (t));
    if (r.caveat) s.add (attribPreFaderCaveatSentence());
    if (r.discriminator.isNotEmpty()) s.add (r.discriminator);
    for (const auto& b : r.buttons) s.add (b);
    if (offer) s.add (*offer);
    return s;
}

/** The report as the chat text (design section 3's shape). */
inline juce::String matchReportText (const MatchReport& r)
{
    juce::String t = r.heading + "\n";
    for (auto tier : { MatchReportTier::CanDo, MatchReportTier::JustSoYouKnow, MatchReportTier::CantCompare })
    {
        bool first = true;
        for (const auto& l : r.lines)
        {
            if (l.tier != tier) continue;
            if (first) { t << "\n" << matchReportTierTitle (tier) << "\n"; first = false; }
            t << "  " << l.label << "  " << l.text << "\n";
            for (const auto& d : l.detail) t << "    " << d << "\n";
        }
    }
    if (r.caveat) t << "\n" << attribPreFaderCaveatSentence() << "\n";
    if (r.discriminator.isNotEmpty()) t << "\n" << r.discriminator << "\n";
    t << "\n";
    for (const auto& b : r.buttons) t << "[" << b << "]   ";
    return t.trimEnd();
}

// -----------------------------------------------------------------------------
// THE LAYOUT: painted rows and hit zones (C2 section 4: painted zones, never
// overlay buttons, because kMaxWavePlayBtns is 8)
// -----------------------------------------------------------------------------

inline constexpr int kMatchReportPad      = 8;
inline constexpr int kMatchReportRowH     = 18;
inline constexpr int kMatchReportTitleH   = 22;
inline constexpr int kMatchReportButtonH  = 24;
inline constexpr int kMatchReportButtonGap = 8;
inline constexpr int kMatchReportButtonPad = 12;   ///< each side of a button's text

enum class MatchReportRowStyle { Heading, TierTitle, Line, Detail, Caveat };

struct MatchReportRow { juce::Rectangle<int> r; juce::String text; MatchReportRowStyle style; };
struct MatchReportZone { juce::Rectangle<int> r; juce::String label; };

struct MatchReportLayout
{
    std::vector<MatchReportRow>  rows;
    std::vector<MatchReportZone> zones;   ///< one per button, in button order
    int height = 0;
};

inline MatchReportLayout layoutMatchReport (const MatchReport& rep, int width,
                                            const std::function<int (const juce::String&)>& textWidth)
{
    MatchReportLayout L;
    const int inner = juce::jmax (0, width - 2 * kMatchReportPad);
    int y = kMatchReportPad;
    auto row = [&] (const juce::String& s, MatchReportRowStyle st, int h, int indent = 0)
    {
        L.rows.push_back ({ { kMatchReportPad + indent, y, juce::jmax (0, inner - indent), h }, s, st });
        y += h;
    };
    row (rep.heading, MatchReportRowStyle::Heading, kMatchReportTitleH);
    for (auto tier : { MatchReportTier::CanDo, MatchReportTier::JustSoYouKnow, MatchReportTier::CantCompare })
    {
        bool first = true;
        for (const auto& l : rep.lines)
        {
            if (l.tier != tier) continue;
            if (first) { row (matchReportTierTitle (tier), MatchReportRowStyle::TierTitle, kMatchReportTitleH); first = false; }
            row (l.label + "  " + l.text, MatchReportRowStyle::Line, kMatchReportRowH, kMatchReportPad);
            for (const auto& d : l.detail) row (d, MatchReportRowStyle::Detail, kMatchReportRowH, 2 * kMatchReportPad);
        }
    }
    if (rep.caveat) row (attribPreFaderCaveatSentence(), MatchReportRowStyle::Caveat, kMatchReportRowH);
    if (rep.discriminator.isNotEmpty()) row (rep.discriminator, MatchReportRowStyle::Caveat, kMatchReportRowH);

    // The buttons, left to right, wrapping to a new line when the next would
    // not fit.
    y += kMatchReportPad;
    int x = kMatchReportPad;
    for (const auto& b : rep.buttons)
    {
        const int w = textWidth (b) + 2 * kMatchReportButtonPad;
        if (x > kMatchReportPad && x + w > kMatchReportPad + inner)
        {
            x = kMatchReportPad;
            y += kMatchReportButtonH + kMatchReportButtonGap;
        }
        L.zones.push_back ({ { x, y, w, kMatchReportButtonH }, b });
        x += w + kMatchReportButtonGap;
    }
    if (! rep.buttons.empty()) y += kMatchReportButtonH;
    L.height = y + kMatchReportPad;
    return L;
}

// -----------------------------------------------------------------------------
// THE CARD PAYLOAD (C2 section 4, with "share"; coverage and the caveat flag
// per band are C4 section 8 Q7's ask). A figure not measured is ABSENT, never 0.
// busTap is C2 section 5.1's; crestLog is C4 rev 4 section 6's, read by nothing
// that renders, in C2 section 4's shape (round 5d: it was { hostBlockSize, mix,
// ref }, which is not the shape C2 gives). In crestLog's six-entry arrays an absent band is null, so the
// positions stay B1's.
// -----------------------------------------------------------------------------

namespace detail
{
    inline juce::var crestArray (const std::array<std::optional<float>, 6>& a)
    {
        juce::Array<juce::var> v;
        for (const auto& x : a) v.add (x ? juce::var ((double) *x) : juce::var());
        return v;
    }

    inline juce::var crestLogSide (const MatchCrestLogSide& s)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("v0", crestArray (s.v0));
        o->setProperty ("v1", crestArray (s.v1));
        if (s.framesPerHop) o->setProperty ("framesPerHop", (double) *s.framesPerHop);
        return juce::var (o.get());
    }
}

inline juce::String matchReportPayload (const MatchReportIn& in, const MatchReport& rep)
{
    using Obj = juce::DynamicObject;
    const bool whole = in.window == MatchWindowMode::Whole;
    const bool psrBoth = whole && in.mix.psr > -99.0f && in.ref.psr > -99.0f;
    const bool plrBoth = whole && in.mix.plr > -99.0f && in.ref.plr > -99.0f;

    auto side = [&] (const juce::String& name, const MatchSide& s)
    {
        Obj::Ptr o = new Obj();
        o->setProperty ("name", name);
        if (! whole) o->setProperty ("windowS", 30.0);
        else if (s.durationSeconds > 0.0f) o->setProperty ("windowS", (double) s.durationSeconds);
        Obj::Ptr f = new Obj();
        if (s.integrated > -99.0f) f->setProperty ("lufs", (double) s.integrated);
        if (psrBoth) f->setProperty ("psr", (double) s.psr);
        if (plrBoth) f->setProperty ("plr", (double) s.plr);
        o->setProperty ("figures", juce::var (f.get()));
        return juce::var (o.get());
    };

    Obj::Ptr p = new Obj();
    p->setProperty ("v", 1);
    p->setProperty ("kind", "match_report");
    p->setProperty ("window", whole ? "whole" : "loudest30");
    p->setProperty ("reference", side (in.referenceName, in.ref));
    p->setProperty ("mix", side ("your mix", in.mix));

    juce::Array<juce::var> bands, attribution;
    for (int b = 0; b < 6; ++b)
    {
        const auto& bi = in.bands[(size_t) b];
        Obj::Ptr o = new Obj();
        o->setProperty ("name", matchBandEntry (b).id);   // B1's id (7b); the card's label is matchReportPayloadBandLabel
        if (bi.mixMinusRefDb) o->setProperty ("deltaDb", (double) *bi.mixMinusRefDb);
        o->setProperty ("tier", "bounded");
        if (bi.attribution && bi.attribution->coverage) o->setProperty ("coverage", *bi.attribution->coverage);
        bands.add (juce::var (o.get()));

        if (bi.attribution)
        {
            for (const auto& k : bi.attribution->known)
            {
                Obj::Ptr a = new Obj();
                a->setProperty ("uid", k.uid); a->setProperty ("name", k.name);
                a->setProperty ("band", b); a->setProperty ("share", k.share);
                a->setProperty ("known", true);
                attribution.add (juce::var (a.get()));
            }
            for (const auto& u : bi.attribution->unknown)
            {
                Obj::Ptr a = new Obj();
                a->setProperty ("uid", u.uid); a->setProperty ("name", u.name);
                a->setProperty ("band", b); a->setProperty ("known", false);
                attribution.add (juce::var (a.get()));
            }
        }
    }
    p->setProperty ("bands", bands);

    juce::Array<juce::var> moves;
    for (const auto& l : rep.lines)
    {
        if (l.kind != MatchReportLineKind::Move || ! l.valueDb) continue;
        Obj::Ptr m = new Obj();
        m->setProperty ("text", l.label + "  " + l.text);
        m->setProperty ("deltaDb", (double) *l.valueDb);
        m->setProperty ("band", l.band);
        m->setProperty ("kind", l.moveKind);
        moves.add (juce::var (m.get()));
    }
    p->setProperty ("moves", moves);
    p->setProperty ("attribution", attribution);
    p->setProperty ("preFaderCaveat", rep.caveat);   // C2 section 4's name
    p->setProperty ("busTap", matchBusTapName (in.busTap));
    if (in.crestLog)
    {
        // C2 section 4's shape: { v: 1, hostBlockSize, sides: { reference, mix } }.
        // hostBlockSize is absent, never 0, when the capture's is not known.
        Obj::Ptr c = new Obj();
        c->setProperty ("v", 1);
        if (in.crestLog->hostBlockSize > 0) c->setProperty ("hostBlockSize", in.crestLog->hostBlockSize);
        Obj::Ptr sides = new Obj();
        sides->setProperty ("reference", detail::crestLogSide (in.crestLog->ref));
        sides->setProperty ("mix",       detail::crestLogSide (in.crestLog->mix));
        c->setProperty ("sides", juce::var (sides.get()));
        p->setProperty ("crestLog", juce::var (c.get()));
    }
    if (rep.discriminator.isNotEmpty()) p->setProperty ("discriminator", rep.discriminator);
    Obj::Ptr after = new Obj();
    after->setProperty ("present", false);
    p->setProperty ("after", juce::var (after.get()));
    p->setProperty ("planId", "");
    return juce::JSON::toString (juce::var (p.get()), true);
}

} // namespace echojay
