// pitch_constshift_probe — the ALWAYS-ON path on REAL VOICE (4 Sep 2026).
//
// The field measurement (antares_new/echojay_new) found the worst cycle-
// similarity breaks at 0.0-4.5 CENTS of applied shift — successive periods
// inverting with no correction being asked for. That contradicts the
// shift-proportional model, and the exp1 steady sweep that scored 0.9998 at
// these shifts ran on a SYNTHETIC tone. This probe supplies the missing
// ingredient: real glottal material through the shifter at a CONSTANT small
// shift with retune disabled entirely — the exact decisive experiment: if
// the engine breaks where an ideal resampler at the identical constant
// ratio doesn't, the mechanism is in the always-on path and has nothing to
// do with correction.
//
// The chain here is the INSTALLED one, mirrored from pitch_ab_test's
// renderEchoJay (detector -> F0JumpGate -> hop-sliced PsolaEngine, formant
// preserve) with ONE substitution: PitchCorrect is deleted and the shift is
// a constant. Three variants per shift bisect the always-on path in the
// ruled order without further instrumentation:
//     full        the path as shipped (formant preserve, splice band)
//     formant-off LPC formant path disabled
//     grain       splice band force-disabled (grain path)
// Every rough window is attributed against the ACTUAL v/uv seams fed to the
// engine (recorded from the drained hops), because seam windows are the
// known floor story and must not masquerade as the new mechanism.
//
// Build: g++ -std=c++17 -O2 -ISource tools/pitch_constshift_probe/main.cpp -o probe
// Run:   ./probe <voice.wav>

#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"   // F0JumpGate lives here; the corrector itself is unused
#include "EedPsolaEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace echojay;

// ---- WAV reader (PCM16/24, float32; channels averaged) — pitch_ab_test's --
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

// Best-lag NCC of two length-T windows (lag tolerance scales with T for real
// voice, where the period estimate itself carries a sample or two of error).
static double ncc (const std::vector<float>& x, long a, long b, int T)
{
    const int maxLag = std::max (2, T / 16);
    if (a < 0 || b - maxLag < 0
        || b + maxLag + T >= (long) x.size() || a + T >= (long) x.size())
        return 2.0;                              // out of range: skip marker
    double best = -2.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < T; ++i)
        {
            const double xa = x[(size_t) (a + i)];
            const double xb = x[(size_t) (b + i + lag)];
            sab += xa * xb; saa += xa * xa; sbb += xb * xb;
        }
        if (saa > 1e-12 && sbb > 1e-12)
            best = std::max (best, sab / std::sqrt (saa * sbb));
    }
    return best;
}

// ---- source f0 track (pitch_ab_test's trackFile, plus per-hop voicing) ----
struct Track { std::vector<float> f0; std::vector<float> conf; int hop = 0; };

static Track trackFile (const std::vector<float>& x, double fs)
{
    PitchEngine e;
    e.prepare (fs, 8192);
    e.setVoiceType (PitchEngine::kLowMale);
    e.setTracking (PitchEngine::kNormal);
    Track t;
    t.hop = e.inputHopLength (PitchEngine::kLowMale);
    for (size_t p = 0; p + (size_t) t.hop <= x.size(); p += (size_t) t.hop)
    {
        e.process (x.data() + p, nullptr, t.hop);
        const PitchReading r = e.getReading();
        t.f0.push_back (r.voiced ? r.f0Hz : 0.0f);
        t.conf.push_back (r.confidence);
    }
    std::vector<float> f = t.f0;
    for (size_t i = 2; i + 2 < f.size(); ++i)
    {
        float v[5]; int n = 0;
        for (int k = -2; k <= 2; ++k) if (f[i + (size_t) k] > 0.0f) v[n++] = f[i + (size_t) k];
        if (n >= 3 && f[i] > 0.0f) { std::sort (v, v + n); t.f0[i] = v[n / 2]; }
    }
    return t;
}

