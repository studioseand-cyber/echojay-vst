# C2: the Match host boundary
### A, 27 Oct 2026, against `feat/08c` @ `cea66b4e`. The contract K codes against; A0 part 1.

The Match tab becomes `EJMatchTab.*` and talks to the rest of the plugin through **one object**, `MatchHost`. Nothing else in the editor is reachable from it, and nothing in it is reachable from the editor except through the tab's own public surface. The line-level fence for the file both of us edit is in `~/Desktop/FOR_REVIEW_22oct/CONTRACT_PLUGINEDITOR_REGIONS_2026-10-27.md`; this document fixes the interface, the file ownership, the `match_report` payload and the hop accumulator.

**Written against C4 revision 2** (`CONTRACT_MATCH_ATTRIBUTION_2026-10-27.md`, 27 Oct late), and reconciled against it: rev 2's §2, §3, §6 and §7 carry A's answers from §6 and §7 below, and its three open questions (§8.5 the bus tap, §8.6 one name for the share, §8.7 the card's fields) are answered here.

**Status of each part:** the interface and the payload are fixed by this document. The extraction (A0 part 3) and the `k-base-1` tag (part 4) are Sean-approved for 28 Oct, after round 15/16's gate, with a gate of their own. The hop accumulator (§5) is A's to build and is queued behind that.

---

## 1. `MatchHost`, the whole of it

A pure-virtual interface implemented by `EchoJayEditor`. Every call is **message thread only**. `K` calls; `K` never reaches past it.

```cpp
struct MatchHost
{
    virtual ~MatchHost() = default;

    // ---- what there is to compare -------------------------------------------------------------
    /** The capture the Match page is reading. Includes `channels`, each a ChannelMeterData with its
        own uid, meterData, thumbnail and framesReceived - which is how attribution gets per-Link
        numbers without the tab knowing what a Link is. Null when nothing has been captured. */
    virtual const CaptureSnapshot* latestCapture() const = 0;
    /** The SECOND capture, taken after the plan was applied, for the "after" curve (K6's closed
        loop). Null until a verify capture exists. Same type on purpose: one window pass, two inputs. */
    virtual const CaptureSnapshot* verifyCapture() const = 0;
    /** The reference the page is comparing against, or null. */
    virtual const ReferenceResult* selectedReference() const = 0;   ///< MEASURED, not filed - below
    /** Every Link this instance can see: uid, name, role, caps, framesReceived. The tab uses it for
        attribution rows and for naming a track in a sentence; it never addresses a Link itself. */
    virtual std::vector<MatchLink> liveLinks() const = 0;

    // ---- the two slots, BY NAME ---------------------------------------------------------------
    /** MATCH_SCREEN_CONTRACT §2B: the page's pickers write the slots by NAME, never through
        refBarIsTop(). The host owns what a pick does to the rest of the plugin - ending codec mode,
        stopping and restarting the stream, redefining AI Compare's subject, announcing the
        displacement - and answers false if it refused, with `why` for the page to show. */
    virtual bool pickMixSlot       (const juce::String& name, juce::String& why) = 0;
    virtual bool pickReferenceSlot (const juce::String& name, juce::String& why) = 0;
    /** The reference browser, as the page opens it today. */
    virtual void openReferenceBrowser (bool forReferenceSide) = 0;

    // ---- listening ----------------------------------------------------------------------------
    /** The dual stream: which side is audible. Today's toggleCompareAudible. */
    virtual void setAudible (MatchRefSide side) = 0;
    /** ONE SEEK, TWO PAGES: the page's click-to-seek calls the host's, so Compare and Match cannot
        drift apart. `fraction` is of the FILE, never of a pixel width. */
    virtual void seekCompareStream (int slotIdx, float fraction) = 0;

    // ---- the plan, and the chat ---------------------------------------------------------------
    /** Post a card into the Whole session chat. `kind` is "match_report" for §4's payload. */
    virtual void postToSessionChat (const juce::String& kind, const juce::var& payload) = 0;
    /** Make a plan: hand the match block to A's draftPlan. The callback carries the planId, or a
        sentence saying why not. One press, per the September contract. */
    virtual void draftPlan (const juce::var& matchBlock,
                            std::function<void (bool ok, juce::String planId,
                                                juce::String why)> cb) = 0;
    /** ...and open the Whole session chat at that plan, which is where Make a plan ends. */
    virtual void openSessionChatAt (const juce::String& planId) = 0;

    // ---- which build this is ------------------------------------------------------------------
    virtual bool isV2() const = 0;                       ///< v1 never offers "do it all"
    virtual bool agentOn() const = 0;                    ///< agent mode, for the walk-through branch
    virtual bool hasCap (const juce::String& cap) const = 0;
    virtual juce::String pluginVersion() const = 0;      ///< MA5 pins against it
};
```

**The names are the code's, not prose** (Sean's point 14, 28 Oct): the side is **`MatchRefSide`** with `Top`/`Bottom` (`EJMatchPage.h`), one of the two types this document used to invent a name for.

**The reference is a `ReferenceResult`, not a `RefEntry`** — corrected on 28 Oct when the re-point made it compile. Two types in the code can answer "which reference", and the difference matters to K's side:

| type | what it is | who writes it |
|---|---|---|
| `echojay::RefEntry` (`EJReferenceIndex.h`) | the **library record**: id, path, folder, availability, and the measurements last written to disk | the reference index, across sessions |
| `ReferenceResult` (`ReferenceAnalyser.h`) | the **measurement made in this session**: `data` (MeterData), `macroBandAccum` + its window, `eqCurve`, the thumbnail, and since A12b `hopFrames`/`hops` | `ReferenceAnalyser`, on analysis |

