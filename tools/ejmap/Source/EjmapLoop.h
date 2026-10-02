/*
  EjmapLoop.h - ONE LOOP DOES EVERYTHING (ruled 2 Oct 2026, docs/STRANGER_MAC_TEST.md).

  The certification batch (--cert-sweep-all) must leave every discovered compressor and tuner in EXACTLY ONE named
  state, with no silent drops and no hand steps, on a Mac nobody here controls. The states:

    exported      an ej_comp_profile/1 file was written, with its tone check embedded
    recorded      a tuner's pitch record was written (the tuner's pass state)
    refused       the measurement stopped at a named stage with a reason (the record says which)
    held          not measured on purpose: licence (PACE without the iLok) or hardware, which one named
    needs_review  measured, but a rule is missing or the result is not profile-grade: the reason names it
                  (several threshold candidates - the channel-strip rule is not built; a flat / unreadable sweep;
                  an export the exporter refused; a tone check that could not run; a certification-grade sweep
                  where a profile was wanted)

  The rules here are PURE (record in, outcome out) so the suite can pin every branch; the driver's loop
  (EjmapCertDriver.h runSweepAll) measures, exports, tone-checks and writes <out>/outcomes.json through them.
  "A product that finishes the loop has an export or a named reason for not having one" is the invariant:
  every outcome whose state is not exported/recorded carries a non-empty reason, and an exported one names its file.
*/
#pragma once
#include <juce_core/juce_core.h>

