#pragma once

// =============================================================================
//  FIX 8a's PINS: THE PRESS USES EXACTLY WHAT IS ON SCREEN (kx).
// =============================================================================
//
// Sean's ruling of 30 Oct: the AI MATCH press reads the page view for the
// selected window (matchPageViewOf) and nothing else. Each row presses through
// matchPressOn, which is what MatchPanel::press() calls, over a FakeMatchHost
// whose whole-file sides and series differ: the whole song moves the mids,
// the loudest 30 s of the capture moves the low-mids. So a press that read the
// whole capture in "Loudest 30 s" would disagree with the page.
//
// kx PIN1   "Whole song": the press's readiness, band moves and posted payload
//           are the page view's;
// kx PIN2   "Loudest 30 s": the same, and the whole song's view differs, so the
//           row can tell the two apart;
// kx PIN3   "Loudest 30 s" with a reference that has no hop series: the page
//           and the press both refuse, with the page's line; no morph, no
//           post. The same reference plays in "Whole song";
// kx PIN4   "Loudest 30 s" with a Live mix: both refuse with the Live line.
//           "Whole song" with a Live mix still plays (MA9);
// kx PIN5   the press and a direct call to matchPageFor (EJMatchPage.h, the
//           helper A's "keep the reason on screen" check calls) with the
//           window named outright agree, in all four cases above.
// =============================================================================

#include "EJMatchHandOff.h"
#include "k5_fakehost.h"