The Match page compares numbers, so it gets the measured one. It is also the one a Reference slot addresses: `CompareSlotState::index` is an index into `getReferences()`, which is the convention `applyReferenceToSlot` writes and the browser reads — so `selectedReference()` is `matchSlots().ref->index` resolved, never a name match. Two files called `master.wav` is the normal case, and name matching would have picked whichever was added first. `MatchLink` is the one new type and it lives in `EJMatchTab.h`: `{ uid, name, role, caps, framesReceived }`, a projection and deliberately **not** `LinkSlotInfo`, so the tab cannot start addressing Links itself. `MatchSide` also exists in `EJMatchPage.h` but it is a side's *figures*, not which side is audible - which is exactly why `setAudible` takes `MatchRefSide`.

**[A0b, 28 Oct] The list grew, because re-pointing the page's own `owner->` uses is what told us what it actually reaches for.** Twelve more calls, all genuinely the editor's business: `slotDisplayName`, `slotSpectralEvidence`, `toggleComparePlay`, `compareStreamFrac`, `matchFastEngine`, `matchFastFrame`, `matchWavePoints`, `matchRefIsTop`/`setMatchRefIsTop`, `goBackToCompare`, `streamSampleRate`, `compareSlotAudible`. And four things the page used to ask the editor for turned out to be the page's own business, so they moved onto the tab instead: the Match sides, the slot pair, the mix picker, and the *storage* of which side is the reference — the editor still decides that one, because `setRefSubTab`'s Match branch sets it, which is why it is a pair of accessors and not a field.

Every one of those takes or returns a type this header can name, which is why **`CompareSlotState` was lifted out of `EchoJayEditor`** first (`EJCompareSlotState.h`, with a `using` alias in the editor so not one existing call site changed). A boundary that cannot be stated without the other side's header is not a boundary.

**[A0b, the re-point itself — 28 Oct] Four more calls, and two more lifted types.** The 37 `owner->` reaches are now calls on `host`. Doing it found four reaches that were not calls at all but reads of the editor's and the processor's own members, and each is now a question with a name:

| what the page read | what it asks now |
|---|---|
| `owner->matchRefIsTop_` (12 times) | `host->matchRefIsTop()` |
| `owner->processorRef.cmpAudible.load()` | `host->compareSlotAudible (slotIdx)` |
| `owner->processorRef.cmpStream[i].playing.load()` | `host->compareSlotPlaying (slotIdx)` — **bounds-checked**, because the index comes from the page |
| `owner->processorRef.getSampleRate()` | `host->streamSampleRate()` |

That is the part of the boundary that was load-bearing. A page holding an `EchoJayEditor*` could read any processor member through it, and no interface could have said so; the reads above are what it happened to use *today*.

The slot pair and the sides stayed the host's answer rather than moving onto the page, because which slot is the reference is held by the editor (`setRefSubTab`'s Match branch sets it) and a second derivation in the page would be a second answer to one question — the exact defect `refBarIsTop()` was. So `matchSlots()` and `buildMatchSides()` are host calls, and their two return types were lifted into `EJMatchTab.h` beside `CompareSlotState`: **`echojay::MatchSides`** and **`echojay::MatchSlotPair`**, aliased in the editor so no call site in either file moved. `openMixPicker()` joins `openReferenceBrowser()`: the left picker offers Live, the session snapshots and the chat captures and **never** a reference, which is half of why the two roles cannot swap slots.

`MatchPanel`'s member is now `echojay::MatchHost* host`, set once in the editor's constructor (`matchPanel_.host = this`). **What the page may ask for is exactly what `EJMatchTab.h` declares**, and anything K needs next is a line A adds there.

**`EJMatchTab.cpp` has one region that is A's, at the end of the file.** The `MatchHost`
implementations (`EchoJayEditor::latestCapture`, `selectedReference`, `liveLinks`, `pickMixSlot`,
`pickReferenceSlot`, `setAudible`, `postToSessionChat`, `draftPlan`, `openSessionChatAt`, `isV2`,
`agentOn`, `hasCap`, `pluginVersion`, `slotSpectralEvidence`, `streamSampleRate`,
`compareSlotPlaying`) are **A's**, and they sit below the banner that says so. They are in this file
deliberately — they exist for the Match tab and they are read beside the code that calls them — but
they are the host's half of the boundary, so the same rule the `PluginEditor.cpp` regions contract
uses applies here: **K owns everything above that banner, A owns everything below it**, and neither
edits the other's region without saying so. A rebase conflict inside one function body is the thing
this split exists to avoid.

**One behaviour did change, and K should know which.** The strips used to read `processorRef.cmpAudible` and compare the index; `compareSlotAudible(slotIdx)` answers from `audibleCompareSlot()`, which applies the three conditions the audio block itself applies — the slot is loaded, it has samples, and its mix gain is above zero. That accessor's own comment says why: *"a display that ignored these would light a button for audio that cannot be heard."* So a side selected as audible whose stream is not loaded, or has stopped at zero, no longer draws as audible. The re-point was meant to change nothing; this one row does, and it is better for it.

**If K needs something that is not here, A adds it.** A missing call is one line of mine; a reach into A's half is a conflict on every rebase. That asymmetry is the whole point of the boundary.

---

## 2. File ownership

**K's:** `EJMatchProposal.h`, `EJCompareFigures.h` (**A applies, K proposes** — it is shared with Compare, and K2 is the round that detours through A), `EJMatchPage.h`, `EJMatchTab.h/.cpp`, the window pass in `ReferenceAnalyser.*`, the new `EJMatchWindow.h`, `EJMatchReport.h`, `EJMatchBlock.h`, `EJMatchAttribution.h`, and the Match rows of `tools/mapfps_test`.

