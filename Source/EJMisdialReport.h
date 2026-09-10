#pragma once

// ===========================================================================
// MISDIAL REPORT v1, the client half's record assembly (9 Sep 2026).
//
// Contract: ~/Documents/ECHOJAY FILES/HANDOVER/misdial-report-v1.md, rewritten
// on 9 Sep from the DEPLOYED route. api/_misdials.js is binding, not the
// contract's prose and not this comment.
//
// WHY A HEADER. The property that matters is what the BODY CONTAINS, and a
// source-text pin cannot see a body. Header-inline in the manner of
// EJDialTally.h, EJRefusalLine.h and EJCaptureChannels.h, so tools/mapfps_test
// exercises the SHIPPED builder rather than a copy of it. PluginEditor.cpp
// holds no key names and no shape decisions of its own.
//
// THE FIVE REQUIRED FIELDS, and the route's exact rules for each
// (api/_misdials.js:109, :122 onward):
//   fp              64 hex characters, lowercased into the record
//   parameterName   the RAW MAP KEY, never a display label
//   parameterIndex  an integer >= 0. ZERO IS LEGITIMATE, so every presence
//                   test here is on the type or on a sentinel, never on
//                   truthiness. -1 is our "no index" sentinel and makes a row
//                   unreportable rather than sending -1 to be refused.
//   valueDialled    a finite number. Zero is legitimate for the same reason.
//   observedResult  a finite NUMBER (a readback) or a non-empty STRING (the
//                   apply outcome). The server stamps observedKind from which
//                   arrived, so a reader never has to guess whether "40" was
//                   a number or text.
//
// THE COMPLETENESS RULE IS HERE, not at the button. A control may only be
// offered when all five can be assembled, because the route's 400 is the
// backstop and not the boundary: a user must never be able to press a report
// that will be refused.
//
// ONLY ACCEPTED KEYS ARE EMITTED. Measured on the deployed route: there is no
// unknown-key check, so an unknown key would NOT 400, it would be silently
// dropped by buildRecord. It is left out anyway because it cannot reach the
// store and still counts against the 16 KB body cap. kAcceptedKeys is exported
// so the gate can assert that nothing outside it is ever sent.
//
// fp IS COPIED INTO THE ROW at apply time and never re-read at press time. The
// record is a snapshot: the user may replace the slot between the dial and the
// press, and a report that silently re-pointed at whatever now occupies that
// slot would send a fix to the wrong map.
// ===========================================================================

#include <JuceHeader.h>
#include <cmath>
#include <vector>

namespace echojay
{

// Every key the deployed route reads. Nothing else is ever put on the wire.
// Order is the record's own order, for a readable body.
inline const juce::StringArray& misdialAcceptedKeys()
{
    static const juce::StringArray keys {
        // the five required
        "fp", "parameterName", "parameterIndex", "valueDialled", "observedResult",
        // identity
        "pluginName", "format", "vendor", "pluginVersion", "appVersion",
        // the map's own belief at dial time
        "mapKind", "mapRangeMin", "mapRangeMax", "mapUnit",
        // which map version the report is against
        "mapVersion", "extractorVersion", "humanVerified",
        // intent
        "rid", "userAsk",
        // the user's own words, which kind of report this is, and which of the
        // four things the user said went wrong
        "note", "kind", "category",
        // provenance of the report itself
        "reportId", "source"
    };
    return keys;
}

// One dialled control, captured at apply time from ChainHost::ApplyReport.
// This is what is retained on the CHAT MESSAGE (not the slot) so it stays
// truthful after a rack change.
struct MisdialRow
{
    juce::String fp;            // Slot::fp, copied here at apply time
    juce::String mapKey;        // ApplyReport::semantic, the raw map key
    int          index = -1;    // ApplyReport::index, -1 = the map has none
    double       valueDialled = 0.0;
    bool         hasValue = false;   // requestedValue was a finite number
    juce::String landedText;    // ApplyReport::landedText, empty = no readback
    juce::String outcome;       // the apply outcome, used when landedText is empty

