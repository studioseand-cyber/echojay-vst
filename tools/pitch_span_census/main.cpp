// pitch_span_census — HOW FRAGMENTED is the detector's voicing on a real
// take? (5 Sep 2026 ruling: measure span fragmentation before touching it.)
//
// Every boundary between voiced spans is a drift-discharge site in the
// shifter (see tools/pitch_constshift_probe), so the span structure IS the
// defect surface. This tool reports, on the engine's own terms:
//   - voiced spans as the RING sees them (gated f0 > 0, lag-compensated -
//     the exact signal the shifter seams on), with a length histogram
//   - every inter-span gap, classified by ASKING THE AUDIO: is the source
//     still periodic at the flanking period through the gap? A periodic
//     gap with pitch-continuous flanks is a TRACKING DROPOUT inside a note
//     (a false split: the waveform continued, the tracker blinked); an
//     aperiodic gap is a real consonant/breath.
//   - NOTES = spans merged across false-split gaps; spans per note is the
//     fragmentation number the ruling asked for.
//
// Build: g++ -std=c++17 -O2 -ISource tools/pitch_span_census/main.cpp -o census
// Run:   ./census <voice.wav>

#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"   // F0JumpGate
#include "EedPsolaEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace echojay;

static bool readWavMono (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) return false;
    auto rd32 = [&] { uint8_t b[4]; if (std::fread (b, 1, 4, f) != 4) return 0u;
                      return (uint32_t) (b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24)); };
    auto rd16 = [&] { uint8_t b[2]; if (std::fread (b, 1, 2, f) != 2) return 0u;
                      return (uint32_t) (b[0] | (b[1] << 8)); };
    char tag[5] = {};
    if (std::fread (tag, 1, 4, f) != 4 || std::strncmp (tag, "RIFF", 4) != 0) { std::fclose (f); return false; }
    rd32();
    if (std::fread (tag, 1, 4, f) != 4 || std::strncmp (tag, "WAVE", 4) != 0) { std::fclose (f); return false; }
    uint16_t fmt = 0, chans = 0, bits = 0; uint32_t rate = 0;
    while (std::fread (tag, 1, 4, f) == 4)
    {
        const uint32_t sz = rd32();
        if (std::strncmp (tag, "fmt ", 4) == 0)
        {
            fmt = (uint16_t) rd16(); chans = (uint16_t) rd16();
            rate = rd32(); rd32(); rd16(); bits = (uint16_t) rd16();
            if (sz > 16) std::fseek (f, (long) sz - 16, SEEK_CUR);
        }
        else if (std::strncmp (tag, "data", 4) == 0)
        {
            if (chans == 0) { std::fclose (f); return false; }
            const uint32_t bytesPer = bits / 8u;
            const uint32_t frames = sz / (bytesPer * chans);
            out.resize (frames);
            std::vector<uint8_t> fr (bytesPer * chans);
            for (uint32_t i = 0; i < frames; ++i)
            {
                if (std::fread (fr.data(), 1, fr.size(), f) != fr.size()) break;
                double acc = 0.0;
                for (uint16_t c = 0; c < chans; ++c)
                {
                    const uint8_t* b = fr.data() + c * bytesPer;
                    if (fmt == 3 && bits == 32) { float v; std::memcpy (&v, b, 4); acc += v; }
                    else if (fmt == 1 && bits == 16) { acc += (double) (int16_t) (b[0] | (b[1] << 8)) / 32768.0; }
                    else if (fmt == 1 && bits == 24)
                    { acc += (double) ((int32_t) ((b[0] << 8) | (b[1] << 16) | ((uint32_t) b[2] << 24)) >> 8) / 8388608.0; }
                    else { std::fclose (f); return false; }
                }
                out[i] = (float) (acc / chans);
            }
            fs = (double) rate; std::fclose (f); return true;
        }
        else std::fseek (f, (long) (sz + (sz & 1)), SEEK_CUR);
    }
    std::fclose (f); return false;
}

// Best-lag NCC of x[a..a+T) vs x[a+T..a+2T) — periodicity at period T.
static double periodicity (const std::vector<float>& x, long a, int T)
{
    const int maxLag = std::max (2, T / 16);
    if (a - 0 < 0 || a + 2 * T + maxLag >= (long) x.size()) return -2.0;
    double best = -2.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < T; ++i)
        {
            const double xa = x[(size_t) (a + i)];
            const double xb = x[(size_t) (a + T + i + lag)];
            sab += xa * xb; saa += xa * xa; sbb += xb * xb;
        }
        if (saa > 1e-12 && sbb > 1e-12)
            best = std::max (best, sab / std::sqrt (saa * sbb));
    }
    return best;
}

