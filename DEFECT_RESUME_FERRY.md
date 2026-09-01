# DEFECT (filed, terminal): the gap-resume envelope ferry — documented limitation

**Filed:** 2026-09-01, by terminal ruling after four measured rounds and a
threshold search that came back empty. Do not propose a fifth variant.

## The defect

At a sub-200ms gap resume, the retune envelope legitimately resumes "the
note in progress" — position and target. When the pre-gap position is an
unconverged limbo (long τ) that detection wander (voice-type mismatch) has
dragged off-pitch, the resume ferries it into the new syllable: measured
as a 230ms, −123c, OFF-GRID hold at 5.20s (sourceNEW, alto_tenor, τ=400),
nine such divergences on the take, eight negative.

**Exposure requires BOTH long τ (≳200ms) and a mismatched voice_type.**
Neither shipped preset reaches the τ regime (natural=120 measured and
ear-verified healthy through every round), and the plugin now actively
flags the mismatch (the voice-fit readout: "voice alto_tenor — range
suggests low_male"). The remaining exposed population is users who set a
character-zone τ by hand while ignoring an amber warning.

## Why it is filed rather than fixed — the four-round chain (register §17.6)

1. Unconditional re-anchor at resumes: healed the ferry, regressed EVERY
   panel row (hard 94.8→85.7 same-semitone) — re-anchored at the 11ms
   blinks the 200ms rule protects. The rule's own first draft violated
   the rule.
2. Corridor judged against one fresh sample: fired on 33–41% of resumes
   (excess tail 1269c) — judged good carried state against the least
   trustworthy sample in the signal, sometimes re-anchoring TO octave
   mis-reads.
3. Corridor at the 3-hop median with deg(median) endpoint: fire-rate UP
   (64–74%) — under ignore-vib the aim's degree is the SLOW track's;
   fixed to deg(slow) → 6–18%, window healed — but re-anchoring
   noteRefCents_ spawned third-note votes (+82). Cur-only: hard
   near-inert AND healed; natural 56→69 spans from ~4 fires.
4. The terminal (i) measurement: carried-offset distributions at every
   resume, both takes, both voices — natural's fires (73–215c) and the
   ferry-regime fires (49–1092c, p50 77–100) FULLY INTERLEAVE. No
   threshold exists that heals the ferry and leaves natural inert.

## What stands

The corridor machinery remains in the code behind `debugMedianSeed`
(0 = shipped/off, 1 = note-start median, 2 = +resume corridor, cur-only),
correct per its final form, for anyone who genuinely needs long-τ +
mismatch operation. The fences: the voice-fit readout, §17.4's τ-caveat,
and §17.5/§17.6 in the register.

## OVERTURNED 2026-09-01 — the mechanism attribution was wrong

The matched-settings round (both UIs read; REFERENCE_SET.md block)
disproved the ferry: the pre-gap envelope entered the 130ms gap HEALTHY
(target 172.2Hz, applied +3.5c, converged on F3). The -123c hold was
made in ONE HOP at 5.264: a single 148.2Hz onset mis-read raised a
pending, and the then-shipped envExp 5 release eased curCents_ toward
that very sample (10ms pole, one hop = -60c: 172.2 -> 161.5); the
pending reverted next hop; tau400 stranded the drag. Causality: env0
renders the window clean (173-174), env5 the defect (163-166). Antares's
immunity: no such release exists there.

**The four rounds above were comparing a regression against itself: the
baseline contained envExp 5 (the then-default) in every panel.** The
transferable lessons stand; the ferry as a mechanism does not. The
terminal filing is superseded: envExp 5's default was REVERTED
(commit f1d9f5f) and the median-destination fix goes through the
four-part acceptance before any re-enable. See §17.6's corollary:
three fixes written to enforce the re-anchor rule violated it at their
own value-choosing sites.
