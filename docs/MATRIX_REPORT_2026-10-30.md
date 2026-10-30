# Matrix report - 2026-10-30 (mock pass)

Build under test: **build-guards**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-30/<scenario>/`.

**0 passed, 0 failed, 8 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **P1** an AU that cannot load in this process is never offered in the inventory | SKIP | - | every one of the 674 AU component(s) on this Mac has an arm64 slice, so there is nothing here that cannot load and nothing to withhold. NEVER counted as a pass. | - |
| **A4b** T2: add with settings to a Link in its own process | SKIP | - | tools/link_host is not built at /private/var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.szGZECBPUG/link_host (bash tools/link_host/build.sh), so the out-of-process Link scenarios cannot run. They are SKIPPED, never passed. | - |
| **G1** T2: the main rack strip follows a Link's change with no manual switch | SKIP | - | tools/link_host is not built at /private/var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.szGZECBPUG/link_host (bash tools/link_host/build.sh), so the out-of-process Link scenarios cannot run. They are SKIPPED, never passed. | - |
| **E5** T2: no GR figure on a Link's slot in the mirror | SKIP | - | tools/link_host is not built at /private/var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.szGZECBPUG/link_host (bash tools/link_host/build.sh), so the out-of-process Link scenarios cannot run. They are SKIPPED, never passed. | - |
| **T3** T3: a Link selected in another instance refuses the edit, in words | SKIP | - | tools/link_host is not built at /private/var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.szGZECBPUG/link_host (bash tools/link_host/build.sh), so the out-of-process Link scenarios cannot run. They are SKIPPED, never passed. | - |
| **I4c** a rack's refusal reads as words, never "op 1 invalid" | SKIP | - | the run produced no failed line at all in this host (the op was refused before it reached a rack's validator), so there is no wording to judge. A10 and B5 hold the same rule on the line Sean actually read. NEVER counted as a pass. | - |
| **RP8K** REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory | SKIP | - | this process was not started with EJ_RIG_BIG_INVENTORY=1, so the catalogue holds this rig's handful of plugins rather than his 1,013. A small result about one plugin cannot test the size of an answer about a thousand - the row's first run passed on exactly that, 568 bytes from half an input. Run it as its own pass: EJ_RIG_BIG_INVENTORY=1 EJ_RIG_ONLY=RP8K. | - |
| **T5** a third party in the Link's mirror says where it is, never "Failed" | SKIP | - | the panel's status line was about something else when the slot was opened ("No live view of this one here - its settings below are read from the Link"), so there is no note to judge. The rule it does hold - never the word "Failed" - was not broken. NEVER counted as a pass. | - |

## Failures by owner

**A**: nothing

**B**: nothing

**Sean**: nothing
