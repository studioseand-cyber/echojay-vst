# Matrix report - 2026-10-26 (mock pass)

Build under test: **build-quick**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-26/<scenario>/`.

**0 passed, 0 failed, 10 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **P1** an AU that cannot load in this process is never offered in the inventory | SKIP | - | every one of the 674 AU component(s) on this Mac has an arm64 slice, so there is nothing here that cannot load and nothing to withhold. NEVER counted as a pass. | - |
| **T3** T3: a Link held by another instance refuses the edit, in words | SKIP | - | this window never saw another instance holding the rack (rackLock="idle" after 12 s), so there was no contention to refuse. Never counted as a pass. | - |
| **T2w** T2: an edit lands on a Link whose OWN window is open | SKIP | - | the Link that AU hosting loaded is the INSTALLED binary, not the one this run was given (item 0b) - an edit between a V2 from this build and a Link from another is a pairing that never ships, so neither a landing nor a refusal would be a verdict on anything. NEVER counted as a pass. | - |
| **I4c** a rack's refusal reads as words, never "op 1 invalid" | SKIP | - | the run produced no failed line at all in this host (the op was refused before it reached a rack's validator), so there is no wording to judge. A10 and B5 hold the same rule on the line Sean actually read. NEVER counted as a pass. | - |
| **RP8K** REPLAY rp_8k: look(inventory) fits 8 KB on HIS inventory | SKIP | - | this process was not started with EJ_RIG_BIG_INVENTORY=1, so the catalogue holds this rig's handful of plugins rather than his 1,013. A small result about one plugin cannot test the size of an answer about a thousand - the row's first run passed on exactly that, 568 bytes from half an input. Run it as its own pass: EJ_RIG_BIG_INVENTORY=1 EJ_RIG_ONLY=RP8K. | - |
| **T5** a third party in the Link's mirror says where it is, never "Failed" | SKIP | - | no third-party slot is drawn in the mirror this run (the strip reads []), so there is nothing to open. | - |
| **I9c** the Link's mirrored card carries the plugin's figure too | SKIP | - | the Link in this run is NOT the Link this build produced: its card reads "", which is the previous build's wording, beside a ledger only this build writes (). The rig registers an AU by type/subtype and instantiates the IDENTIFIER, so the INSTALLED component wins in a fresh process - queued as item 0b, fix: host the Link by PATH. I9b proves the card on the own rack; the Logic smoke test covers the Link. NEVER coun... | - |
| **A2** a built-in add with settings on a Link, and the main rack follows it | SKIP | - | the Link that AU hosting loaded is the INSTALLED binary, not the one this run was given (item 0b): this row asserts that an add with settings lands on a Link AND that the V2's strip follows it, and in a mixed pairing those two halves come from different builds. NEVER counted as a pass. | - |
| **G2** the rack strip follows a Link change (the build road) | SKIP | - | same Link as A2: a mixed pairing (item 0b) | - |
| **A11b** Auto on ANOTHER track: the approved line names that track | SKIP | - | the Link the row is working on has no published track name in this run, so there is no name for the line to carry and nothing to judge. The row is about the NAMING, and it cannot be about a name that does not exist. | - |

## Failures by owner

**A**: nothing

**B**: nothing

**Sean**: nothing