    // THE MAP'S OWN BELIEF ABOUT THIS CONTROL, and it lives HERE rather than on
    // the slot because kind, unit and range are PER CONTROL in the map. They
    // were on the slot facts in the first draft of this header, which would
    // have sent one control's unit under every control's key: a wrong belief in
    // front of the person fixing the map, which is worse than a null.
    //
    // NOT POPULATED IN v1. ApplyReport does not carry them and the apply reads
    // them locally (EchoJayParamApply.h:689, :974, :1028), so capturing them
    // means a second lookup into the map entry by semantic at capture time.
    // They are best effort, so an unset field is simply omitted and no report
    // is ever blocked for want of them. The fields exist now so that filling
    // them later is not a shape change.
    juce::String mapKind, mapUnit;
    double       mapRangeMin = 0.0, mapRangeMax = 0.0;
    bool         hasRange = false;

    // Set once the report has been filed, so the popup row settles and a
    // reload does not invite a second press.
    bool         reported = false;
    // Generated ONCE when the record is assembled and reused on every retry,
    // so a retry dedupes at the server rather than filing twice.
    juce::String reportId;
};

// The per-SLOT facts, all best effort. Absent fields are simply not emitted.
// Only things that are genuinely properties of the slot or its map as a whole
// belong here; anything per control lives on MisdialRow.
struct MisdialSlotFacts
{
    juce::String pluginName, vendor, format, pluginVersion, appVersion;
    juce::String mapVersion;   // the map's rev, one per map
};

// The two kinds the deployed route accepts (api/_misdials.js:112-114). An ABSENT
// kind is read as misdial for back compat with the build already shipping, but
// both are sent explicitly here: relying on a default means a reader of the
// stored record cannot tell an old client from a deliberate misdial.
inline const char* kMisdialKindMisdial() { return "misdial"; }
inline const char* kMisdialKindBug()     { return "bug"; }

// The route's cap on the user's own words (api/_misdials.js:106). Trimmed and
// truncated HERE as well as there, so what the user sees sent is what lands.
inline constexpr int kMisdialNoteMax = 1000;

// ---------------------------------------------------------------------------
// THE CATEGORY, AND THE KIND IT DECIDES.
//
// THE LITERALS ARE THE SERVER'S, COPIED EXACTLY from CATEGORIES at
// api/_misdials.js:137-142. An unknown category is REFUSED BY NAME rather than
// coerced to `other` (:256-260), and folding and trimming happen server side,
// so a near miss does not degrade: it fails outright, at the only moment when
// the user has already written their sentence. That is why these are constants
// here and never spelled inline at a call site.
//
// CATEGORY AND KIND MUST AGREE WHEN BOTH ARRIVE (:289-293). The route refuses a
// clash in both directions and names both in the refusal. The client makes a
// clash IMPOSSIBLE rather than merely unlikely: the body builders check the
// category against their own kind and return empty if it disagrees, so a
// mismatched pair cannot reach the wire at all.
// ---------------------------------------------------------------------------
inline const char* kMisdialCatWrongControl() { return "wrong_control"; }
inline const char* kMisdialCatPluginProblem() { return "plugin_problem"; }
inline const char* kMisdialCatChainProblem()  { return "chain_problem"; }
inline const char* kMisdialCatOther()         { return "other"; }

/** One row of the popup's category dropdown: the wire value, the words the user
    reads, the kind it files, and whether it requires a picked control. */
struct MisdialCategoryChoice
{
    const char* value;
    const char* label;
    const char* kind;           // the kind this category files
    bool        needsControl;   // true only for wrong_control
};

/** THE ONE TABLE. The four, in the order the popup lists them: wrong_control
    leads because it is the specific one, `other` is last because it is the
    fallback.

    THE KIND LIVES HERE RATHER THAN IN A SECOND LOOKUP, and that is not tidiness.
    It was written as a table plus an independent if-chain, and a mutation that
    changed one literal in the table left the if-chain answering correctly for a
    value the table no longer contained: the two disagreed and only one pin
    noticed. One row, one truth. */
inline const std::vector<MisdialCategoryChoice>& misdialCategories()
{
    static const std::vector<MisdialCategoryChoice> all {
        { "wrong_control",  "A setting went to the wrong place",      "misdial", true  },
        { "plugin_problem", "A problem with this plugin",             "bug",     false },
        { "chain_problem",  "A problem with the chain or suggestion", "bug",     false },
        { "other",          "Something else",                         "bug",     false },
    };
    return all;
}

/** The kind a category files, read off the ONE table above. Empty for an
    unknown category, which the caller must treat as "do not send": the route
    refuses it by name rather than coercing it to `other`. */
inline juce::String misdialKindForCategory (const juce::String& category)
{
    const auto c = category.trim().toLowerCase();
    for (const auto& e : misdialCategories())
        if (c == e.value) return e.kind;
    return {};
}

/** A fresh report id. Called ONCE per record; the row keeps it. */
inline juce::String newMisdialReportId()
{
    return juce::Uuid().toDashedString();
}

/** True when this text is a plain scalar reading we can send as a number.
    A colon is the tell that it is not: "2:1" is a ratio whose meaning dies if
    it becomes 2, so it goes as text and keeps its shape. */
inline bool misdialLandedIsScalar (const juce::String& landedRaw, double& out)
{
    const auto t = landedRaw.trim();
    if (t.isEmpty()) return false;
    if (t.containsChar (':')) return false;
    const auto c = t[0];
    if (! (juce::CharacterFunctions::isDigit (c) || c == '-' || c == '+' || c == '.'))
        return false;
    const double v = t.getDoubleValue();
    if (! std::isfinite (v)) return false;
    out = v;
    return true;
}

/** THE COMPLETENESS RULE. All five required fields must be assemblable, by the
    route's own tests, or the control is not offered at all. */
inline bool misdialRowIsReportable (const MisdialRow& r)
{
    // fp: 64 hex. Tested here rather than trusted, because a slot that never
    // resolved one carries an empty string and the route would refuse it.
    const auto fp = r.fp.trim();
    if (fp.length() != 64) return false;
    for (int i = 0; i < 64; ++i)
    {
        const auto c = juce::CharacterFunctions::toLowerCase (fp[i]);
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (! hex) return false;
    }
    if (r.mapKey.trim().isEmpty()) return false;
    if (r.index < 0) return false;                 // -1 sentinel, and negatives
    if (! r.hasValue) return false;
    if (! std::isfinite (r.valueDialled)) return false;
    // observedResult: a scalar readback, or SOME outcome text. Both empty is
    // the one case the route calls missing.
    double scalar = 0.0;
    if (misdialLandedIsScalar (r.landedText, scalar)) return true;
    return r.landedText.trim().isNotEmpty() || r.outcome.trim().isNotEmpty();
}

/** The POST body. Only keys from misdialAcceptedKeys(), best-effort fields
    omitted rather than sent as null, and observedResult in whichever of its two
    shapes the row actually holds.

    Returns an empty string when the row is not reportable, so a caller that
    skips the completeness check cannot put a refusable body on the wire. */
inline juce::String buildMisdialBody (const MisdialRow& row,
                                     const MisdialSlotFacts& facts,
                                     const juce::String& source,
                                     const juce::String& note = {},
                                     const juce::String& category = {})
{
    if (! misdialRowIsReportable (row)) return {};
    // A CLASH CANNOT REACH THE WIRE. The route refuses a category whose implied
    // kind disagrees with the kind sent, and names both. Rather than trust the
    // caller to pair them, refuse here: an unknown category, or one that files
    // a bug, is not a misdial body.
    if (category.trim().isNotEmpty()
        && misdialKindForCategory (category) != kMisdialKindMisdial())
        return {};

    juce::DynamicObject::Ptr o = new juce::DynamicObject();

    // ---- the five required
    o->setProperty ("fp", row.fp.trim().toLowerCase());
    o->setProperty ("parameterName", row.mapKey.trim());
    o->setProperty ("parameterIndex", row.index);
    o->setProperty ("valueDialled", row.valueDialled);

    double scalar = 0.0;
    if (misdialLandedIsScalar (row.landedText, scalar))
        o->setProperty ("observedResult", scalar);            // observedKind "value"
    else
        o->setProperty ("observedResult", row.landedText.trim().isNotEmpty()
                                              ? row.landedText.trim()
                                              : row.outcome.trim());   // "outcome"

    // ---- best effort: present or absent, never null
    auto put = [&o] (const char* key, const juce::String& v)
    {
        if (v.trim().isNotEmpty()) o->setProperty (key, v.trim());
    };
    put ("pluginName",    facts.pluginName);
    put ("format",        facts.format);
    put ("vendor",        facts.vendor);
    put ("pluginVersion", facts.pluginVersion);
    put ("appVersion",    facts.appVersion);
    put ("mapVersion",    facts.mapVersion);
    // Per control, off the ROW. Unset in v1, so ordinarily omitted.
    put ("mapKind",       row.mapKind);
    put ("mapUnit",       row.mapUnit);
    if (row.hasRange && std::isfinite (row.mapRangeMin) && std::isfinite (row.mapRangeMax))
    {
        o->setProperty ("mapRangeMin", row.mapRangeMin);
        o->setProperty ("mapRangeMax", row.mapRangeMax);
    }
    // extractorVersion and humanVerified live in plugin:<fp>:meta on the
    // server and are NOT available in the client. Deliberately never sent:
    // inventing them would put a wrong belief in front of the person fixing
    // the map, which is worse than a null.
    // note is OPTIONAL on a misdial and welcome: the five fields say what
    // happened, the user's words say why it looked wrong.
    if (note.trim().isNotEmpty())
        o->setProperty ("note", note.trim().substring (0, kMisdialNoteMax));
    o->setProperty ("kind", kMisdialKindMisdial());
    if (category.trim().isNotEmpty())
        o->setProperty ("category", category.trim().toLowerCase());
    put ("reportId", row.reportId);
    put ("source",   source);

    return juce::JSON::toString (juce::var (o.get()), true);
}

/** THE BUG KIND. The user's words and whatever context happens to be true, and
    NOTHING ELSE REQUIRED (api/_misdials.js:179, REQUIRED_BUG = ['note']).

    IT MUST NOT CARRY fp, and that is not merely allowed to be absent. The route
    forces fp to null on this kind, and bug records live in their own family
    grouped by plugin name rather than in the by-fingerprint work list. A bug
    body carrying an fp would be a client asserting a key the record does not
    have, and the one thing worth protecting is that the fingerprint queue only
    ever holds defects with a map at the end of them.

    This is the ONLY report a built-in device can produce: a built-in has no
    fingerprint and no parameter map by construction, so there is no map defect
    to describe and no map to open. Returns empty when the note is empty, which
    is the route's one requirement for this kind. */
inline juce::String buildBugBody (const juce::String& note,
                                 const MisdialSlotFacts& facts,
                                 const juce::String& source,
                                 const juce::String& reportId,
                                 const juce::String& category = {})
{
    const auto n = note.trim();
    if (n.isEmpty()) return {};
    // Same clash guard, the other way: wrong_control files a misdial and is
    // never a bug body, and an unknown category is never sent at all.
    if (category.trim().isNotEmpty()
        && misdialKindForCategory (category) != kMisdialKindBug())
        return {};

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("kind", kMisdialKindBug());
    o->setProperty ("note", n.substring (0, kMisdialNoteMax));
    if (category.trim().isNotEmpty())
        o->setProperty ("category", category.trim().toLowerCase());

    auto put = [&o] (const char* key, const juce::String& v)
    {
        if (v.trim().isNotEmpty()) o->setProperty (key, v.trim());
    };
    put ("pluginName",    facts.pluginName);
    put ("format",        facts.format);
    put ("vendor",        facts.vendor);
    put ("pluginVersion", facts.pluginVersion);
    put ("appVersion",    facts.appVersion);
    put ("mapVersion",    facts.mapVersion);
    put ("reportId",      reportId);
    put ("source",        source);
    // No fp, no parameterName, no parameterIndex, no valueDialled, no
    // observedResult. Deliberate, and asserted by the gate.
    //
    // AND NO uid. The server resolves it from the session and overwrites
    // anything the body carries (api/_misdials.js:247-249), because a client
    // that could set it could file under somebody else's account. Sending one
    // would be a client asserting a fact it does not own.
    return juce::JSON::toString (juce::var (o.get()), true);
}

// ---------------------------------------------------------------------------
// PERSISTENCE. The rows live on the CHAT MESSAGE, so they survive a rack change
// and a reload. Serialised here rather than in the editor so the round-trip is
// exercised by the gate: WsMessage carries the string, workspace_roundtrip_test
// asserts it survives, and the `reported` flag rides with it exactly as
// WsMessage::gainJson carries its applied state.
//
// reportId IS PERSISTED. A retry after a reload must reuse the id or the server
// files a second report for the same defect, which is the one thing dedupe
// exists to stop.
// ---------------------------------------------------------------------------

// CURRENTLY UNUSED, and left in deliberately. These were the message-level
// persistence for the rows, which is gone: the rows are owned by the slot now
// (ChainSlot::misdialRows) and the button reads them live, so nothing
// serialises them. They stay because they are the natural shape if rows ever
// need to survive a reload, and because they are known good. Do not go hunting
// for a caller: there is none.
inline juce::String misdialRowsToJson (const std::vector<MisdialRow>& rows)
{
    if (rows.empty()) return {};
    juce::Array<juce::var> arr;
    for (const auto& r : rows)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("fp",   r.fp);
        o->setProperty ("k",    r.mapKey);
        o->setProperty ("i",    r.index);
        o->setProperty ("v",    r.valueDialled);
        o->setProperty ("hv",   r.hasValue);
        if (r.landedText.isNotEmpty()) o->setProperty ("l",  r.landedText);
        if (r.outcome.isNotEmpty())    o->setProperty ("o",  r.outcome);
        if (r.mapKind.isNotEmpty())    o->setProperty ("mk", r.mapKind);
        if (r.mapUnit.isNotEmpty())    o->setProperty ("mu", r.mapUnit);
        if (r.hasRange)
        {
            o->setProperty ("rmin", r.mapRangeMin);
            o->setProperty ("rmax", r.mapRangeMax);
        }
        if (r.reported)                o->setProperty ("rep", true);
        if (r.reportId.isNotEmpty())   o->setProperty ("rid", r.reportId);
        arr.add (juce::var (o.get()));
    }
    return juce::JSON::toString (juce::var (arr), true);
}

