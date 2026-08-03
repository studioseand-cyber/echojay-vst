/*
  ==============================================================================

    EjmapSubject.h — what a parameterised M9 suite reads from a map

    M9 PARAMETERISATION, ITEM 0 (shared machinery). Nothing here measures
    anything; it answers "which index, which ladder, which band, and what must
    be set before the effect exists" from the MAP rather than from constants
    compiled into a suite.

    WHY THIS EXISTS, which is the justification for the whole milestone:
    today a suite sweeps the plugin live and compares a rendered feature
    against the ladder it just read from that same plugin. That is a
    display-versus-render self-consistency check. Reading the ladder from the
    map instead asks whether the plugin does what THE MAP CLAIMS. The two
    diverge exactly when the map is stale, was made in another mode or preset,
    or was made against another version -- which is the case M9 exists to
    catch and cannot currently see.

    EVERY LOOKUP HERE REFUSES RATHER THAN ASSUMES. A missing semantic, an
    ambiguous group, a ladder too short to interpolate: each returns not-ok
    with a reason in words, because the fixture constants being replaced were
    silent assumptions and replacing them with silent defaults would move the
    problem rather than fix it. `groups[0]` is the specimen: it looks like a
    selector and is really an assumption that band 1 is the interesting one.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include "../../../Source/EchoJayParamApply.h"

namespace ejmap
{
namespace subject
{

//==============================================================================
/** One addressable parameter in a map: where it is, what drives it, and -- when
    it cannot be used -- why not, in words a verdict line can carry.
*/
struct SlotRef
{
    bool         found = false;
    int          index = -1;
    juce::String semantic, name, unit, kind;
    juce::String where;                          // "params / ratio", "group 1 / freq_hz"
    juce::String why;                            // refusal reason when ! ok()
    juce::Array<juce::Array<float>> anchors;

    /** Usable as a probe target: located, addressable, and carrying a ladder
        with at least two points (one point cannot express a move, and a suite
        that writes it measures its own stimulus). */
    bool ok() const { return found && index >= 0 && anchors.size() >= 2; }

    double ladderLo() const { return anchors.isEmpty() ? 0.0 : (double) anchors.getFirst()[0]; }
    double ladderHi() const { return anchors.isEmpty() ? 0.0 : (double) anchors.getLast()[0]; }
    double ladderSpan() const { return std::abs (ladderHi() - ladderLo()); }

    /** Normalised value for a semantic value, through the SAME interpolation
        the dial path uses -- never a private copy. A probe that wrote through
        different arithmetic from the consumer would be testing itself. */
    float normFor (double value) const
    {
        auto eff = echojay::dominantMonotonicTable (anchors);
        return echojay::interpolateAnchors (eff.table, (float) value);
    }

    // No predictedLanding wrapper here on purpose: it lives on the measurement
    // side (Probe::predictedLanding) and suites call it with `anchors`. A
    // convenience copy would be a second implementation of ladder arithmetic,
    // and the two would drift.
};

//==============================================================================
namespace detail
{
    inline SlotRef fromEntry (const juce::var& entry, const juce::String& semantic,
                              const juce::String& where)
    {
        SlotRef s;
        s.semantic = semantic;
        s.where    = where;
        if (! entry.isObject())
        { s.why = "no '" + semantic + "' in " + where; return s; }

        s.found = true;
        s.index = (int) entry.getProperty ("index", -1);
        s.name  = entry.getProperty ("name", "").toString();
        s.unit  = entry.getProperty ("unit", "").toString();
        s.kind  = entry.getProperty ("kind", "").toString();
        s.anchors = echojay::anchorsFromVar (entry);

        if (s.index < 0)
            s.why = where + " carries no parameter index";
        else if (s.anchors.size() < 2)
            s.why = where + " has " + juce::String (s.anchors.size())
                  + " anchor(s); a ladder needs at least 2 to express a move";
        return s;
    }
}

//==============================================================================
/** The one group a suite should probe: the group flagged `primary`.

    REFUSES on none and on more than one. The constant this replaces is eq's
    `groups[0]`, which silently means "band 1 is the interesting one" -- true
    of the AMEK fixture, an assumption everywhere else. A map with no primary
    band has not answered the question, and picking for it would fabricate the
    answer.

    Returns the INDEX INTO `groups[]`, not the group's `n`: `n` is the
    plugin's band numbering and the two are only incidentally equal.
*/
struct GroupPick
{
    bool ok = false;
    int  arrayIndex = -1;
    int  n = -1;
    juce::String why;
};

