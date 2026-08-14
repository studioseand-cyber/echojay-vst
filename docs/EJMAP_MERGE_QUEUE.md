# Deferred: feat/ejmap -> feat/plugin-dashboard merge (post-beta)

Deferred 10 Aug 2026 during the 2.26.0 beta preparation, deliberately:
the beta was tested end to end as dashboard + the dial batch WITHOUT
feat/ejmap's shared-source commits, so the merge adds risk the beta does
not need.

## What the merge carries

feat/ejmap is ~235 commits, almost all under tools/ejmap/**. Eight
commits touch SHARED plugin source: ChainHost.cpp (+119), ChainHost.h
(+35), EchoJayParamApply.h (+146, schema 2.3->2.4 constants + history),
PluginEditor.cpp (+24), CMakeLists (+10).

## The known twin — hunk-by-hunk reconciliation, NOT a merge

"The three silent returns now say which one fired" exists TWICE:

  - feat/ejmap commit 01d25b4 (chain: the three silent returns...)
  - feat/plugin-dashboard commit 1d42d89 (the dial batch: EJDial
    classification logging in ChainHost.cpp, same feature, evolved
    further — four failure classes, fp + appVersion on the line)

These are two sessions' implementations of one idea. Whoever merges
must resolve that region hunk-by-hunk against BOTH histories — the
dashboard version is the one that shipped in the beta and is the
baseline; anything ejmap's copy has that dashboard's lacks gets ported
deliberately, nothing gets auto-taken.

The other shared-source commits (units-outrank-number at dial time,
named-controls-resolve, controls exposure client bits) are parallel
evolution of dial-path code dashboard grew independently — expect
equivalent-but-differently-shaped code, same reconciliation posture.

## Also waiting on this queue

- Bipolar/pan signed parse: build-side (EjmapSweeper) + client readback
  (EchoJayParamApply parse) must land TOGETHER — signed anchors on an
  unsigned client revert every left-half dial. Client half rides a
  release; then a ~5-minute re-sweep of the 9 Melda MB plugins.
- DEFECT_BRIDGED_READBACK.md (repo root, merged from v2): the report-only
  mitigation shipped in the 2.26.0 beta (EchoJayBridgedAU.h detection +
  applyOne demotion, proven live on API-2500); the REAL fix (deferred
  stable-read verification, render-cycle aware) is still owed. Two facts
  the beta work measured past the filing: in-stack getValue() is ALSO
  pre-write on the bridge (the filing's setread-immunity premise is
  false), and bridge parameter traffic flushes with render cycles.

## The stale-branch filing rule

A FILING ON A STALE BRANCH NEEDS ITS BASE NAMED. Twice in one week a
document was true when written and false where it was read:
DEFECT_LADDER_TOLERANCE.md described code its own branch carried while
the mainline had already fixed it (1 Aug, typedReadbackMatch) -- the
filing was 297 commits behind and never said so. Any future filing
states the commit it was diagnosed against, and any reader checks the
described code EXISTS at the reading branch before building the fix --
the defect it describes may have been fixed before the filing was even
written.
