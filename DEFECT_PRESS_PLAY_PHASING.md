# DEFECT (open, 5 Sep 2026): "at the start when you press play it goes out of alignment for a tiny bit and phases"

Sean's words, on the installed build (AU arm64 071A2E31). Ruled by the
reviewer as a LATENCY-class symptom (two copies combining slightly
offset), not a pitch one; distinct from the earlier press-play report
(the key going wrong, held in DEFECT_AUTOKEY_PROVENANCE.md).

DISCRIMINATING TEST, pending: does it still phase WITH THE TRACK SOLOED?
  - still phases  -> internal (hypothesis B below)
  - does not      -> host compensation (hypothesis A)
No fix direction is chosen before that answer. This file is the reading
of every latency call site, done meanwhile, so the answer lands on a
prepared map.

## 1. Where the pitch device can tell the host its latency changed

`EedPitchProcessor::refreshLatency()` (EedPitchProcessor.cpp) is the ONLY
path to `setLatencySamples`. It runs:
  - at the end of `prepareToPlay` (memo `latencyVoiceType_` reset to -1 first);
  - on EVERY `processBlock` (audio thread) - but returns immediately unless
    the voice type differs from the memo;
  - immediately from the `low_latency` setter (message thread; memo forced).
It sets `setLatencySamples (shifter().latencySamples())`. JUCE's setter
(juce_AudioProcessor.cpp:415) is a NO-OP unless the value differs, so a
notification reaches the host only when the number actually changes.

The number: `PsolaEngine::latency_ = max (period, lookahead * period)` with
`period = ceil (fs / lowestF0 (voice type))` - recomputed only in
`setLowestF0` / `setLookaheadPeriods`. It does NOT depend on the block
size, so a host re-preparing with a different buffer size at play start
produces the same value and no notification.

