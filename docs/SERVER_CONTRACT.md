# The server contract

Written 5 Aug 2026, before any of it exists, so the dashboard session builds
against a written contract rather than deciding it as it goes.

**Scope: one server session covering three things that were waiting separately**
— the pipeline (categorise, propose, capture, queue, decisions), `settings_by_name`
(queue item 14a) and the `unmappable` transport (14b). They share auth, they share
storage keys, and splitting them would decide the same questions three times.

---

## 0. What the client already fixes — read this first

These are not preferences. They are shapes already compiled into a shipped
client, and the spec matches them rather than what would be tidiest.

### 0.1 Keys. Three of them, and they are not interchangeable

| key | derivation | source | what it identifies |
|---|---|---|---|
| **fingerprint** (`fp`) | `SHA256("format\|hex(uid)\|version\|paramCount")` | `EchoJayParamMaps.h:28` | one BUILD with a known parameter count. **Needs a loaded instance**, so a scan cannot produce it |
| **identity** | `format\|hex(uid)\|version` | `EchoJayParamMaps.h:43` | one BUILD. Available from a scan |
| **product** | `format\|hex(uid)` — version dropped | `EjmapMarks.h:70` | one PRODUCT across versions |

`hex(uid)` is `juce::String::toHexString(int)`: lower-case, **no leading zeros**,
no `0x`. Two-hex-digit values are two characters.

Why it matters: `unmappable` is a decision about a **product** and is carried
forward across updates; an `issue` is about a **build**. Storing either under the
wrong key silently changes what it means.

### 0.2 The ingest POST is byte-truth and cannot be negotiated

`EjmapSend.h` writes the artefact's exact bytes onto a TLS stream. **The TLS
plaintext IS the file on disk, byte for byte.** Consequences for the server:

- **HTTP/1.1.** No h2 upgrade. The client composes the request line and headers
  by hand and reads the raw reply.
- **Any 3xx is a refusal, never followed.** A redirect would send the body to an
  address the artefact does not name. Do not redirect `/api/params/ejmap` — not
  for a trailing slash, not for `www.`, not for HTTP→HTTPS.
- **A timeout is recorded as UNKNOWN and never retried**, because unknown is the
  one state a retry can double-submit from. So the server must be **idempotent
  on `fp` + body hash**: the same map submitted twice is one map.
- Non-2xx is a refusal and is not retried. Say why in the body.

### 0.3 Headers on every POST

```
X-EJMap-Version:  0.1.0            client build
X-EJMap-Machine:  <machine id>     stable per machine
X-EJMap-Tester:   <typed name>     a LABEL. Not an identity
X-EJMap-Token:    <credential>     the mapper token if signed in, else the shared ingest token
X-EJMap-Mapper:   <ref> | none     first 12 hex of SHA256(mapper token)
```

`X-EJMap-Mapper` lets a log attribute a request without holding a credential.
**The raw token never appears in a stored artefact**, only in this header.

### 0.4 The map payload, v2.3, as the client writes it

```
fp, schema:"2.3", identity{format,uid,name,vendor,version,param_count},
category, mode, params{semantic→{index,name,kind,anchors,trust,method,…}},
controls{name→{name,index,kind,range,unit,anchors,duplicate?,indices?}},
groups[], skips[], evidence{…}, provenance{tester_id,mapper_ref?,machine_id,
ejmap_version,extractor_version,apply_header_sha,plugin_version,host_os,at}
```

`params` is **keyed by semantic** and `controls` is **keyed by exact control
name**. Those two namespaces are the whole Tier 1 / Tier 2 distinction and the
server must not merge them.

### 0.5 The map-state GET already has a reply shape the client parses

`GET /api/params/maps?identities=a,b,c` (batched at 500, **no auth today**):

```json
{ "identities": { "AudioUnit|42f15d9|11.2.0": ["<fp>", …] },
  "maps":       { "<fp>": { "identity": {"param_count": 19}, "schema": "2.3",
                            "provenance": {"tester_id": "...", "at": "..."} } } }
```