**A's:** everything else, and specifically the tab router, the transport bar, `seekCompareStream`'s implementation, the chat paint loop including the `match_report` branch, the whole live-view and real-editor half, the plan card (U1), the walk-through, the run, M3-lite, the Whole session chat and the executor.

**Capture figures (the thread, not the file):** §4 of the regions contract. The computation is K's, new stored fields are K's through a PR, and anything inside `processBlock` or the audio-thread fill is A's to write from K's specification — which is what §5 below is.

Neither of us edits the other's files without saying so first.

---

## 3. The merge path

As the regions contract §2: `k/match` off `k-base-1`; K's gate is `tools/mapfps_test` plus a plugin build on her Mac (never `HEAVY.lock` — that is this Mac's); rebase Monday and Thursday; one small PR per round; A merges at A's next gate and runs the full gate on the merge; failures go back with the rows. Installing for a host check is §5 of that document.

---

## 4. The `match_report` card's payload

K's `EJMatchReport.h` is **pure**: it takes the proposal structs and returns layout — rects, strings and hit zones — and it is built **from the proposal, never from the model's reply**. A's paint loop draws it with **painted zones, not overlay buttons**, because `kMaxWavePlayBtns` is 8 and a report can hold more hits than that.

The payload A stores on the chat row and hands back to K's layout:

```jsonc
{
  "v": 1,
  "kind": "match_report",
  "reference": { "name": "...", "windowS": 30.0, "figures": { "lufs": -9.2, "psr": 8.1, "plr": 12.4 } },
  "mix":       { "name": "...", "windowS": 30.0, "figures": { "lufs": -11.8, "psr": 6.3, "plr": 10.9 } },
  "bands": [ { "name": "low-mid", "deltaDb": -2.4, "tier": "bounded" }, ... ],  // six, in band order
                                      // [rev 4, K] NO `crest` HERE. It moved into crestLog below:
                                      // crest is never rendered, and a field in the drawn payload is
                                      // a field something will eventually draw.
  "moves": [ { "text": "cut 2.4 dB at the low-mid", "deltaDb": -2.4, "band": 2, "kind": "eq" }, ... ],
  "attribution": [ { "uid": "...", "name": "Gtr L", "share": 1.34, "known": true }, ... ],  // C4 §3's s_i[b]: 0..1-and-beyond, NOT normalised
  "coverage":    [ { "band": 2, "coverage": 1.16 }, ... ],    // per band, C4 §3's c[b], same scale; six entries
                                      // [rev 4, K] the field is `coverage`, not `share`: one row
                                      // carrying two different quantities under one name is how a
                                      // reader draws a Link's share as the band's coverage.
  "preFaderCaveat": true,                                     // C4 §9: always true when any move is placed on a track
  "busTap": "input",                                          // "input" | "output" - see §5.1; which tap P_bus came from
  "discriminator": "one sentence, K's words",
  "crestLog": { },                    // [rev 4, 28 Oct] LOGGED, NEVER RENDERED - see below
  "after": { "present": false },      // K6 fills this from verifyCapture(): the same shape as "mix"
  "planId": ""                        // set once draftPlan has answered, so the card can open it
}
```

### `crestLog` — logged for tuning, rendered by nothing (C4 rev 4 §6; Sean, 28 Oct)

Sean's ruling after K's MA10 §10 and §11: **crest decides nothing before the beta and reaches no
user.** Her confirmation test is why — the gap between a real drum excess and a body excess is
**0.13 dB** (*"in a finished mix the band is already mostly the peaky material, so adding more barely
changes it"*). So `bandCrest` is `null` on both sides on the wire, no crest finding is ever written,
**and no band move is ever withheld for crest**.

What the plugin still computes, and stores here **for tuning**, per band and per side, over the
selected window — exactly C4 rev 4 §6's definitions, restated here only because this payload is
where they live:

```jsonc
"crestLog": {
  "v": 1,
  "hostBlockSize": 512,                      // the block size the CAPTURE ran at
  "sides": {
    "reference": {
      "framesPerHop": 4,                     // that series' FFT frames per 100 ms hop
      "v0": [ 5.9, 6.4, null, 7.2, 6.8, 5.1 ],   // six bands, in band order; null = not enough hops
      "v1": [ 2.1, 2.4, null, 3.0, 2.8, 1.9 ]    // "as available"; see the bias note
    },
    "mix": { "framesPerHop": 8, "v0": [ ... ], "v1": [ ... ] }
  }
}
```

- **V0** is `10·log10(95th percentile of bandPower_h / its 3 s local mean)`. A hop qualifies when
  `bandFrames > 0` **and** flags bit 0 is clear; the local mean is over hops `h-15 … h+14` with at
  least **10** qualifying; the band is **`null` under 100 qualifying hops** — a figure from a window
  that short is not a figure.
- **V1 "as available"** is the **median** over qualifying hops of `10·log10(bandPeak_h / bandPower_h)`.
  Before the 256-sample cadence this reads `bandPeak` from **one FFT per host block**, so it carries
  the block-size bias §6 of this document names — which is exactly why `hostBlockSize` and
  `framesPerHop` travel with it: the bias can be corrected later, and a figure that does not carry
  what it was measured under cannot be.
- **Nothing renders it.** No layout reads `crestLog`; K's `EJMatchReport.h` does not take it. A field
  nobody draws is the point: it is a log that happens to ride the card's own record.
- **It also goes to the server with the Match request** (Sean, 28 Oct; B takes it). One computation,
  two destinations — never two computations. It rides **`context.match.crestLog`** in **this exact
  shape**, and B's bounds are part of the contract: `hostBlockSize` an integer **16..8192**,
  `framesPerHop` **0..1000**, each `v0`/`v1` entry **−10..40 dB or null**, `v0` required and `v1` as
  available. **An out-of-bounds log is dropped, never the block** — so a figure outside those bounds
  costs the log and nothing else. The plugin therefore **checks before it sends** rather than clamping
  (`echojay::crestlog::withinServerBounds`): a clamped figure would be a wrong number that passes,
  which is worse than a missing log. It is kept at `match:hist:<uid>:<chatId>:crest` (20 entries, 30
  days), covered by account deletion, and **nothing reads it to decide**.

**The card's one sentence, from ONE source.** The wording is Sean's, verbatim, and it is now the only
one: *"These EQ moves were proposed without the check that tells body from peaks."*  B's earlier
variant is gone (settled 28 Oct), so there is one sentence in the system.

Where it is drawn needs a rule, and B found the reason: a Match card that draws the sentence itself
**and** shows a drafted plan whose `warnings` carry it would show it **twice on one screen**. So:

- **before a plan exists** — the Match card draws it itself, once, whenever the card proposes at
  least one band move, and never under each move;
- **once a plan exists** — the plan's `warnings` carry it (B pushes it once per plan, deduped, when
  the plan has an EQ step), and **the card stops drawing its own**;