So at runtime the reported latency changes only when VOICE TYPE or
LOW_LATENCY (lookahead) changes. Sean's saved session (3 Sep decode):
voice_type 1 (alto/tenor, which is also the schema default), low_latency
0 (also the default). State restore writes defaults then the file, so for
his session the value never changes during load either. (For a file whose
voice type differs from the default it changes ONCE during load: default
first, file's value after. At load, not at play.)

## 2. What the chain host does with a slot latency change

The pitch device is a SLOT in the ChainHost graph (EchoJay V2). The slot
listener (ChainHost.h:1552) forwards ONLY `latencyChanged` to
`onHostedLatencyChanged` -> async trigger -> 80 ms debounce (message
thread) -> `rebuildForLatencyIfChanged` (ChainHost.cpp:6590): compares
each slot's current latency with the one the graph was built with and,
only if different, `rebuildGraph()`. That rebuild re-bakes the wet/dry
dry-leg delays and re-mirrors the total into the top-level
`setLatencySamples (lat + reportedBudgetFrames())` (PluginProcessor.cpp).
Its own comment records the audible cost: "the new dry-leg delay buffers
start empty, so a partially wet slot loses its dry component for the
plugin's latency, once". THIS IS THE ONE MECHANISM IN THE PLUGIN THAT CAN
PRODUCE A MOMENTARY MISALIGNMENT AT RUNTIME - and it fires only on an
actual latency change, which section 1 says does not happen at play
start for his session.

## 3. Is anything blended against a delayed dry at start-up?

  - The shifter emits from `base = write_ - n - latency_`
    (EedPsolaEngine.h:626): the output is EXACTLY `latency_` behind the
    input from the first sample; after `prepare` the rings are zero, so
    the first 38 ms are silence, aligned. There is no start-up passthrough
    at a different delay. `emitDry` and `emitMixed` both read at that base.
  - The device's MIX is at 100: the mix stage does not blend.
  - The chain's master dry ring (ChainHost.cpp:950) is bypassed when
    fully wet; the per-slot `SlotWetBlend` early-outs at 100% slot wet.
    If Sean's SLOT wet knob is below 100, the dry leg is a graph delay
    node, aligned by the built latency; it would misalign only across a
    rebuild (section 2).

## 4. The internal candidate that survives SOLO: stale rings across a transport reposition

JUCE's AU wrapper maps AudioUnitReset to `AudioProcessor::reset()`
(juce_audio_plugin_client_AU_1.mm:261). NO LAYER OVERRIDES `reset()`:
not the pitch device, not EedDeviceProcessor, not ChainHost's slot
wrappers, not the top-level processor. The pitch device has no transport
awareness at all (no play-head use). Consequence: on stop / locate /
play, the shifter's rings (input, f0, target, shift, decision) still hold
the tail of wherever playback last was. For the first `latency_` (38 ms)
plus the per-hop lag after play starts, the wet output is synthesised
from STALE decisions applied to NEW audio, and the input ring's old tail
is what the grains straddle. That is a wrong-for-a-tiny-bit start that
needs no host involvement and would still be there soloed. Whether Logic
actually sends Reset at play start is NOT verified here; if it does, we
ignore it, and if it does not, the stale content is there regardless.

## 5. What each answer to the solo test leads to

  A. NOT soloed-phasing (host side): the plugin's report must have changed
     around play start. Sections 1-2 find no changer for his session;
     the next step is to log `setLatencySamples` calls with timestamps in
     a debug build and have him press play, before touching anything.
  B. STILL phases soloed (internal): section 4 is the candidate. The fix
     shape is a `reset()` override that clears the co-timed rings and the
     delay line (and the corrector's hold state), plus a transport-
     discontinuity guard if Logic does not send Reset. THE BAR before any
     build: a render with a mid-file restart WITHOUT clearing (positive
     control - must show the stale-start artefact) vs WITH clearing;
     bit-identity everywhere else; word-start events unchanged.

## 6. RULED (round 48, 5 Sep 2026): BUILD THE reset() OVERRIDE NOW, without waiting for the solo answer

A plugin that does not override reset() and carries synthesis state across
a transport jump is wrong on its own terms; AudioUnitReset exists for
precisely this. Stale ring content synthesised after a locate is a defect
whether or not it is Sean's symptom (the round-21 reasoning: fix what is
broken, let the symptom confirmation arrive separately). The mechanism
also fits his words: grains synthesised from the previous position's ring
content, combining with fresh input across the first 38 ms plus lag, is
what "out of alignment for a tiny bit and phases" sounds like.

THE BAR (committed before the code):
  1. POSITIVE CONTROL: a mid-file restart WITHOUT clearing must show the
     artefact, measurably. If it does not, the mechanism is not reachable
     in the harness and the bar cannot test the fix - say so and stop.
  2. reset() clears the co-timed rings, the corrector's state (curCents_,
     slowCents_, noteRefCents_, pending), the seam state machine, and
     anything else carrying position-dependent content. Every item it
     clears is ENUMERATED with why it qualifies; every item it deliberately
     leaves is enumerated too.
  3. Continuous playback from the top is BIT-IDENTICAL to the current
     build (the saved-state render references from the pre-round-46 binary,
     NEW and OLD takes): the fix must not change anything that was not broken.
  4. The first 150 ms after a locate no longer synthesises from stale
     content - shown against the positive control.
  5. Word-start events and the OLD-take falsifier unchanged (leg 3's
     identity carries them: the continuous renders they are measured on
     are bit-identical).
  6. The KEY state is NOT cleared in reset() - that is the round-20
     stale-key question with its own ruling and its own hysteresis design.
     Kept separate; the commit says so.
The solo answer, when it comes, does not invalidate this fix: "does not
phase soloed" would mean there is ALSO a host-compensation issue, and the
next step there is the timestamped latency logging (section 5A).

METHOD for legs 1 and 4 (the harness, in tools/pitch_mode_test, gated by
the material like every render block): FRESH = a fresh instance rendering
the take from position P2. LOCATE-STALE = an instance that rendered from 0
to P1 and then continued at P2 with nothing cleared (what a host does
today). LOCATE-RESET = the same with reset() between. Both compared with
FRESH sample by sample from P2: differing-sample count and the RMS of the
difference over the first 150 ms (and where the difference ends). The
positive control is LOCATE-STALE differing in the first 150 ms; the fix is
LOCATE-RESET bit-identical to FRESH (reset == fresh by construction - the
same functions prepare() calls).

## 7. BUILT (round 48, 5 Sep 2026) - every leg measured