An identity mapping to `[]` means **unmapped**. An identity **absent** from
`identities` means **unknown**, and the client is careful about the difference —
it will not claim "unmapped" on no evidence. Keep that distinction.

> ### CONTRACT CHANGE, not a server fix: the comma delimiter collides with real data
>
> Measured against production on 8 Aug 2026. `identities=` is comma-separated,
> and **OTT's version string is literally `1,3,7,0`** — so the identity
> `VST3|7c12157|1,3,7,0` arrived as four keys (`VST3|7c12157|1`, `3`, `7`, `0`)
> and 1,108 questions came back as 1,111 answers with one genuine absence.
>
> **The failure is silent and lands on the distinction directly above.** The
> client sees OTT as *unknown* rather than *unmapped*, so it correctly refuses
> to claim anything, and the plugin never resolves. One identity of 1,108 today;
> version strings may legally contain commas, so it is a latent defect, not a
> one-off.
>
> **Both ends need the same decision**, which is why this is a contract change:
> the client batches identities with the same separator. Either pick a delimiter
> that cannot appear in `format|uid|version` (newline, or `;`), or percent-encode
> each identity before joining. A server-side fix alone leaves the client
> emitting a request the server can only guess at.
>
> Whatever is chosen must be applied to ingest and lookup together — they share
> the encoding.

`provenance.tester_id` is currently **not returned**, and the client compensates
locally with a comment saying so. Returning `mapper_ref` here is the fix and the
client is already written to prefer it.

### 0.6 The proposal reader already exists

`ProposalSet::load(root, fp)` reads `<root>/proposals/<fp>.json`:

```json
{ "category": "compressor",
  "params": [ {"index": 7, "kind": "threshold_db", "confidence": "high",
               "reason": "…", "channel": ""} ] }
```

**Nothing needs building client-side to consume proposals** beyond fetching this
file and writing it to that path. `kind` is the semantic; `channel` is optional.

### 0.7 `categories.json` already has a shape the sweep reads

```json
{ "run": {"models": [...], "prompt_sha": "…", "tool": "categorise/1", "at": "…"},
  "products": { "cla-76|waves": {
      "name","vendor","formats":[],"declared","members":[],"mark_keys":[],
      "arms":[{"model","category","confidence","kind"}],
      "category", "confidence":"high|hedged", "kind", "kind_agreed",
      "disposition":"sweep|no_dial_set|not_a_processor|review",
      "refused_by_both": false, "why", "at" } } }
```

The product key is `lower(base_name)|lower(vendor)` where `base_name` strips a
trailing `(m)`, `(s)`, `(mono)`, `(stereo)` or `(NxN)`. **The collapse rule must
match**, or a mapper's `CLA-76 (m)` misses the cache entry for `CLA-76`.

`sweepable` = `disposition == "sweep" && !refused_by_both && category != ""`.

### 0.8 The marks record already uploads unchanged

`{ "issues": { "<identity>": {"by","at"} },
   "unmappable": { "<product>": {"by","at"} } }`

---

## 1. Auth

**One token per mapper, issued out of band, pasted once.** No accounts.

```
POST /api/auth/mapper/verify        {} + X-EJMap-Token
  200 {"ref":"c221cccc958a","name":"Sean","issued":"2026-08-05"}
  401 {"error":"unknown or revoked token"}
```

- `ref` **must equal** `SHA256(token)[0..12)` lower-case hex. The client computes
  it independently and stores it in maps; a server that derives it differently
  breaks attribution silently.
- Revocation is per token. A revoked token 401s everywhere.
- The shared `ingest_token` stays accepted during migration and is the thing to
  retire. Log which one authenticated a request.

**Every endpoint below requires a token except `GET /api/params/maps`**, which is
unauthenticated today and reading it is not sensitive.

---

