# Matrix report - 2026-10-17 (mock pass)

Build under test: **build-guards**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-17/<scenario>/`.

**1 passed, 0 failed, 7 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **P1** an AU that cannot load in this process is never offered in the inventory | SKIP | - | every one of the 674 AU component(s) on this Mac has an arm64 slice, so there is nothing here that cannot load and nothing to withhold. NEVER counted as a pass. | - |
| **T3** T3: a Link held by another instance refuses the edit, in words | SKIP | - | this window never saw another instance holding the rack (rackLock="idle" after 12 s), so there was no contention to refuse. Never counted as a pass. | - |
| **I4c** a rack's refusal reads as words, never "op 1 invalid" | SKIP | - | the run produced no failed line at all in this host (the op was refused before it reached a rack's validator), so there is no wording to judge. A10 and B5 hold the same rule on the line Sean actually read. NEVER counted as a pass. | - |
| **RP8K** REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory (357 unloadable) | pass | - | with 336 of his unloadable plugins seeded, the inventory result the plugin POSTED was 568 bytes - inside the contract's 8 KB; keys [count,hasMore,limit,names,notLoadableTotal,offset,returned,total]; largest result in that body 568 bytes (toolu_rp8k_look) | `docs/review/2026-10-17/RP8K/editor.png` |
| **T5** a third party in the Link's mirror says where it is, never "Failed" | SKIP | - | no third-party slot is drawn in the mirror this run (the strip reads []), so there is nothing to open. | - |
| **A2** a built-in add with settings on a Link, and the main rack follows it | SKIP | - | the Link this row addressed (de6bf11e6f) was not usable by the end of it - a registry row whose publisher has gone still reads as connected and still carries its last slot count (queue item 14), so an add that reached nothing cannot be told from one that was refused. NEVER counted as a pass. | - |
| **G2** the rack strip follows a Link change (the build road) | SKIP | - | same Link as A2: not usable by the end of the row (item 14) | - |
| **A11b** Auto on ANOTHER track: the approved line names that track | SKIP | - | the Link the row is working on has no published track name in this run, so there is no name for the line to carry and nothing to judge. The row is about the NAMING, and it cannot be about a name that does not exist. | - |

## Failures by owner

**A**: nothing

**B**: nothing

**Sean**: nothing
