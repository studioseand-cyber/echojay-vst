# DEFECT (round 50, 5 Sep 2026): CORRECTION EFFECTIVELY OFF WHILE THE PANEL LOOKS ACTIVE - Sean's EJ1 "out of tune" bounce

Sean's three bounces of a new section, measured by the reviewer (2404
comparable hops): Antares off-grid 2.13c / 63.6% within 5c; EJ1 (his "out
of tune" bounce) 11.78c / 25.5%; EJ2 (after fiddling and returning the
dial to 0) 3.19c / 57.7%. Ruled out by measurement: a reference offset
(best-fit A4 439.8 for all three; EJ1 at its own best reference still
11.59c) and a wrong key or scale (chromatic and D-minor figures identical).
EJ1 MEASURES LIKE NEARLY UNCORRECTED AUDIO. The clue: touching controls
restored it.

His current session is not on this Mac (test.logicx was last saved 3 Sep;
the new-section bounces went to the reviewer), so the state was found by
code audit and REPRODUCED BY MEASUREMENT, not read from disk.

## Reachable states that look active and barely correct - each measured on the reference take
Ruler: tools/pitch_key_forensic (all-voiced off-grid vs D minor @ 440,
% within 5c; onset median), the same ruler class the reviewer used.
SOURCE (dry): 6.72c / 39.8% (onset 8.26c).

### 1. THE DEFAULT ITSELF: natural_vibrato 100 = KEEP VIBRATO ON on every fresh instance (the prime finding)
    fresh defaults (KEEP VIBRATO on):   8.71c / 30.1%   onset 10.41c   <- WORSE THAN THE SOURCE on both metrics
    the same with natural_vibrato 0:    2.44c / 66.2%   onset  4.11c   <- the sound of every ear-confirmed clip
Reachable without the user knowing: YES - it is what a NEW INSERT does.
A new section on a new track starts here. The schema default (100, "keep
the singer's vibrato") sends correction through the SHIFT PATH, which
cannot snap (the 140 ms slow track, PATH_UNIFICATION) - and on the
all-voiced ruler the kept wobble plus the lagging centre is WORSE than
dry. EVERY measurement and every ear-confirmed clip of rounds 31-46 was
made with natural_vibrato 0 (the pitch_activity spec's `nat` field); the
schema default never went through the measurement. The verification
renders of ruling A rendered 0 and 100 and confirmed they DIFFER - but
nobody put the default through the tuning ruler.
The match to EJ1: 25.5% within 5c against this state's 30.1% on different
material; Antares' 63.6% against the nat-0 state's 66.2%. Sean's OLD
session (3 Sep decode) carried natural_vibrato 0 explicitly, which is why
it never showed there. "Fiddling" on a front panel where KEEP VIBRATO is
LIT by default plausibly included switching it off.
DECISION (provisional, reversible by ruling - the round-40 form): THE
DEFAULT IS NOW 0. The mode table is unchanged (natural/balanced keep at
100 by choice; tuned 40; hard 0). Sean's saved states carry the field
explicitly and render bit-identical (leg below); only a FRESH instance
changes - to the sound that was measured and approved.

### 2. THE DEPTH TRAP: a saved low depth loads under a dial that reads 0
A session saved by the round-45 build (front DEPTH knob, no `retune`
field) with DEPTH turned down loads, under the round-46 rule "nothing
already saved is reinterpreted", with that depth applied, the dial at 0
"(off)", and DEPTH now in ADVANCED. Reproduced (suite, "THE DEPTH TRAP"):
    loaded: retune dial 0, retune_speed_ms 6.0, depth 10, off-curve 1
    deviation from the latency-aligned source (RMS % of source): dial 0 69.5%  TRAP 11.4%  after the touch 69.5%
    off-grid: TRAP 6.53c / 40.3% (= the SOURCE, 6.72 / 39.8)   after the touch 2.35c / 69.3%
    turning RETUNE (even back to 0) rewrites depth to 100 -> BIT-IDENTICAL to dial 0
Reachable without the user knowing: YES (any session saved with the old
front DEPTH knob below 100, or a depth written by the model/chain).
"Restored by touching" and "returning to 0": exact. The semantics stay
(round 46, bar leg 1); the DIAGNOSTIC below is what makes it visible.

### 3. NOT reachable from the front without the user knowing (checked in code)
  - CORRECT off, MIX 0: both in ADVANCED, both saved in the file; touching
    RETUNE would NOT restore them (they are not on the curve) - not his case.
  - a mode (natural: retune 120 / flex 55 / humanize 60): the dial shows 79
    "(off)", FLEX and HUMAN show their values on the front - visible.
  - flex high: visible on the front. Bypass: dims the whole panel.
  - "applied on change but not on load": every setter was audited
    (EedPitchProcessor::setParamValue vs prepareToPlay/applyReset): every
    engine write is a parameter (atomic) or is re-derived on the first
    block (voice type -> latency/lag geometry, memo -1 after prepare); the
    round-48 reset clears runtime state only. NONE found for audio.
    ONE FOUND FOR STATE: see 4.

### 4. A second state bug of the same class, fixed: key_source AUTO loaded as MANUAL
The schema applies key_source before key_root and scale; the key setters
took manual on every non-defaults write - INCLUDING a state load. Every
session saved in AUTO loaded as MANUAL (the reference path had the "only a
LIVE write takes manual" guard; the key path did not). Fixed: the guard
now also excludes applyingState(). Suite: an AUTO file loads AUTO; a live
key_root write still takes manual.

## THE DIAGNOSTIC (ruled: the panel must answer "is correction actually running" at a glance)
A status line, front (above the ribbon) and ADVANCED (in the READOUTS
title), from STATE and from MEASUREMENT:
  - green  "CORRECTING  6 ms / depth 100 %   applied 4.5 c (2 s median)"
           (+ "(off the curve)" / "KEEP VIBRATO on: note centres only, the wobble stays" when true)
  - amber  "CORRECTING WEAKLY - depth 10 % (ADVANCED > DEPTH; turn RETUNE to reset)   applied ..."
  - amber  "NOT CORRECTING - CORRECT is off (ADVANCED) / depth 0 % / mix 0 % / BYPASSED   applied ..."
The measurement is the applied shift per traced voiced hop, |cents|,
median over the last ~2 s, on EITHER path (the trace's applied field now
carries the legacy path's target-vs-f0 as well as the shift path's
shift). A device that is passing through shows 0.0 whatever its panel
says. Rendered and read (snapshots front_depth_trap.png, front.png,
advanced_340.png).

## Continuous playback (the saved-state identity harness, pre-round-46 references)
Sean's three saved states: NEW take 3/3 and OLD take 3/3 bit-identical.
fresh_default DIFFERS on both takes - that is the default change, and the
reference was re-baselined to the new default after the other three
passed. Suite: 200 PASS / 0 FAIL.

## Installed (5 Sep 2026), ~/Library only, via tools/install_local.sh build-release
  | plugin | arm64 UUID |
  |---|---|
  | AU   `EchoJay V2.component` | 230EA288-CE5E-3B68-A493-A081951C7D06 |
  | VST3 `EchoJay V2.vst3`      | ADDFF263-42F3-34B5-A4A3-018DF0A3C03B |
AUHostingServiceXPC_arrow killed; Sean must relaunch Logic. What changes
for him: a NEW insert now starts with KEEP VIBRATO off (the approved
sound); his saved sessions load exactly as before (bit-identical); the
front and ADVANCED views carry the CORRECTING / NOT CORRECTING line with
the live applied shift; REF shows the applied reference while on auto.