- **with no band move** it is absent from both.

One sentence, one source at a time. This is the same rule the run card already applies to an ask's
question (`askHere`: the row and the block are two drawings of one thing, so the block goes) — and it
is a rule about IDENTITY, not about comparing prose: the card knows whether a plan is on screen, so
it never has to ask whether two strings are the same sentence.

Rules that hold the card honest, and that MA-rows pin: a figure that was not measured is **absent, never 0**; `tier` carries "bounded" where X1 bounds a band; `after.present` false means no verify capture exists, which is not the same as "nothing changed".

**Attribution is a SHARE, and C4 owns its definition. [K's review, 27 Oct; Sean's approval of her three asks, same evening. My first §4 invented a normalised share and this replaces it.]** The authority is **`CONTRACT_MATCH_ATTRIBUTION_2026-10-27.md` (C4, K)** — a Link's band power divided by the **mix bus's** band power in the same band, **not normalised and not capped**. This document does not restate it. C4 §3 defines `share`, `coverage` and `known Link`; C4 §9 fixes the card's fields, which is why the payload above carries per-band `coverage` and `preFaderCaveat`. **[rev 4, K: the
line that stood here said the payload carries `sharePct`. It does not and must not — the field is
`share`, ×100 only at display, which the JSON above and the paragraph below both already say.]** §7 below answers C4 §8's questions about where those powers come from, and §5.1 is the new pre-chain tap that changes the answer for `P_bus`.

**One name, one scale (C4 §8.6, settled 27 Oct):** the field is **`share`**, on the wire and in this payload, the same name and the same **0..1-and-beyond** scale that C3 already uses. **×100 happens at the point of display and nowhere else** — a stored percentage is how two scales end up in one pipeline. `coverage` follows it exactly.

The field rules from C4 §9, so the paint branch cannot drift from them: **`share` is not normalised** and may exceed 1 (a fader below unity, a bus chain cutting the band until the tap exists, or cancellation between parts); **coverage is shown per band** ("Linked tracks hold {c}% of the {band}"); **the caveat is shown whenever any move is placed on a track** ("Track shares are measured before each fader, so they assume faders near unity"); and an **unknown Link is named as unknown, never folded into the shares**. Over 1 means that Link alone carries more of the band than the bus does, which is what says *the move goes on the bus rather than on the track*. So:

- **nothing sums to anything**: two Links can both read over 1, and that is the finding rather than a contradiction. The card must never draw these as parts of a whole;
- `known: false` is an unknown Link: named, never folded into another row and never silently dropped;
- it is only computable where the mix bus's band power for that band exists; absent otherwise, by the same rule as every other figure here.

My first draft said the shares sum to 100 across known Links. That was an invention of mine, and it would have been drawn as a pie that does not exist.

---

## 5. The hop accumulator — A's to build, K's specification

K's ask (via Sean, 27 Oct): one compact frame per hop (~100 ms), kept for **the whole capture and the whole reference**, holding short-term LUFS, short-term true peak and the six macro bands' power and peak, from which her pure headers derive whole-file PSR/PLR, six-band crest and the loudest-30-s window. **Agreed, and it fits the engine better than either of us assumed.**

### Why one accumulator really does give both sides identical figures

`MeterEngine` already finalises a **sample-counted 100 ms LUFS block**: `samplesPerBlock100ms = sampleRate * 0.1`, consumed by `while (blockSampleCount >= samplesPerBlock100ms)` in `processBlock`. That boundary is indifferent to block size — and it has to be, because the live capture runs at the host's block (128, 512...) and `ReferenceAnalyser` runs its own `MeterEngine` at **2048**. A hop counted in blocks would have produced two different time bases; counted in samples, the two are the same clock. **K's "about 100 ms" is exactly the engine's own hop**, so the accumulator is a fourth consumer of values already in hand rather than a new measurement.

