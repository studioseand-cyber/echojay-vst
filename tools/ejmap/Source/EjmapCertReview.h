/*
  EjmapCertReview.h - THE ZIP REVIEW (5 Oct 2026, overnight run 2, R1).

  Kathy gets a zipped-back follow-up folder from Sean's Mac in the morning and needs ONE plain report: what changed
  against his 4 Oct zip, whether the projected re-sweeps happened and how each ended, the sidechain readings, the
  review picks, the tone checks at every level, the deep points, the inert and licence rows, anything that crashed,
  and whether the zip is what the runbook asked for (cert/ only, no config.json, traces present).

  Everything in this header is PURE: it reads parsed JSON and entry lists and renders text. Nothing here loads a
  plugin, unzips, or writes. The driver (EjmapCertDriver.h runReviewZip) unzips to scratch, runs the follow-up's own
  decision pass in derive-only mode over a COPY of the baseline to get the projection (so the projection is the
  follow-up's code, not a second implementation of it), then hands the pieces here.

  "Happened" is decided from the records' own measurement stamps (the sweep's measuredAt, a candidate's, a refusal's
  recordedAt, a tuner candidate's measuredAt): a follow-up record whose latest stamp is newer than the baseline's was
  re-measured. The row in outcomes.json says how it ended. A projected re-sweep with no newer stamp is "not run" and
  the row says what state it was left in - never a guess at why.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapLoop.h"
#include "EjmapSweep.h"
#include <map>
#include <set>
#include <vector>

namespace ejmap::review
{

//==============================================================================
// THE RECORD'S LATEST MEASUREMENT STAMP: the newest of every measurement the record carries (stamps are
// "20261003T194109.596+0100", which order lexically within one Mac's clock). Empty when nothing was measured.
inline juce::String recordStamp (const juce::var& r)
{
    juce::String best;
    auto take = [&] (const juce::var& v) { const auto s = v.toString(); if (s.isNotEmpty() && s > best) best = s; };
    take (r.getProperty ("thresholdSweep", {}).getProperty ("measuredAt", {}));
    take (r.getProperty ("thresholdRefusal", {}).getProperty ("recordedAt", {}));
    if (const auto* cs = r.getProperty ("thresholdCandidates", {}).getArray()) for (const auto& c : *cs) take (c.getProperty ("thresholdSweep", {}).getProperty ("measuredAt", {}));
    if (const auto* ps = r.getProperty ("pitchCandidates", {}).getArray()) for (const auto& p : *ps) take (p.getProperty ("measuredAt", {}));
    return best;
}

//==============================================================================
// HYGIENE: the runbook asks for `cd ~/Library/ejmap && zip -rq ... cert` - cert/ only, never config.json; and the
// follow-up re-derives from traces, so every record's process list and raw captures must be inside.
struct Hygiene
{
    int entries = 0, records = 0, processLists = 0, rawFiles = 0;
    juce::StringArray outsideCert, configJson, recordsWithoutTraces;
    bool certOnly() const { return outsideCert.isEmpty(); }
    bool ok() const { return certOnly() && configJson.isEmpty() && processLists > 0 && rawFiles > 0 && recordsWithoutTraces.isEmpty(); }
};
// entries: every path in the zip (or every file's path relative to the folder's parent, for a folder named cert).
inline Hygiene hygieneOf (const juce::StringArray& entries)
{
    Hygiene h;
    std::set<juce::String> stems, traced;
    for (auto e : entries)
    {
        e = e.replaceCharacter ('\\', '/');
        if (e.isEmpty()) continue;
        ++h.entries;
        if (! (e == "cert" || e == "cert/" || e.startsWith ("cert/"))) h.outsideCert.add (e);
        const auto name = e.fromLastOccurrenceOf ("/", false, false);
        if (name == "config.json") h.configJson.add (e);
        if (e.startsWith ("cert/raw/") && name.isNotEmpty()) ++h.rawFiles;
        if (e.startsWith ("cert/fixtures/") && name.endsWith (".json") && ! name.endsWith (".defaults.json")) { ++h.records; stems.insert (name.dropLastCharacters (5)); }
        if (name.endsWith (".sweep.processes.json") || name.endsWith (".tuner.processes.json"))
        { ++h.processLists; traced.insert (name.upToFirstOccurrenceOf (".sweep.processes.json", false, false).upToFirstOccurrenceOf (".tuner.processes.json", false, false)); }
    }
    for (const auto& s : stems) if (! traced.count (s)) h.recordsWithoutTraces.add (s);
    return h;
}

//==============================================================================
// OUTCOMES: counts per state before and after, every row whose state changed (by product), rows new, rows gone.
struct StateChange { juce::String product, before, after, reason; };
struct StateDiff
{
    std::map<juce::String, int> before, after;
    std::vector<StateChange> changes;          // same identity, different state
    std::vector<StateChange> reasonOnly;       // same state, different reason (said, not counted as a change)
    juce::StringArray added, gone;             // identities present on one side only (product names)
};
inline juce::String stateOrder (int i) { static const char* k[] = { "exported", "recorded", "refused", "held", "needs_review", "needs_licence", "multiband", "surround", "quarantined_at_scan" }; return i < 9 ? k[i] : ""; }
inline StateDiff diffOutcomes (const juce::var& baseline, const juce::var& follow)
{
    StateDiff d;
    std::map<juce::String, juce::var> b, f;
    if (const auto* a = baseline.getArray()) for (const auto& r : *a) { b[r.getProperty ("identity", "").toString()] = r; ++d.before[r.getProperty ("state", "").toString()]; }
    if (const auto* a = follow.getArray())   for (const auto& r : *a) { f[r.getProperty ("identity", "").toString()] = r; ++d.after[r.getProperty ("state", "").toString()]; }
    for (const auto& [id, r] : f)
    {
        const auto it = b.find (id);
        if (it == b.end()) { d.added.add (r.getProperty ("product", "").toString()); continue; }
        const auto s0 = it->second.getProperty ("state", "").toString(), s1 = r.getProperty ("state", "").toString();
        const auto why0 = it->second.getProperty ("reason", "").toString(), why1 = r.getProperty ("reason", "").toString();
        if (s0 != s1) d.changes.push_back ({ r.getProperty ("product", "").toString(), s0, s1, why1 });
        else if (why0 != why1) d.reasonOnly.push_back ({ r.getProperty ("product", "").toString(), s0, s1, why1 });
    }
    for (const auto& [id, r] : b) if (! f.count (id)) d.gone.add (r.getProperty ("product", "").toString());
    auto byName = [] (const StateChange& x, const StateChange& y) { return x.product.compareIgnoreCase (y.product) < 0; };
    std::sort (d.changes.begin(), d.changes.end(), byName); std::sort (d.reasonOnly.begin(), d.reasonOnly.end(), byName);
    return d;
}

//==============================================================================
// A PROJECTED RE-SWEEP against what the follow-up folder holds.
struct ResweepLine { juce::String product, why; bool happened = false, afterRederive = false; juce::String ended; };
inline ResweepLine resweepStatus (const juce::String& product, const juce::String& why, bool afterRederive,
                                  const juce::var& baselineRecord, const juce::var& followRecord, const juce::var& followRow)
{
    ResweepLine l; l.product = product; l.why = why; l.afterRederive = afterRederive;
    const auto s0 = recordStamp (baselineRecord), s1 = recordStamp (followRecord);
    l.happened = followRecord.isObject() && s1.isNotEmpty() && s1 > s0;
    if (! followRecord.isObject()) l.ended = "no record in the follow-up folder";
    else if (followRow.isObject()) l.ended = followRow.getProperty ("state", "").toString() + ": " + followRow.getProperty ("reason", "").toString();
    else l.ended = "no row in outcomes.json";
    return l;
}
// records re-measured in the follow-up that the projection did not name (the landing read can make a stepped re-sweep the
// projection cannot see without a load; a --retry-refused pass can too)
inline juce::StringArray unprojectedResweeps (const std::map<juce::String, juce::var>& baselineRecords, const std::map<juce::String, juce::var>& followRecords, const juce::StringArray& projected)
{
    juce::StringArray out;
    for (const auto& [product, r] : followRecords)
    {
        if (projected.contains (product)) continue;
        const auto it = baselineRecords.find (product);
        const auto s1 = recordStamp (r);
        if (it == baselineRecords.end()) { if (s1.isNotEmpty()) out.add (product + " (no baseline record)"); continue; }
        if (s1.isNotEmpty() && s1 > recordStamp (it->second)) out.add (product);
    }
    return out;
}

//==============================================================================
// THE SIDECHAIN READING on a follow-up record: one word for the verdict and the numbers.
struct SidechainLine { juce::String product, word, detail; };   // word: no effect | re-swept | window | crashed | not run | not read
// A re-swept record says how the re-sweep ended (the row), since the reading itself is then history.
inline SidechainLine sidechainStatus (const juce::String& product, const juce::var& followRecord, const juce::var& followRow = {})
{
    SidechainLine l; l.product = product;
    const auto sc = followRecord.getProperty ("sidechainPolicyCheck", {});
    if (! sc.isObject()) { l.word = "not read"; l.detail = followRecord.isObject() ? "no sidechainPolicyCheck on the record" : "no record in the follow-up folder"; return l; }
    const auto v = sc.getProperty ("verdict", "").toString();
    l.word = v == "keep" ? "no effect" : v == "resweep" ? "re-swept" : v == "window" ? "window" : v == "crashed" ? "crashed" : v == "not_run" ? "not run" : v;
    if (sc.hasProperty ("after_db"))
        l.detail = "norm " + juce::String ((double) sc.getProperty ("norm", 0.0), 3) + " at " + juce::String ((double) sc.getProperty ("level_dbfs", 0.0), 0) + " dBFS: "
                 + juce::String ((double) sc.getProperty ("before_db", 0.0), 2) + " -> " + juce::String ((double) sc.getProperty ("after_db", 0.0), 2) + " dB (" + sc.getProperty ("extraInputBuses", "").toString() + ")";
    else l.detail = sc.getProperty ("why", "").toString();
    if (v == "resweep")
    {
        const auto readAt = sc.getProperty ("readAt", "").toString(); const auto stamp = recordStamp (followRecord);
        l.detail << " | re-sweep " << (stamp > readAt ? "ran" : "NOT run (no measurement after the reading)") << (followRow.isObject() ? ", ended " + followRow.getProperty ("state", "").toString() + ": " + followRow.getProperty ("reason", "").toString() : juce::String());
    }
    return l;
}

//==============================================================================
// A REVIEW PICK: the entry, and whether the follow-up record carries it as its decision.
struct PickLine { juce::String product, candidate, by, result; };
inline PickLine pickStatus (const juce::var& pick, const juce::var& followRecord, const juce::var& followRow)
{
    PickLine l; l.product = pick.getProperty ("product", "").toString(); l.candidate = pick.getProperty ("candidate", "").toString(); l.by = pick.getProperty ("by", "").toString();
    if (! followRecord.isObject()) { l.result = "no record in the follow-up folder"; return l; }
    const auto rd = followRecord.getProperty ("ruleDecided", {}); const auto pc = followRecord.getProperty ("pickedCandidate", {});
    const auto rowState = followRow.isObject() ? followRow.getProperty ("state", "").toString() + ": " + followRow.getProperty ("reason", "").toString() : juce::String ("no row");
    if (rd.isObject() && rd.getProperty ("rule", "").toString() == "review_pick" && pc.getProperty ("name", "").toString() == l.candidate)
        l.result = "applied -> " + rowState;
    else if (pc.isObject())
        l.result = "NOT applied: the record was decided by " + (rd.getProperty ("rule", "").toString().isNotEmpty() ? rd.getProperty ("rule", "").toString() : juce::String ("another pick")) + " ('" + pc.getProperty ("name", "").toString() + "') -> " + rowState;
    else
    {
        // the follow-up's own refusal text, re-derived from the record (applyReviewPick is pure on a copy)
        auto copy = followRecord; juce::Array<juce::var> one; one.add (pick);
        const auto what = loop::applyReviewPick (copy, juce::var (one));
        l.result = "NOT applied: " + (what.isNotEmpty() ? what : juce::String ("the record carries no candidates")) + " -> " + rowState;
    }
    return l;
}

//==============================================================================
// TONE CHECKS: g = 2 and every deep level the profile carried, one letter each; totals per level across products.
struct ToneLevel { int g = 0; juce::String result; double gr = 0.0; juce::String note; };   // result: PASS | FAIL | nulled | not run
struct ToneSummary { juce::String product; bool present = false; std::vector<ToneLevel> levels; };
inline ToneSummary toneSummary (const juce::String& product, const juce::var& tc)
{
    ToneSummary s; s.product = product;
    if (! tc.isObject()) return s;
    s.present = true;
    {
        ToneLevel l; l.g = (int) std::lround ((double) tc.getProperty ("g_db", 2.0)); l.gr = (double) tc.getProperty ("gr_measured_db", 0.0);
        l.result = (bool) tc.getProperty ("pass_within_0_5_db", false) ? "PASS" : "FAIL"; s.levels.push_back (l);
    }
    if (const auto* dl = tc.getProperty ("deep_levels", {}).getArray())
        for (const auto& d : *dl)
        {
            ToneLevel l; l.g = (int) std::lround ((double) d.getProperty ("g_db", 0.0)); l.gr = (double) d.getProperty ("gr_measured_db", 0.0);
            const auto nr = d.getProperty ("null_reason", {});
            if (! (bool) d.getProperty ("ran", false)) { l.result = "not run"; l.note = nr.isString() ? nr.toString() : juce::String(); }
            else if (nr.isString() && nr.toString().isNotEmpty()) { l.result = "nulled"; l.note = nr.toString(); }
            else l.result = (bool) d.getProperty ("pass_within_0_5_db", false) ? "PASS" : "FAIL";
            s.levels.push_back (l);
        }
    return s;
}
struct ToneTotals { std::map<int, std::map<juce::String, int>> byLevel; int products = 0, allPass = 0; };
inline ToneTotals toneTotals (const std::vector<ToneSummary>& all)
{
    ToneTotals t;
    for (const auto& s : all)
    {
        if (! s.present) continue;
        ++t.products; bool pass = true;
        for (const auto& l : s.levels) { ++t.byLevel[l.g][l.result]; if (l.result != "PASS") pass = false; }
        if (pass) ++t.allPass;
    }
    return t;
}
inline juce::String toneLine (const ToneSummary& s)
{
    juce::StringArray parts;
    for (const auto& l : s.levels)
        parts.add (juce::String (l.g) + ":" + (l.result == "PASS" ? "P" : l.result == "FAIL" ? "F(" + juce::String (l.gr, 2) + ")" : l.result == "nulled" ? "N" : "-"));
    juce::StringArray notes; for (const auto& l : s.levels) if (l.note.isNotEmpty()) notes.add (juce::String (l.g) + ": " + l.note);
    return parts.joinIntoString (" ") + (notes.isEmpty() ? juce::String() : "  [" + notes.joinIntoString ("; ") + "]");
}

//==============================================================================
// DEEP POINTS on an exported profile: how many of the deep targets (kDeepFrom..kGrTargetMax) are numbers across the positions.
struct DeepPoints { juce::String product; bool present = false; int positions = 0, deepPresent = 0, deepSlots = 0; juce::var dpe; };
inline DeepPoints deepPointsOf (const juce::String& product, const juce::var& profile)
{
    DeepPoints d; d.product = product;
    const auto* curve = profile.getProperty ("amount", {}).getProperty ("curve", {}).getArray();
    if (curve == nullptr) return d;
    d.present = true; d.positions = curve->size();
    for (const auto& p : *curve)
        for (int t : sweep::kGrTargets)
        {
            if (t < sweep::kDeepFrom) continue;
            ++d.deepSlots;
            const auto v = p.getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (t), {});
            if (v.isDouble() || v.isInt()) ++d.deepPresent;
        }
    d.dpe = profile.getProperty ("quality", {}).getProperty ("deep_point_error_db", {});
    return d;
}

//==============================================================================
// INERT: a record whose sweep (or any candidate's) says inert, or whose inert check ran and said otherwise.
struct InertLine { juce::String product, word, reason; };   // word: inert | checked, not inert
inline std::vector<InertLine> inertLines (const std::map<juce::String, juce::var>& records)
{
    std::vector<InertLine> out;
    for (const auto& [product, r] : records)
    {
        auto look = [&] (const juce::var& sw)
        {
            if (! sw.isObject()) return;
            if (sw.getProperty ("result", "").toString() == "inert") out.push_back ({ product, "inert", sw.getProperty ("reason", "").toString() });
            else if (const auto ic = sw.getProperty ("inertCheck", {}); ic.isObject()) out.push_back ({ product, "checked, not inert", ic.getProperty ("reason", "").toString() });
        };
        look (r.getProperty ("thresholdSweep", {}));
        if (const auto* cs = r.getProperty ("thresholdCandidates", {}).getArray()) for (const auto& c : *cs) look (c.getProperty ("thresholdSweep", {}));
    }
    return out;
}

//==============================================================================
// CRASHES: every run.jsonl attempt that did not exit 0 (a signal, a timeout, a window, a sleep), the rows whose reason
// says the probe crashed or showed a window, and a sidechain reading that ended that way.
struct CrashLine { juce::String where, what; };
inline juce::String productForStem (const std::map<juce::String, juce::var>& recordsByStem, const juce::String& file)
{
    for (const auto& [stem, r] : recordsByStem) if (file.startsWith (stem + ".")) return r.getProperty ("product", "").toString();
    return file.upToFirstOccurrenceOf (".", false, false);
}
inline std::vector<CrashLine> crashesIn (const juce::String& runJsonl, int skipLines, const juce::var& outcomes, const std::map<juce::String, juce::var>& recordsByStem)
{
    std::vector<CrashLine> out; int n = 0;
    for (const auto& line : juce::StringArray::fromLines (runJsonl))
    {
        if (line.trim().isEmpty()) continue;
        if (++n <= skipLines) continue;   // the baseline's own lines (its crashes were its run's business; this report is the follow-up's)
        const auto j = juce::JSON::parse (line);
        if (! j.isObject()) continue;
        const auto outcome = j.getProperty ("outcome", "").toString();
        if (outcome.startsWith ("exit 0") && (bool) j.getProperty ("clean", true)) continue;
        if (outcome.startsWith ("exit ") && (bool) j.getProperty ("clean", true)) continue;   // an answer code (3 = the plugin answered) is not a crash
        out.push_back ({ productForStem (recordsByStem, j.getProperty ("file", "").toString()) + " / " + j.getProperty ("tag", "").toString(), outcome });
    }
    if (const auto* a = outcomes.getArray())
        for (const auto& r : *a)
        {
            const auto why = r.getProperty ("reason", "").toString();
            if (why.containsIgnoreCase ("crash") || why.containsIgnoreCase ("killed by signal") || why.containsIgnoreCase ("timed out") || why.containsIgnoreCase ("SHOWED A WINDOW"))
                out.push_back ({ r.getProperty ("product", "").toString() + " (row)", why });
        }
    for (const auto& [stem, r] : recordsByStem)
    {
        const auto sc = r.getProperty ("sidechainPolicyCheck", {});
        const auto v = sc.getProperty ("verdict", "").toString();
        if (v == "crashed" || v == "window") out.push_back ({ r.getProperty ("product", "").toString() + " (sidechain reading)", sc.getProperty ("why", "").toString() });
    }
    return out;
}

//==============================================================================
// THE REPORT. Every section prints its count even when it is zero, so an empty section is a statement, not an omission.
// PHASE B BY CATEGORY, THE DRAFTS, THE TOP FINDINGS (8 Oct stretch S2): from cert/phaseb/<category>/ - the rows' outcomes, the records,
// the drafts and every acceptance row found anywhere inside a record or a draft (an object with "pass"), so a morning zip is checked in minutes
struct AcceptanceCount { int rows = 0, ran = 0, passed = 0; std::vector<juce::String> failed; };
inline void collectAcceptance (const juce::var& v, AcceptanceCount& c, const juce::String& where, int depth = 0)
{
    if (depth > 12) return;
    if (auto* o = v.getDynamicObject())
    {
        if (o->hasProperty ("pass") && (o->hasProperty ("ran") || o->hasProperty ("step") || o->hasProperty ("target_db") || o->hasProperty ("request_dbtp")))
        {
            ++c.rows; const bool ran = ! o->hasProperty ("ran") || (bool) o->getProperty ("ran");
            if (ran) { ++c.ran; if ((bool) o->getProperty ("pass")) ++c.passed; else if (c.failed.size() < 400) c.failed.push_back (where + ": " + o->getProperty ("step").toString() + o->getProperty ("why").toString().substring (0, 80)); }
        }
        for (const auto& kv : o->getProperties()) collectAcceptance (kv.value, c, where, depth + 1);
    }
    else if (const auto* a = v.getArray()) for (const auto& x : *a) collectAcceptance (x, c, where, depth + 1);
}
struct CategoryReview
{
    juce::String name; int rows = 0, records = 0, drafts = 0, draftsBadSpec = 0, draftsWithNotes = 0;
    std::map<juce::String, int> outcomes; AcceptanceCount acc; std::map<juce::String, int> noteKinds; juce::StringArray failedRows;
};
inline juce::String noteKind (const juce::String& note)
{   // the note's kind: its text up to the first colon, numbers and bracketed indices taken out ("Band 'Air': dynamic ..." -> "Band: dynamic")
    juce::String k = note.upToFirstOccurrenceOf (" - ", false, false).substring (0, 60);
    juce::String out; for (int i = 0; i < k.length(); ++i) { const auto ch = k[i]; if (juce::CharacterFunctions::isDigit (ch)) continue; out << ch; }
    return out.trim();
}
inline std::vector<CategoryReview> phaseBReview (const juce::File& phasebDir)
{
    std::vector<CategoryReview> out;
    for (const auto& d : phasebDir.findChildFiles (juce::File::findDirectories, false))
    {
        CategoryReview c; c.name = d.getFileName(); if (c.name.startsWith (".")) continue;
        for (const auto& f : d.findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
        { const auto r = juce::JSON::parse (f.loadFileAsString()); ++c.rows; const auto oc = r.getProperty ("outcome", "?").toString(); ++c.outcomes[oc]; if (oc == "failed" || oc == "timed_out" || oc == "window") c.failedRows.add (r.getProperty ("product", "").toString() + " (" + oc + (r.getProperty ("reason", "").toString().isNotEmpty() ? ": " + r.getProperty ("reason", "").toString().substring (0, 70) : juce::String()) + ")"); }
        for (const auto& sub : d.findChildFiles (juce::File::findDirectories, false))
        {
            const auto sn = sub.getFileName(); if (sn == "raw" || sn == "logs" || sn.startsWith (".")) continue;
            for (const auto& f : sub.findChildFiles (juce::File::findFiles, false, "*.json"))
            {
                const auto v = juce::JSON::parse (f.loadFileAsString()); const auto product = v.getProperty ("product", v.getProperty ("plugin", {}).getProperty ("name", v.getProperty ("parent", {}).getProperty ("name", ""))).toString();
                if (sn == "drafts")
                {
                    ++c.drafts; if (! (v.getProperty ("spec", "").toString().endsWith (" v0.1 PROPOSAL") || v.getProperty ("spec", "").toString().endsWith (" v0.2 PROPOSAL")) || ! f.getFileName().endsWith (".draft.json")) ++c.draftsBadSpec;
                    if (const auto* n = v.getProperty ("notes", {}).getArray(); n && ! n->isEmpty()) { ++c.draftsWithNotes; for (const auto& x : *n) ++c.noteKinds[noteKind (x.toString())]; }
                    collectAcceptance (v, c.acc, product);
                }
                else { ++c.records; }
            }
        }
        if (c.rows > 0 || c.drafts > 0 || c.records > 0) out.push_back (c);
    }
    std::sort (out.begin(), out.end(), [] (const CategoryReview& a, const CategoryReview& b) { return a.name < b.name; });
    return out;
}
inline juce::String renderPhaseB (const std::vector<CategoryReview>& cats)
{
    juce::String s; auto line = [&] (const juce::String& t = {}) { s << t << "\n"; }; auto head = [&] (const juce::String& t) { line(); line (t); line (juce::String::repeatedString ("-", t.length())); };
    head ("10. PHASE B BY CATEGORY (rows: done / failed / needs licence / needs device / other; records; acceptance passed of ran)");
    if (cats.empty()) line ("no cert/phaseb in this folder");
    int allRan = 0, allPass = 0;
    for (const auto& c : cats)
    {
        auto n = [&] (const char* k) { return c.outcomes.count (k) ? c.outcomes.at (k) : 0; };
        const int other = c.rows - n ("ok") - n ("failed") - n ("timed_out") - n ("needs_licence") - n ("needs_device");
        allRan += c.acc.ran; allPass += c.acc.passed;
        line ("  " + c.name.paddedRight (' ', 11) + juce::String (c.rows).paddedLeft (' ', 4) + " rows: " + juce::String (n ("ok")) + " ok / " + juce::String (n ("failed") + n ("timed_out")) + " failed / " + juce::String (n ("needs_licence")) + " licence / " + juce::String (n ("needs_device")) + " device / " + juce::String (other) + " other; "
              + juce::String (c.records) + " record(s); acceptance " + (c.acc.ran > 0 ? juce::String (c.acc.passed) + "/" + juce::String (c.acc.ran) + " (" + juce::String (100.0 * c.acc.passed / c.acc.ran, 0) + " %)" + (c.acc.rows > c.acc.ran ? ", " + juce::String (c.acc.rows - c.acc.ran) + " null" : juce::String()) : juce::String ("none in the drafts")));
    }
    if (allRan > 0) line ("  all categories: acceptance " + juce::String (allPass) + "/" + juce::String (allRan) + " (" + juce::String (100.0 * allPass / allRan, 1) + " %)");
    head ("11. DRAFTS (cert/phaseb/<category>/drafts/; never cert/profiles)");
    int total = 0, bad = 0; for (const auto& c : cats) { total += c.drafts; bad += c.draftsBadSpec; if (c.drafts > 0) line ("  " + c.name.paddedRight (' ', 11) + juce::String (c.drafts).paddedLeft (' ', 4) + " draft(s), " + juce::String (c.draftsWithNotes) + " with notes" + (c.draftsBadSpec > 0 ? ", " + juce::String (c.draftsBadSpec) + " WITHOUT the 'v0.1 PROPOSAL' spec tag or the .draft.json name" : juce::String())); }
    line ("  " + juce::String (total) + " draft(s)" + (bad > 0 ? ", " + juce::String (bad) + " break the drafts rule" : ", all tagged v0.1 PROPOSAL"));
    head ("12. TOP FINDINGS");
    int shown = 0;
    for (const auto& c : cats) if (! c.failedRows.isEmpty()) { line ("  " + c.name + ": " + juce::String (c.failedRows.size()) + " failed row(s): " + c.failedRows.joinIntoString ("; ").substring (0, 300)); ++shown; }
    for (const auto& c : cats) if (! c.acc.failed.empty()) { juce::StringArray f; for (size_t i = 0; i < c.acc.failed.size() && i < 4; ++i) f.add (c.acc.failed[i]); line ("  " + c.name + ": " + juce::String ((int) c.acc.failed.size()) + " acceptance FAIL(s), e.g. " + f.joinIntoString (" | ")); ++shown; }
    for (const auto& c : cats)
    {
        std::vector<std::pair<int, juce::String>> k; for (const auto& [t, n] : c.noteKinds) k.push_back ({ n, t }); std::sort (k.rbegin(), k.rend());
        juce::StringArray top; for (size_t i = 0; i < k.size() && i < 3; ++i) top.add (juce::String (k[i].first) + "x " + k[i].second);
        if (! top.isEmpty()) { line ("  " + c.name + " notes: " + top.joinIntoString ("; ")); ++shown; }
    }
    if (shown == 0) line ("  nothing failed; no acceptance FAIL; no draft notes");
    return s;
}

struct ReportInput
{
    juce::String subjectName, baselineName, scratchDir, note;
    Hygiene hygiene; bool hygieneKnown = false;
    StateDiff states; bool baselineKnown = false;
    std::vector<ResweepLine> resweeps; juce::StringArray unprojected; bool projectionKnown = false;
    std::vector<SidechainLine> sidechain;
    std::vector<PickLine> picks; int pickEntries = 0; bool picksFilePresent = false;
    std::vector<ToneSummary> tones;
    std::vector<DeepPoints> deep;
    std::vector<InertLine> inert;
    std::vector<std::pair<juce::String, juce::String>> licence;   // product, reason - the SUBJECTS (a record, or category compressor / pitch)
    std::map<juce::String, int> licenceOther;                     // the scan's stops in other categories (not subjects): category -> count
    std::vector<CrashLine> crashes; int runLinesAdded = 0;
    std::map<juce::String, int> probesSeen;                       // cdhash prefix -> count, from the follow-up's tone checks
    bool logPresent = false; juce::String logLastLine;
    std::vector<CategoryReview> phaseb;                           // 8 Oct S2: every Phase B category and its drafts
};
inline juce::String render (const ReportInput& in)
{
    juce::String s;
    auto line = [&] (const juce::String& t = {}) { s << t << "\n"; };
    auto head = [&] (const juce::String& t) { line(); line (t); line (juce::String::repeatedString ("-", t.length())); };
    line ("ZIP REVIEW: " + in.subjectName + (in.baselineKnown ? "  against  " + in.baselineName : "  (no baseline: counts only)"));
    if (in.scratchDir.isNotEmpty()) line ("unzipped to " + in.scratchDir);
    if (in.note.isNotEmpty()) line (in.note);

    head ("1. HYGIENE");
    if (! in.hygieneKnown) line ("not checked");
    else
    {
        const auto& h = in.hygiene;
        line (juce::String (h.entries) + " entries; cert/ only: " + (h.certOnly() ? "yes" : "NO - " + juce::String (h.outsideCert.size()) + " outside cert/: " + h.outsideCert.joinIntoString (", ").substring (0, 300)));
        line ("config.json: " + (h.configJson.isEmpty() ? juce::String ("none (good)") : "PRESENT - " + h.configJson.joinIntoString (", ")));
        line ("traces: " + juce::String (h.processLists) + " process list(s), " + juce::String (h.rawFiles) + " raw capture(s), " + juce::String (h.records) + " record(s)"
              + (h.recordsWithoutTraces.isEmpty() ? juce::String (", every record has its process list") : "; " + juce::String (h.recordsWithoutTraces.size()) + " record(s) WITHOUT a process list: " + h.recordsWithoutTraces.joinIntoString (", ")));
        line ("tonecheck.log: " + (in.logPresent ? "present" + (in.logLastLine.isNotEmpty() ? ", last line: " + in.logLastLine : juce::String()) : juce::String ("absent (the runbook tees it into cert/)")));
        if (! in.probesSeen.empty()) { juce::StringArray p; for (const auto& [k, n] : in.probesSeen) p.add (k + " x" + juce::String (n)); line ("probes in the tone checks: " + p.joinIntoString (", ")); }
        line (h.ok() ? "hygiene: OK" : "hygiene: NOT OK (see above)");
    }

    head ("2. OUTCOMES" + juce::String (in.baselineKnown ? " (baseline -> follow-up)" : ""));
    {
        std::set<juce::String> keys; for (int i = 0; i < 9; ++i) keys.insert (stateOrder (i));
        for (const auto& [k, n] : in.states.before) keys.insert (k); for (const auto& [k, n] : in.states.after) keys.insert (k);
        int tb = 0, ta = 0;
        for (int i = 0; i < 9; ++i)
        {
            const auto k = stateOrder (i); const int b = in.states.before.count (k) ? in.states.before.at (k) : 0, a = in.states.after.count (k) ? in.states.after.at (k) : 0; tb += b; ta += a;
            if (b == 0 && a == 0) continue;
            line ("  " + k.paddedRight (' ', 20) + (in.baselineKnown ? juce::String (b).paddedLeft (' ', 4) + " -> " : juce::String()) + juce::String (a).paddedLeft (' ', 4) + (in.baselineKnown && a != b ? "  (" + juce::String (a - b > 0 ? "+" : "") + juce::String (a - b) + ")" : juce::String()));
        }
        for (const auto& k : keys) if (! k.isEmpty() && ! juce::StringArray { "exported", "recorded", "refused", "held", "needs_review", "needs_licence", "multiband", "surround", "quarantined_at_scan" }.contains (k))
            line ("  " + k.paddedRight (' ', 20) + "UNKNOWN STATE: before " + juce::String (in.states.before.count (k) ? in.states.before.at (k) : 0) + ", after " + juce::String (in.states.after.count (k) ? in.states.after.at (k) : 0));
        line ("  " + juce::String ("rows").paddedRight (' ', 20) + (in.baselineKnown ? juce::String (tb).paddedLeft (' ', 4) + " -> " : juce::String()) + juce::String (ta).paddedLeft (' ', 4));
        if (in.baselineKnown)
        {
            line ("state changes by product: " + juce::String ((int) in.states.changes.size()));
            for (const auto& c : in.states.changes) line ("  " + c.product + ": " + c.before + " -> " + c.after + " | " + c.reason);
            if (! in.states.added.isEmpty()) line ("rows only in the follow-up: " + in.states.added.joinIntoString (", "));
            if (! in.states.gone.isEmpty())  line ("rows only in the baseline (GONE): " + in.states.gone.joinIntoString (", "));
            line ("same state, reason changed: " + juce::String ((int) in.states.reasonOnly.size()));
            for (const auto& c : in.states.reasonOnly) line ("  " + c.product + " (" + c.after + "): " + c.reason);
        }
    }

    head ("3. PROJECTED RE-SWEEPS" + juce::String (in.projectionKnown ? " (the follow-up's own decision pass, derive-only, over the baseline)" : ""));
    if (! in.projectionKnown) line ("no baseline: nothing projected");
    else
    {
        int ran = 0; for (const auto& r : in.resweeps) if (r.happened) ++ran;
        line (juce::String ((int) in.resweeps.size()) + " projected, " + juce::String (ran) + " happened, " + juce::String ((int) in.resweeps.size() - ran) + " not run");
        for (const auto& r : in.resweeps) line ("  " + juce::String (r.happened ? "RAN     " : "NOT RUN ") + r.product + (r.afterRederive ? " (after the re-derive)" : "") + " | " + r.why + " | ended " + r.ended);
        line ("re-measured but not projected: " + juce::String (in.unprojected.size()) + (in.unprojected.isEmpty() ? juce::String() : " - " + in.unprojected.joinIntoString (", ")));
    }

    head ("4. SIDECHAIN READINGS");
    {
        std::map<juce::String, int> n; for (const auto& l : in.sidechain) ++n[l.word];
        juce::StringArray parts; for (const auto& [k, v] : n) parts.add (k + " " + juce::String (v));
        line (juce::String ((int) in.sidechain.size()) + " record(s) in the set" + (parts.isEmpty() ? juce::String() : ": " + parts.joinIntoString (", ")));
        for (const auto& l : in.sidechain) line ("  " + l.word.paddedRight (' ', 10) + l.product + " | " + l.detail);
    }

    head ("5. REVIEW PICKS");
    if (! in.picksFilePresent) line ("no review_picks.json in the follow-up folder");
    else
    {
        int applied = 0; for (const auto& p : in.picks) if (p.result.startsWith ("applied")) ++applied;
        line (juce::String (in.pickEntries) + " entr" + (in.pickEntries == 1 ? "y" : "ies") + ", " + juce::String (applied) + " applied");
        for (const auto& p : in.picks) line ("  " + p.product + " -> '" + p.candidate + "' by " + p.by + ": " + p.result);
    }

    head ("6. TONE CHECKS (g: P pass, F fail with the GR read, N nulled, - not run)");
    {
        const auto t = toneTotals (in.tones);
        line (juce::String (t.products) + " product(s) with a tone check, " + juce::String (t.allPass) + " pass at every level checked");
        for (const auto& [g, m] : t.byLevel)
        {
            juce::StringArray parts; for (const auto& [k, v] : m) parts.add (k + " " + juce::String (v));
            line ("  level " + juce::String (g).paddedLeft (' ', 2) + ": " + parts.joinIntoString (", "));
        }
        for (const auto& s : in.tones) if (s.present) line ("  " + s.product + ": " + toneLine (s));
    }

    head ("7. DEEP POINTS (levels " + juce::String (sweep::kDeepFrom) + ".." + juce::String (sweep::kGrTargetMax) + " present across the positions; deep_point_error_db)");
    {
        int n = 0; for (const auto& d : in.deep) if (d.present) ++n;
        line (juce::String (n) + " exported profile(s)");
        for (const auto& d : in.deep) if (d.present)
            line ("  " + d.product + ": " + juce::String (d.deepPresent) + " of " + juce::String (d.deepSlots) + " over " + juce::String (d.positions) + " positions; dpe " + ((d.dpe.isDouble() || d.dpe.isInt()) ? juce::String ((double) d.dpe, 2) : juce::String ("none")));
    }

    head ("8. INERT AND LICENCE");
    {
        int inert = 0; for (const auto& l : in.inert) if (l.word == "inert") ++inert;
        line ("inert: " + juce::String (inert) + ", checked and not inert: " + juce::String ((int) in.inert.size() - inert));
        for (const auto& l : in.inert) line ("  " + l.word.paddedRight (' ', 20) + l.product + " | " + l.reason);
        int other = 0; for (const auto& [k, n] : in.licenceOther) other += n;
        line ("needs_licence rows: " + juce::String ((int) in.licence.size() + other) + " - " + juce::String ((int) in.licence.size()) + " subject(s) (compressor / pitch, or with a record), " + juce::String (other) + " bundle(s) of other categories stopped at the scan");
        for (const auto& [p, why] : in.licence) line ("  " + p + " | " + why);
        if (other > 0) { juce::StringArray parts; for (const auto& [k, n] : in.licenceOther) parts.add (k + " " + juce::String (n)); line ("  other categories: " + parts.joinIntoString (", ")); }
    }

    head ("9. CRASHES, WINDOWS, TIMEOUTS");
    line (juce::String ((int) in.crashes.size()) + " (over " + juce::String (in.runLinesAdded) + " run.jsonl line(s) the follow-up added)");
    for (const auto& c : in.crashes) line ("  " + c.where + ": " + c.what);
    line();
    s << renderPhaseB (in.phaseb);
    return s;
}

} // namespace ejmap::review
