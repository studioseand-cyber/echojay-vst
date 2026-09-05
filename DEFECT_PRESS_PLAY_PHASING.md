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

### The LOG build, delivered (5 Sep 2026)
  /Users/SeanD/Desktop/EchoJay_LatencyLog_Build/
    EchoJay V2.component            the LOG build, AU arm64 0876789A-AEB6-38F2-8754-66D7DA8F8AA9 (45 MB)
    working/EchoJay V2.component    the WORKING build, AU arm64 63AF3A3F-B080-3CB0-A60A-DF53F20CE371 (45 MB)
    install_log_build.command, restore_working_build.command, README.md
The log strings are present in the LOG binary (6 markers found) and absent
from the working one by construction (the macro is compiled out). The
installed plugin right now is the WORKING build (63AF3A3F), verified.

### Round 50: the log moved to a CONNECTED folder
Sean could not reach ~/Library/Logs (hidden, and empty until a run). The
log now writes /Users/SeanD/echojay-vst/latency-logs/latency.log; the
install script creates the folder, moves any previous log aside as
latency.previous.log so a stale file is never read as a fresh run, and
prints the path. The log build and its scripts live in
/Users/SeanD/echojay-vst/latency-logs/build/ (git-ignored bundles). The
sequence: quit Logic, run install_log_build.command, open Logic, press
play four or five times including once after jumping to a marker, quit
Logic, run restore_working_build.command. Nothing to send.

## 9. THE LOG, READ (round 52, 5 Sep 2026): the borrow alignment budget is the runtime latency change

Sean's capture: /Users/SeanD/echojay-vst/latency-logs/latency.log, 361
lines, 19:17:06 - 19:18:11, six PLAY edges, on the round-50 log build
(the latency path is unchanged since; the log build has since been rebuilt
at HEAD for any re-run).

### What the log settles
  - NOTHING CHANGES LATENCY AT PLAY. Every PLAY edge is followed by 50
    logged blocks with a steady value and rebuildPending 0, e.g.
        19:17:16.226  top: block  1/50  reported 1800  chainTotal 1800  rebuildPending 0  pos 43.937 s
        19:17:29.450  top: block  1/50  reported 1800  chainTotal 1800  rebuildPending 0  pos 79.937 s
    No "chain: latency debounce ARMED", no "onHostedLatencyChanged", no
    "rebuildForLatencyIfChanged" anywhere in 361 lines. The 80 ms debounce
    never fires. THE ROUND-47 PRIME SUSPECT IS EXONERATED.
  - LOGIC SENDS reset() AT PLAY - and at every locate while playing:
        19:17:16.221  top: reset() from the host
        19:17:16.226  top: PLAY started at 43.937 s (block 1024 samples, nonRealtime 0)
        19:17:16.226  chain: transport reset applied to the graph
        19:17:16.227  pitch: transport reset applied (rings cleared)
        19:17:18.651  top: reset() from the host          <- mid-playback (a locate)
        19:17:18.656  pitch: transport reset applied (rings cleared)
    THE ROUND-48 OPEN QUESTION IS CLOSED: the reset fix fires correctly on
    every play. (A mid-playback locate clears the rings too - the host asked
    for it; the output is silent for the reported latency and re-locks.)
  - NO prepareToPlay at any play edge: once at load only
    (19:17:06.818 top / 06.970 pitch, block 1024).
  - THE ONLY ANOMALY IN 361 LINES:
        19:17:39.496  setLatencySamples  PluginProcessor (site 3)  old   1800  new  18184  CHANGED -> host notified
    18184 - 1800 = 16384 = kBorrowAlignBudgetFrames (PluginProcessor.h:443).
    "site 3" is setBorrowBudgetActive(true): "EJCtx: alignment budget ON
    (capable Link present) - PDC re-runs once". It fired 33 s after load,
    10 s INTO A PLAY (run 4, 19:17:29 - 19:18:02). Every PLAY after it
    reports 18184 against chainTotal 1800.

### The question ruled to be answered by measurement: does the plugin actually delay by 18184 while the budget is on?
tools/latency_impulse_test (an impulse through the top-level EchoJay V2
processor, empty chain, 48 kHz / 1024, linked against the SHIPPING
SharedCode library from build-release; the app-support folder snapshotted
before and after):
    budget OFF (fresh, no capable Link)          reported      0   impulse out at      0   MATCH
    budget ON (capable Link seen), same instance reported  16384   impulse out at  16384   MATCH
    budget OFF again                             reported      0   impulse out at      0   MATCH
    budget ON before prepare (re-prepared)       reported  16384   impulse out at  16384   MATCH
THE REPORT IS HONEST. When the budget is on the passthrough really is
delayed by the full 16384 samples (alignPost_ at the constant budget,
PluginProcessor.cpp ~1202) - 341.3 ms at 48 kHz - so "reported 18184,
chainTotal 1800" is not a disagreement: the top level's own delay sits on
top of the chain's. THE DEFECT IS THAT THE VALUE CHANGES MID-SESSION: at
19:17:39 the passthrough delay AND the report jumped by 341 ms during
live playback, and the host had to redo PDC for the whole project while
the track was 341 ms out of place. Live-only by construction (a bounce
never runs the registry pass mid-render). That is the phasing.
(Harness note, on the record: constructing the top-level processor
rewrote auth.json's mtime (same 474 bytes) and made one network poll -
outside this defect, filed as seen.)

