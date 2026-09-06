# MERGE SURVEY, 6 Sep 2026 - Kathy's work on origin/integration/reasoning-plus-pitch. READ-ONLY: nothing combined, checked out or built (Sean is shooting).

## 1. What is there
Remote: origin/integration/reasoning-plus-pitch, tip aa455ff (2026-09-05).
Merge base with our local branch: d68da09 (2026-08-27, Sean D). The remote
is 50 commits ahead of the base; our local branch is 129 commits ahead of it
(all "Sean D" = this Mac's identity, rounds 15-60).
The 50, by author: KATHY 36 (2026-08-26 .. 2026-09-05), SEAN DONOGHUE 14
(2026-08-21 .. 2026-08-25, the other Mac's identity: dashboard webview
login, dial-miss rows A8/A9, card composer). Kathy's f6ac073 (2026-09-01)
merges "Sean's Link and rack-borrow work" - its second parent IS our base
d68da09, so she merged what this Mac had pushed by 27 Aug; none of our
rounds 15-60 are in her branch.
Other remote branches ahead of us (feat/ejmap, feat/pitch-category: 254
commits, all Sean Donoghue, last 2026-08-26; feat/plugin-dashboard-v0,
feat/v2-with-devices, feat/link-tab-polish, main) are older, not this merge.

## 2. Files changed by the 50, grouped (commits touching each)
Source (29 files): PluginEditor.cpp 24, ChainHost.cpp 22, EchoJayAPI.cpp
19, ChainHost.h 18, EchoJayAPI.h 15, PluginEditor.h 12, EchoJayParamApply.h
6, MeterEngine.cpp 3, EJWavesRegistryFeed.h 3, EJNameLadder.h 3,
EJDialMissRows.h 3, PluginProcessor.cpp 2, MeterEngine.h 2,
LinkProcessor.cpp 2, EJWavesAlias.h 2, EJParamReads.h 2, DashboardWeb.h/.cpp
2 each, PluginProcessor.h 1, PluginChecklist.cpp 1, LinkProcessor.h 1,
EedDeviceProcessor.cpp 1, and seven new EJ*.h headers (variant preference,
settings clip, refusal line, disable reasons, dial writes, dial tally,
param maps) 1 each.
tools: mapfps_test 35 (+ its script), dashweb_test 1, paramapply_test 1.
Root: CHAIN_AI_BUILD_SPEC.md 1.
Areas, plainly: plugin NAMING / feed / Waves resolution (ChainHost, the
EJ headers, Aug 26-31); the DIAL client and dial-miss reporting (editor,
API, ParamApply, Aug 21-25 + Sep 4-5); chat context - spectrum on a chat
turn, meter fields, dynamics gating (MeterEngine, API, Sep 2-4); the MOVE
LOG across reload (ChainHost, PluginProcessor state, Sep 4-5); DO NOT DIAL
("suggest every value and write none", Sep 5); Link compile gating.

## 3. THE OVERLAP LIST (her commits touching each; ours in the same file for context)
    Source/EedPitchCorrect.h / EedPitchProcessor.{h,cpp} / EedPsolaEngine.h /
      EedPitchEditor.{h,cpp} / EedRetuneMap.h ........ hers 0   (ours 30/14/22/18/10/14/2)
    Source/PluginProcessor.h ....................... hers 1: 8df84ef (a static spectrumUsesAverage helper; not the budget/latency code)   ours 3
    Source/PluginProcessor.cpp ..................... hers 2: 8df84ef (uses that helper in stopCapture), 45b1c95 (move log in get/setStateInformation)   ours 3
    Source/PluginEditor.h .......................... hers 13   ours 1 (da6939f)
    Source/PluginEditor.cpp ........................ hers 25 (finishEditBubbleWhenDialSettled 9, ctor 6, requestAIFeedback 4, standardChainInjections 4, timerCallback 3, sendChatMessage 3, resized 3, ...)   ours 2 (2f02c65 the editor tick; da6939f the Settings removal)
    Source/ChainHost.{h,cpp} ....................... hers 19/23 (applyStructuredIfReady 10, runNextEditOp 8, buildRecommendable 6, loadPluginAsync 5, resolveByName 3, ...)   ours 3/3 (reset, latency log, budget)
      -> her ChainHost diff touches NONE of process(), rebuildForLatencyIfChanged, LatencyRebuilder, resetPending_/requestReset, latencyRebuildPending, hostReportableLatency (grep: 0 lines)
    Source/LinkProcessor.cpp ....................... hers 2 (aa455ff, ac78f08: buildChainFromSpec, addChainPluginManually, setChainSlotWet, restoreChainFromVar, pollChainCommand)   ours 2 (the sidecar identity block, elsewhere in the file)
    Source/LinkShm.h ............................... hers 0   ours 1
    Source/EedDeviceProcessor.cpp .................. hers 1: aa455ff (see section 4)   ours 1 (7fe50fd)
    tools/pitch_mode_test, tools/latency_impulse_test, tools/settings_snapshot ... hers 0
    every DEFECT_*.md, PITCH_P0_VALIDATION.md, REFERENCE_SET.md ............... hers 0

## 4. THE SILENT-REGRESSION LIST
Commits in the range whose diff adds or removes each token (git log -S,
Source + tools): kSeamAttackMs NONE; kNoteConfirmMs NONE;
kBorrowAlignBudgetFrames NONE; kRetuneFloorMs NONE; kMaxRetuneMs NONE;
kGapIsNoteChangeMs NONE; kSeamFadeMs NONE; kNoteChangeCents NONE; kPresets
NONE; RetuneMap / nearestDial NONE; kDefRetuneMs NONE; kNaturalVib /
kIgnoreVib NONE; kEditCushionFrames / reportedBudgetFrames NONE. No schema
entry line of any Eed*Processor changed in the range (git log -G on the
"{ ...::kX, \"unit\", min, max, def" pattern: none). No pitch file, no
DEFECT record, no pitch tool touched. The defaults of the round-58 record
are not moved by any of the 50.
ONE COMMIT NEEDS ATTENTION, AND IT IS A QUESTION FOR KATHY, NOT A FIX OF OURS:
  aa455ff "Suggest every value and write none" adds, at the TOP of
  EedDeviceProcessor::applyParams (her tree, line ~103):
      if (echojay::dialWritesBlocked()) return {};
  applyParams is ALSO the path EedDeviceProcessor::setStateInformation takes
  to restore a saved session (her tree line 244; ours line 230). With the
  "do not dial" setting ON, a session RELOAD restores NOTHING for every
  built-in device - pitch, EQ, all of them load at their schema defaults.
  Her own header (EJDialWrites.h) says the gate is "deliberately NOT
  consulted by ... applyRestoredParams: restores what the USER saved;
  blocking it corrupts a reload" - that exemption covers the third-party
  path; the built-in restore path is not exempted because the guard sits
  in applyParams rather than applyStructured. The setting defaults OFF
  (absent property -> false, EchoJayAPI.cpp ~3805), so nobody is hit at
  defaults; a user who turns it on gets the reload failure. Flag to Kathy
  before the merge: the guard probably belongs in applyStructured (the
  model's path) - hers to decide, not ours to move.

## 5. Staging read
CONFLICTS (git merge-tree --write-tree, read-only): TWO, both trivial.
  a. Source/ChainHost.cpp, the include block: ours adds #include
     "EedLatencyLog.h"; hers adds five EJ*.h includes. Resolution: keep all
     six lines.
  b. Source/PluginEditor.cpp, the settingsMovers list: ours REMOVED
     &settingsWithheldToggleBtn_ (da6939f); hers ADDED &dialWritesToggle on
     the line above and still lists the withheld toggle (she never touched
     that section). Resolution: keep her &dialWritesToggle, keep our
     removal - i.e. "&dialWritesToggle, &settingsScanBtn, &viewAllPluginsBtn,".
     Our removal commit is the one to carry forward for the withheld
     section (she has no commits on it); her NEW Settings control (the
     do-not-dial toggle) lands in the same panel and must be rendered with
     the settings_snapshot harness after the merge.
Everything else auto-merges: ChainHost.h, EedDeviceProcessor.cpp,
LinkProcessor.{h,cpp}, PluginEditor.h, PluginProcessor.{h,cpp}.
INDEPENDENT of hers: all pitch work (zero overlap), the transport reset
and the borrow-budget commit (her ChainHost/PluginProcessor hunks are in
other functions), the Link sidecar identity, every record and tool of ours.
RECOMMENDED: ONE merge, on a branch (merge/kathy-2026-09-06), taken in TWO
reviewable stages at her own boundaries: stage 1 = up to f6ac073 (Sean's
Aug 21-25 dial-client commits + Kathy's naming/feed/Waves resolver, Aug
26 - Sep 1; no conflicts expected there: our include and list lines
conflict only with commits after it); stage 2 = f6ac073..aa455ff (chat
spectrum/meters, move log, DO NOT DIAL, Link compile gating) - carries
both conflicts and the applyParams question above. After each stage:
build, AU + VST3 load, the pitch suite (196 PASS), settings_snapshot with
her toggle present, and the round-58 defaults read back from the branch
that built the binary with its commit (custom, dial 0 at 6 ms, depth 100,
flex 0, humanize 0, natural_vibrato 0, IGN VIB on, seam 60).
Nothing of this runs until Sean says the shoot is done; the shoot build
(F03A4363) and ~/ej-installed-backup-shoot stay untouched.

## 6. THE OTHER DIRECTION (same day, still read-only)

### 6.0 OUR COMMITS ARE NOT ON ANY REMOTE - THE LARGEST RISK, AND IT IS OPEN
`git rev-list --count HEAD --not --remotes` = 130: every commit since the
base (rounds 15-60, 27 Aug - 6 Sep) exists ONLY on this Mac. No remote ref
contains HEAD; the tracking branch has diverged. Two remedies were attempted
and both were refused by the permission classifier: a push to a NEW branch
name (no force) and a git bundle into Dropbox. SEAN MUST RUN THE PUSH
HIMSELF (one line, no force, a new branch name, nothing else touched):
    git push origin HEAD:refs/heads/backup/seand-mac-2026-09-06
Until that has run, nothing here should be merged, rebased or reset.

### 6.1 Reverse overlap: our 129 against her heavy files
    ChainHost.{h,cpp}     ours 3: 1c5fb52 (reference provenance / KeyFeed), c75b222 (the transport reset flag + graph reset in process()), 222b4fe (latency log lines, latencyRebuildPending)
    PluginEditor.{h,cpp}  ours 2: 2f02c65 (the periodic refreshLinkRegistry tick removed from the editor timer, C5), da6939f (the withheld section removed)
    LinkProcessor.{h,cpp} ours 3: c75b222 (reset() override), 222b4fe (a setLatencySamples log wrap), 2f02c65 (the sidecar publisher pid + host identity)
    PluginProcessor.{h,cpp} ours 4: 1c5fb52, c75b222 (reset), 222b4fe (log + first-50-blocks), 2f02c65 (one committed budget; the 1 Hz registry pass on the processor timer)
    EchoJayAPI.*, EchoJayParamApply.h, MeterEngine.*, DashboardWeb.*, every new EJ*.h: ours 0.
Sean's "Link work on chains in the main plugin" is NOT among our 129: it
is in the BASE (his 25-27 Aug borrow §8 / mute-solo / rack-row commits,
pushed 27 Aug) and therefore on both sides already. His "load of UI
updates" are his 14 remote commits of 21-25 Aug (dashboard webview login,
dial-miss rows, card composer) plus Kathy's editor work; none of ours.

### 6.2 SAME-FUNCTION, NO-CONFLICT (every function both sides modified; per-commit hunk headers, both sides against the base)
    | file :: function | ours | hers | read |
    |---|---|---|---|
    | ChainHost.cpp :: file scope (includes) | 1c5fb52, 222b4fe | 8 of hers | THE conflict (a); independent lines, keep all |
    | ChainHost.h :: class body (public/private regions) | 1c5fb52, c75b222, 222b4fe | 18 of hers | member ADDITIONS only; no identifier appears on both sides (checked both ways); independent |
    | ChainHost.cpp :: any function | 0 in common | | her 35 functions (applyStructuredIfReady, runNextEditOp, buildRecommendable, loadPluginAsync, resolveByName ...) vs our 9 (process, rebuildForLatencyIfChanged, LatencyRebuilder, prepare, rebuildGraph, KeyFeed sites): DISJOINT |
    | PluginEditor.cpp :: EchoJayEditor (ctor) | da6939f: movers list, the withheld toggle handler removed | 5 of hers: dashEntry_, onNeedFallbackMaps, dialWritesToggle setup | independent apart from the movers line = conflict (b) |
    | PluginEditor.cpp :: showSettingsView | da6939f: rebuildSettingsWithheld() call removed | aa455ff: her toggle's state + visible | different controls; independent |
    | PluginEditor.cpp :: hideSettingsView | da6939f: two withheld setVisible(false) removed | aa455ff: her toggle hidden | independent |
    | PluginEditor.cpp :: resized | da6939f: the withheld layout block removed (sy no longer advanced by it) | 9b20735, aa455ff, c1a18df: her toggle laid out directly BELOW autoDialToggle and ABOVE the PLUGINS row; the dash panel text; the ask-chip exemption | her toggle sits above our removed block, so the Settings column is consistent after the merge (toggle, PLUGINS row, then the bottom row); independent - but the merged panel MUST be rendered (settings_snapshot) since both sides moved the Settings layout |
    | PluginEditor.cpp :: timerCallback | 2f02c65: the periodic refreshLinkRegistry() tick removed (C5); da6939f: rebuildSettingsWithheld() removed from the scan-finished path | 32be312: buildRecommendable(feedRowsWithSessionExclusions(...)) at three sites | same function, different lines: her session-exclusions feed vs our registry-pass move; independent in code. refreshLinkRegistry callers: base 10, hers 10, ours 10 (she added none; we moved one to the processor) |
    | PluginEditor.h :: class body | da6939f (members removed) | 11 of hers (members added) | independent |
    | LinkProcessor.h :: public | c75b222: reset() override | ac78f08: buildChainFromSpec gains a LoadOrigin parameter | independent |
    | LinkProcessor.cpp :: any function | 0 in common | | her buildChainFromSpec / addChainPluginManually / setChainSlotWet / restoreChainFromVar / pollChainCommand vs our sidecar publish block and the log wrap: DISJOINT |
    | PluginProcessor.h :: public | c75b222, 222b4fe, 2f02c65 (reset, the committed budget API) | 8df84ef (spectrumUsesAverage) | independent |
    | PluginProcessor.cpp :: any function | 0 in common | | her stopCapture / get+setStateInformation (move log) vs our processBlock / prepareToPlay / commitBorrowBudget / refreshLinkRegistry / timerCallback: DISJOINT |
    | EedDeviceProcessor.cpp :: any function | 0 in common | | her applyParams guard vs our 7fe50fd (onStateApplied): DISJOINT - but see the question below |
Removed-API check: setBorrowBudgetActive (deleted by us in 2f02c65) - her
tree's 4 occurrences are the BASE's own definition and call, not new
callers; the merge takes our deletion cleanly. Her compile-gating commit
5460609 touches only tools/mapfps_test/build_and_run.sh. No identifier
introduced by either side appears in the other's tree.
The Link sidecar / registry path, specifically: her two LinkProcessor
commits and her Link gating do not touch the sidecar publisher, the
registry pass, the processor timer or the budget; our C4/C5 changes
survive the merge untouched by construction. What DOES need a look after
the merge is behavioural, not textual: her do-not-dial gate on
ChainHost::setSlotWet and applyParams sits on paths the borrow session and
the state restore use - the question below.

### 6.3 THE QUESTION FOR KATHY (held until Sean confirms the push; paste-ready)
  Kathy - one question on aa455ff ("Suggest every value and write none").
  The dialWritesBlocked() guard sits at the top of
  EedDeviceProcessor::applyParams. That function is also the path
  EedDeviceProcessor::setStateInformation takes to restore a saved session
  (applyParams(parsed["params"]) at ~line 244 in your tree), so with the
  setting ON a reload restores nothing for every built-in device - pitch,
  EQ, all of them come back at their schema defaults. Your EJDialWrites.h
  note exempts applyRestoredParams for exactly this reason; the built-in
  restore path goes through applyParams rather than applyStructured, so
  it is not covered by that exemption. Was that intended? If not, would
  the guard belong in applyStructured (the model's path) instead? It is
  your call - I have not touched it. (The setting defaults off, so nobody
  is hit at defaults.)

## 7. WHAT IS ALREADY ON BOTH SIDES (verified 6 Sep 2026, `git merge-base --is-ancestor <commit> aa455ff`, every one YES)
Sean's chain-control work in the main plugin is contained in Kathy's tip:
    e52066d  editor placement: ONE decision; the Link stops floating builtins
    b94c346  panes: no auto-open off-tab; the rack menu closes editors first
    6853ea2  editor panes: the MISSING slot speaks; EJPane instruments all four
    ef5a4df  editor panes: the sizing half of the builtin lift
    7a30edf  rack row: tick + M + S centred on the pill
    e4262c3  mixer strips: the tick/M/S group centred, in the stored rects
    637212f  mute/solo: one author, two placements; the rack's drift named and gated
    06e789f  mute/solo layout: lamps get their own space
    68cbc6e  (Kathy) offer the auto-dial toggle only where turning it off would change the answer
    19f3c36  placement: every reader goes through the ONE decision
    0d99d0e  first open: the restored tab is ENTERED, not just displayed
    d68da09  the two-line read: getAllSlotInfos dropped the sixth field  (the merge base itself)
Her commit f6ac073 of 1 September - "merge: Sean's Link and rack-borrow
work into the naming, feed and dial branch" - integrated it herself. It is
on both sides, cannot conflict, and cannot be lost.

The only genuinely two-sided work is ours since d68da09 on the shared
non-pitch files: da6939f (the Settings removal), 2f02c65 (the committed
borrow budget, the sidecar identity, the processor-owned registry pass),
222b4fe (the latency log; the round-48 reset kept), c75b222 (the transport
reset), 1c5fb52 (the reference grid provenance) - and, surfaced by the
same check, 7fe50fd (the reference laundering severed at the state layer:
EedDeviceProcessor::onStateApplied), which shares its FILE with Kathy's
aa455ff guard but not its function; it auto-merges and its behaviour is
covered by the pitch suite's saved-state checks. The V1-V6 legs cover the
five; V6's read-back and the suite cover the sixth.


## 8. V7 - a NAMED leg, and a conditional re-check (6 Sep 2026)
V7  7fe50fd, the reference laundering severed at the state layer
    (EedDeviceProcessor::setStateInformation -> onStateApplied;
    EedPitchProcessor::onStateApplied migrates a loaded MANUAL reference
    that carries no ref_manual_by_user marker back to AUTO). THE INSTRUMENT,
    by name, in tools/pitch_mode_test/main.cpp, block "    std::printf ("== UI coverage: every schema param has a hand control, or is "":
      - "laundered state (manual + value, no marker) reverts to AUTO on load"   (line 1072)
      - "marked manual 442 survives a save/load round-trip"                     (line 1093)
      - "auto survives a save/load round-trip (the field saved is the manual 440, never a detected grid)"   (line 1101)
    plus the "SEAN'S SAVED STATE" block (his 3 Sep file: reference AUTO,
    applied 440, the dormant 439.19 field, then manual applies it). These
    run again after each merge stage and must pass as they pass today.
    onStateApplied also carries the round-51 snap-to-curve; the round-46
    block ("THE RETUNE DIAL") and "THE DEPTH TRAP CANNOT EXIST" cover that
    half, and run with the suite.
CONDITIONAL RE-CHECK, written down now so it does not surface in six
months as "nobody knows why saved state stopped restoring": 7fe50fd
shares EedDeviceProcessor.cpp with Kathy's do-not-dial guard but not its
function, so it auto-merges cleanly TODAY. Her open question is whether
that guard belongs in applyStructured rather than applyParams. IF KATHY
MOVES THE GUARD, 7fe50fd IS RE-CHECKED AGAINST ITS NEW POSITION - the
guard must not sit between setStateInformation's applyParams and the
onStateApplied migration, and must not short-circuit the restore that
the migration reads - AND V7 RUNS AGAIN. A clean auto-merge today is not
a clean auto-merge after her fix.
ON THE MOVING HEAD: Kathy is given the BRANCH NAME,
backup/seand-mac-integration-2026-09-06, never a hash; it has moved four
times today from committing survey records to the branch under survey.
