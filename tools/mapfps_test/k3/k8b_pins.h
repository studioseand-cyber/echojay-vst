#pragma once

// =============================================================================
//  ROUND 8b's PINS: THE AUDITION SWITCH AND THE LEVEL MATCH (ky).
// =============================================================================
//
// Sean's ruling of 30 Oct. MatchHost has no setReferenceGainDb on this base, so
// the level match is driven through K's port (EJMatchAudition.h) with a fake
// that applies a floor, as the agreed interface says A's call will.
//
// ky PIN1   the gap line comes from the page view for the selected window and
//           is there before any flip: "Reference is 6.2 dB louder", "3.1 dB
//           quieter", and the loudest 30 s's own gap, not the whole song's;
// ky PIN2   the reference plays at its real level by default (the host is
//           asked for 0); matching cuts the reference only and the line reads
//           the gain applied: "Matched 24 of 30 dB" at a -24 floor;
// ky PIN3   a quieter reference offers no toggle (a toggle left on is turned
//           off and the host asked for 0), and nothing is offered without the
//           call;
// ky PIN4   a change of window, slot or reference asks the host again and the
//           line is redrawn from the new answer; the old answer is never drawn
//           for the new comparison, and nothing changed asks nothing;
// ky PIN5   the switch flips to the side not audible, never the audible one
//           (A's setAudible would mute it); a Live mix comes back by
//           switching the reference off.
// =============================================================================

#include "EJMatchAudition.h"
#include "k5_fakehost.h"

