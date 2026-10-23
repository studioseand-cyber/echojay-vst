# Matrix report - 2026-10-23 (live pass, 5 attempt(s) per live id)

Build under test: **build-guards**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-23/<scenario>/`.

**15 passed, 3 failed, 2 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **F3** a run SURVIVES its window closing (0b on, his 20 Oct proof), and the reopen shows it once | pass | - | the run was still live, which is what this build ships; the reopened window draws 1 run row(s) and 1 copy(ies) of the goal  (0b is ON - a close must not kill the run, and must not leave a second row either) | `docs/review/2026-10-23/F3/editor.png` |
| **N1** a started run has a row drawn, in the chat it was typed into | pass | - | the row was drawn in 46 ms, rows=5 -> 6 (this run must add exactly one), chat unchanged  (his overnight ask: "a run started, no row drawn". The bound is the row: a card that arrives when the run ENDS is the same fault as no card) | `docs/review/2026-10-23/N1/editor.png` |
| **N2** an agent build with a loudness target leaves the user's pre-gain alone and inserts no limiter | SKIP | - | this row needs a build it CHOSE the shape of - one add, a loudness target, no limiter offered - and on the live road the model picks. It did exactly what it should on this run (read the rack, then asked for playback, because a target needs something to measure), and left an empty rack, which would have let the row pass while proving nothing. It runs on the mock, where the build is scripted. | - |
| **A3-live** live: add a third party with a setting, dialled and confirmed | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/A3-live/editor.png` |
| **A4-live** live: the same add, on a Link | **FAIL** | B | 0 of 5 live attempt(s) held.  attempt 1: the Link's landed field still has no Threshold fifteen seconds after the run: ""  \|  attempt 2: the Link's landed field still has no Threshold fifteen seconds after the run: ""  \|  attempt 3: the Link's landed field still has no Threshold fifteen seconds after the run: ""  \|  attempt 4: the Link's landed field still has no Threshold fifteen seconds after the run: ""  \| ... | `docs/review/2026-10-23/A4-live/editor.png` |
| **B1-live** live: a goal that names a track is answered ON that track | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/B1-live/editor.png` |
| **C1-live** live: the closing never claims a figure the rack does not show | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/C1-live/editor.png` |
| **B2-live** live: tapping a chip answers the question and the run carries on | pass | - | 5 of 5 ask(s) held (it asked in 5 of 5 attempts) | `docs/review/2026-10-23/B2-live/editor.png` |
| **B3-live** live: a typed answer lands in the waiting ask and the run carries on | pass | - | 5 of 5 ask(s) held (it asked in 5 of 5 attempts) | `docs/review/2026-10-23/B3-live/editor.png` |
| **B4-live** live: a new instruction closes the waiting run and starts its own | pass | - | 5 of 5 ask(s) held (it asked in 5 of 5 attempts) | `docs/review/2026-10-23/B4-live/editor.png` |
| **E10-live** live: with audio playing, a GR question is MEASURED, not asked about | **FAIL** | B | 1 of 5 live attempt(s) held.  attempt 2: it neither gave a gain-reduction figure nor said why it could not read one: "add echojay compressor=added echojay compressor in slot 1.; read the rack=; ready to measure the gain reductio"  \|  attempt 3: it neither gave a gain-reduction figure nor said why it could not read one: "add echojay compressor=added echojay compressor in slot 1.; is the track playing now?=; how's ... | `docs/review/2026-10-23/E10-live/editor.png` |
| **E11-live** live: a run that needs the user to act waits as an ask, and the reply resumes THAT job | pass | - | 5 of 5 ask(s) held (it asked in 5 of 5 attempts) | `docs/review/2026-10-23/E11-live/editor.png` |
| **E13-live** live: a read-only answer ends without an offer to change anything | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/E13-live/editor.png` |
| **A8-live** live: a plugin that is not installed is offered as the nearest one that is | pass | - | 4 of 4 ask(s) held (it asked in 4 of 5 attempts).  attempt 1: it did not ask | `docs/review/2026-10-23/A8-live/editor.png` |
| **A9-live** live: the installed plugin is offered once, not twice | pass | - | 5 of 5 ask(s) held (it asked in 5 of 5 attempts) | `docs/review/2026-10-23/A9-live/editor.png` |
| **C2-live** live: a placed add states the position the rack actually has | **FAIL** | B | 3 of 5 live attempt(s) held.  attempt 1: B: the compressor landed at slot 2, AFTER the EQ at 1 - the add ops carried no position and the closing claimed one  \|  attempt 4: there is no EchoJay EQ on the rack to place it against | `docs/review/2026-10-23/C2-live/editor.png` |
| **C3-live** live: the closing says "confirmed" only when a value was read back | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/C3-live/editor.png` |
| **E1-live** live: a gain-reduction answer gives the loudest hits and the typical | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/E1-live/editor.png` |
| **E3-live** live: an uncertified plugin's GR is refused without blaming silence | pass | - | 5 of 5 live attempt(s) held | `docs/review/2026-10-23/E3-live/editor.png` |
| **A6** (agent OFF) a plain chat turn is answered and the rack is untouched | SKIP | - | the server did not answer this turn (2 transcript row(s) arrived and the last assistant line was about something else), so the reply half cannot be judged. The rack half HELD: it was untouched with the agent off. NEVER counted as a pass. | - |

## Failures by owner

**A**: nothing

**B**: 3 - A4-live live: the same add, on a Link; E10-live live: with audio playing, a GR question is MEASURED, not asked about; C2-live live: a placed add states the position the rack actually has

**Sean**: nothing
