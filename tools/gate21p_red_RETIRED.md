# gate21p_red — RETIRED 29 Sep 2026 (21t-m)

This was the behavioural known-bad for 21p items 1 and 4: it drove `ChainHost::measureUnityTrims` over a SILENT
window and asserted that the gated pass refuses the reading and names the reason, where the pre-21p pass wrote a
trim from the floor.

**Its subject no longer exists.** The 29 Sep 2026 ruling deleted the third gain — `measureUnityTrims`,
`setSlotTrimDb`/`getSlotTrimDb`, `slotTrimText`, `hasActiveTrims`, `setCompareActive` and the compare-only
multiply in `SlotWetBlend`. A slot now has exactly two EchoJay gains, IN (the pre-trim, which is the drive) and
OUT (the slot output gain, which the hold writes). There is no unity-trim pass to gate.

The behaviour it was protecting is not lost: **Listen writes nothing at all now** (21t-m item 2), which is a
stronger rule than "Listen must not write from a silent reading", and it is asserted by
`level_loop_guard` case (2d) — with the arm and the measurement asserted first, so that leg cannot pass by not
running.

The file was never in a ctest label (a RED-by-construction leg cannot sit in a suite that must be GREEN), so
retiring it removes nothing from the gate.