struct Hop { uint64_t pos; float f0; bool voiced; };

// Detector + F0JumpGate, exactly as the processor feeds the shifter
// (mirrors pitch_constshift_probe's gatedHops).
static std::vector<Hop> gatedHops (const std::vector<float>& in, double fs)
{
    constexpr int vt = PitchEngine::kLowMale;
    std::vector<Hop> hops;
    PitchEngine det;
    det.prepare (fs, 256);
    det.setVoiceType (vt);
    det.setTracking (PitchEngine::kNormal);
    const float hopMs = det.hopMs();
    F0JumpGate f0Gate;
    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = std::min (worst, PitchEngine::voiceRange (t).fMinHz);
    PsolaEngine ring;
    ring.prepare (fs, 256, PitchEngine::voiceRange (vt).fMinHz, worst);
    ring.setPitchLagSamples (det.pitchLagFor (vt));
    std::vector<float> scratch (256);
    PitchEngine::HopEvent ev[64];
    for (size_t p = 0; p + 256 <= in.size(); p += 256)
    {
        det.process (in.data() + p, nullptr, 256);
        ring.process (in.data() + p, scratch.data(), 256, 0.0f, false, 0.0f);
        const int nHops = det.drainHops (ev, 64);
        for (int h = 0; h < nHops; ++h)
        {
            float rOldT = -1.0f, rNewT = -1.0f;
            const bool seeding = ev[h].voiced && ev[h].f0Hz > 0.0f
                              && f0Gate.lastGood() <= 0.0f;
            if (f0Gate.isBigJump (ev[h].f0Hz, ev[h].voiced) || seeding)
            {
                const double refHz = seeding ? 2.0 * (double) ev[h].f0Hz
                                             : (double) f0Gate.lastGood();
                const int tOld = (int) std::lround (fs / refHz);
                const int tNew = (int) std::lround (fs / (double) ev[h].f0Hz);
                rOldT = ring.inputPeriodicity (ev[h].inputPos, tOld);
                rNewT = ring.inputPeriodicity (ev[h].inputPos, tNew);
            }
            const float g = f0Gate.filter (ev[h].f0Hz, ev[h].voiced, hopMs, rOldT, rNewT);
            hops.push_back ({ ev[h].inputPos, g, ev[h].voiced });
        }
    }
    return hops;
}

