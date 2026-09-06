# Reply to Kathy's survey (6 Sep 2026) - and the merge verification plan

## Answers
(a) Both of this Mac's branches are pushed and hash-verified. Survey
against these, not against origin/feat/pitch (96d87d6, 15 Aug, stale):
    backup/seand-mac-integration-2026-09-06   <- SURVEY AGAINST THIS BRANCH, by NAME (its head moves
                                                 as survey records are committed to it; any hash is stale by the next commit)
    backup/seand-mac-feat-pitch-2026-09-06    (materials only: rulers, traces, probes)
EedRetuneMap.h, kSeamAttackMs, kRetuneFloorMs, tools/latency_impulse_test
and REFERENCE_SET.md are all on the integration backup, none on feat/pitch.

(b) loadPluginAsync and setSlotWet call sites in our tree (fea141a) - all
PRE-EXISTING; diffed d68da09..fea141a we added NONE, so your signature
change has no un-updated call sites coming from our side:
    loadPluginAsync: ChainHost.cpp 1924, 5985, 6003, 6196; LinkProcessor.cpp 2105, 2186; PluginEditor.cpp 27389, 27455
    setSlotWet:      ChainHost.cpp 1865, 1981, 6207; LinkProcessor.cpp 2129, 2263; PluginEditor.cpp 2095, 2097, 29928

(c) There is no echojay-saas repo on this machine. The repos here are
echojay-vst (and its worktrees), echojay-vst-pitch and JUCE; ~/echojay is
extractor scripts and logs. No unpushed server work exists here.

## A correction to your finding 1
kNoteConfirmMs 25.0f and kMaxRetuneMs 400.0f are the BASE's values, not
measured ones. Ours are kNoteConfirmMs 15 (b702412, re-derived with the
co-timed clock) and the 0-400 dial with a 6 ms floor and a 150 ms cap
(480b259, e7e2b63, dee5bcb). Your tree predates that work, so git takes
ours automatically and the outcome is correct - but the authority on
every one of these is DERIVED_VALUES_SINCE_BASE.md on the integration
backup, not either of our lists. After each merge stage every value in it
is read back and compared; any difference is a finding.

## Two questions - yours to decide, we have changed nothing
1. setSlotWet's WetSource defaults to Assistant, which do-not-dial blocks,
   so an un-updated call site is SILENTLY REFUSED rather than failing to
   compile. Would you rather remove the default so it fails loud, the way
   loadPluginAsync does?
2. The dialWritesBlocked() guard sits at the top of
   EedDeviceProcessor::applyParams, which is also the path
   setStateInformation takes to restore a saved session (~line 244 in your
   tree). With the setting ON, a reload restores nothing for every
   built-in device. Your EJDialWrites.h note exempts applyRestoredParams
   for exactly this reason; the built-in restore path is not covered. Was
   that intended, and would the guard belong in applyStructured instead?

## THE MERGE VERIFICATION PLAN - re-running the acceptance test that originally proved each of our five commits
Sean's worry is the UI, Link and chain work, not pitch. "It compiled" is
not the proof. After the merge every one of these runs again and must
produce the same result it produced when the commit landed:
  V1  c75b222, the transport reset. Its positive control: a locate WITHOUT
      clearing must still differ from a fresh instance by ~132.6% RMS in
      the first 150 ms, and the fix must still render BIT-IDENTICAL to a
      fresh instance (tools/pitch_mode_test "TRANSPORT RESET"). Kathy has
      40 ChainHost commits and our reset threads a flag through
      ChainHost::process - the leg most likely to break silently.
  V2  2f02c65, the Link borrow budget. The full five-state impulse table
      (tools/latency_impulse_test): pending 0/0 and 16384/16384, exactly
      one notification per commit, plus the quiet-session zero-notification
      check and the scoping decisions. Her LinkProcessor signature change
      sits beside our sidecar block.
  V3  da6939f, the Settings removal. tools/settings_snapshot at the full
      size range: her do-not-dial toggle present and positioned, our block
      gone, every control below it present, positioned and unclipped, the
      chain-list count unchanged (2328 feed / 2129 AU view / 539 excluded).
  V4  222b4fe + 1c5fb52, the UI changes. The pitch editor snapshots
      (EJ_EDITOR_SNAP) still match their filed renders: the numbers box, the
      status line's absence, the grid provenance line.
  V5  Every setSlotWet call site in the MERGED tree has an EXPLICIT
      WetSource. A default-argument call is the silent-refusal failure Kathy
      named. Grep and list them.
  V6  Kathy's named silent defect: EchoJayAPI keeps maxHistoryMessages = 12
      and maxHistoryBytes = 24000 as TWO INDEPENDENT limits (today:
      EchoJayAPI.cpp:1068 and :1082, two constants, both passed at :1115). If
      a shared payload budget arrives in the merge, chat history goes empty
      on every request with no error. Confirm the two limits in the merged
      tree, then confirm a real request returns non-empty history. We
      touched no API file (0 commits), so we cannot be the source - the
      merge still can.
  V7  7fe50fd, the reference laundering severed at the state layer
      (onStateApplied). Named instrument: tools/pitch_mode_test, the "reference provenance:" block - the checks
      "laundered state (manual + value, no marker) reverts to AUTO on load",
      "marked manual 442 survives a save/load round-trip", "auto survives a
      save/load round-trip", and the SEAN'S SAVED STATE block. IF YOU MOVE
      the do-not-dial guard, 7fe50fd is re-checked against its new position
      and V7 runs again - a clean auto-merge today is not one after that fix.