At that boundary the engine already holds the short-term window (`shortTermBlocks`, 30 × 100 ms) and the short-term true-peak window (`stTpBlocks`, one TP max per hop, 30 = 3 s, which is what `shortTermTruePeak` is made of). The six bands come from `perOct` in the FFT branch — the single value that already feeds three consumers (the ballistic field, the whole-run `bandAccum.sumPower`, and the bounded ring). The hop series becomes the fourth, tapped **at the same line**, so a band's level cannot acquire a fourth definition.

### The frame

```cpp
struct HopFrame                       // 60 bytes; one per sample-counted 100 ms LUFS block
{
    float shortTermLufs;              // the 3 s window ENDING at this hop. -100 for the first 29
                                      // hops, because the window is not full: SKIP those, do not
                                      // treat -100 as a level.
    float shortTermTruePeak;          // dBTP over the same 30 blocks. PSR for this hop is
                                      // shortTermTruePeak - shortTermLufs, which is the definition
                                      // the engine already uses.
    std::array<float, 6> bandPower;   // MEAN of perOct over this hop's FFT frames: linear power,
                                      // per-octave-normalised, UNGATED by silence (matching
                                      // bandAccum and eqCurve, not macroRing - RESULTS_PHASE1B.md).
    std::array<float, 6> bandPeak;    // MAX of perOct over the same frames, same units.
    uint16_t bandFrames;              // how many FFT frames this hop held. 0 = NO spectral evidence
                                      // for this hop, which a reader must render as unavailable and
                                      // never as a level.
    uint16_t flags;                   // bit 0: every block in this hop was silent by isSilentNow().
};
```

**There is no timestamp, on purpose: the index IS the clock.** Frame `i` covers `[i*100 ms, (i+1)*100 ms)` from the start of the capture (or of the file), which is what makes the loudest-30-s window a slice rather than a search. Converting to dB is the reader's job, so the series stays linear and averageable.

