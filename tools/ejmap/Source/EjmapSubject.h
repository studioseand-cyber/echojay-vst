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

/** THE SINGLE RESOLUTION POINT. Every suite asks here and nowhere else.

    Order: the map's own plan wins when one exists (schema 2.3a), otherwise the
    suite's declared plan, otherwise none. Absent key means unavailable -- the
    metering convention -- so a schema-2.2 map falls through to the suite plan
    rather than failing, and every map already written stays probeable.
*/
inline ExcitationPlan resolveExcitation (const juce::var& mapVar,
                                         const ExcitationPlan& suiteDeclared)
{
    // SCHEMA 2.3a HOOK. Nothing writes this key yet; the branch is here so
    // that landing 2.3a is an edit to one function rather than a search.
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
