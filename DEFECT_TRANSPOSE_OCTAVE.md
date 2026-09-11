# DEFECT: transpose +12 gives a ~155c shift instead of an octave in one region; -12 loses 3.7 dB

**Filed:** 2026-09-05, round-35 verification (UI_SIMPLIFICATION.md). The
parameter has never been exposed to a user; verify-first caught it before
it became a front-panel Transpose.

MEASURED (EchoJayPitchModeTest EJ_VERIFY_OUT renders of sourceNEW at the
schema defaults, D minor by hand; ACF f0 on three sustained regions):
    default         164.4 / 172.7 / 164.4 Hz   (2.55 / 3.15 / 4.05 s)
    transpose -12    82.8 /  84.1 /  82.2 Hz   exact octave down everywhere; RMS -3.7 dB
    transpose +12   179.8 / 326.5 / 331.0 Hz   octave up at 3.15 and 4.05 s; at 2.55 s a
                                               +155c shift where +1200c was asked; RMS -0.8 dB
The fine tracker at alto_tenor cannot follow either octave (most hops
unvoiced or octave-disagreeing), so the ACF probe is the ruler here.

WHERE TO LOOK (not investigated): +12 is a ratio of 2.0 - above the 2.5-st
splice band, so the GRAIN path with formant preserve - and the 2.55 s
region is the one where the take's sustained F sits (the note whose
epochs DEFECT_GRAIN_EPOCH_UNITY worried about). The -12 level loss is
the grain path's window/overlap at ratio 0.5. Both are grain-path
behaviours the correction use never reaches (corrections stay inside
the splice band).

STATUS: unexposed; stays so until fixed and re-verified (octave exact at
every probed region, level within 0.5 dB both ways). Then FRONT (Antares
has Transpose).
