#pragma once

// =============================================================================
//  ROUND 4a's PINS: K1, THE WINDOW ON THE PAGE (kw).
// =============================================================================
//
// DESIGN_MATCH_TAB rev 3 section 7: MA1 (the loudest-30 window finds a planted
// loud section) and MA9 (a Live mix side shows the report, and the one line
// says a capture is needed to plan). Plus the window's own rules: no silent
// fallback, the memory per reference, and the switch's place on the page.
// MA4 (stored figures survive a moved file) is in k7a_pins.h, on A's restore
// path (R4-1, 82e46d99).
//
// The series are A's real types: CaptureSnapshot for the mix and
// ReferenceResult for the reference, through the page's own entry point.
// =============================================================================

#include "EJMatchWindowSides.h"
#include "EJMatchPage.h"
#include "PluginProcessor.h"     // CaptureSnapshot
#include "ReferenceAnalyser.h"   // ReferenceResult

namespace k4apins
{
using namespace echojay;

template <typename Check>
inline void runK4aPins (Check&& check)
{
    // n hops at -20 LUFS with every band at power 1, and a loud section
    // [loudAt, loudAt + 300) at -10 LUFS whose lowMid power is 8.
    auto series = [] (std::size_t n, std::size_t loudAt)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            const bool loud = i >= loudAt && i < loudAt + 300;
            h.shortTermLufs = i < 29 ? -100.0f : (loud ? -10.0f : -20.0f);
            h.shortTermTruePeak = h.shortTermLufs + 9.0f;
            h.bandPower.fill (1.0f);
            if (loud) h.bandPower[2] = 8.0f;
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    auto side = [] (float bandDb)
    {
        MatchSide s;
        s.macro.fill (bandDb); s.hasMacro = true;
        s.macroReduction = SpectralReduction::WholeFileAverage;
        s.durationSeconds = 90.0f;
        s.integrated = -12.0f; s.truePeak = -1.0f; s.overs = 0;
        s.psr = 8.0f; s.plr = 11.0f;
        return s;
    };
    auto near = [] (double a, double b, double t) { return std::abs (a - b) <= t; };

    CaptureSnapshot cap;
    cap.hopFrames = series (900, 400);
    cap.hopFramesHops = (int) cap.hopFrames.size();
    ReferenceResult ref;
    ref.hopFrames = series (600, 100);
    ref.hops = (int) ref.hopFrames.size();

    // kw PIN1 -- MA1: THE LOUDEST 30 s IS THE PLANTED LOUD SECTION, on both sides,
    // and the bands are that window's.
    {
        const auto w = matchSidesForWindow (side (-10.0f), side (-10.0f), &cap.hopFrames, &ref.hopFrames,
                                            MatchWindowMode::Loudest30, false);
        check (w.windowed && w.mixWindow && w.mixWindow->begin == 400 && w.mixWindow->end == 700
               && w.refWindow && w.refWindow->begin == 100 && w.refWindow->end == 400 && w.why.isEmpty(),
               "kw PIN1: MA1: the loudest-30 window finds the planted loud section on the capture (hops 400 to 699) "
               "and on the reference (100 to 399)",
               w.mixWindow ? juce::String ((int) w.mixWindow->begin) : juce::String ("no window"));
        check (near (w.mix.macro[2], 10.0 * std::log10 (8.0), 1e-4) && near (w.mix.macro[0], 0.0, 1e-4)
               && w.mix.hasMacro && w.mix.macroReduction == SpectralReduction::WholeWindowAverage
               && w.mix.integrated == -12.0f && w.mix.durationSeconds == 90.0f
               && w.mix.psr <= -99.0f && w.ref.plr <= -99.0f,
               "kw PIN1: and the bands are the window's (lowMid 9.03 dB), integrated and the length stay whole-file, "
               "PSR and PLR are unavailable",
               juce::String (w.mix.macro[2], 3));
    }

    // kw PIN2 -- MA9: A LIVE MIX SIDE SHOWS THE COMPARISON AND THE LINE ASKS FOR A CAPTURE.
    {
        auto mix = side (-10.0f);
        mix.macro[3] = -6.0f;   // the mids 4 dB hot: a band move to show
        const auto w = matchSidesForWindow<MeterEngine::HopFrame> (mix, side (-10.0f), nullptr, &ref.hopFrames,
                                                                    MatchWindowMode::Whole, true);
        const auto prop = computeMatchProposal (w.mix, w.ref);
        MatchReadinessContext ctx; ctx.mixIsLive = true;
        const auto r = matchReadiness (prop, ctx);
        check (! prop.moves.empty() && r.playable && ! r.possible
               && r.line == "Capture at least 30 s of the chorus to make a plan.",
               "kw PIN2: MA9: a Live mix side still shows and plays the comparison, and the one line says a capture "
               "is needed to plan (precedence 1)",
               r.line);
    }

    // kw PIN3 -- A LIVE MIX SIDE IS NEVER WINDOWED, and keeps no bands under the window's name.
    {
        const auto w = matchSidesForWindow<MeterEngine::HopFrame> (side (-10.0f), side (-10.0f), &cap.hopFrames,
                                                                    &ref.hopFrames, MatchWindowMode::Loudest30, true);
        check (! w.windowed && ! w.mixWindow && ! w.mix.hasMacro && w.why == matchLiveNeedsCaptureLine(),
               "kw PIN3: in the loudest 30 s a Live mix side is not windowed and has no bands, and why asks for a capture",
               w.why);
    }

    // kw PIN4 -- NO SILENT FALLBACK: a reference with no series, or under 30 s, loses its bands and says why.
    {
        const std::vector<MeterEngine::HopFrame> none;
        const auto a = matchSidesForWindow (side (-10.0f), side (-10.0f), &cap.hopFrames, &none,
                                            MatchWindowMode::Loudest30, false);
        const auto shortRef = series (250, 0);
        const auto b = matchSidesForWindow (side (-10.0f), side (-10.0f), &cap.hopFrames, &shortRef,
                                            MatchWindowMode::Loudest30, false);
        check (! a.windowed && ! a.ref.hasMacro && a.mix.hasMacro && a.why.contains ("the reference")
               && ! b.windowed && ! b.ref.hasMacro && b.why == loudest30RefusalMessage ("the reference", 25.0),
               "kw PIN4: in the loudest 30 s a reference with no hop series, or under 30 s, has no bands and the "
               "line says which",
               a.why + " | " + b.why);
    }

    // kw PIN5 -- THE MEMORY: whole song by default, remembered per reference.
    {
        MatchWindowMemory m;
        const bool def = m.get ("/refs/a.wav") == MatchWindowMode::Whole && m.get ("") == MatchWindowMode::Whole;
        m.set ("/refs/a.wav", MatchWindowMode::Loudest30);
        check (def && m.get ("/refs/a.wav") == MatchWindowMode::Loudest30 && m.get ("/refs/b.wav") == MatchWindowMode::Whole,
               "kw PIN5: the window is whole song by default and remembered per reference");
    }

    // kw PIN6 -- THE SWITCH SITS IN THE GRAPH CARD AND MOVES NOTHING ELSE.
    {
        const auto R = matchPageLayout ({ 0, 0, 1100, 720 });
        const auto s = matchWindowSwitchRect (R.graph);
        const auto h0 = matchWindowSwitchHalf (R.graph, 0), h1 = matchWindowSwitchHalf (R.graph, 1);
        const bool ok = ! s.isEmpty() && R.graph.contains (s) && s.getRight() <= R.graph.getRight() - kMatchWindowSwitchPad
                     && ! s.intersects (R.axisRow) && ! s.intersects (R.setup) && ! s.intersects (R.readout)
                     && ! h0.intersects (h1) && h0.getUnion (h1) == s
                     && juce::String (matchWindowLabel (MatchWindowMode::Whole)) == "Whole song"
                     && juce::String (matchWindowLabel (MatchWindowMode::Loudest30)) == "Loudest 30 s"
                     && matchWindowSwitchRect ({ 0, 0, 100, 20 }).isEmpty();
        check (ok, "kw PIN6: the switch is inside the graph card's top right, two halves labelled \"Whole song\" "
                   "and \"Loudest 30 s\", clear of the setup row, the axis row and the readout");
    }
}

} // namespace k4apins
