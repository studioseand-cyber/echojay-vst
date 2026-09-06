# Brief: the full merge, then a verified install

**For a Claude Code session in `~/echojay-vst` on the second Mac
(`macbookpro-lan`).** Read `LINK_CHAIN_EDITING_HANDOVER.md` (Sean has it),
`MERGE_NOTES.md`, and `HANDOVER_DASHBOARD.md` before starting.

Run the branch guard first; report path, branch, tip, cleanliness.

---

## The state of play, verified

Three places hold work that must come together:

| Branch | Holds | Base / gap |
|---|---|---|
| `origin/integration/reasoning-plus-pitch` | the trunk: audio + reasoning + most of Link Stage 1 (codec fix `4171741`, status-line fix `8cea0d1`, the whole solo-edit protocol) | — |
| `origin/feat/v2-with-devices` | **2 stranded commits**: `82f3621` built-in devices editable through the remote path, `a5ba249` the 21-device detached-editor probe | 126 commits BEHIND integration |
| `origin/feat/dashboard-tab` | the webview Dashboard, bridge, ChainHost state-matching, four findings docs | merge-base `7cff677` |

**The stranded pair matters.** The handover records fix #2 — "internal EchoJay
devices could not be edited at all", the *No compatible plug-in format exists*
error on the 4-Band Compressor — as fixed in `82f3621`. **It is not in the
trunk.** Verified by content, not just by commit reachability:
`editProceedWithState` on `integration` does not fork on `isBuiltinDescription`
and does not reach `BuiltinDeviceRegistry::findForDescription`. Build from
integration today and that failure returns exactly as first reported.

## Order of work

### Part 1 — bank the stranded pair first

Small, isolated, and easy to lose again. Cherry-pick `a5ba249` then `82f3621`
onto `integration/reasoning-plus-pitch` (in that order — the probe precedes the
fix it proved). They were authored 126 commits back, so expect conflicts around
the moved code; resolve toward the handover's stated intent: widen
`EditSession`'s held pointer from `AudioPluginInstance` to `AudioProcessor`,
fork in `editProceedWithState` mirroring `loadPluginAsync`, resolve builtins via
`findForDescription`, and keep **one** seed-and-begin lambda shared by both
routes so they cannot drift.

Verify by content afterwards: `editProceedWithState` reaches the builtin path,
and the 21-device probe runs and passes. Commit, push.

### Part 2 — the dashboard merge

`git merge origin/feat/dashboard-tab` into
`integration/reasoning-plus-pitch`. Direction is deliberate: the audio branch
keeps its history.

**The hazard the handover names, and it raises no conflict.** Dashboard's stream
extraction moved the chat-reply pipeline into `handleChatReply`; the reasoning
branch has stream-transport work in the same path. A clean auto-merge can place
both sides' logic adjacent and run the pipeline twice.

Do this, exactly:
1. Before merging, count the relevant markers on **each parent** separately and
   write the numbers down.
2. Produce the merged tree without committing —
   `git merge-tree --write-tree A B` — and re-count off that tree.
3. A **doubled** count is the bug. Same or expected count is the pass.
4. Only then commit the merge.

**Also mandatory, conflict or not — `MERGE_NOTES.md` §1:** this branch's
`openSavedChain` takes `(id, name, onFetchError, onSlotsParsed)`. The dashboard
branch added callers using the two-argument form: the confirm re-entry inside
`openSavedChain` itself, `bridgeOpenChainById`, and the recall path. Wire the
hooks through **every** one or recall silently loses its error reporting after a
confirm. Then check §5 (chat-sidebar scroll) against this branch's sidebar work.

Reconcile `CMakeLists.txt`: keep `JUCE_WEB_BROWSER=1`, `DashboardWeb`, and the
three test tools.

### Part 3 — build and prove, before any install

Build AU + VST3. Run **every** suite: `state_match_test`, `dashweb_test`,
`dashboard_test`, the builtin registry suite including the new device probe, the
codec round-trip test from `4171741`, and this branch's own gate. All green or
the merge does not get pushed.

### Part 4 — install, with the transport question answered honestly

**The handover's blocker #1 needs updating, not obeying literally.** It says to
clear `ECHOJAY_DEV_TRANSPORT`. That was right when DEV was accidental. It is
now **deliberate**: the Dashboard tab's preview host, its `ej_v2_preview` gate
and Sean's login all depend on a DEV build reading `~/.echojay/dev.json`. An
`OFF` build points at production and the dashboard work becomes untestable.

So the rule becomes: **know which you have and why, and say so.** Before and
after installing, run `tools/which_build_is_installed.sh` (confirm it exists on
this Mac; if not, say so rather than skipping the check), record the transport,
and record what `dev.json`'s `baseUrl` currently is. Install the DEV build for
day-to-day use. Never assume from a version number — the handover says that
mistake has cost most of a day twice.

Back up the current install to `~/ej-installed-backup-merge` and verify by UUID
before overwriting anything.

**Identity by content, not version.** Assert three things on the installed
binary: the codec round-trip test present and passing, the dashboard work's
marker (`EJWebSpike` absent but `DashboardWeb`/`loadChain` present), and the
transport reading what you expect.

## Report back

Conflicts hit and how resolved; the marker counts before and after (Part 2);
which `MERGE_NOTES` items were hand-wired; suite results; the installed
binary's UUID and transport; and the state of the working tree.

## Do not

Delete or prune worktrees. Touch `~/.echojay/dev.json`. Force push. Improvise
past a `MERGE_NOTES` item — if one is ambiguous, stop and ask.
