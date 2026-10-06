AUTO-TUNE VOCAL COMPRESSOR, Sean's ruling (6 Oct). My "pitch tracker mutes the signal" was a guess and wrong: the unit is a plain
compressor (his sweep: unity -60..-6 at default, compresses cleanly). What his detector run shows (the two traces here):
  sine     : 36 holds, output tracks the input, 2 dB reached at -15.30 dBFS at norm 0.70 (threshold '-18')
  two-tone : the SAME set lines (diff: only the spec line's signal and hz2 1201), input present (in_rms -35 dBFS at level -32),
             output EXACTLY 0 at every one of the 36 levels (level_db -999, out_peak_db -999, nonfinite 0, tone_frac 0)
So "not_reached" came from silence, not from the ladder's top or headroom: the probe read no output at all under 997 + 1201 Hz. The
probe's two-tone path is the one that read 2 dB points on the other 68 compressors; what in the plugin silences a two-tone input the
probe cannot say (nothing non-finite, nothing clipped). The retry at the -27 position now runs both signals again before giving up.
Built: (1) an attempted, unmeasured detector exports EXACTLY "detector_f": null, "detector_f_source": "unknown", the reason in notes,
never assumed, never 0.0; measured profiles carry no detector_f_source; the tone check with a null detector_f tests at both L_ref values
(f = 0 rms -18.4, f = 1 peak -9.21) and passes only if both pass, both recorded (rehearsed live on Lindell SBC with an injected
unmeasurable detector: the two files here). (2) the retry. The Vocal Compressor itself cannot be run here (licence-bound).

SEAN (6 Oct evening): "detector_f_source": "measured" on every measured profile - derive-only over a copy of his folder with the new build, nothing loaded:
{('measured', 'number'): 81, (None, 'number'): 1}
SECOND SIGNAL rehearsal on Lindell SBC (norm 0.7): 2 dB points sine -20.61, two-tone -20.63, sine+2nd harmonic -20.62 -> f 0.01 (two-tone) vs 0.00 (2nd harmonic over its 1.90 dB crest): consistent (an rms unit).
