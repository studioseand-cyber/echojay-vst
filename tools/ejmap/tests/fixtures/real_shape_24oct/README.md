Real-shape profiles for the contract scenarios (24 Oct): real results from this Mac's wizard runs, copied unchanged.
- api2500_m.comp_profile.json + api2500_m.tonecheck.json: API-2500 (m), the 20 Oct run (ej_comp_profile/1 and its tone check).
- cleansweep_v2.filter_profile.draft.json: bx_cleansweep V2, the 22 Oct filter measurement (ej_filter_profile/1).
The suite rewrites only their identity (plugin_id, name, vendor, version, map_fp, product) to each scenario's synthetic plugin, and, where
a scenario needs two testers g dB apart, shifts every curve point by g. The data is otherwise the real measurement, so B's real validators
judge real shapes.