## 2. Endpoints

### 2.1 `POST /api/params/categorise` — stage 0.5

**Sends** the scan census, one row per scanned plug-in:

```json
{"plugins":[{"name":"CLA-76 (m)","vendor":"Waves","format":"AudioUnit",
             "uid":"42f15d9","version":"14.0.0","declared":""}]}
```

~1,819 rows ≈ 220 KB. `declared` is the format's own subcategory string
(`"Fx|Dynamics"`), empty for every AudioUnit — the client already inherits it
across AU↔VST3 siblings before sending.

**Returns** the `products` object of §0.7, for the products asked about only.

```json
{"products": {"cla-76|waves": {"category":"compressor","confidence":"high",
                               "disposition":"sweep","refused_by_both":false,
                               "kind":null,"why":""}}}
```

**Does**: collapse to products, answer from cache, queue the unknown remainder
for the two-arm call. An unanswered product returns
`"disposition":"pending"` — the sweep skips it and it resolves on a later
refresh. **It must not return `"sweep"` with an empty category**; the client
treats that as not-sweepable but it is a contract violation, not a state.

The two-arm gate is `tools/propose/categorise.py`, ported unchanged: agree +
both confident → `sweep`; agree + one hedged → `sweep`, `confidence:"hedged"`;
agree `none` → `no_dial_set`; agree `not_a_processor` → `not_a_processor`;
otherwise `review`, which goes to **your** queue and never to the mapper's.

**Measured on this catalogue**: 1,073 products, $4.25, ~6 min; 62.3% sweep,
24.2% no_dial_set, 5.3% not_a_processor, 8.1% review. The acceptance test is
`categorise.py --selftest` against 40 hand-categorised maps: 103 both-confident
agreements across three runs, 103 matches.

### 2.2 `POST /api/params/ejmap` — submit a map (EXISTS)

Unchanged on the wire. Two additions to what the server does:

1. **Read `provenance.mapper_ref`** and store it as the submitter. Fall back to
   `tester_id` for the 40 maps that predate tokens.
2. **Run stage 2 on ingest** (§2.4). Not batched at the end of a sweep: a sweep
   runs for hours, so per-map means the queue is populated before the mapper
   looks at it.

Idempotent on `fp` + body hash (§0.2).

### 2.3 `POST /api/params/capture` — the panel image

```
POST /api/params/capture?fp=<fp>
Content-Type: image/png
X-EJMap-Capture-Fraction: 0.868
X-EJMap-Capture-Size: 2048x1140
```

**A separate request on purpose.** The map's transport is byte-truth (§0.2) and
a PNG inside that body would end it for the one artefact that has it.

**The client only sends a capture whose state is `ok`.** An `empty` capture is
recorded locally and the PNG is deleted, because a blank image in the corpus
looks like a panel nobody could read rather than a capture that produced nothing.

> **Never name a stored field after a cause.** The client records
> `capture: "ok" | "empty" | "unavailable"` with the fraction and dimensions.
> An earlier reading attributed empty captures to out-of-process hosting; it was
> disproved on 5 Aug — a Waves panel loaded **in-process**, with no `NSRemoteView`
> in the view tree, captures at 0.0% too. A field called `unavailable_bridged`
> would have frozen that wrong cause into the corpus.

Expect **no capture at all** for roughly a third of Waves products. That is a
known ceiling, not a fault.

### 2.4 Stage 2 — proposal, server-side, on ingest

No endpoint. It runs when a map arrives. Port `tools/propose/propose.py`
unchanged; its four gates can only refuse:

1. both arms name the same semantic
2. both are confident
3. the semantic's unit family does not contradict the swept display unit
4. no other control on the same plugin already claims that semantic

Anything else escalates to the queue (§2.5). Where a capture exists, escalations
get the vision pass first.

**Two rules the Python already encodes and the port must keep:**