### What engaged it, and whether it can engage on its own
setBorrowBudgetActive has ONE writer: refreshLinkRegistry(), the
registry pass (PluginProcessor.cpp:4604). It is called from the EDITOR:
every 10 editor ticks (PluginEditor.cpp:21336), on a tab switch (11271),
and on two apply paths. The registry is the MACHINE-WIDE directory
~/Library/Application Support/EchoJay/link (LinkShm::resolveDir): a
capable EchoJay Link in ANY project or host on the machine, publishing
its sidecar, flips the budget in every EchoJay V2 whose window is open.
YES, IT CAN ENGAGE WITHOUT THE USER DOING ANYTHING on this plugin - Sean
had the window open (his screenshots), a capable Link's sidecar was
listed or freshly published, and the pass flipped it during his fourth
play. A latency change that fires on its own.

### THE PROPOSAL (not built - waits for the ruling)
The report is honest, so the budget is not removed from it. The fix is
that THE VALUE NEVER MOVES WHILE THE HOST IS RUNNING AUDIO:
  1. The budget decision is taken at prepareToPlay, and re-taken only
     while the transport is STOPPED. A registry change seen during
     playback is QUEUED and applied at the next STOP edge (or the next
     prepare), report and delay together on the same block. Logic re-runs
     PDC at the next play, which is what the design's "re-runs PDC once"
     always meant.
  2. SCOPE: only capable Links in THIS host process count for the budget
     (the registry rows carry the host identity the prompt resolver
     already reads). A Link in another project or DAW cannot move this
     plugin's latency.
  3. NOT proposed: a fixed pitch-device latency across voice types. The
     log shows its report never moved (refreshLatency unchanged at every
     play), so there is nothing to fix there; the number for the record
     is 3 periods of the voice floor (1800 samples = 37.5 ms at alto/tenor).
  Cost: a capable Link appearing during playback takes effect after the
  next stop; users without Links pay no latency; users with Links pay the
  341 ms they pay today, from the first play after it is seen.
  The alternative the reviewer named - report the budget from the start,
  always - costs 341 ms for every user with no Link and needs its own
  ruling; it is not what this proposes.

