/*
  EjmapSidechainCheck.h

  THE EVIDENCE-BASED RE-SWEEP FOR THE SIDECHAIN POLICY CHANGE (ruled 4 Oct 2026). From 28 Sep to 4 Oct the probe
  CONNECTED every input element past the main one and fed it silence ("enabled_silent"); WaveShell keys from a connected
  sidechain, so five Waves compressors read 0 dB GR and were filed `flat`. The probe now leaves those elements UNCONNECTED,
  as Logic does with no sidechain source. Nobody assumes which records that changes: for every product whose record was
  swept under "enabled_silent" AND declares a second input bus, the follow-up takes ONE reading under the new policy at
  one swept position and one loud level - the very process the record's own trace ran, with the same writes - and compares
  it with the trace's reading there. Within kSameDb -> keep the record ("sidechain policy: no effect"); otherwise -> re-sweep
  ("keys from a connected sidechain"). A product with no second bus, or swept under the new policy already, is not in the
  set; one with no trace to compare against is "unknown" and said so, never guessed.

  Everything here is pure (text in, decision out) so the suite pins it on the 4 Oct measurements; EjmapCertDriver.h runs the
  one process and writes the result on the record (`sidechainPolicyCheck`).
*/

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

namespace ejmap::sidechaincheck
{

inline constexpr double kSameDb = 0.1;               // the ruling's bar: within 0.1 dB the policy had no effect
inline constexpr double kLoudestLevelDb = -6.0;     // the loud level: the loudest swept level at or below this (peak dBFS)
inline constexpr const char* kPolicyNow = "unconnected";
inline constexpr const char* kPolicyOld = "enabled_silent";

// One position process as the probe printed it: the lines the check needs.
struct Trace
{
    bool ok = false;                                  // a sweep process with one position and at least one hold
    juce::String policy;                              // the policy line's third field
    int extraInputBuses = 0;                          // "bus render in <b>" lines with b >= 1
    juce::StringArray extraBusNames;
    int thr = -1;
    double holdS = 0.0, discardS = 0.0, winS = 0.0, movingDb = 0.1; bool reset = false;
    std::vector<std::pair<int, double>> sets;         // every "set" line: index, norm asked
    double norm = 0.0;
    std::map<double, double> holdDb;                  // level (peak dBFS) -> output level_db
};

inline Trace parseTrace (const juce::String& text)
{
    Trace t;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    bool sawPos = false;
    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 2) continue;
        if (f[0] == "policy" && f.size() > 2 && f[1] == "sidechain") t.policy = f[2];
        else if (f[0] == "bus" && f.size() > 4 && f[1] == "render" && f[2] == "in" && f[3].getIntValue() > 0) { ++t.extraInputBuses; t.extraBusNames.add (f[4]); }
        else if (f[0] == "sweep") t.thr = kv (f, 1, "thr").getIntValue();
        else if (f[0] == "spec") { t.holdS = kv (f, 1, "hold_s").getDoubleValue(); t.discardS = kv (f, 1, "discard_s").getDoubleValue(); t.winS = kv (f, 1, "win_s").getDoubleValue();
                                   t.movingDb = kv (f, 1, "moving_db").getDoubleValue(); t.reset = kv (f, 1, "reset_per_hold") == "1"; }
        else if (f[0] == "set" && f.size() > 2) t.sets.push_back ({ f[1].getIntValue(), f[2].getDoubleValue() });
        else if (f[0] == "pos" && f.size() > 3) { t.norm = kv (f, 1, "norm").getDoubleValue(); sawPos = true; }
        else if ((f[0] == "hold" || f[0] == "rerender") && f.size() > 4) t.holdDb[f[2].getDoubleValue()] = kv (f, 3, "level_db").getDoubleValue();
    }
    t.ok = t.thr >= 0 && sawPos && ! t.holdDb.empty();
    return t;
}

