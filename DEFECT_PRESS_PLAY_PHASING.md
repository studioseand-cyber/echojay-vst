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

Nothing is built. Waiting on Sean's soloed answer.
