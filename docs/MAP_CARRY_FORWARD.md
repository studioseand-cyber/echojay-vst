# Carry a map + proposals forward across a version bump

Written 13 Aug 2026. Measured claims are flagged. The core assumption is **not
yet measured** — see §0, which must pass before any of this is built.

## The waste

Maps and proposals are keyed per **fingerprint** (`format|uid|version|paramCount`).
Because `version` is in the key, a plugin update is a brand-new fingerprint, so
it gets **re-swept** (the set-then-read anchor pass) and **re-proposed** (the paid
two-arm call) even when its parameters are byte-for-byte identical to the version
already in the corpus. On the libraries that update most — Waves, UAD, NI — that
is the bulk of the recurring time and API cost.

What is already deduped, and stays that way: **categories** are keyed per
**product** (`format|uid`, version dropped), so a version bump does not
re-categorise. Only maps and proposals repeat.

## Why it's safe in principle

Plugins store settings by parameter **index**, so a vendor that reordered or
removed a control would break every saved session and preset using it. They don't:
existing controls keep their index, name, range, and meaning across versions.
What they do is **append** new controls on the end (defaults fill them in old
sessions). So a prior version's map is valid for every control it already covers;
a new version can only *add*, not *change*, the existing surface.

## §0 — Verify the assumption first (cheap, $0, do before building)

Everything here rests on "the measured anchors hold when only the version
changes." Confirm it on real data before writing code:

1. Find a product present at two versions in the corpus (two fingerprints, same
   `format|uid`).
2. Pull both maps. Diff the control **surface** (name, index, kind, range, unit
   per control) — expect it identical or a pure superset.
3. Diff the **anchors** for the shared controls — expect identical.

If the surface matches and the anchors match, the assumption holds — build it. If
anchors drift on a version-only change with an unchanged surface, stop and bring
that case here; the range/unit gate below won't catch a silent behavioural shift.
This is data already on disk, no model spend.

## The rule

A map is a control **surface** (name, index, kind, range, unit per control) plus
**anchors** (the measured display values). The surface is cheap to read — one
instantiation, no set-then-read. The anchors are the expensive part (set-then-read
per control) and the only thing the AI proposals do **not** depend on. Therefore:

> When a new fingerprint's product already has a mapped fingerprint whose control
> surface matches, copy that map's anchors and that fingerprint's proposals onto
> the new fingerprint. Skip the anchor sweep; skip the model call.

### Sweep side (ejmap tool)

Per worklist row, before the set-then-read pass:

1. Load the plugin (needed regardless — the fingerprint and the surface both need
   a live instance).
2. Read the surface: name/index/kind/range/unit for each control.
3. Look up prior mapped fingerprints of the same product (`format|uid`).
4. Decide:
   - **Full match** — same controls, same range + unit on each → build the new
     map by copying anchors control-by-control from the prior map. Write it for
     the new fingerprint with `provenance.method = "carried_forward"` and
     `carried_from = <prior fp>`. No set-then-read. Outcome `mapped`.
   - **Superset** — existing controls match, the new version appended some →
     carry the matching controls' anchors, set-then-read **only** the new ones.
   - **Range/unit changed on a control, or no prior map** → sweep that control (or
     the whole plugin) as today. A changed range means the old anchors would be
     wrong, so those are never carried.

### Server / propose side

When a map lands (or when propose runs) for a new fingerprint:

- If a prior fingerprint of the same product has proposals **and** the surface
  matches → copy those proposals to the new fingerprint. No two-arm call, no
  charge.
- Spend only on controls that are genuinely new or whose surface changed.

## Read path — keep updated plugins dialable (BUILD THIS FIRST)

The write side above stops the corpus redoing work. But there is a sharper,
user-facing version of the same problem. The dial path fetches maps by
**identity** (`GET /api/params/maps?identities=…`, version-specific — server
contract §2). So the moment a user updates Waves / UAD, every updated plugin has a
new identity the server has no map for, and it goes **undialable** until someone
re-sweeps that version. That is a live regression triggered by a routine update,
and it hits before any carry-forward sweep runs.

Fix — the same insight on the read side: **when the maps fetch has no entry for an
identity, fall back to the most recent mapped identity of the same product
(`format|uid`).** Dialing resolves controls by **name**, and names are stable
across versions, so the prior version's map dials the new version correctly. An
updated plugin stays dialable instantly — no re-sweep, no gap. Tag the served map
`served_from = <prior identity>` so the fallback is visible.

This is cheaper and more urgent than the write-side optimisation — it protects
every user the instant they update. Build it first.

## Plugin robustness — unresolvable controls

Even with the fallback, a control the model names may not exist on the actual
plugin (a new version renamed or dropped one, or there is genuinely no map). The
dial plugin should handle that at build time rather than silently half-dialing —
an extension of the honesty floor (a wrong or partial write must say so, §5):

- **Detect** at dial-build: a named control that does not resolve on the live
  plugin.
- **Mappable-only ON** → treat it as unmappable for that request, report it, and
  suggest the nearest control by fuzzy name match rather than failing blank.
- **Mappable-only OFF** → surface "map this by hand" for that control, so the
  operator resolves it directly instead of the request quietly dropping it.

Never claim the dial happened. The floor already guarantees silence = success;
this makes the failure path actionable instead of merely honest.

## Provenance and trust

Carried maps and proposals carry `method = "carried_forward"` plus the source
fingerprint, so they're auditable and can be force-re-swept if ever doubted. Trust
is inherited unchanged from the source — a carried human-verified control stays
human-verified; a carried model proposal stays a proposal, not an upgrade.

## What it saves

Per updated giant library the recurring cost drops from "re-sweep + re-propose
every version" to "one load + a surface compare" — no set-then-read, no model
spend — on the majority of version bumps that don't touch parameters. The
append-only case pays for the new controls alone.

## Acceptance criteria

1. A new fingerprint whose surface matches a prior mapped version writes its map
   with **zero** set-then-read calls and copies the prior proposals with **zero**
   model calls.
2. A version that changed a control's range or unit re-sweeps that control and
   does **not** carry its anchors.
3. A version that appended controls carries the old ones and sweeps only the new.
4. Carried maps/proposals are tagged `carried_forward` with the source fp, and are
   anchor-identical to the source for the shared controls.
5. Existing `--selftest` / roundtrip suites stay green; no existing map changes
   meaning.

## Rollout

Gate behind a flag (e.g. `EJMAP_CARRY_FORWARD=1`) for the first campaign. On a
sample, run a carried-forward map **and** a full re-sweep of the same plugin and
confirm they're anchor-identical before trusting it wholesale. Then default on.

## Where it lives

- **server** (`echojay-saas`): the read-path product fallback in the maps fetch
  (serve the newest product map when an identity is missing), plus the propose
  carry (serve a prior fingerprint's proposals for a matching new fingerprint).
- **ejmap tool** (`tools/ejmap`, C++): the sweep-side carry — load → surface-read
  → match → copy-or-sweep.
- **EchoJay plugin** (`echojay-vst-v200`): unresolvable-control handling — detect,
  suggest alternative / hand-map, gated by the mappable-only toggle.

Build order, most protective first — each independently shippable:
1. **§0** — confirm anchors hold across a version-only change ($0, data on disk).
2. **Read-path product fallback** (server) — stops updated plugins going dark the
   moment a user updates. Ship first; it's the user-facing regression.
3. **Sweep-side carry** (ejmap) — the big time saver on re-sweeps.
4. **Proposal carry** (server) — the API-cost saver.
5. **Plugin robustness** (EchoJay) — makes the failure path actionable.
