# seam_attack_ms: the broader-material validation still owed - what to bounce

The 60 ms default (d637292) is validated on two takes: the standing NEW
take as the bar and the OLD low-male take as the falsifier. It is flipped,
not shipped, until it has been heard and measured on material that
exercises what the ramp touches: word starts after consonants, the first
tens of ms of every voiced onset. This is the list, in priority order.
Each: 10-20 s, DRY, unprocessed, 48 kHz, mono or stereo, with a few clear
word starts. Bounce the SOURCE only - the renders are made here.

1. CLEAN MODERN POP VOCAL - close mic, no room, precise pitch, hard
   consonant onsets (t/k/p words). Tests the ramp where a listener expects
   Auto-Tune-grade snap: any softening of the first 60 ms is most audible
   here.
2. HEAVY VIBRATO - a held-note singer with 6+ Hz, 40c+ vibrato and
   vibrato already moving at word starts. Tests the ignore-vibrato ON
   residual (DEFECT_VIBRATO_ON_TUNING_COST re-opened) and whether the
   aperiodic mid-note re-entries that dodge the seam discriminator exist
   on a second singer.
3. BREATHY / QUIET - aspirated onsets (h-words, soft attacks, low level).
   Tests the audio-testimony discriminator's aperiodicity threshold
   (0.5): breathy re-entries sit near it, and a misclassified blink here
   re-zeroes a good correction.
4. RAP / SPOKEN WITH HARD CONSONANTS - fast syllables, 60 ms is a large
   fraction of the vowel. Tests whether the ramp leaves audible dry pitch
   on short vowels (the tail cost the bar accepted as informational).
5. A GENUINELY UNTUNED RAW STEM - a singer well off-grid (30-60c), no prior
   correction. Both standing takes were near-grid; a large standing
   correction at every word start is the case where "resume at the dry
   pitch and ramp" costs the most.

AT BOUNCE TIME, per the standing provenance rule (REFERENCE_SET.md): note
the file name, the session's voice_type, and - if the take will also be
bounced THROUGH EchoJay - the [DETECTED KEY] line (source, key,
confidence, reference) and the installed bundle's UUID. A row without
provenance is not a reference row.

What will be measured on each: the corrected bar's legs on the seam ramp
(word-start event fraction at seam 0 vs 60; paired per-instant sustain
delta; onset off-grid median paired; tails), tools/pitch_activity's
aligned attribution, and your ear on the word starts, seam 0 vs 60, one
file per comparison.