// The writes a record was swept with: preconditions + the engage writes (index, norm).
inline std::vector<std::pair<int, double>> recordWrites (const juce::var& sweepView)
{
    std::vector<std::pair<int, double>> w;
    if (const auto* pre = sweepView.getProperty ("preconditions", {}).getArray()) for (const auto& p : *pre) w.push_back ({ (int) p.getProperty ("index", -1), (double) p.getProperty ("norm", 0.0) });
    if (const auto* ew = sweepView.getProperty ("engageWrites", {}).getProperty ("writes", {}).getArray()) for (const auto& p : *ew) w.push_back ({ (int) p.getProperty ("index", -1), (double) p.getProperty ("norm", 0.0) });
    return w;
}

inline bool sameWrites (std::vector<std::pair<int, double>> a, std::vector<std::pair<int, double>> b)
{
    auto norm = [] (std::vector<std::pair<int, double>>& v) { std::sort (v.begin(), v.end()); };
    norm (a); norm (b);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].first != b[i].first || std::abs (a[i].second - b[i].second) > 0.0005) return false;
    return true;
}

// THE ONE READING TO TAKE: among the record's own position traces (those swept under the old policy, declaring a second
// bus, with the record's writes, with a hold at the loud level), the loud level is the loudest at or below -6 dBFS, and
// the position is the one that read LOWEST there (the most reduction: where a change of key shows most; ties -> first).
struct Pick { bool ok = false; juce::String why; size_t trace = 0; double level = 0.0, beforeDb = 0.0; };
inline Pick pickReading (const std::vector<Trace>& traces, const std::vector<std::pair<int, double>>& writes)
{
    Pick p; int candidates = 0, oldPolicy = 0, withBus = 0;
    for (size_t i = 0; i < traces.size(); ++i)
    {
        const auto& t = traces[i];
        if (! t.ok) continue;
        ++candidates;
        if (t.policy != kPolicyOld) continue; ++oldPolicy;
        if (t.extraInputBuses == 0) continue; ++withBus;
        if (! sameWrites (t.sets, writes)) continue;
        double L = -999.0; for (const auto& [lv, db] : t.holdDb) if (lv <= kLoudestLevelDb && lv > L) L = lv; if (L <= -999.0) continue;
        const double db = t.holdDb.at (L);
        if (! p.ok || L > p.level || (L == p.level && db < p.beforeDb)) { p.ok = true; p.trace = i; p.level = L; p.beforeDb = db; }
    }
    if (! p.ok) p.why = candidates == 0 ? "no position trace to compare against" : oldPolicy == 0 ? "not swept under " + juce::String (kPolicyOld) : withBus == 0 ? "no input bus past the main one" : "no trace ran with the record's writes at a level at or below -6 dBFS";
    return p;
}

// The probe arguments that repeat that process at that one level, under whatever policy the probe now has.
inline juce::StringArray argsFor (const Trace& t, double level)
{
    juce::StringArray a { "--sweep", "thr=" + juce::String (t.thr), "norms=" + juce::String (t.norm, 6), "levels=" + juce::String ((int) level), "hz=997",
                          "hold=" + juce::String (t.holdS, 2), "discard=" + juce::String (t.discardS, 2), "win=" + juce::String (t.winS, 2), "ref=0",
                          "moving_db=" + juce::String (t.movingDb, 2), juce::String ("reset=") + (t.reset ? "1" : "0") };
    juce::StringArray sets; for (const auto& [i, n] : t.sets) sets.add (juce::String (i) + ":" + juce::String (n, 6));
    if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (","));
    return a;
}

struct Verdict { bool resweep = false; juce::String verdict, why; double deltaDb = 0.0; };
inline Verdict verdict (double beforeDb, double afterDb, double norm, double level)
{
    Verdict v; v.deltaDb = std::round ((afterDb - beforeDb) * 1e4) / 1e4;   // at the probe's printed resolution (4 decimals), so 0.1000000000000014 is 0.1
    const auto at = " (norm " + juce::String (norm, 3) + " at " + juce::String (level, 0) + " dBFS: " + juce::String (beforeDb, 2) + " -> " + juce::String (afterDb, 2) + " dB)";
    if (std::abs (v.deltaDb) <= kSameDb) { v.verdict = "keep"; v.why = "sidechain policy: no effect" + at; }
    else { v.resweep = true; v.verdict = "resweep"; v.why = "keys from a connected sidechain" + at; }
    return v;
}

} // namespace ejmap::sidechaincheck
