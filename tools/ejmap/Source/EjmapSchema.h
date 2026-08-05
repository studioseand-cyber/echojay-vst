/*
  EjmapSchema.h

  Map payload v2.1 types, shared by ejmap and by the round-trip test.

  Design rules encoded here, on purpose:

    1. ProbeVerdict has three values, not two, and does not convert to bool.
       An inconclusive probe must never read as a pass anywhere in the codebase.
    2. SkipRecord requires a reason. A skip is a recorded fact, never an absence.
    3. Trust is per key, never per map. There is no map-level trust field.
    4. Environment travels in Provenance, never in Identity.

  kMapSchemaVersion is the single source of truth for both binaries. Any change
  to the wire format bumps it, and the round-trip test fails until both sides
  agree.
*/

#pragma once

#include <juce_core/juce_core.h>

// The schema version is NOT defined here. It lives in EchoJayParamApply.h, the
// header that actually applies a map, and is pulled in below. There is exactly
// one definition and both binaries compile it.
#include "EchoJayParamApply.h"

namespace ejmap
{

//==============================================================================
// No local definition: these are the constants EchoJay's apply path uses, by
// name, not by copy.
using echojay::kMapSchemaVersion;
using echojay::kMapSchemaString;

// ejmap's side of the drift guard. The payload writer below emits the v2.1
// field set. If the shared constant moves, this fails and the writer gets read
// before it ships maps EchoJay would parse differently. Bumping the wire format
// means changing this pin deliberately, in the same commit as the writer.
static_assert (kMapSchemaVersion == 23,
               "EjmapSchema.h's payload writer targets schema 2.2. The shared "
               "kMapSchemaVersion in Source/EchoJayParamApply.h has moved: update "
               "the writer, then this pin.");

//==============================================================================
enum class Mode { fast, deep, repair };

/** rule-built < llm-classified < setread < human-verified < admin-approved.
    Ordering is meaningful: comparison is used by the merge-per-key rule.
*/
enum class Trust { ruleBuilt = 0, llmClassified = 1, setread = 2, humanVerified = 3, adminApproved = 4 };

enum class AnchorMethod { gettext, setread, humanTyped };

/** modeMaterial is a RESOLUTION, not an absence: the sweep proved the control
    exists and is discrete, so recording notPresent would be a falsehood and
    deferred would lose the finding. The map carries it as its own outcome and
    the Tier 2 breadcrumb carries the labels.
*/
enum class SkipOutcome { notPresent, notAutomatable, deferred, modeMaterial };

enum class ProbeVerdict { confirms, contradicts, inconclusive };

enum class CaptureSource { poll, listener, both };

/** noTypes is the scan-probe outcome: the format opened the file and it yielded
    no plugin descriptions. Name taken from ejextract's existing status
    vocabulary, where "no-types" is already a terminal result, so the two tools
    describe the same event with the same word.

    LoadOutcome is NOT part of the map payload. It appears only in the ledger and
    in PluginHost::LoadResult, so extending it does not touch the wire format and
    needs no kMapSchemaVersion bump.

    diedDuringLoad IS NOT crash_on_load, and the rename is the point. A SIGKILL
    and a SIGSEGV leave IDENTICAL evidence -- an inflight stake with no
    completion -- so the old name asserted a cause the evidence does not carry.
    An operator force-quitting a slow load produced rows saying the plugin
    crashed. The row now says what was observed (the process died while this
    plugin was loading) and carries a separate `certainty` field for whether a
    crash report corroborates it.

    Rows written before 5 Aug 2026 say "crash_on_load". kLegacyDeathOutcome is
    that string, and every counter matches BOTH: 28 historic death rows are the
    only determinism evidence this project has, and a rename that silently
    dropped them would reset every retry count to zero.
*/
enum class LoadOutcome { ok, diedDuringLoad, timeout, initFailed, licenseRefused, noEditor, noParams, quarantined, noTypes, restarted };

inline constexpr const char* kLegacyDeathOutcome = "crash_on_load";

//==============================================================================
inline juce::String toString (Mode m)
{
    switch (m) { case Mode::fast: return "fast"; case Mode::deep: return "deep"; case Mode::repair: return "repair"; }
    return "fast";
}

inline juce::String toString (Trust t)
{
    switch (t)
    {
        case Trust::ruleBuilt:      return "rule-built";
        case Trust::llmClassified:  return "llm-classified";
        case Trust::setread:        return "setread";
        case Trust::humanVerified:  return "human-verified";
        case Trust::adminApproved:  return "admin-approved";
    }
    return "rule-built";
}

inline juce::String toString (AnchorMethod m)
{
    switch (m) { case AnchorMethod::gettext: return "gettext"; case AnchorMethod::setread: return "setread"; case AnchorMethod::humanTyped: return "human-typed"; }
    return "setread";
}

inline juce::String toString (SkipOutcome o)
{
    switch (o) { case SkipOutcome::notPresent: return "not_present"; case SkipOutcome::notAutomatable: return "not_automatable"; case SkipOutcome::deferred: return "deferred"; case SkipOutcome::modeMaterial: return "mode_material"; }
    return "deferred";
}

inline juce::String toString (ProbeVerdict v)
{
    switch (v) { case ProbeVerdict::confirms: return "confirms"; case ProbeVerdict::contradicts: return "contradicts"; case ProbeVerdict::inconclusive: return "inconclusive"; }
    return "inconclusive";
}

inline juce::String toString (CaptureSource c)
{
    switch (c) { case CaptureSource::poll: return "poll"; case CaptureSource::listener: return "listener"; case CaptureSource::both: return "both"; }
    return "poll";
}

inline juce::String toString (LoadOutcome o)
{
    switch (o)
    {
        case LoadOutcome::ok:             return "ok";
        case LoadOutcome::diedDuringLoad: return "died_during_load";
        case LoadOutcome::timeout:        return "timeout";
        case LoadOutcome::initFailed:     return "init_failed";
        case LoadOutcome::licenseRefused: return "license_refused";
        case LoadOutcome::noEditor:       return "no_editor";
        case LoadOutcome::noParams:       return "no_params";
        case LoadOutcome::quarantined:    return "quarantined";
        case LoadOutcome::noTypes:        return "no_types";
        case LoadOutcome::restarted:      return "restarted";
    }
    return "timeout";
}

/** The only place in the codebase permitted to decide what a verdict means for
    submission. Nothing else should branch on ProbeVerdict directly.
*/
inline bool blocksSubmit (ProbeVerdict v)  { return v == ProbeVerdict::contradicts; }
inline bool countsAsPass  (ProbeVerdict v) { return v == ProbeVerdict::confirms; }

//==============================================================================
/** Normalised to editor bounds, 0..1. Null when the mouse was outside the editor
    at capture time (host automation, MIDI learn, typed entry). Recording null is
    correct; inventing a coordinate is not.
*/
struct UiHint
{
    bool  valid = false;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // normalised 0..1

