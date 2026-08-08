# Queued for next session

Three items, in the user's order. Nothing here is started.

## BUILT 4 Aug 2026 — items 4 and 1, and the naming question ANSWERED for API-550A

**Item 4 (manual band entry) and item 1 (`strideNote` persistence) are built and proven.**
`--selftest-manualbands <plugin_id> [accept|refuse]` runs both halves against API-550A.

**The acceptance test the queue demanded, passing:** three bands entered by hand on API-550A,
submitted, **and the written map carries three band groups** with
`grouping_source: "mapper"`. The derived order came out **50 < 400 < 5000 Hz**, sorted from
the swept frequencies rather than from the order the bands were entered in.

**The guards, each proven by attempting the thing it refuses** (`refuse` mode):

- **order** — bands entered high-first are REFUSED, and the message shows both:
  *"You entered band 1 10000.0 Hz, band 2 2700.0 Hz, band 3 225.0 Hz, which puts them in the
  order band 3, band 2, band 1."*
- **unit sanity** — a frequency slot pointed at a dB control: *"band 1 frequency [2] sweeps in
  'db', not Hz."*
- **the fixed-frequency answer** — N on a frequency row, which the band card could not answer
  at all before, confirms the band with **no index** and `freq_source: "typed_fixed"`.

### THE NAMING QUESTION, ANSWERED BY READING (item 1's whole purpose)

API-550A's parameters, printed from the loaded plugin:

```
[0] High Gain   [1] High Freq   [2] Mid Gain   [3] Mid Freq   [4] Low Gain   [5] Low Freq
[6] Filter  [7] LF-Type  [8] In  [9] HF-Type  [10] Polarity  [11] Analog  [12] Output
```

**The recollection was right, and is now a reading.** `prefixOrder()` is
`{LF, LMF, MF, HMF, HF}` matched WHOLE-TOKEN, so `"Low Freq"` -> `["Low","Freq"]` matches
nothing, `morphSlot` returns -1, and the inference yields zero bands. Note `LF-Type` and
`HF-Type` DO carry the vocabulary — on the two parameters that are not bands.

**This is ONE plugin.** It settles API-550A and it does not settle the category: item 0
step 2's corpus is still what decides whether a synonym fold is the right shape or whether
there are forty conventions. Do not widen `prefixOrder()` on the strength of this.

### Three smaller defects found and fixed on the way

- **W left the card stale.** Re-opening a row changed what its keys meant and the answer strip
  kept showing the old row's answers. Found by a self-test asserting a re-opened frequency row
  offers "N - no frequency control": N worked, and the card had not said so.
- **The fixed-frequency answer did not stand a live capture down**, so a later touch could have
  landed in a row that had already answered. Same discipline as `actionSkip`'s armed branch.
- **The corpus gate demanded every primary group carry a `q`.** It means to assert that two
  routing paths AGREE, and also required them to reach something — so the first real map with
  a q-less group (Dangerous BAX EQ Master, submitted by the user 4 Aug 14:36 BST) failed a
  gate that had simply never met one. Both-absent is agreement. **A q-less band is legitimate
  and the design says so**, which makes this failure routine from here rather than rare.

**Gates after all of it:** round-trip **331 checks / 0 failures** over 22 corpus maps,
registry conformance **90 / 0**, `--selftest-dupescape` **17 / 17**.

### Not done, and deliberately

- **`enable` rows are offered and recorded, but nothing HONOURS them yet.** The consumer reads
  an enable link from `controls.<name>.enabled_by` (`EjmapSubject.h:339`), which is a Tier 2
  control field, not a band-group param. A manually entered enable is captured so the mapper's
  work is not lost; wiring it to the consumer is separate and unbuilt. **Nothing claims it
  works.**
- **`typed_parametric`** — the schema field and its meaning are defined, and the wizard has no
  key that produces it yet. Only `swept` and `typed_fixed` are reachable today.

---

**Items 5-9 were added 4 Aug 2026 (session start, nothing built yet). Items 5-8 come
from the user during that session; item 9 was measured while answering item 6's
questions. Read the ANSWERS block (item 10) before building item 4 — two of the three
graphic-EQ questions are now answered from source.**

---

## 1. Persist the band diagnostic — **DONE, verify only** (found already built 5 Aug 2026)

`persistSession` writes a `band_diagnostic` block whenever an inference has been
ATTEMPTED — `stride_note`, `axis`, `family`, `bands_inferred`, `stride_verified`
and the touched members with their names. Live on disk: 3 of 44 sessions carry it,
all `bands_inferred: 0` with a note naming `Ch1LoFreq` — the Manley case this item
was raised about. So the three states ARE distinguishable today:

| state | how it reads |
|---|---|
| never attempted | no `band_diagnostic` block |
| attempted, found nothing | block present, `bands_inferred: 0`, note names the parameter |
| found bands, mapper deferred | block present, `bands_inferred: N`, `accepted_groups: 0` |

**The third has no live instance yet** and is the state that will dominate once the
pipeline handles bands — it needs an app-level self-test, not a re-build.

### Original item, kept for the reasoning


**The defect, measured 4 Aug 2026 on API-550A (`aufx,A5AM,ksWV`):**

The band inference reported "0 bands inferred from your 3 touches" to the screen and
persisted NOTHING. `assign-bdc578da…json` holds exactly:

```
fp · plugin_id · category · rows · ignore_rows · pending_controls
lockstep_observed · accepted_groups · controls_excluded
```

No `bandPlan`, no `strideNote`, no captures. The three touched indices and their
parameter names died with the prompt. `accepted_groups: 0` is the only trace and it is
**indistinguishable from never having attempted the inference at all.**

`strideNote` is built for exactly this — `EjmapBands.h` sets it to
`"no digit or band-prefix token in '<name>'"`, naming the parameter it failed on. It
reaches the screen and goes no further.

**What to record in the session file when an inference runs:**

- `strideNote` verbatim
- the touched parameter indices
- their parameter names as the plugin reports them
- `axis` (`"digit"` | `"prefix"`) and whether stride verification ran
- enough to distinguish *attempted and found nothing* from *never attempted*

**Then:** the user re-runs API-550A's bands and we READ the note instead of recalling
the product. Do not diagnose the naming question before that read exists — see below.

### What is already established (do not re-derive)

`EjmapBands.h:100`:

```cpp
static juce::StringArray prefixOrder()
{ return { "LF", "LMF", "MF", "HMF", "HF" }; }
```

- There are TWO axes, `"digit"` and `"prefix"`. **Numbering is not required** — LF/MF/HF
  EQs are explicitly supported and the order is treated as the claim.
- The match is `prefixOrder().contains(toks[i], true)` — a WHOLE-TOKEN comparison.
  `"LF Freq"` → `["LF","Freq"]` matches. `"Low Frequency"` → `["Low","Frequency"]` does
  not: "Low", "Mid", "High", "Low Mid", "High Mid" are absent from the vocabulary.

### What is NOT established

**API-550A's actual parameter names.** The hypothesis that it spells them out is
RECOLLECTION of the product, not a reading of its parameter list, and it must not be
treated as a finding. The scope question — one plugin, or every vintage-console EQ named
by frequency range — is open until real names are read from two or three EQs.

If the note confirms spelled-out names, the likely fix is widening `prefixOrder()` plus a
synonym fold (Low→LF, Low Mid→LMF, …). The two-axis machinery already does the hard part;
nothing structural is implicated.

---

## 2. The parked state

**Parking already works. Nothing says so, which is why it has been avoided.**