int main (int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1]
        : "/Users/SeanD/Music/Logic/test/Bounces/sourceNEW.wav";
    std::vector<float> in; double fs = 0;
    if (! readWavMono (path, in, fs)) { std::printf ("cannot read %s\n", path); return 1; }
    std::printf ("material: %s  (%.2fs @ %.0f Hz)\n", path, in.size() / fs, fs);

    const auto hops = gatedHops (in, fs);
    int lag = 0;
    {
        PitchEngine lr; lr.prepare (fs, 256); lr.setVoiceType (PitchEngine::kLowMale);
        PsolaEngine se;
        float w2 = PitchEngine::voiceRange (0).fMinHz;
        for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
            w2 = std::min (w2, PitchEngine::voiceRange (t).fMinHz);
        se.prepare (fs, 256, PitchEngine::voiceRange (PitchEngine::kLowMale).fMinHz, w2);
        lag = std::min (lr.pitchLagFor (PitchEngine::kLowMale), se.latencySamples() - 1);
    }

    // Spans of ring-voicing (gated f0 > 0), lag-compensated positions.
    struct Span { long s, e; float f0First, f0Last; };
    std::vector<Span> spans;
    {
        size_t k = 0;
        while (k < hops.size())
        {
            while (k < hops.size() && hops[k].f0 <= 0.0f) ++k;
            if (k >= hops.size()) break;
            const size_t k0 = k;
            while (k < hops.size() && hops[k].f0 > 0.0f) ++k;
            const long s = std::max (0L, (long) hops[k0].pos - lag);
            const long e = k < hops.size()
                ? std::max (0L, (long) hops[k].pos - lag) : (long) in.size();
            spans.push_back ({ s, e, hops[k0].f0, hops[k - 1].f0 });
        }
    }

    double voicedS = 0;
    for (const auto& sp : spans) voicedS += (sp.e - sp.s) / fs;

    // Histogram.
    const double edgesMs[] = { 0, 50, 100, 200, 400, 800, 1e9 };
    int hist[6] = {};
    int under100 = 0;
    for (const auto& sp : spans)
    {
        const double ms = 1000.0 * (sp.e - sp.s) / fs;
        for (int b = 0; b < 6; ++b)
            if (ms >= edgesMs[b] && ms < edgesMs[b + 1]) { ++hist[b]; break; }
        if (ms < 100.0) ++under100;
    }
    std::printf ("\nvoiced spans (ring view): %d over %.2fs voiced  (%.2f spans/s of voiced)\n",
                 (int) spans.size(), voicedS, spans.size() / voicedS);
    std::printf ("span-length histogram (ms): <50: %d   50-100: %d   100-200: %d   "
                 "200-400: %d   400-800: %d   >800: %d   (under 100ms: %d = %.0f%%)\n",
                 hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
                 under100, 100.0 * under100 / std::max<size_t> (1, spans.size()));

    // Gaps: classify by asking the audio. Periodicity at 3 probe points
    // across the gap at the flanking period; false split = median probe
    // >= 0.60 (the gate's own kStillPeriodic) AND flanks within 200c.
    int falseSplit = 0, realGap = 0;
    int gapHist[4] = {};    // <30, 30-60, 60-120, >=120 ms
    std::vector<bool> merge (spans.size(), false);   // merge[i]: gap i..i+1 is false
    for (size_t i = 0; i + 1 < spans.size(); ++i)
    {
        const long g0 = spans[i].e, g1 = spans[i + 1].s;
        const double gapMs = 1000.0 * (g1 - g0) / fs;
        gapHist[gapMs < 30 ? 0 : gapMs < 60 ? 1 : gapMs < 120 ? 2 : 3]++;
        const float fa = spans[i].f0Last, fb = spans[i + 1].f0First;
        const float fMid = 0.5f * (fa + fb);
        const int T = (int) std::lround (fs / std::max (60.0f, fMid));
        double probes[3];
        for (int q = 0; q < 3; ++q)
        {
            const long c = g0 + (long) ((q + 1) * (g1 - g0) / 4.0);
            probes[q] = periodicity (in, c - T, T);
        }
        std::sort (probes, probes + 3);
        const bool audioPeriodic = probes[1] >= 0.60;
        const bool pitchCont = fa > 0 && fb > 0
            && std::fabs (1200.0 * std::log2 ((double) fb / fa)) < 200.0;
        if (audioPeriodic && pitchCont) { ++falseSplit; merge[i] = true; }
        else ++realGap;
    }
    std::printf ("\ninter-span gaps: %d total  (<30ms: %d  30-60: %d  60-120: %d  >=120: %d)\n",
                 (int) spans.size() - 1, gapHist[0], gapHist[1], gapHist[2], gapHist[3]);
    std::printf ("classified: FALSE SPLITS (audio periodic through gap, flanks within 200c): %d"
                 "   real gaps: %d\n", falseSplit, realGap);

    // Notes = spans merged across false splits.
    int notes = 0, maxSpansInNote = 0, curSpans = 0, notesSplit = 0;
    double maxNoteMs = 0; long worstNoteAt = 0;
    for (size_t i = 0; i < spans.size(); ++i)
    {
        ++curSpans;
        const bool last = i + 1 >= spans.size() || ! merge[i];
        if (last)
        {
            ++notes;
            if (curSpans > 1) ++notesSplit;
            if (curSpans > maxSpansInNote)
            {
                maxSpansInNote = curSpans;
                worstNoteAt = spans[i + 1 - (size_t) curSpans].s;
                maxNoteMs = 1000.0 * (spans[i].e - spans[i + 1 - (size_t) curSpans].s) / fs;
            }
            curSpans = 0;
        }
    }
    std::printf ("\nnotes (spans merged across false splits): %d   spans/note: %.2f\n",
                 notes, (double) spans.size() / std::max (1, notes));
    std::printf ("notes split by tracking dropouts: %d of %d   worst note: %d spans "
                 "over %.0fms starting %.2fs\n",
                 notesSplit, notes, maxSpansInNote, maxNoteMs, worstNoteAt / fs);

    // Every false-split gap listed (they are the discharge sites a fix
    // would remove): time, length, mid-gap periodicity, flank cents.
    std::printf ("\nfalse-split gaps (time, len ms, mid periodicity, flank cents):\n");
    for (size_t i = 0; i + 1 < spans.size(); ++i)
        if (merge[i])
        {
            const long g0 = spans[i].e, g1 = spans[i + 1].s;
            const float fa = spans[i].f0Last, fb = spans[i + 1].f0First;
            const float fMid = 0.5f * (fa + fb);
            const int T = (int) std::lround (fs / std::max (60.0f, fMid));
            const double pr = periodicity (in, g0 + (g1 - g0) / 2 - T, T);
            std::printf ("  %6.2fs  %5.0fms  %.2f  %+5.0fc\n", g0 / fs,
                         1000.0 * (g1 - g0) / fs, pr,
                         1200.0 * std::log2 ((double) fb / fa));
        }
    return 0;
}
