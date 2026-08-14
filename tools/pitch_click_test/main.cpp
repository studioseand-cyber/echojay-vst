/*
    pitch_click_test — find DISCONTINUITIES in the real host path and say what
    was happening at each one.

    A click is a step in the waveform, so it is measurable directly. This runs
    the real processor over a real vocal, locates every sample where the output
    jumps far more than the dry does at the same instant, and then labels each
    one with the state at that moment: voiced flag change, hop boundary, block
    boundary, note-change reset, gap resume, target step.

    The labelling uses a PARALLEL detector and corrector driven with the same
    audio and the same settings. They are the same classes the processor runs,
    fed identically, so their hop positions and decisions are the processor's.
*/
#include <JuceHeader.h>
#include "EedPitchProcessor.h"
#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include <cstdio>
#include <random>
#include <vector>

using echojay::PitchEngine;
using echojay::PitchCorrect;

namespace {

struct HopInfo
{
    uint64_t pos = 0;
    bool  voiced = false;
    float f0 = 0.0f, target = 0.0f;
    bool  noteChange = false, gapResume = false;
    bool  voicedChanged = false;
};

void configure (EedPitchProcessor& p, double fs, int maxBlock)
{
    p.setRateAndBufferSizeDetails (fs, maxBlock);
    p.prepareToPlay (fs, maxBlock);
}

std::vector<float> runProcessor (const std::vector<float>& src, double fs,
                                 int maxBlock, std::function<int()> nextBlock,
                                 std::vector<uint64_t>& blockEdges, int& latency,
                                 std::vector<uint64_t>* reseeds = nullptr,
                                 std::vector<uint64_t>* resets = nullptr,
                                 std::vector<echojay::PsolaEngine::DebugGrain>* grainsOut = nullptr,
                                 std::vector<uint32_t>* winHistOut = nullptr,
                                 float* winMinOut = nullptr)
{
    EedPitchProcessor p;
    configure (p, fs, maxBlock);
    latency = p.getLatencySamples();
    p.shifter (0).debugRecordPhaseEvents (true);

    std::vector<float> out (src.size(), 0.0f);
    juce::AudioBuffer<float> buf (2, maxBlock);
    juce::MidiBuffer midi;
    size_t pos = 0;
    while (pos < src.size())
    {
        int n = juce::jlimit (1, maxBlock, nextBlock());
        n = (int) juce::jmin ((size_t) n, src.size() - pos);
        buf.setSize (2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i) buf.getWritePointer (ch)[i] = src[pos + (size_t) i];
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) out[pos + (size_t) i] = buf.getReadPointer (0)[i];
        pos += (size_t) n;
        blockEdges.push_back (pos);
    }
    if (reseeds) *reseeds = p.shifter (0).debugReseeds();
    if (resets)  *resets  = p.shifter (0).debugCursorResets();
    if (grainsOut) *grainsOut = p.shifter (0).debugGrains();
    if (winHistOut) { *winHistOut = p.shifter (0).debugWinHist(); *winMinOut = p.shifter (0).debugWinMin(); }
    return out;
}

// The same detector and corrector the processor runs, fed the same audio.
std::vector<HopInfo> traceDecisions (const std::vector<float>& src, double fs)
{
    PitchEngine det;
    det.prepare (fs, 2048);
    det.setVoiceType (PitchEngine::kAltoTenor);
    det.setTracking (PitchEngine::kNormal);

    PitchCorrect corr;
    corr.prepare (fs, 512);
    corr.initDegrees();                       // chromatic, the shipped default
    corr.setRetuneMs (120.0f); corr.setFlex (55.0f); corr.setHumanize (60.0f);
    corr.setIgnoreVibrato (true); corr.setNaturalVibrato (100.0f);
    corr.reset();

    std::vector<HopInfo> hops;
    const int B = 512;
    bool prevVoiced = false;
    uint32_t prevNC = 0, prevGR = 0;
    PitchEngine::HopEvent ev[64];

    for (size_t p = 0; p + B <= src.size(); p += B)
    {
        det.process (src.data() + p, nullptr, B);
        const int n = det.drainHops (ev, 64);
        for (int i = 0; i < n; ++i)
        {
            HopInfo h;
            h.pos = ev[i].inputPos; h.voiced = ev[i].voiced; h.f0 = ev[i].f0Hz;
            h.target = corr.process (ev[i].f0Hz, ev[i].voiced, det.hopMs());
            h.noteChange = corr.noteChanges() != prevNC;
            h.gapResume  = corr.gapResumes()  != prevGR;
            h.voicedChanged = ev[i].voiced != prevVoiced;
            prevNC = corr.noteChanges(); prevGR = corr.gapResumes();
            prevVoiced = ev[i].voiced;
            hops.push_back (h);
        }
    }
    return hops;
}

} // namespace