- **A failed arm writes nothing.** Presence means done, so a transient 529 that
  wrote a file recorded an outage as a permanent result. 11 of 33 plugins were
  recorded 100% escalated by exactly this.
- **A confident agreed `none` is a decline, not an escalation.** Measured 57/57.

Settled proposals are written into the map. What is unsettled becomes a card.

### 2.5 `GET /api/params/queue` — the return path

```
GET /api/params/queue?count=1     -> {"count": 40}
GET /api/params/queue             -> {"count": 40, "cards": [ … ]}
```

Scoped to the requesting mapper's plugins. Yours is the same endpoint with a
wider scope — **one interface, because maintaining a second review tool is how
the two answers drift.**

A card carries what `review.py` renders:

```json
{"id":"…","fp":"…","plugin":"CLA-76","category":"compressor",
 "control":{"name":"Sustain","index":7,"kind":"continuous",
            "range":[0,10],"unit":null,"anchors":9,"span":[0,10]},
 "arms":[{"model":"claude-opus-5","semantic":"attack_ms","confidence":"low"},
         {"model":"gpt-5.5","semantic":"none","confidence":"high"}],
 "why_escalated":"arms disagree",
 "capture":{"available":true,"url":"…"}}
```

### 2.6 `POST /api/params/decisions` — what the answers do

```json
{"decisions":[{"id":"…","fp":"…","index":7,"outcome":"assigned",
               "semantic":"attack_ms","source":"human-corrected",
               "trust":"llm-classified"}]}
```

The server writes them into the map, re-runs the gate, and the map becomes
dialable. Two rules that must survive the move into a UI, because a card makes
answering **feel** more authoritative than it is:

- **Reading is not verifying.** Accepting or correcting by reading records
  `human-corrected` and leaves trust at `llm-classified`. Only touching the
  control produces `human-verified`, and only the client can witness that.
  **The server must refuse `trust: "human-verified"` from this endpoint.**
- **A mapper's answer settles their map, not the corpus.** Promoting one answer
  to corpus-wide truth for the same control on other plugins is yours. Otherwise
  the corpus is writable by anyone with the app.

### 2.7 `POST /api/params/marks` — the unmappable transport (item 14b)

```json
{"unmappable":{"AudioUnit|42f15d9":{"by":"c221cccc958a","at":"2026-08-05T…"}},
 "issues":{"AudioUnit|42f15d9|11.2.0":{"by":"…","at":"…"}}}
```

The local record is already shaped `{by, at}` and uploads unchanged.

- **`unmappable` stores on the PRODUCT key, `issues` on the IDENTITY key.** This
  is the one place the distinction is load-bearing: a product decision carried
  across versions versus a statement about one build.
- `GET /api/params/maps?identities=…` gains an `unmappable` set in the reply so
  the flag comes back. The client's row builder already consults local marks and
  will merge.
- **A mark is advisory and never a lock.** Neither mark gates loading, clearing
  is the same gesture that set it, and the server must not treat `unmappable` as
  a reason to reject a map for that product — someone may have decided it *is*
  mappable after all.

Client note: `marks.json` currently holds **zero** `unmappable` entries. The
57 `not_a_processor` proposals from stage 0.5 are a batch to accept, not a
measurement, and this endpoint is what makes accepting them travel.

### 2.8 `settings_structured` — item 14a, and the client half is already done

The queue item asks for a new `settings_by_name: {"Sustain": 4}` field. **It is
not needed, and adding it would be a second key space for one namespace.**

`applySettings` resolves every key of the flat `settings_structured` object in
this order (`EchoJayParamApply.h:904`):

1. `map.params[key]` — a Tier 1 semantic
2. `map.controls[key]` — an **exact, case-sensitive** control name (built 31 Jul)
3. otherwise declined with `"no mapping for this control on this plugin"`

So `{"threshold_db": -18, "Sustain": 4}` already works today, in one object, with
Tier 1 taking precedence on a collision.

