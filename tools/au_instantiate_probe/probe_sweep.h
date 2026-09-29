// probe_sweep.h - EchoJayProbe's SWEEP MODE: spec section 4, the compressor threshold sweep (feat/ejmap-cert, 29 Sep).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --sweep thr=<index> norms=<n0,n1,...> [levels=-24,-12,-6]
//                [hz=997] [hold=1.5] [discard=0.75] [win=0.25] [ref=2.0] [moving_db=0.1] [reset=0|1] [set=<idx>:<norm>,...]
//
// IT MEASURES; IT DOES NOT DERIVE (decision D2). EJ Map chooses the positions (norms=), the preconditions (set=) and the
// still-moving tolerance (moving_db=), and derives reduction, sense, engage and the dB-equivalent map from these lines.
// Changing any of that never costs a rebuild and a re-sign of this binary.
//
// THE PROCEDURE (decided 28 Sep; EJMAP_CERT_DECISIONS.md section 3, spec 4.3):
//   - Stimulus: a phase-continuous sine at hz= on the MAIN input bus, sidechains enabled and silent (probe_render.h).
//     The amplitude is 10^(level/20): the sine's PEAK sits at the level (the peak convention; its RMS is 3.0103 dB
//     lower). The probe prints both, and the convention is EJ Map's to record.
//   - Reference: threshold untouched at its instantiate value, ref= seconds per level, levels in the given order.
//   - Positions OUTER, levels INNER: one write per position, then one hold per level (16 writes, not 48).
//   - Each write through the THREE-STEP VERIFY (probe_write.h): (1) property confirm, 500 ms bound, and on timeout the
//     position is SKIPPED ("write_unlanded"), never rendered; (2) display stable-read; (3) after the position's holds,
//     getValue() and the text again - if either moved, the write landed during the render, and the position's holds
//     are discarded and rendered ONCE more ("rerender"). Moved again prints "unsteady" and EJ Map skips it.
//   - Each hold: hold= seconds, the first discard= seconds thrown away, the level read over the rest. The whole hold is
//     also printed in win= windows, so any other window rule can be re-computed without re-measuring.
//   - STILL MOVING (spec 4.7): if the last window differs from the one before it by more than moving_db=, the hold is
//     DOUBLED ONCE (one more hold= seconds rendered straight on, the level read over the new last hold-discard
//     seconds) and the line says so. Still moving after that is printed, not decided.
//   - Levels are the POWER MEAN over the main output bus's channels; each channel is printed too.
//   - reset=1: AudioUnitReset (juce reset()) before EVERY hold and every reference render, so no reading inherits the
//     previous one's envelope. Added 29 Sep after the first townhouse run: walking hard -> soft, the soft position was
//     still RELEASING from the hard one 3 s later (auto release), and the still-moving doubling could not absorb it.
//     Parameters are untouched by a reset. Off by default: the decided procedure has none, and EJ Map chooses.
//   - ONE PROCESS PER POSITION (ruled 29 Sep): EJ Map runs a reference-only process (norms= empty) and then one
//     process per position with ref=0, the levels quiet to loud inside it. Walking positions inside one process let
//     every quiet reading inherit a loud one's release (townhouse: >3 s of auto release, arms A and B).
//   - Nothing is ever written back: the process exits after the sweep, and the instance dies with it.
#pragma once

#include "probe_write.h"
#include <map>

namespace ejprobe
{

struct SweepSpec
{
    int thr = -1;
    std::vector<float> norms;
    std::vector<double> levels { -24.0, -12.0, -6.0 };
    double hz = 997.0, holdS = 1.5, discardS = 0.75, winS = 0.25, refS = 2.0, movingDb = 0.1;
    bool resetPerHold = false;
    std::vector<std::pair<int, float>> sets;
};

inline bool parseSweepArgs (int argc, char** argv, int first, SweepSpec& s, juce::String& why)
{
    auto doubles = [] (const juce::String& v) { std::vector<double> o; for (auto& t : juce::StringArray::fromTokens (v, ",", "")) o.push_back (t.getDoubleValue()); return o; };
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if (k == "thr") s.thr = v.getIntValue();
        else if (k == "norms") { s.norms.clear(); for (double d : doubles (v)) s.norms.push_back ((float) d); }
        else if (k == "levels") s.levels = doubles (v);
        else if (k == "hz") s.hz = v.getDoubleValue();
        else if (k == "hold") s.holdS = v.getDoubleValue();
        else if (k == "discard") s.discardS = v.getDoubleValue();
        else if (k == "win") s.winS = v.getDoubleValue();
        else if (k == "ref") s.refS = v.getDoubleValue();
        else if (k == "moving_db") s.movingDb = v.getDoubleValue();
        else if (k == "reset") s.resetPerHold = v.getIntValue() != 0;
        else if (k == "set")
            for (auto& t : juce::StringArray::fromTokens (v, ",", ""))
                s.sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(),
                                    (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        else { why = "unknown sweep argument '" + a + "'"; return false; }
    }
    if (s.thr < 0) { why = "no thr=<index>"; return false; }
    if (s.norms.empty() && s.refS <= 0) { why = "nothing to measure: no norms= and ref=0"; return false; }
    if (s.levels.empty() || s.holdS <= s.discardS || s.winS <= 0) { why = "bad levels/hold/discard/win"; return false; }
    return true;
}

// A tone renderer that measures the MAIN output bus as a power mean, in windows of whole samples.
struct SweepRenderer
{
    juce::AudioPluginInstance& p;
    juce::AudioBuffer<float> io;
    juce::MidiBuffer midi;
    double sr, phase = 0.0, step;
    int block, mainIn, mainOut;