int main (int argc, char* argv[])
{
    if (argc < 2) { std::printf ("usage: pitch_click_test <wav>\n"); return 1; }

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr) { std::printf ("could not read %s\n", argv[1]); return 1; }

    const double fs = rd->sampleRate;
    const int numS = (int) juce::jmin ((juce::int64) rd->lengthInSamples,
                                       (juce::int64) (fs * 60.0));
    juce::AudioBuffer<float> in (2, numS);
    rd->read (&in, 0, numS, 0, true, true);

    std::vector<float> src ((size_t) numS);
    for (int i = 0; i < numS; ++i)
        src[(size_t) i] = 0.5f * (in.getReadPointer (0)[i]
                                  + (in.getNumChannels() > 1 ? in.getReadPointer (1)[i]
                                                             : in.getReadPointer (0)[i]));

    std::printf ("file %s: %.0f Hz, %.1f s\n\n", f.getFileName().toRawUTF8(), fs, numS / fs);

    int latency = 0;
    std::vector<uint64_t> edges;
    std::vector<uint64_t> reseeds, resets;
    std::vector<echojay::PsolaEngine::DebugGrain> grains;
    std::vector<uint32_t> winHist; float winMin = 0.0f;
    auto out = runProcessor (src, fs, 2048, [] { return 512; }, edges, latency,
                             &reseeds, &resets, &grains, &winHist, &winMin);
    const auto hops = traceDecisions (src, fs);

    // THE ALIGNMENT THAT MATTERS. The shifter does not act on a hop's f0 at
    // the hop's position: PsolaEngine back-dates the f0 track by pitchLag, so a
    // hop at input P changes the shifter's f0 at P - pitchLag. Comparing a
    // click against P directly (and with a +/-8 window) could never see a
    // voiced seam - it would be looking about 1000 samples away from it.
    PitchEngine lagProbe;
    lagProbe.prepare (fs, 2048);
    lagProbe.setVoiceType (PitchEngine::kAltoTenor);
    const int pitchLag = lagProbe.pitchLagFor (PitchEngine::kAltoTenor);

    std::printf ("latency %d samples; pitchLag %d samples; %zu hops traced\n",
                 latency, pitchLag, hops.size());

    // ---- locate discontinuities ------------------------------------------
    // NOT "a big step" - a pitch-shifted vocal has big steps everywhere, and a
    // 12x-median threshold flagged 1432 per second, i.e. ordinary waveform
    // slope. A CLICK is a sample that breaks the local trend: predict it by
    // linear extrapolation from the two before it, and a discontinuity shows as
    // a prediction error far outside the local error scale.
    auto predictionError = [] (const std::vector<float>& x, int i) -> double
    {
        const double pred = 2.0 * (double) x[(size_t) (i - 1)] - (double) x[(size_t) (i - 2)];
        return std::abs ((double) x[(size_t) i] - pred);
    };

    std::vector<double> eOut ((size_t) numS, 0.0), eDry ((size_t) numS, 0.0);
    for (int i = 2; i < numS; ++i)
    {
        eOut[(size_t) i] = predictionError (out, i);
        eDry[(size_t) i] = predictionError (src, i);
    }

    // Local scale: median prediction error over a window either side, so a
    // loud passage is not automatically "clicky" and a quiet one is not immune.
    const int kWin = 512;
    auto localScale = [&] (const std::vector<double>& e, int i)
    {
        const int lo = juce::jmax (2, i - kWin), hi = juce::jmin (numS - 1, i + kWin);
        std::vector<double> w (e.begin() + lo, e.begin() + hi);
        std::nth_element (w.begin(), w.begin() + w.size() / 2, w.end());
        return w[w.size() / 2];
    };

    struct Click { int at; double err, scale, dryErr; };
    std::vector<Click> clicks;
    const double kFactor = 20.0;         // 20x the local error scale
    for (int i = 2; i < numS; ++i)
    {
        if (i % 8 != 0 && eOut[(size_t) i] < 0.02) continue;   // cheap prefilter
        const double sc = localScale (eOut, i);
        if (sc <= 0.0) continue;
        if (eOut[(size_t) i] < kFactor * sc) continue;

        // The dry must NOT have a discontinuity here - otherwise it is the
        // source's own transient, faithfully reproduced.
        const double dsc = localScale (eDry, i);
        if (dsc > 0.0 && eDry[(size_t) i] > kFactor * dsc) continue;

        if (! clicks.empty() && i - clicks.back().at < 16) continue;   // one per event
        clicks.push_back ({ i, eOut[(size_t) i], sc, eDry[(size_t) i] });
    }

    std::printf ("CLICKS FOUND: %zu  (%.2f per second) at %.0fx local scale\n\n",
                 clicks.size(), clicks.size() / (numS / fs), kFactor);

    // ---- label each one ---------------------------------------------------
    int nVoiceChange = 0, nHopExact = 0, nBlock = 0, nNote = 0, nGap = 0,
        nTargetStep = 0, nBare = 0;
    const int kNear = 8;          // hops are ~128 apart; 64 matched everything

    std::printf ("%-10s %9s %9s  %s\n", "time", "err", "x scale", "what was happening");
    for (size_t c = 0; c < clicks.size(); ++c)
    {
        const auto& k = clicks[c];
        // Position in the SHIFTER's timeline: undo the output latency, then
        // undo the f0 back-dating, so this can be compared against hop
        // positions directly.
        const int64_t inPos = (int64_t) k.at - latency + pitchLag;

        juce::StringArray why;
        bool vc = false, hopNear = false, note = false, gap = false, tstep = false;
        int64_t nearestHop = 1 << 30;
        for (size_t i = 0; i < hops.size(); ++i)
        {
            const int64_t d = (int64_t) hops[i].pos - inPos;
            if (std::llabs (d) < std::llabs (nearestHop)) nearestHop = d;
            if (std::llabs (d) > kNear) continue;
            hopNear = true;
            if (hops[i].voicedChanged) vc = true;
            if (hops[i].noteChange)    note = true;
            if (hops[i].gapResume)     gap = true;
            if (i > 0 && hops[i].target > 0 && hops[i - 1].target > 0
                && std::abs (1200.0 * std::log2 (hops[i].target / hops[i - 1].target)) > 20.0)
                tstep = true;
        }
        bool blockEdge = false;
        for (auto e : edges)
            if (std::llabs ((int64_t) e - (int64_t) k.at) <= 2) { blockEdge = true; break; }

        if (vc)        { why.add ("VOICED FLAG CHANGED"); ++nVoiceChange; }
        if (note)      { why.add ("note-change reset");   ++nNote; }
        if (gap)       { why.add ("gap resume");          ++nGap; }
        if (tstep)     { why.add ("target step >20c");    ++nTargetStep; }
        if (blockEdge) { why.add ("block boundary");      ++nBlock; }
        if (hopNear)   { ++nHopExact; if (why.isEmpty()) why.add ("hop boundary"); }
        if (why.isEmpty())
        { why.add ("unexplained (nearest hop " + juce::String ((int) nearestHop) + ")"); ++nBare; }

        if (c < 30)
            std::printf ("%9.4f %9.4f %9.1f  %s\n", k.at / fs, k.err,
                         k.scale > 0 ? k.err / k.scale : 0.0,
                         why.joinIntoString (", ").toRawUTF8());
    }
    if (clicks.size() > 30) std::printf ("  ... %zu more\n", clicks.size() - 30);

    // Where do clicks sit RELATIVE to a hop? With hops every ~128 samples, a
    // uniformly scattered click lands within 8 of one about 12% of the time -
    // so the base rate has to be stated or "39% near a hop" means nothing.
    {
        std::vector<int> hist (9, 0);
        int hopSpacing = 0;
        if (hops.size() > 2) hopSpacing = (int) (hops[2].pos - hops[1].pos);
        for (const auto& k : clicks)
        {
            const int64_t inPos = (int64_t) k.at - latency + pitchLag;
            int64_t nearest = 1 << 30;
            for (const auto& h : hops)
            {
                const int64_t d = std::llabs ((int64_t) h.pos - inPos);
                if (d < nearest) nearest = d;
            }
            const int bucket = (int) juce::jlimit ((int64_t) 0, (int64_t) 8, nearest / 8);
            ++hist[(size_t) bucket];
        }
        std::printf ("\n=== distance from each click to the nearest hop (spacing %d) ===\n", hopSpacing);
        for (int b = 0; b < 9; ++b)
            std::printf ("  %2d-%2d samples : %4d  (%.1f%%)%s\n", b * 8, b * 8 + 7, hist[(size_t) b],
                         100.0 * hist[(size_t) b] / juce::jmax (1, (int) clicks.size()),
                         b == 0 ? "   <- base rate if scattered would be ~12%" : "");
    }

    // Do the clicks sit on GRAIN-PHASE events?
    {
        int nearReseed = 0, nearReset = 0;
        for (const auto& k : clicks)
        {
            const int64_t q = (int64_t) k.at - latency;      // shifter timeline
            for (auto r : reseeds) if (std::llabs ((int64_t) r - q) <= 48) { ++nearReseed; break; }
            for (auto r : resets)  if (std::llabs ((int64_t) r - q) <= 48) { ++nearReset;  break; }
        }
        std::printf ("\n=== grain-phase events (the shifter's own bookkeeping) ===\n");
        std::printf ("  epoch re-seeds recorded            : %zu\n", reseeds.size());
        std::printf ("  synthesis-cursor resets recorded   : %zu\n", resets.size());
        std::printf ("  clicks within 48 smp of a re-seed  : %d / %zu (%.1f%%)\n",
                     nearReseed, clicks.size(), 100.0 * nearReseed / juce::jmax (1, (int) clicks.size()));
        std::printf ("  clicks within 48 smp of a reset    : %d / %zu (%.1f%%)\n",
                     nearReset, clicks.size(), 100.0 * nearReset / juce::jmax (1, (int) clicks.size()));
    }

    // WHAT CHANGES AT A HOP that could step the output? The f0 handed to the
    // shifter. A new f0 changes Ta, which changes the grain LENGTH and the
    // open-loop gain sqrt(Ts/Ta) - so a jump in f0 between consecutive hops
    // changes how much amplitude the overlap-add sums to, a few samples later
    // when the next grain lands. If that is the mechanism, clicks will sit
    // after hops where f0 moved a lot, and will be AMPLITUDE steps.
    {
        auto centsAt = [&] (size_t i) -> double
        {
            if (i == 0 || i >= hops.size()) return 0.0;
            if (hops[i].f0 <= 0 || hops[i - 1].f0 <= 0) return 0.0;
            return std::abs (1200.0 * std::log2 (hops[i].f0 / hops[i - 1].f0));
        };

        // Base distribution of per-hop f0 change, over voiced hops.
        std::vector<double> allJumps;
        for (size_t i = 1; i < hops.size(); ++i)
            if (hops[i].voiced && hops[i - 1].voiced) allJumps.push_back (centsAt (i));
        std::sort (allJumps.begin(), allJumps.end());

        std::vector<double> clickJumps;
        int ampSteps = 0;
        for (const auto& k : clicks)
        {
            const int64_t q = (int64_t) k.at - latency + pitchLag;
            size_t best = 0; int64_t bestD = 1 << 30;
            for (size_t i = 1; i < hops.size(); ++i)
            {
                const int64_t d = std::llabs ((int64_t) hops[i].pos - q);
                if (d < bestD) { bestD = d; best = i; }
            }
            clickJumps.push_back (centsAt (best));

            // Amplitude step, or phase step? Compare RMS either side.
            double a = 0.0, b = 0.0; int na = 0, nb = 0;
            for (int j = 1; j <= 64; ++j)
            {
                if (k.at - j >= 0)   { b += (double) out[(size_t)(k.at - j)] * out[(size_t)(k.at - j)]; ++nb; }
                if (k.at + j < numS) { a += (double) out[(size_t)(k.at + j)] * out[(size_t)(k.at + j)]; ++na; }
            }
            if (na && nb)
            {
                const double ra = std::sqrt (a / na), rb = std::sqrt (b / nb);
                if (rb > 1e-6 && (ra / rb > 1.4 || rb / ra > 1.4)) ++ampSteps;
            }
        }
        std::sort (clickJumps.begin(), clickJumps.end());
        auto med = [] (const std::vector<double>& v)
        { return v.empty() ? 0.0 : v[v.size() / 2]; };
        auto p90 = [] (const std::vector<double>& v)
        { return v.empty() ? 0.0 : v[(size_t) (v.size() * 0.9)]; };

        std::printf ("\n=== f0 movement at the hop nearest each click ===\n");
        std::printf ("  all voiced hops : median %.1f cents, p90 %.1f\n",
                     med (allJumps), p90 (allJumps));
        std::printf ("  hops at clicks  : median %.1f cents, p90 %.1f\n",
                     med (clickJumps), p90 (clickJumps));
        std::printf ("  clicks that are AMPLITUDE steps (RMS ratio >1.4): %d / %zu (%.1f%%)\n",
                     ampSteps, clicks.size(), 100.0 * ampSteps / juce::jmax (1, (int) clicks.size()));
    }

    // ---- PER-GRAIN CONFIRMATION -------------------------------------------
    {
        int nearChange = 0, nearAny = 0;
        for (const auto& k : clicks)
        {
            const int64_t q = (int64_t) k.at - latency;
            for (size_t i = 1; i < grains.size(); ++i)
            {
                const int64_t d = (int64_t) grains[i].pos - q;
                if (d < -64 || d > 8) continue;          // the grain just placed
                ++nearAny;
                if (grains[i].half != grains[i - 1].half
                    || std::abs (grains[i].gain - grains[i - 1].gain) > 1.0e-4f)
                    ++nearChange;
                break;
            }
        }
        // How often does grain geometry change at all?
        int changed = 0;
        for (size_t i = 1; i < grains.size(); ++i)
            if (grains[i].half != grains[i - 1].half
                || std::abs (grains[i].gain - grains[i - 1].gain) > 1.0e-4f) ++changed;

        std::printf ("\n=== per-grain confirmation ===\n");
        std::printf ("  grains placed                      : %zu\n", grains.size());
        std::printf ("  grains whose half/gain CHANGED     : %d (%.1f%% of grains)\n",
                     changed, 100.0 * changed / juce::jmax (1, (int) grains.size()));
        std::printf ("  clicks with a grain just before    : %d / %zu\n", nearAny, clicks.size());
        std::printf ("  ...where that grain CHANGED shape  : %d (%.1f%% of those)\n",
                     nearChange, 100.0 * nearChange / juce::jmax (1, nearAny));
    }

    // ---- THE WINDOW-SUM DISTRIBUTION, for choosing the floor ---------------
    {
        uint64_t total = 0;
        for (auto c : winHist) total += c;
        std::printf ("\n=== accumulated window sum, over every emitted voiced sample ===\n");
        std::printf ("  samples with a window   : %llu, minimum seen %.4f\n",
                     (unsigned long long) total, winMin);
        uint64_t run = 0;
        for (size_t b = 0; b < winHist.size(); ++b)
        {
            run += winHist[b];
            const double lo = b * 0.05;
            if (lo > 2.0 && winHist[b] == 0) break;
            if (winHist[b] == 0) continue;
            std::printf ("   w %.2f-%.2f : %8u  (cumulative %.3f%%)\n",
                         lo, lo + 0.05, winHist[b], 100.0 * run / juce::jmax ((uint64_t) 1, total));
        }
    }

    std::printf ("\n=== how many clicks coincide with each condition ===\n");
    std::printf ("  voiced flag changed (within %d smp) : %d / %zu\n", kNear, nVoiceChange, clicks.size());
    std::printf ("  note-change reset                  : %d\n", nNote);
    std::printf ("  gap resume                         : %d\n", nGap);
    std::printf ("  target step >20c                   : %d\n", nTargetStep);
    std::printf ("  block boundary                     : %d\n", nBlock);
    std::printf ("  within %d samples of a hop          : %d\n", kNear, nHopExact);
    std::printf ("  explained by none of the above     : %d\n", nBare);

    // ---- suspect 2: do the variable-block differences land on the clicks? --
    std::vector<uint64_t> e2;
    int lat2 = 0;
    std::mt19937 rng (7);
    std::uniform_int_distribution<int> dist (16, 2048);
    auto var = runProcessor (src, fs, 2048, [&] { return dist (rng); }, e2, lat2);

    size_t differing = 0, diffAtClick = 0;
    std::vector<char> isClick ((size_t) numS, 0);
    for (const auto& k : clicks)
        for (int j = -32; j <= 32; ++j)
            if (k.at + j >= 0 && k.at + j < numS) isClick[(size_t) (k.at + j)] = 1;
    for (int i = 0; i < numS; ++i)
        if (std::abs ((double) out[(size_t) i] - var[(size_t) i]) > 1.0e-6)
        {
            ++differing;
            if (isClick[(size_t) i]) ++diffAtClick;
        }
    std::printf ("\n=== suspect 2: variable-block residual vs the clicks ===\n");
    std::printf ("  samples differing under variable blocks : %zu (%.2f%%)\n",
                 differing, 100.0 * differing / numS);
    std::printf ("  of those, within 32 samples of a click  : %zu (%.1f%%)\n",
                 diffAtClick, differing ? 100.0 * diffAtClick / differing : 0.0);
    return 0;
}
