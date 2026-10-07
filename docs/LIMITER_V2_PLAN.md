# EchoJay Limiter v2 — plan
Goal (Sean, 7 Oct 2026): as close to FabFilter Pro-L 2 **Transparent** as we can get. On a mix bus today, Pro-L 2
Transparent at +8.2 dB sounded excellent; the EchoJay Limiter sounded bad enough that he bypassed it.
Black-box measurement of Pro-L 2's BEHAVIOUR only. Nothing of FabFilter's code, no disassembly, no reverse
engineering of the binary — we measure what comes out of it, exactly as we would measure any hardware.

## 0. THE BLOCKING PROBLEM, AND WHY THE HARNESS IS NOT WHAT WAS ASKED FOR

The brief says "the same audio through Pro-L 2 (hosted, Transparent, true peak on)". **Our harnesses cannot host
Pro-L 2, and must not try.** Two independent reasons:

1. **It is PACE-wrapped.** `echojay::refuseIfPaceWrapped` (Source/EJPaceCheck.h:48) exists precisely to refuse these
   before anything is instantiated, and ChainHost already calls it on the load path (ChainHost.cpp:7673).
2. **An unsigned binary cannot load one anyway**, and trying is actively harmful on this Mac: a PACE plugin
   authorises on LOAD, which raises iLok prompts, and on 1 Oct that took the host down twice (exit 144). The
   standing rule is that no licence-bound plugin is ever loaded from this session.

A signed probe with `com.apple.security.cs.allow-unsigned-executable-memory` could host it (the Developer ID
identity exists: `Sean Donoghue (8BT5F9B887)`). That is a real option, but it is a build-and-sign project in itself
and it puts iLok prompts back in the loop.

**So the harness compares RENDERS, not live plugins.** Sean renders the same source through each limiter in a DAW —
which he has to do anyway for the blind listening the brief asks for — and the harness only ANALYSES WAV files. No
hosting, no PACE, no prompts, and the measurements are identical in kind. It also makes every result reproducible
from files we keep, rather than from a plugin state nobody can re-create a month later.

## 1. HARNESS DESIGN — `limiter_ab_guard` (offline, file-based)

Input: pairs of WAVs, same source, sample-aligned, identical length, 48 kHz float (or 24-bit).
    source.wav            the unprocessed material
    proL2_<case>.wav      Pro-L 2 Transparent, true peak on, stated gain + ceiling
    echojay_<case>.wav    EchoJay Limiter, SAME gain + ceiling
It renders no audio itself and loads no plugin. It reads, aligns, measures, and prints a table plus a machine
-readable row per case so a run can be diffed against the last.

ALIGNMENT is the first thing it does and the first thing it asserts: cross-correlate each processed file against the
source over the first few seconds, report the offset in samples, and REFUSE the case if the offset is not constant
across the file (a limiter with lookahead has a fixed latency; a varying offset means the render is not comparable).
Every metric below is computed after alignment. This is the step that silently ruins A/Bs, so it is an assertion.

## 2. THE METRICS, and what each is for

| metric | how | what it answers |
|---|---|---|
| integrated + short-term LUFS | existing `LevelTally` (K-weighted) | did we match level at all? every other comparison is void until this is within ~0.1 LU |
| true peak (BS.1770, 4x) | existing `LevelTally::truePeakDb` | is the ceiling actually held? overs are a hard fail, not a score |
| crest factor over time | peak − RMS per 400 ms | the blunt "is it squashed" number |
| transient retention | per-hop true peak on kick-like bursts, from the existing 100 ms `hopTruePeakDb` ring | the first thing people hear as "lifeless". Measured as attack-peak retained vs source, per hit |
| GR envelope | output − input level per block, after alignment and gain matching | THE central curve: attack shape, release shape, how release varies with programme. Pro-L 2's envelope is derivable from the renders alone |
| release behaviour | GR envelope decay after a step and after a burst, fitted as time-to-X% | is the release program-dependent, and over what range |
| THD / IMD | FFT (EqFft / MeterEngine already present) on 997 Hz and a 19+20 kHz pair, at +6 and +10 dB drive | distortion character — the difference between "loud" and "nasty" |
| pumping | short-term LUFS variance over a sustained bass loop | the audible fault Sean described |
| stereo behaviour | GR difference L vs R on a hard-panned transient | channel linking, which is on the target list |

Each case is a named fixture so results accumulate: `fullmix`, `kick_bursts`, `bass_sustain`, `tone_997`,
`tone_imd`, `panned_transient`.

## 3. TARGET BEHAVIOURS — derived from the measurements, not assumed
Lookahead with a smooth (windowed) attack envelope; program-dependent multi-stage release (fast after transients,
slow on sustained level); oversampling with true-peak detection; transient/release channel linking; styles as
tunings of those parameters. The harness exists to say WHICH of these matters and by how much — e.g. if the GR
envelope shows Pro-L 2's release is effectively two-stage with a ~30 ms and a ~300 ms limb, that is a number to hit,
not a guess.

## 4. THE STOPPING RULE — needed before any code is written
"As close as possible" has no end. Proposed acceptance, to be ruled on:
 - level matched within 0.1 LU and ZERO true-peak overs above the stated ceiling;
 - transient retention within 1 dB of Pro-L 2 per hit on `kick_bursts`;
 - GR envelope within 10% on the fitted release limbs;
 - THD at +10 dB drive no worse than Pro-L 2's by more than 3 dB at any harmonic;
 - and the blind listening test passed by Sean on `fullmix` at +8.2 dB.
The listening test is the real gate; the numbers exist so we know WHY it fails when it fails.

## 5. WHAT I NEED FROM SEAN
 1. **Renders, not a hosted plugin.** For each case: source, Pro-L 2 Transparent, EchoJay Limiter — same source,
    same length, no other processing, no dither, float WAV if possible.
 2. **The exact Pro-L 2 settings** used for every render: gain, ceiling, oversampling, lookahead, channel linking,
    true-peak on/off, and any Transparent-specific controls. Without these the comparison is not reproducible.
 3. **Confirmation of the ceiling** he used in the A/B (the +8.2 dB is known; the ceiling is not).
 4. **The material itself**: the actual mix-bus passage he A/B'd, plus a kick-only burst clip and a sustained bass
    loop. Tones I can generate.
 5. **Where to put them** — a connected folder in the repo, e.g. `docs/limiter_ab/`, so renders and results live
    together and a result can be re-derived later.
 6. **A ruling on section 4's stopping rule.**

## 6. WHAT I AM NOT DOING
Not hosting Pro-L 2. Not scanning plugins. Not inspecting, disassembling or deriving anything from FabFilter's
binary. The comparison is input-output measurement of a product Sean owns and has licensed, which is ordinary
competitive measurement.