    SweepRenderer (juce::AudioPluginInstance& proc, double sampleRate, int blockSize, double hz)
        : p (proc), io (juce::jmax (2, proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()), blockSize),
          sr (sampleRate), step (juce::MathConstants<double>::twoPi * hz / sampleRate), block (blockSize)
    {
        mainIn = mainInputChannels (proc);
        mainOut = proc.getBusCount (false) > 0 && proc.getBus (false, 0) != nullptr && proc.getBus (false, 0)->isEnabled()
                    ? proc.getBus (false, 0)->getNumberOfChannels() : 0;
    }

    struct Hold
    {
        std::vector<double> windowsDb;       // every window over the whole render, power mean over main outputs
        std::vector<double> chanDb;          // per main output channel, over the measured span
        double levelDb = -999.0;             // power mean over main outputs, over the measured span
        double inPeakDb = -999.0, inRmsDb = -999.0;
        long long nonFinite = 0;
    };

    // Renders `seconds` of tone at `dbfs`; measures [measureFrom, seconds) and windows of `winS` from t = 0.
    Hold render (double dbfs, double seconds, double measureFrom, double winS)
    {
        Hold h;
        const double amp = std::pow (10.0, dbfs / 20.0);
        const long long total = (long long) std::llround (seconds * sr), from = (long long) std::llround (measureFrom * sr);
        const long long winN = juce::jmax (1LL, (long long) std::llround (winS * sr));
        std::vector<double> chanSs ((size_t) juce::jmax (1, mainOut), 0.0);
        double winSs = 0.0, inSs = 0.0, inPeak = 0.0; long long inWin = 0, measured = 0;
        for (long long done = 0; done < total; done += block)
        {
            io.clear();
            for (int n = 0; n < block; ++n)
            {
                const float v = (float) (amp * std::sin (phase));
                for (int ch = 0; ch < mainIn; ++ch) io.setSample (ch, n, v);
                phase += step;
                if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            }
            midi.clear();
            p.processBlock (io, midi);
            for (int n = 0; n < block && done + n < total; ++n)
            {
                const long long t = done + n;
                double ss = 0.0;
                for (int ch = 0; ch < mainOut; ++ch)
                {
                    const float d = io.getSample (ch, n);
                    if (! std::isfinite (d)) { ++h.nonFinite; continue; }
                    ss += (double) d * d;
                    if (t >= from) chanSs[(size_t) ch] += (double) d * d;
                }
                if (mainOut > 0) ss /= mainOut;
                winSs += ss;
                if (t >= from) ++measured;
                const double x = amp * std::sin (phase - step * (block - n));   // the input sample, for the input level
                inSs += x * x; inPeak = juce::jmax (inPeak, std::abs (x));
                if (++inWin == winN) { h.windowsDb.push_back (toDb (std::sqrt (winSs / winN))); winSs = 0.0; inWin = 0; }
            }
        }
        double all = 0.0;
        for (int ch = 0; ch < mainOut; ++ch)
        {
            h.chanDb.push_back (toDb (measured > 0 ? std::sqrt (chanSs[(size_t) ch] / measured) : 0.0));
            all += chanSs[(size_t) ch];
        }
        h.levelDb = toDb (measured > 0 && mainOut > 0 ? std::sqrt (all / (measured * (double) mainOut)) : 0.0);
        h.inPeakDb = toDb (inPeak);
        h.inRmsDb = toDb (total > 0 ? std::sqrt (inSs / total) : 0.0);
        return h;
    }
};

inline juce::String joinDb (const std::vector<double>& v)
{
    juce::String s;
    for (size_t i = 0; i < v.size(); ++i) s << (i ? "," : "") << juce::String (v[i], 4);
    return s;
}

// (1) of the verify, instrumented for the sweep: the in-stack read, then pump slices until getValue() matches.
// Returns ms (-1 on timeout); `slices` counts the pumps it took; `instack` says whether getValue() matched with none.
inline double confirmCounted (juce::AudioProcessorParameter& q, float want, float& lastRead, int& slices, bool& instack,
                              int boundMs = 500, float tol = 0.005f)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    lastRead = q.getValue();
    instack = std::abs (lastRead - want) <= tol;
    slices = 0;
    do   // pumps at least once even when the in-stack read already matches (confirmProperty's rule, probe_write.h)
    {
        pumpSlice(); ++slices;
        lastRead = q.getValue();
        if (std::abs (lastRead - want) <= tol) return juce::Time::getMillisecondCounterHiRes() - t0;
    }
    while (juce::Time::getMillisecondCounterHiRes() - t0 < boundMs);
    return -1.0;
}

