"""Round 58: the 14 August chatter premise, made reachable. Synthesises notes
with a wide vibrato (harmonic-rich, 3 s, 6 Hz) - centred on a note, or parked
on the semitone boundary - so the target-selection chatter that
targeting_ignores_vibrato was introduced to prevent can be measured with
tools/pitch_activity PA_TRACE (count_target_switches.py). Usage: python synth_vibrato_probe.py <outdir>"""
import sys, numpy as np, wave
sr = 48000; dur = 3.0; n = int(sr * dur); t = np.arange(n) / sr
def synth(centre_c, depth_c, rate_hz, name):
    f = 440.0 * 2 ** ((centre_c + depth_c * np.sin(2 * np.pi * rate_hz * t)) / 1200.0)
    ph = 2 * np.pi * np.cumsum(f) / sr
    x = sum((0.5 ** k) * np.sin((k + 1) * ph) for k in range(6)); x *= 0.3 / np.abs(x).max()
    x *= np.minimum(1.0, np.minimum(t / 0.05, (dur - t) / 0.05))
    w = wave.open(name, 'w'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes((x * 32767).astype('<i2').tobytes()); w.close()
o = sys.argv[1]
synth(-1650, 60, 6.0, o + '/vib_boundary_F3p50_pm60.wav')   # F3 + 50c (ON the E3/F3.. boundary), +-60c
synth(-1650, 25, 6.0, o + '/vib_boundary_F3p50_pm25.wav')   # on the boundary, +-25c (never arms a pending)
synth(-1700, 60, 6.0, o + '/vib_centred_F3_pm60.wav')       # centred on F3, +-60c: the 14 Aug scenario
print('written')
