// Standalone test for the EqMove apply/allocation logic (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source eq_move_test.cpp -o movetest && ./movetest

#include "EqMove.h"
#include <cstdio>
#include <string>

using namespace echojay;
static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static BandSpec bell (float f, float g, float q)
{ BandSpec b; b.type = BandType::Bell; b.freqHz = f; b.gainDb = g; b.q = q; return b; }

int main()
{
    std::printf ("== band-type parsing (tolerant) ==\n");
    struct { const char* s; BandType t; } ok[] = {
        {"bell", BandType::Bell}, {"Bell", BandType::Bell}, {"peaking", BandType::Bell},
        {"low_shelf", BandType::LowShelf}, {"Low-Shelf", BandType::LowShelf}, {"LS", BandType::LowShelf},
        {"high shelf", BandType::HighShelf}, {"highpass", BandType::HighPass}, {"HPF", BandType::HighPass},
        {"lpf", BandType::LowPass}, {"notch", BandType::Notch},
    };
    for (auto& c : ok)
    {
        BandType t; const bool r = parseBandType (c.s, t);
        check (r && t == c.t, std::string ("\"") + c.s + "\" -> " + bandTypeToString (c.t));
    }
    { BandType t; check (! parseBandType ("wobble", t), "unknown type rejected"); }

    const int N = EqEngine::kMaxBands;

    std::printf ("\n== auto-allocation fills lowest free bands in order ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        EqMove mv[3];
        mv[0].spec = bell (200, -3, 4);
        mv[1].spec = bell (1000, 2, 1);
        mv[2].spec = bell (6000, -5, 8);
        const int a = applyEqMoves (bands, N, mv, 3);
        check (a == 3, "3 moves applied");
        check (bands[0].enabled && bands[0].freqHz == 200, "band 0 = 200 Hz");
        check (bands[1].enabled && bands[1].freqHz == 1000, "band 1 = 1k");
        check (bands[2].enabled && bands[2].freqHz == 6000, "band 2 = 6k");
        check (! bands[3].enabled, "band 3 still free");
    }

    std::printf ("\n== explicit band index targets exactly, is idempotent ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        EqMove mv; mv.band = 3; mv.spec = bell (203, -3.0f, 4.5f);
        applyEqMoves (bands, N, &mv, 1);
        check (bands[2].enabled && bands[2].freqHz == 203 && bands[2].gainDb == -3.0f && bands[2].q == 4.5f,
               "band 3 (1-based) set exactly");
        check (! bands[0].enabled && ! bands[1].enabled, "other bands untouched");
        // re-apply with a tweak: same slot updates, no new band consumed
        mv.spec.gainDb = -4.0f;
        applyEqMoves (bands, N, &mv, 1);
        check (bands[2].gainDb == -4.0f, "re-apply updates same band");
        check (! bands[3].enabled, "no extra band consumed on re-apply");
    }

    std::printf ("\n== merge does not clobber unrelated bands ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        bands[5] = bell (500, 6, 1); bands[5].enabled = true;   // pre-existing manual band
        EqMove mv; mv.spec = bell (9000, -2, 3);                // auto-allocate
        applyEqMoves (bands, N, &mv, 1);
        check (bands[0].enabled && bands[0].freqHz == 9000, "auto move took band 0");
        check (bands[5].enabled && bands[5].freqHz == 500, "manual band 5 preserved");
    }

    std::printf ("\n== disable turns a band off ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        bands[2] = bell (1000, 4, 2); bands[2].enabled = true;
        EqMove mv; mv.band = 3; mv.disable = true;
        applyEqMoves (bands, N, &mv, 1);
        check (! bands[2].enabled, "band 3 disabled");
    }

    std::printf ("\n== no free band -> skipped, counted ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        for (int i = 0; i < N; ++i) { bands[i] = bell (1000, 0, 1); bands[i].enabled = true; }
        EqMove mv; mv.spec = bell (100, 1, 1);
        int skipped = 0;
        const int a = applyEqMoves (bands, N, &mv, 1, &skipped);
        check (a == 0 && skipped == 1, "full EQ -> move skipped and reported");
    }

    std::printf ("\n== dynamic fields carry through ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        EqMove mv; mv.band = 1; mv.spec = bell (6500, 0, 5);
        mv.spec.dynamic = true; mv.spec.thresholdDb = -20; mv.spec.rangeDb = -6;
        mv.spec.attackMs = 2; mv.spec.releaseMs = 60;
        applyEqMoves (bands, N, &mv, 1);
        check (bands[0].dynamic && bands[0].thresholdDb == -20 && bands[0].rangeDb == -6,
               "dynamic de-ess move applied to band 1");
    }

    std::printf ("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