inline GroupPick primaryGroup (const juce::var& mapVar)
{
    GroupPick g;
    auto* arr = mapVar.getProperty ("groups", juce::var()).getArray();
    if (arr == nullptr || arr->isEmpty())
    { g.why = "the map carries no groups"; return g; }

    juce::Array<int> primaries;
    for (int i = 0; i < arr->size(); ++i)
        if ((bool) (*arr)[i].getProperty ("primary", false))
            primaries.add (i);

    if (primaries.isEmpty())
    { g.why = "no group is flagged primary (" + juce::String (arr->size())
            + " groups); the map has not said which band to probe";
      return g; }
    if (primaries.size() > 1)
    {
        juce::StringArray ns;
        for (int i : primaries) ns.add ((*arr)[i].getProperty ("n", "?").toString());
        g.why = juce::String (primaries.size()) + " groups are flagged primary (n = "
              + ns.joinIntoString (", ") + "); the suite does not choose between them";
        return g;
    }
    g.ok = true;
    g.arrayIndex = primaries.getFirst();
    g.n = (int) (*arr)[g.arrayIndex].getProperty ("n", -1);
    return g;
}

//==============================================================================
/** A top-level semantic: `params.<semantic>`. */
inline SlotRef slotFor (const juce::var& mapVar, const juce::String& semantic)
{
    return detail::fromEntry (mapVar.getProperty ("params", juce::var())
                                    .getProperty (semantic, juce::var()),
                              semantic, "params / " + semantic);
}

/** A semantic inside one group, addressed by the group's array index. */
inline SlotRef slotInGroup (const juce::var& mapVar, int groupArrayIndex,
                            const juce::String& semantic)
{
    auto* arr = mapVar.getProperty ("groups", juce::var()).getArray();
    if (arr == nullptr || ! juce::isPositiveAndBelow (groupArrayIndex, arr->size()))
    {
        SlotRef s; s.semantic = semantic;
        s.where = "group[" + juce::String (groupArrayIndex) + "]";
        s.why = "no such group in this map";
        return s;
    }
    const auto& gv = (*arr)[groupArrayIndex];
    const auto n = gv.getProperty ("n", "?").toString();
    return detail::fromEntry (gv.getProperty ("params", juce::var())
                                .getProperty (semantic, juce::var()),
                              semantic, "group " + n + " / " + semantic);
}

/** A named control: `controls.<name>`. Named controls are addressed by name
    because that IS their key; the map has no other handle on them. Schema 2.3b
    adds roles, at which point a suite asks for "the stereo-width control"
    rather than for "Mono Maker". */
inline SlotRef controlNamed (const juce::var& mapVar, const juce::String& controlName)
{
    return detail::fromEntry (mapVar.getProperty ("controls", juce::var())
                                    .getProperty (controlName, juce::var()),
                              controlName, "controls / " + controlName);
}

//==============================================================================
/** Ladder points chosen from the map's OWN ladder, replacing fixed absolutes
    (eq's 100/400 Hz, comp's -30/-20/-10).

    `fractions` are positions along the ladder as the map records it, so the
    suite states its shape ("four points spread across the range") and the map
    supplies the numbers. This is the generalisation of what the limiter/gate
    suite already does with 0.15/0.4/0.65/0.9 -- promoted out of that suite
    rather than reinvented beside it.
*/
inline juce::Array<double> spreadAcrossLadder (const SlotRef& slot,
                                               const juce::Array<double>& fractions)
{
    juce::Array<double> out;
    if (! slot.ok()) return out;
    const double lo = slot.ladderLo(), hi = slot.ladderHi();
    for (double f : fractions) out.add (lo + f * (hi - lo));
    return out;
}

/** Two points a stated number of octaves apart, both inside the ladder,
    centred in it. Replaces eq's 100 Hz -> 400 Hz, which is "two octaves apart
    inside the band's range" written as two constants that happen to satisfy it
    on one fixture.

    Refuses (returns false) when the ladder cannot express the interval, which
    is a real case: a band with a 200 Hz..2 kHz range cannot do four octaves,
    and stretching to fit would silently change what was tested.
*/
struct OctavePair
{
    bool ok = false;
    double lowHz = 0, highHz = 0, octaves = 0;
    juce::String why;
};