Size: 3,000 frames for 5 minutes = **180 KB** (K's 170 KB, plus the two counters I am adding).

### Where it lives, and the thread rules

- `MeterEngine` owns the vector and appends at the hop boundary. **`reserve()` happens in `prepare()`** — never on the audio thread, where a `push_back` that grows would allocate.
- **The cap is 20 minutes** (12,000 frames, 720 KB), the same on both sides so a long reference cannot produce figures the capture could never match. At the cap the audio thread **stops appending and sets a truncation flag**; it does not decimate, because halving 720 KB in place is a memmove on the audio thread and a spike nobody asked for. Every derived figure then carries "the first 20 minutes", which is a true sentence instead of a quiet lie.
- **The hand-off needs no lock.** `stopCapture` reads it on the message thread immediately after the capture state moves to `Complete`, which is exactly how `getAccumulatedBands()` is read today — the audio thread has provably stopped feeding it. `ReferenceAnalyser` runs its engine offline and reads the vector after its file loop.
- Accessors: `const std::vector<HopFrame>& hopFrames() const noexcept` and `bool hopFramesTruncated() const noexcept`. Carried on `CaptureSnapshot` as `hopFrames` / `hopFramesTruncated`, and on the reference's analysis result under the same two names.
- **Per channel, beside its series** (so C4 §2's grid check is a field and not an inference): `hops` (that channel's hop count) and `gainDb` (its registry trim at capture time, which is what C4 §2 adds back). A channel whose `hops` differs from the bus's by more than one is **unknown** for that window — C4 §3's rule, and it needs both numbers to be readable side by side.

### 5.1 The pre-chain band tap — approved, and the naming rule that goes with it
### [Sean approved K's ask, 27 Oct evening. This is the "new tap" §7.2 said it would not slip in.]

**C4 §8.5 answered: yes, A will build it.** A **second `MeterEngine` on the V2's rack INPUT**, running **during a capture only**, with its own hop series on the same sample-counted 100 ms grid as the bus output and every Link. It answers C4's `P_bus` at the point C4 wants it: before the V2's own chain and before the bus trim.

**The naming rule, so `P_bus` can never come from the wrong tap by accident:**

| On `CaptureSnapshot` | What it is |
|---|---|
| `hopFrames` | the bus **output** series: post-chain, post-bus-trim — what leaves EchoJay. Always present |
| `hopFramesRackIn` | the rack **input** series: pre-chain, pre-bus-trim. Present only when the capture ran with the tap |
| `hopFramesRackInHops` | its hop count, for the same alignment check as a Link's (§7.3) |

**`P_bus` comes from `hopFramesRackIn` when it is non-empty**, and otherwise from `hopFrames` with the bus trim added back — and the card says which, through the `match_report` payload's `busTap: "input" | "output"` (§4). A figure that cannot say where it was measured is a figure nobody can check later, and the two taps differ by whatever the V2's own chain is doing.

**What it costs, said plainly:** a second full 2048-point FFT per block while a capture runs, roughly doubling the metering FFT load for the duration, plus that engine's buffers. **Zero when not capturing** — it is prepared with the engine but only fed inside the `Capturing` branch, which is the same gate `captureEngine` already sits behind. That is the whole reason it is capture-only rather than always-on.

### What K can rely on, stated as promises

1. The hop is the same clock on both sides, whatever the block size or sample rate (44.1 k gives 4,410 samples per hop; 48 k gives 4,800).
2. `bandPower` / `bandPeak` are linear and per-octave-normalised, from the same `perOct` as every other band figure in the product.
3. The first 29 frames have no short-term window, and `bandFrames == 0` means no spectral evidence. Both are absences, not zeros.
4. Nothing in the series is silence-gated, so a quiet passage is in it and distinguishable by `flags` bit 0.

### A separate finding, A's to carry

While siting this I found that the same function already does `allBlocks.push_back(block)` once per hop for the whole capture, **unbounded, on the audio thread** — a real-time allocation that nobody has hit because nobody has captured long enough. It is not K's problem and not this contract's, but it is now a queue item of mine, and the new accumulator will not add a second one.

---

## 6. The FFT behind `perOct` — frame length, hop, and what the band peak can actually see
### [K's question, 27 Oct. The short answer is "2048 and the host's block size", and the second half of that is the part that matters.]

**The frame: 2048 samples, Hann-windowed, DC removed.** `MeterEngine::fftOrder = 11`, so `fftSize = 2048`; the window is a full-length Hann built in `prepare()`, the frame is read out of a circular accumulator (always **the most recent 2048 samples**), its mean is subtracted, and `performFrequencyOnlyForwardTransform` gives the magnitudes `perOct` is integrated from. **42.7 ms at 48 kHz, 46.4 ms at 44.1 kHz.**

**The hop: the caller's block size.** `computeSpectrum` is called **once per `processBlock`**, and there is no sample-counted gate in front of it. So the hop is whatever the caller feeds:

| Caller | Hop | Frames per 100 ms hop | Overlap of a 2048 frame |
|---|---|---|---|
| Live, host at 128 | 2.7 ms | ~37 | 16× |
| Live, host at 512 | 10.7 ms | ~9 | 4× |
| Live, host at 1024 | 21.3 ms | ~5 | 2× |
| **`ReferenceAnalyser` today, at 2048** | **42.7 ms** | **~2.3** | **none** |

(The `samplesPerSpecFrame` ~25 fps cadence in the same file is the **spectrogram ring's**, not this. `perOct` does not use it.)

### Three consequences, and the one that bites

1. **`bandPower` survives this.** Each frame is a Hann-weighted average over 2048 samples; the mean of overlapping frames and the mean of non-overlapping ones estimate the same quantity. Comparing a capture's hop power with a reference's is sound.
2. **`bandPeak` does not.** It is a MAX over the frames in a hop, and a side with 9 overlapping frames will find a higher maximum than a side with 2.3 non-overlapping ones — on identical audio. A six-band crest built from `bandPeak` would therefore read differently on the two sides for reasons that are entirely about block size. **This is the thing to know before building the figure on it.**
3. **The shortest transient it can see is ~43 ms, whatever the hop.** The frame length bounds the resolution: a 5 ms snare hit is averaged over 2048 Hann-weighted samples, so it lifts a frame by a few dB rather than appearing at its own level. Shrinking the hop samples that smeared value more often; it does not sharpen it. So a crest from `bandPeak` is honestly described as **short-term band dynamics at ~43 ms**, never as a transient or true-peak crest. A genuine per-band transient measure needs an envelope follower per band (a filterbank), which is a different instrument and not in this contract.

### What I will do about (2), and the choice that is K's

- **Mine:** `ReferenceAnalyser` will feed its engine in **512-sample blocks** instead of 2048. It is offline, so the extra FFTs cost nothing anyone waits for, and it puts the reference on the same hop as the common live case. It lands with the accumulator.
- **Mine:** every hop frame carries `bandFrames`, so K's code can read the frames-per-hop of each series and either correct for it or refuse to compare two series whose counts differ. The figure can then state its own provenance instead of hiding the difference.
- **Residual, stated plainly:** a host at 1024 or 2048 still gives fewer frames per hop than the reference will, so `bandPeak` comparisons are "same within a known bias", not identical. I cannot fix that from the plugin side — the host chooses the block size.
- **SUPERSEDED BY C4 REV 4 §6 (28 Oct, K).** Everything in this bullet and the next was right about
  rev 2's measure and is no longer what happens: under rev 4 **crest decides nothing before the beta
  and is never shown**, both measures are computed and **logged** under `crestLog` (§4), and `V1` is
  kept "as available" rather than abandoned — it reads `bandPeak` from one FFT per host block until
  the parked 256-sample branch merges, which is why `crestLog` carries the host block size and each
  series' frames per hop. Kept below rather than deleted because it is the reasoning rev 4 rests on.
- **(rev 2, superseded) C4 rev 2 §6: the crest is rebuilt on the 100 ms grid and no longer uses `bandPeak` at all.** Precisely, because the one-line version of it is easy to implement wrongly: for each qualifying hop (`bandFrames > 0`, not silent) the **local** level `M_h` is the mean `bandPower` over the qualifying hops in the 3 s *around that hop* (`h−15` to `h+14`, skipped when fewer than 10 qualify); `r_h = bandPower_h / M_h`; and the figure is `10·log10(95th percentile of r_h)` over the window, null under 100 qualifying hops. **A per-hop local mean, not one mean for the window** — which is the whole point, since a single mean would mostly measure the arrangement. `bandPeak` stays in the frame as evidence that a hop held a loud frame. That is the hop-grid definition recommended below, and it has a consequence worth stating: **C4's 1.0 dB margin no longer waits on my FFT answer.** The 95th percentile of hop power against a local mean lives entirely on the sample-counted 100 ms grid, so it does not inherit the frames-per-hop bias of a within-hop maximum, and the margin can be chosen on its own merits. `bandPeak` remains useful for exactly what it is — evidence that a hop held a loud frame — and is no longer the basis of a published number.
- **(The reasoning, kept because it is why the above is right.)** Define the six-band crest from the **distribution of `bandPower` across hops** (say the 95th percentile against the mean over the window) rather than from `bandPeak` within a hop. That lives entirely on the 100 ms grid, which IS sample-counted and identical on both sides by construction, so the figure stops depending on the host's block size at all. `bandPeak` then stays in the frame as evidence rather than as the basis of a published number. Her figure, her call — but if she wants it, it costs nothing extra in the accumulator, because both fields are already there.

---

## 5.2 A12b — the series carried everywhere K reads it  **[Sean's point 27; K's K1, K2, K5]**

| Where | What it carries |
|---|---|
| `ChannelMeterData` (per Link) | `hopFrames`, `hops`, **`gainDb`** — that Link's own trim **as it was when the capture started**. The series comes off the ring, which is post-trim, and C4 §2 adds it back; reading the registry at report time would read whatever the user has moved since |
| `ReferenceResult` | `hopFrames`, `hops`, from the same engine that made every other figure, **fed in 512-sample blocks** instead of 2048 |
| `CaptureSnapshot` | `hopFrames` (bus output), `hopFramesRackIn` (the input tap), `hopFramesHops`, **`hopFramesRackInHops`**, `hopFramesTruncated`, and **`busGainDbAtCapture`** |

**Why 512 on the reference.** The FFT's hop follows whoever feeds it, so a file read in 2048-sample
blocks sat at 42.7 ms while a live capture at a 512-sample host block runs at 10.7 ms. Every per-hop
**mean** survived that; a within-hop **maximum** did not. **K's MA10 measured the cost on real stems:
block size alone moved the crest by up to 0.38 dB** — and for comparison, 4:1 compression at 6 dB of
gain reduction moved it +1.0 dB in the mids and ≤0.25 dB elsewhere. A measure whose block-size noise
is a third of its signal is not yet a measure, which is exactly why the reference now matches the
common live cadence.

### `bandPeak` is FROZEN, deliberately  **[K, 28 Oct]**

`bandPower` is carried exactly as it is. `bandPeak` is carried **as currently defined**, and **neither
its meaning nor the FFT cadence in MeterEngine changes yet** — K is testing offline today whether
`bandPeak / bandPower` within a hop works on a **sample-counted 256-sample cadence**, and the same
with a **1024-point FFT above 250 Hz**. If either does, `bandPeak` will need that cadence in the
audio path, which is mine. So the frame keeps the field and keeps room for it: `HopFrame`'s two
counters sit **last** (`bandFrames`, `flags`), and a cadence marker is additive there — the same
positional rule `RackSidecarSlot` has four notes about. Nothing in the audio path moves until her
numbers say which cadence to build.

**C4 is at rev 4** (K, 28 Oct). Two things from it that live in her code rather than here, noted so
the two documents cannot drift: a Link whose band is silent is **known with share 0**, not unknown
(rev 3); and under **rev 4** crest decides nothing before the beta, is never shown, and withholds no
band move — what this document owes it is `crestLog` in §4 and the card's one sentence.

---

## 5.3 A8 — the limiter dial in loudest-30 mode  **[Sean's point 3, 28 Oct]**

**In `loudest30` mode the dial carries an offset. In `whole` mode it carries none.**

```
offset = ref.loudest30Lufs - mix.loudest30Lufs          // loudest30 mode only
target = <the plugin's OWN fresh integrated reading, taken at dial time> + offset
```

**Why an offset, and why against a fresh reading.** The figures compared in `loudest30` mode are each
side's *loudest 30 seconds* — not their whole songs. The limiter dials against what it can measure at
dial time, which is an integrated reading of what is playing. Handing it a loudest-30 target directly
would mix two windows in one subtraction: a whole-song measurement aimed at a loudest-window number,
missing by however much the mix's quiet parts pull its integrated figure down. The **difference**
between the two loudest-30 figures is the one quantity that survives the change of window, so that is
what travels — and the plugin adds it to a reading it takes itself.

**Fresh matters.** A figure measured when the plan was drafted describes the mix before the plan's own
EQ and dynamics moves landed. The dial happens after them, so the integrated reading is taken **at
dial time, by the plugin, every time**.

**No offset in `whole` mode**, because both sides are already whole-song figures and the plugin's own
integrated reading is the same kind of number: correcting for a window difference that is not there
would simply be wrong.

**What must not cross the wire:** a *target* computed on the server from stale figures. The server
carries `offset` (or nothing, in `whole` mode); the plugin owns the reading it is applied to. One
number travels and it is a difference, not a destination.

---

## 6b. How far an EchoJay EQ band may move  **[Sean's point 6, agreed with B, 28 Oct]**

**24 dB, and it is a real limit now.** The finding was that the processor had **none**: both write
sites (`SurgicalEqProcessor.cpp:106`, the per-control setter, and `:370`, the JSON restore) assigned
`gain_db` straight from whatever arrived, with no `jlimit` anywhere in the file — and the agent and
preset restore both come through those paths, so a plan asking for +60 dB got it. `X1.eqGainMaxDb = 24`
was only the editor's **view scale**, which is a different claim: past it the drawn curve cannot show
what the band is doing, which is its own lie.

- **One definition:** `lib/mixRules` **`X1.eqGainMaxDb = 24`** is the source. The plugin's
  `SurgicalEqProcessor::kBandGainMaxDb` names that figure and nothing else.
- **Clamped in the processor**, at both write sites, through one function — so the two cannot drift.
- **The clamp speaks when it bites:** "band 2: asked for 30.0 dB, the band holds 24.0 dB (a band moves
  at most 24 dB)", in the log and in `takeLastBandGainClamp()` for the step to report. A value quietly
  altered is the fault class the refusal-after-apply rule exists for. It is **silent** for ordinary
  moves, because a clamp that narrates every write is noise.
- **It is a magnitude, not a ceiling:** a −80 dB cut is held at −24 the same way.
- **The server refuses above it** on the Match path already, and B is adding the same refusal to the
  chat and agent paths, so the model learns the bound and this clamp only catches what slips through.
- Guard legs **EQG1–EQG5**, including the silence on an ordinary move and the sentence when it bites.

---

## 7. Answers to C4 §8, measured rather than assumed
### [K asked A to confirm rather than assume. Two of the three premises do not hold, and both would have produced quietly wrong numbers.]

### 7.1 `perOct`, linear power, same definition both sides — **yes**

Both sides come from the same line of the same function. `P_bus` and every `P_i` are `perOct`: raw FFT power integrated per macro band and divided by the band's width in octaves (`EJBandScheme.h` owns the edges), **linear, never dB, never normalised to the six-band mean**. The hop series stores exactly that value — mean and max per hop — and leaves dB to the reader. C4 §2 requirement 1 holds as written.

### 7.2 Where each one is actually measured — **not as C4 §2 requirement 2 assumes**

**The Link side forks, and the two forks differ by exactly the Link's own trim.**

| Source | Tap point | Is it a hop series? |
|---|---|---|
| The Link's **published meter frame** | POST-chain, **PRE-trim** — "the Link's own rack output arriving at the trim" (the 21t-d move of 25 Sep 2026). The frame sets `kFrameHasPreTrim`, and `frameLoudnessAsHeard` adds the trim back for a reader who wants the DAW's figure | **No.** ~25 fps ballistic fields, not hop-aligned |
| The **capture path** — `ChannelMeterData`, what `latestCapture()` hands you, and the only road to a per-Link hop series | The V2 consumes that Link's **ring**, and the ring write sits **below `applyGainSmoothed`**: **POST-trim** | **Yes** (§7.3) |

So the series K needs is **post-trim**, and the trim has to be added back: the V2 holds each Link's `gainDb` in its registry row, so `P_pre = P_post / 10^(gainDb/10)`. That correction is **exact for a steady trim** (a scalar multiply) and approximate only across a hop in which the trim was being moved, because the gain is smoothed. I will carry each channel's `gainDb` beside its hop series so the correction is applied from one place and not guessed.

**One distinction worth keeping separate, because correcting for the wrong one is silent:** C4 §3's "a Link is pre-fader, a track whose fader sits at −6 dB reads four times its real contribution" is about the **DAW's** fader, which is downstream of the Link and which EchoJay cannot see at all. That note stands exactly as written. EchoJay's **own** Link trim is a different gain, it is visible, and it is the one being added back above. Two gains, one sentence each.

**The bus side is not available as specified.** The V2's meters and its capture read the buffer **after `chainHost.process` and after `applyBusGainSmoothed`** — deliberately, so that everything describes "what actually LEAVES EchoJay". There is **no rack-input tap in the capture path**, so `P_bus` is the bus **output**, not its input:

- **The bus trim can be added back exactly** (`getBusGainDb()`), same scalar argument as above.
- **The V2's own chain cannot be removed.** With an empty or neutral bus chain the output and the input are the same signal, so C4's figures are unaffected; with a bus chain doing work, the denominator includes it. For the question C4 asks — *does this band move belong on the bus?* — a denominator that already contains the bus's own processing is arguably the right one, but it has to be **said** rather than discovered later.
- If K wants the true pre-chain bus figure, it is a **new tap**: a second `MeterEngine` on the pre-chain buffer, which is a full 2048-point FFT per block. That is a real CPU decision, not a free field, so it is a separate item with its cost named — not something I will slip in.

**Net, for C4 §2:** requirement 1 holds; requirement 2 should read *"a Link measures at its rack output **post its own trim, with the trim added back from the registry figure**; the V2 measures the bus at its **rack output**, with the bus trim added back and its own chain included."* Requirement 3 holds and §7.3 is why.

### 7.3 Per-Link data cut to the bus's hops, time-aligned — **yes, and not by cutting**

**Every Link's capture channel already has its own `MeterEngine` inside the V2** (`LinkCaptureChannel::meterEngine`, fed by `accumulateLinkChannel` from that Link's ring). They are all driven by the **V2's own capture loop, in the V2's process, block by block**. So the hop accumulator of §5 gives a per-Link series automatically, on the same sample-counted 100 ms grid as the bus's, **aligned by construction rather than by resampling**. Hop *i* of a Link and hop *i* of the bus cover the same span of the same capture.

