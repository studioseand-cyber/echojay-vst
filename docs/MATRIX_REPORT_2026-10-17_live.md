# Matrix report - 2026-10-17 (live pass, 1 attempt(s) per live id)

Build under test: **build-guards**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-17/<scenario>/`.

**13 passed, 0 failed, 9 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **F3** a run stops when its window closes (0b off), and the reopen shows it once | pass | - | the run stopped with the window; the reopened window draws 1 run row(s) and 1 copy(ies) of the goal  (item 0b is OFF by design, so stopping IS the shipped behaviour - what must not happen is a duplicate or a run left spinning) | `docs/review/2026-10-17/F3/editor.png` |
| **A3-live** live: add a third party with a setting, dialled and confirmed | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/A3-live/editor.png` |
| **A4-live** live: the same add, on a Link | SKIP | - | the Link that AU hosting loads here is the INSTALLED binary, not the one this run built (item 0b: registerAU registers a factory and JUCE instantiates the IDENTIFIER, so the system's component wins in a fresh process). A V2 from this build talking to a Link from the last one is a configuration that never ships, so a verdict from it would be about nothing. A3-live proves the own rack; the Logic smoke test covers th... | - |
| **B1-live** live: a goal that names a track is answered ON that track | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/B1-live/editor.png` |
| **C1-live** live: the closing never claims a figure the rack does not show | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/C1-live/editor.png` |
| **B2-live** live: tapping a chip answers the question and the run carries on | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-17/B2-live/editor.png` |
| **B3-live** live: a typed answer lands in the waiting ask and the run carries on | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-17/B3-live/editor.png` |
| **B4-live** live: a new instruction closes the waiting run and starts its own | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-17/B4-live/editor.png` |
| **E10-live** live: with audio playing, a GR question is MEASURED, not asked about | SKIP | - | the behaviour this row measures is the SERVER's half of continuity (FOR_B Â§16), which is not in this build: attempt 1: B: it asked for playback and ENDED (state "done") while audio was already flowing, with transport{playing,signalNow} in the start body: "add echojay compressor=added echojay compressor first; read back 1 slot(s) on the rack; read back echojay comp".  NEVER counted as a pass. | - |
| **E11-live** live: a run that needs the user to act waits as an ask, and the reply resumes THAT job | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-17/E11-live/editor.png` |
| **E13-live** live: a read-only answer ends without an offer to change anything | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/E13-live/editor.png` |
| **A8-live** live: a plugin that is not installed is offered as the nearest one that is | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-17/A8-live/editor.png` |
| **A9-live** live: the installed plugin is offered once, not twice | SKIP | - | the real model did not ask a question in 1 attempts of "Add Pro-C 3 to this rack", so there was no waiting ask to act on. NEVER counted as a pass. | - |
| **C2-live** live: a placed add states the position the rack actually has | SKIP | - | the behaviour this row measures is the SERVER's half of continuity (FOR_B Â§16), which is not in this build: attempt 1: B: the compressor landed at slot 2, AFTER the EQ at 1 - the add ops carried no position and the closing claimed one.  NEVER counted as a pass. | - |
| **C3-live** live: the closing says "confirmed" only when a value was read back | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/C3-live/editor.png` |
| **E1-live** live: a gain-reduction answer gives the loudest hits and the typical | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/E1-live/editor.png` |
| **E3-live** live: an uncertified plugin's GR is refused without blaming silence | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-17/E3-live/editor.png` |
| **L1** add a compressor on another track's Link | SKIP | - | the Link that AU hosting loaded is the INSTALLED binary, not the one this run was given (item 0b): this row sends an edit from THIS build's V2 and reads the LINK's own sidecar, so in a mixed pairing the two halves of its claim come from different builds. NEVER counted as a pass. | - |
| **L2** a build on another track's Link | SKIP | - | the Link that AU hosting loaded is the INSTALLED binary, not the one this run was given (item 0b): this row sends an edit from THIS build's V2 and reads the LINK's own sidecar, so in a mixed pairing the two halves of its claim come from different builds. NEVER counted as a pass. | - |
| **L3** a Link's OUT, set from the V2 | SKIP | - | the Link that AU hosting loaded is the INSTALLED binary, not the one this run was given (item 0b): this row sends an edit from THIS build's V2 and reads the LINK's own sidecar, so in a mixed pairing the two halves of its claim come from different builds. NEVER counted as a pass. | - |
| **K1** (fix 5) selecting a Link straight after an agent build shows its slots | SKIP | - | the Link that AU hosting loaded is the INSTALLED binary, not the one this run was given (item 0b): this row compares the LINK's sidecar with THIS build's view, so a mixed pairing puts the two halves of its claim in different builds - tonight it read "sidecar [] and the view []", which is the pairing talking, not the product. NEVER counted as a pass. | - |
| **A6** (agent OFF) a plain chat turn is answered and the rack is untouched | SKIP | - | the server did not answer this turn (2 transcript row(s) arrived and the last assistant line was about something else), so the reply half cannot be judged. The rack half HELD: it was untouched with the agent off. NEVER counted as a pass. | - |

## Failures by owner

**A**: nothing

**B**: nothing

**Sean**: nothing
