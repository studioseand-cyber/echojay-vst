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
    std::map<double, double> onset1;             // norm -> in_at_gr["1"]: where gain reduction begins (1 dB) at that position (27 Oct)
    std::optional<double> channelWorstDb;        // from this candidate's OWN raw sweep: worst |ch0 - ch1| over every reading (readings above silence)
    int channelReadings = 0;
    int pairWritten = -1; juce::String pairWrittenName;   // this candidate's sweep wrote a twin WITH it at every position (a pair re-sweep, ruled 6 Oct)
};

struct Decision
{
    bool decided = false;
    juce::String rule;                           // linked_pair | leader_follower | master_over_trims | main_over_aux
    int pick = -1; juce::String pickName;
    juce::StringArray others;                    // the twin, or the trims
    juce::StringArray sections;                  // 1 Nov (the multi-stage rule): each other stage, a section of its own, dialled only when named
    juce::String note;                           // why, with the measurements
    juce::String whyNot;                         // the nearest rule that did not fire, for the row
    int pairWith = -1; juce::String pairWithName; // dual_mono_pair: the twin written WITH the amount (ruled 6 Oct)
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

// THE ONSET RULE (Kathy, 25 Oct): several threshold candidates and no rule above decided - move each and see which shifts the ONSET of gain
// reduction (the input where 1 dB of reduction begins) at its positions: exactly one shifts it by kOnsetShiftDb or more -> that is the
// threshold; none or several -> unsure, the reason naming each candidate's shift
inline constexpr double kOnsetShiftDb = 3.0;
inline std::optional<double> onsetShiftDb (const std::map<double, double>& onset)
{
    if (onset.size() < 3) return std::nullopt;
    double lo = 1e9, hi = -1e9; for (const auto& [n, v] : onset) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
    return hi - lo;
}
struct OnsetVerdict { int pick = -1; juce::String pickName, text; juce::StringArray sections; bool multiStage = false; int pairWith = -1; juce::String pairWithName; };
// THE MULTI-STAGE RULE (Kathy, 1 Nov; Maag MAGNUM-K: Threshold 1/2 12.2 dB, K Comp Threshold 1/2 16.9, Limiter Threshold 1/2 6.0 - six
// candidates that each move the onset). Several candidates move where gain reduction begins: the channel pairs among them (a name and
// its twin by a channel token, shifts within kPairShiftDb) are ONE stage; a stage whose threshold moves the onset at the plugin's
// instantiate state is engaged by default (a bypassed stage cannot move it); the stage that moves it the MOST fills the core role,
// when it leads the next stage by kStageLeadDb; every other stage is a section of its own, dialled only when the user names it
inline constexpr double kPairShiftDb = 0.5, kStageLeadDb = 1.0;
inline OnsetVerdict onsetRule (const std::vector<std::pair<juce::String, std::optional<double>>>& shifts, const std::vector<int>& indexes)
{
    OnsetVerdict v; juce::StringArray movers, parts;
    for (size_t i = 0; i < shifts.size(); ++i)
    {
        const auto& [n, sh] = shifts[i];
        parts.add ("'" + n + "' " + (sh ? juce::String (*sh, 1) + " dB" : juce::String ("not read")));
        if (sh && *sh >= kOnsetShiftDb) { movers.add (n); if (movers.size() == 1) { v.pick = i < indexes.size() ? indexes[i] : -1; v.pickName = n; } }
    }
    if (movers.size() == 1) { v.text = "the onset rule: only '" + v.pickName + "' shifts where gain reduction begins (" + parts.joinIntoString (", ") + "): it is the threshold"; return v; }
    v.pick = -1; v.pickName = {};
    if (movers.size() > 1)
    {
        struct Stage { size_t first; double shift; juce::StringArray names; std::vector<size_t> members; };
        std::vector<Stage> stages;
        for (size_t i = 0; i < shifts.size(); ++i)
        {
            const auto& [n, sh] = shifts[i]; if (! sh || *sh < kOnsetShiftDb) continue;
            bool joined = false;
            for (auto& st : stages)
            {   const auto& fn = shifts[st.first].first;
                if ((channelPairBase (fn, n).isNotEmpty() || channelPairBase (n, fn).isNotEmpty()) && std::abs (st.shift - *sh) <= kPairShiftDb) { st.names.add (n); st.members.push_back (i); joined = true; break; } }
            if (! joined) stages.push_back ({ i, *sh, juce::StringArray { n }, std::vector<size_t> { i } });
        }
        std::sort (stages.begin(), stages.end(), [] (const Stage& a, const Stage& b) { return a.shift > b.shift; });
        juce::StringArray st; for (const auto& s : stages) st.add ("[" + s.names.joinIntoString (" / ") + "] " + juce::String (s.shift, 1) + " dB");
        if (stages.size() == 1 || stages[0].shift - stages[1].shift >= kStageLeadDb)
        {
            v.multiStage = true; v.pick = stages[0].first < indexes.size() ? indexes[stages[0].first] : -1; v.pickName = shifts[stages[0].first].first;
            for (size_t k = 1; k < stages.size(); ++k) v.sections.add (stages[k].names.joinIntoString (" / "));
            // a channel pair as the core stage: its twin is written WITH the amount (the dual-mono rule, ruled 6 Oct) - one channel alone never
            if (stages[0].members.size() >= 2 && stages[0].members[1] < indexes.size()) { v.pairWith = indexes[stages[0].members[1]]; v.pairWithName = shifts[stages[0].members[1]].first; }
            v.text = "the multi-stage rule: " + juce::String ((int) stages.size()) + " engaged stage(s) move where gain reduction begins (" + st.joinIntoString (", ") + "); '" + v.pickName
                   + "' moves it the most and fills the core role" + (v.pairWith >= 0 ? " (its channel twin '" + v.pairWithName + "' written with it)" : juce::String()) + (v.sections.isEmpty() ? juce::String() : "; the other stages are sections of their own, dialled only when named: " + v.sections.joinIntoString ("; "));
            return v;
        }
        v.text = "the multi-stage rule: the two stages that move the onset most do so alike (" + st.joinIntoString (", ") + ", lead under " + juce::String (kStageLeadDb, 0) + " dB): unsure, no single core stage";
        return v;
    }
    v.text = movers.isEmpty() ? "the onset rule: no candidate shifts where gain reduction begins by " + juce::String (kOnsetShiftDb, 0) + " dB or more (" + parts.joinIntoString (", ") + "): unsure"
                              : "the onset rule: " + juce::String (movers.size()) + " candidates each shift where gain reduction begins (" + parts.joinIntoString (", ") + "): unsure, no single threshold";
    return v;
}
inline Decision decideNamed (const std::vector<CandidateFacts>& c);
inline Decision decide (const std::vector<CandidateFacts>& c)
{
    auto d = decideNamed (c);
    if (d.decided || c.size() < 2) return d;
    std::vector<std::pair<juce::String, std::optional<double>>> shifts; std::vector<int> idx;
    for (const auto& x : c) { shifts.push_back ({ x.name, onsetShiftDb (x.onset1) }); idx.push_back (x.index); }
    const auto v = onsetRule (shifts, idx);
    if (v.pick >= 0) { Decision o; o.decided = true; o.rule = v.multiStage ? "onset_stage" : "onset"; o.pick = v.pick; o.pickName = v.pickName; o.note = v.text; o.sections = v.sections; o.pairWith = v.pairWith; o.pairWithName = v.pairWithName; for (const auto& x : c) if (x.index != v.pick) o.others.add (x.name); return o; }
    d.whyNot = v.text + (d.whyNot.isNotEmpty() ? "; " + d.whyNot : juce::String());
    return d;
}
inline Decision decideNamed (const std::vector<CandidateFacts>& c)
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
            // a pair RE-SWEEP (the twin written with the first at every position): the channels now agree because of that write; the
            // decision stays dual_mono_pair, so the export and the tone check keep writing both
            if (first.pairWritten == twin.index)
            {
                if (! twin.certified) { d.whyNot = "dual-mono pair '" + base + "': re-swept with '" + twin.name + "' written with '" + first.name + "', but the twin's own sweep did not certify"; return d; }
                d.decided = true; d.rule = "dual_mono_pair"; d.pick = first.index; d.pickName = first.name; d.others.add (twin.name); d.pairWith = twin.index; d.pairWithName = twin.name;
                d.note = "dual-mono pair '" + base + "' (measured): re-swept with '" + twin.name + "' written with '" + first.name + "' at every position; both channels measured" + (first.channelWorstDb ? " and within " + dbs (*first.channelWorstDb) + " dB of each other over " + juce::String (first.channelReadings) + " readings" : juce::String()) + "; the record is gated on the worse channel";
                return d;
            }
            if (! channelsAgree (first, why))
            {
                // item dual_mono_pair (Kathy's ruling 3, 6 Oct - dbx-160 (s), Mixland Vac Attack, Maag MAGNUM-K): a channel pair whose
                // first candidate moves only its own channel - both certify -> both thresholds are written together, both channels
                // measured, the record gated on the worse channel. The pick is the first; the twin follows it at every write.
                if (twin.certified && first.channelWorstDb)
                {
                    d.decided = true; d.rule = "dual_mono_pair"; d.pick = first.index; d.pickName = first.name; d.others.add (twin.name); d.pairWith = twin.index; d.pairWithName = twin.name;
                    d.note = "dual-mono pair '" + base + "' (measured): both certified, and in the first's own sweep the output channels differ by up to " + dbs (*first.channelWorstDb) + " dB over " + juce::String (first.channelReadings)
                           + " readings (bar " + dbs (kChannelAgreeDb) + ") - each threshold moves its own channel. '" + first.name + "' is the amount and '" + twin.name + "' is written WITH it at every position; both channels are measured and the record is gated on the worse";
                    return d;
                }
                d.whyNot = "channel pair '" + base + "': " + why + " - the channels are independent, and the twin did not certify: no pair rule"; return d;
            }
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
