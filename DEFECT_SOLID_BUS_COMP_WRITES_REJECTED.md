# DEFECT: Solid Bus Comp - map present, dial attempted, every write rejected (filed 7 Sep 2026, NOT diagnosed)
Ruled by the reviewer: its own record, not chased tonight.
## The observation (results_2026-09-06/protools_dial_log_20-57_build.log, Pro Tools pid 83157, x86_64 slice under Rosetta)
    20:57:25.505 EJDial: slot 1 ("Solid Bus Comp") NO MAP for fp=d8e076e41586  fetch_requested=n  in_flight=n  fetch_wired=y  cached_maps=16 -> noMap
    20:57:25.505 EJFallback: asking for slot 2 ("Solid Bus Comp") fp d8e076e41586 -- no exact map
    20:57:28.450 EJFallback: AudioUnit|2520b63|1.4.5 served from AudioUnit|2520b63|1.4.5 (tier exact, anchors_unverified=0)
    20:57:28.488 EJDialable: slot 1 ("Solid Bus Comp") fp=d8e076e41586 dialable=ABSENT category=compressor fresh=y
    20:57:30.785 EJDialSummary: slot 1 ("Solid Bus Comp") settings_structured=y shape=mixed keys=[controls{Ratio, Attack}, threshold_db]
                 requested=3  applied=0  manual=3  readbackMiss=1  status=writesRejected  fp=d8e076e41586  map=y
The map arrived (fallback, tier exact, 2,983 ms), the dial ran, three semantics were requested, none stuck, one
readback mismatched. status=writesRejected is "the map covered it, writes were attempted, none stuck - host or
plugin-side" (ChainHost.h). The bubble then said "Solid Bus Comp needs hand-dialing - use the values on its card."
## Open questions, in order
1. THE ROSETTA QUESTION. Pro Tools loads the x86_64 slice; the map was served for identity AudioUnit|2520b63|1.4.5,
   i.e. authored against the AU (arm64, Logic). Is the AAX-hosted Solid Bus Comp's parameter set the same (count,
   names, ranges) as the AU's? The fallback entry sends param_count and param_names - compare the AAX instance's
   list against the map's anchors. A map that matches by identity but not by layout would produce exactly
   "written, read back different".
2. "dialable=ABSENT": the map carries no dialable flag. What does applyStructuredSettings do with an absent flag?
3. readbackMiss=1 of 3: which control, what was written, what came back. The EJDialMissRows emitter has the rows.
4. Does the same plugin dial in Logic (AU) on this Mac with the same map? If yes, it is 1.
## Not to be confused with
DEFECT_SILENT_CHAIN_SUBSTITUTION.md defect 2 (server omitted settings for two other plugins) - different mechanism.