That makes C4 §2's fallback unnecessary: **`loudest30` keeps track attribution**, because "the same 300 hops on the bus and on every Link" is exactly what the grid gives. The sentence about switching to Whole song can go.

**The one caveat, and its detector:** a Link whose ring under-runs delivers fewer samples in a block than the bus does (`ringConsume` returns `n < nReq`), so after an under-run that channel's hop boundaries sit behind the bus's by the dropped samples. This is not hypothetical on a busy host. Two things make it visible rather than silent:

1. `framesReceived` already distinguishes "no frames at all" from "real audio" — and C4 §3's "known Link" rule (frames covering at least 90% of the window) is the right instrument for it.
2. I will carry **the hop count per channel** beside each series, so K can compare a Link's hop count with the bus's directly instead of inferring it from sample totals. A channel short of the bus by more than one hop is a channel whose shares should be reported as unknown for that window.

### 7.4 The FFT frame length and hop — in §6 above

2048-sample Hann frame (42.7 ms at 48 kHz), hop = **the caller's block size**, with the table of consequences in §6. One addition that matters here: for the **per-Link** engines the caller is the V2's capture loop, so **every channel in a capture shares one hop**, which is the other half of why §7.3 aligns. The reference side moves to a 512-sample feed so its hop matches the common live case.