inline void runSweep (juce::AudioPluginInstance& p, const SweepSpec& s, const RenderSpec& rs = {})
{
    auto ps = p.getParameters();
    if (! juce::isPositiveAndBelow (s.thr, ps.size()) || ps[s.thr] == nullptr)
    { std::printf ("refused no parameter at index %d (%d parameters)\n", s.thr, ps.size()); return; }
    std::printf ("sweep\tproto\t1\tthr\t%d\tname\t%s\tpositions\t%d\tlevels\t%d\n", s.thr,
                 clean (ps[s.thr]->getName (128)).toRawUTF8(), (int) s.norms.size(), (int) s.levels.size());
    std::printf ("spec\thz\t%.3f\thold_s\t%.3f\tdiscard_s\t%.3f\twin_s\t%.3f\tref_s\t%.3f\tmoving_db\t%.3f\tamplitude\tpeak_at_level\treset_per_hold\t%d\n",
                 s.hz, s.holdS, s.discardS, s.winS, s.refS, s.movingDb, s.resetPerHold ? 1 : 0);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.hz);
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\n", r.mainIn, r.mainOut, p.getLatencySamples());
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); return; }

    // EVERY PARAMETER AS INSTANTIATED, before anything is written: the ratio EJ Map derives with is read from here.
    for (int i = 0; i < ps.size(); ++i)
        if (auto* q = ps[i])
            std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(),
                         clean (q->getCurrentValueAsText()).toRawUTF8());

    // PRECONDITIONS (spec 4.2), chosen by EJ Map, each confirmed.
    for (const auto& [idx, norm] : s.sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); return; }
        auto& q = *ps[idx];
        q.setValueNotifyingHost (norm);
        float read = 0; int slices = 0; bool instack = false;
        const double ms = confirmCounted (q, norm, read, slices, instack);
        juce::String text; int reads = 0;
        stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\ttext\t%s\n", idx, norm, ms, read, clean (text).toRawUTF8());
        if (ms < 0) { std::printf ("refused set_unlanded %d\n", idx); return; }
    }

    auto& t = *ps[s.thr];
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    double audioS = 0.0;

    if (s.refS > 0)
    {
        stage ("reference");
        juce::String text; int reads = 0;
        stableText (t, text, reads);
        std::printf ("refpos\tval\t%.6f\ttext\t%s\n", t.getValue(), clean (text).toRawUTF8());
        for (double L : s.levels)
        {
            if (s.resetPerHold) p.reset();
            const auto h = r.render (L, s.refS, s.refS - (s.holdS - s.discardS), s.winS);
            audioS += s.refS;
            std::printf ("ref\t%.2f\tlevel_db\t%.4f\tin_peak_db\t%.4f\tin_rms_db\t%.4f\tch\t%s\twin\t%s\tnonfinite\t%lld\n", L,
                         h.levelDb, h.inPeakDb, h.inRmsDb, joinDb (h.chanDb).toRawUTF8(), joinDb (h.windowsDb).toRawUTF8(), h.nonFinite);
        }
    }

    auto holdAll = [&] (int k, const char* tag)
    {
        for (double L : s.levels)
        {
            if (s.resetPerHold) p.reset();
            auto h = r.render (L, s.holdS, s.discardS, s.winS);
            audioS += s.holdS;
            const size_t n = h.windowsDb.size();
            const double lastMove = n >= 2 ? h.windowsDb[n - 1] - h.windowsDb[n - 2] : 0.0;
            int doubled = 0;
            if (std::abs (lastMove) > s.movingDb)
            {
                // DOUBLED ONCE: render straight on, and read the level over the new last (hold - discard) seconds.
                auto h2 = r.render (L, s.holdS, s.discardS, s.winS);   // measured over its last (hold - discard) s
                audioS += s.holdS;
                for (double w : h2.windowsDb) h.windowsDb.push_back (w);
                h.levelDb = h2.levelDb; h.chanDb = h2.chanDb; h.nonFinite += h2.nonFinite;
                doubled = 1;
            }
            const size_t m = h.windowsDb.size();
            const double finalMove = m >= 2 ? h.windowsDb[m - 1] - h.windowsDb[m - 2] : 0.0;
            std::printf ("%s\t%d\t%.2f\tlevel_db\t%.4f\tin_rms_db\t%.4f\tch\t%s\tdoubled\t%d\tlast_move_db\t%.4f\tfinal_move_db\t%.4f\twin\t%s\tnonfinite\t%lld\n",
                         tag, k, L, h.levelDb, h.inRmsDb, joinDb (h.chanDb).toRawUTF8(), doubled, lastMove, finalMove,
                         joinDb (h.windowsDb).toRawUTF8(), h.nonFinite);
        }
    };

    for (int k = 0; k < (int) s.norms.size(); ++k)
    {
        stage ("position");
        const float want = s.norms[(size_t) k];
        t.setValueNotifyingHost (want);
        float read = 0; int slices = 0; bool instack = false;
        const double ms = confirmCounted (t, want, read, slices, instack);
        if (ms < 0)
        {
            std::printf ("pos\t%d\tnorm\t%.6f\twrite_unlanded\tgetValue\t%.6f\tslices\t%d\n", k, want, read, slices);
            continue;                                   // SKIPPED, never rendered
        }
        juce::String text; int reads = 0;
        const double tms = stableText (t, text, reads);
        std::printf ("pos\t%d\tnorm\t%.6f\tconfirm_ms\t%.2f\tslices\t%d\tinstack_match\t%d\tgetValue\t%.6f\ttext_ms\t%.1f\treads\t%d\ttext\t%s\n",
                     k, want, ms, slices, instack ? 1 : 0, read, tms, reads, clean (text).toRawUTF8());
        const float preVal = t.getValue();
        const juce::String preText = t.getCurrentValueAsText();
        holdAll (k, "hold");
        auto movedNow = [&] { return std::abs (t.getValue() - preVal) > 0.005f || t.getCurrentValueAsText() != preText; };
        if (movedNow())
        {
            std::printf ("reread\t%d\tMOVED\tpre\t%.6f\t%s\tpost\t%.6f\t%s\n", k, preVal, clean (preText).toRawUTF8(),
                         t.getValue(), clean (t.getCurrentValueAsText()).toRawUTF8());
            const float preVal2 = t.getValue(); const juce::String preText2 = t.getCurrentValueAsText();
            holdAll (k, "rerender");
            const bool again = std::abs (t.getValue() - preVal2) > 0.005f || t.getCurrentValueAsText() != preText2;
            std::printf ("reread\t%d\t%s\n", k, again ? "unsteady" : "steady_after_rerender");
        }
        else std::printf ("reread\t%d\tsteady\n", k);
    }
    std::printf ("timing\twall_ms\t%.1f\taudio_s\t%.3f\n", juce::Time::getMillisecondCounterHiRes() - t0, audioS);
    stage ("done");
}

