#pragma once

// =============================================================================
//  K3's PINS: THE MATCH REPORT (mr) AND THE context.match BLOCK (mb).
// =============================================================================
//
// DESIGN_MATCH_TAB rev 7 section 7: MA3 (the report's sentences, one per tier)
// and MA5 (who sees "Make a plan" and any agent wording). C3 rev 2 section 2:
// one mb row per bound or rule, each an in-bound value kept and an out-of-bound
// one clamped or refused as C3 (or the reading named in EJMatchBlock.h) says.
//
// A NEW FILE, so K3 commits apart from round 1. runK3Pins takes the suite's
// check (bool, name, detail); k3_pins_main.cpp runs it alone today, and
// mapfps_test.cpp can call it with one include and one line once round 1 is in.
// =============================================================================

#include "EJMatchReport.h"
#include "EJMatchBlock.h"

namespace k3pins
{
using namespace echojay;

template <typename Check>
inline void runK3Pins (Check&& check)
{
    // ---- mr: the report ----------------------------------------------------

    auto noBands = [] { return std::array<MatchReportBandIn, 6> {}; };
    auto busBand = [] (float D)
    {
        MatchReportBandIn b;
        b.mixMinusRefDb = D;
        b.crestSaysPeaks = false;
        return b;
    };
    auto baseIn = [&]
    {
        MatchReportIn in;
        in.referenceName = "Song X";
        in.mix.integrated = -11.8f; in.ref.integrated = -9.7f;
        in.hasMatchPlan = true; in.agentOn = true;
        in.bands = noBands();
        return in;
    };
    auto lineFor = [] (const MatchReport& r, MatchReportTier t, const juce::String& label) -> const MatchReportLine*
    {
        for (const auto& l : r.lines) if (l.tier == t && l.label == label) return &l;
        return nullptr;
    };
    const juce::String arrow = matchReportArrow();

    // mr PIN1 -- MA3, CAN DO: a bus band move, one sentence, exact.
    {
        auto in = baseIn();
        in.bands[2] = busBand (2.8f);
        const auto r = buildMatchReport (in);
        const auto* l = lineFor (r, MatchReportTier::CanDo, "Low-mids");
        const juce::String want = "2.8 dB hotter than the reference " + arrow + " a 2.8 dB cut on the mix bus";
        check (l != nullptr && l->text == want && l->kind == MatchReportLineKind::Move,
               "mr PIN1: MA3 Can do: \"Low-mids  2.8 dB hotter than the reference -> a 2.8 dB cut on the mix bus\"",
               l ? l->text : juce::String ("no line"));
    }

    // mr PIN2 -- MA3, JUST SO YOU KNOW: a width finding, exact.
    {
        auto in = baseIn();
        in.proposal.findings.push_back ({ MatchFigure::Width, 62.0f, 70.0f, -8.0f });
        const auto r = buildMatchReport (in);
        const auto* l = lineFor (r, MatchReportTier::JustSoYouKnow, "Width");
        check (l != nullptr && l->text == "narrower than the reference (62% vs 70%)",
               "mr PIN2: MA3 Just so you know: \"Width  narrower than the reference (62% vs 70%)\"",
               l ? l->text : juce::String ("no line"));
    }

    // mr PIN3 -- MA3, CAN'T COMPARE: a too-short refusal, in its own words.
    {
        auto in = baseIn();
        MatchRefusal x;
        x.kind = MatchRefusalKind::BandsTooShort;
        x.side = "the capture";
        x.thresholdSeconds = kMatchBandMinSeconds; x.measuredSeconds = 24.0f;
        x.message = matchTooShortMessage ("band proposal", "the capture", kMatchBandMinSeconds, 24.0f);
        in.proposal.refusals.push_back (x);
        const auto r = buildMatchReport (in);
        const auto* l = lineFor (r, MatchReportTier::CantCompare, "Tone");
        check (l != nullptr && l->text == "No band proposal: the capture is 24.0 s long, and a band proposal needs at least 30 s.",
               "mr PIN3: MA3 Can't compare: \"Tone  No band proposal: the capture is 24.0 s long, ...\"",
               l ? l->text : juce::String ("no line"));
    }

    // mr PIN4 to PIN6 -- THE CREST FINDING READS ONLY yes / no / unknown.
    {
        auto withPeaks = [&] (std::optional<bool> peaks)
        {
            auto in = baseIn();
            in.bands[4].mixMinusRefDb = 2.4f;
            in.bands[4].crestSaysPeaks = peaks;
            in.bands[4].crestDb = 99.0f;   // a crest figure nothing may read
            return buildMatchReport (in);
        };
        const auto yes = withPeaks (true), no = withPeaks (false), unk = withPeaks (std::nullopt);
        const auto* y = lineFor (yes, MatchReportTier::JustSoYouKnow, "High-mids");
        check (y != nullptr && y->kind == MatchReportLineKind::PeaksFinding
               && y->text == "The high-mids is 2.4 dB hotter than the reference, and the excess is peaks, not body. "
                             "That's a dynamics job; EQ would cut the body with it."
               && lineFor (yes, MatchReportTier::CanDo, "High-mids") == nullptr,
               "mr PIN4: crest says peaks: no EQ move, a Just so you know line in C4's words without a crest figure",
               y ? y->text : juce::String ("no line"));
        const auto* n = lineFor (no, MatchReportTier::CanDo, "High-mids");
        check (n != nullptr && n->kind == MatchReportLineKind::Move && n->detail.isEmpty()
               && no.discriminator.isEmpty(),
               "mr PIN5: crest says no peaks: the move stays, with no decision 4 sentence");
        const auto* u = lineFor (unk, MatchReportTier::CanDo, "High-mids");
        check (u != nullptr && u->kind == MatchReportLineKind::Move
               && u->detail.contains (attribNoDiscriminatorSentence())
               && unk.discriminator == attribNoDiscriminatorSentence(),
               "mr PIN6: crest unknown: the move stays, with decision 4's sentence on the line and in the payload");
    }

    // mr PIN7 to PIN9 -- MA5: who sees "Make a plan" and any agent wording.
    {
        auto strings = [&] (bool hasPlan, bool agentOn)
        {
            auto in = baseIn();
            in.hasMatchPlan = hasPlan; in.agentOn = agentOn;
            in.bands[2] = busBand (2.8f);
            const auto r = buildMatchReport (in);
            return matchReportAllStrings (r, matchPlanCardAgentOffer (hasPlan, agentOn));
        };
        auto agentWords = [] (const juce::StringArray& s)
        {
            juce::StringArray a;
            for (const auto& x : s) if (x.containsIgnoreCase ("agent")) a.add (x);
            return a;
        };
        bool noPlanOk = true;
        for (bool agentOn : { false, true })
        {
            const auto s = strings (false, agentOn);
            noPlanOk = noPlanOk && ! s.contains (kMatchMakePlanLabel) && agentWords (s).isEmpty();
        }
        check (noPlanOk, "mr PIN7: MA5: a build without match_plan shows no \"Make a plan\" and no string containing \"Agent\"");
        const auto off = agentWords (strings (true, false));
        check (off.size() == 1 && off[0] == "Turn on Agent and do it all" && strings (true, false).contains (kMatchMakePlanLabel),
               "mr PIN8: MA5: with the agent off, the only agent wording is \"Turn on Agent and do it all\"",
               off.joinIntoString (" | "));
        check (agentWords (strings (true, true)).isEmpty(),
               "mr PIN9: MA5: with the agent on there is no agent wording at all");
    }

    // mr PIN10 -- PSR IS WHOLE-FILE ONLY: a line in "whole", none in "loudest30".
    {
        auto psrIn = [&] (MatchWindowMode w)
        {
            auto in = baseIn();
            in.window = w;
            in.mix.psr = 7.1f; in.ref.psr = 9.0f;
            in.proposal.findings.push_back ({ MatchFigure::PSR, 7.1f, 9.0f, -1.9f });
            return buildMatchReport (in);
        };
        const auto whole = psrIn (MatchWindowMode::Whole), l30 = psrIn (MatchWindowMode::Loudest30);
        const auto* w = lineFor (whole, MatchReportTier::JustSoYouKnow, "Dynamics");
        const auto* l = lineFor (l30, MatchReportTier::JustSoYouKnow, "Dynamics");
        check (w != nullptr && w->text == "more squashed than the reference (PSR 7.1 vs 9.0)" && l == nullptr,
               "mr PIN10: the PSR line shows in \"whole\" and never in \"loudest30\"");
    }

    // mr PIN11 -- THE PRE-FADER CAVEAT: when a line names a track, not for the bus.
    {
        auto in = baseIn();
        AttribResult a;
        a.target = AttribTarget::Track; a.rule = AttribRule::R6Single;
        a.links = { { "uid-v", "Lead Vox", 0.8 } };
        a.known = a.links; a.coverage = 0.8; a.valueDb = -3.9f; a.measuredDb = 2.8f;
        a.reason = attribSingleSentence ("Lead Vox", 0.8, 3); a.caveat = true;
        in.bands[3] = busBand (2.8f);
        in.bands[3].attribution = a;
        const auto t = buildMatchReport (in);
        auto in2 = baseIn();
        in2.bands[3] = busBand (2.8f);
        const auto b = buildMatchReport (in2);
        const auto* l = lineFor (t, MatchReportTier::CanDo, "Mids");
        check (t.caveat && ! b.caveat && l != nullptr
               && l->text.endsWith ("a 3.9 dB cut on Lead Vox")
               && matchReportText (t).contains (attribPreFaderCaveatSentence())
               && ! matchReportText (b).contains (attribPreFaderCaveatSentence()),
               "mr PIN11: the pre-fader caveat shows when a Can do line names a track, and not for the bus alone");
    }

    // mr PIN12 -- THE PAYLOAD: "share", never "ratio"; a figure not measured is absent, never 0.
    {
        auto in = baseIn();
        in.ref.integrated = -100.0f;   // unavailable
        AttribResult a;
        a.target = AttribTarget::Track; a.links = { { "uid-g", "Gtr L", 0.62 } };
        a.known = a.links; a.coverage = 0.7; a.valueDb = -4.0f;
        in.bands[2] = busBand (2.8f);
        in.bands[2].attribution = a;
        const auto r = buildMatchReport (in);
        const juce::var p = juce::JSON::parse (matchReportPayload (in, r));
        const juce::var att = p.getProperty ("attribution", juce::var());
        const juce::var a0 = att.size() > 0 ? att[0] : juce::var();
        const juce::var refFig = p.getProperty ("reference", juce::var()).getProperty ("figures", juce::var());
        const juce::var b2 = p.getProperty ("bands", juce::var())[2];
        const juce::var b0 = p.getProperty ("bands", juce::var())[0];
        check (a0.hasProperty ("share") && ! a0.hasProperty ("ratio")
               && std::abs ((double) a0.getProperty ("share", 0) - 0.62) < 1e-9
               && ! refFig.hasProperty ("lufs") && ! b0.hasProperty ("deltaDb") && ! b2.hasProperty ("crest")
               && b2.getProperty ("peaks", juce::var()).toString() == "no",
               "mr PIN12: the payload says \"share\", not \"ratio\", and a figure not measured is absent, never 0",
               matchReportPayload (in, r).substring (0, 200));
    }

    // mr PIN13 -- NOTHING TO PLAN, NO "Make a plan": refusals and under-floor bands only.
    {
        auto in = baseIn();
        in.bands[5] = busBand (-1.6f);
        MatchRefusal x; x.kind = MatchRefusalKind::DynamicsTooShort; x.message = "No loudness or dynamics proposal.";
        in.proposal.refusals.push_back (x);
        const auto r = buildMatchReport (in);
        const auto* l = lineFor (r, MatchReportTier::CanDo, "Air");
        check (std::find (r.buttons.begin(), r.buttons.end(), juce::String (kMatchMakePlanLabel)) == r.buttons.end()
               && l != nullptr && l->kind == MatchReportLineKind::UnderFloor
               && l->text == "1.6 dB cooler than the reference " + arrow + " (under the 2.0 dB measurement floor; leaving it)",
               "mr PIN13: only refusals and under-floor bands: the report offers no \"Make a plan\"");
    }

    // mr PIN14 -- THE LAYOUT: one painted zone per button, inside the card, none overlapping.
    {
        auto in = baseIn();
        in.bands[2] = busBand (2.8f);
        const auto r = buildMatchReport (in);
        auto tw = [] (const juce::String& s) { return s.length() * 7; };
        const auto L = layoutMatchReport (r, 260, tw);
        bool ok = L.zones.size() == r.buttons.size() && r.buttons.size() == 3;
        for (size_t i = 0; ok && i < L.zones.size(); ++i)
        {
            const auto z = L.zones[i].r;
            ok = L.zones[i].label == r.buttons[i] && z.getX() >= 0 && z.getRight() <= 260 - kMatchReportPad
              && z.getBottom() <= L.height;
            for (size_t j = 0; ok && j < i; ++j) ok = ! z.intersects (L.zones[j].r);
            for (const auto& row : L.rows) ok = ok && ! z.intersects (row.r);
        }
        check (ok, "mr PIN14: the layout gives one zone per button, inside the card, overlapping nothing");
    }

    // ---- mb: the context.match v1 block (C3 section 2) ---------------------

    auto base = []
    {
        return juce::JSON::parse (R"({
          "v": 1, "reference": { "id": "ref_8c1d", "name": "Song X" }, "window": "whole",
          "mix": { "integrated": -10.2, "loudest30": -8.1, "truePeak": -0.8, "overs": 0, "lra": 6.5,
                   "psr": 8.9, "plr": 10.1, "width": 64, "corr": 0.41,
                   "bandsRel": [ -3.0, 1.2, 2.9, 0.4, -0.6, -1.9 ], "bandCrest": [ 9.1, 8.4, 7.9, 8.8, 9.6, 10.2 ] },
          "ref": { "integrated": -9.0, "loudest30": -7.0, "truePeak": -1.0, "overs": 0, "lra": 7.0,
                   "psr": 9.5, "plr": 11.0, "width": 70, "corr": 0.5,
                   "bandsRel": [ -2.0, 1.0, 0.1, 0.4, -0.2, -0.9 ], "bandCrest": null },
          "moves": [ { "kind": "band", "band": "lowMid", "valueDb": -2.8, "measuredDb": 2.8,
                       "attribution": { "target": "pair", "links": [ { "uid": "a1", "name": "Gtr L", "share": 0.4 },
                                                                    { "uid": "a2", "name": "Gtr R", "share": 0.35 } ],
                                        "combinedShare": 0.75, "reason": "x" } },
                     { "kind": "gain", "valueDb": 1.2, "measuredDb": 1.2, "attribution": null } ],
          "findings": [ { "figure": "width", "mix": 62, "ref": 70, "difference": -8 } ],
          "refusals": [] })");
    };
    auto set = [] (juce::var o, const juce::String& path, const juce::var& v)
    {
        juce::StringArray parts; parts.addTokens (path, ".", "");
        juce::var cur = o;
        for (int i = 0; i < parts.size() - 1; ++i)
        {
            const juce::var next = parts[i].containsOnly ("0123456789") ? cur[parts[i].getIntValue()]
                                                                        : cur.getProperty (parts[i], juce::var());
            cur = next;
        }
        if (auto* d = cur.getDynamicObject()) d->setProperty (parts[parts.size() - 1], v);
        return o;
    };
    auto read = [] (const juce::var& b) { return readMatchBlock (b); };
    auto field = [] (const MatchBlockRead& r, const char* side, const char* f)
    {
        return r.block.getProperty (side, juce::var()).getProperty (f, juce::var());
    };
    auto movesOf = [] (const MatchBlockRead& r) { return r.block.getProperty ("moves", juce::var()).size(); };

