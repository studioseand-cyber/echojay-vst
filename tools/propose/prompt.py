"""The one prompt both arms see.

Nothing here is arm-specific, and that is load-bearing rather than tidy: the
whole design rests on two models agreeing, and two models given different text
agreeing means nothing. The prompt is hashed into every proposal file so a row
written today can be reconstructed against the text that produced it.
"""
import hashlib

from evidence import VOCAB

DEFINITIONS = """\
threshold_db     the level above (or below) which processing begins, in dB
ratio            compression/expansion ratio, e.g. 4 meaning 4:1
attack_ms        how fast the processor responds to a rise, in milliseconds
release_ms       how fast it recovers, in milliseconds
makeup_db        gain added after compression to restore level, in dB
knee_db          the width of the soft knee around the threshold, in dB
mix_pct          dry/wet blend of processed against unprocessed, as a percentage
wet_pct          the wet (processed) amount alone, as a percentage
input_db         gain applied at the input of the processor, in dB
output_db        gain applied at the output of the processor, in dB
ceiling_db       the absolute output limit a limiter will not exceed, in dB
freq_hz          a centre or corner frequency, in Hz
gain_db          the boost or cut of an EQ band, in dB
q                bandwidth / resonance of an EQ band (dimensionless)
low_cut_freq_hz  a high-pass / low-cut corner frequency, in Hz
high_cut_freq_hz a low-pass / high-cut corner frequency, in Hz
sensitivity      how strongly the detector reacts (de-essers, transient shapers)
drive            how hard the signal is pushed into a saturating stage
delay_time_ms    delay time, in milliseconds
feedback_pct     how much of the delay output is fed back, as a percentage
reverb_decay_s   reverb decay / RT60, in seconds
predelay_ms      delay before the reverb onset, in milliseconds
position         a stepped multi-position selector (e.g. a time-constant switch)"""

SYSTEM = f"""You are labelling the controls of an audio plugin from measured evidence alone.

You will be given the plugin's name, its category, and controls a sweeper
measured on it. Each control carries:

  name     the parameter name exactly as the plugin reports it
  kind     "anchored" (continuous, the sweeper could read values back) or
           "mode" (a switch, non-numeric)
  range    the parameter range the plugin declared, or null if it declared none
  span     the numeric span the sweep actually observed, or null
  unit     the unit the plugin's OWN VALUE DISPLAY printed, or "" for none
  anchors  how many points the sweep captured

ABOUT unit -- read this carefully.

An empty unit is a MEASUREMENT, not a missing field. It means the plugin's value
display printed a bare number with no unit at all. Most controls are like this.
Do not invent a unit that was not declared, and do not treat the absence as
licence to trust the name alone; weigh the name against the range and span.

Where a unit IS declared it is authoritative and it outranks the name. A control
called "Attack" whose display reads dB is not a time control, whatever it is
called. A control whose display reads dB over a symmetric range like -15..+15 is
a gain-like control, not a millisecond one. Names lie; measured units do not.

Assign each control exactly one of these semantics, or "none":

{chr(10).join('  ' + v for v in VOCAB)}

What each one means:

{DEFINITIONS}

RULES

1. Answer for every control you are given, in the order given.
2. Use "none" when nothing in the list applies -- bypasses, in/out switches,
   band or mode selectors, metering, anything outside the list. Most plugins
   have several. "none" is a real answer, not a way of opting out.
3. confidence is "high" only when the evidence supports the label -- a declared
   unit that agrees, or a range and span that only one semantic fits.
   Use "low" when you are reading the name and guessing, when the unit is absent
   and the range is ambiguous, or when two semantics fit equally well.
4. Do not explain. Do not add commentary before or after.

Return JSON and nothing else, in exactly this shape:

{{"controls": [{{"name": "<the control name, copied exactly>", "semantic": "<one of the list, or none>", "confidence": "high"|"low"}}]}}"""

SYSTEM_SHA = hashlib.sha256(SYSTEM.encode()).hexdigest()[:16]