    /** THE DENOMINATOR, carried with the value. Schema 2.2.

        x/y/w/h are fractions of the editor. Without the bounds they were
        divided by, a hint cannot be checked: a bridged editor reaches its real
        size about 2.5 s after createEditorIfNeeded, and a resizable GUI can
        resize mid-session, so a stored fraction may refer to an editor that no
        longer exists at that size. A reader that finds these disagreeing with
        the live editor should decline to place the label rather than place it
        wrongly.
    */
    int editorWidth = 0, editorHeight = 0;

    /** Which display the coordinate was measured on, and at what scale. Two
        displays at different scale factors are normal, and M1's own failure list
        already includes editors opening on a second display.
    */
    juce::String screen;

    juce::var toVar() const
    {
        if (! valid) return juce::var();
        auto* o = new juce::DynamicObject();
        o->setProperty ("x", x); o->setProperty ("y", y);
        o->setProperty ("w", w); o->setProperty ("h", h);
        o->setProperty ("editor_w", editorWidth);
        o->setProperty ("editor_h", editorHeight);
        o->setProperty ("screen", screen);
        return juce::var (o);
    }
};

struct AnchorPoint
{
    double normalised = 0.0;   // 0..1 as written
    double value      = 0.0;   // real-world value as displayed
};

//==============================================================================
/** THE READBACK PROBE, in one place.

    The old rule asked for the MIDPOINT of the two middle anchors and allowed
    60% of that same gap. On a quantised parameter the plugin can only land on
    one of those two anchors, and both are 50% away, so both passed: the check
    could not fail. Measured on Dangerous BAX EQ Master -- anchors
    18000/12600, ask 15300, landed 12600, miss 2700, tolerance 3240, verified.
    A check that cannot fail is not a check.

    Three changes, and each has a reason:

    1. ASK OFF-CENTRE, at 25% of the gap. The nearest step is then unambiguous
       -- 25% away against 75% -- so "it landed on the nearest step" becomes a
       statement with a false case.
    2. LANDING ON THE FAR ANCHOR IS A FAILURE, stated rather than left to
       arithmetic. This is the failure mode the old rule could not express.
    3. PROPORTIONAL TOLERANCE FOR FREQUENCY. A fraction of the anchor span is
       backwards for Hz: 2% of a 7.5k-70k span is 1250 Hz, which is 17% at the
       bottom of the range and 1.8% at the top, while the error that matters to
       an ear is proportional. Frequency gets 3% of the asked value.

    A quantised parameter still passes, by landing on the NEAREST anchor. A
    continuous one still passes, by landing near the ask. What no longer passes
    is landing on the wrong one of the two.
*/
struct ReadbackProbe
{
    bool   valid   = false;
    double ask     = 0.0;   // what to write
    double nearest = 0.0;   // the step a quantised parameter should snap to
    double far     = 0.0;   // the other anchor: landing here is a failure
    double tol     = 0.0;

