# Threshold-sweep traces, 29 Sep 2026 (batch 2)

Raw probe output from `ejmap --cert-sweep-all` under the rules committed in 6c0c249. That means
one process per position, each write escalated and its mechanism recorded, a quiet reference for
input-as-threshold, and the narrowed licence rule. 28 products were attempted: kHs and townhouse
were skipped explicitly, and the 19 UAD-2 products were skipped by the hardware category.

Conditions to know when reading these:
- The Mac was on battery at 9% and slept through most of the run. It advanced only in dark wakes,
  from about 18:48 to 19:25.
- One bridged process straddled a wake: RCompressor (s) position 5. There the ratio read 0, the
  silence escalation "rendered" 2.1 M blocks inside its 500 ms, and the write was correctly left
  unlanded.
- Four PACE-wrapped products brought up PACE's activation window at load: Tube-Tech CL 1B, SSL
  Native X-Comp v6, U73b and SSL Native Bus Compressor 2. In batch 1 the same four instantiated
  and rendered, so licence availability changed between the runs (cause not verified).

Redaction: `/Users/<name>` was replaced by `~` in plugin logger lines only.
