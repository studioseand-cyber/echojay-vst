// Part-A readback instrument for the EQ params-accounting fix (15 Sep 2026).
// It does NOT trust the applier's self-count. It drives the REAL device apply
// path - SurgicalEqProcessor::applyStructured, the exact method ChainHost's
// builtin dial calls - and then READS BACK the actual parameter state via the
// device getters. An applier's opinion of its own success is not evidence.
//
//   CASE 1  EQ 4-value: {params:{phase_mode,auto_gain}, eq_bands:[2]} ->
//           applied == 4 (RED on the pre-fix bands-only count == 2), and readback
//           confirms phase_mode, auto_gain AND both bands actually landed.
//   CASE 2  mis-typed phase_mode: a valid Linear is set, then {params:{phase_mode:
//           "banana"}} is applied. Readback must show phase_mode STILL Linear
//           (unmoved, NOT coerced to 0.0 and written) AND the summary names it
//           skipped with a reason. A 0.0 write would be a silent wrong write = RED.
#include <JuceHeader.h>
#include "SurgicalEqProcessor.h"
#include <cstdio>
using namespace juce;

static int g_fails = 0;
static void check(const char* label, bool ok)
{
    std::fprintf(stderr, "   %-62s %s\n", label, ok ? "PASS" : "FAIL  <-- RED");
    if (!ok) ++g_fails;
}

static var parse(const char* json)
{
    var v; JSON::parse(String::fromUTF8(json), v); return v;
}

int main()
{
    ScopedJuceInitialiser_GUI gui;
    using PM = SurgicalEqProcessor::PhaseMode;

    // ---- CASE 1: EQ 4-value, counted + readback-verified ----
    std::fprintf(stderr, "CASE 1  EQ 4-value (params{phase_mode,auto_gain} + eq_bands[2]):\n");
    SurgicalEqProcessor eq;
    int applied = 0, skipped = 0;
    const var payload = parse(
        R"({"params":{"phase_mode":1,"auto_gain":1},)"
        R"("eq_bands":[{"band":1,"type":"bell","freq_hz":250,"gain_db":-3,"q":1.0},)"
        R"({"band":2,"type":"bell","freq_hz":5000,"gain_db":2,"q":1.5}]})");
    const String summary = eq.applyStructured(payload, EedDeviceProcessor::ParamSource::Assistant, &applied, &skipped);
    std::fprintf(stderr, "   applier says: applied=%d skipped=%d  summary=\"%s\"\n",
                 applied, skipped, summary.toRawUTF8());

    // The COUNT: all four counted (pre-fix counted bands only -> 2 -> RED).
    check("applied count == 4 (params + bands, not bands only)", applied == 4);

    // READBACK: the actual device state, not the applier's word.
    const bool pmOk = (eq.getPhaseMode() == PM::Linear);
    const bool agOk = (eq.getAutoGain() == true);
    check("readback: phase_mode == Linear (param actually moved)", pmOk);
    check("readback: auto_gain == on (param actually moved)",      agOk);

    const var bands = eq.currentEqBandsVar();
    int b1 = -1, b2 = -1;
    if (auto* arr = bands.getArray())
        for (auto& b : *arr)
        {
            const int idx = (int) b.getProperty("band", -1);
            const double f = (double) b.getProperty("freq_hz", 0.0);
            if (idx == 1) b1 = (int) juce::roundToInt(f);
            if (idx == 2) b2 = (int) juce::roundToInt(f);
        }
    check("readback: band 1 freq == 250 Hz (band actually moved)",  b1 == 250);
    check("readback: band 2 freq == 5000 Hz (band actually moved)", b2 == 5000);

    // ---- CASE 2: mis-typed phase_mode must be reported, never written wrong ----
    std::fprintf(stderr, "\nCASE 2  mis-typed phase_mode (\"banana\"):\n");
    SurgicalEqProcessor eq2;
    int a2 = 0, s2 = 0;
    eq2.applyStructured(parse(R"({"params":{"phase_mode":1}})"),
                        EedDeviceProcessor::ParamSource::Assistant, &a2, &s2);
    const bool preLinear = (eq2.getPhaseMode() == PM::Linear);   // known-good starting state
    check("precondition: phase_mode set to Linear first", preLinear);

    int a3 = 0, s3 = 0;
    const String misSummary = eq2.applyStructured(parse(R"({"params":{"phase_mode":"banana"}})"),
                                                  EedDeviceProcessor::ParamSource::Assistant, &a3, &s3);
    std::fprintf(stderr, "   mis-typed apply: applied=%d skipped=%d  summary=\"%s\"\n",
                 a3, s3, misSummary.toRawUTF8());
    // The wrong-write test: if "banana" were coerced to 0.0 and written, phase_mode
    // would flip to Zero. It must stay Linear (unmoved).
    check("readback: phase_mode STILL Linear (no silent wrong write)", eq2.getPhaseMode() == PM::Linear);
    check("skipped >= 1 (the mis-typed value was counted as skipped)", s3 >= 1);
    check("summary names it skipped with a reason",
          misSummary.containsIgnoreCase("banana") || misSummary.containsIgnoreCase("ignored")
          || misSummary.containsIgnoreCase("not a number"));

    std::fprintf(stderr, "\n==== EQ READBACK INSTRUMENT: %s (%d assertion(s) failed) ====\n",
                 g_fails == 0 ? "GREEN" : "RED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