// ---- the always-on path at CONSTANT shift ---------------------------------
// renderEchoJay minus PitchCorrect: target = gatedF0 * r held through gaps,
// shift = the constant. Voicing seams as fed to the engine are recorded.
static std::vector<float> renderConstShift (const std::vector<float>& in, double fs,
                                            float shiftCents, int fmode,
                                            bool disableSplice, bool driftBleed,
                                            std::vector<uint64_t>& seams,
                                            std::vector<uint64_t>& splices,
                                            std::vector<PsolaEngine::DebugSpl>& trace,
                                            int& latencyOut,
                                            std::vector<uint64_t>& f0zero)
{
    constexpr int vt = PitchEngine::kLowMale;
    const float r = std::pow (2.0f, shiftCents / 1200.0f);

    PitchEngine det;
    det.prepare (fs, 256);
    det.setVoiceType (vt);
    det.setTracking (PitchEngine::kNormal);
    const float hopMs = det.hopMs();

    F0JumpGate f0Gate;

    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = std::min (worst, PitchEngine::voiceRange (t).fMinHz);

    PsolaEngine sh;
    sh.prepare (fs, 256, PitchEngine::voiceRange (vt).fMinHz, worst);
    sh.setFormantMode (fmode);
    sh.debugRecordPhaseEvents (true);
    sh.debugDisableSplice (disableSplice);
    sh.setDriftBleed (driftBleed);
    sh.setPitchLagSamples (det.pitchLagFor (vt));
    const int latency = sh.latencySamples();

    std::vector<float> out (in.size(), 0.0f);
    PitchEngine::HopEvent ev[64];
    float target = 0.0f, sliceF0 = 0.0f;
    float shift  = PsolaEngine::kNoShift;
    bool  sliceVoiced = false, lastVoiced = false;
    for (size_t p = 0; p + 256 <= in.size(); p += 256)
    {
        det.process (in.data() + p, nullptr, 256);
        const int nHops = det.drainHops (ev, 64);
        const uint64_t blockStart = det.inputPosition() - 256;

        int cursor = 0;
        for (int h = 0; h <= nHops; ++h)
        {
            int sliceEnd = 256;
            if (h < nHops)
            {
                const int64_t rel = (int64_t) ev[h].inputPos - (int64_t) blockStart;
                sliceEnd = (int) std::clamp (rel, (int64_t) cursor, (int64_t) 256);
            }
            if (sliceEnd > cursor)
            {
                sh.process (in.data() + p + (size_t) cursor,
                            out.data() + p + (size_t) cursor,
                            sliceEnd - cursor, sliceF0, sliceVoiced, target, shift);
                cursor = sliceEnd;
            }
            if (h < nHops)
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
                    rOldT = sh.inputPeriodicity (ev[h].inputPos, tOld);
                    rNewT = sh.inputPeriodicity (ev[h].inputPos, tNew);
                }
                const float gatedF0 = f0Gate.filter (ev[h].f0Hz, ev[h].voiced, hopMs,
                                                     rOldT, rNewT);
                sliceF0 = gatedF0; sliceVoiced = ev[h].voiced;
                // The ENGINE's voicing view is f0>0 in its ring (voicedAt),
                // not the detector's voiced flag — a voiced hop whose gated
                // f0 is 0 (seed vetting, or the detector itself) is a seam
                // the engine executes that a flag-based record cannot see.
                // ...and it lands LAG-COMPENSATED: process() writes the f0
                // ring pitchLag_ samples behind the hop position, so the
                // seam the engine executes sits at inputPos - lag.
                const bool ringVoiced = gatedF0 > 0.0f;
                if (ringVoiced != lastVoiced)
                {
                    const int lag = std::min (sh.getPitchLagSamples(),
                                              latency - 1);
                    seams.push_back (ev[h].inputPos > (uint64_t) lag
                                     ? ev[h].inputPos - (uint64_t) lag : 0);
                    lastVoiced = ringVoiced;
                }
                if (ev[h].voiced && gatedF0 <= 0.0f)
                    f0zero.push_back (ev[h].inputPos);
                if (gatedF0 > 0.0f && ev[h].voiced)
                { target = gatedF0 * r; shift = shiftCents; }   // held through gaps
            }
        }
        if (cursor < 256)
            sh.process (in.data() + p + (size_t) cursor,
                        out.data() + p + (size_t) cursor,
                        256 - cursor, sliceF0, sliceVoiced, target, shift);
    }

    splices = sh.debugSplices();     // pre-latency output index = source time
    trace = sh.debugSpliceTrace();   // trace[k] = k-th emitted sample
    latencyOut = latency;

    std::vector<float> aligned (in.size(), 0.0f);
    for (size_t i = (size_t) latency; i < out.size(); ++i)
        aligned[i - (size_t) latency] = out[i];
    return aligned;
}