namespace k8apins
{
using namespace echojay;
using k5pins::FakeMatchHost;

template <typename Check>
inline void runK8aPins (Check&& check)
{
    // n hops at -20 LUFS, every band at power 1; a loud section
    // [loudAt, loudAt + 300) at -10 LUFS whose lowMid power is lowMid.
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
    // The whole-file sides: the mix's mids 4 dB hot and the mix 2 dB quieter.
    // The loudness gap is a Gain move from whole-file figures (the window keeps
    // integrated), so a refused window still leaves a move a fallback would play.
    auto wholeSides = []
    {
        auto side = [] (float midDb, float integrated)
        {
            MatchSide s;
            s.macro.fill (-10.0f); s.macro[3] = midDb; s.hasMacro = true;
            s.macroReduction = SpectralReduction::WholeFileAverage;
            s.durationSeconds = 90.0f;
            s.integrated = integrated; s.truePeak = -1.0f; s.overs = 0;
            s.psr = 8.0f; s.plr = 11.0f;
            return s;
        };
        return MatchSides { side (-6.0f, -14.0f), side (-10.0f, -12.0f) };
    };
    const juce::String refPath = "/refs/k8a-ref.wav";
    auto session = [&] (bool refHasSeries)
    {
        FakeMatchHost host;
        CaptureSnapshot c;
        c.id = "1730000000081"; c.name = "Capture 81";
        c.hopFrames = series (900, 400, 8.0f); c.hopFramesHops = 900;
        ChannelMeterData self; self.name = "Mix Bus"; self.hopFrames = c.hopFrames; self.hops = 900;
        c.channels = { self };
        host.snaps = { c };
        ReferenceResult r;
        r.name = "Song K8"; r.path = refPath;
        if (refHasSeries) { r.hopFrames = series (600, 100, 1.0f); r.hops = 600; }
        host.ref = r;
        host.mixSlot = k5pins::snapshotSlot (0);
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
        host.sides = wholeSides();
        return host;
    };
    auto setMode = [&] (MatchWindowMode m) { matchWindowMemory().set (refPath, m); };
    auto sameReady = [] (const MatchReadiness& a, const MatchReadiness& b)
    {
        return a.line == b.line && a.playable == b.playable && a.possible == b.possible && a.goodNews == b.goodNews;
    };
    auto json = [] (const juce::var& v) { return juce::JSON::toString (v, true); };

    // ONE PRESS, and the page view of the same host. The payload the page view
    // would post is built from its own hand-off.
    struct Pressed
    {
        MatchPress press;
        MatchPageView view;
        juce::var posted, expected;
        bool onePost = false;
    };
    auto pressOn = [&] (FakeMatchHost& host)
    {
        Pressed out;
        juce::NamedValueSet page;
        out.press = matchPressOn (host, page);
        out.view  = matchPageViewOf (host, host.sides, host.matchSlots());
        out.expected = matchReportVar (matchHandOffOf (host, out.view), host.hasCap (kMatchPlanCap), host.agentOn());
        out.onePost = host.posts.size() == 1 && host.posts[0].kind == kMatchReportKind;
        if (out.onePost) out.posted = host.posts[0].payload;
        return out;
    };
    auto equalsView = [&] (const Pressed& p)
    {
        return sameReady (p.press.ready, p.view.ready)
            && p.press.moves == matchBandMoves (p.view.prop)
            && p.onePost && p.posted.isObject() && json (p.posted) == json (p.expected);
    };
    auto movesText = [] (const std::array<float, 6>& m)
    {
        juce::StringArray a;
        for (float x : m) a.add (juce::String (x, 2));
        return a.joinIntoString (" ");
    };

    // kx PIN1 -- WHOLE SONG: THE PRESS IS THE PAGE VIEW.
    {
        setMode (MatchWindowMode::Whole);
        auto host = session (true);
        const auto p = pressOn (host);
        check (p.press.ready.playable && p.press.acts.morph && equalsView (p),
               "kx PIN1: in \"Whole song\" the press's readiness, band moves and posted match_report are the page "
               "view's",
               p.press.ready.line + " | " + movesText (p.press.moves));
    }

    // kx PIN2 -- LOUDEST 30 s: THE PRESS IS THE PAGE VIEW, and the whole song's would differ.
    {
        setMode (MatchWindowMode::Whole);
        auto wholeHost = session (true);
        const auto whole = pressOn (wholeHost);
        setMode (MatchWindowMode::Loudest30);
        auto host = session (true);
        const auto p = pressOn (host);
        const bool differs = p.view.w.windowed
                          && matchBandMoves (p.view.prop) != matchBandMoves (whole.view.prop)
                          && json (p.expected) != json (whole.expected);
        check (p.press.ready.playable && p.press.acts.morph && equalsView (p) && differs
               && p.posted.getProperty ("window", juce::var()).toString() == "loudest30",
               "kx PIN2: in \"Loudest 30 s\" the press's readiness, band moves and posted match_report are the page "
               "view's over the window, which differ from the whole song's",
               p.press.ready.line + " | " + movesText (p.press.moves) + " | whole " + movesText (whole.press.moves));
    }

    // kx PIN3 -- A REFERENCE WITH NO SERIES IN LOUDEST 30 s REFUSES ON THE PAGE AND THE PRESS.
    {
        setMode (MatchWindowMode::Whole);
        auto wholeHost = session (false);
        const auto whole = pressOn (wholeHost);
        setMode (MatchWindowMode::Loudest30);
        auto host = session (false);
        const auto p = pressOn (host);
        const auto said = matchPressRefusedText (p.press.ready);
        check (whole.press.acts.morph && whole.onePost
               && ! p.view.prop.moves.empty()
               && ! p.view.ready.playable && ! p.press.acts.morph && ! p.press.acts.post && host.posts.empty()
               && sameReady (p.press.ready, p.view.ready)
               && said == p.view.ready.line && said.contains ("the reference's hop series"),
               "kx PIN3: in \"Loudest 30 s\" a reference with no hop series refuses on the page and on the press, "
               "with the page's line; no morph and no report, though the whole song would play",
               said);
    }

    // kx PIN4 -- A LIVE MIX IN LOUDEST 30 s REFUSES ON BOTH; IN WHOLE SONG IT STILL PLAYS (MA9).
    {
        auto live = [&] (MatchWindowMode m)
        {
            setMode (m);
            auto host = session (true);
            host.mixSlot = {};
            host.mixSlot.kind = CompareSlotState::Kind::Live;
            return pressOn (host);
        };
        const auto whole = live (MatchWindowMode::Whole);
        const auto p     = live (MatchWindowMode::Loudest30);
        const auto said  = matchPressRefusedText (p.press.ready);
        check (whole.press.acts.morph && equalsView (whole)
               && ! p.view.prop.moves.empty()
               && ! p.view.ready.playable && ! p.press.acts.morph && ! p.onePost
               && sameReady (p.press.ready, p.view.ready) && said == matchLiveNeedsCaptureLine(),
               "kx PIN4: in \"Loudest 30 s\" a Live mix refuses on the page and on the press with the Live line; "
               "in \"Whole song\" it still plays, and the press is the page view (MA9)",
               said + " | whole: " + whole.press.ready.line);
    }

    // kx PIN5 -- THE PRESS AND A DIRECT CALL TO THE HELPER AGREE.
    {
        struct Case { const char* name; MatchWindowMode mode; bool refHasSeries, live; };
        const Case cases[] = { { "whole", MatchWindowMode::Whole, true, false },
                               { "loudest30", MatchWindowMode::Loudest30, true, false },
                               { "no series", MatchWindowMode::Loudest30, false, false },
                               { "live", MatchWindowMode::Loudest30, true, true } };
        juce::StringArray bad;
        for (const auto& c : cases)
        {
            setMode (c.mode);
            auto host = session (c.refHasSeries);
            if (c.live) { host.mixSlot = {}; host.mixSlot.kind = CompareSlotState::Kind::Live; }
            juce::NamedValueSet page;
            const auto pressed = matchPressOn (host, page);
            const MatchHost& asHost = host;
            const auto direct = matchPageFor (asHost, c.mode, asHost.matchSlots());
            const juce::var posted = host.posts.empty() ? juce::var() : host.posts[0].payload;
            const bool payloadAgrees = direct.ready.playable
                ? host.posts.size() == 1
                      && json (posted) == json (matchReportVar (matchHandOffOf (asHost, direct),
                                                                host.hasCap (kMatchPlanCap), host.agentOn()))
                : host.posts.empty();
            if (! (sameReady (pressed.ready, direct.ready) && pressed.moves == matchBandMoves (direct.prop)
                   && pressed.acts.morph == direct.ready.playable && payloadAgrees))
                bad.add (c.name);
        }
        check (bad.isEmpty(),
               "kx PIN5: the press and a direct call to matchPageFor with the window named agree on the line, "
               "playable, possible, the band moves and the report, in whole, loudest 30 s, no series and Live",
               bad.joinIntoString (", "));
    }
    setMode (MatchWindowMode::Whole);
}

} // namespace k8apins