### The mechanism, made to happen and then removed (legs 1 and 4)
Harness in the suite (tools/pitch_mode_test, "TRANSPORT RESET"): the take
(sourceNEW, D minor by hand) played 0 -> 4.00 s, then a LOCATE to 5.79 s
and 0.99 s rendered; compared sample by sample with FRESH, a fresh
instance rendering from 5.79 s.
  | leg | first 150 ms | after 150 ms | last differing sample |
  |---|---|---|---|
  | LOCATE-STALE (today's build: nothing cleared) | 5712 / 7200 samples differ; diff RMS 132.6 % of the signal RMS | diff RMS 64.5 % | 992 ms (the whole render) |
  | LOCATE-RESET (with reset()) | 0 / 7200 | 0.0 % | none - 0 samples differ in all |
POSITIVE CONTROL: PASS - the stale start is reachable in the harness and
large (the first 38 ms are the previous position's ring tail where FRESH
has silence, and the grains after it are placed from stale epochs, so the
waveform never re-converges within the second even where the pitch does).
THE FIX: PASS - after reset() the locate renders BIT-IDENTICAL to a fresh
instance from the same position, first 150 ms and all of it: "reset ==
fresh by construction" is measured, not argued.

### What reset() clears, and what it leaves (leg 2, enumerated; the same list is in the code beside applyReset())
CLEARED - each carries the previous position's content:
  - detector (PitchEngine::reset, new): input ring, frame/difference
    scratch, decimation and hop phase, sweep state, f0 history and
    continuity memory (hist_, lastF0_, hopsSinceVoiced_), anti-alias
    filter states, hop-event list, published reading; config re-applied on
    the next block exactly as prepare() does.
  - shifter (PsolaEngine::reset, completed): the co-timed rings - input,
    f0, target, shift, decision, and slowRing_ (was MISSED: the lag-
    compensated slow reference); write/emit/place heads; splice and seam
    state machines (drift, fade, ramp, bridge, method mix); LPC coefficient
    ring and synthesis state; the drift-bleed gate bleedGate_ (was missed:
    a 100 ms pole over recent shift); curShift_ (was missed).
  - corrector (PitchCorrect::reset, completed): curCents_, slowCents_,
    noteRefCents_, the pending note and its median buffer, the resume/seed
    medians and judgement state, the applied-shift pole and its snap latch
    (shiftSm_/shiftCents_/shiftSnap_ - were missed), the measured vibrato
    depth, the slow track's age, the last-hop readbacks.
  - the jump gate's last-good f0; the block-to-block hold (lastTarget_,
    lastShift_, lastHopF0_, lastHopVoiced_, lastCorrecting_) - the "hold
    the last target through a gap" state, which would otherwise hold the
    previous position's target into the new one; the ribbon's decimation
    phase (display).
LEFT, deliberately:
  - every parameter; the latency memo (configuration);
  - THE KEY STATE: keyAuto_, the auto-key memos (lastAutoRoot_,
    lastAutoMinor_, lastAutoFellBack_, lastAutoTuning_), the corrector's
    key/scale/degrees and the scale cross-fade. Round 20's stale-key
    question has its own ruling and its own hysteresis design; NOT bundled;
  - the retune trace ring (an editor readout); the octave-guard
    statistics (reset_stats' domain); resumeReanchors_ (a statistic);
  - the humanize path holds no position content beyond the above.
"Was missed" marks state that the pre-existing reset() functions (written
for prepare()) did not cover: a reset that had merely been forwarded
without completing them would have left the slow reference, the bleed
gate and the shift pole stale - and LOCATE-RESET would not have been
bit-identical to FRESH.

### How the reset reaches the device
JUCE maps AudioUnitReset to AudioProcessor::reset(). EchoJay V2 and Link
now override it (PluginProcessor.h, LinkProcessor.h) -> ChainHost::
requestReset() (a flag, any thread) -> at the top of the next
ChainHost::process() on the audio thread, graph_->reset(), which calls
reset() on every slot's processor (third-party plugins get the host
semantics they expect). The pitch device's reset() is itself a flag,
applied at the top of its next processBlock (before the bypass path), so
a reset from any thread never races the block in flight. Caveat recorded:
JUCE's graph reset iterates the node list without a lock, the same
contract the chain already relies on for its topology (processors are
suspended across structural ops); a reset landing in the same instant as
a structural edit is the residual exposure, not new to this change.