// CAUSALITY CHECK, not a fix: the same always-on path, but the GATED f0
// track is computed first and short zero-gaps (< bridgeMs, voiced on both
// sides, flanks within 300c of each other) are bridged by interpolation
// before the engine sees it. If the mid-voiced breaks are the flicker-seam
// mechanism, they collapse to the ideal's level here; anything left is
// something else.
struct Hop { uint64_t pos; float f0; bool voiced; };

// Detector + gate offline: the gated hop list the engine would be fed,
// with a dry-fed engine copy holding the ring the gate's audio-ask reads.
static std::vector<Hop> gatedHops (const std::vector<float>& in, double fs)
{
    constexpr int vt = PitchEngine::kLowMale;
    std::vector<Hop> hops;
    {
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
                const float g = f0Gate.filter (ev[h].f0Hz, ev[h].voiced, hopMs,
                                               rOldT, rNewT);
                hops.push_back ({ ev[h].inputPos, g, ev[h].voiced });
            }
        }
    }
    return hops;
}

static std::vector<float> renderConstShiftBridged (const std::vector<float>& in,
                                                   double fs, float shiftCents,
                                                   float bridgeMs)
{
    constexpr int vt = PitchEngine::kLowMale;
    const float r = std::pow (2.0f, shiftCents / 1200.0f);
    std::vector<Hop> hops = gatedHops (in, fs);

    // Bridge: zero-f0 runs shorter than bridgeMs with close voiced flanks.
    {
        const double maxGapSmp = bridgeMs * 0.001 * fs;
        size_t i = 0;
        while (i < hops.size())
        {
            if (hops[i].f0 > 0.0f) { ++i; continue; }
            size_t j = i;
            while (j < hops.size() && hops[j].f0 <= 0.0f) ++j;
            if (i > 0 && j < hops.size())
            {
                const float fa = hops[i - 1].f0, fb = hops[j].f0;
                const double gap = (double) (hops[j].pos - hops[i - 1].pos);
                if (fa > 0.0f && fb > 0.0f && gap < maxGapSmp
                    && std::fabs (1200.0 * std::log2 ((double) fb / fa)) < 1200.0)
                    for (size_t k = i; k < j; ++k)
                    {
                        const double t = (double) (hops[k].pos - hops[i - 1].pos) / gap;
                        hops[k].f0 = (float) (fa + t * (fb - fa));
                        hops[k].voiced = true;
                    }
            }
            i = j;
        }
    }

    // Pass 2: the engine, fed the bridged track with the same hop slicing.
    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = std::min (worst, PitchEngine::voiceRange (t).fMinHz);
    PsolaEngine sh;
    sh.prepare (fs, 256, PitchEngine::voiceRange (vt).fMinHz, worst);
    sh.setFormantMode (PsolaEngine::kFormantPreserve);
    {
        PitchEngine lagRef;
        lagRef.prepare (fs, 256);
        lagRef.setVoiceType (vt);
        sh.setPitchLagSamples (lagRef.pitchLagFor (vt));
    }
    const int latency = sh.latencySamples();

    std::vector<float> out (in.size(), 0.0f);
    float target = 0.0f, sliceF0 = 0.0f;
    float shift  = PsolaEngine::kNoShift;
    bool  sliceVoiced = false;
    size_t hi = 0;
    for (size_t p = 0; p + 256 <= in.size(); p += 256)
    {
        size_t cursor = p;
        while (hi < hops.size() && hops[hi].pos < p + 256)
        {
            const size_t end = std::clamp ((size_t) hops[hi].pos, cursor, p + 256);
            if (end > cursor)
            {
                sh.process (in.data() + cursor, out.data() + cursor,
                            (int) (end - cursor), sliceF0, sliceVoiced, target, shift);
                cursor = end;
            }
            sliceF0 = hops[hi].f0; sliceVoiced = hops[hi].voiced;
            if (hops[hi].f0 > 0.0f && hops[hi].voiced)
            { target = hops[hi].f0 * r; shift = shiftCents; }
            ++hi;
        }
        if (cursor < p + 256)
            sh.process (in.data() + cursor, out.data() + cursor,
                        (int) (p + 256 - cursor), sliceF0, sliceVoiced, target, shift);
    }

    std::vector<float> aligned (in.size(), 0.0f);
    for (size_t i2 = (size_t) latency; i2 < out.size(); ++i2)
        aligned[i2 - (size_t) latency] = out[i2];
    return aligned;
}

