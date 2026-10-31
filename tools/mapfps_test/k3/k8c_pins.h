#pragma once

// =============================================================================
//  ROUND 8c's PINS: THE "AFTER" CURVE AND THE CLOSED LOOP (kz).
// =============================================================================
//
// K6, plan section 7, C2 section 4's "after" block. MatchHost::verifyCapture()
// is on this base; the pins set it on the FakeMatchHost.
//
// kz PIN1   closing, widening and unchanged bands: before and after are the
//           same D, and only the band that got further from the reference
//           warns;
// kz PIN2   the press records both sets of six band deltas in the payload's
//           "after" block, the same shape as "mix"; a verify capture arriving
//           after a report posts a new one that carries it;
// kz PIN3   a verify capture against another reference, or another window,
//           gives no after curve and says why, in the payload too;
// kz PIN4   no verify capture: "after" is exactly { "present": false }, which
//           is not six unchanged bands;
// kz PIN5   in "Loudest 30 s" the after side is the verify capture's own
//           loudest 30 s, not its whole run.
// =============================================================================

#include "EJMatchHandOff.h"
#include "k5_fakehost.h"

namespace k8cpins
{
using namespace echojay;
using k5pins::FakeMatchHost;

template <typename Check>
inline void runK8cPins (Check&& check)
{
    // n hops at -20 LUFS, every band at power 1; a loud section at -10 LUFS
    // [loudAt, loudAt + 300) whose lowMid power is lowMid.
    auto series = [] (std::size_t n, std::size_t loudAt, float lowMid)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            const bool loud = i >= loudAt && i < loudAt + 300;
            h.shortTermLufs = i < 29 ? -100.0f : (loud ? -10.0f : -20.0f);
            h.shortTermTruePeak = h.shortTermLufs + 9.0f;
            h.bandPower.fill (1.0f);
            if (loud) h.bandPower[2] = lowMid;
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    // Before: the mix's relatives are +2 +2 +2 -2 -2 -2 against a flat reference.
    // After: +0.5 +3.5 +2 -2 -2 -2, so the sub closes, the low widens and the
    // rest are unchanged.
    const std::array<float, 6> beforeD { 2.0f, 2.0f, 2.0f, -2.0f, -2.0f, -2.0f };
    const std::array<float, 6> afterD  { 0.5f, 3.5f, 2.0f, -2.0f, -2.0f, -2.0f };
    auto side = [] (const std::array<float, 6>& macro)
    {
        MatchSide s;
        s.macro = macro; s.hasMacro = true;
        s.macroReduction = SpectralReduction::WholeFileAverage;
        s.durationSeconds = 90.0f; s.integrated = -12.0f; s.truePeak = -1.0f; s.overs = 0;
        return s;
    };
    auto plus = [] (const std::array<float, 6>& d) { std::array<float, 6> m {}; for (int b = 0; b < 6; ++b) m[(size_t) b] = -10.0f + d[(size_t) b]; return m; };
    const juce::String refPath = "/refs/k8c-ref.wav";
    auto session = [&]
    {
        FakeMatchHost host;
        CaptureSnapshot c;
        c.id = "1730000000083"; c.name = "Before";
        c.hopFrames = series (900, 400, 1.0f); c.hopFramesHops = 900;
        host.snaps = { c };
        ReferenceResult r;
        r.name = "Song Z"; r.path = refPath;
        r.hopFrames = series (600, 100, 1.0f); r.hops = 600;
        host.ref = r;
        host.mixSlot = k5pins::snapshotSlot (0);
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
        std::array<float, 6> flat; flat.fill (-10.0f);
        host.sides = MatchSides { side (plus (beforeD)), side (flat) };
        return host;
    };
    auto verifyCap = [&]
    {
        CaptureSnapshot v;
        v.id = "1730000000099"; v.name = "After";
        v.macroBandAccum = plus (afterD); v.hasMacroBandAccum = true;
        v.hopFrames = series (900, 300, 8.0f); v.hopFramesHops = 900;
        v.averagedData.integrated = -11.5f; v.durationSeconds = 95.0f;
        return v;
    };
    auto viewOf = [] (const FakeMatchHost& h, MatchWindowMode m)
    {
        const MatchHost& host = h;
        return matchPageFor (host, m, host.matchSlots());
    };
    auto near = [] (float a, float b) { return std::abs (a - b) < 0.01f; };
    auto json = [] (const juce::var& v) { return juce::JSON::toString (v, true); };
    matchWindowMemory().set (refPath, MatchWindowMode::Whole);

    // kz PIN1 -- CLOSING, WIDENING, UNCHANGED.
    {
        auto host = session();
        host.verify = verifyCap();
        const auto v = viewOf (host, MatchWindowMode::Whole);
        juce::NamedValueSet page;
        matchNotePlanFor (page, v, "Song Z");
        const auto a = matchAfterOf (host.verifyCapture(), v, matchPlanForOf (page));
        bool ok = a.present;
        for (int b = 0; ok && b < 6; ++b)
            ok = near (a.beforeDb[(size_t) b], beforeD[(size_t) b]) && near (a.afterDb[(size_t) b], afterD[(size_t) b]);
        const bool trends = a.trend[0] == MatchAfterTrend::Closing && a.trend[1] == MatchAfterTrend::Widening
                         && a.trend[2] == MatchAfterTrend::Unchanged && a.trend[3] == MatchAfterTrend::Unchanged
                         && ! matchAfterWarns (a.trend[0]) && matchAfterWarns (a.trend[1]) && ! matchAfterWarns (a.trend[2]);
        check (ok && trends,
               "kz PIN1: before and after are the same D per band; the sub closes, the low widens and warns, the "
               "rest are unchanged and do not warn",
               juce::String (a.afterDb[0], 2) + " " + juce::String (a.afterDb[1], 2));
    }

    // kz PIN2 -- THE PAYLOAD RECORDS BOTH SETS; A VERIFY CAPTURE AFTER A REPORT POSTS A NEW ONE.
    {
        auto host = session();
        juce::NamedValueSet page;
        matchNotePlanFor (page, viewOf (host, MatchWindowMode::Whole), "Song Z");
        matchPressOn (host, page);                 // before any verify capture
        host.verify = verifyCap();
        matchPressOn (host, page);                 // the same comparison, now with an after
        const bool two = host.posts.size() == 2;
        const juce::var first  = two ? host.posts[0].payload.getProperty ("after", juce::var()) : juce::var();
        const juce::var second = two ? host.posts[1].payload.getProperty ("after", juce::var()) : juce::var();
        const juce::var b = second.getProperty ("deltasBeforeDb", juce::var());
        const juce::var d = second.getProperty ("deltasAfterDb", juce::var());
        bool sets = b.isArray() && d.isArray() && b.size() == 6 && d.size() == 6;
        for (int i = 0; sets && i < 6; ++i)
            sets = near ((float) (double) b[i], beforeD[(size_t) i]) && near ((float) (double) d[i], afterD[(size_t) i]);
        const juce::var fig = second.getProperty ("figures", juce::var());
        check (two && ! (bool) first.getProperty ("present", true) && (bool) second.getProperty ("present", false)
               && sets && second.getProperty ("name", juce::var()).toString() == "after"
               && std::abs ((double) second.getProperty ("windowS", 0.0) - 95.0) < 1e-6
               && std::abs ((double) fig.getProperty ("lufs", 0.0) + 11.5) < 1e-4,
               "kz PIN2: the press records both sets of six band deltas in the payload's after block, the same shape as "
               "mix; a verify capture arriving after a report posts a new report that carries it",
               juce::String ((int) host.posts.size()) + " posted");
    }

    // kz PIN3 -- ANOTHER REFERENCE OR ANOTHER WINDOW: NO AFTER CURVE, AND WHY.
    {
        auto host = session();
        host.verify = verifyCap();
        const auto v = viewOf (host, MatchWindowMode::Whole);
        MatchPlanFor other; other.refKey = "/refs/k8c-other.wav"; other.refName = "Song Other";
        const auto a = matchAfterOf (host.verifyCapture(), v, other);
        MatchPlanFor otherWindow; otherWindow.refKey = refPath; otherWindow.refName = "Song Z";
        otherWindow.mode = MatchWindowMode::Loudest30;
        const auto w = matchAfterOf (host.verifyCapture(), v, otherWindow);

        juce::NamedValueSet page;
        page.set (matchPagePlanForId(), juce::JSON::toString (juce::var (juce::Array<juce::var> {
                      juce::var ("/refs/k8c-other.wav"), juce::var ("Song Other"), juce::var ("whole") }), true));
        matchPressOn (host, page);
        const juce::var posted = host.posts.empty() ? juce::var() : host.posts[0].payload.getProperty ("after", juce::var());
        check (! a.present && a.why.contains ("\"Song Other\"") && ! w.present && w.why.contains ("the loudest 30 s")
               && w.why.contains ("not the whole song")
               && ! (bool) posted.getProperty ("present", true) && posted.getProperty ("why", juce::var()).toString() == a.why
               && ! posted.hasProperty ("deltasAfterDb"),
               "kz PIN3: a verify capture against another reference, or another window, gives no after curve and says "
               "why, in the payload too",
               a.why + " | " + w.why);
    }

    // kz PIN4 -- NO VERIFY CAPTURE IS NOT "NOTHING CHANGED".
    {
        auto host = session();
        juce::NamedValueSet page;
        matchNotePlanFor (page, viewOf (host, MatchWindowMode::Whole), "Song Z");
        const auto a = matchAfterOf (host.verifyCapture(), viewOf (host, MatchWindowMode::Whole), matchPlanForOf (page));
        matchPressOn (host, page);
        const juce::var posted = host.posts.empty() ? juce::var() : host.posts[0].payload.getProperty ("after", juce::var());
        check (! a.present && a.why.isEmpty() && host.posts.size() == 1
               && json (posted) == json (juce::JSON::parse ("{\"present\": false}")),
               "kz PIN4: with no verify capture the after block is exactly { \"present\": false }, with no figures and no "
               "reason: it is not six unchanged bands",
               json (posted));
    }

    // kz PIN5 -- LOUDEST 30 s: THE AFTER SIDE IS THE VERIFY CAPTURE'S OWN WINDOW.
    {
        auto host = session();
        host.verify = verifyCap();
        const auto v = viewOf (host, MatchWindowMode::Loudest30);
        juce::NamedValueSet page;
        matchNotePlanFor (page, v, "Song Z");
        const auto a = matchAfterOf (host.verifyCapture(), v, matchPlanForOf (page));
        std::array<float, 6> windowRel {}, wholeRel {};
        const auto L = loudest30Window (host.verify->hopFrames);
        const bool ok = L.window && matchBandRelatives (matchWindowBandsDb (host.verify->hopFrames, *L.window), windowRel)
                     && matchBandRelatives (host.verify->macroBandAccum, wholeRel);
        bool same = ok && a.present;
        for (int b = 0; same && b < 6; ++b) same = near (a.afterRel[(size_t) b], windowRel[(size_t) b]);
        check (same && L.window->begin == 300 && std::abs (a.afterRel[2] - wholeRel[2]) > 1.0f,
               "kz PIN5: in \"Loudest 30 s\" the after side is the verify capture's own loudest 30 s (hops 300 to 599), "
               "not its whole run",
               juce::String (a.afterRel[2], 2) + " vs whole " + juce::String (wholeRel[2], 2));
    }
    matchWindowMemory().set (refPath, MatchWindowMode::Whole);
}

} // namespace k8cpins
