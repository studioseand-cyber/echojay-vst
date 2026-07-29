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
static_assert (kMapSchemaVersion == 21,
               "EjmapSchema.h's payload writer targets schema 2.1. The shared "
               "kMapSchemaVersion in Source/EchoJayParamApply.h has moved: update "
               "the writer, then this pin.");

//==============================================================================
enum class Mode { fast, deep, repair };

/** rule-built < llm-classified < setread < human-verified < admin-approved.
    Ordering is meaningful: comparison is used by the merge-per-key rule.
*/
enum class Trust { ruleBuilt = 0, llmClassified = 1, setread = 2, humanVerified = 3, adminApproved = 4 };

enum class AnchorMethod { gettext, setread, humanTyped };

enum class SkipOutcome { notPresent, notAutomatable, deferred };

enum class ProbeVerdict { confirms, contradicts, inconclusive };

enum class CaptureSource { poll, listener, both };

/** noTypes is the scan-probe outcome: the format opened the file and it yielded
    no plugin descriptions. Name taken from ejextract's existing status
    vocabulary, where "no-types" is already a terminal result, so the two tools
    describe the same event with the same word.

    LoadOutcome is NOT part of the map payload. It appears only in the ledger and
    in PluginHost::LoadResult, so extending it does not touch the wire format and
    needs no kMapSchemaVersion bump.
*/
enum class LoadOutcome { ok, crashOnLoad, timeout, licenseRefused, noEditor, noParams, quarantined, noTypes };

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
    switch (o) { case SkipOutcome::notPresent: return "not_present"; case SkipOutcome::notAutomatable: return "not_automatable"; case SkipOutcome::deferred: return "deferred"; }
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
        case LoadOutcome::crashOnLoad:    return "crash_on_load";
        case LoadOutcome::timeout:        return "timeout";
        case LoadOutcome::licenseRefused: return "license_refused";
        case LoadOutcome::noEditor:       return "no_editor";
        case LoadOutcome::noParams:       return "no_params";
        case LoadOutcome::quarantined:    return "quarantined";
        case LoadOutcome::noTypes:        return "no_types";
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
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    juce::var toVar() const
    {
        if (! valid) return juce::var();
        auto* o = new juce::DynamicObject();
        o->setProperty ("x", x); o->setProperty ("y", y);
        o->setProperty ("w", w); o->setProperty ("h", h);
        return juce::var (o);
    }
};

struct AnchorPoint
{
    double normalised = 0.0;   // 0..1 as written
    double value      = 0.0;   // real-world value as displayed
};

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
    juce::String unit;                      // empty when the parser found none
    juce::Array<AnchorPoint> anchors;
    Trust trust = Trust::setread;
    bool duplicate = false;                 // same name appears twice: unresolvable

    juce::var toVar() const;
};

struct GroupSpec
{
    juce::String family;                    // "sband", "vband", or empty
    int n = 0;                              // band count
    juce::Array<ParamMapping> params;
    double freqLo = 0.0, freqHi = 0.0;

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
    juce::String wrote, read;
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
            juce::Array<juce::var> pair;
            pair.add (p.normalised);
            pair.add (p.value);
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
    o->setProperty ("param_name", paramName);
    o->setProperty ("kind", kind);
    o->setProperty ("anchors", detail::anchorsToVar (anchors));
    if (anchorsReversed) o->setProperty ("anchors_reversed", true);
    o->setProperty ("trust", toString (trust));
    o->setProperty ("method", toString (method));
    o->setProperty ("ui_hint", uiHint.toVar());
    o->setProperty ("captured_by", toString (capturedBy));
    return juce::var (o);
}

inline juce::var NamedControl::toVar() const
{
    auto* o = new juce::DynamicObject();
    if (indices.size() == 1) o->setProperty ("index", indices[0]);
    else                     o->setProperty ("indices", detail::indicesToVar (indices));
    juce::Array<juce::var> range; range.add (rangeLo); range.add (rangeHi);
    o->setProperty ("range", juce::var (range));
    o->setProperty ("unit", unit.isEmpty() ? juce::var() : juce::var (unit));
    o->setProperty ("anchors", detail::anchorsToVar (anchors));
    o->setProperty ("trust", toString (trust));
    if (duplicate) o->setProperty ("duplicate", true);
    return juce::var (o);
}

inline juce::var GroupSpec::toVar() const
{
    auto* o = new juce::DynamicObject();
    if (family.isNotEmpty()) o->setProperty ("family", family);
    o->setProperty ("n", n);
    auto* p = new juce::DynamicObject();
    for (const auto& m : params) p->setProperty (m.semantic, m.toVar());
    o->setProperty ("params", juce::var (p));
    juce::Array<juce::var> fr; fr.add (freqLo); fr.add (freqHi);
    o->setProperty ("freq_range", juce::var (fr));
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
