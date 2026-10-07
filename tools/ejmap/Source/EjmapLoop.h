/*
  EjmapLoop.h - ONE LOOP DOES EVERYTHING (ruled 2 Oct 2026, docs/STRANGER_MAC_TEST.md).

  The certification batch (--cert-sweep-all) must leave every discovered compressor and tuner in EXACTLY ONE named
  state, with no silent drops and no hand steps, on a Mac nobody here controls. The states:

    needs_licence  the scan's window watch killed the bundle's load because it raised a licence / activation window
                  (PACE's with the iLok away, or a vendor's own); recorded with the windows and the time; re-scanned
                  only by --scan --retry-licence once the licence is back; never clicked
  "Unmapped" is a FIELD on the row (map: local map | server map state N | server map at a different build | none),
  never a state (ruled 2 Oct, afternoon): certification does not need a map - the join is by map_fp.
    quarantined_at_scan  the scan quarantined the bundle (a stall or a crash) so it never reached the census; the row
                  names the product(s), the category where known, and says if it is a VST3 (the AU is unaffected)

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
#include "EjmapSidechainCheck.h"
#include "EjmapSweep.h"
#include <optional>
#include <cstring>

namespace ejmap::loop
{
inline const char* const kStates[] = { "exported", "recorded", "refused", "held", "needs_review", "quarantined_at_scan", "needs_licence", "multiband", "surround" };

// OUT-OF-SCOPE STATES (ruled 4 Oct, ON): a multiband (band-numbered threshold candidates, or Low AND Mid AND High threshold
// candidates by literal word) is "multiband: profiling not built yet"; a product with more than two channels (Logic's
// "(N->N)" in the name, N > 2 - the Spherix units) is "surround: not profiled". Neither is needs_review: nobody reviews them.
// BAND WORDS (widened 4 Oct for DynOne3 and OTT): "Band N"; Low AND Mid AND High; the band words LF / LMF / MF / HMF / HF (two
// or more distinct, whatever channel or position prefix sits before them - DynOne3's "C HMF", "LR MF", "S HF"); and the
// single letters L AND M AND H all present (OTT's "Thresh L / M / H"). A genuine L/R pair never has an M alongside an H, and
// "L/M" + "R/S" (DPR-402, dbx-160) is split on the slash into L, M, R, S - no H, so not a multiband: pinned.
inline juce::String multibandBands (const juce::var& cands)
{
    juce::StringArray bands, bandWords; bool low = false, mid = false, high = false, l = false, m = false, h = false;
    static const juce::StringArray kBandWords { "lf", "lmf", "mf", "hmf", "hf" };
    for (int i = 0; i < cands.size(); ++i)
    {
        const auto n = cands[i].getProperty ("name", "").toString();
        const auto tokens = juce::StringArray::fromTokens (n.replaceCharacters ("()-:/", "     "), " ", "");
        for (int k = 0; k + 1 < tokens.size(); ++k) if (tokens[k].equalsIgnoreCase ("band") && tokens[k + 1].containsOnly ("0123456789") && tokens[k + 1].isNotEmpty()) bands.addIfNotAlreadyThere ("Band " + tokens[k + 1]);
        for (const auto& t : tokens)
        {
            const auto lt = t.toLowerCase();
            if (lt == "low") low = true; if (lt == "mid") mid = true; if (lt == "high") high = true;
            if (lt == "l") l = true; if (lt == "m") m = true; if (lt == "h") h = true;
            if (kBandWords.contains (lt)) bandWords.addIfNotAlreadyThere (t.toUpperCase());
        }
    }
    if (bands.size() >= 2) return bands.joinIntoString (", ");
    if (low && mid && high) return "Low, Mid, High";
    if (bandWords.size() >= 2) return bandWords.joinIntoString (", ");
    if (l && m && h) return "L, M, H";
    return {};
}
inline int surroundChannels (const juce::String& product)
{
    const auto m = product.fromLastOccurrenceOf ("(", false, false).upToFirstOccurrenceOf (")", false, false);   // "10->10"
    if (! m.contains ("->")) return 0;
    const int out = m.fromFirstOccurrenceOf ("->", false, false).trim().getIntValue();
    return out > 2 ? out : 0;
}
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
        // A WINDOW AT THE PROBE'S LOAD IS needs_licence (ruled 2 Oct): the same evidence the scan's window watch records,
        // the same state; --retry-licence brings it back when the licence is.
        if (ref.getProperty ("stage", "").toString() == "window")
        {
            o.state = "needs_licence";
            o.reason = "window at the probe's load (batch): " + ref.getProperty ("reason", "").toString() + "; not retried until --retry-licence";
            return o;
        }
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
    if (const int ch = surroundChannels (record.getProperty ("product", "").toString()); ch > 2)
    { o.state = "surround"; o.reason = "surround: not profiled (" + juce::String (ch) + " channels; the profile is a stereo contract)"; return o; }
    if (const auto cands = record.getProperty ("thresholdCandidates", {}); cands.isArray())
    {
        // LICENCE FIRST (ruled 4 Oct): a product that produced no tone on any candidate is a licence row before it is anything else
        // LICENCE, NOT CHANNEL STRIP (ruled 4 Oct): when EVERY candidate's verdict is licence_suspect the product produced no tone
        // on any stage - the row is needs_licence with the reason, never "N threshold candidates" (Sean's run: 7 Melda + Pro-C 3)
        if (const auto verdicts = record.getProperty ("thresholdReview", {}).getProperty ("verdicts", {}); verdicts.isArray() && verdicts.size() > 0)
        {
            bool allSuspect = true; for (int i = 0; i < verdicts.size(); ++i) if (verdicts[i].getProperty ("result", "").toString() != "licence_suspect") allSuspect = false;
            if (allSuspect)
            {
                juce::String why; for (int i = 0; i < cands.size() && why.isEmpty(); ++i) why = cands[i].getProperty ("licenceSuspectReason", "").toString();
                o.state = "needs_licence";
                o.reason = "licence suspected: " + (why.isNotEmpty() ? why : juce::String ("every candidate's output was silent or not the tone at its reference (recorded as licence_suspect; the reason text is not on this record)")) + "; " + juce::String (cands.size()) + " candidate(s), none produced the tone; --retry-licence re-checks it";
                return o;
            }
        }
        // a Rule-1 or measured-rule pick goes through first (a decided record is never out of scope by its band names)
        const bool decided = record.getProperty ("pickedCandidate", {}).isObject() && record.getProperty ("ruleDecided", {}).isObject();
        if (! decided) if (const auto bands = multibandBands (cands); bands.isNotEmpty())
        { o.state = "multiband"; o.reason = "multiband: profiling not built yet (" + bands + "; " + juce::String (cands.size()) + " threshold candidates)"; return o; }
        // RULE 1 (ruled 2 Oct, evening): a record whose comp-worded candidate certified alone carries pickedCandidate +
        // ruleDecided; it goes on through the single view (candidateAsSingle) like any certified profile-grade record.
        const auto pick = record.getProperty ("pickedCandidate", {});
        if (pick.isObject() && record.getProperty ("ruleDecided", {}).isObject())
        {
            for (int i = 0; i < cands.size(); ++i)
                if ((int) cands[i].getProperty ("index", -2) == (int) pick.getProperty ("index", -1))
                {
                    const auto sw = cands[i].getProperty ("thresholdSweep", {});
                    if (sw.getProperty ("result", "").toString() == "certified" && profileGrade (sw)) { o.exportPending = true; o.state = "needs_review"; o.reason = "export pending (Rule 1 pick)"; return o; }
                }
        }
        // INERT ON EVERY CANDIDATE (ruled 5 Oct, with the single-sweep rule below): no stage ran its processing - a licence row
        {
            bool allInert = cands.size() > 0; juce::String inertWhy;
            for (int i = 0; i < cands.size(); ++i) { const auto sw = cands[i].getProperty ("thresholdSweep", {}); if (sw.getProperty ("result", "").toString() != "inert") allInert = false; else if (inertWhy.isEmpty()) inertWhy = sw.getProperty ("reason", "").toString(); }
            if (allInert) { o.state = "needs_licence"; o.reason = "licence suspected: " + (inertWhy.isNotEmpty() ? inertWhy : juce::String ("processing never runs (inert)")) + " on every candidate (" + juce::String (cands.size()) + "); the remedy is activation and a re-run (ruled 5 Oct); --retry-licence re-checks it"; return o; }
        }
        // FLAT ON EVERY CANDIDATE (ruled 4 Oct, dbx-160 (s) and kHs Dynamics): nothing to pick from - filed with the flat-results
        // investigation, in the same words as a single-sweep flat, not as a review item
        {
            bool allFlat = cands.size() > 0; juce::String flatWhy;
            for (int i = 0; i < cands.size(); ++i) { const auto sw = cands[i].getProperty ("thresholdSweep", {}); if (sw.getProperty ("result", "").toString() != "flat") allFlat = false; else if (flatWhy.isEmpty()) flatWhy = sw.getProperty ("reason", "").toString(); }
            if (allFlat) { o.state = "needs_review"; o.reason = "sweep result flat on every candidate (" + juce::String (cands.size()) + "): " + flatWhy + " - the flat-results investigation, nothing to pick"; return o; }
        }
        o.state = "needs_review";
        if (record.getProperty ("strip_section", "").toString() == "compressor")   // Kathy, 7 Oct: each is a compressor threshold by the section word - ordinary candidates, a review pick decides
        { o.reason = juce::String (cands.size()) + " candidates in the strip's compressor section (each a compressor threshold by the section word): no measured rule decided them; a review pick is needed"; return o; }
        o.reason = juce::String (cands.size()) + " threshold candidates (a channel strip or multiband): no rule decides it, nobody picks";
        return o;
    }
    const auto sw = record.getProperty ("thresholdSweep", {});
    if (! sw.isObject()) { o.state = "needs_review"; o.reason = "record carries no sweep, no candidates and no refusal"; return o; }
    const auto result = sw.getProperty ("result", "").toString();
    // INERT = LICENCE (Kathy's ruling, 5 Oct): a product whose output no control moves, Power included, never ran its
    // processing; the remedy is activation and a re-run, so the row is needs_licence with that reason, and
    // --retry-licence is what brings it back. The record keeps result "inert" and its inertCheck (what was measured).
    if (result == "inert")
    {
        o.state = "needs_licence";
        o.reason = "licence suspected: " + (sw.getProperty ("reason", "").toString().isNotEmpty() ? sw.getProperty ("reason", "").toString() : juce::String ("processing never runs (inert)"))
                 + "; the remedy is activation and a re-run (ruled 5 Oct); --retry-licence re-checks it";
        return o;
    }
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
// A FAILED TONE CHECK IS needs_review (ruled 4 Oct, Lindell 254E and VBC FG-Grey in Sean's run): the profile stays on disk
// with its tone_check block, but the row never says exported - the server would act on a profile the check contradicts.
inline Outcome outcomeAfterExport (bool exportOk, const juce::String& exportWhy, bool toneRan, const juce::String& toneWhy, const juce::var& toneResult = {})
{
    Outcome o;
    if (! exportOk) { o.state = "needs_review"; o.reason = "export refused: " + exportWhy; return o; }
    if (! toneRan)  { o.state = "needs_review"; o.reason = "exported, but the tone check could not run: " + toneWhy; return o; }
    if (toneResult.isObject() && ! (bool) toneResult.getProperty ("pass_within_0_5_db", false))
    {
        const auto gr = toneResult.getProperty ("gr_measured_db", {});
        o.state = "needs_review";
        o.reason = "tone check failed: " + ((gr.isDouble() || gr.isInt()) ? juce::String ((double) gr, 2) : juce::String ("unreadable")) + " vs " + juce::String ((double) toneResult.getProperty ("g_db", 2.0), 1)
                 + ((bool) toneResult.getProperty ("quiet_check_ok", true) ? juce::String() : juce::String (" (the quiet-reference check failed too)")) + "; the profile is written but not exported";
        return o;
    }
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
    if (! isState (state)) return "state '" + state + "' is not one of the seven";
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

// QUARANTINED AT SCAN (ruled 2 Oct): a bundle the scan quarantined (a stall, a crash) never reaches the census, so a
// compressor or tuner inside it would drop out silently - the one thing the pass criteria forbid. Each quarantine entry
// becomes a row in its own state, with the product's category from categories.json where the product was ever
// categorised (by the bundle's registered AU uid, else by name), else "unknown". A VST3 bundle is named as such:
// certification hosts AudioUnits, so the same product's AU, if it scanned, is unaffected.
inline juce::String productKeyName (const juce::String& n);
struct QuarantinedBundle { juce::String bundle, reason, stage, at; juce::StringArray products; juce::String category = "unknown"; bool vst3 = false; bool licence = false; juce::StringArray windows; };
inline std::vector<QuarantinedBundle> quarantinedAtScan (const juce::var& quarantine, const juce::var& categories,
                                                         const std::map<juce::String, juce::StringArray>& auNamesByBundle,
                                                         const std::map<juce::String, juce::String>& uidByAuName)
{
    std::map<juce::String, juce::String> catByUid, catByName;
    if (const auto* prods = categories.getProperty ("products", {}).getDynamicObject())
        for (const auto& kv : prods->getProperties())
        {
            const auto cat = kv.value.getProperty ("category", "").toString();
            if (cat.isEmpty()) continue;
            catByName[kv.value.getProperty ("name", "").toString().toLowerCase()] = cat;
            if (const auto* mk = kv.value.getProperty ("mark_keys", {}).getArray()) for (const auto& k : *mk) catByUid[k.toString()] = cat;
        }
    std::vector<QuarantinedBundle> out;
    if (const auto* a = quarantine.getArray())
        for (const auto& q : *a)
        {
            QuarantinedBundle b;
            b.bundle = q.getProperty ("plugin_id", "").toString(); b.reason = q.getProperty ("reason", "").toString();
            b.stage = q.getProperty ("stage", "").toString(); b.at = q.getProperty ("at", "").toString();
            // NEEDS LICENCE (ruled 2 Oct): an entry from licence-stops.json carries its windows and state instead of a reason
            if (q.getProperty ("state", "").toString() == "needs_licence")
            {
                b.licence = true;
                if (const auto* w = q.getProperty ("windows", {}).getArray()) for (const auto& x : *w) b.windows.add (x.toString());
                b.reason = juce::String ((bool) q.getProperty ("pace", false) ? "activation window" : "window") + " at scan (" + b.windows.joinIntoString (", ") + "); load killed at once, not retried";
            }
            b.vst3 = b.bundle.endsWithIgnoreCase (".vst3");
            const auto stem = juce::File (b.bundle).getFileNameWithoutExtension();
            if (auto it = auNamesByBundle.find (b.bundle); it != auNamesByBundle.end()) b.products = it->second;
            if (b.products.isEmpty()) b.products.add (stem);
            for (const auto& n : b.products)
            {
                if (auto u = uidByAuName.find (n); u != uidByAuName.end()) if (auto c = catByUid.find ("AudioUnit|" + u->second); c != catByUid.end()) { b.category = c->second; break; }
                if (auto c = catByName.find (n.toLowerCase()); c != catByName.end()) { b.category = c->second; break; }
            }
            out.push_back (b);
        }
    return out;
}
inline bool certificationCategory (const juce::String& c) { return c == "compressor" || c == "pitch" || c == "tuner"; }
// ONE ROW PER PRODUCT (ruled 2 Oct, evening): the scan's bundle evidence becomes rows for the products that have no
// subject row of their own - one per product named by the bundle, keyed "product|<name>", linked to the bundle. A product
// the batch has as a subject gets its state on its own row (carried forward) and no second row here.
inline juce::var bundleProductRow (const QuarantinedBundle& b, const juce::String& product, const juce::String& when)
{
    Outcome o; o.state = b.licence ? "needs_licence" : "quarantined_at_scan";
    o.reason = b.reason + " at stage " + b.stage + (b.vst3 ? " (a VST3 bundle; certification hosts the AudioUnit, which is unaffected if it scanned)" : juce::String()) + "; category " + b.category;
    auto row = makeRow ("product|" + productKeyName (product), product, b.category, o, {}, {}, {}, when);
    if (auto* r = row.getDynamicObject()) r->setProperty ("scan_bundle", b.bundle);
    return row;
}
inline std::vector<juce::var> bundleRows (const std::vector<QuarantinedBundle>& bundles, const juce::StringArray& subjectNames, const juce::String& when)
{
    juce::StringArray subjects; for (const auto& n : subjectNames) subjects.add (productKeyName (n));
    std::vector<juce::var> out; juce::StringArray seen;
    for (const auto& b : bundles)
        for (const auto& p : b.products)
        {
            const auto key = productKeyName (p);
            if (subjects.contains (key) || seen.contains (key)) continue;
            seen.add (key);
            out.push_back (bundleProductRow (b, p, when));
        }
    return out;
}

inline juce::var quarantineRow (const QuarantinedBundle& b, const juce::String& when)
{
    Outcome o; o.state = b.licence ? "needs_licence" : "quarantined_at_scan";
    o.reason = b.reason + " at stage " + b.stage + (b.vst3 ? " (a VST3 bundle; certification hosts the AudioUnit, which is unaffected if it scanned)" : juce::String()) + "; category " + b.category;
    return makeRow ("bundle|" + b.bundle, b.products.joinIntoString (", "), b.category, o, {}, {}, {}, when);
}

// THE SCAN'S LICENCE EVIDENCE, CARRIED FORWARD (ruled 2 Oct): the batch does not load a product whose bundle raised an
// activation window at the scan; it writes the same state and names the scan's window. Matched by product name
// (the scan loads the VST3 bundle; the licence is the product's, so its AudioUnit is held by the same evidence);
// "(m)/(s)" channel suffixes do not break the match.
inline juce::String productKeyName (const juce::String& n)
{
    auto s = n.trim().toLowerCase();
    for (const char* suf : { " (m)", " (s)", " (mono)", " (stereo)" }) if (s.endsWith (suf)) s = s.dropLastCharacters ((int) std::strlen (suf)).trim();
    return s;
}
inline std::optional<QuarantinedBundle> carriedLicenceStop (const std::vector<QuarantinedBundle>& stops, const juce::String& product)
{
    const auto want = productKeyName (product);
    for (const auto& b : stops)
    {
        if (! b.licence) continue;
        for (const auto& p : b.products) if (productKeyName (p) == want) return b;
    }
    return std::nullopt;
}

// DO NOT LOAD A PRODUCT THE SESSION KNOWS NEEDS A LICENCE THAT IS NOT PRESENT (ruled 3 Oct, after CL 1B was loaded and killed
// at 2.9 s by the tone check with the iLok away): the scan's licence stop in this ledger, or a needs_licence row already in
// this cert folder, is the knowledge; the reason names its source. Empty = nothing known. --retry-licence is the only way past.
inline juce::String knownLicenceStop (const std::vector<QuarantinedBundle>& scanStops, const juce::var& outcomes, const juce::String& product, bool retryLicence)
{
    if (retryLicence) return {};
    if (const auto stop = carriedLicenceStop (scanStops, product); stop)
        return "needs a licence (known from the scan: " + (stop->reason.isNotEmpty() ? stop->reason : stop->bundle) + "); not loaded";
    if (const auto* a = outcomes.getArray())
        for (const auto& r : *a)
            if (r.getProperty ("state", "").toString() == "needs_licence" && productKeyName (r.getProperty ("product", "").toString()) == productKeyName (product))
                return "needs a licence (known from this folder's outcomes: " + r.getProperty ("reason", "").toString().upToFirstOccurrenceOf (";", false, false) + "); not loaded";
    return {};
}
inline Outcome outcomeCarriedLicence (const QuarantinedBundle& b)
{
    Outcome o; o.state = "needs_licence";
    o.reason = "carried forward from the scan: " + b.reason + " at " + b.at + " (" + juce::File (b.bundle).getFileName() + "); not loaded again; --retry-licence re-checks it";
    return o;
}

// CARRY-OVER ON RE-DERIVATION (v1.7 tone-check-only mode): a re-derivation rebuilds the sweep from the traces, and the
// traces do not hold what later steps wrote INTO the record - the detector (its own processes), Rule 1's decision, the
// pick, the map state, the manufacturer. Those ride over from the old record; the sweep's measured fields do not.
// THE ONE-TIME REVIEW PICK (ruled 4 Oct): cert/review_picks.json - [{"product", "candidate", "by", "date", "note"}] - is where
// Kathy records a pick she made from the candidates' curves. A picked candidate whose sweep certified becomes
// pickedCandidate + ruleDecided {rule: "review_pick", by, date} and goes on exactly like a Rule-1 pick (re-derived,
// exported, tone-checked), the notes naming the pick and who made it. NOTHING IS EVER PICKED WITHOUT AN ENTRY, and an entry
// that names an uncertified candidate (or no candidate) picks nothing and says why on the row.
inline juce::var reviewPickFor (const juce::var& picks, const juce::String& product)
{
    if (const auto* a = picks.getArray()) for (const auto& p : *a) if (p.getProperty ("product", "").toString() == product) return p;
    return {};
}
inline juce::String applyReviewPick (juce::var& record, const juce::var& picks)   // returns what happened, empty when no entry applies
{
    const auto cands = record.getProperty ("thresholdCandidates", {});
    if (! cands.isArray()) return {};
    const auto pick = reviewPickFor (picks, record.getProperty ("product", "").toString());
    if (! pick.isObject()) return {};
    // an earlier REVIEW pick on the record gives way to a changed entry (a later date, or another candidate - MAGNUM-K, 6 Oct);
    // a measured rule's pick or an older entry's own pick stays
    if (record.getProperty ("pickedCandidate", {}).isObject())
    {
        const auto rd = record.getProperty ("ruleDecided", {});
        const bool byReview = rd.getProperty ("rule", "").toString() == "review_pick";
        const bool changed = rd.getProperty ("date", "").toString() != pick.getProperty ("date", "").toString() || rd.getProperty ("entry", "").toString() != pick.getProperty ("candidate", "").toString();
        if (! (byReview && changed)) return {};
    }
    auto want = pick.getProperty ("candidate", "").toString(); const auto by = pick.getProperty ("by", "").toString(), date = pick.getProperty ("date", "").toString();
    if (want.isEmpty() || by.isEmpty() || date.isEmpty()) return "review pick entry incomplete (needs candidate, by, date): nothing picked";
    // "A + B as a pair" (ruled 6 Oct, MAGNUM-K): A is the amount, B is written WITH it at every position (a dual-mono pair)
    juce::String pairName;
    if (want.endsWith (" as a pair") && want.contains (" + ")) { const auto both = want.dropLastCharacters (10); want = both.upToFirstOccurrenceOf (" + ", false, false).trim(); pairName = both.fromFirstOccurrenceOf (" + ", false, false).trim(); }
    for (int i = 0; i < cands.size(); ++i)
    {
        if (cands[i].getProperty ("name", "").toString() != want) continue;
        const auto res = cands[i].getProperty ("thresholdSweep", {}).getProperty ("result", "").toString();
        if (res != "certified") return "review pick names '" + want + "' but its sweep is " + (res.isEmpty() ? juce::String ("absent") : res) + ": nothing picked";
        auto* pk = new juce::DynamicObject(); pk->setProperty ("index", cands[i].getProperty ("index", -1)); pk->setProperty ("name", want);
        pk->setProperty ("note", "review pick by " + by + " on " + date + " (cert/review_picks.json)");
        auto* rd = new juce::DynamicObject(); rd->setProperty ("rule", "review_pick"); rd->setProperty ("by", by); rd->setProperty ("date", date); rd->setProperty ("entry", pick.getProperty ("candidate", ""));
        rd->setProperty ("ruleText", "picked by " + by + " on " + date + " from the candidates' 2 dB curves and verdicts (review_picks.json)" + (pick.getProperty ("note", "").toString().isNotEmpty() ? ": " + pick.getProperty ("note", "").toString() : juce::String())
                                     + "; the other candidates stay at their instantiate values (neutral); the tone check writes in the server's order and requires both output channels within 0.5 dB of g, as for a pair rule");
        auto* pv = new juce::DynamicObject(); pv->setProperty ("index", cands[i].getProperty ("index", -1)); pv->setProperty ("name", want); rd->setProperty ("pick", juce::var (pv));
        juce::Array<juce::var> others; for (int k = 0; k < cands.size(); ++k) if (k != i) others.add (cands[k].getProperty ("name", "")); rd->setProperty ("trims", others);
        if (pairName.isNotEmpty())
        {
            int pi = -1; for (int k = 0; k < cands.size(); ++k) if (cands[k].getProperty ("name", "").toString() == pairName) pi = (int) cands[k].getProperty ("index", -1);
            if (pi < 0) return "review pick names '" + pairName + "' as the pair, which is not one of this record's candidates: nothing picked";
            auto* pw = new juce::DynamicObject(); pw->setProperty ("index", pi); pw->setProperty ("name", pairName); rd->setProperty ("pair_with", juce::var (pw));
            rd->setProperty ("ruleText", rd->getProperty ("ruleText").toString() + "; PAIR: '" + pairName + "' is written WITH '" + want + "' at every position (dual-mono pair, ruled 6 Oct), both channels measured, gated on the worse");
        }
        record.getDynamicObject()->setProperty ("pickedCandidate", juce::var (pk)); record.getDynamicObject()->setProperty ("ruleDecided", juce::var (rd));
        return "review pick applied: '" + want + "'" + (pairName.isNotEmpty() ? " + '" + pairName + "' as a pair" : juce::String()) + " by " + by + " on " + date;
    }
    return "review pick names '" + want + "', which is not one of this record's candidates: nothing picked";
}

// RE-SWEEP OR RE-DERIVE (ruled 4 Oct): the follow-up decides this itself - a record whose plan UNDER THIS BUILD differs from the
// plan it was swept under is re-swept; everything else is re-derived from its traces and tone-checked. Sean never names a
// product. Differences that count: the record was refused at plan and now plans (the input-drive / one-knob products); the
// amount control or its flags changed; the candidate set changed (a switch dropped, an amount pair found); a certified
// single sweep whose reachable 2 dB curve still has a gap over the refinement bar after the rounds it took (DSM V3). A record
// with no controls, a tuner, or a refusal that still refuses, never re-sweeps. Licence rows are the --retry-licence set.
struct PlanDiff { bool resweep = false; juce::String why; };
inline PlanDiff planDiffers (const juce::var& record, const sweep::Plan& now)
{
    PlanDiff d;
    // THE PAIR WRITE (Kathy's ruling 3, 6 Oct): a record decided as a dual-mono pair (measured, or a review pick "A + B as a pair")
    // whose picked candidate was not swept with the twin written at every position is re-swept that way
    if (const auto rd = record.getProperty ("ruleDecided", {}); rd.getProperty ("pair_with", {}).isObject() && record.getProperty ("controls", {}).isArray())
    {
        const int pick = (int) rd.getProperty ("pick", {}).getProperty ("index", -1), twin = (int) rd.getProperty ("pair_with", {}).getProperty ("index", -1);
        juce::var view;
        if (const auto* cs = record.getProperty ("thresholdCandidates", {}).getArray()) for (const auto& c : *cs) if ((int) c.getProperty ("index", -1) == pick) view = c.getProperty ("thresholdSweep", {});
        if (! view.isObject()) view = record.getProperty ("thresholdSweep", {});
        if (view.isObject() && (int) view.getProperty ("pairWrite", {}).getProperty ("index", -2) != twin)
        { d.resweep = true; d.why = "the pair write: '" + rd.getProperty ("pair_with", {}).getProperty ("name", "").toString() + "' must be written with '" + rd.getProperty ("pick", {}).getProperty ("name", "").toString() + "' at every position (dual-mono pair, ruled 6 Oct) and this sweep did not"; return d; }
    }
    if (! record.getProperty ("controls", {}).isArray()) return d;
    // A TUNER (4 Oct, A4): re-measured when it was measured under an older tuner procedure (no pitchPlan, or an earlier
    // version): detents by evidence and the adaptive speed half period are plan changes the record cannot show otherwise
    if (record.getProperty ("schema", "").toString() == "ej_cert_tuner/1")
    {
        if (! record.hasProperty ("pitchCandidates")) return d;                        // nothing was measured: a refusal or an identity-only record
        const int v = (int) record.getProperty ("pitchPlan", {}).getProperty ("version", 1);
        if (v < 2) { d.resweep = true; d.why = "the tuner procedure changed (plan v" + juce::String (v) + " -> v2: detents by evidence, speed half period 1/2/4 s)"; }
        return d;
    }
    auto listOf = [] (const sweep::Plan& p) { juce::StringArray a; if (p.thr >= 0) a.add (juce::String (p.thr)); for (const auto& c : p.candidates) a.add (juce::String (c.index)); a.sort (false); return a.joinIntoString (","); };
    if (const auto ref = record.getProperty ("thresholdRefusal", {}); ref.isObject())
    {
        const bool atPlan = ref.getProperty ("stage", "").toString() == "plan";
        if (atPlan && now.ok) { d.resweep = true; d.why = "refused at plan under the batch build (" + ref.getProperty ("reason", "").toString().upToFirstOccurrenceOf (";", false, false) + "); this build plans " + (now.thr >= 0 ? "[" + juce::String (now.thr) + "] " + now.thrName : juce::String ((int) now.candidates.size()) + " candidates"); }
        return d;   // any other refusal: --retry-refused / --retry-licence decide, not the plan
    }
    if (! now.ok) return d;
    // the plan the record was swept under
    juce::String sweptList, sweptFlags; int rounds = 0; juce::var sw = record.getProperty ("thresholdSweep", {});
    // a certified sweep that could still refine: fewer than 9 positions reach 1 dB and a reachable 2 dB gap is still over the bar, rounds left
    // A STEPPED CONTROL HAS NO POSITION BETWEEN ITS DETENTS: declared stepped, or stepped by evidence (amountLanding from the
    // tone-check session's landing read), a round adds nothing - without this a 6-detent UnFairchild would be re-swept on
    // every run until kRefineRounds (a filter that creates its own work)
    auto steppedControl = [&] (int idx)
    {
        if (idx >= 0 && sweep::isSteppedControl (sweep::findControl (record, idx))) return true;
        const auto ev = record.getProperty ("amountLanding", {});
        return ev.isObject() && (int) ev.getProperty ("control", -1) == idx && (int) ev.getProperty ("detents", 0) >= 2;
    };
    // SWEPT AT THE DETENTS (Sean's condition 1, 4 Oct): a control the landing evidence shows stepped must have been swept at
    // exactly those read-back detents; a sweep at other positions (16 evenly spaced over a 6-detent UnFairchild) is re-swept
    auto detentCheck = [&] (const juce::var& sv) -> bool
    {
        if (! sv.isObject()) return false;
        const int idx = (int) sv.getProperty ("sweptControl", {}).getProperty ("index", (int) sv.getProperty ("thresholdPick", {}).getProperty ("index", now.thr));
        const auto det = sweep::landingDetentNorms (record, idx);
        if (det.empty()) return false;
        const auto norms = sv.getProperty ("positionNorms", {});
        bool same = norms.size() == (int) det.size();
        for (int i = 0; same && i < norms.size(); ++i) { bool hit = false; for (float d : det) if (std::abs ((double) norms[i] - d) < 1e-4) hit = true; same = hit; }
        if (same) return false;
        d.resweep = true; d.why = "swept at " + juce::String (norms.size()) + " position(s) but the control holds " + juce::String ((int) det.size()) + " detents (the landing read): re-swept at the detents as read back";
        return true;
    };
    auto refinementCheck = [&] (const juce::var& sv, int roundsTaken)
    {
        if (detentCheck (sv)) return;
        if (! sv.isObject() || sv.getProperty ("result", "").toString() != "certified" || roundsTaken >= sweep::kRefineRounds) return;
        const int idx = (int) sv.getProperty ("sweptControl", {}).getProperty ("index", (int) sv.getProperty ("thresholdPick", {}).getProperty ("index", now.thr));
        if (steppedControl (idx)) return;
        // STEPPED BY ITS OWN SWEEP (ruled 6 Oct, UnFairchild): writes that landed only on k/(n-1) make the control stepped with those
        // detents; a refinement round would add positions between detents that cannot land. Sean's stepped rule judges it, not "at least 9".
        if (sweep::landedDetents (sv) >= 2) return;
        const auto norms = sv.getProperty ("positionNorms", {}); const auto ia = sv.getProperty ("inAtGr", {});
        int withOne = 0; std::vector<double> twos, ones;
        for (int i = 0; i < ia.size() && i < norms.size(); ++i) { const auto one = ia[i].getProperty ("1", {}); if (one.isDouble() || one.isInt()) { ++withOne; ones.push_back ((double) one); } const auto two = ia[i].getProperty ("2", {}); if (two.isDouble() || two.isInt()) twos.push_back ((double) two); }
        auto worstGap = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); double w = 0.0; for (size_t k = 1; k < v.size(); ++k) w = juce::jmax (w, v[k] - v[k - 1]); return w; };
        const double worst2 = worstGap (twos), worst1 = worstGap (ones);
        if (withOne < 9 && (worst2 > sweep::kRefineGapDb || worst1 > sweep::kRefineGapDb))
        { d.resweep = true; d.why = "only " + juce::String (withOne) + " positions reach 1 dB and a " + (worst2 > sweep::kRefineGapDb ? "2 dB gap of " + juce::String (worst2, 1) : "1 dB gap of " + juce::String (worst1, 1)) + " dB remains after " + juce::String (roundsTaken) + " refinement round(s): a round on that gap adds positions"; }
    };
    // a candidates record (Rule 1, a measured rule, or a review pick): the plan's candidate set against the record's; the
    // picked candidate's sweep is the one the refinement check reads
    if (! sw.isObject()) if (const auto* cs = record.getProperty ("thresholdCandidates", {}).getArray())
    {
        juce::StringArray a; for (const auto& c : *cs) a.add (juce::String ((int) c.getProperty ("index", -1))); a.sort (false);
        const auto nowList = listOf (now);
        const int picked = (int) record.getProperty ("pickedCandidate", {}).getProperty ("index", -1);
        // a decided record (Rule 1 keeps only the candidate it swept): the pick must still be one of this build's candidates (or its single amount)
        if (picked >= 0 && record.getProperty ("ruleDecided", {}).isObject())
        {
            bool still = now.thr == picked; for (const auto& c : now.candidates) still = still || c.index == picked;
            if (! still) { d.resweep = true; d.why = "the picked candidate [" + juce::String (picked) + "] is no longer one this build plans [" + nowList + "]"; return d; }
        }
        else if (nowList != a.joinIntoString (",")) { d.resweep = true; d.why = "the candidate set changed: swept [" + a.joinIntoString (",") + "], this build plans [" + nowList + "]"; return d; }
        for (const auto& c : *cs) if ((int) c.getProperty ("index", -1) == picked) { const auto cv = c.getProperty ("thresholdSweep", {}); refinementCheck (cv, (int) cv.getProperty ("sweptControl", {}).getProperty ("refineRounds", (int) cv.getProperty ("gridRefinement", {}).getProperty ("rounds", 0))); }
        return d;
    }
    if (sw.isObject())
    {
        const auto sc = sw.getProperty ("sweptControl", {});
        if (sc.isObject()) { sweptList = juce::String ((int) sc.getProperty ("index", -1)); sweptFlags = sc.getProperty ("flags", "").toString(); rounds = (int) sc.getProperty ("refineRounds", 0); }
        else if (sw.getProperty ("thresholdPick", {}).isObject()) sweptList = juce::String ((int) sw.getProperty ("thresholdPick", {}).getProperty ("index", -1));
        else { rounds = (int) sw.getProperty ("gridRefinement", {}).getProperty ("rounds", 0); if (sw.getProperty ("roleFlag", "").toString().isNotEmpty()) sweptFlags = sw.getProperty ("roleFlag", "").toString(); }
    }
    const auto nowList = listOf (now);
    // A COLLAPSED CANDIDATES RECORD (found 6 Oct in Sean's b0258a7b run, in 17ebf114 too): the 5 Oct tone check's landing read wrote the
    // picked candidate's SINGLE VIEW back over the record, so a decided pair / pick (Vertigo, DPR-402, MaxxVolume ...) reads as "swept
    // [4]" against a plan of [11,4]. That is the pick, not a plan change: the record keeps its decision and the re-derive rebuilds the
    // candidates from their own traces (the "c<index>." runs). Never a re-sweep.
    if (sw.isObject() && ! now.candidates.empty() && record.getProperty ("pickedCandidate", {}).isObject() && record.getProperty ("ruleDecided", {}).isObject())
    {
        const int picked = (int) record.getProperty ("pickedCandidate", {}).getProperty ("index", -1);
        bool inPlan = false; for (const auto& c : now.candidates) inPlan = inPlan || c.index == picked;
        if (inPlan && sweptList == juce::String (picked)) { d.why = "a collapsed candidates record: the picked candidate's view stands for the record; rebuilt from its traces by the re-derive, not re-swept"; return d; }
    }
    if (sweptList.isNotEmpty() && nowList != sweptList)
    { d.resweep = true; d.why = "the plan changed: swept [" + sweptList + "], this build plans [" + nowList + "]" + (now.pickNote.isNotEmpty() ? " (" + now.pickNote.upToFirstOccurrenceOf (";", false, false) + ")" : juce::String()); return d; }
    if (sweptFlags.isNotEmpty() && now.thr >= 0 && now.thrFlags.joinIntoString (",") != sweptFlags && now.thrFlags.joinIntoString (",").isNotEmpty())
    { d.resweep = true; d.why = "the amount control's flags changed: swept as '" + sweptFlags + "', this build '" + now.thrFlags.joinIntoString (",") + "'"; return d; }
    refinementCheck (sw, rounds);
    return d;
}

inline juce::var carryOverAfterRederive (const juce::var& oldRecord, juce::var fresh)
{
    auto* o = fresh.getDynamicObject(); if (o == nullptr) return fresh;
    for (const char* k : { "ruleDecided", "pickedCandidate", "mapState", "manufacturer", "category" })
        if (oldRecord.hasProperty (k) && ! fresh.hasProperty (k)) o->setProperty (k, oldRecord.getProperty (k, {}));
    // THE WRITES THE SWEEP RAN AT (6 Oct, from Sean's b0258a7b run): the preconditions (a ratio raise, a make-up zero, a mix
    // at wet) come from the PLAN's two-sweep test at sweep time; the re-derive rebuilds the plan from the record without that
    // test, so its list is empty - and the export then fell back to the instantiate norm (Zip's ratio written back at 1:1, its
    // tone check 0.00 dB; 49 records lost their preconditions, 7 tone checks failed on it). The re-derived view keeps the
    // record's own writes when its own are empty: the traces hold the points, the record holds what was written.
    auto copyWrites = [] (const juce::var& from, juce::var to) {
        if (auto* t = to.getDynamicObject())
            for (const char* k : { "preconditions", "engageWrites", "thresholdPick", "passThroughAtDefaults", "passThroughOffset_db" })
            {
                const auto a = from.getProperty (k, {}); const auto b = to.getProperty (k, {});
                const bool bEmpty = b.isVoid() || (b.isArray() && b.size() == 0);
                const bool aFull = ! a.isVoid() && ! (a.isArray() && a.size() == 0);
                if (aFull && bEmpty) t->setProperty (k, a);
            } };
    auto copyDetector = [copyWrites] (const juce::var& from, juce::var to) { copyWrites (from, to); const auto d = from.getProperty ("detector", {}); if (d.isObject()) if (auto* t = to.getDynamicObject()) if (! to.hasProperty ("detector")) t->setProperty ("detector", d); };
    if (oldRecord.getProperty ("thresholdSweep", {}).isObject() && fresh.getProperty ("thresholdSweep", {}).isObject())
        copyDetector (oldRecord.getProperty ("thresholdSweep", {}), fresh.getProperty ("thresholdSweep", {}));
    if (const auto* oc = oldRecord.getProperty ("thresholdCandidates", {}).getArray())
        if (const auto* nc = fresh.getProperty ("thresholdCandidates", {}).getArray())
            for (const auto& a : *oc) for (const auto& b : *nc)
                if ((int) a.getProperty ("index", -1) == (int) b.getProperty ("index", -2)) copyDetector (a.getProperty ("thresholdSweep", {}), b.getProperty ("thresholdSweep", {}));
    // a COLLAPSED candidates record (6 Oct) rebuilt as candidates: the old single sweep is the picked candidate's view - its detector and
    // writes go to that candidate
    if (oldRecord.getProperty ("thresholdSweep", {}).isObject() && ! oldRecord.hasProperty ("thresholdCandidates"))
        if (const auto* nc = fresh.getProperty ("thresholdCandidates", {}).getArray())
            for (const auto& b : *nc) if ((int) b.getProperty ("index", -2) == (int) oldRecord.getProperty ("pickedCandidate", {}).getProperty ("index", -1)) copyDetector (oldRecord.getProperty ("thresholdSweep", {}), b.getProperty ("thresholdSweep", {}));
    return fresh;
}

// THE SIDECHAIN A/B OWED (ruled 6 Oct, the re-verify plan): a record swept under an earlier sidechain policy, with an extra
// input declared, that has no A/B verdict under the policy now is still owed its reading - a passed tone check does not
// close it. The view is the single sweep or the picked (else first) candidate's.
inline bool sidechainAbOwed (const juce::var& record)
{
    juce::var view = record.getProperty ("thresholdSweep", {});
    if (! view.isObject()) if (const auto* cs = record.getProperty ("thresholdCandidates", {}).getArray(); cs != nullptr && ! cs->isEmpty())
    {
        const int picked = (int) record.getProperty ("pickedCandidate", {}).getProperty ("index", -1);
        view = cs->getReference (0).getProperty ("thresholdSweep", {});
        for (const auto& c : *cs) if ((int) c.getProperty ("index", -1) == picked) view = c.getProperty ("thresholdSweep", {});
    }
    if (! view.isObject()) return false;
    const auto sc = view.getProperty ("sidechain", {});
    if (! sc.isObject() || sc.getProperty ("extraInputBuses", {}).size() == 0) return false;
    if (sc.getProperty ("policy", "").toString() == sidechaincheck::kPolicyNow) return false;
    const auto ab = record.getProperty ("sidechainPolicyCheck", {});
    return ! (ab.isObject() && ab.getProperty ("policyNow", "").toString() == sidechaincheck::kPolicyNow);
}

struct Counts { int exported = 0, recorded = 0, refused = 0, held = 0, needsReview = 0, quarantined = 0, needsLicence = 0, multiband = 0, surround = 0, rows = 0; };
inline Counts count (const juce::var& outcomes)
{
    Counts c;
    if (const auto* a = outcomes.getArray())
        for (const auto& r : *a)
        {
            ++c.rows;
            const auto s = r.getProperty ("state", "").toString();
            if (s == "exported") ++c.exported; else if (s == "recorded") ++c.recorded; else if (s == "refused") ++c.refused;
            else if (s == "held") ++c.held; else if (s == "needs_review") ++c.needsReview; else if (s == "quarantined_at_scan") ++c.quarantined; else if (s == "needs_licence") ++c.needsLicence;
            else if (s == "multiband") ++c.multiband; else if (s == "surround") ++c.surround;
        }
    return c;
}
} // namespace ejmap::loop
