# DEFECT: applyOne's readback reads the pre-write display on bridged AUs, and reverts correct writes

**Filed:** 31 Jul 2026, from the ejmap M6 investigation (feat/ejmap `a8e1cf9`)
**Status:** REPORTED, NOT FIXED. Plugin-side release decision for v2.
**Severity:** live in shipping EchoJay at dial time; invisible by construction (see §2)

---

## The defect in one paragraph

`applyOne` (Source/EchoJayParamApply.h) writes a parameter and immediately reads
`getCurrentValueAsText()` in the same synchronous call stack. On a **bridged AU**
(AUv2 without arm64, hosted through AUHostingServiceXPC), that read returns the
**pre-write display**: the XPC value event that would freshen it is delivered by
the message loop, and no runloop iteration can occur between the write and the
read because they are one stack frame. The readback then compares the stale text
against the requested value, concludes the write landed wrong, and **reverts a
write that was in fact correct**.

Measured on Waves API-2500 (m), 3 of 3 attempts:

```
asked "Soft" -> wrote the label's norm -> readback read "Hard"   <- the PRE-write state
-> reverted ("value restored")
post-revert probe with a pumped settle: display "Hard", norm 0   <- consistent with the
                                                                    map's own labels: the
                                                                    write had been CORRECT
```

## 1. The dial-time path (from code, not analogy)

- `ChainHost::applyStructuredIfReady` (ChainHost.cpp:1659)
- → `ChainHost::applyStructuredSettings` (:1302)
- → `echojay::applySettings` (:1317) → `applyOne` → immediate readback

Every call site is message-thread: `completeLoad` (:1446),
`setSlotStructuredSettings` (:1460), and the map-fetch re-eval paths (:1530,
:1574). The context is identical to the harness where the defect was measured.

## 2. What the user sees: a map gap costume, never an error

The false mismatch sets `readbackMismatch`, reverts the parameter, and the
semantic joins `dialManual` (ChainHost.cpp:1755+). Surfaced as
(PluginEditor.cpp:13780+):

- Some keys survive → slot reads **partial**: "Applied automatically" for the
  survivors, "(threshold, ratio by hand)" for the reverted. **Looks exactly
  like the map not covering those controls.**
- All keys revert → **unusableMap**. Looks like a bad map.
- The true reason (`readback_mismatch`) goes only to `logDialMiss` →
  `events.jsonl`. Never to the user.

**Telemetry note:** `~/Library/EchoJay/events.jsonl` does not exist on the
development machine — the event log shipped 26 Jul and has never captured a
dial session here. Whether it is running on ANY machine is worth checking
separately; if it is, `readback_mismatch` counts against bridged-AU products
are the historical footprint of this defect.

## 3. Population

Architecture census (per-component Mach-O archs, 31 Jul 2026):
**622 of 1,305 AU components are bridged (47.7%) — Waves 604**, iZotope 3,
Antares 2, Plugin Alliance 2, Dolby 2, oeksound 2 (soothe2, spiff), NI 2,
Cymatics 1. VST3 is unaffected (x86-only VST3 cannot load at all). Native AU
verified unaffected (M4 write-back verifies passed 3/3 in-process).

Exact overlap with the dialable-product list requires the server's dialable
flags; materially this is **every dialable Waves product loaded as AU**, plus
soothe2, spiff, Trash 2.

## 4. Deterministic per attempt, intermittent in disguise

The stale read is not a race the plugin sometimes wins: no dispatch can occur
mid-stack, so the read is structurally pre-write. What varies is only WHICH
stale text is read (the XPC pipeline depth wobbles; two runs read different
stale labels). The hiding conditions are conditional, not temporal:

- **setread-method map entries are immune** (norm round-trip only; the norm
  cache updates synchronously). Post-27-Jul maps are heavily setread.
- A request equal to the current value passes coincidentally.
- Stale text that fails to parse → "applied (read-back unparseable)", no revert.

Fires on: bridged AU + display-verified entry (gettext anchored/linear, or
mode labels) + a value actually changing. Per-plugin method mix makes it look
plugin-specific. Zero clean bug reports is fully explained.

## Notes for whoever picks this up (from the filing review)

1. **Scope the fix to the bridged path.** Native AU and VST3 are unaffected;
   a settle-before-read applied to every dial changes timing for the 53% that
   already work. Gate it on the instance actually being bridged.

2. **Question whether the readback should revert at all.** The readback exists
   to catch a write that did not take; here it catches a READ that did not
   refresh, and its response is to undo correct work. A mismatch that
   REPORTED without reverting would have surfaced this defect immediately
   instead of wearing the map-gap costume for months. Consider: report-only
   mismatch (carry the caveat on the card), or verify-then-revert only after
   a refreshed read confirms the miss.

3. **A single pumped settle interval is NOT sufficient.** Measured: with one
   `runDispatchLoopUntil(15)` between write and read, two runs disagreed
   about which end of a three-position switch was which — the bridge is a lag
   pipeline. The working reference is ejmap's stable-read loop
   (tools/ejmap/Source/MainComponent.h, spotCheck hook): read until two
   consecutive reads agree, bounded ~300 ms. Whatever ships must meet that
   bar or it trades a deterministic bug for a flaky one.

## Reproduction

```
# from the ejmap worktree, feat/ejmap >= a8e1cf9:
./build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app/Contents/MacOS/ejmap \
  --ledger-root /tmp/repro --selftest-controls 'AudioUnit:Effects/aufx,APCM,ksWV' \
  'Knee' 'Soft' 'Thrust;Analog;Makeup'
# the FINDING line prints applyOne's verdict on the bridge:
#   FINDING: applyOne verdict on the bridge: REVERTED (stale readback) - asked "Soft",
#            plugin shows "Hard", value restored
# while the map-truth assertions (stable set-then-read) pass against the same map.
```

ejmap's own maps are unaffected as artifacts: labels and anchors are
set-read-verified truth. Only the shipping consumer's verification step
mis-reads them on the bridge.
