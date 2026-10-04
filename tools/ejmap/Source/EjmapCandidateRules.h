/*
  EjmapCandidateRules.h

  THE RULES THAT DECIDE A MULTI-CANDIDATE RECORD AFTER ITS SWEEPS (ruled 4 Oct 2026, from Sean's 3 Oct run:
  65 needs_review rows, 50 of them "N threshold candidates"). Rule 1 (the compressor stage word, EjmapSweep.h) decides at
  PLAN time; these decide from what was MEASURED, never from a name alone - every one needs the picked candidate to have
  certified, and the channel rules need the picked candidate's own sweep to show both output channels moving together.

    linked_pair      (item 1)  two candidates whose names differ only by a literal channel token; both certify; their
                               2 dB curves agree within 0.5 dB at every common position; and in the FIRST candidate's own
                               sweep both output channels agree within 0.5 dB at every reading -> the first is the amount,
                               the twin and every link/mode control stay at instantiate (they are in neutral).
    leader_follower  (item 10) the same pair, the FIRST certifies and its channels agree, the second does NOT certify
                               (PuigChild 670 (s): Right Threshold flat because Left drives both in Linked mode).
    master_over_trims(item 11) one candidate's name equals every other's minus a channel or band suffix (INPUT vs Input
                               Left / Input Right; Threshold vs Threshold 1 / 2 / 3), and it certifies -> it is the amount.
    main_over_aux    (item 12) the literal words "Stereo/Main" against "Aux" (Ozone 12 Vintage Compressor): Main certifies.

  The record carries pickedCandidate + ruleDecided {rule, pick, twin|trims, measurements, note} exactly as Rule 1 does, so
  the export, the tone check and the row go through the same single view. The tone check for any decided pick writes in
  the server's order (engage, neutral including the twin, ratio, amount last) and requires BOTH output channels within
  0.5 dB of g, which catches a unit whose twin write mirrors back onto the amount.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <map>
#include <vector>

namespace ejmap::candidaterules
{

inline constexpr double kCurveAgreeDb   = 0.5;   // the two candidates' 2 dB curves, at every common position
inline constexpr double kChannelAgreeDb = 0.5;   // both output channels of the first candidate's own sweep, at every reading

struct CandidateFacts
{
    int index = -1;
    juce::String name;
    bool certified = false;
    std::map<double, double> curve2;             // norm -> in_at_gr["2"] (peak dBFS, the record's convention), numeric points only
    std::optional<double> channelWorstDb;        // from this candidate's OWN raw sweep: worst |ch0 - ch1| over every reading (readings above silence)
    int channelReadings = 0;
};

struct Decision
{
    bool decided = false;
    juce::String rule;                           // linked_pair | leader_follower | master_over_trims | main_over_aux
    int pick = -1; juce::String pickName;
    juce::StringArray others;                    // the twin, or the trims
    juce::String note;                           // why, with the measurements
    juce::String whyNot;                         // the nearest rule that did not fire, for the row
};

// THE CHANNEL TOKENS, literal (ruled 4 Oct): a name pair is a channel pair when the names are identical once one token
// from a listed pair is removed from the same END of each (suffix, or prefix - UnFairchild's "L Threshold / R Threshold"),
// or when one name is the other plus " R" (Abbey Road RS124's "Input Control / Input Control R").
inline const std::vector<std::pair<juce::String, juce::String>> kChannelPairs
{
    { "L", "R" }, { "Left", "Right" }, { "1", "2" }, { "A", "B" }, { "L/M", "R/S" }, { "M", "S" }
};

inline juce::String stripToken (const juce::String& name, const juce::String& token, bool& stripped)
{
    stripped = false;
    const auto n = name.trim();
    const auto t = token.toLowerCase();
    if (n.toLowerCase().endsWith (" " + t)) { stripped = true; return n.dropLastCharacters (token.length() + 1).trim(); }
    if (n.toLowerCase().startsWith (t + " ")) { stripped = true; return n.substring (token.length() + 1).trim(); }
    return n;
}

// Returns the shared base name when a and b form a channel pair (a first, b the twin), else empty.
inline juce::String channelPairBase (const juce::String& a, const juce::String& b)
{
    if (a.trim() + " R" == b.trim() || a.trim() + " r" == b.trim()) return a.trim();     // "" / " R"
    for (const auto& [x, y] : kChannelPairs)
    {
        bool sa = false, sb = false;
        const auto ba = stripToken (a, x, sa), bb = stripToken (b, y, sb);
        if (sa && sb && ba.equalsIgnoreCase (bb) && ba.isNotEmpty()) return ba;
    }
    return {};
}

// ONE NAME IS ANOTHER MINUS A CHANNEL OR BAND SUFFIX (item 11): "INPUT" vs "Input Left"; "Threshold" vs "Threshold 2".
inline bool isMasterOf (const juce::String& master, const juce::String& trim)
{
    const auto m = master.trim().toLowerCase(), t = trim.trim().toLowerCase();
    if (! t.startsWith (m + " ") || t.length() <= m.length() + 1) return false;
    const auto suffix = t.substring (m.length() + 1).trim();
    static const juce::StringArray tokens { "l", "r", "left", "right", "a", "b", "l/m", "r/s", "m", "s", "low", "mid", "high", "lo", "hi" };
    if (tokens.contains (suffix)) return true;
    return suffix.containsOnly ("0123456789") && suffix.isNotEmpty();
}

inline juce::String dbs (double v) { return juce::String (v, 2); }

inline Decision decide (const std::vector<CandidateFacts>& c)
{
    Decision d;
    if (c.size() < 2) { d.whyNot = "fewer than two candidates"; return d; }
    auto certifiedCount = [&] { int n = 0; for (const auto& x : c) if (x.certified) ++n; return n; };
    auto channelsAgree = [&] (const CandidateFacts& x, juce::String& why)
    {
        if (! x.channelWorstDb) { why = "no per-channel readings in its own sweep (an older trace, or a mono unit)"; return false; }
        if (*x.channelWorstDb > kChannelAgreeDb) { why = "its own sweep's output channels differ by up to " + dbs (*x.channelWorstDb) + " dB (bar " + dbs (kChannelAgreeDb) + ")"; return false; }
        return true;
    };
    // items 1 and 10: a channel pair
    if (c.size() == 2)
    {
        const auto base = channelPairBase (c[0].name, c[1].name);
        if (base.isNotEmpty())
        {
            const auto& first = c[0]; const auto& twin = c[1];
            juce::String why;
            if (! first.certified) { d.whyNot = "channel pair '" + base + "': the first candidate did not certify"; return d; }
            if (! channelsAgree (first, why)) { d.whyNot = "channel pair '" + base + "': " + why + " - the channels are independent, no pair rule"; return d; }
            if (twin.certified)
            {
                int common = 0; double worst = 0.0;
                for (const auto& [n, v] : first.curve2) { auto it = twin.curve2.find (n); if (it != twin.curve2.end()) { ++common; worst = juce::jmax (worst, std::abs (v - it->second)); } }
                if (common == 0) { d.whyNot = "channel pair '" + base + "': no common 2 dB position to compare"; return d; }
                if (worst > kCurveAgreeDb) { d.whyNot = "channel pair '" + base + "': the two 2 dB curves differ by up to " + dbs (worst) + " dB over " + juce::String (common) + " common positions (bar " + dbs (kCurveAgreeDb) + ")"; return d; }
                d.decided = true; d.rule = "linked_pair"; d.pick = first.index; d.pickName = first.name; d.others.add (twin.name);
                d.note = "linked pair '" + base + "' (measured, not by label): both certified; their 2 dB curves agree within " + dbs (worst) + " dB over " + juce::String (common) + " common positions; in the first's own sweep both output channels agree within "
                       + dbs (*first.channelWorstDb) + " dB over " + juce::String (first.channelReadings) + " readings. '" + first.name + "' is the amount; '" + twin.name + "' and every link/mode control stay at their instantiate values (neutral)";
                return d;
            }
            d.decided = true; d.rule = "leader_follower"; d.pick = first.index; d.pickName = first.name; d.others.add (twin.name);
            d.note = "leader/follower '" + base + "' (measured): '" + first.name + "' certified and in its own sweep both output channels agree within " + dbs (*first.channelWorstDb) + " dB over " + juce::String (first.channelReadings)
                   + " readings, while '" + twin.name + "' did not certify (it follows the leader). '" + first.name + "' is the amount; '" + twin.name + "' stays at its instantiate value (neutral)";
            return d;
        }
    }
    // item 12: Stereo/Main over Aux, literal words
    {
        const CandidateFacts* main = nullptr; const CandidateFacts* aux = nullptr;
        for (const auto& x : c) { if (x.name.contains ("Stereo/Main")) main = &x; else if (x.name.contains ("Aux")) aux = &x; }
        if (main && aux && c.size() == 2)
        {
            if (! main->certified) { d.whyNot = "Stereo/Main vs Aux: Main did not certify"; return d; }
            d.decided = true; d.rule = "main_over_aux"; d.pick = main->index; d.pickName = main->name; d.others.add (aux->name);
            d.note = "Stereo/Main over Aux (literal words): '" + main->name + "' certified" + (aux->certified ? juce::String() : juce::String (", '" + aux->name + "' did not")) + "; '" + main->name + "' is the amount, '" + aux->name + "' stays at its instantiate value";
            return d;
        }
    }
    // item 11: a master whose name is every other's minus a channel or band suffix
    for (const auto& m : c)
    {
        bool all = true; juce::StringArray trims;
        for (const auto& t : c) { if (&t == &m) continue; if (! isMasterOf (m.name, t.name)) { all = false; break; } trims.add (t.name); }
        if (! all) continue;
        if (! m.certified) { d.whyNot = "master '" + m.name + "' over trims (" + trims.joinIntoString (", ") + "): the master did not certify - a name match alone never decides"; return d; }
        d.decided = true; d.rule = "master_over_trims"; d.pick = m.index; d.pickName = m.name; d.others = trims;
        d.note = "master over trims: '" + m.name + "' is every other candidate's name minus a channel or band suffix (" + trims.joinIntoString (", ") + ") and it certified; it is the amount, the trims stay at their instantiate values (neutral)";
        return d;
    }
    d.whyNot = juce::String (c.size()) + " candidates, " + juce::String (certifiedCount()) + " certified: no channel pair, no master, no Main/Aux";
    return d;
}

} // namespace ejmap::candidaterules