Established: `persistSession()` writes on every accept; `restoreSession()` matches on `fp`
and works across restarts (it is the mechanism that restored CLA-76's rows). Scanning or
loading another plugin does not touch `assign-<fp>.json`. API-550A's file holds real state
— `output_db` confirmed at index 12, two `not_present`, bands and controls deferred.

NOT established: a full park-and-return round trip has never been watched. **The user is
testing this manually first and will report.** Read that report before building.

**Propose (all of it reads disk and asserts nothing):**

- a **parked** state in the list — a plugin with a session file and unresolved rows is
  neither unmapped nor mapped, and column 1 currently calls it unmapped
- a line on leaving that says the work is saved
- a line on return saying what was restored and where the mapper left off (row counts are
  already in the file)

The capability needs nothing. The gap is entirely in what is said and shown, and silence
about an honoured guarantee is what makes a mapper rescan and re-map instead of parking.

**Why it matters at volume:** the intended workflow is park a plugin that is fighting you,
do three easy ones, come back.

---

## 3. Still queued from earlier sessions

- **Row-index keys in `map-state.json`** — keys `"0"`, `"3"`, `"7"` sit beside 1,799 proper
  `format|uid|version` strings. Row indices shift between scans, so those entries will point
  at whatever occupies that row next time: a column reading confidently WRONG rather than
  unknown. Find the writer passing an index where an identity belongs; make the loader
  reject any key not matching the identity shape rather than trusting the file.
- **The retry rule + force-quit attribution** — `docs/NEXT_SESSION_RETRY_AND_ATTRIBUTION.md`,
  including the two corrections appended 4 Aug (stage-scoped `prior_ok_in_ledger`; crashes
  key on the binary, not the product) and Drawmer's re-quarantine after release.
- **The refusal audit** — `docs/NEXT_SESSION_REFUSAL_AUDIT.md`. Read every user-facing
  refusal and ask whether that code path measured what the message claims. The keyword
  sweep already run is NOT that audit.

## Related tooling proposed but not built

`--load-once <plugin_id> [--repeat N] [--editor] [--render]` — a single-plugin headless
load. Wanted twice in two days: Drawmer 1973 could not be reproduced because `--probe-batch`
needs a map, and the plugins worth diagnosing are precisely those that never got far enough
to have one. Separate child process per attempt, one JSON line per attempt.

---

## 4. Manual band entry — ACCEPTED, AND NOW THE FIRST THING TO BUILD

**PRIORITY CHANGED 4 Aug 2026, BY THE USER, AFTER the corpus ordering was recorded
below. Build THIS first — ahead of item 0's corpus, ahead of item 1.**

The scope note at the end of this item, and item 0's ordering, both argued corpus-first.
That reasoning still holds for what the corpus DECIDES; it was wrong about what to build
first. The user's correction, verbatim in substance:

> the priority was wrong: I am the one typing, manual entry works whatever the corpus
> says, and vintage EQs are blocked today. The corpus later just reduces how often I
> need it.

Manual entry is unblocked by nothing and unblocks the category today. The corpus changes
only the FREQUENCY with which it is needed, not whether it works.

**Persisting `strideNote` (item 1) ships WITH this**, not after: a failed inference should
still say why even once a manual path exists.

**ACCEPTANCE TEST — API-550A**, parked with three bands the mapper knows to be LF, MF and
HF (`aufx,A5AM,ksWV`, session `assign-bdc578da…json`, `output_db` already confirmed at
index 12). Enter its bands manually and submit a map carrying three band groups. Nothing
short of a submitted three-group map is proof.

The typed-anchors equivalent for bands: when inference cannot group a band, the mapper
says which indices form it.

### What a band group needs

- **Required: `freq_hz` + `gain_db`.** A band the consumer cannot place or cannot move is
  not dialable. This is also the pair the band matcher already routes as a unit on
  grouped EQs.
- **`q` — optional, and genuinely often absent.** API-550A has none; several vintage EQs
  fix or step it. A group without `q` dials fine.
- **`enable` — optional to HAVE, mandatory to HONOUR.** If a band has an enable reading
  off, the freq/gain writes are inert and the dial silently does nothing. A manually
  entered band goes through `enableLinkFor` / `checkEnableIsNull` exactly as an inferred
  one does.

### Interaction: synthesise rows, do NOT build a picker

Do not ask "which of these three is band 1's frequency?" — that is a new widget and a new
mental model. Instead: one count prompt ("how many bands?"), then synthesised assignment
rows, one per band-slot:

```
band 1 frequency      unresolved
band 1 gain           unresolved
band 2 frequency      unresolved
```

Each resolves through the flow the mapper already knows — touch, W to sweep, confirm.
`q` and `enable` rows are offered and D-able without penalty, since both are legitimately
absent. The panel already renders and resolves rows generically, so mismatch warnings,
readback and skip reasons all come along free.

### Recording: TWO CLAIMS, RECORDED SEPARATELY

```json
{ "n": 1, "label": "LF", "grouping_source": "mapper",
  "ordering_source": "measured",
  "params": { "freq_hz": {...}, "gain_db": {...} } }
```

- `grouping_source: "mapper" | "inferred"` — who decided these indices form a band. Human
  is stronger evidence FOR THE GROUPING.
- **Per-parameter evidence is unchanged** — sweep anchors, unit family, trust, readback.

The mapper's say-so groups them; the sweep says what they do. Nothing asserted is ever
promoted to a measurement. Do not merge these into one trust field.

### Ordering: the mapper supplies the LABEL, the tool derives the ORDER

The mapper supplies `LF`/`MF`/`HF` (or `1/2/3`). Ordering is derived from the **swept
frequency range** — each freq slot is swept anyway, yielding a real lo/hi — and bands sort
by measured midpoint. Then:

- **agree** → `n` assigned from measured order, both recorded, the label rides along
- **disagree** → **REFUSE and show both.** "You labelled index 4 as LF, but it sweeps
  800–8000 Hz and sits above the band you called MF." Either a mislabel or a mis-picked
  index; both are worth stopping for.

> **DO NOT "SIMPLIFY" THIS BACK TO ENTRY ORDER.** This is the strongest part of the design
> and it is counterintuitive, so someone will try.
>
> Deriving order from measured frequency makes the manual path **better than inference on
> the ordering claim**, not a weaker fallback. Inference reads order from NAME POSITION
> (`prefixOrder()`'s LF→LMF→MF→HMF→HF); this reads it from BEHAVIOUR. M5 treats
> LF-below-MF-below-HF as *the claim*, so the manual path must ESTABLISH that claim rather
> than inherit it from the order the mapper happened to type.

### AMENDMENT 4 Aug 2026 — fixed-frequency bands, and WHY ordering stays derived

Raised when the graphic-EQ fold-in was answered. The first reading of it was that a typed
frequency weakens the ordering claim to `ordering_source: "mapper"`. **That reading was
wrong and the correction is the important part of this section.**

On a graphic EQ the frequency is typed because **it is printed on the panel** — 31, 63, 125,
250. The ordering still derives from those numbers, not from an assertion about which band
is which. **Sorting typed values by magnitude is derivation, not trust.** What the original
design protects against is ENTRY ORDER becoming the claim, and that does not happen here,
because the numbers order themselves.

**So `ordering_source` stays `"derived"`, and a THIRD field records where the frequency came
from.** The two claims stay separate, as everything else in this design does:

| field | values | what it says |
|---|---|---|
| `grouping_source` | `mapper` \| `inferred` | who decided these indices form a band |
| `ordering_source` | `derived` | the order came from frequency magnitudes, never from entry order |
| `freq_source` | `swept` \| `typed_fixed` \| `typed_parametric` | where the frequency numbers came from |

**THE DISTINCTION THAT IS EASY TO COLLAPSE, AND MUST NOT BE:**

- **`typed_fixed`** — a graphic-EQ band with no frequency parameter. The typed number is a
  **transcription of a constant** printed on the panel. Strong.
- **`typed_parametric`** — a movable frequency control the mapper typed instead of sweeping.
  The number is **the mapper's reading of a control that can be anywhere**. Weak, and weak
  for a different reason than grouping is: nothing measured it, and it can drift the moment
  the control moves.

If manual entry allows both — and it should — they cannot share a source value. A consumer
reading `typed_fixed` is reading a constant; one reading `typed_parametric` is reading a
snapshot of a control's position. Collapsing them would let the weakest case borrow the
credibility of the strongest.

### Two cheap checks (the sweep already runs; these cost nothing)

- **Unit sanity** — a slot claimed as frequency that sweeps to a dB unit family is
  refused, and vice versa. Catches the commonest slip: picking the gain when you meant
  the freq.
- **Distinctness** — two bands with identical swept ranges suggest the same parameter
  picked twice, or a channel bank rather than distinct bands. Flag; refusal optional.

### SCOPE NOTE — why item 1 comes first

Item 1 (persisting `strideNote`) decides whether manual entry is an **escape hatch** or
the **main road**, and that changes what should be built.

If most vintage EQs miss because their names are spelled out ("Low Frequency" rather than
"LF"), then widening `prefixOrder()` plus a synonym fold (Low→LF, Low Mid→LMF, …) is the
cheaper fix and manual entry stays the exception. Building manual entry first would be
building an exception path for the common case.

**So: item 1 first for this reason, not merely because it is smaller.**

---

## 0. VINTAGE EQ BAND INFERENCE — DO THIS FIRST, AHEAD OF EVERYTHING ABOVE

Raised 4 Aug 2026. Vintage EQs are a large and important part of the catalogue and
deferring their bands is **not acceptable**. This supersedes the ordering in item 4's
scope note: item 1 (`strideNote`) explains why ONE inference failed; this explains how
they ALL fail, at once, and it is the one that unblocks the category.

### STEP 2'S CORPUS NOW EXISTS — read it before doing step 2 again (5 Aug 2026)

The offline proposer's first run over 597 controls produced the corpus reading this
item was waiting for, as a by-product. **`docs/BAND_ROUTING_PROPOSAL.md` §2 has it in
full.** In summary, across the 11 plugins whose band controls collided:

**Five conventions, and they are systematic rather than arbitrary.**

| convention | example | in `prefixOrder()` today? |
|---|---|---|
| ordered prefix, whole token | `LF Freq`, `HMF Q` | yes |
| spelled-out ordered word | `Low Freq`, `Mid Gain` | no |
| compound, no separators | `Ch1LoFreq`, `Ch1HiMidFreq` | no |
| shelf-typed pair | `Low Shelf Frequency` / `High Shelf ...` | no |
| digit run | `... Frequency 1` / `... 2` | yes, and see the trap below |

A fold of `Low/Lo→LF, LoMid→LMF, HiMid→HMF, Hi→HF` reaches **7 of the 8 band-set
plugins**. So the answer to "a synonym fold, or forty conventions?" is **a fold**.

**AND THE DIGIT AXIS IS NOT SAFE ON ITS OWN.** Dangerous BAX EQ Master has a digit
run — the strongest band signal there is — where the digit is an INSTANCE number,
given away by its `Output Level 1` / `Output Level 2`. Same shape in DPR-402's
`Freq L/M` vs `Freq R/S` and Manley's `Ch1` vs `Ch2`. Whatever step 3 builds must
refuse these from the evidence; `tools/propose/bands.py` refuses all three and
`test_bands.py` attempts each one as an acceptance test.

**WIDENING `prefixOrder()` IS ITS OWN PIECE OF WORK, WITH ITS OWN BLAST RADIUS.**
It changes the HAND PATH's inference — what `morphSlot` matches, what the wizard
offers, what a stride verifies — and so it needs its own gate proving both
directions on real names, not just the proposer's tests. The proposer deliberately
carries its OWN local fold (`bands.FOLD`) and hands the matcher explicit members
instead, precisely so that this decision stays here and is made on purpose. The
evidence is now gathered; the decision is still open.

**Three steps, strictly in order. Do not skip to step 3.**

### Step 1 — build `--load-once`

```
ejmap --load-once <plugin_id> [--params] [--repeat N] [--editor] [--render]
```

Load one plugin headless by the `plugin_id` already used in ledger rows
(`AudioUnit:Effects/aufx,A5AM,ksWV` or a `.vst3` path), **print its full parameter list**
(index, name, and label/unit if the plugin reports one), unload, exit. One JSON line per
attempt.

Wanted THREE times in two days: Drawmer 1973 could not be reproduced (`--probe-batch`
needs a map, and the plugins worth diagnosing never got far enough to have one),
API-550A's names could not be read, and now the corpus needs it. It unblocks this and
every future diagnosis.

`--repeat N` must use **separate child processes** — a second load in the same process
inherits whatever the first corrupted, and determinism is what it would be measuring.

The supervisor, child-process load, crash attribution and ledger write all already exist.
This is argument parsing plus a loop over machinery that is there.

### Step 2 — read the names from EVERY EQ, not a sample

Enumerate every AU and VST3 component on the machine whose category resolves to `eq`
(the scan cache already holds category and there are ~1,400 AU + ~860 VST3 descriptions),
and dump each one's parameter names to a file.

**A corpus, not a sample.** Two or three plugins cannot distinguish "one convention with
a spelling variant" from "forty conventions", and those need different fixes.

Expect crashes and hangs across a run this size — quarantine-and-continue, and record
which plugins could not be read so the corpus states its own coverage rather than
implying completeness.

### Step 3 — derive the fix FROM THE CORPUS, and only then

**REPORT THE NAMING SCHEMES FOUND AND HOW MANY PRODUCTS EACH COVERS, BEFORE PROPOSING
ANYTHING.**

- If "Low / Mid / High spelled out" covers 40 products, the synonym fold is obvious and
  `prefixOrder()` widening is the right shape.
- If it is forty different conventions, **a synonym list is the wrong shape** and
  something else is needed — do not force it.

> **DO NOT PROPOSE A SYNONYM LIST BEFORE THE CORPUS IS READ.**
> Standing instruction from the user, and earned: *"every guess in this project has
> collapsed when measured, usually smaller."* The current hypothesis — that vintage EQs
> spell their names out — is RECOLLECTION of the products, not a reading of any parameter
> list, and it must not be treated as a finding at any point before step 2 completes.

### What is established (do not re-derive)

`EjmapBands.h:100` — `prefixOrder()` is `{ "LF", "LMF", "MF", "HMF", "HF" }`, matched
WHOLE-TOKEN via `prefixOrder().contains(toks[i], true)`. Two axes exist, `"digit"` and
`"prefix"`; **numbering is not required**. `"LF Freq"` → `["LF","Freq"]` matches;
`"Low Frequency"` → `["Low","Frequency"]` does not.

`strideNote` already names the parameter it failed on — it simply is not persisted (item 1).

---

## 5. Eiosis AirEQ's editor draws over the whole ejmap window

**Reported by the user, 4 Aug 2026.** Loading Eiosis AirEQ leaves only the plugin GUI and
the title bar: toolbar, list and wizard all disappear. Not a crash — the app is still
running. A plugin that hides the tool cannot be mapped, and this recurs on any oversized
editor.

**Established by source read at 6f610c8, not by watching it happen:**

- `layoutEditor()` (`MainComponent.h:9396`) does **one thing**: `setTopLeftPosition (0, 0)`.
  It never sets a size and never clamps one. The comment says why a transform is refused —
  *"M2 records mouse position inside these bounds and a transform would make ui_hint
  coordinates lie"* — so scaling is a decision already taken against, not an oversight.
- The editor is added to `editorHolder` (`MainComponent.h:9382`), which gets whatever is
  left of the window after the list/panel column (`MainComponent.h:6974`). An editor larger
  than that region overflows it, and nothing anywhere constrains it.
- **No plugin editor is being clamped today.** There is no call that sizes a hosted editor
  in `MainComponent.h`; `layoutEditor` is the only sizing code and it sets position only.
  That answers the second half of the question: nothing else is being clamped either.

**Why the overflow paints over the toolbar rather than being clipped by the parent** — a
hypothesis, but a well-supported one: a hosted AU/VST3 editor on macOS is a real `NSView`
child of the window's view, not a JUCE-painted component, so JUCE's parent-bounds clipping
does not apply to it. **This project has already solved exactly this**, in the main plugin:
`Source/NativeClip.mm:12` — *"wantsLayer + masksToBounds go on the CONTAINER; the plugin
view …"* — with `[container layer].masksToBounds = YES` at `NativeClip.mm:141`. That recipe
is also recorded in memory as the CHAIN inline-hosting rule: **a fixed clip-container NSView
with masksToBounds, the plugin view reparented into it, real NSView frames rather than JUCE
sizes, and never clamp the JUCE editor component.**

**So the fix has a proven shape already in this repo**, and it is the one that does not
touch coordinates: clip, do not scale. Scrolling the clipped container is the natural
follow-on for reaching the bottom of a tall editor. Scaling stays refused for the ui_hint
reason above unless someone first decides what a transform does to recorded mouse positions.

### MEASURED 4 Aug 2026 — and it REFUTES the size hypothesis

`--selftest-editorfit <plugin_id> [holdSeconds]` was built for this and now exists. It
loads one plugin, attaches the editor, prints the geometry at attach and again 3 s later
(a bridged editor "reaches its real size about 2.5 s" after createEditorIfNeeded,
`PluginHost.h:65`), and writes a JUCE-layer snapshot.

```
Eiosis AirEQ Premium  editor 1022x700 | holder 996x824 | window 1440x900 | OVERFLOWS
API-2500 (m)          editor  690x393 | holder 996x824 | fits
API-550A (m)          editor  316x597 | holder 996x824 | fits
```

**The overflow is 26 px, horizontally, off the RIGHT edge.** It cannot hide a toolbar at the
top and a list on the left. Three further facts, all measured:

- **The JUCE layer is correct.** `createComponentSnapshot` of the whole window shows the
  toolbar and the list drawn normally, with the editor's region blank — a hosted `NSView`
  never renders into a JUCE snapshot, so blank there is expected and proves nothing about
  the native layer, but toolbar-and-list-intact proves the layout is not the fault.
- **ejmap has exactly ONE window while AirEQ is loaded** (`AXStandardWindow`, 1440x928 at
  144,99, via System Events against the test process's pid). So AirEQ does **not** open its
  own window: its editor is a subview of ejmap's window. The "plugin opens a borderless
  window over the client area" explanation is out.
- **No editor is clamped**, confirming the earlier source read: `layoutEditor` only
  positions.

**NOT ESTABLISHED, and this is the gap:** the reported symptom was never reproduced on
screen. Every screen grab caught an empty Space — the test process's window opens on a
different Space from the one the capture tool sees, `screencapture -l<windowid>` returned
*"could not create image from window"*, and chasing it further was not worth the session.
**So the mechanism behind "toolbar, list and wizard all disappear" is still unexplained by
anything measured.** The measurements and the report disagree, and the rule is that the
measurement wins — which here means *do not build the clip container yet*, because it would
be a fix aimed at a mechanism nobody has seen.

**The cheapest way to close this** is on the mapping machine, where AirEQ and the app are
already in front of the operator: load it, screenshot the window, and note the window size
at the time. Two candidates the screenshot separates immediately —

1. an unclipped `NSView` covering more than its frame (the `NativeClip.mm:141`
   `masksToBounds` recipe is then the fix, and it is proven in this repo), or
2. a much smaller ejmap window at the time, which changes the arithmetic entirely — the
   test ran at 1440x900 and the holder was 996 wide, so a narrower window makes the same
   editor overflow far more.

**Also worth knowing:** AirEQ's editor is 1022x700, so at any window narrower than about
1530 px the editor cannot fit beside a 500 px control column. Whatever the mechanism turns
out to be, "the editor does not fit" is a real and recurring state, and the requirement
stands: ejmap's controls must stay reachable regardless of editor size.

**Also a test fixture:** AirEQ is the better manual-band-entry subject when item 4 lands —
seven bands each with Freq/Gain/Q, plus LoCut, HiCut, Earth and Air. Bigger and cleaner
than API-550A. API-550A stays the acceptance test (it is the parked one); AirEQ is the
second subject that shows the flow scales past three bands.

---

## 6. The category list does not cover everything worth mapping, and cannot say "not worth mapping"

**Raised by the user, 4 Aug 2026, from two live cases.**

**Auto-Key** — key detection, three parameters, no audio processing, output goes to
Auto-Tune. None of the categories fit. Skipping it is right, but the dropdown offers no
"not a processor", so the honest answer is only reachable by abandoning the plugin.

**Auto-Tune** — the opposite: genuinely dialable (retune speed, humanize, formant, key,
scale), users would ask for it, and pitch correction is not a category either. Same for
tuners, meters, analysers and utility plugins.

### What the picker actually offers (source, 6f610c8)

`DialSets::categories()` (`EjmapAssignment.h:85`) returns **eleven**: compressor, limiter,
eq, de-esser, delay, reverb, saturation, gate, transient_shaper, channel_strip, amp_sim.
The dropdown is built straight off it (`EjmapAssignPanel.h:91`). There is a twelfth,
unreachable path: `forCategory()` falls through to `{ mix_pct, output_db }` for an unknown
string, but only a classifier verdict or an explicit override can put an unlisted category
there — the human cannot type one.

### Question 1 — a "not a processor" outcome. **Yes, and it is nearly free.**

What exists today: **S ("S skip plugin", `EjmapAssignPanel.h:112`)** marks every unresolved
row `deferred` with reason `"plugin skipped by mapper"`, persists the session, and returns to
the list. So a dismissal IS recorded — but:

- it is recorded as **deferred**, which means *not done yet*, and the corpus cannot tell it
  from a plugin the mapper simply ran out of time on;
- it is recorded **only in the local session file**. Submit requires ≥1 confirmed row
  (`actionSubmit`, `EjmapAssignPanel.h:1279`), so a dismissal never reaches the server. The
  corpus the user wants to be able to ask "considered and dismissed?" is on one machine.

Both are the project's own distinction — `noMap` versus `unmapped`, `not_present` versus
`deferred` — applied one level up, to the plugin instead of the row. The shape:

- a category or an S-variant that records **`not_a_processor`** with a reason, distinct from
  deferred;
- a **list state** for it, so column 1 stops calling it unmapped (this is the same missing
  vocabulary as item 2's parked state — build them together);
- whether it should reach the server is a **separate decision** with a cost: it would need a
  submit path that carries zero params, which the empty-map refusal currently forbids for
  good reason. Recommend recording locally first, and deciding server-side once the parked
  state exists, since both need the same "a statement about a plugin, not a map of it"
  channel.

### Question 2 — is pitch correction worth a category? **Say what it contains first.**

A `pitch_correction` dial set, checked against what the consumer can actually do:

| key | unit | consumer support |
|---|---|---|
| `retune_speed_ms` | ms | **free** — `semanticUnit()` (`EchoJayParamApply.h`) is suffix-driven: `_ms` → ms |
| `humanize_pct` | pct | **free** — `_pct` → pct |
| `formant_pct` or `formant_shift` | pct / none | free if `_pct`; a bare `formant_shift` has **no unit** and lands in the same "unitless, magnitude unverifiable" hole as `drive` |
| `mix_pct`, `output_db` | pct, dB | already in every dial set |
| key, scale | — | **not anchorable.** These are enumerations. They ride the existing `mode` / labels path (`NamedControl.kind == "mode"`, `EjmapSchema.h:225`), which was built before ejmap and is fed by Tier 2 — so they are reachable *as named controls*, not as dial-set semantics |

**So the consumer could use most of it with no new machinery**: the unit inference is
suffix-driven, so any `_ms`/`_pct`/`_db` key works the day it is invented; and `key`/`scale`
already have a home in Tier 2's labelled-mode entries.

**What is NOT established:** the server-side vocabulary. `lib/params-lib.js` is not in this
worktree, so whether ingest validates semantics against a fixed list is **unread**. If it
does, a new semantic is a two-repo change and that decides the cost. **Check that before
adding the category, not after.**

**Recommendation, stated as a recommendation:** add `pitch_correction` with
`{ retune_speed_ms, humanize_pct, formant_pct, mix_pct, output_db }` and let key/scale ride
Tier 2 — *after* the server vocabulary is read. It is a real request shape ("make the tuning
tighter"), it costs one array entry client-side, and Tier 2 already carries the rest.

---

## 7. No way to leave a loaded plugin and get back to the list

**Raised by the user, 4 Aug 2026:** once a plugin loads there is no exit. A wrong load, an
unmappable one (Auto-Key), or one worth parking — restart-map resets the wizard but keeps
you on the plugin. Same class as the other three: the wizard supports a state it gives no
way to reach, and the state here is *"not working on this plugin any more."*

**Correction from source (6f610c8), because it changes what to build:** an exit does exist
and is on screen. **S — "S skip plugin"** (`EjmapAssignPanel.h:112`, in the button strip,
visible whenever the wizard is) runs `actionSkipPlugin()` → `hooks.exitPanel` →
`endAssignment()` (`MainComponent.h`), which hides the panel and **shows the list again**.
The plugin **stays loaded**; nothing unloads it.

So the capability is there and the gap is narrower and sharper than "no exit":

1. **It is labelled as a verdict, not as an exit.** "Skip plugin" reads as *dismiss this
   plugin*, so it is not what you reach for when you mean *come back to this later*.
2. **It costs the row states.** Every unresolved row is written `deferred` with reason
   `"plugin skipped by mapper"` — a resolution recorded about rows the mapper never
   considered. Returning later means re-opening rows that now claim they were decided.
3. **It says nothing about the session being saved**, which is item 2's gap exactly.
4. Whether the user knew S was there is worth asking — if the answer is no, that is a
   **labelling defect** and half the fix is a word.

**Propose:**

- **A park exit, distinct from skip.** Leaves every row exactly as it is (no `deferred`
  sweep), persists the session — `persistSession()` already writes on every accept and
  `restoreSession()` already matches on `fp` — returns to the list, and **says so**: "AirEQ
  parked: 4 confirmed, 6 open, saved. It will be waiting."
- **Keep S as it is**, and reword it so the two read as different acts: *"S — dismiss this
  plugin (records every open row as deferred)"* versus *"ESC — park and return to the list"*.
- **A half-resolved row parks as it stands.** `armed` / `captured` / `swept` are honest
  descriptions of where the mapper stopped, and the sweep evidence is already in the row.
  Nothing should be promoted or demoted on the way out. **One thing to check:**
  `actionWiggle` does not call `persistSession()`, so a row armed and then parked may not
  reach disk in that state — the park exit must persist, not assume.
- **Unload or stay loaded?** **Unload.** Staying loaded keeps a plugin's editor, its
  timers and its audio thread alive behind the list, and the next load then runs against a
  process that is already hosting something — the exact condition `--load-once`'s separate
  child processes exist to avoid. Parking is *"I am done with this for now"*, and leaving it
  resident contradicts that. Counter-argument worth weighing: re-loading costs 0.5-1.6 s and
  a re-load is where crashes happen, so parking-and-returning twice is two more load
  attempts against a plugin that may be flaky. If that matters, the compromise is unload on
  park and **keep the session**, never the instance.

**This is the one that costs most at volume** — every wrong load currently costs the whole
plugin.

---

## 8. The two live issues from the handover, now checked

Both were carried in `ejmap-handover-prompt.md` with a "NOT VERIFIED" note. The note is now
resolved in one direction each. **Source-read at 6f610c8; neither was run.**

### PROVEN BY RUNNING IT, 4 Aug 2026: `--selftest-dupescape`

`ejmap --selftest-dupescape <plugin_id>` runs against API-2500 (m) itself, on a scratch
ledger root. It constructs the duplicate (and SAYS it constructs it — no keypress sequence
in this build reaches that state), sweeps the real plugin for real anchors, and then uses
only keys a human presses. **17 of 17 ok.** Both escapes already exist:

| the shape on disk | the keys | result |
|---|---|---|
| `[3]` claimed by `release_ms` AND `release_ms` | **W then D** | the loser defers, submit enabled |
| `[8]` claimed by `makeup_db` AND `output_db` | **W, re-touch, SPACE** | `INSISTED: [0] serves both makeup_db and output_db`, submit enabled |

So **nothing needed building for the un-confirm affordance**. W on a confirmed row is not
gated by state (`keyValid`, `:466` refuses `wiggle` only for `ignore` rows), it sets the row
`armed`, and the sweep anchors and resolvedIndex survive — evidence intact, verdict
withdrawn. The test asserts that survival explicitly.

**One real defect was found and fixed:** the stale reason. `actionSkip` overwrites
`skipReason`, every confirm path did not, and that is how a row superseded at 11:53:01 came
to sit on disk CONFIRMED while still carrying `"superseded: [3] confirmed for release_ms"`.
Cleared now at `actionWiggle` — the single choke point where a resolved row becomes
unresolved again — rather than at the four confirm sites. Two assertions cover it.

**Still not built, and still true:** W does not `persistSession()`, so a withdrawn verdict
does not reach disk until the next action writes. Left alone deliberately: persisting would
mean a crash mid-re-open loses a confirmation, and not persisting means the disk briefly
disagrees with the screen. That is a decision, not an oversight, and it wants deciding
rather than defaulting.

### 8a. The duplicate-index insist IS reachable. The handover's blocker reads the wrong path.

**The question asked:** does the review screen set `conflictWith` and re-open the rows on
detecting the duplicate? **It does not.** `openSummary()` (`EjmapAssignPanel.h:2384`) calls
`duplicateConflicts()`, prints the strings and disables submit. It touches no row state.
`conflictWith` is written in exactly **two** places, both at capture time
(`EjmapAssignPanel.h:799` in `actionSpace`, `:3069` in `finishCaptureWith`) — enumerated,
not inferred from a comment.

**But the insist is still reachable, and the route is already in the self-test.** `W` on a
confirmed row (`actionWiggle`, `:856-857`) clears `conflictWith` and sets the row to
`armed` — it is **not** blocked on confirmed rows (`keyValid`, `:466`: `wiggle` is refused
only for `ignore` rows). Re-touching the same control lands in `finishCaptureWith`, which
asks `confirmedHolderOf` — the *other* row still holds [8] — and raises the conflict card.
**SPACE then insists.** `MainComponent.h:6504-6526` drives precisely this sequence (`W` →
re-capture → *"re-capture raised the conflict card again"* → SPACE → `sharedInsisted`) and
asserts it passes.

`duplicateIndexConflicts` (`EjmapAssignment.h:372`) drops `sharedInsisted` rows from the
map entirely, so **one** of the pair carrying the flag clears the conflict; both is not
needed.

**So: probably nothing to build here.** What is missing is that nothing on the confirmed
row says the route exists — the legend offers `> - next row` and `W - re-open (re-capture)`
(`:2855-2861`) and the review's refusal text says *"W re-captures or re-opens, D defers"*
without connecting that to the shared-control decision.

**Prove it before believing it:** on API-2500 (m), W the `output_db` row, touch the output
knob, confirm the card names `makeup_db`, press SPACE, and check submit is enabled. If that
works, the fix is a sentence in the refusal, not a feature. **A guard is tested by
attempting the thing it refuses.**

### 8b. Clearing a confirmation: W already does most of it, and does not persist it

`W` on a confirmed row sets `armed` and **keeps `resolvedIndex` and the sweep** — evidence
intact, verdict withdrawn, which is the affordance the handover asked for. Two real gaps:

- **It arms a capture.** There is no "un-confirm and stop"; the mapper must either touch
  something or navigate away (`clearCaptureTargets`, `:430`, fires on row change).
- **It does not persist.** `actionWiggle` calls `list.updateContent()` and no
  `persistSession()`, so disk still says `confirmed` until something else writes. A crash
  between the two leaves the withdrawn verdict standing.

---

## 9. MEASURED: API-2500 (m) has a second duplicate the insist cannot fix

**Read 4 Aug 2026 12:22Z from `assign-c1c6a082…json`, mtime 12:09:39Z (13 minutes before
the read, and the app was running — this is a sample, not a state).**

`AudioUnit:Effects/aufx,APCM,ksWV` carries **two** duplicate-index conflicts, not one:

| index | claimed by | resolved_at |
|---|---|---|
| 8 | `makeup_db`, `output_db` | 11:53:38, 11:54:42 (31 Jul) |
| **3** | **`release_ms`, `release_ms`** | 11:53:01, 11:54:30 (31 Jul) |

The second is **the same semantic twice on the same index** — and the losing row still
carries `skip_reason: "superseded: [3] confirmed for release_ms"` while being `confirmed`.

**What the file says happened**, in its own timestamps: the row proposing [10] "Release
Variable" was W-captured onto [3] and confirmed at 11:53:01; `supersedeSiblings`
(`EjmapAssignPanel.h:3113`) deferred the sibling row proposing [3] "Release" and wrote it
that reason; then at 11:54:30 that deferred row was **confirmed anyway** (W re-opens a
resolved row, and nothing stops a superseded row being re-confirmed). Its stale
`skip_reason` was never cleared.

**Two consequences:**

1. `duplicateIndexConflicts` emits **`"[3] claimed by: release_ms AND release_ms"`** — a
   refusal that reads as nonsense and names no way out. The SPACE insist is the wrong
   remedy: `sharedInsisted` means *this plugin genuinely shares one control between two
   semantics*, which is false here. One of the two rows should simply cease to claim [3],
   and **nothing in the wizard can make a confirmed row stop claiming an index** — which is
   8b, arriving as a live blocker rather than a design point.
2. `supersedeSiblings` only touches `! r.isResolved()` rows, so it cannot supersede a
   sibling that is already confirmed. Confirming the same semantic twice is therefore
   reachable and unguarded.

**Fix candidates, not yet chosen:** clear `skip_reason` on re-confirm (a one-liner, and
independent); make a same-semantic duplicate a distinct refusal that offers to drop the
older row; or let `supersedeSiblings` supersede a confirmed sibling **with a recorded
reason**, which is the same "the verdict is withdrawn, the evidence survives" move as 8b.

**This is live** — API-2500 (m) cannot submit while it stands.

---

## 10. ANSWERS to the three graphic-EQ questions the handover asked before item 4

Source-read at 6f610c8. Question 3 is answered only to a floor and says so.

### Q1 — Can schema 2.3 express a fixed frequency with a gain index? **Almost, and the consumer is closer than the schema.**

`GroupSpec` (`EjmapSchema.h:264`) is `{ family, n, primary, params[], freqLo, freqHi }`, and
`params` is an array of `ParamMapping` — each of which needs an index. **A band whose
frequency is not a parameter cannot be a `ParamMapping`.** So the frequency has to live as
group metadata, and `freqLo`/`freqHi` are already exactly that shape.

The consumer already behaves correctly on such a group. `applyBands`
(`EchoJayParamApply.h:681`) picks a band by a **reach test against `freq_range`**, then
writes only the request keys the band actually carries — a missing key returns
*"band has no <key> control"* (`:793`) rather than failing the move. So *"cut 250 Hz by
3 dB"* against a group with a freq range covering 250 and a `gain_db` param **lands the gain
and declines the frequency, with a reason.** That is the correct behaviour and it is already
built.

**One blocker, and it is one function:** `groupIsEqBand` (`EchoJayParamApply.h:646`)
**requires both** `freq_hz` and `gain_db` to be usable objects, or the group is not an EQ
band at all — so a gain-only group is filtered out before the reach test ever runs, and
`applyBands` reports *"no EQ band group for this control on this plugin"*.

**What it would take**, therefore: (a) let `groupIsEqBand` accept a group with `gain_db` and
a declared frequency range but no `freq_hz` param; (b) have ejmap write the fixed frequency
into the group's range. **Note the range is not a point.** A 250 Hz slider on a ten-band EQ
covers a band, so `[250, 250]` would fail the reach test for a 240 Hz request and fall
through to *"first-free, none reached target"*. Octave-spaced bands want roughly
`f × 2^±0.5`, third-octave `f × 2^±(1/6)` — and **the honest source for that is the
plugin's own band spacing**, which manual entry can read off the labels the mapper is
already looking at. This is a consumer-side change in the shipping plugin, not only in
ejmap, so it is **cross-repo and needs the drift gate**.

### Q2 — The band card needs an N. **Confirmed: there is none, and D costs everything.**

While the band flow is live (`bandStep` in `capFreq1`/`capGain1`/`capQ1`/`capFreqLast`) the
dispatcher (`EjmapAssignPanel.h:509-548`) accepts only: `wiggle` (re-arm), `notpresent`
**only at `capQ1`**, and `defer`. Everything else answers *"Band card is waiting for a
touch. R re-arms, D leaves bands for later."* The legend agrees (`:2889-2894`). And `D`
does not skip the frequency — it exits the whole flow: *"Bands left for later; nothing
recorded."*

So on a graphic EQ the first card asks for band 1's frequency and the only honest answer
costs the entire bands row. **N at `capFreq1` must mean "this band has no frequency
control", not "no bands"** — and it is exactly the same one-line shape as the `capQ1` N that
already exists.

**This lands inside item 4, not beside it.** Item 4's synthesised rows are
`band N frequency` / `band N gain`; a graphic EQ answers `not_present` to every frequency
row and the group is built from gain plus a typed frequency. Which raises the design point
item 4's accepted spec did not cover: **when the frequency is not a parameter, the mapper
types the frequency as a number rather than sweeping it** — and item 4's ordering rule
("the tool derives the order from the swept frequency") **cannot run**, because there is no
sweep. For graphic EQs the ordering claim must come from the typed frequencies, recorded as
`ordering_source: "mapper"` rather than `"measured"`. That is a weaker claim and must be
recorded as one — do not let it inherit `"measured"`.

### Q3 — How wide is this? **Floor of ~10 descriptions by name; the real answer needs the corpus.**

Name search over `scan-cache.xml` (read 4 Aug 2026 12:21Z, cache mtime 10:03Z, **1,819
descriptions**): API-560 (m), API-560 (s), UAD API 560, GEQ Classic (m/s), GEQ Modern (m/s)
— call it **~10 descriptions across ~5 products**. F6, Q10 and Sontec matched the pattern
and are parametric; they are not members.

**That number is a floor and a weak one.** The defining property of this class is *a gain
control with no frequency parameter*, which **is not visible in a name** — the search cannot
see console strips with stepped frequencies, amp tone stacks, or any fixed-band EQ not
called "GEQ". So the breadth question is **the same question item 0 step 2 answers**: read
every EQ's parameter list, and count the ones whose bands have gain but no frequency. It
comes free with that corpus and cannot be had honestly before it.

**One narrowing that costs nothing:** the bands flow is reached **only when the category is
`eq`** (`EjmapAssignPanel.h:230`). `amp_sim` has no band-class semantics at all and
`channel_strip` gets flat `freq_hz`/`gain_db`, so neither hits this card. The blast radius
today is EQ-categorised plugins only.

---

## 11. Character boxes: is Tier 2 the right home? **Yes — and it already works end to end**

**Raised by the user, 4 Aug 2026, alongside item 6.** Scheps Parallel Particles has four
character controls — Thick, Air, Bite, Sub — plus input, output and a Sub frequency. It is a
parallel-processing multi-effect and no category fits: `saturation` would map three of seven
controls and miss the point of the plugin.

**A class, not a plugin:** modern character boxes whose controls are named for what they do
rather than for a standard semantic. Decapitator, Soothe, the Scheps range, most one-knob
plugins.

### Can the consumer dial a named control it has no semantic for? **Yes. Built, and fed.**

`applySettings` (`EchoJayParamApply.h:851-884`) — when a settings key has no Tier 1 entry in
`map.params`, it resolves the key **by exact, case-sensitive name against `map.controls`** and
dials it through `applyOne`, the *same* anchor path a semantic uses. Anchored entries ride the
anchor path; `kind: "mode"` entries ride the labels path. A duplicate name refuses with both
indices — *"two controls wearing one name means neither is addressable by it."* An unknown
name declines with *"no mapping for this control on this plugin"*.

So `{ "Bite": 6.5 }` on a Scheps map is a real dial, with real anchors and real read-back.
**Nothing needs building on the consumer for this.**

### And ejmap can already submit such a map

`actionControlsAccept` (`EjmapAssignPanel.h`) sets the controls row to **confirmed**, and
`actionSubmit` only requires `confirmed >= 1` — which that row satisfies on its own. So a map
with **zero Tier 1 params and a full Tier 2 surface is submittable today**, by choosing any
category, dismissing its dial-set rows, and accepting the controls row.

**The gap is therefore honesty and friction, not capability:**

- the mapper must pick a category that is **false** (`saturation` on a parallel multi-effect),
  and the map then carries that false category;
- 5-9 manufactured dial-set rows must each be dismissed, and each dismissal is a recorded
  claim about a semantic the plugin was never going to have;
- nothing in the map says *"this plugin's whole surface is Tier 2 by decision"* as opposed to
  *"someone mapped three rows and gave up."*

**So the dropdown option the user asked for is right, and it is small:** a
`controls_only` (or `character`) category whose `DialSets::forCategory()` returns an **empty**
array, so `buildRows` produces the controls row and nothing else. `forCategory` already has an
unknown-category fallback of `{ mix_pct, output_db }` — this needs an explicit empty entry, not
that fallback, because even those two are a claim.

### Two things that are NOT established, and one that bites later

1. **The model is not told control names.** `standardChainInjections`
   (`PluginEditor.cpp:14656`) injects **plugin names only** — `buildChainInjection(recommendable)`.
   Nothing in the client feed carries a map's control names. So *"more bite"* → `{"Bite": …}`
   depends on the model already knowing the product's controls, or on a server-side prompt that
   carries them. **The server prompt is not in this worktree and was not read.** This decides
   whether these maps are USED, and it is the single most important unknown in this item —
   answer it before mapping a shelf of character boxes.
2. **`dialable` is stamped server-side** by `mapClearsCategoryBar` over params **and groups**
   (`EchoJayParamApply.h:594-605`). A controls-only map has neither, so it will almost
   certainly read **not dialable**. Today that gates only `kDialSignalsEnabled`, which is
   **false** (`:612`), so it costs nothing live — but when dial signals are enabled these
   plugins never light up. Either the flag learns about Tier 2 surfaces, or this class is
   permanently invisible to that feature.
3. Verify (1) before deciding these are worth mapping at all. If the model cannot name the
   controls, a Scheps map is dialable by a request nobody can phrase — and then this class
   belongs with Auto-Key in item 6 as a deliberate non-processor, recorded and dismissed
   rather than mapped.

---

## 12. THE HUMAN-TYPED READBACK GATE — proposal only, agree the shape before building

**The case, measured 4 Aug 2026.** TBTECH Cenozoix Compressor's map is refused by
`structuralGate` with *"params present but zero matching readback evidence: the map was never
write-back verified"*. Its six readback entries all carry an **empty** `read` — the plugin
returns nothing from `getCurrentValueAsText()` — and every one of its params is
`method: "human-typed"` with 5 anchors. The mapper typed the table **precisely because** the
display was unreadable. This is the plugin already on record as 99 params with 0 nameable
controls.

So the gate demands a display readback from a plugin that has no display, and the answer it
demands cannot exist. **A human-typed table is stronger evidence about the mapping than a
display read, not weaker** — a human read the plugin's own UI and wrote down what it said,
where a display read only proves the host can echo a number back to itself.

### The shape proposed

**The gate should stop asking one question of two different things.** Today it asks "did any
parameter verify by readback", which conflates *was this map verified* with *was it verified
BY READBACK*. Split it:

- a param whose `method` is `setread` or `gettext` **must** carry a matching readback, exactly
  as now — nothing is relaxed for the ordinary path;
- a param whose `method` is `human-typed` satisfies the gate **without** a readback, because
  the verification it carries is a different and declared one.

**What stops it becoming a bypass**, which is the real risk and the reason not to build this
yet:

1. **`human-typed` must be earned, not asserted.** It is set by the typed-anchor flow, which
   runs only after a sweep has been ATTEMPTED and refused (`sw.flat`, or a non-parsing
   display). The gate should require that evidence to be present — the map should carry why
   the sweep could not be used, per parameter. Today it records the method and not the
   reason, so **this is the piece that needs building first**, and it is the honest place to
   start.
2. **A typed table still has to be internally coherent**: monotonic, at least N points, values
   inside the parameter's own declared range. A typed table that is not monotonic is a
   mis-typing and should refuse.
3. **The map must SAY which lane each param passed by.** A consumer reading the map should be
   able to tell a display-verified param from a typed one without inferring it from `method`,
   and the count should appear in the submit line — "readback 1/2, typed 4/4" rather than a
   single ratio that hides the mixture.
4. **A map where EVERY param is typed and NONE was verifiable is worth flagging** even if it
   passes, because that is the shape a fabricated map would also have. Flag, not refuse: it
   is also the shape a legitimately display-less plugin has, which is exactly Cenozoix.

### What is NOT proposed

Relaxing the readback requirement for `setread` params. The three rejections that started
this were ADA (a real parser defect, fixed), Cenozoix (this item), and SPL Vitalizer (parked,
below) — and only Cenozoix's is an argument about the gate's question being wrong.

**Do not build until the shape is agreed.** The gate decides what enters the corpus, and a
bypass here is indistinguishable from a fabricated map at the point where it matters.

---

## 13. PARKED: SPL Vitalizer MK2-T, two indices returning one display

`drive` at index 2 ("Drive") and `output_db` at index 10 ("Output Gain"), 17 anchors each,
both `setread`. On a live re-verify both read back the **same** string, `-2.89 dB`, for
different asked values (2.355 and 1.57).

Two different indices returning one display needs a live probe to explain — a shared meter, a
read landing on the wrong control, or a plugin that ignores host writes without its UI open.
**Not diagnosed, and deliberately not guessed at**: an asserted cause gets believed and acted
on. One plugin, parked; ADA (fixed) and Cenozoix (item 12) are the general cases.

---

## 12b. THE SHIPPING ORDER, agreed 7 Aug 2026

1. full stage-2 run over the 1,108 lands
2. the NONE CLUSTERS -- the vocabulary answer the 40-map pilot could not support
3. THE DASHBOARD SESSION, one sitting, in this order: per-mapper auth (everything
   authenticates against it), categorise (largest win), stage 2 + capture on
   ingest, the return path, marks + withdraw, settings_structured passthrough
4. the dial-report endpoint and the halt mechanism -- `docs/HALT_DESIGN.md` --
   because that is what makes shipping model-proposed semantics safe
5. the client-side queue UI, once the endpoints are real

THE REVIEW PILE IS NOT DRAINED FIRST, by decision. ~12,850 questions is not a
session; the field orders them better than we can, since a wrong dial surfaces
on the controls people actually dial. Everything unreviewed stays
NAME-ADDRESSABLE, which is why leaving it costs a generic phrasing rather than a
control.

---

## 13a. AFTER THE CAMPAIGN: bx_rooMS index 45, a text liar that kills the process

Not urgent, and deliberately not chased mid-campaign. Recorded because the
evidence is unusually clean and will not be this clean later.

The last ledger row before ALL SIX consecutive restarts on 7 Aug was identical:

    bx_rooMS  idx 45: text liar (gettext) flat via gettext; set-then-read
                      readback-verify fa...

Same plugin, same parameter, every time. The plugin LOADS fine (7 ok rows) and
sweeps 44 parameters fine; index 45 is the one that takes the process down, and
it does so while the sweeper is falling back from a flat gettext read to the
set-then-read path. So the suspicion is that the crash is in the FALLBACK, not
in the parameter -- which would make it a bug in our sweeper reachable by any
text liar, not a quirk of one Brainworx plugin.

Worth one focused look with --no-supervise --sweep --sweep-limit 1 once the
campaign is done: what parameter 45 is, and whether set-then-read on a flat
control is the actual killer. If it is, the same crash is waiting on every
text liar in the catalogue.

The plugin is off the worklist meanwhile: three unfinished attempts quarantine
it without anyone watching, which is the general guard and the part that
mattered.

---

## 13b. KNOWN, BOUNDED, UNEXPLAINED: SIGABRT in JUCE's quit path

`docs/KNOWN_UNEXPLAINED_ABORT.md`. NOT closed, NOT fixed, deliberately left.

Signature: `NSInternalInconsistencyException`, "Periodic events are already
being generated", thrown from JUCE's own unbalanced `startPeriodicEvents` in
`shutdownNSApp`. Fires AFTER the work is finished, so it destroys nothing.
Costs one process restart; the supervisor's progress exemption absorbs it (152
plugins banked, restart not charged, sweep resumed itself).

Two mitigations are in the tree and NEITHER prevented the last recurrence. Two
observed facts do not reconcile -- the resume marker is deleted before the throw
yet the sweep resumed after it -- and that contradiction is where anyone picking
this up should start.

The one line that would close it, `quitNow: ALREADY QUITTING`, is wired to
stdout, stderr and the status strip. It has never appeared.

---

## 14. WAITING ON THE DASHBOARD REPO — now THREE items, one session

> **SPECIFIED 5 Aug 2026: `docs/SERVER_CONTRACT.md`.** The pipeline joined these
> two, because all three share the same auth and the same storage keys and
> splitting them would decide the same questions three times. Build against the
> contract, not against a stub.
>
> **14a is smaller than written.** The item asks for a `settings_by_name` field.
> It is not needed: `applySettings` has resolved an unmapped `settings_structured`
> key by EXACT control name against `map.controls` since 31 July, so
> `{"threshold_db": -18, "Sustain": 4}` already works in ONE object with Tier 1
> taking precedence. This item was written on 4 August and did not account for
> it. What the server needs is to PASS control names through, not a new field --
> a second key space for one namespace would also need a merge rule.

Both are half-built here and worthless until the server half lands. They are listed together
so they can be done in one sitting rather than two.

### 14a. `settings_by_name` — the return path for a named control

**Built client-side (4 Aug 2026):** `ChainHost::rackedControlSurface()` and the
`[RACKED PLUGIN CONTROLS]` block in `standardChainInjections`. The model is now TOLD every
control of every racked plugin, by name, with range and unit.

**What is missing is somewhere for it to answer.** The chain block's `settings` field is free
prose (`"4:1, -18dB thr, 30ms att"`) that is drawn on a card and never parsed. The field that
actually dials is `settings_structured`, composed SERVER-SIDE (`ChainHost.cpp:919` — *"the
server sends the same settings_structured object"*). So:

- the block format needs a `settings_by_name: {"Sustain": 4}` field;
- whatever composes `settings_structured` must pass those through.

`applySettings` already resolves an unknown key by EXACT, case-sensitive name against
`map.controls` and declines anything else with a reason. That is the only matcher, by
decision. **No fuzzy matching** — see item 15.

### 14b. `unmappable` transport

The local half is built (`marks.json`, keyed `format|uid`). To travel it needs an
authenticated POST beside the ingest one, storage keyed on the product rather than the fp, and
`identities=` extended to return the flag. The record is already shaped `{by, at}` so it
uploads unchanged.

---

## 15. NO FUZZY MATCHING UNTIL THERE IS A FEEDBACK PATH

Recorded because it will be proposed again the first time a model names a control slightly
wrong.

**The blocker is not the matcher, it is that nothing reports back.** Measured 4 Aug 2026: an
unmatched settings key returns `"no mapping for this control on this plugin"`, and
`ChainHost` routes it into `s.dialManual`, which surfaces on the card as *needs hand-dialing*.
The USER sees it. **The model never does** — the chat is a single `postJSON` with no
tool-call loop and no second turn. So a fuzzy match that picks the wrong knob is silent to
the thing that made the request.

If it is ever added it must REFUSE, not guess, on: two controls within the same edit distance;
any match crossing a unit family (`Drive` -> `Drive Mix`); and any numeric request matched to
a `mode` control. And it needs the feedback path first.

---

## 16. THE CHAT DOES NOT STREAM — a gap independent of any feature

Established 4 Aug 2026 while investigating the control-surface work. There is no
`text/event-stream`, no partial-delta handling: `EchoJayAPI` sends one `postJSON` and waits.
The typewriter reveal (`PluginEditor.cpp:2487`) animates an ALREADY-COMPLETE reply.

**What this changes:** any "show progress while it works" idea buys a longer wait on a blank
bubble rather than visible work. That applies to a per-plugin resolution pass, to a tool-call
loop, and to anything else slow enough to want narrating -- so it is worth fixing on its own
terms rather than as part of whichever feature next needs it.

The scope is real, not cosmetic: SSE or chunked reads in `EchoJayAPI`, a partial-reply sink on
the editor, and a decision about what happens to the typewriter when text arrives in chunks
rather than all at once.

---

## 17. ONE SEMANTIC PER `params` KEY — a real shape the catalogue contains

Raised 5 Aug 2026, out of the proposer's first run. **Not absorbed into band routing,
because band routing cannot fix it.**

`params` is a JSON object keyed by semantic, so a plugin can record exactly one
`mix_pct`, one `release_ms`, one `output_db`. The catalogue contains plugins where
that is genuinely too few:

```
UAD Vertigo VSM-3      'FETMix' / 'THDMix' / 'ZnBlMix'   three saturation stages
UAD 4K Channel Strip   'CMP Release' / 'EXP Release'     compressor vs expander
UAD Neve 88RS Legacy   'G/E REL' / 'L/C REL'             gate/exp vs lim/comp
```

These are not bands, not channels, and not a naming problem. They are different
controls that each legitimately want the same semantic. 8 of the 78 duplicate-semantic
rows in the first run are this shape, across 4 plugins.

**Today the second write silently wins** — which is what `duplicateSemanticConflicts`
(commit da31112) now refuses at both the review screen and the submit path, so the
map can no longer be quietly wrong. The refusal is correct and it is not an answer:
the mapper is now told they cannot record what the plugin actually has.

**Options, none chosen:**

- **(a) One is Tier 1, the rest stay Tier 2.** Costs nothing and works today —
  `applySettings` already resolves an unmapped semantic by exact, case-sensitive
  control name against `map.controls`. "Set CMP Release to 200 ms" would work;
  "set release to 200 ms" would reach whichever one was promoted.
- **(b) A disambiguated key** — `release_ms@cmp`. Touches the schema, the server
  map-builder, the apply path and every consumer. Large.
- **(c) A stage qualifier on the param entry**, leaving the key alone. Smaller than
  (b), but the key still collides, so it does not actually solve the recording
  problem.

**(a) is the honest default and needs measuring first**: how often does a chat
request name a stage ("compressor release") versus a bare semantic ("release")? That
is answerable from the chat corpus and should be answered before anything is built.

**UPDATED 8 Aug 2026 — this item now caps the vocabulary work, and the cap is
large.** The twelve additions of `docs/VOCABULARY_ADDITIONS.md` land on 1,235
controls, but item 17 means at most **~503** can become Tier 1 rows: one per
plugin per semantic, regardless of how many controls carry the word. The cap is
very uneven and is now measured per semantic:

```
slope_db_oct   91% of its plugins have MORE THAN ONE      <- item 17 eats 83% of its count
pan            86%
reverb_size    76%
hold_ms        58%      balance 50%      range_db 44%
width_pct      41%      depth_pct 37%    tone 27%    mod_rate_hz 23%
density         0%      tempo_bpm 0%                       <- free of this item entirely
```

Whoever decides between (a), (b) and (c) should read that table first: it is the
first real measurement of how much (a) actually costs, and it did not exist
before the 1,095-map corpus.

---

## 18. THE `attack_ms` VOCABULARY IS STRAINING — a dial-set question, not a classification one

Raised 5 Aug 2026 by the mapper working the review pile. **Collecting cases, not
yet deciding.**

`attack_ms` is being asked to cover four different things and only one of them is
a time:

```
UAD SPL Transient Designer  'Attack'       dB of transient gain, sweeps -15..+15 dB
FG-X 2                      'Comp Attack'  an arbitrary 0-10 scale, no unit
ValhallaVintageVerb         'Attack'       reverb onset SHAPE, not a time constant
(the intended meaning)                     milliseconds to full gain reduction
```

**This is why the Transient Designer row was wrong in the truth set, and why no
classifier could have got it right**: there is no correct answer in the
vocabulary, so refusing was the only honest outcome. The unit-family rule catches
the dB case because it contradicts a measured unit; it cannot catch FG-X 2's
unitless 0-10 scale or Valhalla's shape control, because neither declares
anything to contradict.

**It is a dial-set question.** `DialSets::forCategory` decides what a category is
asked about, and a transient shaper's "attack" is not a compressor's. Options,
none chosen:

- **(a) Per-category meaning.** `attack_ms` under `transient_shaper` means
  something different from `attack_ms` under `compressor`. Cheap, and dishonest:
  the key still says `_ms`, and the unit-family rule would then have to be
  category-aware to stop refusing it.
- **(b) New semantics.** `attack_amount` (dimensionless), `attack_shape`.
  Honest, and every one costs a server map-builder change plus consumer support.
- **(c) Refuse the category.** A transient shaper's attack is simply not
  addressable until there is a semantic for it, and the control stays Tier 2,
  reachable by exact name. Free, and narrows what "set the attack" can do.

**Do not decide on three cases.** The mapper is collecting more while working the
pile; the same question is likely live for `release_ms` (FG-X 2 has a matching
`Comp Release`) and for `sensitivity`.

---

## 19. PARTIAL BAND SETS ARE A CHANNEL-STRIP PATTERN, and the panel may recover them

Raised 5 Aug 2026 from the first band-routing run. Both partial band sets in the
corpus are **channel strips**; all four complete ones are **dedicated EQs**:

```
PARTIAL   UAD 4K Channel Strip        4 bands offered, 2 placed (HF, HMF missing)
PARTIAL   UAD Neve 88RS Legacy        4 bands offered, 2 placed (HF, HMF missing)
complete  API-550A (m), API-550A (s), AMEK EQ 200, Dangerous BAX EQ Mix
```

Both fail the same way: **on a channel strip the EQ section's HF and HMF
frequency displays declare no unit**, so no ordinal can be measured and the set
is refused as partial (correctly -- proposing the two would claim the plugin has
two bands). The LF and LMF frequencies on the same plugins DO declare hz.

**Stated as a pattern rather than two cases, because it predicts the catalogue**:
channel strips are a large category, and this says their EQ sections will
routinely arrive half-orderable.

**And it points straight at the screenshot stage.** Those panels print the
frequencies the display omits -- the same shape as the Maag EQ4 case that
motivated stage 4. So the missing evidence is plausibly RECOVERABLE rather than
absent, which would turn both partials complete. **Worth testing on 4K and Neve
before building anything else for partial sets**: two captures, two vision calls,
and the answer is measured rather than assumed.

---

## 20. AMP SIM TONE CONTROLS HAVE NO SEMANTIC — the second vocabulary gap

Raised 5 Aug 2026 by the mapper working the review pile. **The second entry on the
same list as item 18 (`attack_ms`), and the larger of the two.**

Presence, Bass, Middle and Treble on a guitar or bass amp are tone shaping on
**arbitrary 0-10 scales with no dB relationship**. `DialSets::forCategory
("amp_sim")` offers `{drive, input_db, output_db, mix_pct}` — no home for any of
them.

**The evidence is a withdrawal, not an opinion.** 14 decisions were made
answering `gain_db` across four amp sims, then withdrawn in a batch once the
pattern was clear (`tools/propose/revert.py`, outcome `reverted`, originals kept
under `superseded`). The reasoning that withdrew them:

- the scales are arbitrary, so `gain_db` is a false unit claim — and once these
  plugins record a swept `unit`, the unit-family rule will refuse it anyway
- `gain_db` on an amp sim is **ambiguous with the amp's own gain and volume**,
  which are separately mapped as `drive` on the same plugins

**It is large, not marginal.** 5 of 40 mapped plugins are `amp_sim` (12.5%) and
they carry **28 tone-shaped controls between them** — 4 or 5 each, and 11 on UAD
Softube Vintage Amp Room. Every amp sim in the catalogue has three or four. At
1,300 plugins this is the single biggest block of controls the dial set cannot
name.

**Options, none chosen** (and note the same three shapes as item 18):

- **(a) New semantics** — `tone_bass`, `tone_mid`, `tone_treble`, `presence`,
  dimensionless by design. Honest, names what the user actually asks for
  ("more presence"), and costs a server map-builder change plus consumer support.
- **(b) One parameterised semantic** — `tone` with a band qualifier. Runs into
  item 17: `params` is keyed by semantic, so three tone controls collide.
- **(c) Leave them Tier 2**, reachable by exact name. Free, and "turn up the
  treble" then only works if the model knows the product's control names —
  which is exactly the semantic-versus-named distinction the tier question is
  about.

**Do not decide (a) vs (c) without the tier answer.** If the map becomes one
control surface where some entries carry a semantic and some do not, (c) stops
being a demotion and becomes the ordinary case, and the question reduces to
whether "treble" is worth a standard name. That analysis is owed and should come
first.

**Collecting continues.** Item 18 (`attack_ms` covering four things) and this are
the first two; `release_ms` and `sensitivity` are suspected. The list is the
input to a dial-set revision, not four separate patches.

---

## 21. ONE CONTROL SURFACE — the dict shape is what makes the collision silent

Reasoning recorded 5 Aug 2026 while fresh. **Agreed as right and agreed to wait**
until after stage 1 and the catalogue sweep: it touches the server map-builder
and every consumer, and 40 maps weighted 11 compressor / 7 EQ is not a corpus to
design a schema against.

### What the boundary actually is

Measured: **122 of 125 param indices do not appear in `controls` at all.**
`params` and `controls` are DISJOINT. So the tier boundary is not "what the
wizard asked about" -- it is **addressed-by-semantic against addressed-by-name**,
which is the distinction that survives the mapping flow:

| | `params` | `controls` |
|---|---|---|
| key | the semantic | the exact name |
| uniqueness | one per map (a dict key) | one per name (duplicates refused) |
| serves | "set the threshold to -20" | "Sustain: 4" |

Load-bearing: `applySettings` (two lookups because two key spaces),
`Exposure::build` (reads `controls` and `groups`, NEVER `params` -- the twelve
exposed are the named surface), and dialability (counts usable params).

Vestigial: the exposure `tier` field (`"primary"|"hidden"`, human-set, **0 of 600
controls carry it**, and not Tier 1/2 at all), and the dial set as a wizard
checklist -- which survives only as the per-category semantic vocabulary.

### Why one surface is right

**THE DICT SHAPE IS NOT A SAFETY PROPERTY. IT IS THE CAUSE.** Two controls
claiming `output_db` cannot be REPRESENTED, so one wins and nothing records that
the other existed. `duplicateSemanticConflicts` (da31112) is a patch over a
representational hole, and the audit of the first review pile found **46
collisions across 27 of 40 plugins, 13 of which would have overwritten an
existing param**.

One surface -- entries carrying name, index, kind, anchors, unit, trust and an
OPTIONAL semantic -- makes that collision representable and therefore
**refusable rather than silent**. Uniqueness becomes a validated invariant
instead of an impossibility enforced at the wrong layer. Exposure could then see
everything and sort on semantic-presence rather than a field nobody sets.

It does NOT solve item 17 by itself: three controls wanting `mix_pct` still need
a qualifier. It turns a silent loss into a refusal, which is the same move as
every other gate landed this week.

**Prerequisite: a corpus that can support the design.** That means stage 1, then
the catalogue sweep.

---

## 22. A COHORT CAN HAVE TWO AXES — mixed band/channel sets

Raised 5 Aug 2026, when the cohort collapse landed. **Deliberately unsolved**, and
kept out of the cohort work rather than absorbed into it.

UAD Manley Massive Passive has seven controls claiming `q`:

```
Ch1LoBW  Ch1LoMidBW  Ch1HiMidBW  Ch1HiBW      <- band axis, channel 1
Ch2LoMidBW  Ch2HiMidBW  Ch2HiBW               <- the same bands, channel 2
```

They vary on **two** axes at once — a band axis (`Lo/LoMid/HiMid/Hi`) and a
channel axis (`Ch1/Ch2`). `find_axis` demands exactly one varying token position,
so it finds none, and the cohort is presented as "no shared axis".

**That is honest and it is the right behaviour today**: one question with no
channel hint beats a hint that guesses which axis it is. But it is not the
answer.

**The answer is probably that a cohort may carry two axes and the card says which
is which** — "4 bands across 2 channels; the bands are the grouping, the channels
are the duplication". That needs:

- `find_axis` generalised to find an ordered LIST of varying positions rather
  than refusing when there is more than one
- each position classified independently (band / channel / instance) with the
  existing set-based rule
- a disposition that can say "group on the band axis, dedupe on the channel
  axis" in one answer

**Do not attempt it before the catalogue sweep.** One plugin is not enough to
know whether two axes is the common mixed shape or whether three (channel x band
x instance) appears too, and the corpus that answers that does not exist yet.
