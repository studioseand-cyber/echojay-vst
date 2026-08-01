/*
  typedReadbackMatch self-test, born from a live-telemetry defect: AMEK EQ 200
  logged partial, manual ["q"] on every dial while freq and gain applied.

  The Q control is a hardware-style stepped ladder and the display snaps to a
  step. The tolerance is half the bracketing anchor gap - "the map's own
  resolution" - and the common request q=0.7 sits at the EXACT midpoint of
  the 0.6/0.8 bracket, where the verdict is decided by float representation:
  |0.8 - 0.7| in float32 beats 0.5*gap by ~3e-8, so a correct best-effort
  write was reverted, every time, deterministically.

  The fixture is the REAL cached map's LF Q 1 anchor table (rev b3f287f9537d),
  not a synthetic ladder, so this test fails the same way the fleet did. The
  header under test is header-only inline code: including it IS the shipped
  implementation, no reimplementation and no lib-vs-header drift possible for
  these functions. The SharedCode link is for the JUCE symbols only.
*/

#include <JuceHeader.h>
#include "EchoJayParamApply.h"

static int passN = 0, failN = 0;
static void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok) { ++passN; std::cout << "  ok    " << name << "\n"; }
    else    { ++failN; std::cout << "  FAIL  " << name
                                 << (detail.isNotEmpty() ? ("\n        " + detail) : juce::String()) << "\n"; }
}

// LF Q 1, index 34, map rev b3f287f9537d - copied verbatim from the cache.
static juce::Array<juce::Array<float>> amekQTable()
{
    const float pairs[][2] = {
        { 0.400000006f, 0.100000001f }, { 0.600000024f, 0.150000006f },
        { 0.800000012f, 0.200000003f }, { 0.899999976f, 0.25f },
        { 1.10000002f,  0.300000012f }, { 1.29999995f,  0.350000024f },
        { 1.5f,         0.400000006f }, { 1.60000002f,  0.449999988f },
        { 1.79999995f,  0.5f },         { 2.0f,         0.550000012f },
        { 2.20000005f,  0.600000024f }, { 2.4000001f,   0.649999976f },
        { 2.70000005f,  0.699999988f }, { 2.9000001f,   0.75f },
        { 3.0999999f,   0.800000012f }, { 3.29999995f,  0.850000024f },
        { 3.5999999f,   0.899999976f }, { 3.79999995f,  0.949999988f },
        { 4.0f,         1.0f },
    };
    juce::Array<juce::Array<float>> t;
    for (auto& p : pairs) { juce::Array<float> a; a.add (p[0]); a.add (p[1]); t.add (a); }
    return t;
}

int main()
{
    std::cout << "typedReadbackMatch nearest-step self-test\n";
    const auto q = amekQTable();

    // THE REPRO. Asked 0.7, ladder snapped up to "0.8". Before the fix this
    // returned -1 by ~3e-8 and applyOne reverted a correct write.
    check (echojay::typedReadbackMatch ("q", 0.7f, "0.8", q) == +1,
           "q=0.7 landing on \"0.8\" (upper bracket step) is a match");
    check (echojay::typedReadbackMatch ("q", 0.7f, "0.6", q) == +1,
           "q=0.7 landing on \"0.6\" (lower bracket step) is a match");

    // The other common midpoint, both snap directions.
    check (echojay::typedReadbackMatch ("q", 1.0f, "0.9", q) == +1,
           "q=1.0 landing on \"0.9\" is a match");
    check (echojay::typedReadbackMatch ("q", 1.0f, "1.1", q) == +1,
           "q=1.0 landing on \"1.1\" is a match");

    // Off-midpoint interior value inside the old tolerance: unchanged.
    check (echojay::typedReadbackMatch ("q", 0.7f, "0.75", q) == +1,
           "q=0.7 landing \"0.75\" (inside half-gap) still matches");

    // The wrong-write class stays caught: landing on a NON-bracket step, or
    // nowhere near, is still a mismatch. Nearest-step must not become
    // any-step.
    check (echojay::typedReadbackMatch ("q", 0.7f, "1.5", q) == -1,
           "q=0.7 landing \"1.5\" (non-bracket step) is refused");
    check (echojay::typedReadbackMatch ("q", 0.7f, "4", q) == -1,
           "q=0.7 landing \"4\" (far end) is refused");

    // Unparseable display stays 0: cannot verify either way.
    check (echojay::typedReadbackMatch ("q", 0.7f, "wide", q) == 0,
           "unparseable display returns 0 (unverifiable)");

    // Continuous-display guard on a coarse table: a landing one full step
    // past the FAR bracket stays refused; a landing ON the far bracket is
    // now accepted as within the map's resolution (the spec'd trade).
    juce::Array<juce::Array<float>> gain;
    for (auto p : { std::pair<float,float>{ -24.0f, 0.0f }, { -21.0f, 0.1f },
                    { -18.0f, 0.2f }, { -15.0f, 0.3f }, { -12.0f, 0.4f } })
    { juce::Array<float> a; a.add (p.first); a.add (p.second); gain.add (a); }
    check (echojay::typedReadbackMatch ("gain_db", -17.0f, "-14.0", gain) == -1,
           "gain -17 landing \"-14\" (beyond the bracket) is refused");
    check (echojay::typedReadbackMatch ("gain_db", -17.0f, "-15.0", gain) == +1,
           "gain -17 landing \"-15\" (far bracket step) is accepted");

    // The map entry itself is healthy - documents that the fleet failure was
    // never about the data. Mirrors the usableParamEntry rules.
    {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("index", 34);
        e->setProperty ("kind", "anchored");
        juce::Array<juce::var> av;
        for (auto& a : q) { juce::Array<juce::var> p; p.add (a[0]); p.add (a[1]); av.add (juce::var (p)); }
        e->setProperty ("anchors", juce::var (av));
        check (echojay::usableParamEntry (juce::var (e.get())),
               "the real AMEK q entry passes usableParamEntry");
    }

    std::cout << passN << " passed, " << failN << " failed\n";
    return failN == 0 ? 0 : 1;
}
