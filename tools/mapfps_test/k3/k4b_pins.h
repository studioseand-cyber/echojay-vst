#pragma once

// =============================================================================
//  ROUND 4b's PINS: THE PLAN'S PROPOSAL, UNCAPPED (kp).
// =============================================================================
//
// X1 (BUILD_PLAN_PLANS_UX section 2: "the proposal per X1, no cap, 2 dB band
// floor, 1 dB gain floor") and DESIGN_MATCH_TAB rev 5's MA7: a 9 dB band
// fixture proposes 9 dB, and mutating the band floor or re-adding a cap
// reddens it. The page keeps its capped move (A's mr PIN20 and the rows near
// mapfps_test.cpp:13632 pin that); kp PIN3 pins that the plan's change did not
// reach it.
// =============================================================================

#include "EJMatchProposal.h"

namespace k4bpins
{
using namespace echojay;

template <typename Check>
inline void runK4bPins (Check&& check)
{
    // Two sides, 90 s each, every band at -10 dB, integrated at -12 LUFS.
    auto side = []
    {
        MatchSide s;
        s.macro.fill (-10.0f); s.hasMacro = true;
        s.macroReduction = SpectralReduction::WholeFileAverage;
        s.durationSeconds = 90.0f;
        s.integrated = -12.0f; s.truePeak = -3.0f; s.overs = 0;
        return s;
    };
    auto bandMove = [] (const MatchProposal& p, int band) -> const MatchMove*
    {
        for (const auto& m : p.moves) if (m.kind == MatchMoveKind::Band && m.band == band) return &m;
        return nullptr;
    };
    auto gainMove = [] (const MatchProposal& p) -> const MatchMove*
    {
        for (const auto& m : p.moves) if (m.kind == MatchMoveKind::Gain) return &m;
        return nullptr;
    };

    // The mix's lowMid 9 dB hot and its mid 9 dB cold: the six-band mean is
    // unchanged, so each relative gap is exactly 9 dB.
    auto mix9 = side();
    mix9.macro[2] = -1.0f; mix9.macro[3] = -19.0f;
    // And the same shape at 1.99 dB, just under the floor.
    auto mixUnder = side();
    mixUnder.macro[2] = -10.0f + 1.99f; mixUnder.macro[3] = -10.0f - 1.99f;

    // kp PIN1 -- MA7: THE PLAN PROPOSES THE WHOLE 9 dB, AND THE 2 dB FLOOR STAYS.
    {
        const auto p = computeMatchProposal (mix9, side(), MatchProposalFor::Plan);
        const auto* lm = bandMove (p, 2);
        const auto* md = bandMove (p, 3);
        const auto u = computeMatchProposal (mixUnder, side(), MatchProposalFor::Plan);
        check (lm != nullptr && lm->valueDb == -9.0f && ! lm->capped && md != nullptr && md->valueDb == 9.0f
               && bandMove (u, 2) == nullptr && bandMove (u, 3) == nullptr,
               "kp PIN1: MA7: a 9 dB band gap proposes 9 dB for the plan, uncapped, and 1.99 dB proposes nothing",
               lm != nullptr ? juce::String (lm->valueDb, 2) : juce::String ("no move"));
    }

    // kp PIN2 -- THE 1 dB GAIN FLOOR STAYS FOR THE PLAN.
    {
        auto quiet = side(); quiet.integrated = -12.0f - 0.99f;
        auto one   = side(); one.integrated   = -13.0f;
        const auto a = computeMatchProposal (quiet, side(), MatchProposalFor::Plan);
        const auto b = computeMatchProposal (one, side(), MatchProposalFor::Plan);
        check (gainMove (a) == nullptr && gainMove (b) != nullptr && gainMove (b)->valueDb == 1.0f,
               "kp PIN2: the plan keeps the 1 dB gain floor: 0.99 dB proposes no gain move, 1.0 dB proposes 1.0");
    }

    // kp PIN3 -- THE PAGE IS UNCHANGED: the same 9 dB gap still plays a capped 3 dB there.
    {
        const auto p = computeMatchProposal (mix9, side());
        const auto* lm = bandMove (p, 2);
        check (lm != nullptr && lm->valueDb == -kMatchBandCapDb && lm->capped && lm->measuredDb == -9.0f,
               "kp PIN3: the page's proposal (the default) still caps the 9 dB gap at 3 dB and says it was capped");
    }
}

} // namespace k4bpins