    /** The single decision. Kept beside the plan so the two can never drift:
        one function builds the question, the same struct answers it.
    */
    bool matches (double landed) const
    {
        if (! valid) return false;
        // Landing closer to the anchor we deliberately asked AWAY from is a
        // fail whatever the tolerance says.
        if (std::abs (landed - far) < std::abs (landed - nearest)) return false;
        return std::abs (landed - ask) <= tol || std::abs (landed - nearest) <= tol;
    }
};

inline ReadbackProbe planReadback (const juce::Array<juce::Array<float>>& anchors,
                                   const juce::String& semantic)
{
    ReadbackProbe p;
    if (anchors.size() < 3) return p;              // as before: needs a middle

    const int mid = anchors.size() / 2;
    const double a = anchors[mid - 1][0], b = anchors[mid][0];
    if (std::abs (b - a) < 1.0e-9) return p;       // a degenerate pair decides nothing

    p.nearest = a;
    p.far     = b;
    p.ask     = a + 0.25 * (b - a);

    double lo = anchors.getFirst()[0], hi = lo;
    for (const auto& an : anchors) { lo = juce::jmin (lo, (double) an[0]); hi = juce::jmax (hi, (double) an[0]); }

    if (echojay::semanticUnit (semantic) == "hz")
        p.tol = juce::jmax (0.03 * std::abs (p.ask), 0.5);   // proportional, with a floor
    else
        p.tol = juce::jmax (0.02 * (hi - lo), 0.05);

    p.valid = true;
    return p;
}

//==============================================================================
struct ParamMapping
{
    juce::String semantic;                  // "freq_hz", "threshold_db"
    juce::Array<int> indices;               // one entry, or several for channel twins
    juce::String paramName;                 // real name, CASE SENSITIVE
    juce::String kind;                      // typed vocabulary entry
    juce::Array<AnchorPoint> anchors;
    bool anchorsReversed = false;
    Trust trust = Trust::setread;
    AnchorMethod method = AnchorMethod::setread;
    UiHint uiHint;
    CaptureSource capturedBy = CaptureSource::poll;

    /** Set only when method is humanTyped: what the sweeper OBSERVED that made
        typing necessary ("display flat through both paths", "identity
        display", "display is labels, not numbers"). Empty on every other
        method.

        The artefact recorded the method and not the reason, so nothing
        downstream could tell a table typed because the plugin has no readable
        display from one typed by preference. A gate that means to accept the
        first and refuse the second needs this field to exist before it can be
        written (queue item 12).
    */
    juce::String typedReason;

