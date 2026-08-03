// Standalone test for note<->frequency and the preset table (P5 of
// SURGICAL_EQ_ENHANCEMENTS.md). No JUCE. Build:
//   g++ -std=c++17 -O2 -I../Source eq_note_test.cpp -o notetest && ./notetest

#include "EqNote.h"
#include "EqPresets.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

using namespace echojay;

static int g_fail = 0;
static void check (bool cond, const std::string& what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (! cond) ++g_fail;
}

int main()
{
    // -----------------------------------------------------------------------
    std::printf ("== note -> frequency ==\n");
    {
        struct { const char* s; float hz; } ok[] = {
            { "A4",  440.0f    }, { "a4",  440.0f   },
            { "G5",  783.99f   }, { "C#3", 138.59f  },
            { "Db3", 138.59f   }, { "Bb2", 116.54f  },
            { "C0",  16.35f    }, { "E2",  82.41f   },
            { "As4", 466.16f   },                       // 's' sharp alias
            { "C-1", 8.18f     },                       // MIDI 0
        };
        bool all = true;
        for (const auto& t : ok)
        {
            float hz = 0.0f;
            const bool got = parseNoteToFreq (t.s, hz);
            const bool near = got && std::fabs (hz - t.hz) < t.hz * 0.001f;
            if (! near)
                std::printf ("       %s -> %f (want %f)\n", t.s, hz, t.hz);
            all = all && near;
        }
        check (all, "every spelling lands within 0.1%");

        float hz = 999.0f;
        check (! parseNoteToFreq ("H4", hz)   && hz == 999.0f, "H4 is rejected, out untouched");
        check (! parseNoteToFreq ("A", hz),    "no octave is rejected");
        check (! parseNoteToFreq ("A4x", hz),  "trailing junk is rejected");
        check (! parseNoteToFreq ("", hz),     "empty is rejected");
        check (! parseNoteToFreq ("A-2", hz),  "below MIDI 0 is rejected");
        check (! parseNoteToFreq (nullptr, hz),"nullptr is rejected");
    }

    // -----------------------------------------------------------------------
    std::printf ("== frequency -> nearest note ==\n");
    {
        char buf[16];
        check (describeFreqAsNote (440.0f, buf, sizeof (buf))
               && std::strcmp (buf, "A4") == 0,
               std::string ("440 -> \"") + buf + "\"");

        check (describeFreqAsNote (446.0f, buf, sizeof (buf))
               && std::strcmp (buf, "A4 +23c") == 0,
               std::string ("446 -> \"") + buf + "\"");

        check (describeFreqAsNote (781.0f, buf, sizeof (buf))
               && std::strncmp (buf, "G5", 2) == 0,
               std::string ("781 -> \"") + buf + "\" (nearest G5)");

        check (! describeFreqAsNote (0.0f, buf, sizeof (buf)), "0 Hz has no note");
        check (! describeFreqAsNote (-5.0f, buf, sizeof (buf)), "negative has no note");
    }

    // -----------------------------------------------------------------------
    std::printf ("== round trip: every note names itself ==\n");
    {
        bool all = true;
        const char* names[] = { "C2", "E3", "G#4", "B5", "F#7" };
        for (const char* n : names)
        {
            float hz = 0.0f; char buf[16];
            all = all && parseNoteToFreq (n, hz)
                      && describeFreqAsNote (hz, buf, sizeof (buf))
                      && std::strcmp (buf, n) == 0;
        }
        check (all, "parse -> describe is the identity on exact pitches");
    }

    // -----------------------------------------------------------------------
    std::printf ("== preset table sanity ==\n");
    {
        check (kNumEqPresets == 6, "six built-ins");

        bool sane = true;
        for (const auto& p : kEqPresets)
        {
            sane = sane && p.name != nullptr && p.blurb != nullptr
                        && p.bands != nullptr && p.numBands >= 1
                        && p.numBands <= EqEngine::kMaxBands
                        && p.outputDb >= -24.0f && p.outputDb <= 24.0f;
            for (int i = 0; i < p.numBands; ++i)
            {
                const BandSpec& b = p.bands[i];
                sane = sane && b.enabled
                            && b.freqHz >= 16.0f && b.freqHz <= 20000.0f
                            && b.gainDb >= -24.0f && b.gainDb <= 24.0f
                            && b.q >= 0.1f && b.q <= 40.0f;
            }
        }
        check (sane, "every preset's every band is in range and enabled");
    }

    // -----------------------------------------------------------------------
    std::printf ("== preset lookup tolerance ==\n");
    {
        check (findEqPreset ("vocal-clarity") == &kEqPresets[0], "exact name");
        check (findEqPreset ("Vocal Clarity") == &kEqPresets[0], "spaces + case");
        check (findEqPreset ("vocal_clarity") == &kEqPresets[0], "underscores");
        check (findEqPreset ("airlift")       != nullptr,        "no separator at all");
        check (findEqPreset ("air-lift")->bands[0].channel == BandChannel::Side,
               "air-lift's shelf really is side-routed");
        check (findEqPreset ("de-harsh")->bands[0].dynamic,      "de-harsh is dynamic");
        check (findEqPreset ("smiley-face")   == nullptr,        "unknown name is a miss");
        check (findEqPreset (nullptr)         == nullptr,        "nullptr is a miss");
        check (findEqPreset ("")              == nullptr,        "empty is a miss");
    }

    std::printf (g_fail == 0 ? "\nALL PASS (0 failures)\n"
                             : "\n%d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