inline OctavePair octavesApartWithin (const SlotRef& slot, double octaves)
{
    OctavePair p;
    p.octaves = octaves;
    if (! slot.ok()) { p.why = slot.why.isNotEmpty() ? slot.why : "no usable ladder"; return p; }

    const double lo = juce::jmin (slot.ladderLo(), slot.ladderHi());
    const double hi = juce::jmax (slot.ladderLo(), slot.ladderHi());
    if (lo <= 0.0 || hi <= 0.0)
    { p.why = "ladder is not in a positive frequency domain (" + juce::String (lo, 2)
            + " .. " + juce::String (hi, 2) + ")"; return p; }

    const double availableOct = std::log2 (hi / lo);
    if (availableOct < octaves)
    { p.why = "ladder spans " + juce::String (availableOct, 2) + " octaves ("
            + juce::String (lo, 1) + " .. " + juce::String (hi, 1) + " Hz), which cannot "
              "express the " + juce::String (octaves, 2) + " octaves asked for";
      return p; }

    // Centred: equal headroom below and above, so neither point sits on an
    // endpoint where a plugin's ladder often behaves differently.
    const double slackOct = (availableOct - octaves) / 2.0;
    p.lowHz  = lo * std::pow (2.0, slackOct);
    p.highHz = p.lowHz * std::pow (2.0, octaves);
    p.ok = true;
    return p;
}

//==============================================================================
/** DOES THE PLUGIN DO WHAT THE MAP SAYS? -- one implementation, every suite.

    Write the norm the map gives for a semantic value, read what the plugin
    then displays, and compare it to the value the map claimed. This is the
    check that makes parameterisation worth doing, and it is the check that
    catches the failure parameterisation CREATES: a ladder whose values are
    right and whose norms are not lands the plugin somewhere else on a curve
    the map describes correctly, and every span- or delta-based verdict in M9
    is blind to it because the span is unchanged.

    It lives here rather than in a suite because the second copy is where the
    [-1] duplicate refusal survived its own fix.

    NOTE what this does NOT prove: the display is the plugin's own claim about
    itself, so a plugin whose display and DSP disagree passes this and fails
    the acoustic check. The two are complementary and neither replaces the
    other -- that divergence is measured on this project (API-2500).
*/
struct MapClaim
{
    double claimed = 0;      // what the map says this norm means
    double shown   = 0;      // what the plugin displays after the write
    float  norm    = 0;
    bool   parsed  = false;  // could the display be read as a number at all
    juce::String display;

    double error() const { return std::abs (shown - claimed); }
};

/** `readDisplay` is supplied by the caller so this header stays free of the
    audio-processor types: pass a lambda returning getCurrentValueAsText(). */
template <typename WriteFn, typename ReadDisplayFn>
inline MapClaim checkMapClaim (const SlotRef& slot, double wantValue,
                               WriteFn&& write, ReadDisplayFn&& readDisplay)
{
    MapClaim c;
    c.claimed = wantValue;
    c.norm    = slot.normFor (wantValue);
    write (c.norm);
    c.display = readDisplay();
    float f = 0; bool negInf = false;
    const auto unit = slot.unit.isNotEmpty() ? slot.unit : juce::String ("db");
    c.parsed = echojay::parseDisplayForUnit (c.display, unit, f, negInf);
    if (! c.parsed) c.parsed = echojay::parseDisplayForUnit (c.display, "db", f, negInf);
    c.shown = negInf ? -std::numeric_limits<double>::infinity() : (double) f;
    return c;
}

/** Worst |claim - display| over a set of checks, and a line per check. */
struct MapClaimReport
{
    double worst = 0;
    int    checked = 0, unparsed = 0;
    juce::StringArray rows;

