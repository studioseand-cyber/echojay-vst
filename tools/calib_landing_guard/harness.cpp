// calib_landing_guard - 12 Oct 2026, Sean's item 5: THE LOOP MUST NOT WALK AWAY AND LEAVE IT PAST THE TARGET.
//
// THE DEFECT, from the host. On the V2's own rack (a commercial hip-hop master) a compressor landed at
// -17.8 dB of gain reduction when about -6 was right. The band test itself is correct; the fault is in the
// ENDINGS. Every "Clamped" exit - the step budget spent, the control at the end of its range, the input out of
// headroom - called endHere() and left the control exactly where the last probe put it. If that probe overshot,
// the overshoot is what the user is left listening to, and the closing line reported that the band had not been
// reached without doing anything about it.
//
// THE RULE, as Sean stated it: land within about 2 dB of the stated target, never leave a compressor far past
// it, and report the measured figure.
//
// WHY THIS LEG IS ON THE PURE LOOP. CalibLoop::onWindow is a pure function of (state, window): it returns the
// writes it wants rather than performing them, so the whole rule can be driven with fabricated windows and
// asserted on the Step that comes back - no audio, no host, no plugin. That is also what makes the RED
// direction cheap, and the red direction is the point: a revert that cannot be observed to NOT happen is not a
// test. Every assertion below names what it would have looked like before the fix.
#include <JuceHeader.h>
// EJCalibLoop logs through the C hook that NativeClip.mm defines and every harness links; declared before the
// include so this translation unit does not need the whole plugin header just for one extern.
extern "C" void EchoJay_NSLog (const char* msg);
#include "EJCalibLoop.h"
#include <cstdio>
#include <cmath>

namespace {
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) std::printf ("  ok    %s%s\n", what.toRawUTF8(),
                         detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    else  { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                         detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
}

using Loop = echojay::CalibLoop;

// A window the loop will treat as a real sample: measured, not silent, with the GR we want it to see.
//
// THE READINGS MUST MOVE, and this leg's first cut got it wrong in a way worth recording. It fed BYTE-IDENTICAL
// windows, and the loop correctly refuses to treat a repeated reading as a fresh sample - that stale-run rule
// exists because of a session where MDynamicsMBLarge produced 400+ windows of identical figures. So the run
// ended through endBecauseStuck rather than through the Clamped exit the leg was aiming at, and the leg was
// measuring the stale detector instead of the landing rule. Real audio never repeats to the bit, so neither
// does this: a small deterministic wobble, far smaller than the 2 dB tolerance under test, keeps every window a
// genuine sample without moving the thing being asserted.
//
// (It also found a real gap doing it: the stuck ending had no revert either, and now does.)
int wobbleN = 0;
Loop::Window sample (float grDb)
{
    Loop::Window w;
    w.measured = true;
    w.silent   = false;
    w.grDb     = grDb + 0.01f * (float) (++wobbleN % 7);   // <= 0.06 dB: never crosses a band edge or a tolerance
    return w;
}

// A LISTEN-mode drive run with a 5-7 dB aim, which is the shape of Sean's case (about -6 dB wanted).
Loop::Config listenDriveCfg()
{
    Loop::Config c;
    c.purpose  = Loop::Purpose::askRung;
    c.plugin   = "Guard Compressor";
    c.slot     = 0;
    c.lo       = 5.0f;
    c.hi       = 7.0f;
    c.mode     = Loop::Mode::Listen;
    c.actuator = Loop::Actuator::Drive;
    c.startDb  = 0.0f;
    return c;
}
}

// FEED UNTIL SOMETHING HAPPENS, WITH A BOUND - never a fixed number of windows.
//
// The leg's first cut asserted on window 2 and window 13, and both were wrong, because a write is followed by
// TWO one-shot gates before anything is judged: awaitFresh holds exactly one window, and freshWanted holds three
// more ("the figure the ask quotes must describe the setting the ask is about"). So four windows after every
// write are seen, logged and deliberately not judged. A leg that counts windows is measuring those gates.
//
// This drives the loop to a STATE and reports how long it took, which is the same discipline the loop itself
// follows. The bound is generous and its exhaustion is a failure, not a pass.
struct Fed
{
    Loop::Step last;
    int   windows = 0;
    bool  finished = false;
    // The last drive the loop WROTE before the ending. The revert is judged against this rather than against a
    // number the leg guessed: the first cut asserted the revert would land on the OPENING drive, which was
    // wrong because 4.0 dB is itself outside the aim, so the loop had already stepped away from the opener by
    // the time it measured its best window. Asserting an arithmetic result means re-deriving the loop's own
    // stepping in the leg; asserting the PROPERTY does not.
    float lastWritten = std::numeric_limits<float>::quiet_NaN();
    bool  wroteAny = false;
};
Fed feedUntilFinished (Loop& loop, float grDb, int bound = 200)
{
    Fed f;
    for (; f.windows < bound; ++f.windows)
    {
        f.last = loop.onWindow (sample (grDb), 3000.0);
        if (f.last.finished) { f.finished = true; ++f.windows; break; }
        if (f.last.writeDrive) { f.lastWritten = f.last.newPre; f.wroteAny = true; }
    }
    return f;
}

