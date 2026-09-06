/*  V9 (merge survey, 6 Sep 2026): saved device values SURVIVE a reload with
    DO NOT DIAL on. Kathy's find: the guard in EedDeviceProcessor::applyParams
    (aa455ff) also sits on the restore path, and setStateInformation calls
    resetParamsToDefaults() on the line before applyParams - so with the
    setting ON a reload writes defaults and restores nothing: DATA LOSS.
    Positive control = the same round-trip with the setting OFF, which must
    pass; the ON leg is EXPECTED TO FAIL on pre-fix code and to pass once her
    fix (a required ParamSource making the guard inert on restore) lands.
    Values chosen ON the dial curve (round 51 snaps off-curve states). */
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "EedPitchProcessor.h"
#include "EJDialWrites.h"
#include <cstdio>

static int roundTrip (bool blockedDuringLoad, const char* label)
{
    EedPitchProcessor a;
    a.setParamValue (EedPitchProcessor::kRetune,     200.0);   // dial 200 -> 150 ms / depth 15 on the curve
    a.setParamValue (EedPitchProcessor::kNaturalVib,  50.0);
    a.setParamValue (EedPitchProcessor::kIgnoreVib,    0.0);
    a.setParamValue (EedPitchProcessor::kFlex,        30.0);
    juce::MemoryBlock st; a.getStateInformation (st);

    echojay::setDialWritesBlocked (blockedDuringLoad);
    EedPitchProcessor b; b.setStateInformation (st.getData(), (int) st.getSize());
    echojay::setDialWritesBlocked (false);

    struct Row { const char* id; double want; };
    const Row rows[] = { { EedPitchProcessor::kRetune, 200.0 }, { EedPitchProcessor::kRetuneMs, 150.0 },
                         { EedPitchProcessor::kNaturalVib, 50.0 }, { EedPitchProcessor::kIgnoreVib, 0.0 },
                         { EedPitchProcessor::kFlex, 30.0 } };
    int bad = 0;
    std::printf ("%s (dialWritesBlocked=%s during setStateInformation)\n", label, blockedDuringLoad ? "TRUE" : "false");
    for (auto& r : rows)
    {
        const double got = b.getParamValue (r.id);
        const bool ok = std::abs (got - r.want) < 0.5;
        bad += ! ok;
        std::printf ("  %-28s saved %7.2f  reloaded %7.2f  %s\n", r.id, r.want, got, ok ? "ok" : "LOST (default written)");
    }
    return bad;
}

int main()
{
    echojay::requireIsolationOrDie ("v9_dialwrites_restore_test.cpp");
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const int ctl = roundTrip (false, "POSITIVE CONTROL: setting OFF");
    const int on  = roundTrip (true,  "V9: setting ON");
    std::printf ("control: %s   V9: %s\n", ctl == 0 ? "PASS" : "FAIL (harness invalid)", on == 0 ? "PASS" : "FAIL - DATA LOSS on reload with DO NOT DIAL on");
    return (ctl != 0) ? 2 : (on != 0 ? 1 : 0);
}
