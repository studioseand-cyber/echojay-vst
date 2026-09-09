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
                                     const juce::String& source)
{
    if (! misdialRowIsReportable (row)) return {};

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
    put ("reportId", row.reportId);
    put ("source",   source);

    return juce::JSON::toString (juce::var (o.get()), true);
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