### THE BAR (for the ruling; every leg measured before the install is described)
  a. THE REPORTED LATENCY NEVER CHANGES AFTER PREPARE, IN ANY STATE
     INCLUDING BORROW: suite leg - with transportPlaying true, a registry
     change that would flip the budget leaves getLatencySamples() unchanged
     and the passthrough delay unchanged; at the STOP edge both change
     together; the impulse test shows report == delay before and after.
  b. IMPULSE TEST: tools/latency_impulse_test MATCH at both values, off,
     on, off again, on-before-prepare (today's four legs), plus the
     queued-then-applied case.
  c. NO REGRESSION TO BORROW: tools/borrowhost_test's gate passes; the
     engage/release path still never touches the report; the injection
     pad and the passthrough pad flip on the same block.
  d. SCOPE: a registry row from a foreign host pid does not flip the
     budget; a row from this pid does (suite).
  e. SEAN'S LOG RE-RUN on the fixed build, window open, with his Link
     present: no setLatencySamples line during any PLAY; every "reported /
     chainTotal" pair constant across all 50 blocks of every run; the one
     allowed change lands between a STOP and the next PLAY.

## 10. ROUND 53 (5 Sep 2026): THE FIX BUILT to the ruling - queue-to-next-stop, five conditions, seven legs

### The five conditions, as implemented
  C1 ONE COMMITTED BUDGET. `borrowBudgetActive_` is the delay applied
     (alignPost_ at the constant budget), the value reported
     (reportedBudgetFrames), and the value the borrow path reads (in-context
     engage now requires it). It is written ONLY by commitBorrowBudget(),
     called from exactly two places: prepareToPlay, and a processBlock that
     observed the transport STOPPED. The registry pass writes only
     `borrowBudgetWanted_`, which is inert: not applied, not reported, not
     read by the borrow path (the engage decision reads the committed value,
     so borrowing against an unreported budget cannot happen). When a budget
     commits DOWN, an in-context session drops to the solo fallback (its
     arithmetic assumed the old passthrough delay).
  C2 UNKNOWN = PLAYING. The commit sits inside `if (playHead) if (position)`;
     no play head or no position never reaches it. prepareToPlay re-decides.
  C3 BOTH DIRECTIONS QUEUE: the commit is `active != wanted`, either way.
  C4 SCOPING. The Link's sidecar now carries publisherPid and the host
     identity (pid + process start, ChainHost::getHostIdentity - the DAW, or
     the helper it resolves to). A rack counts only if in-context capable,
     host identity == ours, publisher pid alive (kill(pid,0)); a cached row
     whose publisher died is re-read once (a restarted Link) and otherwise
     ignored. Absent fields (an old Link) never count - fail closed.
     Evaluated EVERY pass (liveness is not cacheable).
  C5 THE EDITOR DOES NOT DECIDE AUDIO LATENCY. refreshLinkRegistry() runs on
     the processor's own 1 Hz timer; the editor's periodic call is gone (its
     tab-switch/apply refreshes remain, for the list). The installer now
     installs the LINK with the main (an old Link would never count).

### The seven legs
L3  REPORT AND DELAY MOVE TOGETHER, INCLUDING WHILE PENDING
    tools/latency_impulse_test, top-level EchoJay V2, empty chain, 48 kHz /
    1024, a fake play head the test drives, latency notifications counted
    through the processor's listener (what the host receives):
    | state | reported | impulse out at | notifications |
    |---|---|---|---|
    | (a) budget off, fresh, stopped | 0 | 0 | 0 after prepare |
    | (b) wanted ON during PLAYBACK: pending | 0 | 0 | 0 |
    | (b') wanted ON, transport UNKNOWN (C2) | 0 | 0 | 0 |
    | (c) committed at STOP | 16384 | 16384 | exactly 1 |
    | play again | 16384 | - | 0 further |
    | (d) wanted OFF during PLAYBACK: pending-down (C3) | 16384 | 16384 | 0 |
    | (e) committed-down at STOP | 0 | 0 | exactly 1 |
    | wanted ON before prepare -> committed at prepare | 16384 | 16384 | - |
    (b) and (d) are the half-engagement legs: nothing moved. ALL PASS.
L7  QUIET SESSION: fresh instance, no Link: 1200 blocks of alternating
    play/stop, reported stays 0 (= chain total), ZERO notifications after
    prepare. PASS.
L4  SCOPING (the decision function, driven directly): POSITIVE CONTROL an
    in-process capable live row COUNTS; a row from a different host
    process does not; same pid with a different process start (recycled
    pid) does not; a DEAD publisher does not; an old sidecar with no
    fields never does; a live in-process row that is not in-context
    capable does not. Liveness probe: own pid alive, pid 999999 dead. PASS.
L6  NO REGRESSION: tools/pitch_mode_test 196 PASS / 0 FAIL (unchanged).
    tools/borrowhost_test, adapted to the committed budget (wanted +
    prepare where it prepared, wanted + commit where it did not) and
    linked against the SHIPPING library (build-release): PASS - 311 ok,
    the one FAIL is its planted negative control; "no capable Link: no
    alignment budget is carried [0]", "a capable Link present: the budget
    is carried [16384]", "in-context OK at engage", "the last capable Link
    leaving withdraws the budget", "IN-CONTEXT output stays bounded over
    60 real blocks", "a chain-latency jump produces NO mix discontinuity
    beyond the ramp".
L1, L2, L5 NEED SEAN'S MACHINE (a real Logic transport and a real Link) -
    the log build at HEAD is in latency-logs/build. POSITIVE CONTROL for
    L1 on the pre-fix build: his round-52 capture, the natural occurrence
    of the same mechanism (a capable Link seen mid-playback):
        19:17:39.496  setLatencySamples  PluginProcessor (site 3)  old 1800  new 18184  CHANGED -> host notified
    The procedure (also in latency-logs/build/README.md): Logic quit; run
    install_log_build.command (it installs the LOG main; the Link installed
    is the new one, EE5CF406); open the session with NO EchoJay Link in it
    and the EchoJay window CLOSED (L5); press play; while playing, insert
    an EchoJay Link on another track; keep playing 30 s; stop; play again
    10 s; stop; while playing again, remove the Link; stop. Expected in
    the log: "borrow budget WANTED ON (pending ...)" during the first play,
    ZERO setLatencySamples lines between that PLAY and its STOP; at the
    STOP one "COMMITTED ON at PluginProcessor commit at STOPPED block" with
    old 1800 new 18184; the next play's 50 blocks constant at 18184; the
    removal: WANTED OFF pending, committed at the following stop.

### Installed (5 Sep 2026), ~/Library only, via tools/install_local.sh build-release
  | plugin | arm64 UUID |
  |---|---|
  | AU   `EchoJay V2.component`   | 7F0618CD-0ED5-3E98-9C4F-3E447E10B72B |
  | VST3 `EchoJay V2.vst3`        | F58B37CA-8490-39AE-9EF1-4BE706076E43 |
  | AU   `EchoJay Link.component` | EE5CF406-57E2-325E-93E4-8D3F0616962B |
  | VST3 `EchoJay Link.vst3`      | 78441F93-E292-3FD7-BD1D-18B6EF960104 |
  | LOG build (latency-logs/build only) | 6E91E383-2713-3385-A42E-8D903B88C372 |
AUHostingServiceXPC_arrow killed; Sean must relaunch Logic. Not touched:
kBorrowAlignBudgetFrames (16384; the 1024 + 15360 split stays on the
eighteen-constant register), no user-facing indicator, no always-on report.
