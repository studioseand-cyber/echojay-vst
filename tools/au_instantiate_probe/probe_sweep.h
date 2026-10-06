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
//   - Every write says which mechanism landed it (landWrite): instack, pump, render (silence rendered after the pump
//     timed out), or unlanded. Every hold prints tone_frac, the share of output power at the test tone.
//   - Nothing is ever written back: the process exits after the sweep, and the instance dies with it.
#pragma once

#include "probe_write.h"
#include <array>
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
    // THE DETECTOR TEST (spec v1.2 `detector`, built 1 Oct): a two-tone signal, hz + hz2 at equal amplitude, at the SAME RMS as
    // the sine would have at the level - its peak is 3.01 dB higher (crest 6.02 dB vs the sine's 3.01). An RMS detector
    // reads it as the sine; a peak detector reaches the same GR 3.01 dB lower in level. `level` keeps meaning sine-peak dBFS.
    double hz2 = 0.0;      // 0 = sine; > 0 = two-tone at hz and hz2
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
        else if (k == "hz2") s.hz2 = v.getDoubleValue();
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
    double phase2 = 0.0, step2 = 0.0;     // the second tone of a two-tone signal (0 = sine only)
    int block, mainIn, mainOut, fedIn = 0;

    SweepRenderer (juce::AudioPluginInstance& proc, double sampleRate, int blockSize, double hz, double hz2 = 0.0)
        : p (proc), io (juce::jmax (2, proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()), blockSize),
          sr (sampleRate), step (juce::MathConstants<double>::twoPi * hz / sampleRate),
          step2 (hz2 > 0.0 ? juce::MathConstants<double>::twoPi * hz2 / sampleRate : 0.0), block (blockSize)
    {
        mainIn = mainInputChannels (proc); fedIn = fedInputChannels (proc);
        mainOut = proc.getBusCount (false) > 0 && proc.getBus (false, 0) != nullptr && proc.getBus (false, 0)->isEnabled()
                    ? proc.getBus (false, 0)->getNumberOfChannels() : 0;
    }

    struct Hold
    {
        std::vector<double> windowsDb;       // every window over the whole render, power mean over main outputs
        std::vector<double> chanDb;          // per main output channel, over the measured span
        double levelDb = -999.0;             // power mean over main outputs, over the measured span
        double inPeakDb = -999.0, inRmsDb = -999.0;
        double toneFrac = 0.0;               // share of the measured output power at the test tone (Goertzel), 0..1
        long long nonFinite = 0;
        // OUTPUT PEAKS over the measured span (5 Oct, limiter ceilings): the sample peak, and a true-peak estimate from 4x cubic
        // (Catmull-Rom) interpolation between consecutive samples - an approximation of ITU-R BS.1770's oversampled peak, said as such
        double outPeakDb = -999.0, outTruePeakDb = -999.0;
    };

    // One block of SILENCE through the plugin: the write escalation confirms while rendering silence, which builds
    // no compression history (ruled 29 Sep).
    void renderSilentBlock()
    {
        io.clear();
        midi.clear();
        p.processBlock (io, midi);
    }

    // Renders `seconds` of tone at `dbfs`; measures [measureFrom, seconds) and windows of `winS` from t = 0.
    Hold render (double dbfs, double seconds, double measureFrom, double winS)
    {
        Hold h;
        const double amp = std::pow (10.0, dbfs / 20.0);
        const long long total = (long long) std::llround (seconds * sr), from = (long long) std::llround (measureFrom * sr);
        const long long winN = juce::jmax (1LL, (long long) std::llround (winS * sr));
        std::vector<double> chanSs ((size_t) juce::jmax (1, mainOut), 0.0);
        double winSs = 0.0, inSs = 0.0, inPeak = 0.0; long long inWin = 0, measured = 0;
        double outPeak = 0.0, outTruePeak = 0.0;
        std::vector<std::array<double, 4>> last ((size_t) juce::jmax (1, mainOut), std::array<double, 4> { 0.0, 0.0, 0.0, 0.0 });   // the last four samples per channel for the interpolation
        // Goertzel at the tone over the measured span, per main output channel: is the output still the INPUT's tone?
        const double gcoef = 2.0 * std::cos (step), gcoef2 = step2 > 0.0 ? 2.0 * std::cos (step2) : 0.0;
        std::vector<double> gs1 ((size_t) juce::jmax (1, mainOut), 0.0), gs2 ((size_t) juce::jmax (1, mainOut), 0.0);
        std::vector<double> hs1 ((size_t) juce::jmax (1, mainOut), 0.0), hs2 ((size_t) juce::jmax (1, mainOut), 0.0);   // the second tone's Goertzel
        for (long long done = 0; done < total; done += block)
        {
            io.clear();
            for (int n = 0; n < block; ++n)
            {
                // Two-tone: each tone at amp / sqrt 2, so the RMS equals the sine's (amp^2/2) and the peak is 3.01 dB higher.
                const float v = step2 > 0.0 ? (float) (amp * (std::sin (phase) + std::sin (phase2)) / std::sqrt (2.0)) : (float) (amp * std::sin (phase));
                for (int ch = 0; ch < fedIn; ++ch) io.setSample (ch, n, v);
                phase += step;
                if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                if (step2 > 0.0) { phase2 += step2; if (phase2 > juce::MathConstants<double>::twoPi) phase2 -= juce::MathConstants<double>::twoPi; }
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
                    if (t >= from)
                    {
                        chanSs[(size_t) ch] += (double) d * d;
                        outPeak = juce::jmax (outPeak, (double) std::abs (d));
                        {
                            auto& q = last[(size_t) ch]; q[0] = q[1]; q[1] = q[2]; q[2] = q[3]; q[3] = (double) d;
                            for (int k = 1; k < 4; ++k)   // three points between q[1] and q[2] (Catmull-Rom), i.e. 4x
                            {
                                const double u = k / 4.0, a0 = q[1], a1 = 0.5 * (q[2] - q[0]), a2 = q[0] - 2.5 * q[1] + 2.0 * q[2] - 0.5 * q[3], a3 = 0.5 * (q[3] - q[0]) + 1.5 * (q[1] - q[2]);
                                outTruePeak = juce::jmax (outTruePeak, std::abs (((a3 * u + a2) * u + a1) * u + a0));
                            }
                            outTruePeak = juce::jmax (outTruePeak, (double) std::abs (d));
                        }
                        const double s0 = (double) d + gcoef * gs1[(size_t) ch] - gs2[(size_t) ch];
                        gs2[(size_t) ch] = gs1[(size_t) ch];
                        gs1[(size_t) ch] = s0;
                        if (step2 > 0.0) { const double h0 = (double) d + gcoef2 * hs1[(size_t) ch] - hs2[(size_t) ch]; hs2[(size_t) ch] = hs1[(size_t) ch]; hs1[(size_t) ch] = h0; }
                    }
                }
                if (mainOut > 0) ss /= mainOut;
                winSs += ss;
                if (t >= from) ++measured;
                const double x = step2 > 0.0 ? amp * (std::sin (phase - step * (block - n)) + std::sin (phase2 - step2 * (block - n))) / std::sqrt (2.0)
                                             : amp * std::sin (phase - step * (block - n));   // the input sample, for the input level (two-tone when set)
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
        {
            // Tone power per channel = 2 |X|^2 / N^2 (a sine of amplitude A gives |X| = A N / 2, power A^2 / 2); its
            // share of the total output power over the measured span.
            double tone = 0.0;
            for (int ch = 0; ch < mainOut; ++ch)
            {
                const double a = gs1[(size_t) ch], b = gs2[(size_t) ch];
                const double mag2 = a * a + b * b - gcoef * a * b;
                tone += measured > 0 ? 2.0 * mag2 / ((double) measured * (double) measured) : 0.0;
                if (step2 > 0.0) { const double a2 = hs1[(size_t) ch], b2 = hs2[(size_t) ch]; const double m2 = a2 * a2 + b2 * b2 - gcoef2 * a2 * b2;
                                   tone += measured > 0 ? 2.0 * m2 / ((double) measured * (double) measured) : 0.0; }   // both tones are "the input's tone"
            }
            const double total2 = measured > 0 ? all / (double) measured : 0.0;
            h.toneFrac = total2 > 0.0 ? juce::jlimit (0.0, 1.0, tone / total2) : 0.0;
        }
        h.inPeakDb = toDb (inPeak);
        h.outPeakDb = toDb (outPeak); h.outTruePeakDb = toDb (outTruePeak);
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

// LAND A WRITE, ESCALATING (ruled 29 Sep). Pump first (the verify's step 1, 500 ms); on timeout, confirm while
// rendering SILENCE for up to 500 ms more. Which mechanism landed it is printed with every write: "instack" (already
// true before any pump), "pump", "render", or "unlanded". Recorded per write, it settles write by write whether a
// plugin needs the message loop, wall time, or render cycles (the 2 Aug / 10 Aug disagreement).
struct Landing { double ms = -1.0; int slices = 0, blocks = 0; bool instack = false; float read = 0.0f; const char* by = "unlanded"; };
inline Landing landWrite (juce::AudioProcessorParameter& q, float want, SweepRenderer* r, float tol = 0.005f)
{
    Landing l;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const double ms = confirmCounted (q, want, l.read, l.slices, l.instack, 500, tol);
    if (ms >= 0) { l.ms = ms; l.by = l.instack ? "instack" : "pump"; return l; }
    if (r == nullptr) return l;
    const double t1 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t1 < 500.0)
    {
        r->renderSilentBlock();
        ++l.blocks;
        l.read = q.getValue();
        if (std::abs (l.read - want) <= tol) { l.ms = juce::Time::getMillisecondCounterHiRes() - t0; l.by = "render"; return l; }
    }
    return l;
}

inline void runSweep (juce::AudioPluginInstance& p, const SweepSpec& s, const RenderSpec& rs = {})
{
    auto ps = p.getParameters();
    if (! juce::isPositiveAndBelow (s.thr, ps.size()) || ps[s.thr] == nullptr)
    { std::printf ("refused no parameter at index %d (%d parameters)\n", s.thr, ps.size()); return; }
    std::printf ("sweep\tproto\t1\tthr\t%d\tname\t%s\tpositions\t%d\tlevels\t%d\n", s.thr,
                 clean (ps[s.thr]->getName (128)).toRawUTF8(), (int) s.norms.size(), (int) s.levels.size());
    std::printf ("spec\thz\t%.3f\thold_s\t%.3f\tdiscard_s\t%.3f\twin_s\t%.3f\tref_s\t%.3f\tmoving_db\t%.3f\tamplitude\tpeak_at_level\treset_per_hold\t%d\tsignal\t%s\thz2\t%.3f\n",
                 s.hz, s.holdS, s.discardS, s.winS, s.refS, s.movingDb, s.resetPerHold ? 1 : 0, s.hz2 > 0.0 ? "two_tone_same_rms" : "sine", s.hz2);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.hz, s.hz2);
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
        const auto l = landWrite (q, norm, &r);
        juce::String text; int reads = 0;
        stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext\t%s\n", idx, norm, l.ms,
                     l.read, l.by, l.blocks, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("refused set_unlanded %d\n", idx); return; }
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
            std::printf ("ref\t%.2f\tlevel_db\t%.4f\tin_peak_db\t%.4f\tin_rms_db\t%.4f\ttone_frac\t%.4f\tch\t%s\twin\t%s\tnonfinite\t%lld\n", L,
                         h.levelDb, h.inPeakDb, h.inRmsDb, h.toneFrac, joinDb (h.chanDb).toRawUTF8(), joinDb (h.windowsDb).toRawUTF8(),
                         h.nonFinite);
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
                h.levelDb = h2.levelDb; h.chanDb = h2.chanDb; h.toneFrac = h2.toneFrac; h.nonFinite += h2.nonFinite;
                doubled = 1;
            }
            const size_t m = h.windowsDb.size();
            const double finalMove = m >= 2 ? h.windowsDb[m - 1] - h.windowsDb[m - 2] : 0.0;
            std::printf ("%s\t%d\t%.2f\tlevel_db\t%.4f\tin_rms_db\t%.4f\ttone_frac\t%.4f\tch\t%s\tdoubled\t%d\tlast_move_db\t%.4f\tfinal_move_db\t%.4f\twin\t%s\tnonfinite\t%lld\tout_peak_db\t%.4f\tout_true_peak_db\t%.4f\n",
                         tag, k, L, h.levelDb, h.inRmsDb, h.toneFrac, joinDb (h.chanDb).toRawUTF8(), doubled, lastMove, finalMove,
                         joinDb (h.windowsDb).toRawUTF8(), h.nonFinite, h.outPeakDb, h.outTruePeakDb);
        }
    };

    for (int k = 0; k < (int) s.norms.size(); ++k)
    {
        stage ("position");
        const float want = s.norms[(size_t) k];
        t.setValueNotifyingHost (want);
        const auto l = landWrite (t, want, &r);
        if (l.ms < 0)
        {
            std::printf ("pos\t%d\tnorm\t%.6f\twrite_unlanded\tgetValue\t%.6f\tslices\t%d\trender_blocks\t%d\tlanded_by\tunlanded\n",
                         k, want, l.read, l.slices, l.blocks);
            continue;                                   // SKIPPED, never rendered
        }
        juce::String text; int reads = 0;
        const double tms = stableText (t, text, reads);
        std::printf ("pos\t%d\tnorm\t%.6f\tconfirm_ms\t%.2f\tslices\t%d\tinstack_match\t%d\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext_ms\t%.1f\treads\t%d\ttext\t%s\n",
                     k, want, l.ms, l.slices, l.instack ? 1 : 0, l.read, l.by, l.blocks, tms, reads, clean (text).toRawUTF8());
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
    SweepRenderer r (p, 48000.0, 512, 997.0);
    for (float n : norms)
    {
        q.setValueNotifyingHost (n);
        const auto l = landWrite (q, n, &r);
        juce::String text; int reads = 0;
        stableText (q, text, reads);
        std::printf ("at\t%.6f\t%s\tgetValue\t%.6f\tconfirm_ms\t%.1f\tlanded_by\t%s\ttext\t%s\n", n, l.ms < 0 ? "unlanded" : "landed",
                     l.read, l.ms, l.by, clean (text).toRawUTF8());
    }
    stage ("done");
}

} // namespace ejprobe
