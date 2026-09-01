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