namespace k8bpins
{
using namespace echojay;
using k5pins::FakeMatchHost;

/** A host's level call as the agreed interface has it: a cut stops at the floor. */
struct FakeGainPort : MatchReferenceGainPort
{
    float floorDb = -24.0f;
    std::vector<float> calls;
    float setReferenceGainDb (float wantedDb) override
    {
        calls.push_back (wantedDb);
        return juce::jmax (wantedDb, floorDb);
    }
    float referenceGainFloorDb() const override { return floorDb; }
};

template <typename Check>
inline void runK8bPins (Check&& check)
{
    // n hops at quiet LUFS with a loud section [loudAt, loudAt + 300) at loud LUFS.
    auto series = [] (std::size_t n, std::size_t loudAt, float loud, float quiet)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            const bool isLoud = i >= loudAt && i < loudAt + 300;
            h.shortTermLufs = i < 29 ? -100.0f : (isLoud ? loud : quiet);
            h.shortTermTruePeak = h.shortTermLufs + 9.0f;
            h.bandPower.fill (1.0f);
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    auto sides = [] (float mixLufs, float refLufs)
    {
        auto side = [] (float lufs)
        {
            MatchSide s;
            s.macro.fill (-10.0f); s.hasMacro = true;
            s.macroReduction = SpectralReduction::WholeFileAverage;
            s.durationSeconds = 90.0f;
            s.integrated = lufs; s.truePeak = -1.0f; s.overs = 0;
            return s;
        };
        return MatchSides { side (mixLufs), side (refLufs) };
    };
    const juce::String refPath = "/refs/k8b-ref.wav";
    // The capture's loudest 30 s is at -10 LUFS and the reference's at -6, so
    // the window's gap is 4 dB whatever the whole files' integrated says.
    auto session = [&] (float mixLufs, float refLufs)
    {
        FakeMatchHost host;
        for (int i = 0; i < 2; ++i)
        {
            CaptureSnapshot c;
            c.id = juce::String ("17300000008") + juce::String (i); c.name = "Capture";
            c.hopFrames = series (900, 400, -10.0f, -20.0f); c.hopFramesHops = 900;
            host.snaps.push_back (c);
        }
        ReferenceResult r;
        r.name = "Song K8b"; r.path = refPath;
        r.hopFrames = series (600, 100, -6.0f, -16.0f); r.hops = 600;
        host.ref = r;
        host.mixSlot = k5pins::snapshotSlot (0);
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
        host.sides = sides (mixLufs, refLufs);
        return host;
    };
    auto viewOf = [] (const FakeMatchHost& h, MatchWindowMode m)
    {
        const MatchHost& host = h;
        return matchPageFor (host, m, host.matchSlots());
    };
    auto setMode = [&] (MatchWindowMode m) { matchWindowMemory().set (refPath, m); };
    setMode (MatchWindowMode::Whole);

    // ky PIN1 -- THE GAP LINE, PER WINDOW, BEFORE ANY FLIP.
    {
        auto louder  = session (-14.2f, -8.0f);
        auto quieter = session (-14.2f, -17.3f);
        juce::NamedValueSet page;
        const auto a = matchLevelLineOf (viewOf (louder, MatchWindowMode::Whole), louder.matchSlots(), nullptr, page);
        const auto b = matchLevelLineOf (viewOf (quieter, MatchWindowMode::Whole), quieter.matchSlots(), nullptr, page);
        const auto c = matchLevelLineOf (viewOf (louder, MatchWindowMode::Loudest30), louder.matchSlots(), nullptr, page);
        check (a.gap == "Reference is 6.2 dB louder" && b.gap == "Reference is 3.1 dB quieter"
               && c.gap == "Reference is 4 dB louder"
               && louder.audibleCalls.empty() && quieter.audibleCalls.empty(),
               "ky PIN1: the gap line comes from the page view for the selected window and shows before any flip "
               "(\"Reference is 6.2 dB louder\", \"3.1 dB quieter\", and the loudest 30 s's own 4 dB)",
               a.gap + " | " + b.gap + " | " + c.gap);
    }

    // ky PIN2 -- REAL LEVEL BY DEFAULT; THE MATCH CUTS THE REFERENCE AND SAYS WHAT WAS APPLIED.
    {
        auto host = session (-38.0f, -8.0f);
        FakeGainPort port;
        juce::NamedValueSet page;
        const auto v = viewOf (host, MatchWindowMode::Whole);
        const auto slots = host.matchSlots();
        matchLevelSync (&port, v, slots, page);
        const auto off = matchLevelLineOf (v, slots, &port, page);
        page.set (matchLevelOnId(), true);
        matchLevelSync (&port, v, slots, page);
        const auto on = matchLevelLineOf (v, slots, &port, page);

        auto deep = session (-14.2f, -8.0f);
        FakeGainPort deepPort; deepPort.floorDb = -60.0f;
        juce::NamedValueSet deepPage;
        deepPage.set (matchLevelOnId(), true);
        const auto dv = viewOf (deep, MatchWindowMode::Whole);
        matchLevelSync (&deepPort, dv, deep.matchSlots(), deepPage);
        const auto full = matchLevelLineOf (dv, deep.matchSlots(), &deepPort, deepPage);

        bool cutsOnly = true;
        for (float c : port.calls) cutsOnly = cutsOnly && c <= 0.0f;
        check (port.calls.size() == 2 && port.calls[0] == 0.0f && std::abs (port.calls[1] + 30.0f) < 1e-3f && cutsOnly
               && off.offerToggle && ! off.on && off.matched.isEmpty() && off.gap == "Reference is 30 dB louder"
               && on.on && on.matched == "Matched 24 of 30 dB" && full.matched == "Matched 6.2 of 6.2 dB",
               "ky PIN2: the reference plays at its real level by default (the host is asked for 0); the match cuts the "
               "reference only, and the line reads what was applied: \"Matched 24 of 30 dB\" at a -24 dB floor",
               on.matched + " | " + full.matched + " | " + juce::String ((int) port.calls.size()) + " calls");
    }

    // ky PIN3 -- A QUIETER REFERENCE OFFERS NO TOGGLE; NOTHING IS OFFERED WITHOUT THE CALL.
    {
        auto quieter = session (-14.2f, -17.3f);
        auto louder  = session (-14.2f, -8.0f);
        FakeGainPort port;
        juce::NamedValueSet page;
        page.set (matchLevelOnId(), true);   // left on from a louder reference
        const auto qv = viewOf (quieter, MatchWindowMode::Whole);
        matchLevelSync (&port, qv, quieter.matchSlots(), page);
        const auto q = matchLevelLineOf (qv, quieter.matchSlots(), &port, page);
        const auto noCall = matchLevelLineOf (viewOf (louder, MatchWindowMode::Whole), louder.matchSlots(), nullptr, page);
        check (! q.offerToggle && ! q.on && q.matched.isEmpty() && ! (bool) page.getWithDefault (matchLevelOnId(), true)
               && port.calls.size() == 1 && port.calls[0] == 0.0f
               && ! noCall.offerToggle && noCall.gap == "Reference is 6.2 dB louder",
               "ky PIN3: a quieter reference offers no toggle (one left on is turned off and the reference goes back to "
               "its real level), and without the host's call no toggle is offered at all",
               juce::String ((int) port.calls.size()) + " calls");
    }

    // ky PIN4 -- EVERY CHANGE ASKS AGAIN; THE OLD ANSWER IS NEVER DRAWN FOR A NEW COMPARISON.
    // The whole song's gap is 4 dB, as the loudest 30 s's is, so every step asks
    // for the same 4 dB cut: only the comparison changes, never what is asked.
    {
        auto host = session (-12.0f, -8.0f);
        FakeGainPort port;
        juce::NamedValueSet page;
        page.set (matchLevelOnId(), true);
        juce::StringArray seen, bad;
        auto step = [&] (const char* what, MatchWindowMode m, int expectCalls)
        {
            const auto v = viewOf (host, m);
            const auto slots = host.matchSlots();
            const auto before = matchLevelLineOf (v, slots, &port, page);
            const bool staleHidden = expectCalls == 0 || before.matched.isEmpty();
            const auto n0 = port.calls.size();
            matchLevelSync (&port, v, slots, page);
            const auto after = matchLevelLineOf (v, slots, &port, page);
            const bool callsOk = (int) (port.calls.size() - n0) == expectCalls;
            const float applied = port.calls.empty() ? 0.0f : juce::jmax (port.calls.back(), port.floorDb);
            const bool fromReturn = expectCalls == 0
                                 || after.matched == matchLevelMatchedLine (applied, port.calls.back());
            seen.add (juce::String (what) + ": " + after.matched);
            if (! (staleHidden && callsOk && fromReturn)) bad.add (what);
        };
        step ("first", MatchWindowMode::Whole, 1);
        port.floorDb = -3.0f;
        step ("nothing changed", MatchWindowMode::Whole, 0);
        host.mixSlot = k5pins::snapshotSlot (1);
        step ("slot", MatchWindowMode::Whole, 1);
        port.floorDb = -2.0f;
        host.ref->path = "/refs/k8b-other.wav";
        step ("reference", MatchWindowMode::Whole, 1);
        port.floorDb = -1.0f;
        step ("window", MatchWindowMode::Loudest30, 1);
        check (bad.isEmpty() && seen[0] == "first: Matched 4 of 4 dB" && seen[1] == "nothing changed: Matched 4 of 4 dB"
               && seen[2] == "slot: Matched 3 of 4 dB" && seen[3] == "reference: Matched 2 of 4 dB"
               && seen[4] == "window: Matched 1 of 4 dB",
               "ky PIN4: a change of window, slot or reference asks the host again and the line comes from the new "
               "answer; the old answer is never drawn for the new comparison, and nothing changed asks nothing",
               seen.joinIntoString (" | ") + (bad.isEmpty() ? "" : " | bad: " + bad.joinIntoString (", ")));
    }

    // ky PIN5 -- THE SWITCH FLIPS TO THE SIDE NOT AUDIBLE.
    {
        bool ok = true;
        for (bool refIsTop : { true, false })
        {
            const auto refSide = refIsTop ? MatchRefSide::Top : MatchRefSide::Bottom;
            const auto mixSide = refIsTop ? MatchRefSide::Bottom : MatchRefSide::Top;
            ok = ok && matchAuditionFlipTarget (refIsTop, false, false) == refSide   // hearing the mix: to the reference
                    && matchAuditionFlipTarget (refIsTop, true,  false) == mixSide   // hearing the reference: to the mix
                    && matchAuditionFlipTarget (refIsTop, true,  true)  == refSide;  // Live mix: the reference off
        }
        auto host = session (-14.2f, -8.0f);
        host.setAudible (matchAuditionFlipTarget (false, false, false));
        check (ok && host.audibleCalls.size() == 1 && host.audibleCalls[0] == MatchRefSide::Bottom
               && matchAuditionLabel (false) == "Hearing mix" && matchAuditionLabel (true) == "Hearing reference",
               "ky PIN5: the switch flips to the side not audible, never the audible one (setAudible would mute it); "
               "a Live mix comes back by switching the reference off");
    }
    setMode (MatchWindowMode::Whole);
}

} // namespace k8bpins