> **The queue item was written on 4 Aug, after Tier 2 name resolution landed on
> 31 Jul, and did not account for it.** Re-measured before building.

**What the server must do:** whatever composes `settings_structured` may emit
exact control names as keys alongside semantics. Nothing else changes.

**And it must not fuzzy-match.** A key that is nearly a control name is declined,
with the reason. The blocker is not the matcher: an unmatched key surfaces to the
**user** as "needs hand-dialing" and the model never sees it — the chat is a
single `postJSON` with no tool-call loop — so a fuzzy match that picks the wrong
knob is silent to the thing that made the request. The model is already told
every racked control by name, with range and unit, by
`ChainHost::rackedControlSurface()`. **Duplicate names are deliberately not
offered**, because they are not addressable by name and naming them would invite
a request that can only be declined.


### 2.8b `POST /api/params/dial-report` — how a wrong semantic gets caught

**This is what makes shipping model-proposed semantics safe.** ~12,850
escalations are left unreviewed by choice; that is affordable only because a
wrong one gets stopped by the people using it.

```json
POST /api/params/dial-report
{"fp":"…","semantic":"attack_ms","index":7,
 "asked":-1.125,"landed_text":"-1.13 dB",
 "readback_mismatch":true,"display_verified":false,
 "kind":"automatic-mismatch|user-wrong-knob"}
```

Every field already exists in `ApplyResult`. The client already POSTs to
`/api/chat`, `/api/data` and `/api/login`, so this is one more call on an
existing transport.

**The detection is already built and already reverts the write.**
`typedReadbackMatch` (`EchoJayParamApply.h:273`) returns −1 when the landed
display's unit family contradicts the semantic's. What is missing is only that
the verdict never leaves the machine: it surfaces as "needs hand-dialing" on
the card (`ChainHost.cpp:1804`) and stops there.

**What a report does is `docs/HALT_DESIGN.md`**, and it is deliberately not
symmetric: an automatic mismatch halts that semantic on ONE report because it
is a measurement the client already acted on; a user report on a verified write
QUEUES and needs corroboration because it is an account nobody can check.
Never the map, never a tombstone.

### 2.9 `POST /api/params/withdraw` — a tombstone, not a delete

**There is no way to remove a bad map today, and "it is up there and I cannot
take it down" is not a state a corpus should have.** One instance is live: SSL
Fusion HF Compressor, fp `dfe4e61a85e6…`, accepted 4 Aug with HTTP 200, carrying
Dangerous BAX EQ Mix's six controls.

```json
POST /api/params/withdraw
{"fp":"dfe4e61a85e6…",
 "reason":"foreign_controls",
 "detail":"controls belong to Dangerous BAX EQ Mix, mapped 3s earlier; index 3 is
           'Output Trim' in params and 'Low Shelf Level' in controls"}
```

**A TOMBSTONE, and the reasoning is the client's own rule about absence.** A
deleted map is indistinguishable from one that never existed, and the client
already refuses to treat "unknown" as "unmapped" for exactly that reason. A
mapper who finds a fingerprint missing should learn it was **withdrawn**, not
wonder whether their submit failed.

So a withdrawal:

- **stops the map being served.** The dial path gets nothing for that fp — that
  is the urgent half, because until it happens a client dialling by control name
  gets another plugin's surface.
- **keeps the map, the reason, who withdrew it and when.** A wrong map is
  evidence about a defect; deleting it destroys the only artefact.
- **is visible in the read path.** `GET /api/params/maps` gains a `withdrawn`
  object beside `maps` and `identities`:

  ```json
  {"identities": {"AudioUnit|7a606966|1.1.3": []},
   "withdrawn":  {"dfe4e61a85e6…": {"at":"…","by":"c221cccc958a",
                                    "reason":"foreign_controls"}}}
  ```

  The identity maps to `[]` — **unmapped**, which is now true — and the
  `withdrawn` entry says why the fingerprint a mapper remembers submitting is no
  longer there. The client's existing empty-list handling already reads that as
  unmapped, so nothing breaks before the client learns the new field.

