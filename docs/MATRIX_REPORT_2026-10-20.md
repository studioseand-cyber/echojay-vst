# Matrix report - 2026-10-20 (mock pass)

Build under test: **build-guards**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-20/<scenario>/`.

**3 passed, 0 failed, 10 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **B1** an ask is answerable: chips when there are chips, and a named road when there are not | pass | - | while the question was up: askHint="Type your answer in the box below and press Enter."  zones=[7:Stop]  (RED on 16a: the row showed the question and a faint "your answer?" with nothing to press, and typing was refused with "Tap one of the choices" about choices that did not exist) | `docs/review/2026-10-20/B1/editor.png` |
| **B3** a typed answer lands in the waiting ask and the run carries on | pass | - | the row recorded "You said: MTransient" and the run went on to set the pre-gain to 3.00 dB  (RED on 16a: interceptTyped refused it because the server sent allowFreeText false, so the text vanished and the run sat in awaiting_ask until Stop) | `docs/review/2026-10-20/B3/editor.png` |
| **P1** an AU that cannot load in this process is never offered in the inventory | SKIP | - | every one of the 674 AU component(s) on this Mac has an arm64 slice, so there is nothing here that cannot load and nothing to withhold. NEVER counted as a pass. | - |
| **B4** a new instruction typed while an ask waits closes that run and starts its own | pass | - | rows 1 -> 2, session "ag_rig_17924" -> "", status now "Working"  (RED on 16a: the text was answered INTO the waiting run, so a request to add a plugin arrived as the answer to "What now?" and carried the old run's steps with it) | `docs/review/2026-10-20/B4/editor.png` |
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