    /** The unit family the SWEEP measured on this control's display ("db",
        "hz", "ms", "pct", "s"), empty when the display declared none. Recorded
        so the unit-family rule can be checked from a map alone rather than
        reconstructed from readback strings.
    */
    juce::String unitFamily;

    juce::var toVar() const;
};

/** Tier 2. Any parameter the human or the sweep reached that has no typed kind.
    No ceiling, no validator entry, no category-bar effect.
*/
struct NamedControl
{
    juce::String name;                      // real name, CASE SENSITIVE
    juce::Array<int> indices;
    double rangeLo = 0.0, rangeHi = 0.0;

    /** DISPLAY-DECLARED unit (the M5 unit_family finding), never inferred
        from the name: "hz" because the sweep read "1.0 kHz", not because the
        name said Freq. Empty when the display carries none.
    */
    juce::String unit;
    juce::Array<AnchorPoint> anchors;

    /** "anchored" rides applyOne's anchor path; "mode" rides its EXISTING
        labels path -- built for served maps before M0 and never fed until
        Tier 2. A three-position knee switch lands here.
    */
    juce::String kind { "anchored" };

    struct LabelPoint { juce::String text; double norm = 0.0; };
    juce::Array<LabelPoint> labels;         // mode kind only

    /** The M3 finding, carried honestly: every display value equalled its own
        norm, so the display is likely fabricated. The control still dials by
        raw number in norm terms; the flag says what the numbers are.
    */
    bool identityDisplay = false;

    Trust trust = Trust::setread;
    bool duplicate = false;                 // same EXACT name twice: both indices
                                            // recorded here, neither resolvable

    /** Channel-duplicate marker (server contract, 2026-08-02). States the
        OBSERVATION -- this parameter moved 1:1 with the picked address during
        discovery -- never the interpretation, so it survives whatever shape
        the next vendor invents. The server excludes carriers from both the
        exposure pool and the inventory. lockstepBy says which evidence source
        made the observation ("human_pick" | "write_verify"); the two never
        blur.
    */
    int lockstepOf = -1;
    juce::String lockstepBy;

    /** The HUMAN tier field ("primary" | "hidden"); empty means the server's
        exposure heuristic decides. Set only by an explicit gesture on the
        exposure preview card; untouched controls emit nothing.
    */
    juce::String tier;

    juce::var toVar() const;
};

struct GroupSpec
{
    juce::String family;                    // "sband", "vband", or empty

    /** The band NUMBER within the family -- applyBands sorts on it and labels
        results "band <family><n>". One GroupSpec per band, an array of them
        per family. The first version of this comment said "band count",
        copied from the plan's own example, which wrote one group with n=5
        meaning five bands; applyBands would have read that as a single band
        numbered 5. The plan is corrected in the same commit. No served map
        has ever carried a group (measured: 0 of the corpus), so the consumer
        code IS the contract and the drift gate pins this writer to it.
    */
    int n = 0;

    /** The family the human actually touched. applyBands prefers it when a
        request names no family.
    */
    bool primary = false;
    juce::Array<ParamMapping> params;
    double freqLo = 0.0, freqHi = 0.0;

    /** THREE CLAIMS, RECORDED SEPARATELY (signed 4 Aug 2026). Merging any two
        of these lets the weakest borrow the credibility of the strongest.

        groupingSource -- WHO decided these indices form a band.
          "inferred"  the name pattern proposed it, every member swept
          "mapper"    a human said so. Stronger evidence for the GROUPING, and
                      it says nothing whatever about what the members do.

        orderingSource -- where the band ORDER came from. Always "derived":
          band order is sorted from frequency magnitudes, never from the order
          the mapper typed things in. Entry order becoming the claim is the
          failure this field exists to make visible.

        freqSource -- where the frequency NUMBERS came from.
          "swept"             measured off the plugin by the sweeper
          "typed_fixed"       transcribed from a fixed band's printed constant
                              (a graphic EQ: 31, 63, 125...). There is no
                              frequency parameter to sweep, and the number is a
                              constant, not a reading.
          "typed_parametric"  the mapper typed a MOVABLE control's value
                              instead of sweeping it. Weakest: nothing measured
                              it and it describes where a knob happened to sit.

        Empty means unstated, which is what every map written before this field
        existed says. Absent is not "swept".
    */
    juce::String groupingSource, orderingSource, freqSource;

