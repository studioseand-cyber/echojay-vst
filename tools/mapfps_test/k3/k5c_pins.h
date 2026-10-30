#pragma once

// =============================================================================
//  ROUND 5c's PINS: combinedShare ON THE WIRE (ks).
// =============================================================================
//
// C3 rev 3: combinedShare is "null for one track or the bus", and a pair's
// figure is held to 0..2 (B's staging, b43c4a3). Both are read off the
// SERIALISED block, because the wire is where null and 0 differ.
// =============================================================================

#include "EJMatchBlock.h"

namespace k5cpins
{
using namespace echojay;

template <typename Check>
inline void runK5cPins (Check&& check)
{
    auto blockWith = [] (const juce::String& target, std::optional<double> cs, int nLinks)
    {
        MatchBlockIn in;
        in.referenceId = "ref_1"; in.referenceName = "Song X";
        in.mix.integrated = -10.0; in.ref.integrated = -9.0;
        MatchBlockMove m;
        m.kind = "band"; m.band = 2; m.valueDb = -2.8; m.measuredDb = 2.8;
        MatchBlockAttribution a;
        a.target = target; a.combinedShare = cs; a.reason = "x";
        for (int i = 0; i < nLinks; ++i)
            a.links.push_back ({ "uid" + juce::String (i), "Gtr " + juce::String (i), 0.4 });
        m.attribution = a;
        in.moves.push_back (m);
        return buildMatchBlock (in);
    };
    // The block as the server receives it: serialised, parsed and read.
    auto wire = [] (const MatchBlockRead& r)
    {
        const juce::String text = juce::JSON::toString (r.block, true);
        const juce::var parsed = juce::JSON::parse (text);
        return std::make_pair (text, readMatchBlock (parsed));
    };
    auto attrOf = [] (const MatchBlockRead& r)
    {
        const juce::var moves = r.block.getProperty ("moves", juce::var());
        const juce::var m0 = moves[0];
        return m0.getProperty ("attribution", juce::var());
    };

    // ks PIN1 -- NULL, NOT 0, FOR A TRACK OR A BUS MOVE, whatever the writer was
    // handed; and a block that arrives with a 0 there loses the attribution.
    {
        const auto t = wire (blockWith ("track", 0.0, 1));
        const auto b = wire (blockWith ("bus", 0.5, 0));
        const juce::var ta = attrOf (t.second), ba = attrOf (b.second);
        // A block written by something else, with combinedShare 0 on a track.
        juce::var zero = juce::JSON::parse (t.first);
        {
            juce::var mv = zero.getProperty ("moves", juce::var());
            juce::var m0 = mv[0];
            juce::var at = m0.getProperty ("attribution", juce::var());
            if (auto* o = at.getDynamicObject()) o->setProperty ("combinedShare", 0.0);
        }
        const auto zr = readMatchBlock (zero);
        const juce::var zm = zr.block.getProperty ("moves", juce::var());
        const juce::var zm0 = zm[0];
        check (t.first.contains ("\"combinedShare\": null") && b.first.contains ("\"combinedShare\": null")
               && ! t.first.contains ("\"combinedShare\": 0") && ! b.first.contains ("\"combinedShare\": 0")
               && ta.isObject() && ta.getProperty ("combinedShare", 1).isVoid()
               && ba.isObject() && ba.getProperty ("combinedShare", 1).isVoid()
               && t.second.readable && b.second.readable
               && zr.readable && zm0.getProperty ("attribution", 1).isVoid() && zm0.isObject(),
               "ks PIN1: a track or bus move goes on the wire with combinedShare null, never 0, whatever it was handed; "
               "a track move arriving with 0 loses its attribution and keeps the move",
               t.first.fromFirstOccurrenceOf ("combinedShare", true, false).upToFirstOccurrenceOf (",", false, false));
    }

    // ks PIN2 -- A PAIR CARRIES ITS FIGURE, 0..2: 0.75 and 2 survive the wire,
    // 2.01 and a missing figure drop the attribution and keep the move.
    {
        const auto p075 = wire (blockWith ("pair", 0.75, 2));
        const auto p2   = wire (blockWith ("pair", 2.0, 2));
        const auto p201 = wire (blockWith ("pair", 2.01, 2));
        const auto pnul = wire (blockWith ("pair", std::nullopt, 2));
        const juce::var a075 = attrOf (p075.second), a2 = attrOf (p2.second);
        const juce::var a201 = attrOf (p201.second), anul = attrOf (pnul.second);
        const juce::var mv201 = p201.second.block.getProperty ("moves", juce::var());
        check (p075.first.contains ("\"combinedShare\": 0.75") && a075.isObject()
               && (double) a075.getProperty ("combinedShare", 0) == 0.75
               && a2.isObject() && (double) a2.getProperty ("combinedShare", 0) == 2.0
               && a201.isVoid() && anul.isVoid() && mv201.size() == 1,
               "ks PIN2: a pair's combinedShare goes on the wire as its figure; 0.75 and 2 keep the attribution, "
               "2.01 or no figure drops it and keeps the move",
               juce::JSON::toString (a075, true));
    }
}

} // namespace k5cpins
