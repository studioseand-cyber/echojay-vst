/*
  EjmapCapture.h

  M2 pass one: turn a human touching a knob into a parameter index.

  WHAT IS SUPERSEDED FROM THE PLAN, and why.

  The plan specifies a 30 Hz poll. That number is retired. Detection diffs
  against the snapshot taken ON ARM, so a parameter that moves stays moved and a
  later tick still sees it: detection does not depend on rate at all. Rate
  governs two other things, sequential-edit disambiguation (two controls touched
  inside one tick collapse into one moved set and get misread as a twin pair)
  and ui_hint fidelity in pass two. So the rate is calibrated per plugin from
  what a sweep actually costs, not fixed.

  WHY THAT MATTERS RATHER THAN BEING TIDY. Measured: a parameter read costs
  0.03 to 0.24 us in-process and 50 to 80 us across the XPC bridge, a factor of
  200 to 2500. Saturn 2 has 951 parameters and Pro-Q 3 has 358. A fixed 30 Hz
  would spend the entire frame budget reading parameters on a bridged plugin
  with a large surface, and would be nearly free on a small native one.

  CONCURRENCY, established from JUCE's source rather than assumed.

    AU    getValue() takes AudioUnitPluginInstance::lock; processAudio does NOT
          take that lock. Reads and processBlock therefore run concurrently and
          reads never block on the pump. Safety rests on AudioUnitGetParameter
          being callable against AudioUnitRender, which is the contract every
          host relies on.
    VST3  getValue() reads CachedParamValues, a lock-free float cache. No
          contention with processBlock whatsoever.

  What the same lock DOES guard is instance teardown. So the rule this engine
  follows is the one the pump SIGSEGV taught: stop capture before unload, rather
  than trying to serialise it against processBlock.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <map>

#include "EjmapWatchdog.h"
#include "EjmapParamListeners.h"

namespace ejmap
{

class CaptureEngine : private juce::Thread
{
public:
    //==========================================================================
    struct Calibration
    {
        int    paramCount   = 0;
        double sweepMicros  = 0.0;   // one full read of every parameter
        double rateHz       = 0.0;   // derived, clamped
        bool   valid        = false;
        bool   reasonEmpty  = false;  // plugin exposes no automatable parameters

        juce::String describe() const
        {
            return juce::String (paramCount) + " params, "
                 + juce::String (sweepMicros / 1000.0, 2) + " ms/sweep, "
                 + juce::String (rateHz, 1) + " Hz";
        }
    };

    struct NoiseMask
    {
        juce::SortedSet<int> indices;
        int    samples = 0;          // recorded next to the mask: a thin baseline is visible
        double seconds = 0.0;

        /** Retroactive additions. Structured, not prose, because a promotion is
            an event the owner writes to the record and can later be RELEASED by
            a human -- both need the index, not a sentence containing it.
        */
        struct Promotion
        {
            int index = -1;
            juce::String reason;
        };
        juce::Array<Promotion> promotions;

        /** How this mask was arrived at, so a reader never has to guess whether
            an empty mask means "looked hard and found nothing" or "barely
            looked". Recorded in the payload beside the mask itself.
        */
        juce::String method;         // "short_probe_clean" | "full_baseline" | "promoted_only"
    };

    /** Every arm ends in exactly one of these, and every one carries a reason.
        A capture that produced nothing is a recorded fact, never an absence.
    */
    struct Result
    {
        /** No "uncorrelated" entry. It was removed rather than left unreachable:
            an outcome vocabulary that names something nothing emits misleads the
            next reader. The macro and bad-epsilon explanations still exist, as
            possibilities offered inside a gesture's reason string rather than as
            an asserted verdict.

            NO "twins" ENTRY EITHER, for the same reason and a stronger one.
            "Channel twins" named a mechanism -- one on-screen control writing two
            parameters -- that this engine cannot observe. All it sees is a delta
            vector. One control writing two parameters, a link switch mirroring a
            value change onto its partner, and two parameters that merely
            correlate are ONE event to a poll: N indices changed together inside
            one detection window. Emitting "twins" asserted a mechanism from
            evidence that cannot carry it, which is the same fault as the old
            "uncorrelated" verdict.

            The shape that used to trigger it -- same direction, magnitudes within
            1.5x -- is still measured and still reported, as sameDirection and
            magnitudeRatio below. It is a description of what was seen, not a
            claim about what produced it.

            What would earn the distinction back is a reliable touched-parameter
            signal: parameterGestureChanged names the parameter the human began a
            gesture on, so the touched control becomes separable from the
            follower. That arrives with the listener layer. Until it does, a
            gesture reports its members and does not rank them.
        */
        enum class Kind { captured, gesture, tooMany, notAutomatable, stalled };

        Kind kind = Kind::notAutomatable;
        juce::Array<int> indices;

        /** Real parameter names, parallel to indices. A human cannot act on
            "[0, 2, 3, 7, 8]"; they can act on "Band 1 Freq, Band 1 Gain".
        */
        juce::StringArray names;
        juce::Array<float> deltas;
        juce::String reason;

        /** Observed shape of a multi-parameter move. DESCRIPTIVE ONLY.

            sameDirection: every delta had the same sign.
            magnitudeRatio: largest |delta| over smallest, so 1.0 is exact
              lockstep and 0.0 means not applicable (fewer than two moved, or a
              zero delta slipped through the epsilon).

            A reader may well conclude "1.00 in lockstep, that is a mirror". That
            conclusion is theirs to draw from the numbers, and it is not what the
            record says happened.
        */
        bool  sameDirection  = false;
        float magnitudeRatio = 0.0f;

        /** When the change was DETECTED. The mouse ring is queried against this
            rather than sampled at this moment, so ui_hint fidelity does not
            depend on the calibrated poll rate.
        */
        juce::uint32 detectedAtMs = 0;

        /** When the capture was ARMED. The evidence probe needs the window
            [armed, detected]: a gesture or grab before arm is stale evidence.
        */
        juce::uint32 armedAtMs = 0;

        /** The index the plugin itself named via a parameter gesture, when it
            named exactly one. -1 otherwise. This is what earns a
            multi-parameter move the "captured" kind without a human pick: the
            follower question is answered by the plugin, not guessed at.
        */
        int primaryIndex = -1;

        /** Which mechanism produced the attribution, recorded per row because
            the plan requires evidence.captured_by and because a corpus of
            these tells us how often the listener layer actually fires.
              "poll"          the poll alone; the listener was silent or ambiguous
              "poll+gesture"  the poll found one index and the plugin's gesture agrees
              "gesture"       the plugin's gesture resolved a multi-parameter move
        */
        juce::String capturedBy { "poll" };

        juce::String kindString() const
        {
            switch (kind)
            {
                case Kind::captured:       return "captured";
                case Kind::gesture:        return "gesture";
                case Kind::tooMany:        return "too_many";
                case Kind::notAutomatable: return "not_automatable";
                case Kind::stalled:        return "stalled";
            }
            return "not_automatable";
        }
    };

    using ResultFn = std::function<void (const Result&)>;

    /** Answers "is there human evidence behind this index's movement?" for one
        delivered result -- a mouse grab inside the hosted editor, a parameter
        gesture reported by the plugin. Supplied by the owner because the
        evidence lives in things the engine has no business owning (the mouse
        ring, the editor's bounds, the listener bank).

        Runs on the message thread, at delivery. Returning true means the
        appearance does NOT count toward noise promotion.
    */
    using HumanEvidenceFn = std::function<bool (int index, const Result&)>;

    void setHumanEvidenceProbe (HumanEvidenceFn f) { humanEvidence = std::move (f); }

    /** The listener bank is optional: with none set, every result is
        attributed by the poll alone, which is exactly the pass-one behaviour.
    */
    void setListenerBank (ParamListenerBank* b) noexcept { listeners = b; }

    //==========================================================================
    /** Rate policy. 30% of the frame budget so capture never starves the editor,
        floored at 4 Hz because below ~250 ms two sequential edits collapse into
        one moved set and are misread as a linked pair, which is WRONG data
        rather than missing data. Ceiling 30 Hz: faster buys nothing once
        detection is snapshot-relative.
    */
    static constexpr double kBudgetFraction = 0.30;
    static constexpr double kMinRateHz      = 4.0;
    static constexpr double kMaxRateHz      = 30.0;

    /** BASELINE IS ADAPTIVE, AND SHORT BY DEFAULT.

        The plan assumed meters, LFOs and gain-reduction readouts routinely appear
        as automatable parameters, and charged 3 s of every tester's time on every
        plugin to find them. Measured across 4,157 extracted maps and 1,074,600
        parameters, that assumption is wrong: of 6,438 parameters whose NAME looks
        like a readout, 6,365 respond to being written, so they are output trims
        and level controls rather than readouts. Only 73 in 1,074,600 have both a
        readout name and a flat sweep. The 21.7% of parameters with a flat sweep
        are dominated by MIDI CC automation slots (72,800 named "MIDI" alone), not
        by meters.

        So: probe briefly, and only pay for a full baseline when the short probe
        actually finds something. Cheap on the ~99.99% where nothing is
        self-changing, thorough where something is.

        RETROACTIVE PROMOTION IS THE PRIMARY DEFENCE, because it observes rather
        than predicts. The baseline guesses in advance which indices will misbehave;
        promotion watches what actually moves when nobody touched anything.
    */
    static constexpr int    kShortProbeSamples  = 8;
    static constexpr double kBaselineMinSeconds = 3.0;
    static constexpr int    kBaselineMinSamples = 30;
    static constexpr double kBaselineMaxSeconds = 10.0;

    static constexpr int    kCalibrationSweeps  = 20;   // first is discarded
    static constexpr int    kTooManyMoved       = 8;
    static constexpr int    kNothingMovedMs     = 8000;

    /** An index seen moving in this many separate arm cycles WITH NO HUMAN
        EVIDENCE BEHIND ANY OF THEM is self-changing, whatever the baseline
        concluded.

        The "no human evidence" clause is not decoration; without it this
        constant masked two real controls on this machine, and the rows prove
        both:

          - Q1 (s), run 133241: captures went idx 5, 5, 6, 2, 2, 2 -- and on the
            third deliberate capture of Band 1 Gain (idx 2) it was promoted.
            Three wiggles of a working control read as self-change.
          - Pro-Q 3, run 114642: idx 2 (Band 1 Frequency) promoted the same way,
            while the 89-sample baselines in the same session, and a fresh
            baseline run for this fix (silent AND signal-driven), all returned
            an empty mask. The baseline never flagged it once.

        So an appearance only counts toward promotion when the owner's evidence
        probe (see setHumanEvidenceProbe) finds nothing human behind it. A
        meter or LFO moves with hands off; a control moves because a hand was on
        it, and the difference is observable.
    */
    static constexpr int    kPromoteAfterCycles = 3;

    //==========================================================================
    explicit CaptureEngine (Watchdog& w) : juce::Thread ("ejmap capture"), watchdog (w) {}
    ~CaptureEngine() override { stop(); }

    /** Times a full sweep and derives the rate. The first sweep is discarded:
        on a bridged plugin it pays the XPC connection setup and is not
        representative of steady state.
    */
    Calibration calibrate (juce::AudioPluginInstance& inst, const juce::String& pluginId)
    {
        Calibration c;
        const auto& params = inst.getParameters();
        c.paramCount = params.size();
        if (c.paramCount == 0)
        {
            c.reasonEmpty = true;
            return c;
        }

        Watchdog::Scope guard (watchdog, "capture calibration", pluginId, {}, {}, "capture",
                               Watchdog::kEditorCreateDeadlineMs);

        float sink = 0.0f;
        for (auto* p : params) sink += p->getValue();          // discarded warm-up

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 1; i < kCalibrationSweeps; ++i)
            for (auto* p : params) sink += p->getValue();
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;

        juce::ignoreUnused (sink);

        c.sweepMicros = (elapsedMs * 1000.0) / (double) (kCalibrationSweeps - 1);
        const double sweepSeconds = c.sweepMicros / 1.0e6;
        c.rateHz = sweepSeconds > 0.0
                     ? juce::jlimit (kMinRateHz, kMaxRateHz, kBudgetFraction / sweepSeconds)
                     : kMaxRateHz;
        c.valid = true;
        return c;
    }

    /** Baseline with nothing touched. Anything that moves on its own is a meter,
        an LFO or a gain-reduction readout, and must never be capturable.
    */
    NoiseMask buildNoiseMask (juce::AudioPluginInstance& inst,
                              const Calibration& cal,
                              const juce::String& pluginId)
    {
        NoiseMask mask;
        const auto& params = inst.getParameters();
        if (params.isEmpty() || ! cal.valid)
            return mask;

        Watchdog::Scope guard (watchdog, "capture baseline", pluginId, {}, {}, "capture",
                               Watchdog::kEditorCreateDeadlineMs);

        const int intervalMs = (int) juce::jmax (1.0, 1000.0 / cal.rateHz);

        std::vector<float> ref (params.size());
        for (int i = 0; i < params.size(); ++i) ref[(size_t) i] = params[i]->getValue();

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        int samples = 0;

        // SHORT PROBE. Eight samples is enough to see anything moving at audio
        // or UI rate, which is what a meter does.
        for (int i = 0; i < kShortProbeSamples; ++i)
        {
            juce::Thread::sleep (intervalMs);
            ++samples;
            for (int p = 0; p < params.size(); ++p)
                if (std::abs (params[p]->getValue() - ref[(size_t) p]) > epsilonFor (params[p]))
                    mask.indices.add (p);
        }

        if (mask.indices.isEmpty())
        {
            // Nothing moved. Do not spend the other 2.7 seconds looking again:
            // promotion will catch anything this missed, and it will catch it by
            // observation rather than by having waited longer.
            mask.samples = samples;
            mask.seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            mask.method  = "short_probe_clean";
            return mask;
        }

        // Something IS self-changing. Now it is worth characterising properly,
        // because a thin sample cannot separate a slow meter from one spurious
        // read.
        for (;;)
        {
            juce::Thread::sleep (intervalMs);
            ++samples;

            for (int i = 0; i < params.size(); ++i)
                if (std::abs (params[i]->getValue() - ref[(size_t) i]) > epsilonFor (params[i]))
                    mask.indices.add (i);

            const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            if (secs >= kBaselineMaxSeconds) break;
            if (secs >= kBaselineMinSeconds && samples >= kBaselineMinSamples) break;
        }

        mask.samples = samples;
        mask.seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        mask.method  = "full_baseline";
        return mask;
    }

    //==========================================================================
    /** Snapshots and starts polling. One arm produces one Result. */
    void arm (juce::AudioPluginInstance& inst,
              const Calibration& cal,
              NoiseMask& maskRef,
              ResultFn onResult)
    {
        stop();

        instance = &inst;
        calibration = cal;
        mask = &maskRef;
        callback = std::move (onResult);

        const auto& params = inst.getParameters();
        snapshot.resize ((size_t) params.size());
        for (int i = 0; i < params.size(); ++i)
            snapshot[(size_t) i] = params[i]->getValue();

        armedAt = juce::Time::getMillisecondCounter();
        startThread (juce::Thread::Priority::normal);
    }

    void stop()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            notify();
            stopThread (3000);
        }
        instance = nullptr;
    }

    bool isArmed() const { return isThreadRunning(); }

    /** Promotion counts are per plugin CONTEXT, not per engine lifetime. The
        engine outlives loads, and the counts are keyed by bare index, so
        without this a new load inherits the old plugin's counts. Measured, not
        hypothetical: Pro-Q 3 run 114642, row at 11:56:44 -- idx 2 re-promoted
        on its FIRST appearance after a reload whose fresh 89-sample baseline
        was clean, because the count of 3 survived the reload. Call on every
        load, before the new mask is built.
    */
    void resetCycleCounts()             { cycleAppearances.clear(); }

    /** A human releasing an index from the mask is also saying the count that
        promoted it was wrong. Without this the very next appearance would
        re-promote instantly (the count is still at threshold) and the release
        would be a no-op with extra steps.
    */
    void clearCycleCount (int index)    { cycleAppearances.erase (index); }

    /** Per-parameter epsilon. A single global value misses fine controls and
        false-fires on coarse ones: half a step for a discrete parameter, 1e-4
        for a continuous one.
    */
    static float epsilonFor (const juce::AudioProcessorParameter* p)
    {
        const int steps = p->getNumSteps();
        if (p->isDiscrete() && steps > 1)
            return 0.5f / (float) (steps - 1);
        return 1.0e-4f;
    }

