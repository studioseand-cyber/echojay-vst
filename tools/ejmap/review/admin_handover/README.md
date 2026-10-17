# Plugin role review - handover for Sean's admin session (Kathy, 16 Oct 2026)

**What it is:** the human check for plugin controls EJ Map could not identify by name and unit. One row per control: plugin, control, the unit its display shows, a role dropdown (section + role, e.g. "Gate → Threshold", pre-set to the suggestion, plus "Other / not a main control"), why it was not identified, and Yes / No / Unsure.
- **Yes:** the suggestion is right. **Picking a role:** a human check of that role, in that section. **No:** not the suggestion, no role given. **Unsure:** counts neither way; the row moves to a "Needs measuring / ask Sean" list.
- **Keyboard on a row:** y / n / u answer and move on; r opens the dropdown; Backspace clears.
- An answer saves at once and counts straight away.
- Rows marked "counts now" are the ones where a single yes makes that plugin's map dialable ("mapped"). They come first.
- **17 Oct:** a row may instead carry `counts_with` (its partner's id): Yes on BOTH moves the map to "mapped" (an EQ band's frequency and gain). Pairs show "counts with <control>" and sort with the counts-now rows. The list may name a `first_category` (EJ Map's `--review-first`, e.g. `eq`); that category's counts-now cards come first. Both fields are optional; FORMAT.md has them.

**Where it runs now:** today Kathy uses it locally (`tools/ejmap/review/review_server.py`; answers go to a JSON file). This folder is everything needed to move the same page into the operator console, untouched in layout and data format.

Nothing here was put in the echojay-saas repo, and no route was deployed. This is for Sean's session to review and land.

## What goes where (echojay-saas)

| file here | goes to | notes |
|---|---|---|
| `public/js/admin-plugin-review.js` | `public/js/admin-plugin-review.js` | The tab module. Same UMD shape as `admin-community.js`: it registers with `EchoJayAdmin` and builds every node with `ctx.el` (textContent only, no innerHTML). |
| `public/css/admin-plugin-review.css` | append to `public/css/admin.css` (or link it) | The `rv-` classes only; everything else is the console's existing `adm-` classes. |
| `api/v2/admin/plugin-review.js` | `api/v2/admin/plugin-review.js` | GET list + answers, GET `?export=1`, POST an answer. The gate and the refusal order follow `misdials.js`; it logs `[admin-action]` before every write. |
| `scripts/import-plugin-review.mjs` | `scripts/import-plugin-review.mjs` | Loads EJ Map's list and any answers already given. Dry run by default; `--apply` writes. |
| `test/test_review.cjs` | the suite, as you see fit | The module's pure parts, plus the route against a fake store (401 / 403 / 400 / 404, save, clear, export). |

Two lines in the console itself:
1. `public/js/admin-shell.js` TABS: add `{ key: 'plugin-review', label: 'Plugin review' }`.
2. `public/admin.html`: add `<script src="/js/admin-plugin-review.js?v=1"></script>` after the other tab scripts.

The page is then `/admin?section=plugin-review` (the console's own routing). If you want the path Kathy asked for, `/admin/plugins/review`, a rewrite to that is your call.

## The API and the store
- `GET /api/v2/admin/plugin-review` returns `{ schema: "ejmap_role_review/1", generated_at, items: [...], answers: { <id>: answer } }`.
- `GET /api/v2/admin/plugin-review?export=1` returns `{ schema: "ejmap_human_checks/1", items_generated_at, answers: [...] }`. This is exactly what EJ Map reads with `--trust-levels --human <file>`.
- `POST /api/v2/admin/plugin-review` with `{ id, verdict: "yes" | "no" | "clear" }` returns `{ ok, answer | null, counts: { items, answered, countsNowMaps, mapsMappedByAnswers } }`.
- **Store (Upstash, env-scoped with `scopedKey`):**
  - `ejmap:review:items`: a string holding the whole review document;
  - `ejmap:review:answers`: a hash, id → answer JSON.
- Nothing else reads these keys yet. EchoJay's trust levels (docs/TRUST_LEVELS_v0.md §2, §6.5) are a separate server item.

## The JSON formats
See `FORMAT.md`. The ids are `<map fp>|<control name>`, the same on the local page, the route and EJ Map.

## Importing the answers Kathy already gave
On the console's environment:
```
node scripts/import-plugin-review.mjs --items review_items.json --answers human_checks.json          # dry run: says what it would write
node scripts/import-plugin-review.mjs --items review_items.json --answers human_checks.json --apply
```
- `review_items.json` comes from EJ Map's `--trust-levels --out <dir>`.
- `human_checks.json` is the file Kathy's local page writes (`~/Desktop/EJMAP_REVIEW/human_checks.json`).
- An answer for an id that is no longer in the list is reported and skipped.
- Where an answer is already in the store with a later time, the newer one is kept.

## Round trip
1. EJ Map writes `review_items.json`.
2. Run the import.
3. Kathy (or any admin) answers on the console.
4. `GET ?export=1` gives `human_checks.json`.
5. `ejmap --trust-levels ... --human human_checks.json` recounts. A yes identifies the control for that role on that map; a no refutes it. "Mapped" still needs the control's read-back.