    check (read (base()).readable && read (base()).dropped.isEmpty(),
           "mb PIN0 premise: the base block is readable and drops nothing", read (base()).dropped.joinIntoString ("; "));

    // mb PIN1 -- not an object: unreadable.
    check (! read (juce::var ("match")).readable && ! read (juce::var()).readable,
           "mb PIN1: a block that is not an object is unreadable (409 match_unreadable)");
    // mb PIN2 -- v must be 1.
    check (! read (set (base(), "v", 2)).readable && read (set (base(), "v", 1)).readable,
           "mb PIN2: v 2 is unreadable, v 1 reads");
    // mb PIN3 -- bandsRel exactly 6 numbers.
    {
        juce::Array<juce::var> five { 1.0, 2.0, 3.0, 4.0, 5.0 }, sixStr { 1.0, 2.0, 3.0, 4.0, 5.0, "6" };
        check (! read (set (base(), "mix.bandsRel", five)).readable && ! read (set (base(), "ref.bandsRel", sixStr)).readable,
               "mb PIN3: bandsRel with 5 entries, or 6 with one not a number, is unreadable");
    }
    // mb PIN4 -- a bandsRel entry outside -30..30 (reading R2).
    {
        juce::Array<juce::var> edge { -30.0, 30.0, 0.0, 0.0, 0.0, 0.0 }, over { -30.0, 30.1, 0.0, 0.0, 0.0, 0.0 };
        check (read (set (base(), "mix.bandsRel", edge)).readable && ! read (set (base(), "mix.bandsRel", over)).readable,
               "mb PIN4: a bandsRel entry at 30 reads, at 30.1 the block is unreadable");
    }
    // mb PIN5 -- corrSub is never sent (reading R3).
    check (! read (set (base(), "ref.corrSub", 0.3)).readable && ! read (set (base(), "corrSub", 0.3)).readable,
           "mb PIN5: corrSub present anywhere refuses the block");
    // mb PIN6 -- reference.name trimmed to 80.
    {
        const juce::String n81 = juce::String::repeatedString ("a", 81);
        const auto r = read (set (base(), "reference.name", n81));
        const auto r80 = read (set (base(), "reference.name", n81.substring (0, 80)));
        check (r.readable && r.block.getProperty ("reference", juce::var()).getProperty ("name", juce::var()).toString().length() == 80
               && r80.dropped.isEmpty(),
               "mb PIN6: a reference name of 81 characters is trimmed to 80; 80 is left alone");
    }
    // mb PIN7 -- window: "whole" or "loudest30", absent means "whole" (reading R5).
    {
        auto b = base();
        b.getDynamicObject()->removeProperty ("window");
        const auto r = read (b);
        check (! read (set (base(), "window", "loudest20")).readable && r.readable
               && r.block.getProperty ("window", juce::var()).toString() == "whole",
               "mb PIN7: an unknown window is unreadable, and an absent one is \"whole\"");
    }
    // mb PIN8 -- "loudest30" needs loudest30 on both sides (reading R6).
    check (read (set (base(), "window", "loudest30")).readable
           && ! read (set (set (base(), "window", "loudest30"), "ref.loudest30", juce::var())).readable,
           "mb PIN8: in \"loudest30\" a side without loudest30 is unreadable");

