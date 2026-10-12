# Categoriser release rule (Kathy, 12 Oct; feat/ejmap-cert, in the next cut)

**The defect.** A compressor's release after the tail burst reads as a short tail, and the tail check ran before dynamics. So Lindell 254E (RT60 0.40 s, gain moving 3.14 dB with level, flat to 0.14 dB) came out "reverb or delay: review".

**The rule** (`EjmapCategorise.h`, `decide`, `kReleaseTailS = 1.5`): a gain that moves with level (≥ 1 dB) AND is flat across the tones (span < 3 dB, the categoriser's own EQ bar) wins over a tail shorter than 1.5 s. Discrete repeats are still checked first, so delays stay delays.

**Chosen from the data** (`calibration_reads.txt`, read on this Mac; `redecide_sean_605.txt`):
- Compressors' tails: 254E 0.40 s and XLA-3 0.66 s, both flat (0.14 and 0.50 dB).
- Reverbs and delays with a tail are never level-dependent beyond 0.62 dB.
- The level-dependent tails in Sean's 605 (CLA Effects span 34.7 dB, FilterFreak2 20.6, Absynth 5 MFX 4.95) are not flat. Without the flatness condition they would all have become a confident "dynamics", which is wrong (multi-effects and a filter). With it, they stay for review.
- 1.5 s is over twice the longest compressor release seen.

**Re-decided with no loads** (`ejmap --categorise-redecide <folder>`: the raw categorise traces, the same parsers):
- **Sean's 605:** 358 rows have reads, and the old rule reproduces all 358 stored answers (fidelity 358/358). **The new rule changes 0.** Nothing goes the wrong way.
- **This Mac's calibration set:** 20 rows, fidelity 20/20, and 2 change. Both are compressors in the catalogue: Lindell 254E and Acme Opticom XLA-3, "reverb (unsettled)" → "dynamics".

**Pin** CAT-RELEASE uses the real traces (`tests/fixtures/categorise_release/`): 254E and XLA-3 → dynamics, and FilterFreak2 (Sean's trace) stays reverb for review. All 3 mutants go red: flatness not required, the rule off, and the bound lowered to 0.5 s.

**MMorph** (EQ on Sean's Mac, dynamics here). The traces show a different **instantiate state**: the same version 14.16.0, but different starting values in the spectral processor (`mmorph_instantiate_state_sean_vs_here.tsv`).
- Here: Output Compression 25%, attack and release 0 ms, Harmonics and Whitening 0.
- Sean's: Output Compression 10%, 0.15 ms, Harmonics 18.5% / 12.6%, Whitening 34.8%, M/S on.
- Even the parameter names are formatted differently ("A - Compression" vs "Compression (A)").

So the plugin starts from a different default on each Mac, consistent with a per-user default preset or Melda's own settings. The reads are faithful to what each Mac's MMorph does at load: more output compression here → level-dependent (1.69 dB) → dynamics; level-independent with a 5 dB tonal span there → EQ. This is not a measuring fault. It is a limit of categorising at the instantiate state.

**For the cut's equality:** the categoriser is not part of the derive-only sets, so there is no derive-only difference. Live, a categorise answer changes only where the new rule fires: 0 of Sean's 605.
