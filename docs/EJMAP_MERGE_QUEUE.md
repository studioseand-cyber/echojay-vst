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
- DEFECT_BRIDGED_READBACK.md (repo root, merged from v2): the real fix
  (stable-read or deferred verify, gated on bridged instances).
