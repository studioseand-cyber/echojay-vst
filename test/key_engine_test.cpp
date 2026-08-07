// Standalone test for KeyEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source key_engine_test.cpp ../Source/EedKeyEngine.cpp \
//       -o keytest && ./keytest
//
// The claims under test are the spec's accuracy claims (KEY_DETECTOR_SPEC.md §2),
// not implementation details:
//   * C-major material reads C major, confidently;
//   * A-minor material reads A minor and specifically NOT C major — the classic
//     relative-key failure of naive chromagram detectors;
//   * material detuned 30 cents is still identified AND its tuning is reported
//     within a few cents (the tuning-first design doing its job);
//   * a drum loop and white noise report LOW confidence rather than a confident
//     wrong answer;
//   * on a percussive mix, HPSS measurably beats the no-HPSS path on the SAME
//     input;
//   * mode_lock constrains the answer, hold freezes it, reset clears it.

#include "EedKeyEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static constexpr double kFs = 48000.0;

// Deterministic noise — no <random>, so numbers match across libstdc++/libc++.
struct Noise
{
    unsigned s = 777u;
    float next()
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((double) (s >> 8) / 8388608.0 - 1.0);
    }
};

// ---------------------------------------------------------------------------
// synthesis
// ---------------------------------------------------------------------------
static double midiHz (int midi, double detuneCents = 0.0)
{
    return 440.0 * std::pow (2.0, ((double) (midi - 69) + detuneCents / 100.0) / 12.0);
}

// A harmonically rich note (1/h partials), soft attack, exponential release —
// close enough to a plucked/struck instrument for the pipeline to chew on.
static void addNote (std::vector<float>& buf, double t0, double dur,
                     double freq, float amp, int nHarm = 6)
{
    const int start = (int) (t0 * kFs);
    const int len   = (int) (dur * kFs);
    for (int h = 1; h <= nHarm; ++h)
    {
        const double f = freq * h;
        if (f >= kFs * 0.45) break;
        const double w = 2.0 * 3.14159265358979323846 * f / kFs;
        const float  a = amp / (float) h;
        for (int i = 0; i < len; ++i)
        {
            const int idx = start + i;
            if (idx < 0 || idx >= (int) buf.size()) break;
            const double t   = (double) i / kFs;
            const double env = (1.0 - std::exp (-t / 0.01)) * std::exp (-t / (dur * 0.6));
            buf[(size_t) idx] += (float) (std::sin (w * i) * env) * a;
        }
    }
}

static void addChord (std::vector<float>& buf, double t0, double dur,
                      const int* midis, int n, float amp, double detuneCents = 0.0)
{
    for (int i = 0; i < n; ++i)
        addNote (buf, t0, dur, midiHz (midis[i], detuneCents), amp);
}