private:
    void run() override
    {
        const int intervalMs = (int) juce::jmax (1.0, 1000.0 / calibration.rateHz);

        while (! threadShouldExit())
        {
            wait (intervalMs);
            if (threadShouldExit() || instance == nullptr)
                return;

            const auto& params = instance->getParameters();

            juce::Array<int> moved;
            juce::Array<float> deltas;

            for (int i = 0; i < params.size() && i < (int) snapshot.size(); ++i)
            {
                if (mask != nullptr && mask->indices.contains (i))
                    continue;

                const float now = params[i]->getValue();
                const float d   = now - snapshot[(size_t) i];

                if (std::abs (d) > epsilonFor (params[i]))
                {
                    moved.add (i);
                    deltas.add (d);
                }
            }

            if (moved.isEmpty())
            {
                if ((int) (juce::Time::getMillisecondCounter() - armedAt) >= kNothingMovedMs)
                {
                    Result r;
                    r.kind = Result::Kind::notAutomatable;
                    r.reason = "nothing moved in " + juce::String (kNothingMovedMs / 1000)
                                 + "s; the control may exist on the GUI without an "
                                   "automatable parameter behind it";
                    deliver (r);
                    return;
                }
                continue;
            }

            {
                auto res = classify (moved, deltas, params);
                res.detectedAtMs = juce::Time::getMillisecondCounter();
                res.armedAtMs    = armedAt;
                deliver (res);
            }
            return;
        }
    }

    Result classify (const juce::Array<int>& moved,
                     const juce::Array<float>& deltas,
                     const juce::Array<juce::AudioProcessorParameter*>& params)
    {
        Result r;
        r.deltas = deltas;
        for (int i : moved)
            r.names.add (juce::isPositiveAndBelow (i, params.size())
                           ? params[i]->getName (48) : juce::String ("index " + juce::String (i)));

        // What the plugin itself says was touched: gesture begins since arm,
        // intersected with what actually moved. A gesture on an index that
        // never moved is not attribution evidence for this result.
        juce::Array<int> gestured;
        if (listeners != nullptr)
            for (int g : listeners->gestureBeginsSince (armedAt))
                if (moved.contains (g))
                    gestured.add (g);

        if (moved.size() > kTooManyMoved)
        {
            r.kind = Result::Kind::tooMany;
            r.indices = moved;
            r.reason = juce::String (moved.size()) + " parameters moved at once ("
                     + r.names.joinIntoString (", ").substring (0, 160)
                     + "): most likely a preset change or a modulation source rather than one "
                       "gesture. Discarded. The 8-parameter boundary is a heuristic, so a very "
                       "wide deliberate gesture would land here too.";
            return r;
        }

        if (moved.size() == 1)
        {
            r.kind = Result::Kind::captured;
            r.indices = moved;
            r.primaryIndex = moved[0];
            if (gestured.contains (moved[0]))
            {
                r.capturedBy = "poll+gesture";
                r.reason = "exactly one parameter moved: " + r.names[0]
                             + "; the plugin's own gesture report agrees";
            }
            else
            {
                r.reason = "exactly one parameter moved: " + r.names[0];
            }
            return r;
        }

        // Two or more. Measure the SHAPE of the move -- same direction, and how
        // far apart the magnitudes are. This used to fork here: same sign plus
        // comparable magnitude was emitted as Kind::twins ("channel twins or a
        // linked pair"), everything else as a gesture.
        //
        // That fork is gone, because the branch could not support it. See the
        // Kind enum for the full reasoning; the short version is that a poll sees
        // a delta vector, and one control writing two parameters is
        // indistinguishable from a link mirroring a value onto its partner, or
        // from two parameters that merely correlate. The shape below is real and
        // is kept. The mechanism it was taken to prove was never observed: the
        // MC 77 case the plan cited as precedent turned out to be an inference
        // from a parameter list, and no confirmed case has been found.
        bool sameSign = true;
        float lo = std::abs (deltas[0]), hi = lo;
        for (int i = 1; i < deltas.size(); ++i)
        {
            if ((deltas[i] > 0.0f) != (deltas[0] > 0.0f)) sameSign = false;
            lo = juce::jmin (lo, std::abs (deltas[i]));
            hi = juce::jmax (hi, std::abs (deltas[i]));
        }

        r.sameDirection  = sameSign;
        r.magnitudeRatio = lo > 0.0f ? (hi / lo) : 0.0f;

        // THE PLUGIN NAMED THE TOUCHED CONTROL. When exactly one of the moved
        // indices had a gesture begin behind it, the follower question -- the
        // one the poll structurally cannot answer and the human used to be
        // asked -- is answered by the plugin itself. The rest stay co-moved.
        // Exactly one, deliberately: zero gestures decides nothing (a known
        // class of plugins never sends them), and two or more is ambiguity
        // again (a mirroring plugin may report both sides of a link), so both
        // fall through to the human.
        if (gestured.size() == 1)
        {
            r.kind = Result::Kind::captured;
            r.indices = moved;
            r.primaryIndex = gestured[0];
            r.capturedBy = "gesture";

            const int pos = moved.indexOf (gestured[0]);
            r.reason = juce::String (moved.size()) + " parameters moved together and the plugin "
                         "reported a gesture on "
                     + (pos >= 0 ? r.names[pos] : juce::String ("index " + juce::String (gestured[0])))
                     + ": that is the touched control, the rest are co-moved.";
            r.reason << " Shape: " << (sameSign ? "all same direction" : "mixed directions")
                     << ", magnitudes "
                     << (r.magnitudeRatio > 0.0f
                            ? juce::String (r.magnitudeRatio, 2) + "x apart"
                            : juce::String ("not comparable"))
                     << ".";
            return r;
        }

        {
            // MULTI-PARAMETER GESTURE, not a fault.
            //
            // The previous verdict here was "uncorrelated: a macro, or the
            // epsilon is wrong. Wiggle again." All three parts were wrong for
            // the commonest case on a curve-based EQ: dragging a band node
            // genuinely moves frequency and gain together, so nothing is
            // uncorrelated, nothing is misconfigured, and wiggling again cannot
            // succeed because the gesture always moves the same set. Measured on
            // FabFilter Pro-Q 3 VST3, which returned five moved parameters for
            // one node drag.
            //
            // So: name the candidates and let the human say which they meant.
            // The rest are co-moved, which is real information about the
            // plugin (Band 1 Freq and Band 1 Gain move together) and is kept,
            // not discarded.
            //
            // Macro and epsilon remain possible and are offered as such rather
            // than asserted, because a simpler explanation fits.
            // NOTE FOR M5, recorded where co_moved is produced because this is
            // the input it will consume.
            //
            // If M5 derives an index stride to verify a name-pattern grouping,
            // that stride MUST come from observed captures -- two bands actually
            // touched -- and NEVER from the indices the name pattern itself
            // assigned. Two methods sharing an input are one method, and their
            // agreement proves nothing while looking like corroboration.
            //
            // Measured across 4,233 extracted maps: of 571 with a semantic band
            // family, 85.1% have a stride predictive from its first two members,
            // and 67.2% of multi-family maps share one stride. So it is a real
            // signal and not a FabFilter artifact, but 15% fail, including
            // DMGAudio EQuality, Eiosis AirEQ, Waves REQ 4 and Waves C6. It is a
            // verifier and a disagreement detector, never the generaliser.
            r.kind = Result::Kind::gesture;
            r.indices = moved;
            r.reason = juce::String (moved.size()) + " parameters moved in one gesture ("
                     + r.names.joinIntoString (", ") + "). On a curve-based EQ that is one node "
                       "drag moving frequency and gain together. Pick the one you meant; the rest "
                       "are recorded as co-moved. If you touched only one control, this may instead "
                       "be a macro, or the epsilon may be too small.";
            if (gestured.size() > 1)
                r.reason << " The plugin reported gestures on " << gestured.size()
                         << " of them, which does not single one out.";

            // The shape, stated and not interpreted. Lockstep is worth showing
            // because it is what a mirrored or linked pair would look like, and
            // it is equally what two parameters driven by one gesture look like.
            // The record says which was seen; it does not say which it was.
            r.reason << " Shape: " << (sameSign ? "all same direction" : "mixed directions")
                     << ", magnitudes "
                     << (r.magnitudeRatio > 0.0f
                            ? juce::String (r.magnitudeRatio, 2) + "x apart"
                            : juce::String ("not comparable"))
                     << ".";
        }

        return r;
    }

    /** Retroactive noise promotion. A thin baseline is provisional, so an index
        that keeps turning up across separate arm cycles WITH NO HUMAN EVIDENCE
        BEHIND ANY OF THEM is self-changing even though the baseline missed it.

        Runs on the MESSAGE THREAD, at delivery, not on the capture thread in
        classify() where it used to live. Two reasons:

          - The evidence probe reads the mouse ring against editor bounds and,
            later, the listener bank; both belong to the message thread.
          - This mutates mask->indices, which the poll loop reads. A result
            ends the capture (run() returns straight after posting delivery),
            and the next arm() joins the old thread before starting, so the
            mutation is sequenced strictly between polls -- but only from here.
            From classify() it interleaved with the row writer, which is how
            rows came to show "captured idx 2" and "mask [2]" at once.
    */
    void countTowardPromotion (const Result& r)
    {
        if (mask == nullptr || r.indices.isEmpty())
            return;

        for (int n = 0; n < r.indices.size(); ++n)
        {
            const int i = r.indices[n];

            if (humanEvidence != nullptr && humanEvidence (i, r))
                continue;               // a hand was on it: that is a capture, not noise

            const int seen = ++cycleAppearances[i];
            if (seen >= kPromoteAfterCycles && ! mask->indices.contains (i))
            {
                mask->indices.add (i);

                NoiseMask::Promotion p;
                p.index  = i;
                p.reason = "promoted after moving in " + juce::String (seen)
                             + " separate arm cycles with no human evidence behind any of them"
                             " (baseline was " + mask->method + ", "
                             + juce::String (mask->samples) + " samples)";
                mask->promotions.add (p);

                if (mask->method == "short_probe_clean")
                    mask->method = "promoted_only";
            }
        }
    }

    void deliver (const Result& r)
    {
        if (callback)
        {
            auto cb = callback;
            auto copy = r;
            juce::WeakReference<CaptureEngine> self (this);
            juce::MessageManager::callAsync ([self, cb, copy]
            {
                // Count BEFORE the callback: the callback writes the row, and
                // a promotion this result caused belongs in front of it, where
                // the owner can flush it as its own record.
                if (self != nullptr)
                    self->countTowardPromotion (copy);
                cb (copy);
            });
        }
    }

    Watchdog& watchdog;
    juce::AudioPluginInstance* instance = nullptr;
    Calibration calibration;
    NoiseMask* mask = nullptr;
    ResultFn callback;
    HumanEvidenceFn humanEvidence;
    ParamListenerBank* listeners = nullptr;
    std::vector<float> snapshot;
    std::map<int, int> cycleAppearances;
    juce::uint32 armedAt = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (CaptureEngine)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureEngine)
};

} // namespace ejmap