Plus, after each stage: the build (-j 4, explicit targets), AU and VST3
both load, the pitch suite 196-200 PASS / 0 FAIL, and every value in
DERIVED_VALUES_SINCE_BASE.md read back and compared.

## Staging, unchanged
Stage 1: base -> f6ac073. Stage 2: f6ac073 -> aa455ff. Both conflicts
(the ChainHost include block; the settings movers list) resolved KEEP-
BOTH and shown to Sean before committing. Kathy's work is not ours to
simplify. Nothing runs until Kathy confirms her side is fully pushed AND
Sean says the shoot is done.

## Follow-up (after your reply, 6 Sep)
- Confirmed: we added NO applyParams / applyStructured call sites since
  d68da09. Ours are all pre-existing: EedDeviceProcessor.cpp 79 and 230,
  EedMultibandProcessor.cpp 338 and 374, EedPitchProcessor.cpp 637 and 647,
  ChainHost.cpp 3178 - plus three your list did not carry, also
  pre-existing: SurgicalEqProcessor.cpp 559 and SurgicalEqEditor.cpp 207
  and 258. Your no-default change has nothing un-updated coming from us.
- Predicted conflict #3 is written down with its merged shape (survey
  section 9): setStateInformation keeps applyingState_ = true /
  resetParamsToDefaults / applyParams(..., <ParamSource>) / applyingState_
  = false / onStateApplied(). Neither side wholesale.
- Your defect is recorded as DATA LOSS (defaults written, nothing
  restored), your find, and your fix shape (no default, inert on the
  restore path by construction) as the right one.
- feat/ejmap e727891 (echojay-vst) is a fast-forward of 131aa5e, fine.
  feat/bulk-ingest b76df80 (echojay-saas) - CORRECTED, see the follow-up
  below: our first version of this bullet said it was not on the remote.
  It is a branch of echojay-saas and we had fetched echojay-vst. The pair
  still lands together or not at all.
- V8 (explicit ParamSource at every call site) and V9 (saved values survive
  a reload with do-not-dial ON, with the pre-fix run as the positive
  control) are added to the plan.


---

## Follow-up 2 (6 Sep 2026): a correction, ours; and one note, offered not pressed

CORRECTION. We told you feat/bulk-ingest b76df80 "is NOT on the remote as
of our fetch" and asked you to re-push it. That was wrong, and the error
was ours. b76df80 is a branch of echojay-saas, a different repository, and
we had fetched echojay-vst. A ref of echojay-saas could never have shown up
in that fetch, so its absence told us nothing, and the report that it never
landed was false. Please disregard the re-push request. So: ONE push failure
today, not two, and it was ours (fd43f4d, a GitHub auth drop in the agent
shell), not yours. We are saying this explicitly because a wrong shared fact
corrodes confidence in the ones that were right, and the rest of that
message stands.

Rule we have filed from it, so it does not recur: every hash on the record
carries its repository name, and a ref's absence is evidence only in the
repository that owns it. The staging note now reads: the two halves of the
gzipped-contribute change span two repositories, e727891 (echojay-vst) and
b76df80 (echojay-saas); they still land together, and the check that both
are present is two ls-remotes against two remotes, printed side by side with
the local hashes.

ONE NOTE, entirely your call, different project: WEB_MIXER_BUILD_SPEC.md is
870 lines that exist in exactly one place on one laptop. It is the same
exposure we spent today eliminating on Sean's Mac (133 commits on no
remote). Worth a commit somewhere if you want it. Offered, not pressed.

Still holding on our side for your hash table, Sean's push of fd43f4d, and
the end of the shoot.