    void add (const MapClaim& c)
    {
        ++checked;
        if (! c.parsed) { ++unparsed;
            rows.add ("     map says " + juce::String (c.claimed, 2) + " at norm "
                      + juce::String (c.norm, 4) + " -> plugin displays '" + c.display
                      + "' (unreadable as a number; not counted)");
            return; }
        worst = juce::jmax (worst, c.error());
        rows.add ("     map says " + juce::String (c.claimed, 2) + " at norm "
                  + juce::String (c.norm, 4) + " -> plugin displays "
                  + juce::String (c.shown, 2) + "  |diff| " + juce::String (c.error(), 2));
    }
};

/** The map's index must address the parameter the map NAMES. A map whose
    indices shifted (a plugin update inserting a Bank broke 339 indices on this
    project) still resolves to a valid index, and every verdict computed from
    it would be about the wrong parameter. Names survived every version
    transition measured, so the name is the cross-check on the index -- the
    reverse of the resolution order, deliberately. */
struct IndexNameCheck
{
    bool agrees = false, checkable = false;
    juce::String mapName, pluginName, why;
};

inline IndexNameCheck crossCheckName (const SlotRef& slot, const juce::String& pluginParamName)
{
    IndexNameCheck r;
    r.mapName = slot.name;
    r.pluginName = pluginParamName;
    if (slot.name.isEmpty() || pluginParamName.isEmpty())
    { r.why = "no name on one side; the index cannot be cross-checked"; return r; }
    r.checkable = true;
    r.agrees = slot.name.trim().equalsIgnoreCase (pluginParamName.trim());
    if (! r.agrees)
        r.why = "the map's " + slot.semantic + " points at index " + juce::String (slot.index)
              + ", which the map calls '" + slot.name + "' and this instance calls '"
              + pluginParamName + "'";
    return r;
}

//==============================================================================
/** A SELF-MAP: live ladders written in map shape.

    Circular as verification -- it cannot disagree with the plugin it came
    from -- and exactly what is needed as a SPECIMEN, because a divergence
    check that has never been shown NOT to fire is as unproven as one that has
    never fired. Both times a ladder was hand-constructed for this purpose on
    this project it was wrong (linear assumed, curved in fact), and the wrong
    specimen read as a real divergence.

    One builder, two suites: the limiter/gate copy was the first, and a second
    inline copy in comp is where duplicated rules start drifting apart.
*/
struct SelfMapEntry
{
    juce::String semantic, name, unit;
    int index = -1;
    juce::Array<juce::Array<float>> anchors;
};

inline juce::var selfMapVar (const juce::String& fp, const juce::String& schema,
                             const juce::String& category,
                             const juce::Array<SelfMapEntry>& entries)
{
    auto* params = new juce::DynamicObject();
    for (const auto& e : entries)
    {
        juce::Array<juce::var> rows;
        for (const auto& a : e.anchors)
            rows.add (juce::var (juce::Array<juce::var> { (double) a[0], (double) a[1] }));
        auto* pe = new juce::DynamicObject();
        pe->setProperty ("index", e.index);
        pe->setProperty ("name", e.name);
        pe->setProperty ("kind", e.semantic);
        pe->setProperty ("unit", e.unit);
        pe->setProperty ("anchors", juce::var (rows));
        params->setProperty (e.semantic, juce::var (pe));
    }
    auto* mo = new juce::DynamicObject();
    mo->setProperty ("fp", fp);
    mo->setProperty ("schema", schema);
    mo->setProperty ("category", category);
    mo->setProperty ("mode", "self-map (live sweep in map shape, NOT a mapped artefact)");
    mo->setProperty ("params", juce::var (params));
    mo->setProperty ("controls", juce::var (new juce::DynamicObject()));
    mo->setProperty ("groups", juce::var (juce::Array<juce::var> {}));
    return juce::var (mo);
}

//==============================================================================
/** EXCITATION: what must be set before the effect being probed exists at all.

    A compressor with ratio 1:1 compresses nothing; a saturator with its stage
    bypassed distorts nothing (which M9 measured, driving a bypassed stage and
    reporting a null). The suites that need one hard-code it inline today.

    THIS IS THE INTERFACE, DELIBERATELY AHEAD OF THE SCHEMA. Schema 2.3a will
    serialise plans into maps; when it does, `resolve()` below changes and no
    suite does. The alternative -- each suite inventing its own inline
    excitation now -- would need three rewrites when 2.3a lands, which is the
    duplicated-rule shape that let the [-1] refusal survive its own fix.
*/
struct ExcitationStep
{
    int          index = -1;
    double       value = 0.0;
    juce::String semantic, why;
};