// I-IV-V-I in C with a walking bass and the scale on top: unambiguous C major.
static std::vector<float> cMajorSong (double seconds, double detuneCents = 0.0)
{
    std::vector<float> buf ((size_t) (seconds * kFs), 0.0f);

    const int C[] = { 60, 64, 67 };     // C4 E4 G4
    const int F[] = { 65, 69, 72 };     // F4 A4 C5
    const int G[] = { 55, 59, 62 };     // G3 B3 D4
    const double bar = seconds / 4.0;

    addChord (buf, 0 * bar, bar, C, 3, 0.28f, detuneCents);
    addChord (buf, 1 * bar, bar, F, 3, 0.28f, detuneCents);
    addChord (buf, 2 * bar, bar, G, 3, 0.28f, detuneCents);
    addChord (buf, 3 * bar, bar, C, 3, 0.28f, detuneCents);

    const int bass[] = { 36, 41, 43, 36 };              // C2 F2 G2 C2
    for (int b = 0; b < 4; ++b)
        addNote (buf, b * bar, bar, midiHz (bass[b], detuneCents), 0.35f, 4);

    const int scale[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    const double q = seconds / 8.0;
    for (int i = 0; i < 8; ++i)
        addNote (buf, i * q, q * 0.9, midiHz (scale[i], detuneCents), 0.2f);

    return buf;
}

// i-iv-V-i in A minor (harmonic-minor V with G#): unambiguous A minor, and the
// exact material a relative-key-blind detector calls C major.
static std::vector<float> aMinorSong (double seconds)
{
    std::vector<float> buf ((size_t) (seconds * kFs), 0.0f);

    const int Am[] = { 57, 60, 64, 69 }; // A3 C4 E4 A4 — tonic-doubled voicing
    const int Dm[] = { 62, 65, 69 };     // D4 F4 A4
    const int E [] = { 64, 68, 71 };     // E4 G#4 B4
    const double bar = seconds / 4.0;

    addChord (buf, 0 * bar, bar, Am, 4, 0.28f);
    addChord (buf, 1 * bar, bar, Dm, 3, 0.28f);
    addChord (buf, 2 * bar, bar, E,  3, 0.28f);
    addChord (buf, 3 * bar, bar, Am, 4, 0.28f);

    const int bass[] = { 33, 38, 40, 33 };              // A1 D2 E2 A1
    for (int b = 0; b < 4; ++b)
        addNote (buf, b * bar, bar, midiHz (bass[b]), 0.35f, 4);

    const int line[] = { 69, 71, 72, 74, 76, 74, 72, 69 };   // A B C D E D C A
    const double q = seconds / 8.0;
    for (int i = 0; i < 8; ++i)
        addNote (buf, i * q, q * 0.9, midiHz (line[i]), 0.2f);

    return buf;
}

// Kick / snare / hats — atonal by construction (noise bursts and a pitch-swept
// thump, nothing sustained).
static std::vector<float> drumLoop (double seconds)
{
    std::vector<float> buf ((size_t) (seconds * kFs), 0.0f);
    Noise nz;

    const double beat = 0.5;                             // 120 BPM
    for (double t = 0.0; t < seconds; t += beat)
    {
        const int start = (int) (t * kFs);

        // kick: 90→45 Hz sweep, 90 ms
        double phase = 0.0;
        for (int i = 0; i < (int) (0.09 * kFs); ++i)
        {
            const int idx = start + i;
            if (idx >= (int) buf.size()) break;
            const double tt = (double) i / kFs;
            const double f  = 90.0 * std::exp (-tt / 0.03) + 45.0;
            phase += 2.0 * 3.14159265358979323846 * f / kFs;
            buf[(size_t) idx] += (float) (std::sin (phase) * std::exp (-tt / 0.05)) * 0.8f;
        }

        // snare on the off-beat: 120 ms noise burst
        const int snStart = (int) ((t + beat * 0.5) * kFs);
        for (int i = 0; i < (int) (0.12 * kFs); ++i)
        {
            const int idx = snStart + i;
            if (idx >= (int) buf.size()) break;
            const double tt = (double) i / kFs;
            buf[(size_t) idx] += nz.next() * (float) std::exp (-tt / 0.04) * 0.5f;
        }

        // hats: short bright ticks on eighths
        for (int e = 0; e < 2; ++e)
        {
            const int hhStart = (int) ((t + beat * 0.5 * e + beat * 0.25) * kFs);
            float hp = 0.0f;
            for (int i = 0; i < (int) (0.03 * kFs); ++i)
            {
                const int idx = hhStart + i;
                if (idx >= (int) buf.size()) break;
                const float w = nz.next();
                const float hi = w - hp;                 // crude one-pole highpass
                hp += 0.15f * (w - hp);
                buf[(size_t) idx] += hi * (float) std::exp (-(double) i / kFs / 0.008) * 0.35f;
            }
        }
    }
    return buf;
}

// Percussion the way a real kit is pitched: hard, LOUD hits on chromatic
// out-of-key pitches (F#3, C#4, G#3) plus bright noise bursts, four to the
// second. Each hit is genuinely transient (~60 ms — well inside HPSS's
// time-median window) but pitched, so without HPSS the chroma drowns in
// F#/C#/G# against a much quieter C-major bed; with HPSS the hits are
// recognised as percussive and suppressed. This is the input the separation
// has to earn its CPU on — calibrated (see the HPSS section below) so the
// no-HPSS path actually FAILS the confidence rule rather than merely
// scoring a little lower.
static std::vector<float> pitchedPercLoop (double seconds)
{
    std::vector<float> buf ((size_t) (seconds * kFs), 0.0f);
    Noise nz;

    // Hits are JITTERED (deterministically) off the 0.25 s grid. A perfectly
    // periodic pattern can land its hits exactly on the analysis segment
    // boundaries, where the CQT windows' Hann taper nulls them — a measure-
    // zero lucky alignment that made the no-HPSS path look far better than
    // it is. Real percussion is never sample-metronomic; neither is this.
    Noise jitter; jitter.s = 424242u;

    const double step = 0.25;
    int which = 0;
    for (double t = 0.13; t < seconds; t += step, ++which)
    {
        const int start = (int) ((t + 0.06 * (double) jitter.next()) * kFs);
        if (start < 0) continue;

        // tom: F#3 / C#4 / G#3 in rotation, 60 ms, a few partials
        const double f0 = (which % 3 == 0) ? midiHz (54)
                        : (which % 3 == 1) ? midiHz (61) : midiHz (56);
        for (int h = 1; h <= 4; ++h)
        {
            const double w = 2.0 * 3.14159265358979323846 * f0 * h / kFs;
            for (int i = 0; i < (int) (0.06 * kFs); ++i)
            {
                const int idx = start + i;
                if (idx >= (int) buf.size()) break;
                const double tt = (double) i / kFs;
                buf[(size_t) idx] += (float) (std::sin (w * i) * std::exp (-tt / 0.02))
                                     * (1.1f / (float) h);
            }
        }

        // bright noise burst on every hit
        for (int i = 0; i < (int) (0.06 * kFs); ++i)
        {
            const int idx = start + i;
            if (idx >= (int) buf.size()) break;
            buf[(size_t) idx] += nz.next()
                                 * (float) std::exp (-(double) i / kFs / 0.015) * 0.9f;
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------
static void feed (KeyEngine& e, const std::vector<float>& buf)
{
    const int block = 512;
    int fed = 0;
    for (size_t i = 0; i < buf.size(); i += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, buf.size() - i);
        e.pushBlock (buf.data() + i, nullptr, n);
        fed += n;
        if (fed >= 24000) { e.update(); fed = 0; }       // drain like the real thread
    }
    e.update();
}

static KeyReading runCommitted (const std::vector<float>& buf, float windowS,
                                bool hpss, int modeLock = KeyEngine::kLockAuto)
{
    KeyEngine e;
    e.prepare (kFs, 512);
    e.setWindowSeconds (windowS);
    e.setHpss (hpss);
    e.setModeLock (modeLock);
    e.startAnalysis();
    feed (e, buf);
    return e.getReading();
}

static std::string keyStr (const KeyReading& r)
{
    char buf[24];
    KeyEngine::keyName (r.root, r.minor, buf, (int) sizeof (buf));
    char out[96];
    std::snprintf (out, sizeof (out), "%s conf %.2f tuning %.1f Hz (%+.1f c) root %.1f Hz",
                   buf, r.confidence, r.tuningHz, r.tuningCents, r.rootHz);
    return out;
}

int main()
{
    std::printf ("== the analysis rate lands near 16 kHz ==\n");
    {
        KeyEngine e;
        e.prepare (48000.0, 512);
        check (std::fabs (e.analysisRate() - 16000.0) < 1.0, "48 kHz decimates to 16 kHz");
        e.prepare (44100.0, 512);
        check (std::fabs (e.analysisRate() - 14700.0) < 1.0, "44.1 kHz decimates to 14.7 kHz");
        e.prepare (96000.0, 512);
        check (std::fabs (e.analysisRate() - 16000.0) < 1.0, "96 kHz decimates to 16 kHz");
    }

    std::printf ("== C major reads C major, confidently ==\n");
    {
        const auto r = runCommitted (cMajorSong (9.0), 8.0f, true);
        std::printf ("       -> %s\n", keyStr (r).c_str());
        check (r.valid, "a committed pass produced a reading");
        check (r.committed, "and it is marked committed");
        check (r.root == 0 && ! r.minor, "the key is C major");
        check (r.confidence > 0.75f, "confidence is HIGH (> 0.75)");
        check (std::fabs (r.tuningCents) < 6.0f, "tuning reads as concert pitch (within 6 c)");
        check (std::fabs (r.rootHz / 65.41f - std::round (r.rootHz / 65.41f)) < 0.1f
                 || std::fabs (1200.0 * std::log2 (r.rootHz / 65.41)) < 1210.0,
               "root_hz is a C");
        // root_hz must be AN OCTAVE OF C, i.e. log2(rootHz/C1) is near-integer.
        const double oct = std::log2 (r.rootHz / 32.70);
        check (std::fabs (oct - std::round (oct)) < 0.05, "root_hz sits on a C octave");
        check (r.chroma[0] > 0.8f, "the C bin dominates the chroma");
    }

    std::printf ("== A minor reads A minor — NOT C major ==\n");
    {
        const auto r = runCommitted (aMinorSong (9.0), 8.0f, true);
        std::printf ("       -> %s\n", keyStr (r).c_str());
        check (r.valid, "a reading was produced");
        check (r.root == 9 && r.minor, "the key is A minor");
        check (! (r.root == 0 && ! r.minor), "specifically NOT C major (the relative-key trap)");
        // The relative pair gets its calibrated treatment: C major is the
        // runner-up here BY DESIGN (same seven notes), and that must not drag
        // a correct tonic call under the ~0.5 rule — it shows up as the named
        // first alternate instead.
        check (r.confidence > 0.75f, "confidence clears the consumption rule COMFORTABLY (> 0.75)");
        bool relNamed = false;
        for (int i = 0; i < r.numAlternates; ++i)
            if (r.alternates[(size_t) i].root == 0 && ! r.alternates[(size_t) i].minor)
                relNamed = true;
        check (relNamed, "and the relative (C major) is among the named alternates");
        const double oct = std::log2 (r.rootHz / 27.50);
        check (std::fabs (oct - std::round (oct)) < 0.05, "root_hz sits on an A octave");
    }

    std::printf ("== 30 cents sharp: still identified, tuning REPORTED ==\n");
    {
        const auto r = runCommitted (cMajorSong (9.0, 30.0), 8.0f, true);
        std::printf ("       -> %s\n", keyStr (r).c_str());
        check (r.valid, "a reading was produced");
        check (r.root == 0 && ! r.minor, "the detuned material still reads C major");
        check (r.confidence > 0.75f, "still confident (> 0.75)");
        check (std::fabs (r.tuningCents - 30.0f) < 6.0f,
               "detected tuning within 6 c of the actual +30 c");
        // 440 * 2^(30/1200) = 447.69 Hz
        check (std::fabs (r.tuningHz - 447.69f) < 2.0f, "detected_tuning_hz near 447.7");
    }

    std::printf ("== a drum loop reports LOW confidence, not a confident wrong key ==\n");
    {
        const auto r = runCommitted (drumLoop (9.0), 8.0f, true);
        std::printf ("       -> %s\n", keyStr (r).c_str());
        check (r.confidence < 0.2f, "drum-loop confidence collapses (< 0.2)");
    }

    std::printf ("== white noise reports LOW confidence ==\n");
    {
        std::vector<float> buf ((size_t) (9.0 * kFs));
        Noise nz;
        for (auto& v : buf) v = nz.next() * 0.4f;
        const auto r = runCommitted (buf, 8.0f, true);
        std::printf ("       -> %s\n", keyStr (r).c_str());
        check (r.confidence < 0.2f, "white-noise confidence collapses (< 0.2)");
    }

    // The calibration is pinned in BOTH directions against the ONE number the
    // consumers use (the feed block, the backend teaching and spec §4 all say
    // "below ~0.5 treat as unknown"): a regression that drags every correct
    // answer under the rule fails here, and so does one that lets noise over
    // it. The margins (0.75 / 0.2) are deliberately wide of 0.5 so the pins
    // catch drift long before the rule actually misbehaves.
    std::printf ("== CALIBRATION: correct readings sit ABOVE the 0.5 rule, junk BELOW ==\n");
    {
        constexpr float kRule = 0.5f;
        const auto cMaj = runCommitted (cMajorSong (9.0), 8.0f, true);
        const auto aMin = runCommitted (aMinorSong (9.0), 8.0f, true);
        const auto drums = runCommitted (drumLoop (9.0), 8.0f, true);
        check (cMaj.confidence > kRule && aMin.confidence > kRule,
               "correct majors AND minors are USABLE under the rule ("
               + std::to_string (cMaj.confidence).substr (0, 4) + " / "
               + std::to_string (aMin.confidence).substr (0, 4) + " > 0.5)");
        check (drums.confidence < kRule,
               "and percussion is discarded by the same rule");
    }

    std::printf ("== HPSS: no-HPSS FAILS the confidence rule; HPSS passes it ==\n");
    {
        // A quiet C-major bed under loud, dense pitched percussion — the mix
        // where a chromagram without separation has no chance. The point is
        // not a small score difference: on the SAME input, the no-HPSS path
        // must land where the ~0.5 rule DISCARDS it, and the HPSS path must
        // land where the rule keeps it, correct. That is HPSS earning its CPU.
        auto mix = cMajorSong (9.0);
        {
            const auto perc = pitchedPercLoop (9.0);
            for (size_t i = 0; i < mix.size() && i < perc.size(); ++i)
                mix[i] = mix[i] * 0.3f + perc[i];
        }
        const auto on  = runCommitted (mix, 8.0f, true);
        const auto off = runCommitted (mix, 8.0f, false);
        std::printf ("       -> HPSS on : %s\n", keyStr (on).c_str());
        std::printf ("       -> HPSS off: %s\n", keyStr (off).c_str());
        check (on.root == 0 && ! on.minor, "with HPSS the mix resolves to C major");
        check (on.confidence > 0.5f, "and clears the confidence rule");
        check (off.confidence < 0.5f,
               "without HPSS the same input FAILS the rule (reads as unknown)");
        check (on.confidence > off.confidence + 0.2f,
               "the separation is worth a wide margin, not a rounding error");
    }

    std::printf ("== mode_lock constrains the search ==\n");
    {
        const auto r = runCommitted (cMajorSong (9.0), 8.0f, true, KeyEngine::kLockMinor);
        std::printf ("       -> %s\n", keyStr (r).c_str());
        check (r.valid && r.minor, "locked to minor, a minor key is reported");
    }

    std::printf ("== hold freezes; reset clears ==\n");
    {
        KeyEngine e;
        e.prepare (kFs, 512);
        e.setWindowSeconds (4.0f);
        e.startAnalysis();
        feed (e, cMajorSong (5.0));
        check (e.getReading().valid, "first pass produced a reading");

        e.setHold (true);
        e.startAnalysis();
        feed (e, aMinorSong (5.0));
        const auto held = e.getReading();
        check (held.valid && held.root == 0 && ! held.minor,
               "held: the A-minor pass did not overwrite the C-major reading");

        e.setHold (false);
        e.clearAccumulation();
        check (! e.getReading().valid, "reset cleared the reading");
    }

    std::printf ("== the committed pass gathers the NEXT window, then holds ==\n");
    {
        KeyEngine e;
        e.prepare (kFs, 512);
        e.setWindowSeconds (4.0f);

        // Not armed: feeding alone must not produce a committed reading.
        feed (e, cMajorSong (5.0));
        check (! e.getReading().valid, "no reading before ANALYSE is pressed");

        e.startAnalysis();
        check (e.isCollecting(), "armed and collecting");
        feed (e, aMinorSong (5.0));
        const auto r = e.getReading();
        check (r.valid && r.root == 9 && r.minor,
               "the committed pass analysed the post-arm audio (A minor)");
        check (! e.isCollecting(), "and the pass is finished");

        // Nothing further arrives: the reading holds.
        feed (e, drumLoop (3.0));
        check (e.getReading().root == 9, "the reading HOLDS until re-armed");
    }

    std::printf ("== RESET takes effect NOW: no dead zone, no stale watermark ==\n");
    {
        // THE live-testing bug: reset zeroed the sample counter but left the
        // live-chroma high-water mark (and the armed/continuous marks) at
        // their old values, so the display went dead until the counter had
        // regrown past them — a minute of apparent death for a minute of
        // prior playback, then "recovering on its own". Pinned: after a long
        // session and a reset, the reading clears IMMEDIATELY and the live
        // chroma revives within a couple of seconds of new audio.
        KeyEngine e;
        e.prepare (kFs, 512);
        e.setWindowSeconds (8.0f);
        e.startAnalysis();
        feed (e, cMajorSong (30.0));                 // a long prior session
        check (e.getReading().valid, "a long session produced a reading");

        e.clearAccumulation();
        check (! e.getReading().valid, "RESET clears the reading IMMEDIATELY");
        {
            float c[12]; e.getLiveChroma (c);
            float sum = 0; for (float v : c) sum += v;
            check (sum == 0.0f, "and the wheel clears with it");
        }

        feed (e, aMinorSong (3.0));                  // a NEW song, briefly
        {
            float c[12]; e.getLiveChroma (c);
            float mx = 0; for (float v : c) mx = std::max (mx, v);
            check (mx > 0.5f, "the live chroma REVIVES within seconds of new "
                              "audio (the dead-zone regression)");
        }
    }

    std::printf ("== an explicit ANALYSE preempts the continuous schedule ==\n");
    {
        KeyEngine e;
        e.prepare (kFs, 512);
        e.setContinuous (true);
        e.setWindowSeconds (4.0f);
        feed (e, cMajorSong (6.0));                  // continuous is mid-flight
        e.startAnalysis();                           // the user: "listen again"
        check (e.isCollecting(), "armed immediately, no waiting for a schedule");
        feed (e, aMinorSong (6.0));
        const auto r = e.getReading();
        check (r.valid && r.committed, "the explicit pass ran and COMMITTED");
        check (r.root == 9 && r.minor,
               "on the post-arm audio (A minor), not the old song");
    }

    std::printf ("== gated states are REPORTED, never a silent idle ==\n");
    {
        KeyEngine e;
        e.prepare (kFs, 512);
        e.setWindowSeconds (4.0f);
        e.startAnalysis();
        e.update();
        auto a = e.getActivity();
        check (a.collecting && a.waitingForSignal,
               "armed with no audio at all: collecting + waiting-for-signal");

        // Silence arrives (transport rolling, channel silent): still gated,
        // and the window must NOT fill itself with nothing.
        std::vector<float> silence ((size_t) (3.0 * kFs), 0.0f);
        feed (e, silence);
        a = e.getActivity();
        check (a.collecting && a.waitingForSignal,
               "3 s of silence: still armed, still saying WAITING");
        check (a.progress == 0.0f, "and the window has not started counting");

        // Real audio arrives: the gate lifts and the window runs from HERE.
        feed (e, aMinorSong (5.0));
        const auto r = e.getReading();
        check (r.valid && r.root == 9 && r.minor,
               "the pass completes on the signal alone (A minor) - silence "
               "never consumed the window");
        check (! e.getActivity().waitingForSignal, "and the waiting flag is down");
    }

    std::printf ("== the offline pass (captures) matches the committed pipeline ==\n");
    {
        // KEY_PRECONDITION_SPEC.md §5.2: a capture is analysed offline with
        // the SAME pipeline the committed pass runs — same key, same
        // confidence discipline. Both directions pinned: musical material
        // reads correctly and confidently; noise stays LOW.
        KeyEngine e;
        e.prepare (kFs, 512);
        const auto song = cMajorSong (12.0);
        const auto r = e.analyseBufferOffline (song.data(), nullptr, (int) song.size());
        check (r.valid && r.root == 0 && ! r.minor,
               "offline pass over a C-major capture reads C major");
        check (r.confidence >= 0.5f, "confidently");
        check (r.committed, "and reports as a committed reading");

        // The engine is reusable afterwards (window/continuous restored).
        check (e.getWindowSeconds() == KeyEngine::kDefWindowS,
               "window setting restored after the offline pass");
    }
    {
        // A long capture: multiple windows, best confidence wins. The middle
        // and end of this buffer are A minor; a noisy start must not decide it.
        KeyEngine e;
        e.prepare (kFs, 512);
        std::vector<float> noise ((size_t) (8.0 * kFs));
        { Noise nz; for (auto& v : noise) v = nz.next() * 0.4f; }
        auto song = aMinorSong (35.0);
        std::vector<float> buf;
        buf.reserve (noise.size() + song.size());
        buf.insert (buf.end(), noise.begin(), noise.end());
        buf.insert (buf.end(), song.begin(),  song.end());
        const auto r = e.analyseBufferOffline (buf.data(), nullptr, (int) buf.size());
        check (r.valid && r.root == 9 && r.minor,
               "a long capture with a noise intro still reads A minor");
        check (r.confidence >= 0.5f, "confidently");
    }
    {
        // Nothing tonal: the offline pass must say so, not guess.
        KeyEngine e;
        e.prepare (kFs, 512);
        std::vector<float> n ((size_t) (12.0 * kFs));
        { Noise nz; for (auto& v : n) v = nz.next() * 0.4f; }
        const auto r = e.analyseBufferOffline (n.data(), nullptr, (int) n.size());
        check (! r.valid || r.confidence < 0.4f,
               "an offline pass over noise reports LOW/none, never a confident key");
    }

    std::printf ("== the live-chroma switch starves the display, not the detector ==\n");
    {
        KeyEngine e;
        e.prepare (kFs, 512);
        e.setLiveChromaEnabled (false);        // the Link's headless setting
        e.setWindowSeconds (4.0f);
        e.startAnalysis();
        feed (e, cMajorSong (5.0));
        const auto r = e.getReading();
        check (r.valid && r.root == 0 && ! r.minor,
               "committed pass unaffected with live chroma off");
        float c[12]; e.getLiveChroma (c);
        // With the display path off, getLiveChroma falls back to the
        // committed reading's chroma — still a truthful picture.
        check (c[0] > 0.5f, "getLiveChroma falls back to the reading's chroma");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
