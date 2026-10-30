#pragma once

// =============================================================================
//  ROUND 5d's PINS: THE REPORT AND "MAKE A PLAN", THROUGH MatchHost (kh).
// =============================================================================
//
// kh PIN1       the block from a synthetic capture, field for field against C3
//               rev 3 section 2;
// kh PIN2..12   each staging fixture's request.match rebuilt by our writer from
//               the fixture's own values, its track and pair moves through
//               C4's routing from the shares and D it gives; every difference
//               is listed, and the list is the row's expectation;
// kh PIN13      when "Make a plan" is offered;
// kh PIN14      MA5's agent wording;
// kh PIN15      the press posts the match_report;
// kh PIN16, 17  "Make a plan": draftPlan, then openSessionChatAt on ok, or the
//               callback's sentence on failure.
// =============================================================================

#include "EJMatchHandOff.h"
#include "k5_fakehost.h"

namespace k5dpins
{
using namespace echojay;
using k5pins::FakeMatchHost;

/** v[i], or void when v is not an array that long: JUCE's operator[] reads
    through a null array in a Release build, and a pin must fail, not crash. */
inline juce::var elem (const juce::var& v, int i)
{
    return (v.isArray() && i >= 0 && i < v.size()) ? v[i] : juce::var();
}

/** Every path at which two JSON values differ: a key on one side only, a type
    or a value that differs (numbers within tol), an array element past the
    other's end. Paths read "mix.bandCrest", "moves[0].attribution.links[0]". */
inline void jsonDiff (const juce::var& a, const juce::var& b, const juce::String& path,
                      juce::StringArray& out, double tol)
{
    const bool an = matchBlockIsNumber (a), bn = matchBlockIsNumber (b);
    if (an || bn)
    {
        if (! (an && bn) || std::abs ((double) a - (double) b) > tol) out.add (path);
        return;
    }
    if (a.isArray() && b.isArray())
    {
        const int n = juce::jmax (a.size(), b.size());
        for (int i = 0; i < n; ++i)
        {
            const juce::String sub = path + "[" + juce::String (i) + "]";
            if (i >= a.size() || i >= b.size()) { out.add (sub); continue; }
            const juce::var av = a[i];
            const juce::var bv = b[i];
            jsonDiff (av, bv, sub, out, tol);
        }
        return;
    }
    // ARRAYS FIRST: a juce::var array also answers isObject(), with no DynamicObject.
    if (a.getDynamicObject() != nullptr && b.getDynamicObject() != nullptr)
    {
        juce::StringArray keys;
        for (const auto& p : a.getDynamicObject()->getProperties()) keys.addIfNotAlreadyThere (p.name.toString());
        for (const auto& p : b.getDynamicObject()->getProperties()) keys.addIfNotAlreadyThere (p.name.toString());
        keys.sort (false);
        for (const auto& k : keys)
        {
            const juce::String sub = path.isEmpty() ? k : path + "." + k;
            if (! a.hasProperty (k) || ! b.hasProperty (k)) { out.add (sub); continue; }
            const juce::var av = a.getProperty (k, juce::var());
            const juce::var bv = b.getProperty (k, juce::var());
            jsonDiff (av, bv, sub, out, tol);
        }
        return;
    }
    if (a.isVoid() && b.isVoid()) return;
    if (a.isString() && b.isString() && a.toString() == b.toString()) return;
    out.add (path);
}

/** A fixture's request.match as our writer's input. */
inline MatchBlockIn blockInFromFixture (const juce::var& m)
{
    auto num = [] (const juce::var& o, const char* k) -> std::optional<double>
    {
        const juce::var x = o.getProperty (k, juce::var());
        if (matchBlockIsNumber (x)) return (double) x;
        return std::nullopt;
    };
    auto side = [&] (const juce::var& s)
    {
        MatchBlockSide o;
        o.integrated = num (s, "integrated"); o.loudest30 = num (s, "loudest30");
        o.truePeak = num (s, "truePeak");
        if (const auto ov = num (s, "overs")) o.overs = (int) *ov;
        o.lra = num (s, "lra"); o.psr = num (s, "psr"); o.plr = num (s, "plr");
        o.width = num (s, "width"); o.corr = num (s, "corr");
        const juce::var rel = s.getProperty ("bandsRel", juce::var());
        for (int i = 0; i < 6 && i < rel.size(); ++i) { const juce::var e = rel[i]; o.bandsRel[(size_t) i] = (double) e; }
        return o;
    };
    MatchBlockIn in;
    const juce::var ref = m.getProperty ("reference", juce::var());
    in.referenceId = ref.getProperty ("id", juce::var()).toString();
    in.referenceName = ref.getProperty ("name", juce::var()).toString();
    in.window = m.getProperty ("window", "whole").toString();
    const juce::var ms = m.getProperty ("mix", juce::var());
    const juce::var rs = m.getProperty ("ref", juce::var());
    in.mix = side (ms); in.ref = side (rs);
    const juce::var moves = m.getProperty ("moves", juce::var());
    for (int i = 0; i < moves.size(); ++i)
    {
        const juce::var mv = moves[i];
        MatchBlockMove o;
        o.kind = mv.getProperty ("kind", juce::var()).toString();
        o.band = matchBlockBandIndex (mv.getProperty ("band", juce::var()).toString());
        o.valueDb = *num (mv, "valueDb"); o.measuredDb = *num (mv, "measuredDb");
        const juce::var at = mv.getProperty ("attribution", juce::var());
        if (at.isObject())
        {
            MatchBlockAttribution a;
            a.target = at.getProperty ("target", juce::var()).toString();
            const juce::var links = at.getProperty ("links", juce::var());
            for (int k = 0; k < links.size(); ++k)
            {
                const juce::var l = links[k];
                a.links.push_back ({ l.getProperty ("uid", juce::var()).toString(),
                                     l.getProperty ("name", juce::var()).toString(), *num (l, "share") });
            }
            a.combinedShare = num (at, "combinedShare");
            a.reason = at.getProperty ("reason", juce::var()).toString();
            o.attribution = a;
        }
        in.moves.push_back (o);
    }
    const juce::var fs = m.getProperty ("findings", juce::var());
    for (int i = 0; i < fs.size(); ++i)
    {
        const juce::var f = fs[i];
        in.findings.push_back ({ f.getProperty ("figure", juce::var()).toString(),
                                 *num (f, "mix"), *num (f, "ref"), *num (f, "difference") });
    }
    const juce::var rf = m.getProperty ("refusals", juce::var());
    for (int i = 0; i < rf.size(); ++i)
    {
        const juce::var r = rf[i];
        in.refusals.push_back ({ r.getProperty ("kind", juce::var()).toString(),
                                 r.getProperty ("side", juce::var()).toString(),
                                 r.getProperty ("message", juce::var()).toString() });
    }
    return in;
}

/** "From the same inputs where the fixture gives them": a track or pair move
    carries its Links' shares and its D, so C4's routing (attributeBandMove)
    places and sizes it again from those. A bus move gives no shares, so it is
    kept as given. */
inline void rerouteFromShares (MatchBlockIn& in)
{
    for (auto& m : in.moves)
    {
        if (m.kind != "band" || ! m.attribution) continue;
        if (m.attribution->target != "track" && m.attribution->target != "pair") continue;
        AttribIn a;
        a.band = m.band; a.busPower = 1.0; a.busHopCount = 100;
        a.D = (float) m.measuredDb; a.busTap = MatchBusTap::Output;
        for (const auto& l : m.attribution->links)
        {
            AttribLinkIn li;
            li.uid = l.uid; li.name = l.name; li.postTrimPower = l.share;
            li.gainDb = 0.0f; li.hopCount = 100; li.frameCoverage = 1.0;
            a.links.push_back (li);
        }
        const auto r = attributeBandMove (a);
        if (r.valueDb) m.valueDb = *r.valueDb;
        m.attribution = matchAttributionPlaced (std::optional<AttribResult> (r))
                            ? std::optional<MatchBlockAttribution> (matchBlockAttributionOf (r))
                            : std::nullopt;
    }
}

inline juce::File fixtureDir()
{
    const juce::String here (__FILE__);
    const juce::File self = juce::File::isAbsolutePath (here) ? juce::File (here)
                                                              : juce::File::getCurrentWorkingDirectory().getChildFile (here);
    const auto beside = self.getSiblingFile ("fixtures").getChildFile ("match-wire");
    if (beside.isDirectory()) return beside;
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tools/mapfps_test/k3/fixtures/match-wire");
}

template <typename Check>
inline void runK5dPins (Check&& check)
{
    auto near = [] (double a, double b, double t) { return std::abs (a - b) <= t; };

    // ---- a synthetic session: a 60 s capture with the host's channel and two
    // Links, a reference, Whole song.
    auto series = [] (std::size_t n, bool alternate, float lufs, float lowMid)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            h.shortTermLufs = i < 29 ? -100.0f : (alternate ? (i % 2 == 0 ? -10.0f : -20.0f) : lufs);
            h.shortTermTruePeak = h.shortTermLufs + 8.0f;
            h.bandPower.fill (1.0f);
            h.bandPower[2] = lowMid;
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    auto channel = [&] (const juce::String& uid, const juce::String& name, float lowMid)
    {
        ChannelMeterData ch;
        ch.uid = uid; ch.name = name; ch.gainDb = 0.0f;
        ch.hopFrames = series (600, false, -20.0f, lowMid);
        for (auto& h : ch.hopFrames) for (std::size_t b = 0; b < 6; ++b) if (b != 2) h.bandPower[b] = 0.01f;
        ch.hops = 600;
        return ch;
    };
    auto session = [&]
    {
        FakeMatchHost host;
        CaptureSnapshot c;
        c.name = "Capture 1";
        c.hopFrames = series (600, true, 0.0f, 1.0f);
        c.hopFramesHops = 600;
        c.busGainDbAtCapture = 0.0f;
        ChannelMeterData self; self.name = "Mix Bus"; self.hopFrames = c.hopFrames; self.hops = 600;
        c.channels = { self, channel ("bb411b8240", "Gtr L", 0.40f), channel ("cc522c9351", "Gtr R", 0.35f) };
        host.snaps = { c };
        ReferenceResult r;
        r.name = "Song X"; r.path = "/refs/song-x.wav";
        r.hopFrames = series (1800, false, -12.0f, 1.0f); r.hops = 1800;
        host.ref = r;
        host.links = { { "bb411b8240", "Gtr L", "", {}, 600 }, { "cc522c9351", "Gtr R", "", {}, 600 } };
        host.mixSlot = k5pins::snapshotSlot (0);
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
        host.caps = { "plan_card", "match_plan" };
        return host;
    };
    auto sides = [] (float mixSeconds)
    {
        MatchSide m, r;
        m.macro = { 0.0f, 0.0f, 3.36f, 0.0f, 0.0f, 0.0f };   // lowMid 2.8 dB over the six-band mean
        m.hasMacro = true; m.macroReduction = SpectralReduction::WholeWindowAverage;
        m.durationSeconds = mixSeconds; m.integrated = -14.0f; m.truePeak = -1.0f; m.overs = 0;
        m.lra = 6.0f; m.psr = 8.0f; m.plr = 11.0f; m.width = 64.0f; m.correlation = 0.40f;
        r.macro.fill (0.0f); r.hasMacro = true; r.macroReduction = SpectralReduction::WholeFileAverage;
        r.durationSeconds = 180.0f; r.integrated = -12.0f; r.truePeak = -0.5f; r.overs = 0;
        r.lra = 7.0f; r.psr = 9.0f; r.plr = 12.0f; r.width = 70.0f; r.correlation = 0.50f;
        return MatchSides { m, r };
    };
    auto keysOf = [] (const juce::var& o)
    {
        juce::StringArray k;
        if (auto* d = o.getDynamicObject()) for (const auto& p : d->getProperties()) k.add (p.name.toString());
        k.sort (false);
        return k.joinIntoString (",");
    };

    // kh PIN1 -- THE BLOCK FROM A SYNTHETIC CAPTURE MATCHES C3 rev 3 FIELD FOR FIELD.
    {
        auto host = session();
        const auto view = matchPageViewOf (host, sides (60.0f), host.matchSlots());
        const auto h = matchHandOffOf (host, view);
        const auto r = matchPlanBlockOf (h);
        const juce::var b = r.block;
        const juce::var mix = b.getProperty ("mix", juce::var());
        const juce::var ref = b.getProperty ("ref", juce::var());
        const juce::var moves = b.getProperty ("moves", juce::var());
        const juce::var gain = elem (moves, 0);
        const juce::var band = elem (moves, 1);
        const juce::var at = band.getProperty ("attribution", juce::var());
        const juce::var links = at.getProperty ("links", juce::var());
        const juce::var l0 = elem (links, 0);
        const juce::var l1 = elem (links, 1);
        const juce::var finds = b.getProperty ("findings", juce::var());
        const juce::var rel = mix.getProperty ("bandsRel", juce::var());
        const juce::var rid = b.getProperty ("reference", juce::var());
        juce::StringArray figs;
        for (int i = 0; i < finds.size(); ++i) { const juce::var f = finds[i]; figs.add (f.getProperty ("figure", juce::var()).toString()); }
        const juce::String sideKeys = "bandCrest,bandsRel,corr,integrated,loudest30,lra,overs,plr,psr,truePeak,width";
        const bool shape = r.readable && r.dropped.isEmpty()
            && keysOf (b) == "findings,mix,moves,ref,reference,refusals,v,window"
            && keysOf (mix) == sideKeys && keysOf (ref) == sideKeys
            && keysOf (rid) == "id,name"
            && keysOf (gain) == "attribution,kind,measuredDb,valueDb"
            && keysOf (band) == "attribution,band,kind,measuredDb,valueDb"
            && keysOf (at) == "combinedShare,links,reason,target" && keysOf (l0) == "name,share,uid";
        const bool values = (int) b.getProperty ("v", 0) == 1 && b.getProperty ("window", "").toString() == "whole"
            && rid.getProperty ("name", "").toString() == "Song X"
            && rid.getProperty ("id", "").toString() == matchReferenceId ("/refs/song-x.wav")
            && rid.getProperty ("id", "").toString().matchesWildcard ("ref_????????", false)
            && near ((double) mix.getProperty ("integrated", 0), -14.0, 1e-4)
            && near ((double) mix.getProperty ("loudest30", 0), 10.0 * std::log10 ((0.1 + 0.01) / 2.0), 1e-3)
            && near ((double) ref.getProperty ("loudest30", 0), -12.0, 1e-4)
            && mix.getProperty ("bandCrest", 1).isVoid() && ref.getProperty ("bandCrest", 1).isVoid()
            && near ((double) mix.getProperty ("psr", 0), 8.0, 1e-4) && near ((double) mix.getProperty ("width", 0), 64.0, 1e-4)
            && rel.size() == 6 && near ((double) elem (rel, 2), 2.8, 1e-4) && near ((double) elem (rel, 0), -0.56, 1e-4)
            && gain.getProperty ("kind", "").toString() == "gain"
            && near ((double) gain.getProperty ("valueDb", 0), 2.0, 1e-4)
            && near ((double) gain.getProperty ("measuredDb", 0), -2.0, 1e-4)
            && gain.getProperty ("attribution", 1).isVoid()
            && band.getProperty ("band", "").toString() == "lowMid"
            && near ((double) band.getProperty ("measuredDb", 0), 2.8, 1e-4)
            && near ((double) band.getProperty ("valueDb", 0), -4.4, 0.05)
            && at.getProperty ("target", "").toString() == "pair"
            && near ((double) at.getProperty ("combinedShare", 0), 0.75, 1e-4)
            && l0.getProperty ("name", "").toString() == "Gtr L" && near ((double) l0.getProperty ("share", 0), 0.40, 1e-4)
            && l1.getProperty ("uid", "").toString() == "cc522c9351" && near ((double) l1.getProperty ("share", 0), 0.35, 1e-4)
            && figs.joinIntoString (",") == "lra,psr,plr,width,corr";
        // A 24 s capture: the refusals, in C3's words.
        auto host2 = session();
        const auto h2 = matchHandOffOf (host2, matchPageViewOf (host2, sides (24.0f), host2.matchSlots()));
        const auto r2 = matchPlanBlockOf (h2);
        const juce::var refs = r2.block.getProperty ("refusals", juce::var());
        const juce::var ref0 = elem (refs, 0);
        const bool refusals = r2.readable && refs.size() == 2
            && ref0.getProperty ("kind", "").toString() == "too_short"
            && ref0.getProperty ("side", "").toString() == "mix"
            && ref0.getProperty ("message", "").toString().contains ("24");
        check (shape && values && refusals,
               "kh PIN1: the block from a synthetic capture is C3 rev 3's, field for field: keys, the whole-file "
               "and loudest-30 figures, bandCrest null, the gain and the pair move with C4's x, the findings and the refusals",
               juce::String (shape ? "" : "SHAPE ") + (values ? "" : "VALUES ") + (refusals ? "" : "REFUSALS ")
                   + juce::JSON::toString (b, true).substring (0, 900));
    }

    // kh PIN2 .. PIN12 -- EACH STAGING FIXTURE, REBUILT. The expected
    // differences were listed before the first run (round 5 predictions):
    //   every fixture  mix.bandCrest, ref.bandCrest (numbers there; null here,
    //                  C4 rev 4);
    //   clean          findings[1] (a crest finding; none is written, C4 rev 4);
    //   attribution-pair  NONE since 6a: the reasons name the band from the
    //                  one table ("the low-mids"), as B's do;
    //   track-near-limit  the move itself: a share of 0.85 cannot take 9.1 dB
    //                  off the mix (C4 section 5 reaches 8.24), so C4 sends it
    //                  to the bus at -9.1, where the fixture has -12 on the track.
    {
        const juce::StringArray common { "mix.bandCrest", "ref.bandCrest" };
        struct Expect { const char* file; juce::StringArray extra; };
        const std::vector<Expect> expect {
            { "air-shelf.json", {} },
            { "attribution-pair.json", {} },
            { "bus-partial.json", {} },
            { "bus-widened.json", {} },
            { "clean.json", { "findings[1]" } },
            { "mapped-stand-in.json", {} },
            { "push-warning.json", {} },
            { "refusals-only.json", {} },
            { "track-cant-air.json", {} },
            { "track-near-limit.json", { "moves[0].attribution.links[0]", "moves[0].attribution.reason",
                                         "moves[0].attribution.target", "moves[0].valueDb" } },
            { "v1-refused.json", {} } };
        const auto dir = fixtureDir();
        static const char* rowNames[11] = {
            "kh PIN2: air-shelf.json is reproduced by our block but for the expected differences",
            "kh PIN3: attribution-pair.json is reproduced by our block but for the expected differences",
            "kh PIN4: bus-partial.json is reproduced by our block but for the expected differences",
            "kh PIN5: bus-widened.json is reproduced by our block but for the expected differences",
            "kh PIN6: clean.json is reproduced by our block but for the expected differences",
            "kh PIN7: mapped-stand-in.json is reproduced by our block but for the expected differences",
            "kh PIN8: push-warning.json is reproduced by our block but for the expected differences",
            "kh PIN9: refusals-only.json is reproduced by our block but for the expected differences",
            "kh PIN10: track-cant-air.json is reproduced by our block but for the expected differences",
            "kh PIN11: track-near-limit.json is reproduced by our block but for the expected differences",
            "kh PIN12: v1-refused.json is reproduced by our block but for the expected differences" };
        for (std::size_t i = 0; i < expect.size(); ++i)
        {
            const auto f = dir.getChildFile (expect[i].file);
            const juce::var fx = juce::JSON::parse (f.loadFileAsString());
            const juce::var req = fx.getProperty ("request", juce::var());
            const juce::var want = req.getProperty ("match", juce::var());
            auto in = blockInFromFixture (want);
            rerouteFromShares (in);
            const auto built = buildMatchBlock (in);
            juce::StringArray diffs;
            jsonDiff (built.block, want, {}, diffs, 0.05);
            diffs.sort (false);
            juce::StringArray exp = common;
            exp.addArray (expect[i].extra);
            exp.sort (false);
            check (f.existsAsFile() && built.readable && diffs == exp, rowNames[i],
                   "differences: " + diffs.joinIntoString (", "));
        }
    }

    // kh PIN13 -- "MAKE A PLAN" IS OFFERED ONLY PER C2 / C3: a v2 build, match_plan,
    // something to plan, and a capture of its own in the mix slot.
    {
        int offeredCount = 0;
        for (int m = 0; m < 16; ++m)
            offeredCount += matchMakePlanOffered (m & 1, m & 2, m & 4, m & 8) ? 1 : 0;
        auto host = session();
        const auto s = sides (60.0f);
        const bool all = matchMakePlanOfferedOn (host, matchPageViewOf (host, s, host.matchSlots()));
        host.caps = { "plan_card" };
        const bool noCap = matchMakePlanOfferedOn (host, matchPageViewOf (host, s, host.matchSlots()));
        host.caps = { "plan_card", "match_plan" };
        host.v2 = false;
        const bool v1 = matchMakePlanOfferedOn (host, matchPageViewOf (host, s, host.matchSlots()));
        host.v2 = true;
        host.mixSlot = {}; host.mixSlot.kind = CompareSlotState::Kind::Live;
        const bool live = matchMakePlanOfferedOn (host, matchPageViewOf (host, s, host.matchSlots()));
        host.mixSlot = k5pins::snapshotSlot (0);
        const MatchSides same { s.ref, s.ref };
        const bool nothing = matchMakePlanOfferedOn (host, matchPageViewOf (host, same, host.matchSlots()));
        check (offeredCount == 1 && matchMakePlanOffered (true, true, true, true)
               && all && ! noCap && ! v1 && ! live && ! nothing,
               "kh PIN13: Make a plan is offered only on a v2 build with match_plan, something to plan and a capture "
               "in the mix slot; without any one of them it is not",
               juce::String (offeredCount) + " " + (all ? "all " : "") + (noCap ? "noCap " : "") + (v1 ? "v1 " : "")
                   + (live ? "live " : "") + (nothing ? "nothing" : ""));
    }

    // kh PIN14 -- MA5: with the agent off, the only agent wording is the plan
    // card's "Turn on Agent and do it all"; without match_plan there is no
    // "Make a plan" and no string containing "Agent". The page adds none.
    {
        auto host = session();
        const auto h = matchHandOffOf (host, matchPageViewOf (host, sides (60.0f), host.matchSlots()));
        auto agentWords = [] (const juce::StringArray& s)
        {
            juce::StringArray a;
            for (const auto& x : s) if (x.containsIgnoreCase ("agent")) a.add (x);
            return a;
        };
        const auto withCap    = buildMatchReport (matchReportInOf (h, true, false));
        const auto withoutCap = buildMatchReport (matchReportInOf (h, false, false));
        const auto offer      = matchPlanCardAgentOffer (true, false);
        const auto aWith      = agentWords (matchReportAllStrings (withCap, offer));
        const auto aWithout   = agentWords (matchReportAllStrings (withoutCap, matchPlanCardAgentOffer (false, false)));
        bool planButtonWithout = false;
        for (const auto& bt : withoutCap.buttons) planButtonWithout = planButtonWithout || bt == kMatchMakePlanLabel;
        check (agentWords (matchHandOffPageStrings()).isEmpty()
               && aWith.size() == 1 && aWith[0] == kMatchTurnOnAgentLabel
               && aWithout.isEmpty() && ! planButtonWithout,
               "kh PIN14: MA5: the page's hand-off has no agent wording; with the agent off the only agent string is "
               "\"Turn on Agent and do it all\", and without match_plan there is no Make a plan and no \"Agent\"",
               aWith.joinIntoString (" | "));
    }

    // kh PIN15 -- THE PRESS POSTS THE match_report THROUGH MatchHost, with
    // crestLog in C2's shape, busTap and preFaderCaveat, and the page says
    // "Report in chat".
    {
        auto host = session();
        const auto h = matchHandOffOf (host, matchPageViewOf (host, sides (60.0f), host.matchSlots()));
        juce::NamedValueSet page;
        const bool posted = matchPostReport (host, h, page);
        juce::var p;
        if (! host.posts.empty()) p = host.posts[0].payload;
        const juce::var c = p.getProperty ("crestLog", juce::var());
        const juce::var cs = c.getProperty ("sides", juce::var());
        const juce::var attr = p.getProperty ("attribution", juce::var());
        check (posted && host.posts.size() == 1 && host.posts[0].kind == "match_report"
               && p.getProperty ("kind", "").toString() == "match_report" && (int) p.getProperty ("v", 0) == 1
               && (int) c.getProperty ("v", 0) == 1 && cs.hasProperty ("reference") && cs.hasProperty ("mix")
               && ! c.hasProperty ("hostBlockSize")
               && p.getProperty ("busTap", "").toString() == "output"
               && (bool) p.getProperty ("preFaderCaveat", false)
               && attr.isArray() && attr.size() >= 2
               && (bool) page.getWithDefault (matchPageReportInChatId(), false)
               && host.drafts.empty() && host.opened.isEmpty(),
               "kh PIN15: the press posts one match_report through postToSessionChat, crestLog in C2's shape with no "
               "unknown block size, busTap and preFaderCaveat set, and the page notes \"Report in chat\"; nothing else is sent",
               juce::JSON::toString (p, true).substring (0, 600));
    }

    // kh PIN16 -- "MAKE A PLAN", OK: the block goes to draftPlan, and the
    // answer opens the Whole session chat at the plan.
    {
        auto host = session();
        const auto h = matchHandOffOf (host, matchPageViewOf (host, sides (60.0f), host.matchSlots()));
        juce::String shown = "unset", whyNot;
        const bool sent = matchSendPlan (host, h, [&] (bool ok, juce::String id, juce::String why)
                                                  { shown = matchPlanAnswered (host, ok, id, why); }, whyNot);
        const juce::String sentJson = host.drafts.empty() ? juce::String() : juce::JSON::toString (host.drafts[0], true);
        check (sent && whyNot.isEmpty() && host.drafts.size() == 1
               && sentJson == juce::JSON::toString (matchPlanBlockOf (h).block, true)
               && host.opened.size() == 1 && host.opened[0] == "plan_1" && shown.isEmpty() && host.posts.empty(),
               "kh PIN16: Make a plan sends the context.match block through draftPlan, and on ok opens the Whole "
               "session chat at the plan id and shows nothing more",
               shown + " / " + host.opened.joinIntoString (","));
    }

    // kh PIN17 -- "MAKE A PLAN", REFUSED: the callback's sentence is shown and
    // nothing opens; a comparison the server could not read is never sent.
    {
        auto host = session();
        host.draftOk = false;
        host.draftWhy = "the plan road is not built yet (A2), so Make a plan has nowhere to send this";
        const auto h = matchHandOffOf (host, matchPageViewOf (host, sides (60.0f), host.matchSlots()));
        juce::String shown, whyNot;
        matchSendPlan (host, h, [&] (bool ok, juce::String id, juce::String why)
                                { shown = matchPlanAnswered (host, ok, id, why); }, whyNot);
        // No six bands on the reference: nothing to send.
        auto s = sides (60.0f);
        s.ref.hasMacro = false;
        auto host2 = session();
        const auto h2 = matchHandOffOf (host2, matchPageViewOf (host2, s, host2.matchSlots()));
        juce::String whyNot2;
        const bool sent2 = matchSendPlan (host2, h2, [] (bool, juce::String, juce::String) {}, whyNot2);
        check (shown == host.draftWhy && host.opened.isEmpty() && host.drafts.size() == 1
               && ! sent2 && host2.drafts.empty() && whyNot2 == matchPlanNeedsBandsSentence(),
               "kh PIN17: a refused plan shows the callback's sentence and opens nothing; a comparison without its "
               "six bands is never sent, and says why",
               shown + " / " + whyNot2);
    }
}

} // namespace k5dpins