// THE RIGHT FLOOR for a correct-vowels-dry-consonants shifter on real
// voice (the plain resample below has no seams and honours no dry
// contract): an IDEAL seamed shifter on the SAME gated track the engine
// sees — per-voiced-span cubic resample at constant r, started
// phase-aligned at each span's own entry (exactly the engine's anchor
// rule), bit-exact dry in the gaps, 1.5 ms linear fades on the voiced
// side. Whatever this scores is what the seam CONTRACT costs at this
// shift on this material; only the excess above it belongs to the engine.
static std::vector<float> renderSeamIdeal (const std::vector<float>& in, double fs,
                                           float shiftCents,
                                           const std::vector<Hop>& hops, int lag)
{
    const double r = std::pow (2.0, shiftCents / 1200.0);
    const long n = (long) in.size();
    std::vector<float> out (in.begin(), in.end());   // gaps stay bit-exact dry
    auto cubic = [&] (double x) -> float
    {
        const long i1 = (long) x;
        if (i1 < 1 || i1 + 2 >= n)
            return i1 >= 0 && i1 < n ? in[(size_t) i1] : 0.0f;
        const double fr = x - (double) i1;
        const double xm = in[(size_t) (i1 - 1)], x0 = in[(size_t) i1],
                     x1 = in[(size_t) (i1 + 1)], x2 = in[(size_t) (i1 + 2)];
        return (float) (x0 + 0.5 * fr * (x1 - xm
                      + fr * (2.0 * xm - 5.0 * x0 + 4.0 * x1 - x2
                      + fr * (3.0 * (x0 - x1) + x2 - xm))));
    };
    const int fadeN = (int) (0.0015 * fs);
    // Spans of ring-voicing (f0>0), positions lag-compensated like the ring.
    size_t k = 0;
    while (k < hops.size())
    {
        while (k < hops.size() && hops[k].f0 <= 0.0f) ++k;
        if (k >= hops.size()) break;
        const size_t k0 = k;
        while (k < hops.size() && hops[k].f0 > 0.0f) ++k;
        const long s = std::max (0L, (long) hops[k0].pos - (long) lag);
        const long e = k < hops.size()
            ? std::max (0L, (long) hops[k].pos - (long) lag) : n;
        for (long j = s; j < e && j < n; ++j)
        {
            const float wet = cubic ((double) s + (double) (j - s) * r);
            const long edge = std::min (j - s, e - 1 - j);
            const float g = edge >= fadeN ? 1.0f
                          : (float) (edge + 1) / (float) (fadeN + 1);
            out[(size_t) j] = in[(size_t) j] + g * (wet - in[(size_t) j]);
        }
    }
    return out;
}

// Ideal control: whole-file cubic resample at the identical constant ratio.
// Output index j reads source j*r — it cannot glitch; whatever it scores at
// a window is the floor for a shifter with no seams and no epochs at all.
static std::vector<float> renderIdeal (const std::vector<float>& in, float shiftCents)
{
    const double r = std::pow (2.0, shiftCents / 1200.0);
    const long n = (long) in.size();
    std::vector<float> out ((size_t) ((double) n / r), 0.0f);
    for (long j = 0; j < (long) out.size(); ++j)
    {
        const double x = (double) j * r;
        const long i1 = (long) x;
        if (i1 < 1 || i1 + 2 >= n) { out[(size_t) j] = i1 >= 0 && i1 < n ? in[(size_t) i1] : 0.0f; continue; }
        const double fr = x - (double) i1;
        const double xm = in[(size_t) (i1 - 1)], x0 = in[(size_t) i1],
                     x1 = in[(size_t) (i1 + 1)], x2 = in[(size_t) (i1 + 2)];
        out[(size_t) j] = (float) (x0 + 0.5 * fr * (x1 - xm
                        + fr * (2.0 * xm - 5.0 * x0 + 4.0 * x1 - x2
                        + fr * (3.0 * (x0 - x1) + x2 - xm))));
    }
    return out;
}

struct WorstWin { double t, eng, idl, src; bool nearSeam; long dSplice;
                  double drift, srcDisp; };

