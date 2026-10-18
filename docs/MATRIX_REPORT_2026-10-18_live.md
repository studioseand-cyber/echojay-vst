# Matrix report - 2026-10-18 (live pass, 1 attempt(s) per live id)

Build under test: **build-guards**.  Runner: `tools/host_rig` driving the real bundles, real plugins and real editors; evidence under `docs/review/2026-10-18/<scenario>/`.

**10 passed, 3 failed, 4 skipped.** A skip is never a pass (matrix rule 9).

| Scenario | Result | Owner | What happened | Evidence |
|---|---|---|---|---|
| **F3** a run stops when its window closes (0b off), and the reopen shows it once | pass | - | the run stopped with the window; the reopened window draws 1 run row(s) and 1 copy(ies) of the goal  (item 0b is OFF by design, so stopping IS the shipped behaviour - what must not happen is a duplicate or a run left spinning) | `docs/review/2026-10-18/F3/editor.png` |
| **A3-live** live: add a third party with a setting, dialled and confirmed | **FAIL** | B | 0 of 1 live attempt(s) held.  attempt 1: the slot's landed field is empty: "read the inventory=; read the inventory=; read the rack=;  I tried adding Renaissance Axx (s) but EchoJay refu" | `docs/review/2026-10-18/A3-live/editor.png` |
| **A4-live** live: the same add, on a Link | **FAIL** | B | 0 of 1 live attempt(s) held.  attempt 1: the Link's landed field still has no Threshold fifteen seconds after the run: "slot 1 \|slot 2 " | `docs/review/2026-10-18/A4-live/editor.png` |
| **B1-live** live: a goal that names a track is answered ON that track | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-18/B1-live/editor.png` |
| **C1-live** live: the closing never claims a figure the rack does not show | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-18/C1-live/editor.png` |
| **B2-live** live: tapping a chip answers the question and the run carries on | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-18/B2-live/editor.png` |
| **B3-live** live: a typed answer lands in the waiting ask and the run carries on | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-18/B3-live/editor.png` |
| **B4-live** live: a new instruction closes the waiting run and starts its own | SKIP | - | the real model did not ask a question in 1 attempts of "Add a compressor to this rack", so there was no waiting ask to act on. NEVER counted as a pass. | - |
| **E10-live** live: with audio playing, a GR question is MEASURED, not asked about | SKIP | - | the behaviour this row measures is the SERVER's half of continuity (FOR_B Â§16), which is not in this build: attempt 1: B: it asked for playback and ENDED (state "done") while audio was already flowing, with transport{playing,signalNow} in the start body: "add echojay compressor=added echojay compressor first; read back 1 slot(s) on the rack; read back echojay comp".  NEVER counted as a pass. | - |
| **E11-live** live: a run that needs the user to act waits as an ask, and the reply resumes THAT job | pass | - | 1 of 1 ask(s) held (it asked in 1 of 1 attempts) | `docs/review/2026-10-18/E11-live/editor.png` |
| **E13-live** live: a read-only answer ends without an offer to change anything | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-18/E13-live/editor.png` |
| **A8-live** live: a plugin that is not installed is offered as the nearest one that is | SKIP | - | the real model did not ask a question in 1 attempts of "Add Pro-C 2 to this rack", so there was no waiting ask to act on. NEVER counted as a pass. | - |
| **A9-live** live: the installed plugin is offered once, not twice | SKIP | - | the real model did not ask a question in 1 attempts of "Add Pro-C 3 to this rack", so there was no waiting ask to act on. NEVER counted as a pass. | - |
| **C2-live** live: a placed add states the position the rack actually has | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-18/C2-live/editor.png` |
| **C3-live** live: the closing says "confirmed" only when a value was read back | **FAIL** | B | 0 of 1 live attempt(s) held.  attempt 1: it said confirmed with nothing read back: "I can't add Renaissance Axx (s) here - EchoJay flags it as not yet confirmed as settable, so it refuses the ad" | `docs/review/2026-10-18/C3-live/editor.png` |
| **E1-live** live: a gain-reduction answer gives the loudest hits and the typical | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-18/E1-live/editor.png` |
| **E3-live** live: an uncertified plugin's GR is refused without blaming silence | pass | - | 1 of 1 live attempt(s) held | `docs/review/2026-10-18/E3-live/editor.png` |

## Failures by owner

**A**: nothing

**B**: 3 - A3-live live: add a third party with a setting, dialled and confirmed; A4-live live: the same add, on a Link; C3-live live: the closing says "confirmed" only when a value was read back

**Sean**: nothing
