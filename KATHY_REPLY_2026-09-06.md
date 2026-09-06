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


---

## Follow-up 3 (6 Sep 2026): the merge is done on a branch; three things for you

Sean moved the merge BEFORE the shoot. Both stages are on merge/kathy-2026-09-06
(0075fe8 base..f6ac073, 9177b96 f6ac073..aa455ff). Exactly the two predicted
conflicts (#1 the include block, #2 the movers list), resolved as written; #3
auto-merged with onStateApplied() intact and V7 passes. Build clean, both
formats load and pass audio, pitch suite 196/0, every derived value read back.
Record: MERGE_2026-09-06.md.

1. YOUR FIX IS NOT IN aa455ff. The merged tree has the guard inside
   applyParams (EedDeviceProcessor.cpp:102) and setStateInformation calling
   applyParams with no source argument (:245). V9 - our new harness,
   tools/merge_gate_tests/v9_dialwrites_restore_test.cpp - reproduces the data
   loss exactly: with the toggle ON at restore, retune 200 -> 0, natural_vibrato
   50 -> 0, flex 30 -> 0 (positive control with the toggle OFF passes). It is
   your find; the harness is the regression test for your fix. When the
   ParamSource change lands, V8's call-site list is in the record (twelve sites
   in Source/, three of them in Surgical EQ, plus four in tools/).

2. A LAYOUT DEFECT IN YOUR TREE, caught by the render, fixed on the merge branch
   in one line. Your do-not-dial toggle is placed by resized() beneath the
   auto-dial toggle and the cursor steps past it (:19477), but paintSettingsView
   does not step, so the YOUR PLUGINS heading paints half-hidden behind your
   toggle at every window size. Same code at your line 14586, so it is in
   aa455ff, not made by the merge. Fix: `y += fh + 8;` after
   label("CHAIN SUGGESTIONS") in the paint. Renders before and after are in
   tools/settings_snapshot/snapshots_2026-09-06/merge_9177b96/. If you want it
   shaped differently, the fix is one commit and yours to replace.

3. A ONE-NAME DELTA: the settings harness counts 540 enabled names resolving
   in neither format on the merged tree, 539 on the shoot build; the chain feed
   (2328, AU view 2129) is identical. Your resolver/feed changes are in this
   tree, so one name moved sides. Noted, not chased.


---

## Follow-up 4 (6 Sep 2026): your ParamSource fix is merged, gated and proven; four things back

Merged 25861e6 into merge/kathy-2026-09-06 (stage 3, db4643d + 02dcd8a;
branch pushed under that name - re-run your trial merge against it, never a
hash). Exactly the predicted conflict #3, resolved to the merged shape with
your Restore and our onStateApplied() after it. Build clean across eight
targets, both formats of both plugins load and pass audio, pitch suite 196/0,
pitch host test 18/0, your registry 1667/0, every derived value read back.

1. V9 PAIR - the regression evidence for your fix, same harness, same inputs
   (tools/merge_gate_tests/v9_dialwrites_restore_test.cpp):
     BEFORE (9177b96, your guard, no ParamSource), do-not-dial ON at reload:
       retune 200 -> 0, retune_speed_ms 150 -> 6, natural_vibrato 50 -> 0,
       targeting_ignores_vibrato 0 -> 1, flex 30 -> 0        FAIL, data loss
     AFTER (02dcd8a, your fix), same:
       200 / 150 / 50 / 0 / 30 all back                       PASS
     Positive control (setting OFF) passes both times. The pairing is the proof.

2. THE VALUES WE CHOSE AT EVERY SITE WE TOUCHED, so you can disagree with any:
   twelve sites in tools/pitch_mode_test/main.cpp that your 14 edits could not
   see (our rounds added them after the base) - ALL Assistant:
     :230 key_source auto (circularity-guard baseline), :306 correction_mode
     hard, :337/:340/:348/:355/:358 key_source auto / scale chromatic (the
     key-guard render lambdas), :411 reference_source manual (the dormant-field
     switch after a setStateInformation load), :464 correction_mode natural
     (the dial block, "the model's path" in its own words), :698 / :766 / :1136
     key_source manual + root + scale (transport-reset, verification-render and
     editor-snapshot setup).
   Reason, one for all twelve: each is the harness writing through the
   structured path, which for the pitch device the product reaches only via
   ChainHost::applyStructuredToBuiltinSlot - your Assistant - and the pitch
   editor never calls applyStructured. Reviewed by what each enclosing test
   asserts: none has restore or migration as its subject; the suite's
   restore-subject tests go through setStateInformation (13 calls), Restore by
   construction. We added no other argument anywhere; the merge took yours.

3. setChainSlotWet (LinkProcessor.cpp:2266, WetSource::Restore): it is also
   what the Link editor's wet knob calls (LinkEditor.h:787), so a HAND MOVE is
   labelled Restore there. Behaviour is identical (neither is refused), and
   authorship is NOT taken from WetSource - stampLocalRackEdit() runs on every
   setChainSlotWet and never in the restore callback, so a user's move is
   still a local edit in the log. Vocabulary only, yours to relabel to User.
   Also seen, not a loss: the Link restore callback re-dials structured
   settings (Assistant, refused under the mode) before restoring the state
   blob (Restore, lands) - the same final values as before your change.

4. TWO OF YOUR SUITES ON OUR MACHINE:
   - builtin registry: 1667 / 0 against your 1659 / 0. The eight are exactly
     two assertions each ("advertised id is settable", "advertisement is
     ASCII") for four pitch params our tree has and yours does not: retune,
     depth, seam_attack_ms, ref_manual_by_user. Closed.
   - mapfps: 678 ok, 8 FAIL against your 692 / 0 - and every failure is a
     FIXTURE, none is code (the pinned files are byte-identical between your
     tip and ours):
       wa PIN3 / wa PIN5 x2: Abbey Road TG Mastering Chain IS installed on
         this Mac (/Applications/Waves/Plug-Ins V15/), so it resolves here and
         69 of 69 answer;
       nl PIN11 x2: no Waves DeEsser here (the De-Essers in this scan cache are
         Harrison and SPL), MEASURED 0 bindings removed;
       nl PIN5: this Mac's installed set resolves a non-Waves name;
       ef PIN5 x2: reads HANDOVER/commit-features-block.txt - HANDOVER/ is
         never committed and is not on this machine.
     Reported as A DEFECT IN THE TEST, yours to fix: a pin whose result depends
     on which plugins are installed (or on an uncommitted file) can only pass
     on your machine, so the suite cannot serve as a gate anywhere else. It
     should SKIP with a reason when the fixture is absent (or present), not
     fail. Also: its Link step is `cd build && make -j$(hw.ncpu) EchoJayLink` -
     a full-core -j; this project caps at -j 4 after two swap-kills, and the
     directory is named `build` which this Mac does not have. We ran the pins
     against the fresh build-release archive with that step skipped; the Link
     compiled in the gate build.

Your V3 paint-cursor fix offer (follow-up 3) stands; it is on the merge
branch as 9eb5f34, renders before/after filed.