int main (int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1]
        : "/Users/SeanD/Music/Logic/test/Audio Files/source4.wav";
    std::vector<float> in; double fs = 0;
    if (! readWavMono (path, in, fs)) { std::printf ("cannot read %s\n", path); return 1; }
    std::printf ("material: %s  (%.2fs @ %.0f Hz)\n", path, in.size() / fs, fs);

    const Track tr = trackFile (in, fs);

    struct Var { const char* name; int fmode; bool noSplice; int kind; };
    const Var vars[] = {                       // kind: 0 engine, 1 bridged, 2 seam-ideal, 3 engine+bleed
        { "full       ", PsolaEngine::kFormantPreserve, false, 0 },
        { "bleed      ", PsolaEngine::kFormantPreserve, false, 3 },
        { "seam-ideal ", PsolaEngine::kFormantPreserve, false, 2 },
        { "grain      ", PsolaEngine::kFormantPreserve, true,  0 },
    };
    const std::vector<Hop> hops = gatedHops (in, fs);
    int ringLag = 0;
    {
        PitchEngine lr; lr.prepare (fs, 256); lr.setVoiceType (PitchEngine::kLowMale);
        PsolaEngine se;
        float w2 = PitchEngine::voiceRange (0).fMinHz;
        for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
            w2 = std::min (w2, PitchEngine::voiceRange (t).fMinHz);
        se.prepare (fs, 256, PitchEngine::voiceRange (PitchEngine::kLowMale).fMinHz, w2);
        ringLag = std::min (lr.pitchLagFor (PitchEngine::kLowMale),
                            se.latencySamples() - 1);
    }

    for (float sc : { 0.0f, 5.0f, 10.0f })
    {
        const double r = std::pow (2.0, sc / 1200.0);
        const auto ideal = renderIdeal (in, sc);
        std::printf ("\n== constant shift %+.0fc (r=%.5f) ==\n", sc, r);
        std::printf ("variant, voiced_wins, med_src, med_eng, med_idl, "
                     "rough_eng, rough_idl, brk_eng, brk_idl, neg_eng, "
                     "eng_worse_spans, ofwhich_midvoiced\n");
        for (const auto& v : vars)
        {
            std::vector<uint64_t> seams, splices, f0zero;
            std::vector<PsolaEngine::DebugSpl> trace;
            int lat = 0;
            const auto eng = v.kind == 1
                ? renderConstShiftBridged (in, fs, sc, 120.0f)
                : v.kind == 2
                ? renderSeamIdeal (in, fs, sc, hops, ringLag)
                : renderConstShift (in, fs, sc, v.fmode, v.noSplice,
                                    v.kind == 3,
                                    seams, splices, trace, lat, f0zero);

            std::vector<double> simsS, simsE, simsI;
            std::vector<WorstWin> wins;
            int voiced = 0, roughE = 0, roughI = 0, brkE = 0, brkI = 0,
                negE = 0, worse = 0, worseMid = 0;
            const long lead = (long) (0.3 * fs);
            for (size_t hIdx = 0; hIdx < tr.f0.size(); ++hIdx)
            {
                const float f0 = tr.f0[hIdx];
                if (f0 <= 0.0f) continue;
                const long a = (long) hIdx * tr.hop;
                if (a < lead || a > (long) in.size() - lead) continue;
                const int T    = (int) std::lround (fs / f0);
                const int Tout = (int) std::lround (fs / (f0 * r));
                const double sS = ncc (in, a, a + T, T);
                const double sE = ncc (eng, a, a + Tout, Tout);
                const double sI = ncc (ideal, (long) ((double) a / r),
                                       (long) ((double) a / r) + Tout, Tout);
                if (sS > 1.5 || sE > 1.5 || sI > 1.5) continue;   // range skip
                if (sS < 0.5) continue;      // source itself aperiodic here
                ++voiced;
                simsS.push_back (sS); simsE.push_back (sE); simsI.push_back (sI);
                if (sE < 0.90) ++roughE;
                if (sI < 0.90) ++roughI;
                if (sE < 0.50) ++brkE;
                if (sI < 0.50) ++brkI;
                if (sE < 0.0)  ++negE;
                // near an engine-fed voicing seam? (the known floor story)
                bool nearSeam = false;
                for (uint64_t s : seams)
                    if (std::labs ((long) s - a) < 3 * T) { nearSeam = true; break; }
                long dSp = 1 << 30;
                for (uint64_t s : splices)
                    dSp = std::min (dSp, std::labs ((long) s - a));
                if (sE < sI - 0.15 && sE < 0.90)
                { ++worse; if (! nearSeam) ++worseMid; }
                if (sE < 0.80)
                {
                    // The DISPLACED-ANCHOR check: the splice-resampler reads
                    // at p+drift, drift=(r-1)*(p-entry) since the last voiced
                    // entry — its output at a carries source content from
                    // a+D. If the source's OWN similarity at a+D is broken
                    // too, the engine faithfully reproduced a source
                    // transient a few ms displaced, and a time-anchored
                    // metric charged the displacement as roughness.
                    long entry = 0;
                    for (size_t si = 0; si < seams.size(); si += 2)
                        if ((long) seams[si] <= a) entry = (long) seams[si];
                    const double D = (r - 1.0) * (double) (a - entry);
                    const long aD = a + (long) std::lround (D);
                    const double sSd = ncc (in, aD, aD + T, T);
                    wins.push_back ({ a / fs, sE, sI, sS, nearSeam, dSp, D,
                                      sSd > 1.5 ? -9.0 : sSd });
                }
            }
            // CS_DUMP=<dir>: raw float32 of engine/ideal/source for offline
            // waveform inspection of the break windows (full variant only).
            if (const char* dir = getenv ("CS_DUMP"); dir && ! std::strcmp (v.name, "full       "))
            {
                auto dump = [&] (const char* nm, const std::vector<float>& x)
                {
                    char pth[512];
                    std::snprintf (pth, sizeof (pth), "%s/%s_%+.0fc.f32", dir, nm, sc);
                    if (FILE* f = std::fopen (pth, "wb"))
                    { std::fwrite (x.data(), 4, x.size(), f); std::fclose (f); }
                };
                dump ("eng", eng); dump ("idl", ideal); dump ("src", in);
            }
            auto med = [] (std::vector<double>& s) {
                if (s.empty()) return 0.0;
                std::sort (s.begin(), s.end()); return s[s.size() / 2]; };
            std::printf ("%s, %d, %.4f, %.4f, %.4f, %d, %d, %d, %d, %d, %d, %d  "
                         "(splices %d, voiced-but-f0zero hops %d)\n",
                         v.name, voiced, med (simsS), med (simsE), med (simsI),
                         roughE, roughI, brkE, brkI, negE, worse, worseMid,
                         (int) splices.size(), (int) f0zero.size());
            std::sort (wins.begin(), wins.end(),
                       [] (const WorstWin& x, const WorstWin& y) { return x.eng < y.eng; });
            int dispExplained = 0, dispTotal = 0;
            for (const auto& w : wins)
                if (w.eng < 0.50 && w.idl > 0.50)
                { ++dispTotal; if (w.srcDisp < 0.60) ++dispExplained; }
            std::printf ("    displaced-anchor: %d/%d engine breaks have the "
                         "SOURCE broken at a+drift\n", dispExplained, dispTotal);
            for (size_t k = 0; k < wins.size() && k < 8; ++k)
            {
                std::printf ("    worst %5.2fs  eng %+.3f  idl %+.3f  src %.3f  "
                             "srcAtDrift %.3f (D=%+.0f)  %s\n",
                             wins[k].t, wins[k].eng, wins[k].idl, wins[k].src,
                             wins[k].srcDisp, wins[k].drift,
                             wins[k].nearSeam ? "AT-SEAM" : "MID-VOICED");
                // The author record across the window: aligned index a maps
                // to trace index a+latency.
                const long a0 = (long) (wins[k].t * fs) + lat;
                float gMin = 9, gMax = -9, mMin = 9, mMax = -9, rMin = 9, rMax = -9,
                      dMin = 1e9f, dMax = -1e9f;
                int tMin = 1 << 30, tMax = -1;
                for (long q = a0 - 400; q < a0 + 800 && q < (long) trace.size(); ++q)
                {
                    if (q < 0) continue;
                    const auto& tt = trace[(size_t) q];
                    gMin = std::min (gMin, tt.g);       gMax = std::max (gMax, tt.g);
                    mMin = std::min (mMin, tt.mix);     mMax = std::max (mMax, tt.mix);
                    rMin = std::min (rMin, tt.r);       rMax = std::max (rMax, tt.r);
                    dMin = std::min (dMin, tt.drift);   dMax = std::max (dMax, tt.drift);
                    tMin = std::min (tMin, tt.T);       tMax = std::max (tMax, tt.T);
                }
                std::printf ("           g %.2f..%.2f  mix %.2f..%.2f  "
                             "r %.5f..%.5f  drift %.1f..%.1f  T %d..%d\n",
                             gMin, gMax, mMin, mMax, rMin, rMax, dMin, dMax,
                             tMin, tMax);
            }
        }
    }
    return 0;
}