    juce::var toVar() const;
};

struct SkipRecord
{
    juce::String semantic;
    SkipOutcome outcome = SkipOutcome::deferred;
    juce::String reason;                    // required, asserted on construction

    SkipRecord() = default;
    SkipRecord (juce::String s, SkipOutcome o, juce::String r)
        : semantic (std::move (s)), outcome (o), reason (std::move (r))
    {
        // A skip with no reason is the silent-drop class wearing a hat.
        jassert (reason.isNotEmpty());
    }

    juce::var toVar() const;
};

struct ProbeResult
{
    juce::String semantic;
    ProbeVerdict verdict = ProbeVerdict::inconclusive;
    juce::String measure;                   // "peak_freq_hz", "knee_db", "rt60_s"
    double expected = 0.0;
    double observed = 0.0;
    juce::String note;                      // why inconclusive, when it is

    juce::var toVar() const;
};

struct ReadbackResult
{
    juce::String semantic;
    juce::String asked;          // the real-world value requested ("-18 dB")
    juce::String wrote, read;    // normalised written, display read back
    bool match = false;

    juce::var toVar() const;
};

//==============================================================================
struct PluginIdentity
{
    juce::String format;                    // "AudioUnit" | "VST3"
    juce::String uid;
    juce::String name;
    juce::String vendor;
    juce::String version;                   // metadata only, never in the fp key
    int paramCount = 0;

    juce::var toVar() const;
};

struct Provenance
{
    juce::String testerId, machineId;

    /** The MAPPER, derived from the token they were issued: the first 12 hex of
        its SHA-256. Non-secret on purpose -- a map is stored, copied and read
        by other people, so a credential inside one would leak by being useful.
        The server issued the token, so it can resolve this back to a person;
        nobody else can. */
    juce::String mapperRef;
    juce::String ejmapVersion, extractorVersion;
    juce::String applyHeaderSha;            // pins which apply logic verified this
    juce::String pluginVersion, hostOs;
    juce::String at;                        // ISO 8601
    juce::String transferredFrom;           // set by Repair/regression transfer

    juce::var toVar() const;
};

struct Evidence
{
    juce::Array<int> noiseMask;             // self-changing indices, excluded from capture
    juce::Array<ReadbackResult> readback;
    juce::StringArray visualConfirmed;
    juce::Array<ProbeResult> audioProbe;
    juce::var rehearsal;                    // settings, applied, probe_delta
    juce::String labelledScreenshotSha, fullScreenshotSha;
    int assistTurns = 0;
    int durationSeconds = 0;

    juce::var toVar() const;
};

//==============================================================================
struct MapPayload
{
    juce::String fp;
    juce::String schema = kMapSchemaString;
    PluginIdentity identity;
    juce::String category;
    Mode mode = Mode::fast;

    juce::Array<ParamMapping>  params;      // Tier 1, typed
    juce::Array<NamedControl>  controls;    // Tier 2, named
    juce::Array<GroupSpec>     groups;
    juce::Array<SkipRecord>    skips;
    Evidence   evidence;
    Provenance provenance;

    juce::var toVar() const;
    juce::String toJson() const { return juce::JSON::toString (toVar(), false); }

    /** True when nothing in this payload is unresolved. The approval sheet and
        the upload gate both call this. It deliberately does NOT check that the
        category bar clears: a map that misses the bar is still a valid map.
    */
    bool hasUnresolvedContradiction() const
    {
        for (const auto& p : evidence.audioProbe)
            if (blocksSubmit (p.verdict))
                return true;
        return false;
    }
};

//==============================================================================
// Inline implementations
//==============================================================================
namespace detail
{
    inline juce::var anchorsToVar (const juce::Array<AnchorPoint>& a)
    {
        juce::Array<juce::var> out;
        for (const auto& p : a)
        {
            // [value, normalised], NOT [normalised, value]. This is the order
            // the served maps emit and the order EchoJayParamApply's
            // anchorsFromVar/interpolateAnchors consume. The first version of
            // this function had it reversed and no consumer had ever read a
            // payload this writer produced, so it sat dormant until the M4
            // submit path was about to ship it; the drift gate now round-trips
            // this writer through the real apply path so the order cannot
            // silently flip again.
            juce::Array<juce::var> pair;
            pair.add (p.value);
            pair.add (p.normalised);
            out.add (juce::var (pair));
        }
        return juce::var (out);
    }