int main()
{
    std::printf ("calib_landing_guard: an ending that missed the band puts the control back\n");

    // =========================================================================================================
    //  (1) THE OVERSHOOT: the run ends far past the aim -> the control goes back to the best it MEASURED
    // =========================================================================================================
    std::printf ("== (1) the run ends past the aim ==\n");
    {
        Loop loop;
        loop.begin (listenDriveCfg());
        check (loop.active(), "fixture: a LISTEN drive run is open with a 5-7 dB aim");

        // First, windows just UNDER the aim, long enough that at least one is judged: 4.0 dB is 1.0 dB outside,
        // and the opening drive (0 dB) is the control that produced it. This is the best the run will ever see.
        const auto near = feedUntilFinished (loop, 4.0f, 12);
        check (! near.finished,
               "fixture: 4.0 dB windows (1 dB under the aim) are judged without ending the run",
               juce::String (near.windows) + " window(s) fed");

        // Then a real overshoot, until the loop gives up.
        const auto over = feedUntilFinished (loop, 17.8f);
        check (over.finished, "the run ends rather than stepping for ever",
               juce::String (over.windows) + " further window(s)");

        // THE ASSERTION THIS GUARD EXISTS FOR. Before the fix this Step carried NO write: the loop stopped and
        // left the control wherever the last probe had put it, which is how a compressor ended up at -17.8 dB
        // GR on Sean's master when about -6 was wanted.
        check (over.last.writeDrive || over.last.writeParams,
               "the ending CARRIES A WRITE - the control is put back rather than abandoned  (RED as it stood: "
               "every ending returned with no write and left it at the last probe)",
               over.last.writeDrive ? ("drive -> " + juce::String (over.last.newPre, 2) + " dB")
                                    : (over.last.writeParams ? ("param -> " + juce::String (over.last.paramValue, 2))
                                                             : juce::String ("NO WRITE")));

        // ...and it goes SOMEWHERE ELSE than the probe it gave up on. That is the whole behaviour under test:
        // the old code left the control at the last probe, so "the write moves it off the last probe" is the
        // property that separates fixed from broken, and it needs no re-derivation of the loop's stepping.
        if (over.last.writeDrive && over.wroteAny)
            check (std::abs (over.last.newPre - over.lastWritten) > 0.001f,
                   "...and back to a DIFFERENT setting than the one it gave up on - not left at the last probe",
                   "reverted to " + juce::String (over.last.newPre, 2) + " dB, gave up at "
                       + juce::String (over.lastWritten, 2) + " dB");
        // ...and the setting it chose is one whose MEASURED reduction was closer to the aim than the overshoot.
        // Read out of the closing line, which is the sentence the user actually gets.
        check (over.last.closing.contains ("closest setting I actually measured"),
               "...and the sentence names it as the closest setting it MEASURED, not a computed guess",
               over.last.closing.fromFirstOccurrenceOf ("closest setting", true, false));

        // ...and it SAYS so, with the figure. Sean: "report the measured figure."
        check (over.last.closing.contains ("17.8") || over.last.closing.contains ("17."),
               "...and the closing line names the figure it measured", over.last.closing);
        check (over.last.closing.containsIgnoreCase ("put it back")
                   || over.last.closing.containsIgnoreCase ("closest setting"),
               "...and says that it put it back, in words a user can act on", over.last.closing);
    }

    // =========================================================================================================
    //  (2) THE CONTROL CASE: an ending just outside the band is LEFT ALONE
    // =========================================================================================================
    // The half that stops the fix becoming its own bug. A loop that reverted on every miss would throw away a
    // good landing 1 dB outside a 2 dB tolerance, and would undo its own last useful step.
    std::printf ("== (2) an ending just outside the band is left where it is ==\n");
    {
        Loop loop;
        loop.begin (listenDriveCfg());
        const auto f = feedUntilFinished (loop, 8.0f);   // 1.0 dB over a 5-7 aim: inside tolerance
        check (f.finished, "fixture: the run ends", juce::String (f.windows) + " window(s)");
        check (! f.last.writeDrive && ! f.last.writeParams,
               "a 1 dB miss is inside the tolerance, so the ending writes NOTHING and the landing stands",
               f.last.writeDrive ? ("drive -> " + juce::String (f.last.newPre, 2)) : juce::String ("no write"));
    }

    // =========================================================================================================
    //  (3) REACHING THE BAND still ends the way it always did
    // =========================================================================================================
    std::printf ("== (3) reaching the band is unchanged ==\n");
    {
        Loop loop;
        loop.begin (listenDriveCfg());
        const auto f = feedUntilFinished (loop, 6.0f);   // squarely in the 5-7 band
        check (f.finished, "in-band windows end the run", juce::String (f.windows) + " window(s)");
        check (! f.last.writeDrive && ! f.last.writeParams,
               "...and a run that REACHED the band never reverts - there is nothing to put back",
               f.last.writeDrive ? juce::String ("UNEXPECTED WRITE") : juce::String ("no write"));
    }

    std::printf (failures == 0 ? "==== calib_landing_guard: GREEN (0 assertion(s) failed) ====\n"
                               : "==== calib_landing_guard: RED (%d assertion(s) failed) ====\n", failures);
    return failures == 0 ? 0 : 1;
}