    // mb PIN9 to PIN17 -- SIDE figures: in bounds kept, out of bounds sent as null (reading R1).
    auto sideRow = [&] (const char* f, juce::var in, juce::var out)
    {
        const auto a = read (set (base(), juce::String ("mix.") + f, in));
        const auto b = read (set (base(), juce::String ("mix.") + f, out));
        return a.readable && a.dropped.isEmpty() && field (a, "mix", f) == in
            && b.readable && field (b, "mix", f).isVoid() && b.dropped.size() >= 1;
    };
    check (sideRow ("integrated", -70.0, -70.1), "mb PIN9: integrated -70 is kept, -70.1 is sent as null");
    check (sideRow ("loudest30", 0.0, 0.1), "mb PIN10: loudest30 0 is kept, 0.1 is sent as null");
    check (sideRow ("truePeak", 6.0, 6.1), "mb PIN11: truePeak +6 is kept, +6.1 is sent as null");
    check (sideRow ("overs", 100000, 100001) && sideRow ("overs", 0, 1.5),
           "mb PIN12: overs 100000 is kept; 100001, and 1.5 (not an integer), are sent as null");
    check (sideRow ("lra", 40.0, 40.1), "mb PIN13: lra 40 is kept, 40.1 is sent as null");
    {
        const auto one = read (set (base(), "ref.psr", juce::var()));
        const auto l30 = read (set (base(), "window", "loudest30"));
        check (sideRow ("psr", 40.0, 40.1)
               && field (one, "mix", "psr").isVoid() && field (one, "ref", "psr").isVoid()
               && field (l30, "mix", "psr").isVoid() && field (l30, "ref", "psr").isVoid(),
               "mb PIN14: psr 0..40 on both sides in \"whole\"; one-sided, or in \"loudest30\", null on both");
    }
    {
        const auto one = read (set (base(), "mix.plr", juce::var()));
        const auto l30 = read (set (base(), "window", "loudest30"));
        check (sideRow ("plr", 40.0, 40.1)
               && field (one, "mix", "plr").isVoid() && field (one, "ref", "plr").isVoid()
               && field (l30, "mix", "plr").isVoid() && field (l30, "ref", "plr").isVoid(),
               "mb PIN15: plr 0..40 on both sides in \"whole\"; one-sided, or in \"loudest30\", null on both");
    }
    check (sideRow ("width", 200.0, 200.1), "mb PIN16: width 200 is kept, 200.1 is sent as null");
    check (sideRow ("corr", -1.0, -1.01), "mb PIN17: corr -1 is kept, -1.01 is sent as null");
    // mb PIN18 -- bandCrest: exactly 6 in 0..40, else null (reading R7).
    {
        juce::Array<juce::var> edge { 0.0, 40.0, 1.0, 1.0, 1.0, 1.0 }, over { 0.0, 40.1, 1.0, 1.0, 1.0, 1.0 },
                               five { 1.0, 1.0, 1.0, 1.0, 1.0 };
        const auto a = read (set (base(), "mix.bandCrest", edge));
        check (field (a, "mix", "bandCrest").size() == 6
               && field (read (set (base(), "mix.bandCrest", over)), "mix", "bandCrest").isVoid()
               && field (read (set (base(), "mix.bandCrest", five)), "mix", "bandCrest").isVoid(),
               "mb PIN18: bandCrest of 6 figures in 0..40 is kept; 40.1, or 5 figures, is sent as null");
    }
    // mb PIN19 to PIN21 -- at most 8 moves, findings and refusals (reading R4).
    auto listOf = [&] (const char* key, int n)
    {
        auto b = base();
        const juce::var list = b.getProperty (key, juce::var());
        const juce::var one = list.size() > 0 ? list[0]
                                              : juce::JSON::parse (R"({"kind":"too_short","side":"mix","message":"m"})");
        juce::Array<juce::var> a;
        for (int i = 0; i < n; ++i) a.add (one);
        return set (b, key, a);
    };
    check (read (listOf ("moves", 8)).readable && ! read (listOf ("moves", 9)).readable,
           "mb PIN19: 8 moves read, 9 refuse the block");
    check (read (listOf ("findings", 8)).readable && ! read (listOf ("findings", 9)).readable,
           "mb PIN20: 8 findings read, 9 refuse the block");
    check (read (listOf ("refusals", 8)).readable && ! read (listOf ("refusals", 9)).readable,
           "mb PIN21: 8 refusals read, 9 refuse the block");
    // mb PIN22 to PIN24 -- a move's own fields: an out-of-bound one drops the move.
    check (movesOf (read (set (base(), "moves.0.valueDb", -30.0))) == 2 && movesOf (read (set (base(), "moves.0.valueDb", -30.1))) == 1
           && movesOf (read (set (base(), "moves.1.valueDb", 30.0))) == 2,
           "mb PIN22: valueDb -30 and +30 keep the move, -30.1 drops it");
    check (movesOf (read (set (base(), "moves.0.measuredDb", 30.0))) == 2 && movesOf (read (set (base(), "moves.0.measuredDb", 30.1))) == 1,
           "mb PIN23: measuredDb 30 keeps the move, 30.1 drops it");
    check (movesOf (read (set (base(), "moves.1.band", "low"))) == 1 && movesOf (read (set (base(), "moves.0.band", "lowMids"))) == 1,
           "mb PIN24: a band on a gain move, or a band name outside B1's six, drops the move");
    // mb PIN25 to PIN27 -- the attribution: a bad one is dropped, the move stays (reading R8).
    auto attrOf = [] (const MatchBlockRead& r) { return r.block.getProperty ("moves", juce::var())[0].getProperty ("attribution", juce::var()); };
    {
        auto b4 = base(), b5 = base();
        const juce::var lk = juce::JSON::parse (R"({"uid":"a3","name":"Keys","share":0.1})");
        for (auto* b : { &b4, &b5 })
        {
            juce::var links = b->getProperty ("moves", juce::var())[0].getProperty ("attribution", juce::var()).getProperty ("links", juce::var());
            const int n = b == &b4 ? 2 : 3;
            for (int i = 0; i < n; ++i) links.append (lk);
        }
        const auto r4 = read (b4), r5 = read (b5);
        check (attrOf (r4).isObject() && attrOf (r5).isVoid() && movesOf (r5) == 2,
               "mb PIN25: 4 links keep the attribution, 5 drop it and keep the move");
    }
    check (attrOf (read (set (base(), "moves.0.attribution.links.0.share", 8.0))).isObject()
           && attrOf (read (set (base(), "moves.0.attribution.links.0.share", 8.1))).isVoid(),
           "mb PIN26: a share of 8 keeps the attribution, 8.1 drops it");
    check (attrOf (read (set (base(), "moves.0.attribution.target", "track"))).isVoid()
           && attrOf (read (set (set (base(), "moves.0.attribution.target", "track"), "moves.0.attribution.combinedShare", juce::var()))).isObject(),
           "mb PIN27: combinedShare is for a pair only; on a track it drops the attribution");
    // mb PIN28 -- a finding out of its figure's bounds, or a psr finding when psr is not sent (reading R9).
    {
        const auto bad = read (set (base(), "findings.0.mix", 200.1));
        auto psrF = set (set (base(), "findings.0.figure", "psr"), "findings.0.mix", 8.9);
        psrF = set (set (psrF, "findings.0.ref", 9.5), "findings.0.difference", -0.6);
        const auto psrWhole = read (psrF);
        const auto psrL30 = read (set (psrF, "window", "loudest30"));
        auto nf = [] (const MatchBlockRead& r) { return r.block.getProperty ("findings", juce::var()).size(); };
        check (nf (bad) == 0 && nf (psrWhole) == 1 && nf (psrL30) == 0,
               "mb PIN28: a width finding at 200.1 is dropped; a psr finding stays in \"whole\" and is dropped in \"loudest30\"");
    }
    // mb PIN29 -- THE WRITER: C3's field names, exactly, and nothing else.
    {
        MatchBlockIn in;
        in.referenceId = "ref_1"; in.referenceName = "Song X";
        in.mix.integrated = -10.0; in.ref.integrated = -9.0;
        in.mix.psr = 8.0; in.ref.psr = 9.0;
        MatchBlockMove m; m.kind = "band"; m.band = 2; m.valueDb = -2.8; m.measuredDb = 2.8;
        m.attribution = MatchBlockAttribution { "bus", {}, std::nullopt, "spread" };
        in.moves.push_back (m);
        const auto r = buildMatchBlock (in);
        auto keys = [] (const juce::var& v)
        {
            juce::StringArray k;
            if (auto* d = v.getDynamicObject()) for (const auto& p : d->getProperties()) k.add (p.name.toString());
            k.sort (false);
            return k.joinIntoString (",");
        };
        const juce::var mix = r.block.getProperty ("mix", juce::var());
        const juce::var mvs = r.block.getProperty ("moves", juce::var());
        const juce::var mv = mvs.size() > 0 ? mvs[0] : juce::var();
        check (r.readable
               && keys (r.block) == "findings,mix,moves,ref,reference,refusals,v,window"
               && keys (mix) == "bandCrest,bandsRel,corr,integrated,loudest30,lra,overs,plr,psr,truePeak,width"
               && keys (mv) == "attribution,band,kind,measuredDb,valueDb"
               && keys (mv.getProperty ("attribution", juce::var())) == "combinedShare,links,reason,target"
               && mv.getProperty ("band", juce::var()).toString() == "lowMid"
               && (double) mix.getProperty ("psr", juce::var()) == 8.0,
               "mb PIN29: the writer uses C3 section 2's field names and no others, and never writes corrSub",
               keys (r.block) + " | " + keys (mix));
    }
}

} // namespace k3pins
