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
