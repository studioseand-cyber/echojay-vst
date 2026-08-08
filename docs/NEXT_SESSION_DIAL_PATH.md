# Next session: the dial path

Written 8 Aug 2026 at the end of a long day. Four items, in order, all agreed.
Everything here is **measured**, not suspected — the measurement is quoted with
each item so nobody re-derives it.

## The state to start from

- Corpus is live: **1,108 maps, 1,106 proposals**, verified against the server.
- A **2.99.99 gate-test build** is installed (`CMakeLists.txt` in
  `echojay-vst-v200` is modified and **uncommitted** — `git status` is the
  reminder). It opens all three server gates for that one binary. Revert with
  `git checkout -- CMakeLists.txt` and a rebuild, ~13 min.
- The chain **runs end to end**: `EJDialable` fires, `fp=e83a8cb3aa24`,
  `dialable=true`, `applySettings` executes. Nothing is written yet, for the
  reason in item 1.
- `docs/TREES.md` says which checkout ships what. Read it before editing
  anything that reaches a binary.

---

## 1. Key `index:mapped-controls` by fingerprint  ← THE CAUSE

`registerMappedName` writes the registry under the **plugin name**
(`params-lib.js:523`) while maps are keyed by **fingerprint**. Two maps of one
plugin therefore share one entry and the later write wins.

```
SSL Blitzer VST3  ae22177d   0 params, 16 controls  -> Attack, Ratio, Drive, Input Gain, ...
SSL Blitzer AU    e83a8cb3   9 params,  7 controls  -> those nine are Tier 1 PARAMS
```

The VST3 twin, uploaded 8 Aug, overwrote the AU entry. The exposure told the
model *"SSL Blitzer has a control called Ratio"*; it emitted
`controls: {"Ratio": …}`; the client looked in `map.controls`, correctly found
nothing, and wrote nothing. **The model did as it was told.**

**15 plugins affected today** — the hand-mapped maps that gained a campaign
twin: FG-X 2, UAD Precision Limiter, Korg SDD-3000, SPL Transient Designer,
SSL Fusion HF, Manley Massive Passive, Softube Vintage Amp Room, AVOX WARM,
SSL Blitzer (9 colliding names), UAD elysia alpha mix, and five more. **Every
future twin adds one.**

Feasible because the client already sends a per-slot `fp`
(`PluginEditor.cpp:19129`). Touches the write site and the three read sites
(`chat.js:2528`, `:2576`, `:2665`).

> **STATE WHAT HAPPENS TO AN ENTRY WITH NO FP DURING THE TRANSITION.** Do not
> assume there are none. The 22 pre-campaign maps predate several conventions,
> the crowd/seed paths wrote entries too, and a name-keyed entry with no
> resolvable fingerprint must have a defined fate — served, ignored, or
> deleted — chosen deliberately and written down. A backfill that silently
> drops what it cannot key is how a plugin loses its exposure without anyone
> noticing.

## 2. Exclude a map's own Tier 1 display names from its controls exposure

Two lines in `buildControlsRegistryEntry`. Correct independently of item 1: a
name reachable by semantic should not also be offered as a Tier 2 control,
because that is the key confusion that produced this.

**It does NOT fix item 1 on its own, and this was checked rather than assumed:**
the VST3 Blitzer map has **zero params**, so "exclude its own param names"
excludes nothing. `Ratio` stays in its entry and still overwrites the sibling.
Defence in depth, not the fix.

## 3. Gate the result line on `dialStatus`, not on the model

Observed: `applySettings` returned `manual`, `dialStatus` would have settled
`unusableMap`, and the user read *"Done — Attack and Ratio updated."*

The prompt rule already exists and is emphatic (`chat.js:161` — *"the result
field must never claim settings were applied"*), so **more prompt text is not
the fix**. The app already computes the truth: `dialStatus` and
`dialAppliedCount`, in the same function that produces the sentence.

Compose the result line from the verdict. Use the model's `result` string only
when `dialStatus == applied`; render `partial` and `unusableMap` from what
`applySettings` actually reported. Client-side, in `ChainHost`.

**The layer that knows is downstream of the layer that speaks.** The model is
not a reliable narrator of what landed and does not need to be.

## 4. Fix the telemetry itself

`settingsEmitted` and `settingsDropped` exist to explain exactly this class of
turn, and were **null on 1,596 of 1,597 events on the day it happened** — the
single populated one was a generate turn eight hours earlier. Zero events
mention Blitzer.

The edit branch (`chat.js:3067`) does set both fields, so something between
that branch and `logClassification` is not firing on the edit path.

> This is worse than not having the fields. **Their emptiness reads as "nothing
> was dropped"**, and that is what I would have reported if I had not checked
> whether they fire at all. Same class as the empty captures: a green run whose
> output is silence. Whatever the fix, it needs a test that asserts the
> PERSISTED entry on an EDIT turn — the `_db.js` whitelist comment says exactly
> this two lines above the field that was dark.

Live evidence meanwhile: `console.log('[chain-settings-edit]', …)` is not gated
by the redis whitelist, so `vercel logs` filtered to `chain-settings-edit`
carries `emitted` and `dropped` for the Blitzer turns.

---

## Still open, not blocking

- **Where `Attack` went.** One key reached `applySettings`, not two. The model's
  block, the parse, or the validator — undetermined, and item 4 is why.
- **The 91-fingerprint prefetch.** `stored/updated 0` means zero map objects
  came back (`ChainHost.cpp:1690`). Correct if none of the 91 is in the corpus,
  broken if any is. Needs the fp list from the `EJParamMaps: prefetching` log
  line, then a one-step intersection against the 1,108.
- **Promotion + halt** (`HALT_DESIGN.md`), the original next item. 4,173
  model-proposed semantics are stored but **not serving** — `applySettings`
  reads `map.params`, and only 34 maps have any. Serve-time merge, halt as an
  exclusion from that merge, both decided:
  - collision: human wins; same index silently superseded, different index
    superseded **and recorded as a conflict**;
  - halt is per `(fp, semantic)`, persists across re-derivation, and a later
    claim on a halted semantic escalates rather than auto-serving.
- **Sibling transfer** of human answers across AU/VST3 fingerprints — the 87
  human-verified rows reach one binary each. Refused as a copy-by-index: three
  of the eight pairs differ in `param_count`, so the indices provably do not
  align. Wants a structural rule verified against captures.