    inline juce::var indicesToVar (const juce::Array<int>& idx)
    {
        juce::Array<juce::var> out;
        for (auto i : idx) out.add (i);
        return juce::var (out);
    }
}

inline juce::var ParamMapping::toVar() const
{
    auto* o = new juce::DynamicObject();
    if (indices.size() == 1) o->setProperty ("index", indices[0]);
    else                     o->setProperty ("indices", detail::indicesToVar (indices));
    // "name", not "param_name": groupIsEqBand's imposter guard reads
    // entry.name, and with no served grouped map in existence the consumer
    // code is the contract. A writer emitting a key nobody reads would leave
    // that guard running blind on every ejmap-built map.
    o->setProperty ("name", paramName);
    o->setProperty ("kind", kind);
    o->setProperty ("anchors", detail::anchorsToVar (anchors));
    if (anchorsReversed) o->setProperty ("anchors_reversed", true);
    o->setProperty ("trust", toString (trust));
    o->setProperty ("method", toString (method));
    // Only where it means something. An empty typed_reason on a swept
    // parameter would read as "no reason given" rather than "not applicable".
    if (method == AnchorMethod::humanTyped && typedReason.isNotEmpty())
        o->setProperty ("typed_reason", typedReason);
    if (unitFamily.isNotEmpty()) o->setProperty ("unit", unitFamily);
    o->setProperty ("ui_hint", uiHint.toVar());
    o->setProperty ("captured_by", toString (capturedBy));
    return juce::var (o);
}

inline juce::var NamedControl::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", name);          // also inside: tooling reads entries alone
    if (indices.size() == 1) o->setProperty ("index", indices[0]);
    else                     o->setProperty ("indices", detail::indicesToVar (indices));
    o->setProperty ("kind", kind);
    juce::Array<juce::var> range; range.add (rangeLo); range.add (rangeHi);
    o->setProperty ("range", juce::var (range));
    o->setProperty ("unit", unit.isEmpty() ? juce::var() : juce::var (unit));
    if (kind == "mode")
    {
        // The shape applyOne's mode path has consumed since before M0:
        // labels: { "<display text>": <norm> }.
        auto* lo = new juce::DynamicObject();
        for (const auto& l : labels) lo->setProperty (l.text, l.norm);
        o->setProperty ("labels", juce::var (lo));
        o->setProperty ("caseInsensitiveOk", true);
    }
    else
        o->setProperty ("anchors", detail::anchorsToVar (anchors));
    if (identityDisplay) o->setProperty ("identity_display", true);
    o->setProperty ("trust", toString (trust));
    if (duplicate) o->setProperty ("duplicate", true);
    if (lockstepOf >= 0)
    {
        o->setProperty ("lockstep_of", lockstepOf);
        o->setProperty ("lockstep_by", lockstepBy);
    }
    if (tier == "primary" || tier == "hidden")
        o->setProperty ("tier", tier);
    return juce::var (o);
}

inline juce::var GroupSpec::toVar() const
{
    auto* o = new juce::DynamicObject();
    if (family.isNotEmpty()) o->setProperty ("family", family);
    o->setProperty ("n", n);
    if (primary) o->setProperty ("primary", true);
    auto* p = new juce::DynamicObject();
    for (const auto& m : params) p->setProperty (m.semantic, m.toVar());
    o->setProperty ("params", juce::var (p));
    juce::Array<juce::var> fr; fr.add (freqLo); fr.add (freqHi);
    o->setProperty ("freq_range", juce::var (fr));
    // Emitted only when stated. A group written before these existed says
    // nothing about them, and an absent field must keep reading as unstated
    // rather than acquiring a default the writer never claimed.
    if (groupingSource.isNotEmpty()) o->setProperty ("grouping_source", groupingSource);
    if (orderingSource.isNotEmpty()) o->setProperty ("ordering_source", orderingSource);
    if (freqSource.isNotEmpty())     o->setProperty ("freq_source", freqSource);
    return juce::var (o);
}