inline std::vector<MisdialRow> misdialRowsFromJson (const juce::String& json)
{
    std::vector<MisdialRow> out;
    if (json.trim().isEmpty()) return out;
    auto v = juce::JSON::parse (json);
    if (auto* a = v.getArray())
        for (auto& e : *a)
        {
            if (auto* o = e.getDynamicObject())
            {
                MisdialRow r;
                r.fp           = o->getProperty ("fp").toString();
                r.mapKey       = o->getProperty ("k").toString();
                r.index        = (int) o->getProperty ("i");
                r.valueDialled = (double) o->getProperty ("v");
                r.hasValue     = (bool) o->getProperty ("hv");
                r.landedText   = o->getProperty ("l").toString();
                r.outcome      = o->getProperty ("o").toString();
                r.mapKind      = o->getProperty ("mk").toString();
                r.mapUnit      = o->getProperty ("mu").toString();
                if (o->hasProperty ("rmin") && o->hasProperty ("rmax"))
                {
                    r.mapRangeMin = (double) o->getProperty ("rmin");
                    r.mapRangeMax = (double) o->getProperty ("rmax");
                    r.hasRange = true;
                }
                r.reported = (bool) o->getProperty ("rep");
                r.reportId = o->getProperty ("rid").toString();
                out.push_back (r);
            }
        }
    return out;
}

/** True when this message has at least one row worth offering. THE BUTTON'S
    OWN CONDITION: never a dead button, so it appears only when this is true. */
inline bool misdialAnyReportable (const std::vector<MisdialRow>& rows)
{
    for (const auto& r : rows)
        if (misdialRowIsReportable (r)) return true;
    return false;
}

/** What the popup row shows: the map key, the value asked for, and what
    landed. One author, so the line the user chooses from and the record that is
    sent cannot describe different things. */
inline juce::String misdialRowLabel (const MisdialRow& r)
{
    juce::String s;
    s << r.mapKey;
    if (r.index >= 0) s << " [" << r.index << "]";
    s << ": asked " << juce::String (r.valueDialled, 2);
    const auto landed = r.landedText.trim();
    if (landed.isNotEmpty()) s << ", landed " << landed;
    else if (r.outcome.trim().isNotEmpty()) s << ", " << r.outcome.trim();
    return s;
}

} // namespace echojay
