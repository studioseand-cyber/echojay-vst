# The reward rule, Pause / Send what's done, partial uploads - rehearsals 13 Oct (wizard 48116637)

Hostable Waves v12 (DeEsser, RDeEsser, Sibilance, C1 gate). Work folders, the stub server and the results were all in scratch. The release rehearsals seed the stub server's db with the snapshot's categories.json, and B and C set `qualify_minimum: 3` in the stub server's config (a server setting).

- **A (release, default 25):** 4 < 25 -> `5_not_enough.png`.
- **B (release):**
  - launch 1 offline: the close question (`4_close_question.png`, from `--auto-ask-close`), Pause at 1 measured, the checkpoint kept in the outbox, Paused (`8b_...BEFORE_FIX.png`: Quit greyed and "has been sent" while offline, both fixed);
  - launch 2 online: the outbox sent at launch, checkpoints 1 / 3 / 4 of 4, finished -> code (`6_finish_reward.png`). Receipts in `receipts.txt`.
- **C (release):** Send what's done at 1 -> no code, the full-run message (`7_send_whats_done_no_code.png`; this shot predates dropping the repeated title from the body).
- **D (beta):** Pause at 1 -> the Desktop file (1 draft), Paused (`8_beta_paused.png`); the finish -> the SAME file rewritten (4 drafts, one file; `9_beta_finish.png`).

`1_scan_done_625_27h_STAGED.png` is STAGED: a work folder seeded with this Mac's plan figures (625 plugins, 98,513 s at 2 jobs), because a real full scan would load licence-bound vendors. `2_scan_done_rehearsal_B.png` is the same screen from the real rehearsal B (4 plugins).