**Who may withdraw.** The submitter of that map, or the owner. Not any mapper —
the same rule as §2.6: a mapper's answer settles their own map, not the corpus.
An attempt on someone else's map is a 403, not a silent no-op.

**Re-submission is allowed and is the normal ending.** A withdrawn fingerprint
is re-mappable; a new submit for it supersedes the tombstone, and the record
keeps both, so "this was wrong once and re-done" is answerable.

**`reason` is a small enumeration, not free text** — `foreign_controls`,
`wrong_identity`, `bad_anchors`, `superseded`, `other` — with `detail` free
alongside. An enumeration is what makes "how often does this happen" a query
instead of a reading exercise.

**Withdrawal is not a halt.** A withdrawal is for a map whose CONTROLS WERE
MEASURED ON ANOTHER PLUGIN -- nothing in it is evidence about its own plugin. A
wrong SEMANTIC is the opposite: the anchors, ranges and indices are all real
measurements and only one row's label is wrong, so it is halted per-semantic
and stays name-addressable. See `docs/HALT_DESIGN.md`.

**Not a correction endpoint.** There is deliberately no way to edit a stored
map. A map whose controls were measured on another plugin has nothing in it to
repair, and an edit path would invite exactly that repair.

---

## 3. Storage

| store | key | holds |
|---|---|---|
| categories | **product** `lower(base)\|lower(vendor)` | category, both arms, confidence, kind, disposition, prompt hash, date |
| maps | **fp** | the payload, plus submitter `mapper_ref` |
| captures | **fp** | the PNG, the fraction, the dimensions |
| queue cards | **fp + index** | the escalation and its arms |
| decisions | **fp + index** | outcome, semantic, source, trust, who, when |
| marks: unmappable | **product** | `{by, at}` |
| marks: issues | **identity** | `{by, at}` |
| mappers | token hash | `ref`, name, issued, revoked |
| withdrawals | **fp** | the tombstone: reason, detail, by, at, and the map as submitted |

### What is retained from a census, and what is not

This is the paragraph a mapper is shown, so it is settled before the endpoint
exists rather than after someone asks.

- **Sent**: name, vendor, format, uid, version, declared subcategory.
- **Not sent**: no parameters, no audio, no presets, no project data. Nothing is
  loaded to produce it.
- **Kept, keyed by PRODUCT**: the category row. It contains no reference to any
  person, and it is the point — it makes the hundredth mapper's catalogue free.
- **Kept, keyed by MAPPER**: nothing from the census. The token answers who
  submitted a *map*, never what they own.
- **Discarded**: the association between a mapper and the products in their
  census, once the unknown ones are queued.

The last rule is right independently of privacy: a cache keyed by person answers
the same question once per mapper instead of once ever.

---

## 4. Build order, server-side

1. `POST /api/auth/mapper/verify` and per-token storage. Everything else
   authenticates against it.
2. `POST /api/params/categorise` — the largest win and it unblocks unattended
   sweeping for anyone who is not you.
3. Stage 2 on ingest, plus `POST /api/params/capture`.
4. `GET /api/params/queue` and `POST /api/params/decisions` — the return path.
5. `POST /api/params/marks` and `unmappable` in the identities reply.
7. `settings_structured` passthrough for control names (§2.8). Smallest of the
   seven and independent of the rest.

---

## 5. Not specified here, deliberately

- **Corpus-wide promotion of a mapper's decision.** Named as yours in §2.6 and
  left undesigned; it needs the wrongness-feedback tiers first.
- **The vision prompt.** Stage 4 has never run at volume; specifying its output
  before it has been measured would be inventing a contract.
- **Rate limits and quotas.** No numbers exist to base them on.
- **Anything about `no_dial_set` beyond storing it.** 260 products say which dial
  set to add next; that is a product decision, not an endpoint.
