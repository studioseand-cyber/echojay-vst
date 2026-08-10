// Standalone test for PitchCorrect (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source pitch_correct_test.cpp -o correcttest && ./correcttest
//
// P2 acceptance (PITCH_CORRECTION_SPEC.md §2.2, §2.3). The decision stage and
// the retune envelope, each pinned separately, because "it sounds tuned" is not
// a test and the two failure modes look identical at the output.

#include "EedPitchCorrect.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static float hz (float cents, float ref = 440.0f)
{ return ref * std::pow (2.0f, cents / 1200.0f); }

static float cents (float f, float ref = 440.0f)
{ return 1200.0f * std::log2 (f / ref); }

// Drive n hops of a steady pitch, return the final target in cents.
static float settle (PitchCorrect& c, float f0, int hops, bool voiced = true)
{
    float out = 0.0f;
    for (int i = 0; i < hops; ++i) out = c.process (f0, voiced);
    return out > 0.0f ? cents (out) : -99999.0f;
}

// In-place init: PitchCorrect holds atomics and cannot be returned by value.
static void makeMajor (PitchCorrect& c, float retuneMs, float flex, float humanize)
{
    c.prepare (48000.0, 128);          // 2.67 ms per hop
    c.initDegrees();
    c.setAllDegrees (false);
    // C major relative to the root.
    for (int s : { 0, 2, 4, 5, 7, 9, 11 }) c.setDegree (s, true, 0.0f);
    c.setKeyRoot (0);
    c.setRetuneMs (retuneMs);
    c.setFlex (flex);
    c.setHumanize (humanize);
    c.setIgnoreVibrato (false);
    c.reset();
}

