# Surgical EQ — Backend (/api/chat) Integration Spec

This is the **last piece** to make the built-in EQ dialable by prompt. The plugin client is already done: it hosts "EchoJay EQ" as a chain device and applies an `eq_bands` move to *exact* per-band values, bypassing the anchor-table path used for third-party plugins. What's missing is the **server side** — the model that answers in EchoJay's chat (`/api/chat` on echojay.ai) must know the device exists and emit `eq_bands` for it.

> This spec lives in the plugin repo because it documents the **client↔server contract** (the client half is here and authoritative). The work itself is in the **backend repo/deployment** that serves `/api/chat` — implement it there (point a Claude Code session at that repo with this spec).

## How the client already consumes it (do not change the client)
The server already augments each chain entry with a server-validated `settings_structured` object (the client calls this out in `PluginEditor.cpp`). For the EchoJay EQ, put the move under `settings_structured.eq_bands`. Flow, all already built on the client:

1. Chain response entry: `{"name":"EchoJay EQ","role":"…","settings":"…","settings_structured":{"eq_bands":[ … ]}}`.
2. Client resolves the name to the built-in device and loads it (`ChainHost` built-in load path).
3. `ChainHost::applyStructuredIfReady` detects the built-in and calls `SurgicalEqProcessor::applyEqBands(settings_structured.eq_bands)` **directly** — no fingerprint, no map, no interpolation, no read-back/revert. Exact.

So the entire backend job is: **decide to use the EchoJay EQ, and emit a correct `eq_bands` array.** The client does the rest.

## The `eq_bands` schema (authoritative — from `SurgicalEqProcessor::specFromVar`)
`eq_bands` is an array of band objects. Fields:

| field | type | notes |
|---|---|---|
| `type` | string | `bell` \| `lowshelf` \| `highshelf` \| `notch` \| `highpass` \| `lowpass`. Tolerant of separators/case and common synonyms (`peak`→bell, `hpf`→highpass, `ls`→lowshelf…). Defaults to `bell` if omitted. |
| `freq_hz` | number | 20–20000. Center/corner frequency. |
| `gain_db` | number | bell & shelves only. Clamp to ±24 (UI range). Ignored for notch/HP/LP. |
| `q` | number | 0.1–100. Bandwidth/resonance. |
| `slope_db_oct` | int | HP/LP only. Multiple of 12: 12,24,36,…,96. |
| `band` | int | OPTIONAL, 1-based. Targets that exact band slot. Omit to auto-allocate the lowest free band. |
| `enabled` | bool | OPTIONAL, default true (a set enables). |
| `disable` | bool | OPTIONAL. Turns the target band off. |
| `dynamic` | object | OPTIONAL, **Bell only**. `{ "threshold_db", "range_db", "attack_ms", "release_ms" }` — threshold-driven (de-ess / resonance taming). `range_db` negative = downward (compressive). |

Semantics: applied as a **per-band merge** — explicit `band` targets exactly and is idempotent; auto-allocate never clobbers existing bands; overflow (all 24 in use) is reported, not silently dropped. So the model can send just the bands it wants to change.

### Examples
```json
{"eq_bands":[{"type":"bell","freq_hz":400,"gain_db":-3.5,"q":3.0}]}                     // tame boxiness
{"eq_bands":[{"type":"highpass","freq_hz":80,"slope_db_oct":24}]}                        // clean sub-80 rumble
{"eq_bands":[{"type":"notch","freq_hz":6100,"q":8}]}                                     // kill a resonance
{"eq_bands":[{"type":"bell","freq_hz":6500,"gain_db":0,"q":5,
              "dynamic":{"threshold_db":-20,"range_db":-6,"attack_ms":2,"release_ms":60}}]}  // de-ess
```

## Backend changes
1. **Register the device.** The model currently only sees the user's *scanned* plugins (injected via the plugin list). The EchoJay EQ is **built-in and always available** — add it to the device set the model may choose, in its own "EchoJay built-in devices" section so it's never filtered out by the user's installed list. One entry: name exactly `EchoJay EQ`.
2. **Describe it + when to use it.** In the system prompt, describe the EchoJay EQ as EchoJay's own precise, fully-parametric EQ that it can dial *exactly* (unlike third-party plugins, which are approximate). Guidance: **prefer the EchoJay EQ for surgical / precise moves** — specific-frequency cuts or boosts, high-pass/low-pass cleanup, notching resonances, and dynamic de-essing/resonance-taming (dynamic bells). Third-party EQs remain fine for character/vibe. (Tune this preference to taste — it's a product decision.)
3. **Teach the schema.** Give the model the `eq_bands` schema above with 2–3 examples, and instruct: when it places or edits the EchoJay EQ, emit the move under `settings_structured.eq_bands` on that chain entry. Only the bands being set need to be included (merge semantics).
4. **Validate/clamp server-side** (same layer that validates `settings_structured` today): coerce `type` to the allowed set, clamp `freq_hz` 20–20000, `gain_db` ±24, `q` 0.1–100, `slope_db_oct` to the nearest multiple of 12, and drop malformed bands. The client clamps too, but validate here so the move is sane before it ships.
5. **Multi-band moves** in a single response are expected — a "clean up the low mids and de-ess" request can be one `eq_bands` array with several entries.

## Known follow-up (client-side, not blocking)
Retuning an EQ that's *already in the chain* via a chain-**edit** op: the client's edit ops don't currently carry `settings_structured` (noted in `PluginEditor.cpp` ~14600). So "adjust the EQ in slot 2" either needs the client edit path extended to carry `eq_bands`, or the model re-emits the slot. For the first pass, "place an EchoJay EQ with these moves" (build/add path) works end-to-end; per-slot live retune is a fast follow once the edit path carries `eq_bands`.

## Acceptance
In EchoJay's chat, a prompt like "high-pass the rumble and take 3 dB out of the boxiness around 400" results in the model adding an EchoJay EQ whose `settings_structured.eq_bands` places a high-pass and a −3 dB bell at ~400 — and the client dials those bands to exactly those values (verifiable on the EQ's curve/readouts). No anchor map involved; the values land exactly as sent.
