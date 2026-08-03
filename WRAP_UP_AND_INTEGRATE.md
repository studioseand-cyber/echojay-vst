# Wrap-up & integrate — the reusable prompt

Paste this into any parallel session when it has done substantial work and you want it
landed safely. It encodes what this project's merges actually cost us.

Integration branches:
- plugin repo `echojay-vst` → **`feat/v2-with-devices`**
- backend/web repo `echojay-saas` → **`feat/preview-combined`**

---

```
WRAP UP AND INTEGRATE. Land your work safely — follow these steps in order.

STEP 1 — COMMIT AND PUSH YOUR OWN WORK FIRST, before merging anything.
Uncommitted work in a worktree is the only thing here that can actually be lost; everything
else is recoverable from git. Commit in focused commits and push your branch. In the commit
subject or body, NAME any shared/high-traffic file you touched (api/chat.js, PluginEditor.cpp,
ChainHost.cpp, CMakeLists.txt, the registry test) — that is what makes the next merge cheap.
Report your branch tip.

STEP 2 — CHECK YOUR HEADROOM, honestly.
If you are near the end of your context, STOP after step 1 and say so. A hunk-by-hunk merge
started on an exhausted context is how a silent bug ships. Stopping here is the correct
outcome, not a failure — the merge gets a fresh session.

STEP 3 — MERGE INTO THE INTEGRATION BRANCH (only if you have real headroom).
  plugin repo  (echojay-vst):  feat/v2-with-devices
  backend/web  (echojay-saas): feat/preview-combined
    git fetch origin && git checkout <INTEGRATION BRANCH> && git pull && git merge <your branch>
Do NOT merge into anyone else's feature branch. Resolve HUNK BY HUNK, keeping BOTH sides'
functionality. Never pick a side wholesale, never bulk-resolve, never script the resolution.

STEP 4 — THE THREE HAZARDS THAT DO NOT APPEAR AS CONFLICTS.
Git reports a clean merge and the result is still broken. All three have bitten this repo:
  a) DUPLICATE MODULE-SCOPE DECLARATIONS. Two branches that both hoisted the same helper give
     two declarations git merges without conflicting. Run `node --check` (JS) or compile (C++)
     on every file you touched. NOTE node --check is not sufficient alone: it catches a
     duplicate `const`, but NOT a duplicate `function` declaration or a duplicate object-literal
     key, both of which are legal JS. Also grep for duplicate declarations and duplicate keys.
  b) DUPLICATE ADJACENT BLOCKS. Git can place both sides' versions of the same logic next to
     each other without conflicting, so both run — double-validating, double-splicing,
     double-applying. Read merged regions for logic that now appears twice.
  c) SILENT INTERACTIONS. A change on one side can invalidate an assumption on the other with
     no textual overlap at all (e.g. one branch scoping a key prefix while the other's test stub
     matches the unscoped literal — the test passes while asserting nothing). Ask what each
     side assumes about the other's data, keys, ordering and formats.

STEP 5 — VERIFY BY READING WHAT THE CONSUMER READS, not by proxy.
A grep count consistent with correct placement is not proof of placement. A green suite is not
proof the assertion is live. Use positive controls: prove the check FAILS when it should, then
passes when it should. Confirm the real consumer's own output shows the value arriving.

STEP 6 — TESTS.
Run every suite INDIVIDUALLY first (so nothing hides behind exit-on-first-failure), then via
the gate. All green before anything ships. If something fails, prove whether the merge caused
it (did the merge touch any of that suite's inputs?) rather than assuming either way.

STEP 7 — SHIPPING, per repo.
  PLUGIN: cluster/feature sessions do NOT install to the system AU/VST3 folders — every worktree
  builds the same identifier to the same path, so an install silently overwrites whatever a host
  loads. Build and self-test in your own worktree only. Only the integration worktree installs,
  via ./tools/install_local.sh, then confirms with ./tools/which_build_is_installed.sh. If a host
  ever "ignores your fix", run that FIRST before re-debugging the fix.
  BACKEND/WEB: deploy previews only from the integration branch. All three Vercel environments
  point at ONE Redis instance, so a preview that saves a workspace OVERWRITES the real one — do
  not run anything that writes workspace blobs. Event writes are fine.

STEP 8 — REPORT: your branch tip, every conflict and how you resolved it, anything from any side
you could NOT preserve, test output, and (if you deployed/installed) the URL or installed UUID.
```
