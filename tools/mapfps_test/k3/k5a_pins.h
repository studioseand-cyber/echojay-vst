#pragma once

// =============================================================================
//  ROUND 5a's PINS: THE WINDOW FROM THE SLOT'S OWN CAPTURE (kv).
// =============================================================================
//
// matchPageViewOf (EJMatchHandOff.h) reads the mix's series through
// MatchHost::captureForSlot (*matchSlots().mix). These drive it with a host
// whose captureForSlot is A's real lookup over two captures, the older one in
// the mix slot, so "the newest capture's bands" is a different answer the pin
// can see.
// =============================================================================

#include "EJMatchHandOff.h"
#include "k5_fakehost.h"

namespace k5apins
{
using namespace echojay;
using k5pins::FakeMatchHost;

template <typename Check>
inline void runK5aPins (Check&& check)
{
    // n hops at -20 LUFS, every band at power 1, and a loud section
    // [loudAt, loudAt + 300) at -10 LUFS whose lowMid power is p2.
    auto series = [] (std::size_t n, std::size_t loudAt, float p2)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            const bool loud = i >= loudAt && i < loudAt + 300;
            h.shortTermLufs = i < 29 ? -100.0f : (loud ? -10.0f : -20.0f);
            h.shortTermTruePeak = h.shortTermLufs + 9.0f;
            h.bandPower.fill (1.0f);
            if (loud) h.bandPower[2] = p2;
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    auto side = []
    {
        MatchSide s;
        s.macro.fill (-10.0f); s.hasMacro = true;
        s.macroReduction = SpectralReduction::WholeFileAverage;
        s.durationSeconds = 90.0f;
        s.integrated = -12.0f; s.truePeak = -1.0f; s.overs = 0;
        return s;
    };
    auto near = [] (double a, double b, double t) { return std::abs (a - b) <= t; };

    // Two captures: the OLDER one is loud at hops 100..399 with lowMid 8, the
    // NEWER one at 400..699 with lowMid 2. The reference is its own series.
    FakeMatchHost host;
    {
        CaptureSnapshot older, newer;
        older.name = "older"; older.hopFrames = series (900, 100, 8.0f); older.hopFramesHops = 900;
        newer.name = "newer"; newer.hopFrames = series (900, 400, 2.0f); newer.hopFramesHops = 900;
        host.snaps = { older, newer };
        ReferenceResult r;
        r.name = "ref"; r.path = "/refs/k5a-ref.wav";
        r.hopFrames = series (600, 100, 4.0f); r.hops = 600;
        host.ref = r;
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
    }
    matchWindowMemory().set ("/refs/k5a-ref.wav", MatchWindowMode::Loudest30);
    const MatchSides sides { side(), side() };

    // kv PIN1 -- AN OLDER SNAPSHOT IN THE MIX SLOT GIVES ITS OWN BANDS, never
    // the newest capture's. The same host with the newer one in the slot gives
    // the newer one's, so the pin can tell the two apart.
    {
        host.mixSlot = k5pins::snapshotSlot (0);
        const auto v0 = matchPageViewOf (host, sides, host.matchSlots());
        host.mixSlot = k5pins::snapshotSlot (1);
        const auto v1 = matchPageViewOf (host, sides, host.matchSlots());
        check (v0.w.windowed && v0.w.mixWindow && v0.w.mixWindow->begin == 100
               && near (v0.w.mix.macro[2], 10.0 * std::log10 (8.0), 1e-4)
               && v0.mixCapture && v0.mixCapture->name == "older"
               && v1.w.mixWindow && v1.w.mixWindow->begin == 400
               && near (v1.w.mix.macro[2], 10.0 * std::log10 (2.0), 1e-4),
               "kv PIN1: an older snapshot in the mix slot is windowed from its own capture (hops 100 to 399, "
               "lowMid 9.03 dB), never the newest capture's (400 to 699, 3.01 dB)",
               v0.mixCapture ? v0.mixCapture->name + " " + juce::String (v0.w.mix.macro[2], 3) : juce::String ("no capture"));
    }

    // kv PIN2 -- A LIVE MIX SLOT KEEPS 4a's LINE (MA9), and the newest capture
    // is not read in its place.
    {
        host.mixSlot = {};
        host.mixSlot.kind = CompareSlotState::Kind::Live;
        const auto v = matchPageViewOf (host, sides, host.matchSlots());
        const auto ready = matchReadiness (computeMatchProposal (v.w.mix, v.w.ref), v.ctx);
        check (! v.mixCapture && v.ctx.mixIsLive && ! v.w.windowed && ! v.w.mix.hasMacro
               && v.w.why == matchLiveNeedsCaptureLine() && ready.line == matchLiveNeedsCaptureLine(),
               "kv PIN2: a Live mix slot has no capture of its own, no window bands, and the one line asks for a capture (MA9)",
               ready.line);
    }

    // kv PIN3 -- A SLOT WITH NO CAPTURE OF ITS OWN IS NOT GIVEN ONE. A workspace
    // review is not a CaptureSnapshot (A's table on captureForSlot): the window
    // refuses that side and says why, rather than reading the newest capture.
    {
        host.mixSlot = {};
        host.mixSlot.kind = CompareSlotState::Kind::WsCapture;
        host.mixSlot.wsReviewId = "rev_1";
        const auto v = matchPageViewOf (host, sides, host.matchSlots());
        check (! v.mixCapture && ! v.w.windowed && ! v.w.mix.hasMacro && v.w.ref.hasMacro
               && v.w.why.contains ("hop series"),
               "kv PIN3: a workspace-review mix slot gets no capture and no window bands, and the line says why; "
               "the newest capture is never read in its place",
               v.w.why);
    }
    matchWindowMemory().set ("/refs/k5a-ref.wav", MatchWindowMode::Whole);
}

} // namespace k5apins