namespace ejmap::loop
{
inline const char* const kStates[] = { "exported", "recorded", "refused", "held", "needs_review" };
inline bool isState (const juce::String& s) { for (auto* k : kStates) if (s == k) return true; return false; }

struct Outcome
{
    juce::String state, reason;
    bool exportPending = false;        // the record is profile-grade and certified: the export + tone check decide
};

// A profile-grade sweep: the 31-level grid with the 2.5 s hold (Plan::makeProfile), not the 3-level certification sweep.
inline bool profileGrade (const juce::var& sweepVar)
{
    const auto lv = sweepVar.getProperty ("tone", {}).getProperty ("levels_dbfs", {});
    return lv.size() > 3 && (double) sweepVar.getProperty ("hold_s", 0.0) >= 2.0;
}

// FROM THE RECORD ALONE. Tuners: a record with pitchCandidates is recorded. Compressors: a refusal is refused at its
// stage; several candidates are needs_review (the channel-strip rule is not built); a certified profile-grade sweep is
// export-pending; anything else measured is needs_review with the sweep's own result and reason.
inline Outcome outcomeForRecord (const juce::var& record)
{
    Outcome o;
    if (const auto ref = record.getProperty ("thresholdRefusal", {}); ref.isObject())
    {
        o.state = "refused";
        o.reason = "stage " + ref.getProperty ("stage", "?").toString() + ": " + ref.getProperty ("reason", "").toString();
        return o;
    }
    if (record.getProperty ("schema", "").toString() == "ej_cert_tuner/1" || record.getProperty ("pitchCandidates", {}).isArray())
    {
        const auto pc = record.getProperty ("pitchCandidates", {});
        if (pc.isArray() && pc.size() > 0) { o.state = "recorded"; o.reason = juce::String (pc.size()) + " pitch candidate(s) recorded"; return o; }
        o.state = "needs_review"; o.reason = "tuner record with no pitch candidates"; return o;
    }
    if (const auto cands = record.getProperty ("thresholdCandidates", {}); cands.isArray())
    {
        o.state = "needs_review";
        o.reason = juce::String (cands.size()) + " threshold candidates (a channel strip or multiband): the one-candidate rule is not built, nobody picks";
        return o;
    }
    const auto sw = record.getProperty ("thresholdSweep", {});
    if (! sw.isObject()) { o.state = "needs_review"; o.reason = "record carries no sweep, no candidates and no refusal"; return o; }
    const auto result = sw.getProperty ("result", "").toString();
    if (result != "certified")
    {
        o.state = "needs_review";
        o.reason = "sweep result " + result + (sw.getProperty ("reason", "").toString().isNotEmpty() ? ": " + sw.getProperty ("reason", "").toString() : juce::String());
        return o;
    }
    if (! profileGrade (sw)) { o.state = "needs_review"; o.reason = "certified on a certification-grade sweep (3 levels); a profile sweep is needed for an export"; return o; }
    o.exportPending = true; o.state = "needs_review"; o.reason = "export pending";
    return o;
}

// AFTER THE EXPORT AND THE TONE CHECK. exported only when the file was written AND the tone check ran (pass or fail is
// a result, recorded on the profile); otherwise needs_review with the exporter's or the tone check's refusal.
inline Outcome outcomeAfterExport (bool exportOk, const juce::String& exportWhy, bool toneRan, const juce::String& toneWhy)
{
    Outcome o;
    if (! exportOk) { o.state = "needs_review"; o.reason = "export refused: " + exportWhy; return o; }
    if (! toneRan)  { o.state = "needs_review"; o.reason = "exported, but the tone check could not run: " + toneWhy; return o; }
    o.state = "exported"; o.reason = "profile exported with its tone check";
    return o;
}

// HELD: the subject was never measured, on purpose, and the row says which purpose.
inline Outcome outcomeHeld (bool licenceBound, bool hardware, const juce::String& detail)
{
    Outcome o; o.state = "held";
    o.reason = hardware ? "hardware: " + detail : licenceBound ? "licence: PACE-wrapped and the batch runs without the iLok (" + detail + ")" : "not measurable: " + detail;
    return o;
}

// THE ROW, and the invariant every row must satisfy.
inline juce::var makeRow (const juce::String& identity, const juce::String& product, const juce::String& category, const Outcome& o,
                          const juce::String& recordFile, const juce::String& profileFile, const juce::String& toneFile, const juce::String& when)
{
    auto* r = new juce::DynamicObject();
    r->setProperty ("identity", identity); r->setProperty ("product", product); r->setProperty ("category", category);
    r->setProperty ("state", o.state); r->setProperty ("reason", o.reason);
    if (recordFile.isNotEmpty())  r->setProperty ("record", recordFile);
    if (profileFile.isNotEmpty()) r->setProperty ("profile", profileFile);
    if (toneFile.isNotEmpty())    r->setProperty ("tonecheck", toneFile);
    r->setProperty ("at", when);
    return juce::var (r);
}

inline juce::String rowViolation (const juce::var& row)
{
    const auto state = row.getProperty ("state", "").toString();
    if (! isState (state)) return "state '" + state + "' is not one of the five";
    if (row.getProperty ("reason", "").toString().isEmpty()) return "no reason";
    if (state == "exported" && row.getProperty ("profile", "").toString().isEmpty()) return "exported without a profile file";
    if (state == "exported" && row.getProperty ("tonecheck", "").toString().isEmpty()) return "exported without a tone check";
    if (row.getProperty ("identity", "").toString().isEmpty()) return "no identity";
    return {};
}

// outcomes.json: one row per identity, the latest replacing the earlier (a resumed batch rewrites what it finished).
inline juce::var mergeRow (const juce::var& existing, const juce::var& row)
{
    juce::Array<juce::var> out;
    bool replaced = false;
    if (const auto* a = existing.getArray())
        for (const auto& r : *a)
        {
            if (r.getProperty ("identity", "") == row.getProperty ("identity", "")) { out.add (row); replaced = true; }
            else out.add (r);
        }
    if (! replaced) out.add (row);
    return out;
}

inline juce::var findRow (const juce::var& outcomes, const juce::String& identity)
{
    if (const auto* a = outcomes.getArray()) for (const auto& r : *a) if (r.getProperty ("identity", "") == identity) return r;
    return {};
}

struct Counts { int exported = 0, recorded = 0, refused = 0, held = 0, needsReview = 0, rows = 0; };
inline Counts count (const juce::var& outcomes)
{
    Counts c;
    if (const auto* a = outcomes.getArray())
        for (const auto& r : *a)
        {
            ++c.rows;
            const auto s = r.getProperty ("state", "").toString();
            if (s == "exported") ++c.exported; else if (s == "recorded") ++c.recorded; else if (s == "refused") ++c.refused;
            else if (s == "held") ++c.held; else if (s == "needs_review") ++c.needsReview;
        }
    return c;
}
} // namespace ejmap::loop