int main()
{
    std::printf ("== the decision: nearest ENABLED degree ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        // A440 is +0 cents. In C major the degrees are C D E F G A B.
        // 440 Hz is A = 900 cents above C. Ask for something 30 cents sharp.
        check (std::fabs (c.nearestDegreeCents (930.0f) - 900.0f) < 0.01f,
               "930 cents snaps back to A (900)");
        check (std::fabs (c.nearestDegreeCents (1010.0f) - 1100.0f) < 0.01f,
               "1010 cents snaps up to B (1100), not to the disabled A# (1000)");
        // A# is NOT in C major: a note sitting exactly on it must go to a
        // neighbour rather than staying put.
        const float got = c.nearestDegreeCents (1000.0f);
        check (std::fabs (got - 900.0f) < 0.01f || std::fabs (got - 1100.0f) < 0.01f,
               "a disabled degree is never a target");
    }

    std::printf ("== bias_cents moves the degree itself ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        c.setDegree (9, true, -14.0f);          // pull A 14 cents flat
        check (std::fabs (c.nearestDegreeCents (900.0f) - 886.0f) < 0.01f,
               "A with bias -14 targets 886 cents");
    }

    std::printf ("== octave boundaries: the nearest degree can be in the next octave ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        // 1190 cents is just under C an octave up (1200), which IS enabled.
        check (std::fabs (c.nearestDegreeCents (1190.0f) - 1200.0f) < 0.01f,
               "1190 snaps up across the octave line to 1200");
        check (std::fabs (c.nearestDegreeCents (-10.0f) - 0.0f) < 0.01f,
               "-10 snaps up to 0");
    }

    std::printf ("== key root moves the whole scale ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        c.setKeyRoot (2);                        // D major
        // In D major, F# (600 cents above C) is a degree; F natural (500) is not.
        check (std::fabs (c.nearestDegreeCents (600.0f) - 600.0f) < 0.01f,
               "F# is in D major and stays put");
        const float f = c.nearestDegreeCents (500.0f);
        check (std::fabs (f - 500.0f) > 1.0f, "F natural is not, and is moved");
    }

    std::printf ("== retune 0 is the HARD-TUNED effect: instant, exact ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        const float out = settle (c, hz (930.0f), 2);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "930 cents in -> %.1f cents out on the 2nd hop", out);
        check (std::fabs (out - 900.0f) < 1.0f, msg);
    }

    std::printf ("== retune 120 ms is a GLIDE, not a snap ==\n");
    {
        PitchCorrect c; makeMajor (c, 120.0f, 0.0f, 0.0f);
        c.process (hz (930.0f), true);            // establishes the note at 930
        const float after1 = cents (c.process (hz (930.0f), true));
        // One hop is 2.67 ms against a 120 ms constant: barely any movement.
        check (std::fabs (after1 - 930.0f) < 5.0f, "after one hop it has hardly moved");
        float out = 0.0f;
        for (int i = 0; i < 200; ++i) out = c.process (hz (930.0f), true);  // ~530 ms
        char msg[128];
        std::snprintf (msg, sizeof (msg), "after 530 ms it has arrived: %.1f cents", cents (out));
        check (std::fabs (cents (out) - 900.0f) < 5.0f, msg);
    }

    std::printf ("== flex leaves small deviation alone and still fixes gross error ==\n");
    {
        PitchCorrect tight; makeMajor (tight, 0.0f, 0.0f, 0.0f);
        PitchCorrect loose; makeMajor (loose, 0.0f, 60.0f, 0.0f);

        const float smallTight = settle (tight, hz (915.0f), 4);   // 15 cents sharp
        const float smallLoose = settle (loose, hz (915.0f), 4);
        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "15 cents sharp: flex 0 -> %.1f (corrected), flex 60 -> %.1f (left alone)",
                       smallTight, smallLoose);
        check (std::fabs (smallTight - 900.0f) < 1.0f
                 && std::fabs (smallLoose - 915.0f) < 8.0f, msg);

        // A gross error is still corrected at high flex - but give it time.
        // Since the note and the wobble are now separated, a NEW deviation is
        // treated as movement WITHIN the note until the slow pitch catches up
        // (~140 ms), and only then judged as the note being off. That is a real
        // and deliberate property: it stops the corrector chasing a scoop, at
        // the cost of engaging slightly later on a genuine drift.
        const float grossLoose = settle (loose, hz (960.0f), 150);   // ~400 ms
        std::snprintf (msg, sizeof (msg), "60 cents sharp at flex 60 settles to %.1f", grossLoose);
        check (std::fabs (grossLoose - 900.0f) < 20.0f, msg);
    }

    std::printf ("== humanize relaxes SUSTAINED notes, judged from stability ==\n");
    {
        PitchCorrect none; makeMajor (none, 0.0f, 0.0f, 0.0f);
        PitchCorrect soft; makeMajor (soft, 0.0f, 0.0f, 80.0f);

        // Onset: both correct fully, because nothing is sustained yet.
        const float onsetNone = settle (none, hz (925.0f), 3);
        const float onsetSoft = settle (soft, hz (925.0f), 3);
        char msg[180];
        std::snprintf (msg, sizeof (msg), "at the onset both correct: %.1f / %.1f",
                       onsetNone, onsetSoft);
        check (std::fabs (onsetNone - 900.0f) < 2.0f
                 && std::fabs (onsetSoft - 900.0f) < 2.0f, msg);

        // Hold it past the sustain threshold: humanize should back off.
        const float heldNone = settle (none, hz (925.0f), 120);   // ~320 ms
        const float heldSoft = settle (soft, hz (925.0f), 120);
        std::snprintf (msg, sizeof (msg),
                       "once sustained: humanize 0 -> %.1f (still tight), 80 -> %.1f (relaxed)",
                       heldNone, heldSoft);
        check (std::fabs (heldNone - 900.0f) < 2.0f && heldSoft > 910.0f, msg);
    }

    std::printf ("== transpose shifts the RESULT, after correction ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        c.setTranspose (12.0f);
        const float out = settle (c, hz (930.0f), 3);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "930 in, +12 st -> %.1f cents (want 2100)", out);
        check (std::fabs (out - 2100.0f) < 2.0f, msg);
    }

    std::printf ("== NOTE CHANGE resets the envelope: an interval is not a portamento ==\n");
    {
        PitchCorrect c; makeMajor (c, 120.0f, 0.0f, 0.0f);
        settle (c, hz (900.0f), 200);                       // settled on A
        // Jump a fifth up to E (1600 cents). Hold long enough to confirm.
        float out = 0.0f;
        for (int i = 0; i < 20; ++i) out = c.process (hz (1600.0f), true);   // ~53 ms
        const float got = cents (out);
        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "after a fifth jump the output is at the NEW note (%.0f), not gliding from the old", got);
        check (got > 1500.0f, msg);
        check (c.noteChanges() >= 1, "the note change was counted");
    }

    std::printf ("== a SCOOP into a note is not treated as a note change ==\n");
    {
        PitchCorrect c; makeMajor (c, 120.0f, 0.0f, 0.0f);
        settle (c, hz (900.0f), 100);
        const uint32_t before = c.noteChanges();
        // Slide up 150 cents over ~50 ms, passing through the threshold, then
        // come back. A corrector that resets on the way through would chop it.
        for (int i = 0; i < 10; ++i) c.process (hz (900.0f + 15.0f * i), true);
        for (int i = 10; i > 0; --i) c.process (hz (900.0f + 15.0f * i), true);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "note changes during a scoop: %u (want 0)",
                       c.noteChanges() - before);
        check (c.noteChanges() == before, msg);
    }

    std::printf ("== GAP RESUME is not a note change - the 142 ms case, explicitly ==\n");
    {
        // The measured worst-case mid-phrase gap. It MUST resume the note.
        const int hops142 = (int) std::lround (142.0 / 2.67);
        PitchCorrect c; makeMajor (c, 120.0f, 0.0f, 0.0f);
        settle (c, hz (930.0f), 200);
        const uint32_t changesBefore = c.noteChanges();
        const float beforeGap = c.lastTargetCents();

        for (int i = 0; i < hops142; ++i) c.process (0.0f, false);
        check (c.inNote(), "still inside the note after a 142 ms gap");

        const float after = cents (c.process (hz (930.0f), true));
        char msg[180];
        std::snprintf (msg, sizeof (msg),
                       "resumed at %.1f cents having held %.1f - moved %.1f (want < 5)",
                       after, beforeGap, std::fabs (after - beforeGap));
        check (std::fabs (after - beforeGap) < 5.0f, msg);
        check (c.noteChanges() == changesBefore, "and it was NOT counted as a note change");
        check (c.gapResumes() >= 1, "it was counted as a gap resume");
    }

    std::printf ("== a LONG gap IS a new note ==\n");
    {
        const int hops400 = (int) std::lround (400.0 / 2.67);
        PitchCorrect c; makeMajor (c, 120.0f, 0.0f, 0.0f);
        settle (c, hz (930.0f), 200);
        for (int i = 0; i < hops400; ++i) c.process (0.0f, false);
        check (! c.inNote(), "a 400 ms gap ends the note");
        // Restarting on a different pitch must begin AT it, not glide.
        const float out = cents (c.process (hz (500.0f), true));
        char msg[128];
        std::snprintf (msg, sizeof (msg), "restarts at the new pitch: %.1f cents (want ~500)", out);
        check (std::fabs (out - 500.0f) < 60.0f, msg);
    }

    std::printf ("== targeting_ignores_vibrato stops the target chattering ==\n");
    {
        // Vibrato +/-70 cents straddling the boundary between two degrees.
        // With the guard OFF the target flips; with it ON it should not.
        auto runVibrato = [] (bool ignore, int& flips)
        {
            PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
            c.setIgnoreVibrato (ignore);
            // Centre between A (900) and B (1100) is 1000 - the worst place.
            float last = 0.0f; flips = 0;
            for (int i = 0; i < 400; ++i)
            {
                // Centred 30 cents ABOVE the midpoint, not exactly on it: a
                // vibrato centred precisely on a boundary crosses it however
                // hard it is smoothed, which tests the smoother's residual
                // rather than the guard. This is the real case - a singer near
                // a boundary, wobbling across it.
                const float dev = 70.0f * std::sin (2.0 * M_PI * 5.5 * i * 0.00267);
                const float out = c.process (hz (1030.0f + dev), true);
                const float t = cents (out);
                if (i > 60)
                {
                    const float snapped = t > 1000.0f ? 1100.0f : 900.0f;
                    if (last != 0.0f && snapped != last) ++flips;
                    last = snapped;
                }
                else if (i == 60) last = t > 1000.0f ? 1100.0f : 900.0f;
            }
        };
        int flipsOff = 0, flipsOn = 0;
        runVibrato (false, flipsOff);
        runVibrato (true,  flipsOn);
        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "target flips across the boundary: guard off %d, guard on %d",
                       flipsOff, flipsOn);
        check (flipsOff > flipsOn, msg);
    }

    std::printf ("== ADDED vibrato: off by default, and fades in from the note start ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        check (c.getVibDepthCents() == 0.0f, "added vibrato is OFF by default");

        settle (c, hz (900.0f), 200);
        check (std::abs (c.vibratoCents()) < 0.01f, "...and adds nothing while off");

        PitchCorrect v; makeMajor (v, 0.0f, 0.0f, 0.0f);
        v.setVibDepthCents (50.0f);
        v.setVibRateHz (5.5f);
        v.setVibOnsetMs (300.0f);

        // At the attack the onset fade holds it near zero.
        v.process (hz (900.0f), true);
        v.process (hz (900.0f), true);
        check (std::abs (v.vibratoCents()) < 8.0f, "at the onset it is still nearly silent");

        // Once the note has been held past the onset, it is present and moving.
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 400; ++i)
        {
            v.process (hz (900.0f), true);
            if (i > 200) { lo = std::min (lo, v.vibratoCents()); hi = std::max (hi, v.vibratoCents()); }
        }
        char msg[160];
        std::snprintf (msg, sizeof (msg), "after the onset it swings %.0f cents peak to peak (want ~100)", hi - lo);
        check (hi - lo > 70.0f && hi - lo < 130.0f, msg);
    }

    std::printf ("== natural_vibrato scales the SINGER'S movement, not a generator ==\n");
    {
        // A note held dead straight has no vibrato to scale, so the control
        // must do nothing at all - that is what makes it different from the
        // added-vibrato generator.
        PitchCorrect flat0; makeMajor (flat0, 0.0f, 0.0f, 0.0f);
        flat0.setNaturalVibrato (0.0f);
        const float straight = settle (flat0, hz (900.0f), 200);
        check (std::abs (straight - 900.0f) < 2.0f,
               "on a dead-straight note, natural_vibrato 0 changes nothing");

        // On a note that IS wobbling, 0 should flatten it and 200 exaggerate.
        // Measured at a REALISTIC setting. natural_vibrato scales the
        // oscillation in the pitch the corrector aims at, so it only has
        // meaning where correction is not total - at retune 0 with flex 0 the
        // vibrato is gone by definition and the control has nothing to scale.
        // Using flex here instead would confound the two: flex ALSO decides how
        // much deviation survives, and the test could not say which acted.
        auto swing = [] (float natPct)
        {
            PitchCorrect c; makeMajor (c, 120.0f, 0.0f, 0.0f);   // transparent retune
            c.setNaturalVibrato (natPct);
            c.setIgnoreVibrato (true);
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < 500; ++i)
            {
                const float dev = 60.0f * std::sin (2.0 * M_PI * 5.5 * i * 0.00267);
                const float out = c.process (hz (900.0f + dev), true);
                if (i > 250) { const float t = cents (out); lo = std::min (lo, t); hi = std::max (hi, t); }
            }
            return hi - lo;
        };
        const float kept = swing (100.0f);
        const float gone = swing (0.0f);
        const float more = swing (200.0f);
        char msg[180];
        std::snprintf (msg, sizeof (msg),
                       "peak-to-peak: 0%% -> %.0f c, 100%% -> %.0f c, 200%% -> %.0f c",
                       gone, kept, more);
        check (gone < kept * 0.6f && more > kept * 1.4f, msg);
    }

    std::printf ("== unvoiced returns 0: the caller must leave the frame alone ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        check (c.process (0.0f, false) == 0.0f, "unvoiced yields no target");
        check (c.process (220.0f, false) == 0.0f, "voiced=false yields no target even with an f0");
    }

    std::printf ("== reference_hz moves everything with it ==\n");
    {
        PitchCorrect c; makeMajor (c, 0.0f, 0.0f, 0.0f);
        c.setReferenceHz (441.3f);
        // A note exactly on A at the NEW reference must be left alone.
        // Measured against the SAME reference the corrector is using - reading
        // the result in 440-cents would just re-measure the reference change.
        const float inHz = 441.3f * std::pow (2.0f, 900.0f / 1200.0f);
        float outHz = 0.0f;
        for (int i = 0; i < 3; ++i) outHz = c.process (inHz, true);
        const float outCents = cents (outHz, 441.3f);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "at ref 441.3 an in-tune A stays put: %.1f cents", outCents);
        check (std::fabs (outCents - 900.0f) < 2.0f, msg);
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