### Continuous playback unchanged (legs 3 and 5)
The saved-state render-identity harness against the references produced
by the pre-round-46 binary: NEW take 4/4 bit-identical, OLD take (dry.wav)
4/4 bit-identical (0 samples differ). Word-start events and the OLD-take
falsifier are measured on those continuous renders, so they are unchanged
by identity. Suite: 182 PASS / 0 FAIL (180 + the two reset checks).

### Whether Logic sends Reset at play
Still not verified here. If it does, the fix applies at every play/locate.
If it does not, the stale start remains reachable in Logic and the next
step is a transport-discontinuity guard from the play head (a position
jump larger than a block -> the same applyReset()), behind the same bar.
Sean's soloed answer and his listen on this build decide which.

### Installed (5 Sep 2026), ~/Library only, via tools/install_local.sh build-release
  | plugin | arm64 UUID |
  |---|---|
  | AU   `EchoJay V2.component`   | 444376C2-3068-31F7-87FA-240DBB796588 |
  | VST3 `EchoJay V2.vst3`        | 52799A77-B930-3E0E-A90E-84253B2038DB |
  | AU   `EchoJay Link.component` | A8C23A25-5981-3278-9211-FFC1F558928A |
AUHostingServiceXPC_arrow killed after the install; Sean must relaunch
Logic. What to listen for: press play from a marker after having played
elsewhere - the first moment should now be the same as playing from that
marker fresh. Continuous playback is bit-identical to before.

## 8. Round 49 (5 Sep 2026): LIVE-ONLY. His offline Logic bounce is clean.

Sean's report on AU 444376C2: the phasing happens in live playback; an
offline bounce of the same session is clean. Ruled the better discriminator
than the solo test (not asked again): the engine's output is CORRECT - an
offline render proves it - and the fault is in LIVE PLAYBACK ALIGNMENT.

RECORDED: the round-48 reset fix is correct on its own terms and is KEPT,
and it is NOT this defect - if stale rings were the cause, the offline
bounce would have been dirty too (a bounce locates and starts exactly as
live playback does, through the same rings).

What points where: the one live-only mechanism the round-47 reading found
is the chain host rebuilding its graph on a slot latency change after an
80 ms DEBOUNCE (section 2). 80 ms sits inside "the first second or so",
and an offline bounce never exercises that path. Also checked, as asked:
NOTHING in Source reads isNonRealtime()/setNonRealtime() - there is no
realtime-only branch in the device or the chain; if the split is ours it
comes from timing (a report or a rebuild landing during live playback),
not from a code path that differs offline.

### The timestamped log, built (tools/latency_log_build/, a DEBUG build)
`EJ_LATENCY_LOG=1` (CMake option, the build-latencylog directory; OFF in
every other build, where every macro is a no-op). Source/EedLatencyLog.h
writes ~/Library/Logs/EchoJay/latency.log - wall clock, ms since open,
thread - for:
  - EVERY setLatencySamples call, at every site in the code base (the pitch
    device's refreshLatency; the top-level's onChainChanged, prepareToPlay
    and its third site; Link; compressor, exciter, gate, expander, limiter,
    saturation): caller, old value, new value, changed-or-not (JUCE
    notifies the host only on a change);
  - the chain host: onHostedLatencyChanged (a slot reported), the debounce
    ARMED / re-armed / FIRED, rebuildForLatencyIfChanged's verdict with the
    slot and old -> new, every rebuildGraph, prepare, the transport reset;
  - the top level: PLAY/STOP edges with position, block size and
    isNonRealtime(); the FIRST 50 BLOCKS after play starts - block index,
    the latency the host has been told, the chain's total, whether a
    rebuild is pending, position; prepareToPlay on every layer; reset().
Handed to Sean SEPARATELY (Desktop/EchoJay_LatencyLog_Build/): the log
component, a copy of the working build, `install_log_build.command`,
`restore_working_build.command`, and a README - so a logging build is not
what he works in. He presses play four or five times and sends the file.

### What the log decides
  - A latency report or a rebuild inside the first second after play: THAT
    is the defect, and the fix follows from WHICH (a report -> make it not
    change at runtime; a rebuild -> the debounce path).
  - Nothing fires: the next suspect is Logic's own PDC settling, and the
    answer may be to report a FIXED latency that never changes at runtime
    (the top level already adds a fixed alignment budget; the chain total
    is what moves).