inline juce::var SkipRecord::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("semantic", semantic);
    o->setProperty ("outcome", toString (outcome));
    o->setProperty ("reason", reason);
    return juce::var (o);
}

inline juce::var ProbeResult::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("verdict", toString (verdict));
    o->setProperty ("measure", measure);
    o->setProperty ("expected", expected);
    o->setProperty ("observed", observed);
    o->setProperty ("note", note.isEmpty() ? juce::var() : juce::var (note));
    return juce::var (o);
}

inline juce::var ReadbackResult::toVar() const
{
    auto* o = new juce::DynamicObject();
    if (asked.isNotEmpty()) o->setProperty ("asked", asked);
    o->setProperty ("wrote", wrote);
    o->setProperty ("read", read);
    o->setProperty ("match", match);
    return juce::var (o);
}

inline juce::var PluginIdentity::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", format);
    o->setProperty ("uid", uid);
    o->setProperty ("name", name);
    o->setProperty ("vendor", vendor);
    o->setProperty ("version", version);
    o->setProperty ("param_count", paramCount);
    return juce::var (o);
}

inline juce::var Provenance::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("tester_id", testerId);
    if (mapperRef.isNotEmpty()) o->setProperty ("mapper_ref", mapperRef);
    o->setProperty ("machine_id", machineId);
    o->setProperty ("ejmap_version", ejmapVersion);
    o->setProperty ("extractor_version", extractorVersion);
    o->setProperty ("apply_header_sha", applyHeaderSha);
    o->setProperty ("plugin_version", pluginVersion);
    o->setProperty ("host_os", hostOs);
    o->setProperty ("at", at);
    if (transferredFrom.isNotEmpty()) o->setProperty ("transferred_from", transferredFrom);
    return juce::var (o);
}

inline juce::var Evidence::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("noise_mask", detail::indicesToVar (noiseMask));

    auto* rb = new juce::DynamicObject();
    for (const auto& r : readback) rb->setProperty (r.semantic, r.toVar());
    o->setProperty ("readback", juce::var (rb));

    juce::Array<juce::var> vc;
    for (const auto& s : visualConfirmed) vc.add (s);
    o->setProperty ("visual_confirmed", juce::var (vc));

    auto* ap = new juce::DynamicObject();
    for (const auto& p : audioProbe) ap->setProperty (p.semantic, p.toVar());
    o->setProperty ("audio_probe", juce::var (ap));

    o->setProperty ("rehearsal", rehearsal);

    auto* ss = new juce::DynamicObject();
    ss->setProperty ("labelled", labelledScreenshotSha);
    ss->setProperty ("full", fullScreenshotSha);
    o->setProperty ("screenshots", juce::var (ss));

    o->setProperty ("assist_turns", assistTurns);
    o->setProperty ("duration_s", durationSeconds);
    return juce::var (o);
}

inline juce::var MapPayload::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("fp", fp);
    o->setProperty ("schema", schema);
    o->setProperty ("identity", identity.toVar());
    o->setProperty ("category", category);
    o->setProperty ("mode", toString (mode));

    auto* p = new juce::DynamicObject();
    for (const auto& m : params) p->setProperty (m.semantic, m.toVar());
    o->setProperty ("params", juce::var (p));

    auto* c = new juce::DynamicObject();
    for (const auto& n : controls) c->setProperty (n.name, n.toVar());
    o->setProperty ("controls", juce::var (c));

    juce::Array<juce::var> g;
    for (const auto& s : groups) g.add (s.toVar());
    o->setProperty ("groups", juce::var (g));

    juce::Array<juce::var> sk;
    for (const auto& s : skips) sk.add (s.toVar());
    o->setProperty ("skips", juce::var (sk));

    o->setProperty ("evidence", evidence.toVar());
    o->setProperty ("provenance", provenance.toVar());
    return juce::var (o);
}

} // namespace ejmap