struct ExcitationPlan
{
    juce::Array<ExcitationStep> steps;
    /** "none" | "suite:<name>" | "map" | "fixture:<name>". Named so a report
        can say where the excitation came from, which matters when a verdict
        depends on it. */
    juce::String source = "none";

    bool declared() const { return source != "none" && ! steps.isEmpty(); }

    /** The declared value for a semantic, or `fallback` when the plan says
        nothing about it. Suites re-establishing excitation between measurement
        blocks must ask HERE: reading the ladder's own maximum instead is how a
        map-declared ratio of 6:1 got silently overwritten with 10:1 while the
        report still said 6. */
    double valueFor (const juce::String& semantic, double fallback) const
    {
        for (const auto& s : steps)
            if (s.semantic == semantic) return s.value;
        return fallback;
    }

    juce::String describe() const
    {
        if (! declared()) return "no excitation plan (source: " + source + ")";
        juce::StringArray parts;
        for (const auto& s : steps)
            parts.add (s.semantic + "[" + juce::String (s.index) + "] -> "
                       + juce::String (s.value, 2) + (s.why.isNotEmpty() ? " (" + s.why + ")" : ""));
        return "excitation from " + source + ": " + parts.joinIntoString ("; ");
    }
};

/** THE RESULT OF APPLYING A PLAN. Reported, never assumed: a write that did
    not land is the difference between "the plugin was excited" and "the suite
    believes the plugin was excited". */
struct ExcitationResult
{
    int applied = 0, unlanded = 0, outOfRange = 0;
    juce::String source, detail;
    bool ok() const { return unlanded == 0 && outOfRange == 0 && applied > 0; }
};

/** APPLY the plan. `write` returns the landing time in ms, negative if the
    write never landed -- the same contract writeAndServiceRunloop already has.

    This exists because comp declared a plan and then wrote its excitation
    inline anyway: the declaration described the code instead of being it, and
    a description beside an implementation is the false-comment class with a
    struct around it. The inline writes are deleted; this is the only path.
*/
template <typename WriteFn>
inline ExcitationResult applyExcitation (const ExcitationPlan& plan, int paramCount,
                                         WriteFn&& write)
{
    ExcitationResult r;
    r.source = plan.source;
    juce::StringArray notes;
    for (const auto& st : plan.steps)
    {
        if (! juce::isPositiveAndBelow (st.index, paramCount))
        { ++r.outOfRange;
          notes.add (st.semantic + "[" + juce::String (st.index) + "] is outside the instance's "
                     + juce::String (paramCount) + " parameters");
          continue; }
        const double ms = write (st.index, st.value, st.semantic);
        if (ms < 0) { ++r.unlanded; notes.add (st.semantic + " did not land"); }
        else ++r.applied;
    }
    r.detail = notes.joinIntoString ("; ");
    return r;
}

/** THE SINGLE RESOLUTION POINT. Every suite asks here and nowhere else.

    Order: the map's own plan wins when one exists (schema 2.3a), otherwise the
    suite's declared plan, otherwise none. Absent key means unavailable -- the
    metering convention -- so a schema-2.2 map falls through to the suite plan
    rather than failing, and every map already written stays probeable.
*/
inline ExcitationPlan resolveExcitation (const juce::var& mapVar,
                                         const ExcitationPlan& suiteDeclared)
{
    // SCHEMA 2.3a, SERIALISED. `excitation` is an array of
    // {index, value, semantic, why}. Absent means unavailable, so every
    // schema-2.2 map still resolves to its suite's declared plan.
    if (auto* arr = mapVar.getProperty ("excitation", juce::var()).getArray())
    {
        ExcitationPlan p;
        p.source = "map";
        for (const auto& sv : *arr)
        {
            ExcitationStep st;
            st.index    = (int) sv.getProperty ("index", -1);
            st.value    = (double) sv.getProperty ("value", 0.0);
            st.semantic = sv.getProperty ("semantic", "").toString();
            st.why      = sv.getProperty ("why", "").toString();
            if (st.index >= 0) p.steps.add (st);
        }
        if (! p.steps.isEmpty()) return p;
    }
    return suiteDeclared;
}

} // namespace subject
} // namespace ejmap
