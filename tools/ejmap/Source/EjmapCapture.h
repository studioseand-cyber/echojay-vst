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
        juce::StringArray promotions;   // retroactive additions, each with its reason

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
        */
        enum class Kind { captured, twins, gesture, tooMany, notAutomatable, stalled };

        Kind kind = Kind::notAutomatable;
        juce::Array<int> indices;

        /** Real parameter names, parallel to indices. A human cannot act on
            "[0, 2, 3, 7, 8]"; they can act on "Band 1 Freq, Band 1 Gain".
        */
        juce::StringArray names;
        juce::Array<float> deltas;
        juce::String reason;

        /** When the change was DETECTED. The mouse ring is queried against this
            rather than sampled at this moment, so ui_hint fidelity does not
            depend on the calibrated poll rate.
        */
        juce::uint32 detectedAtMs = 0;

        juce::String kindString() const
        {
            switch (kind)
            {
                case Kind::captured:       return "captured";
                case Kind::twins:          return "twins";
                case Kind::gesture:        return "gesture";
                case Kind::tooMany:        return "too_many";
                case Kind::notAutomatable: return "not_automatable";
                case Kind::stalled:        return "stalled";
            }
            return "not_automatable";
        }
    };

    using ResultFn = std::function<void (const Result&)>;

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

    /** An index seen moving in this many separate arm cycles is self-changing,
        whatever the baseline concluded. Pass two replaces this with "moved with
        no mouse-down behind it", which is the stronger signal.
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

        if (moved.size() > kTooManyMoved)
        {
            r.kind = Result::Kind::tooMany;
            r.indices = moved;
            r.reason = juce::String (moved.size()) + " parameters moved at once ("
                     + r.names.joinIntoString (", ").substring (0, 160)
                     + "): most likely a preset change or a modulation source rather than one "
                       "gesture. Discarded. The 8-parameter boundary is a heuristic, so a very "
                       "wide deliberate gesture would land here too.";
            noteCycle (moved);
            return r;
        }

        if (moved.size() == 1)
        {
            r.kind = Result::Kind::captured;
            r.indices = moved;
            r.reason = "exactly one parameter moved: " + r.names[0];
            noteCycle (moved);
            return r;
        }

        // Two or more. Same direction AND comparable magnitude is the channel
        // twin / linked pair signature; anything else is a macro or a bad
        // epsilon, and the human is asked rather than guessed at.
        bool sameSign = true;
        float lo = std::abs (deltas[0]), hi = lo;
        for (int i = 1; i < deltas.size(); ++i)
        {
            if ((deltas[i] > 0.0f) != (deltas[0] > 0.0f)) sameSign = false;
            lo = juce::jmin (lo, std::abs (deltas[i]));
            hi = juce::jmax (hi, std::abs (deltas[i]));
        }

        const bool comparable = lo > 0.0f && (hi / lo) <= 1.5f;

        if (sameSign && comparable)
        {
            r.kind = Result::Kind::twins;
            r.indices = moved;
            r.reason = juce::String (moved.size()) + " moved together, same direction, magnitudes "
                       "within 1.5x: channel twins or a linked pair. Confirm before use.";
        }
        else
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
        }

        noteCycle (moved);
        return r;
    }

    /** Retroactive noise promotion. A thin baseline is provisional, so an index
        that keeps turning up across separate arm cycles is self-changing even
        though the baseline missed it. Pass two replaces this with the stronger
        "moved with no mouse-down behind it".
    */
    void noteCycle (const juce::Array<int>& moved)
    {
        if (mask == nullptr)
            return;

        for (int i : moved)
        {
            const int n = ++cycleAppearances[i];
            if (n >= kPromoteAfterCycles && ! mask->indices.contains (i))
            {
                mask->indices.add (i);
                mask->promotions.add ("index " + juce::String (i) + " promoted to the noise mask "
                                      "after moving in " + juce::String (n) + " separate arm cycles "
                                      "(baseline was " + mask->method + ", "
                                      + juce::String (mask->samples) + " samples)");
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
            juce::MessageManager::callAsync ([cb, copy] { cb (copy); });
        }
    }

    Watchdog& watchdog;
    juce::AudioPluginInstance* instance = nullptr;
    Calibration calibration;
    NoiseMask* mask = nullptr;
    ResultFn callback;
    std::vector<float> snapshot;
    std::map<int, int> cycleAppearances;
    juce::uint32 armedAt = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureEngine)
};

} // namespace ejmap
