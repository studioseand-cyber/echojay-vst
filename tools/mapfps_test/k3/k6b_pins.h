#pragma once

// =============================================================================
//  ROUND 6b's PINS: ONE REPORT PER COMPARISON (ko).
// =============================================================================
//
// ko PIN1        the same press twice posts once, and the page still says
//                "Report in chat";
// ko PIN2..5     a different capture, reference, window or hop count posts
//                again (one row each);
// ko PIN6        the morph plays on every press, posted or not;
// ko PIN7        a Live mix has no identity, so every press posts.
//
// Each row drives what press() calls: matchPageViewOf over a FakeMatchHost,
// matchReportIdentityOf, matchPressActs and matchPressReport.
// =============================================================================

#include "EJMatchHandOff.h"
#include "k5_fakehost.h"

namespace k6bpins
{
using namespace echojay;
using k5pins::FakeMatchHost;

template <typename Check>
inline void runK6bPins (Check&& check)
{
    auto series = [] (std::size_t n)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            auto& h = v[i];
            h.shortTermLufs = i < 29 ? -100.0f : (i % 2 == 0 ? -10.0f : -20.0f);
            h.shortTermTruePeak = h.shortTermLufs + 8.0f;
            h.bandPower.fill (1.0f);
            h.bandPeak = h.bandPower;
            h.bandFrames = 9;
        }
        return v;
    };
    auto capture = [&] (const char* id)
    {
        CaptureSnapshot c;
        c.id = id; c.name = juce::String ("Capture ") + id;
        c.hopFrames = series (600); c.hopFramesHops = 600;
        ChannelMeterData self; self.name = "Mix Bus"; self.hopFrames = c.hopFrames; self.hops = 600;
        c.channels = { self };
        return c;
    };
    auto reference = [&] (const char* path)
    {
        ReferenceResult r;
        r.name = "Song X"; r.path = path;
        r.hopFrames = series (1800); r.hops = 1800;
        return r;
    };
    auto session = [&]
    {
        FakeMatchHost host;
        host.snaps = { capture ("1730000000001"), capture ("1730000000002") };
        host.ref = reference ("/refs/k6b-song-x.wav");
        host.mixSlot = k5pins::snapshotSlot (0);
        host.refSlot.kind = CompareSlotState::Kind::Reference; host.refSlot.index = 0;
        return host;
    };
    auto sides = []
    {
        MatchSide m, r;
        m.macro = { 0.0f, 0.0f, 3.36f, 0.0f, 0.0f, 0.0f };
        m.hasMacro = true; m.macroReduction = SpectralReduction::WholeWindowAverage;
        m.durationSeconds = 60.0f; m.integrated = -14.0f; m.truePeak = -1.0f;
        r.macro.fill (0.0f); r.hasMacro = true; r.macroReduction = SpectralReduction::WholeFileAverage;
        r.durationSeconds = 180.0f; r.integrated = -12.0f; r.truePeak = -0.5f;
        return MatchSides { m, r };
    };
    // ONE PRESS, as press() makes it after the morph has started.
    auto press = [&] (FakeMatchHost& host, juce::NamedValueSet& page)
    {
        const auto view = matchPageViewOf (host, sides(), host.matchSlots());
        const auto id   = matchReportIdentityOf (view);
        const auto acts = matchPressActs (true, id, page);
        if (acts.post) matchPressReport (host, matchHandOffOf (host, view), id, page);
        else           page.set (matchPageReportInChatId(), true);
        return acts;
    };
    auto inChat = [] (const juce::NamedValueSet& p) { return (bool) p.getWithDefault (matchPageReportInChatId(), false); };
    const juce::String refPath = "/refs/k6b-song-x.wav";
    matchWindowMemory().set (refPath, MatchWindowMode::Whole);

    // ko PIN1 -- THE SAME PRESS TWICE POSTS ONCE.
    {
        auto host = session();
        juce::NamedValueSet page;
        const auto a = press (host, page);
        page.remove (matchPageReportInChatId());   // so the second press must set it again
        const auto b = press (host, page);
        check (a.post && ! b.post && host.posts.size() == 1 && inChat (page),
               "ko PIN1: the same press twice posts one match_report, and the second press says \"Report in chat\" "
               "for it", juce::String ((int) host.posts.size()) + " posted");
    }

    // ko PIN2..5 -- ANY OF THE FOUR CHANGED POSTS AGAIN.
    {
        auto twice = [&] (std::function<void (FakeMatchHost&)> change)
        {
            auto host = session();
            juce::NamedValueSet page;
            press (host, page);
            change (host);
            press (host, page);
            const auto n = (int) host.posts.size();
            matchWindowMemory().set (refPath, MatchWindowMode::Whole);
            return n;
        };
        const int capN = twice ([] (FakeMatchHost& h) { h.mixSlot = k5pins::snapshotSlot (1); });
        check (capN == 2, "ko PIN2: another capture in the mix slot posts a new report",
               juce::String (capN) + " posted");
        const int refN = twice ([&] (FakeMatchHost& h) { h.ref = reference ("/refs/k6b-song-y.wav"); });
        check (refN == 2, "ko PIN3: another reference posts a new report", juce::String (refN) + " posted");
        const int winN = twice ([&] (FakeMatchHost&) { matchWindowMemory().set (refPath, MatchWindowMode::Loudest30); });
        check (winN == 2, "ko PIN4: the other window posts a new report", juce::String (winN) + " posted");
        const int hopN = twice ([] (FakeMatchHost& h) { h.snaps[0].hopFramesHops = 601; });
        check (hopN == 2, "ko PIN5: the same capture with a different hop count posts a new report",
               juce::String (hopN) + " posted");
    }

    // ko PIN6 -- THE MORPH PLAYS ON EVERY PRESS.
    {
        auto host = session();
        juce::NamedValueSet page;
        const auto a = press (host, page);
        const auto b = press (host, page);
        const auto c = press (host, page);
        const auto refused = matchPressActs (false, matchReportIdentityOf (matchPageViewOf (host, sides(), host.matchSlots())),
                                             juce::NamedValueSet {});
        check (a.morph && b.morph && c.morph && a.post && ! b.post && ! c.post && ! refused.morph && ! refused.post,
               "ko PIN6: the morph plays on every press, the first that posts and the ones that do not; a proposal "
               "that cannot play neither morphs nor posts");
    }

    // ko PIN7 -- A LIVE MIX HAS NO IDENTITY: every press posts.
    {
        auto host = session();
        host.mixSlot = {}; host.mixSlot.kind = CompareSlotState::Kind::Live;
        juce::NamedValueSet page;
        press (host, page);
        press (host, page);
        const bool none = ! matchReportIdentityOf (matchPageViewOf (host, sides(), host.matchSlots())).has_value();
        check (none && host.posts.size() == 2,
               "ko PIN7: a Live mix slot holds no capture, so a press has no identity and every press posts",
               juce::String ((int) host.posts.size()) + " posted");
    }
}

} // namespace k6bpins
