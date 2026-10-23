/*
  probe_extract.h - "--extract" (Kathy, 22 Oct, decision 0: the wizard saves FULL maps). The parameter list exactly as "--list-params"
  prints it, then every parameter's display sweep, in the SAME instantiation:

    the shared corpus extractor (tools/ejextract/EchoJayParamExtractor.h - sweepContinuous: 11 points + adaptive subdivision where
    the curve bends; sweepDiscrete: every step), read-only through getText; a sweep that reads flat gets the extractor's own
    set-then-read retry, bracketed by a full state save / restore so the instance leaves as it arrived. Nothing is derived here:
    EJ Map turns the points into controls with the same code the EJ Map app uses (EjmapSweeper.h outcomeFromPoints).

  OUTPUT, after the "--list-params" rows (tab separated):
    xparam  <i>  method <gettext|setread|flat|flat_unretried>  points <n>  def <norm>  deftext <text>
    xpt     <i>  <norm>  <text>                                  one per sweep point, in norm order
    extract done  params <n>  setread <n>  ms <n>
  THE BUDGET: set-then-read writes the plugin and settles on the run loop; past kSetReadBudgetMs of them in one run the rest are not
  retried ("flat_unretried"), so one plugin with hundreds of flat read-outs cannot hold the scan.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <CoreFoundation/CoreFoundation.h>
#include "../ejextract/EchoJayParamExtractor.h"

namespace ejprobe
{

inline constexpr int kSetReadBudgetMs = 20000;
// THE MAP STEP'S BOUND, SCALED BY PARAMETER COUNT (Kathy, 23 Oct): a big plugin's sweep (StudioRack, CLA MixHub, Scheps Omni Channel)
// outran the caller's flat per-process bound with other work running and needed a second try alone. Once the plugin has loaded and its
// list is known, the extract DECLARES its own bound ("GUARD<TAB>seconds", the hang guard every caller already honours, direct and served):
// the base, the set-then-read budget, and kExtractGuardPerParamS for every parameter. A load that hangs never reaches the declaration
inline constexpr double kExtractGuardBaseS = 120.0, kExtractGuardPerParamS = 1.0;
inline double extractGuardS (int params) { return kExtractGuardBaseS + kSetReadBudgetMs / 1000.0 + kExtractGuardPerParamS * (params > 0 ? params : 0); }

inline void runExtract (juce::AudioPluginInstance& inst)
{
    const auto clean = [] (juce::String t) { return t.replace ("\t", " ").replace ("\n", " ").replace ("\r", " "); };
    echojay::ExtractorConfig cfg;
    cfg.settleMs = 15;   // EJ Map's own settle (a bridged AU serves the stale text without it)
    cfg.settle = [] { const double t0 = juce::Time::getMillisecondCounterHiRes();
                      while (juce::Time::getMillisecondCounterHiRes() - t0 < 15.0) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } };
    const auto& ps = inst.getParameters();
    std::printf ("GUARD\t%.0f\n", extractGuardS (ps.size())); std::fflush (stdout);   // 23 Oct: read by the caller while the sweep runs
    const double start = juce::Time::getMillisecondCounterHiRes(); double setReadMs = 0.0; int setReads = 0;
    for (int i = 0; i < ps.size(); ++i)
    {
        auto* p = ps[i]; if (p == nullptr) continue;
        const float before = p->getValue();
        auto points = p->isDiscrete() ? echojay::sweepDiscrete (*p, cfg) : echojay::sweepContinuous (*p, cfg);
        juce::String method = "gettext";
        if (echojay::sweepIsFlat (points))
        {
            if (setReadMs >= kSetReadBudgetMs) method = "flat_unretried";
            else
            {
                const double t1 = juce::Time::getMillisecondCounterHiRes();
                juce::MemoryBlock state; inst.getStateInformation (state);
                bool verified = false; auto retried = echojay::sweepSetRead (*p, cfg, verified);
                if (state.getSize() > 0) inst.setStateInformation (state.getData(), (int) state.getSize());
                setReadMs += juce::Time::getMillisecondCounterHiRes() - t1; ++setReads;
                if (verified && ! echojay::sweepIsFlat (retried)) { points = retried; method = "setread"; } else method = "flat";
            }
        }
        std::printf ("xparam\t%d\tmethod\t%s\tpoints\t%d\tdef\t%.6f\tdeftext\t%s\n", i, method.toRawUTF8(), points.size(), before,
                     clean (p->getText (before, 256).trim()).toRawUTF8());
        for (const auto& pt : points) std::printf ("xpt\t%d\t%.6f\t%s\n", i, pt.n, clean (pt.t).toRawUTF8());
    }
    std::printf ("extract\tdone\tparams\t%d\tsetread\t%d\tms\t%d\n", ps.size(), setReads, (int) (juce::Time::getMillisecondCounterHiRes() - start));
    std::fflush (stdout);
}

} // namespace ejprobe
