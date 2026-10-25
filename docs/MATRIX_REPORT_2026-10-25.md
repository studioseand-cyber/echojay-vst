# Matrix report - 2026-10-25 (mock pass)

Build under test: **build-quick**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-25/<scenario>/`.

**1 passed, 0 failed, 7 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **P1** an AU that cannot load in this process is never offered in the inventory | SKIP | - | every one of the 674 AU component(s) on this Mac has an arm64 slice, so there is nothing here that cannot load and nothing to withhold. NEVER counted as a pass. | - |
| **B6** a new instruction typed while a run waits on APPLY stops it and starts its own | pass | - | the run was awaiting_approval when the text was typed; rows 1 -> 2, session "ag_rig_179" -> "", status now "Working"  (RED on 16b: "The agent is still working on ... Stop it first, or wait for it to finish" - #21 was wired to the ask road only) | `docs/review/2026-10-25/B6/editor.png` |
| **T3** T3: a Link held by another instance refuses the edit, in words | SKIP | - | this window never saw another instance holding the rack (rackLock="idle" after 12 s), so there was no contention to refuse. Never counted as a pass. | - |
| **T2w** T2: an edit lands on a Link whose OWN window is open | SKIP | - | the Link's own process would not open an editor this run (""), so there is no open window for an edit to land under. Never counted as a pass. | - |
| **I4c** a rack's refusal reads as words, never "op 1 invalid" | SKIP | - | the run produced no failed line at all in this host (the op was refused before it reached a rack's validator), so there is no wording to judge. A10 and B5 hold the same rule on the line Sean actually read. NEVER counted as a pass. | - |
| **RP8K** REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory | SKIP | - | this process was not started with EJ_RIG_BIG_INVENTORY=1, so the catalogue holds this rig's handful of plugins rather than his 1,013. A small result about one plugin cannot test the size of an answer about a thousand - the row's first run passed on exactly that, 568 bytes from half an input. Run it as its own pass: EJ_RIG_BIG_INVENTORY=1 EJ_RIG_ONLY=RP8K. | - |
| **T5** a third party in the Link's mirror says where it is, never "Failed" | SKIP | - | no third-party slot is drawn in the mirror this run (the strip reads []), so there is nothing to open. | - |
| **A11b** Auto on ANOTHER track: the approved line names that track | SKIP | - | the Link the row is working on has no published track name in this run, so there is no name for the line to carry and nothing to judge. The row is about the NAMING, and it cannot be about a name that does not exist. | - |

## Failures by owner

**A**: nothing

**B**: nothing

**Sean**: nothing
