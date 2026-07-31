/*
  EjmapSweeper.h

  M3: turn a captured index into a value curve. PER CAPTURED INDEX, never per
  plugin: the settle-and-read cost is per parameter, a Pro-Q 3 has 358 of
  them, and the mapper only needs curves for the handful a human actually
  captured.

  EVERY MOVING PART IS SHARED CODE, per the M3 rule that reimplementation is
  drift:

    - sweepContinuous / sweepDiscrete / sweepIsFlat / sweepSetRead come from
      tools/ejextract/EchoJayParamExtractor.h, the header the corpus was built
      with, byte-identity proven at the lift.
    - parseLeadingFloat (same header) turns display texts into anchor values,
      exactly as corpus refinement did.
    - dominantMonotonicTable (Source/EchoJayParamApply.h) is the sanitizer:
      strictly-monotonic dominant run, either direction, >= 60% or refuse.
      Strictness means plateau points and trailing junk fall out of the run
      (recorded as rejected, never silently), and mirror shapes fail the 60%
      rule. This is the same function applyOne trusts at dial time, so what
      survives sanitization here is precisely what will interpolate there.

  FLAT DETECTION IS BEHAVIOURAL, NEVER NAME-BASED (register rule). A flat
  gettext sweep (>= 3 points, one distinct text -- the third point is the
  plan's confirmation) earns the set-then-read retry; flat through BOTH paths
  is the text-liar verdict, and the typed-anchor path (M3 pass two) is its
  fallback. The retry brackets the whole instance with a full state
  snapshot/restore, same as the extractor, because moving one control can
  side-effect another.

  THE CALLER PAUSES THE PUMP. sweepSetRead mutates parameters and the flat
  retry restores full plugin state, and neither is specified to be callable
  against a concurrent processBlock. The extractor never met this hazard
  because it never ran audio; ejmap pumps, so callers pause
  (PluginHost::pausePumpForMutation, which also drains the in-flight block).
  Do NOT cite the elysia mpressor crash as this race: three backtraces showed
  it dying in its own render with no mutation anywhere in flight. That is a
  pump-compatibility fact about mpressor, recorded where it belongs.

  ANCHOR VALUES ARE RAW PARSED FLOATS. No unit normalisation happens here:
  the sweep does not know its semantic yet (assignment is M4), so "1k1"
  parses as 1.0, not 1100. The display texts travel with the anchors in the
  record, and the unit-aware parse (parseDisplayForUnit under the assigned
  semantic's unit) happens at assignment, where a unit exists to parse into.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "EjmapWatchdog.h"
#include "EchoJayParamExtractor.h"   // shared sweep, shared parseLeadingFloat
#include "EchoJayParamApply.h"       // shared sanitizer (dominantMonotonicTable)

namespace ejmap
{

struct SweepOutcome
{
    /** ok means: a sanitized anchor table with >= 2 points and real span
        exists. Everything else is a recorded refusal with a reason, because a
        sweep that produced nothing is a fact, not an absence.
    */
    bool ok = false;

    juce::String method;              // "gettext" | "setread"
    bool flat            = false;     // flat through BOTH paths: text liar
    bool setreadRefused  = false;     // readback-verify failed; not mutated further
    bool anchorsReversed = false;     // values run descending across the table
    int  rejectedPoints  = 0;         // parsed points outside the dominant run
    int  unparsedPoints  = 0;         // texts with no leading float

    /** Every parsed value equals its own normalized position (within 0.005,
        over >= 5 anchors). CANDIDATE flag, not a verdict: measured on
        ValhallaVintageVerb AU, where Decay -- really 0.2 s to 70 s -- swept
        as a perfect 0..1 identity because the AU hosting layer fabricates a
        normalized display when the plugin provides no string-from-value. A
        genuine unitless 0..1 control looks identical, which is exactly why
        this describes the shape and asserts nothing. Identity anchors still
        dial correctly in NORM terms; what they cannot support is
        unit-bearing requests ("2.5 s"), which is the typed path's territory.
        The second liar signature: the first reads flat, this one reads
        plausible.
    */
    bool identityDisplay = false;

    juce::Array<echojay::SweepPoint>  points;      // what the plugin displayed
    juce::Array<juce::Array<float>>   anchors;     // [value, norm], sanitized
    juce::Array<juce::Array<float>>   rawAnchors;  // [value, norm], pre-sanitize:
                                                   // the curve view marks what
                                                   // sanitization rejected
    juce::String reason;
    int durationMs = 0;
};

