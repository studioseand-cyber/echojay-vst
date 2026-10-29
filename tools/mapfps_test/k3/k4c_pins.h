#pragma once

// =============================================================================
//  ROUND 4c's PINS: K5, ATTRIBUTION FROM A REAL CAPTURE (kc).
// =============================================================================
//
// A synthetic CaptureSnapshot (A's real type) with the host's own channel and
// two Links, fed through attribInsFromCapture and on into attributeBandMove:
// the bus from the rack input when present, else the output with the bus trim
// added back; each Link's trim added back; roles from MatchHost::liveLinks();
// the grid and coverage checks on the real series.
// =============================================================================

#include "EJMatchAttributionCapture.h"
#include "EJMatchTab.h"          // MatchLink, the host's projection of a Link
#include "PluginProcessor.h"     // CaptureSnapshot, ChannelMeterData

namespace k4cpins
{
using namespace echojay;

template <typename Check>
inline void runK4cPins (Check&& check)
{
    auto near = [] (double a, double b, double t) { return std::abs (a - b) <= t; };
    // n hops, every band at power p except band 2 at p2, every hop with frames.
    auto series = [] (std::size_t n, float p, float p2)
    {
        std::vector<MeterEngine::HopFrame> v (n);
        for (auto& h : v) { h.bandPower.fill (p); h.bandPower[2] = p2; h.bandPeak = h.bandPower; h.bandFrames = 9; }
        return v;
    };
    auto channel = [&] (const juce::String& uid, const juce::String& name, float preTrimLowMid, float gainDb, int hops)
    {
        ChannelMeterData ch;
        ch.uid = uid; ch.name = name; ch.gainDb = gainDb;
        // The ring is post-trim, so the series carries the trim.
        ch.hopFrames = series ((std::size_t) hops, 0.01f, preTrimLowMid * std::pow (10.0f, gainDb / 10.0f));
        ch.hops = hops;
        return ch;
    };
    // The bus OUTPUT reads 2.0 in every band with a +3.01 dB bus trim, so its
    // pre-trim power is 1.0. Gtr L sits at 0.40 of the low-mids behind a -6 dB
    // trim, Gtr R at 0.35, and the host's own channel (uid "") is no Link.
    auto capture = [&]
    {
        CaptureSnapshot c;
        c.hopFrames = series (600, 2.0f, 2.0f);
        c.hopFramesHops = 600;
        c.busGainDbAtCapture = (float) (10.0 * std::log10 (2.0));
        c.channels.push_back (channel ("", "Host", 0.9f, 0.0f, 600));
        c.channels.push_back (channel ("g1", "Gtr L", 0.40f, -6.0f, 600));
        c.channels.push_back (channel ("g2", "Gtr R", 0.35f, 0.0f, 600));
        return c;
    };
    std::vector<MatchLink> links (2);
    links[0].uid = "g1"; links[0].name = "Gtr L"; links[0].role = "guitar";
    links[1].uid = "g2"; links[1].name = "Gtr R"; links[1].role = "guitar";
    std::array<std::optional<float>, 6> D {};
    D[2] = 2.8f;
    const HopRange whole { 0, 600 };

    // kc PIN1 -- NO RACK-INPUT SERIES: the bus is the output with its trim added
    // back, busTap "output"; the host's channel is skipped; roles come from the host.
    {
        const auto c = capture();
        const auto ins = attribInsFromCapture (c, whole, D, links);
        const auto& a = ins[2];
        check (a && a->busTap == MatchBusTap::Output && near (a->busPower, 1.0, 1e-5) && a->busHopCount == 600
               && a->links.size() == 2 && a->links[0].name == "Gtr L" && a->links[0].role == "guitar"
               && a->links[0].gainDb == -6.0f && ! ins[0] && ! ins[5],
               "kc PIN1: without a rack-input series P_bus is the output with busGainDbAtCapture added back, busTap "
               "\"output\", the host's channel is no Link, and a band with no gap gets no AttribIn",
               a ? juce::String (a->busPower, 4) : juce::String ("none"));
    }

    // kc PIN2 -- WITH ONE: the bus is the rack input as read (pre-chain, pre-trim), busTap "input".
    {
        auto c = capture();
        c.hopFramesRackIn = series (601, 0.5f, 0.5f);
        c.hopFramesRackInHops = 601;
        const auto ins = attribInsFromCapture (c, whole, D, links);
        const auto& a = ins[2];
        check (a && a->busTap == MatchBusTap::Input && near (a->busPower, 0.5, 1e-6) && a->busHopCount == 601,
               "kc PIN2: with a rack-input series P_bus is that series as read, with no trim added, and busTap \"input\"",
               a ? juce::String (a->busPower, 4) : juce::String ("none"));
    }

    // kc PIN3 -- END TO END: the two Links take the low-mids between them (C4 section 5's 0.40 + 0.35).
    {
        const auto c = capture();
        const auto ins = attribInsFromCapture (c, whole, D, links);
        const auto r = attributeBandMove (*ins[2]);
        bool shares = r.links.size() == 2;
        for (const auto& k : r.links)
            shares = shares && (k.name == "Gtr L" ? near (k.share, 0.40, 1e-4) : near (k.share, 0.35, 1e-4));
        check (r.target == AttribTarget::Pair && r.rule == AttribRule::R7Pair && shares
               && r.combinedShare && near (*r.combinedShare, 0.75, 1e-4)
               && r.valueDb && near (*r.valueDb, -4.4, 0.05) && r.caveat,
               "kc PIN3: from the capture, Gtr L (trim added back) and Gtr R take the low-mids as a pair at 0.40 + 0.35, "
               "-4.4 dB on each, with the caveat",
               r.reason);
    }

    // kc PIN4 -- THE GRID AND THE FRAMES: a Link three hops short is unknown, and
    // so is one whose frames cover under 90% of the window.
    {
        auto c = capture();
        c.channels[1] = channel ("g1", "Gtr L", 0.40f, -6.0f, 597);
        auto thin = channel ("g2", "Gtr R", 0.35f, 0.0f, 600);
        for (std::size_t i = 0; i < 600; i += 5) thin.hopFrames[i].bandFrames = 0;   // 80% covered
        c.channels[2] = thin;
        const auto ins = attribInsFromCapture (c, whole, D, links);
        const auto r = attributeBandMove (*ins[2]);
        check (ins[2]->links[1].frameCoverage < 0.9 && near (ins[2]->links[1].frameCoverage, 0.8, 1e-9)
               && r.unknown.size() == 2 && r.known.empty() && r.target == AttribTarget::Bus,
               "kc PIN4: a Link three hops short of the bus, and one with frames over 80% of the window, are both "
               "unknown, and the move goes on the bus",
               juce::String ((int) r.unknown.size()) + " unknown");
    }
}

} // namespace k4cpins
