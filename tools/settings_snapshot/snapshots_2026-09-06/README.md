# Settings snapshots, 6 Sep 2026 (round 60, the withheld-section removal)

## Evidence, stated honestly
The FIRST before set rendered in this round had the first-run prompt card
over the panel (the harness did not yet hide the overlays); it is NOT filed
here and was never a like-for-like pair. Standing rule: a control measured
with a different instrument than the test is not a control.
The pairs filed here are the SAME harness (tools/settings_snapshot, overlays
hidden, 1x), with:
  - before = the library built from da6939f^ (15e51fd) in a worktree, the
    harness compiled against THAT tree's headers (compiling against the new
    header and linking the old library gave garbage member offsets - caught
    by the audit reading a 1 px viewport, and re-done);
  - after  = the library of the installed build (da6939f).

## The chain-list count, same instrument both sides (must not move; it did not)
    | | scanner rows | enabled names | chain feed entries | AU-host collapsed view | enabled names usable in NEITHER format (excluded) |
    |---|---|---|---|---|---|
    | before (15e51fd) | 1683 | 1346 | 2328 | 2129 | 539 |
    | after (da6939f)  | 1683 | 1346 | 2328 | 2129 | 539 |
(The removed panel counted 537 of 1340 on Sean's session; this harness reads
the same caches under a different enabled set - 1346 - and applies the
panel's core test, resolveByName in both formats. The number to watch is
the feed: 2328 entries, 2129 in the AU view, identical.)

## The bounds audit (every control below the removed block, down to the bottom row)
Sizes 900x580 (the minimum), 1000x650, 1100x720, 1400x900, 1800x1200 (the
maximum): UI SCALE, CHAIN SUGGESTIONS, the YOUR PLUGINS field, View all,
Save, Manual, the saved label, Help & Support, Log Out - visible, inside
the content, reachable at every size. 0 failures. Content height before /
after: 984 / 788 (900x580, 1000x650), 984 / 653 (1100x720 - now fits
without scrolling), 833 / 833 (1400x900), 1133 / 1133 (1800x1200).