inline SweepOutcome sweepOneIndex (juce::AudioPluginInstance& inst,
                                   int index,
                                   Watchdog& watchdog,
                                   const juce::String& pluginId)
{
    SweepOutcome out;

    auto& params = inst.getParameters();
    if (! juce::isPositiveAndBelow (index, params.size()))
    {
        out.reason = "index " + juce::String (index) + " not present on this instance";
        return out;
    }
    auto* p = params[index];

    Watchdog::Scope guard (watchdog, "sweep index " + juce::String (index),
                           pluginId, {}, {}, "sweep", Watchdog::kEditorCreateDeadlineMs);

    const auto t0 = juce::Time::getMillisecondCounter();
    echojay::ExtractorConfig cfg;

    // Read-only first, exactly like the extractor.
    out.method = "gettext";
    out.points = p->isDiscrete() ? echojay::sweepDiscrete   (*p, cfg)
                                 : echojay::sweepContinuous (*p, cfg);

    if (echojay::sweepIsFlat (out.points))
    {
        // Full-state bracket around the mutating retry, same as the
        // extractor: each parameter restores its own value, but a linked or
        // macro'd control can side-effect others and only a full restore
        // guarantees the instance leaves as it arrived.
        juce::MemoryBlock stateBefore;
        inst.getStateInformation (stateBefore);

        bool verified = false;
        auto retried = echojay::sweepSetRead (*p, cfg, verified);

        if (stateBefore.getSize() > 0)
            inst.setStateInformation (stateBefore.getData(), (int) stateBefore.getSize());

        if (! verified)
        {
            out.setreadRefused = true;
            out.flat = true;
            out.reason = "flat via gettext; set-then-read readback-verify failed "
                         "(two distinct set points did not yield distinct texts), "
                         "so the plugin was not mutated further. Text liar; "
                         "typed-anchor path is the fallback.";
            out.durationMs = (int) (juce::Time::getMillisecondCounter() - t0);
            return out;
        }

        if (echojay::sweepIsFlat (retried))
        {
            out.flat = true;
            out.reason = "flat via BOTH paths: the display carries no value "
                         "information. Text liar; typed-anchor path is the fallback.";
            out.durationMs = (int) (juce::Time::getMillisecondCounter() - t0);
            return out;
        }

        out.method = "setread";
        out.points = retried;
    }

    // Texts -> raw anchors, with the corpus parser.
    juce::Array<juce::Array<float>> raw;
    for (const auto& sp : out.points)
    {
        double v = 0.0;
        if (echojay::parseLeadingFloat (sp.t, v))
        {
            juce::Array<float> a;
            a.add ((float) v);
            a.add (sp.n);
            raw.add (a);
        }
        else
            ++out.unparsedPoints;
    }

    if (raw.size() < 2)
    {
        out.reason = "only " + juce::String (raw.size()) + " of "
                   + juce::String (out.points.size()) + " points parse numerically: "
                     "mode/position material for M4, not an anchor curve.";
        out.durationMs = (int) (juce::Time::getMillisecondCounter() - t0);
        return out;
    }

    out.rawAnchors = raw;

    // The shared sanitizer. Rejections are counted, never silent.
    auto eff = echojay::dominantMonotonicTable (raw);
    if (! eff.ok)
    {
        out.rejectedPoints = raw.size();
        out.reason = "sanitizer refused: no strictly-monotonic run covers 60% of "
                   + juce::String (raw.size()) + " parsed points (mirror or garbage "
                     "shape). Refused, not guessed at.";
        out.durationMs = (int) (juce::Time::getMillisecondCounter() - t0);
        return out;
    }

    out.anchors        = eff.table;
    out.rejectedPoints = raw.size() - eff.table.size();
    out.anchorsReversed = eff.table.getFirst()[0] > eff.table.getLast()[0];

    // Degenerate span: the same 1e-6 rule applyOne refuses at. Catching it
    // here means a map that cannot dial is never built, rather than being
    // built and refused later.
    float lo = eff.table.getFirst()[0], hi = lo;
    for (const auto& a : eff.table) { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
    if (hi - lo < 1.0e-6f)
    {
        out.ok = false;
        out.reason = "degenerate span: sanitized values cover "
                   + juce::String (hi - lo, 8) + ", which applyOne would refuse. "
                     "Not anchor material.";
        out.durationMs = (int) (juce::Time::getMillisecondCounter() - t0);
        return out;
    }

    {
        bool ident = out.anchors.size() >= 5;
        for (const auto& a : out.anchors)
            if (std::abs (a[0] - a[1]) > 0.005f) { ident = false; break; }
        out.identityDisplay = ident;
    }

    out.ok = true;
    out.reason = juce::String (out.anchors.size()) + " anchors ("
               + out.method + (out.anchorsReversed ? ", descending" : ", ascending")
               + (out.rejectedPoints > 0
                    ? ", " + juce::String (out.rejectedPoints) + " point(s) outside the dominant run"
                    : juce::String())
               + (out.unparsedPoints > 0
                    ? ", " + juce::String (out.unparsedPoints) + " unparsed"
                    : juce::String())
               + (out.identityDisplay
                    ? ", IDENTITY DISPLAY: every value equals its norm, so the display "
                      "may be fabricated and unit-bearing requests may need the typed path"
                    : juce::String())
               + ")";
    out.durationMs = (int) (juce::Time::getMillisecondCounter() - t0);
    return out;
}

} // namespace ejmap