// --text-at-norms <index> <n0,n1,...>: the control's OWN TEXT at each normalised value, each write confirmed (the verify's
// step 1) and read stable (step 2). It measures; EJ Map chooses (spec 4.2's ratio raise reads its grid from here, so the
// position is picked from what the plugin prints, and the sweep then reads the ratio back again after its own write).
inline void runTextAtNorms (juce::AudioPluginInstance& p, int index, const std::vector<float>& norms)
{
    auto ps = p.getParameters();
    if (! juce::isPositiveAndBelow (index, ps.size()) || ps[index] == nullptr)
    { std::printf ("refused no parameter at index %d (%d parameters)\n", index, ps.size()); return; }
    auto& q = *ps[index];
    std::printf ("textat\tproto\t1\tindex\t%d\tname\t%s\tpoints\t%d\tinstantiated\t%.6f\t%s\n", index,
                 clean (q.getName (128)).toRawUTF8(), (int) norms.size(), q.getValue(), clean (q.getCurrentValueAsText()).toRawUTF8());
    for (float n : norms)
    {
        q.setValueNotifyingHost (n);
        float read = 0; int slices = 0; bool instack = false;
        const double ms = confirmCounted (q, n, read, slices, instack);
        juce::String text; int reads = 0;
        stableText (q, text, reads);
        std::printf ("at\t%.6f\t%s\tgetValue\t%.6f\tconfirm_ms\t%.1f\ttext\t%s\n", n, ms < 0 ? "unlanded" : "landed", read, ms,
                     clean (text).toRawUTF8());
    }
    stage ("done");
}

} // namespace ejprobe
