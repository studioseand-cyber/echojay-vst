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
#include <optional>
#include <cstring>

namespace ejmap::loop
{
inline const char* const kStates[] = { "exported", "recorded", "refused", "held", "needs_review", "quarantined_at_scan", "needs_licence" };
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
inline Outcome outcomeCarriedLicence (const QuarantinedBundle& b)
{
    Outcome o; o.state = "needs_licence";
    o.reason = "carried forward from the scan: " + b.reason + " at " + b.at + " (" + juce::File (b.bundle).getFileName() + "); not loaded again; --retry-licence re-checks it";
    return o;
}

struct Counts { int exported = 0, recorded = 0, refused = 0, held = 0, needsReview = 0, quarantined = 0, needsLicence = 0, rows = 0; };
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
        }
    return c;
}
} // namespace ejmap::loop
