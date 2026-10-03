# Packaging EJ Map the way a stranger gets it — ONE-TIME steps (2 Oct 2026; the follow-up build 3 Oct)

These steps are done ONCE on the Mac that builds (Sean's, with his Developer ID). They are not mapper
steps: the mapper (`docs/SEAN_MAC_RUNBOOK.md`) uses only the finished `ejmap.app`.

What comes out: one `ejmap.app` with `EchoJayProbe` inside `Contents/MacOS`, both signed with the same
Developer ID (hardened runtime, timestamp, the probe's entitlements), so every certification command
finds the probe beside its own executable with no `--probe` flag, and PACE-wrapped plugins accept the
probe's signature.

## 1. Build both targets in one tree (RelWithDebInfo)

```
REPO=~/src/echojay-vst            # wherever the checkout is
# TWO BUILDS, TWO COMMITS - never mix them:
#   the BATCH build (Sean's run, 2 Oct):   36397676   - what the runbook's batch ran on; keep this app as it is
#   the FOLLOW-UP build (tone checks, 3 Oct evening): dcf7fb73 - docs/SEAN_MAC_TONECHECK.md; v1.8-v1.10 rules, no sweeps, re-derives from the batch's traces
git -C "$REPO" fetch && git -C "$REPO" checkout dcf7fb73     # branch feat/ejmap-cert; the commit after it is this documentation
cd "$REPO"
cmake -S . -B build-ejmap -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEJ_BUILD_AAX=OFF
cmake --build build-ejmap --target ejmap EchoJayProbe -j 4
```

`-j 4`, not bare `-j` (a 16 GB Mac swaps to death on an unlimited build). Expect 10-20 minutes from
scratch. Outputs:

```
build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app
build-ejmap/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe
```

## 2. Package and sign

```
security find-identity -v -p codesigning | grep "Developer ID Application"      # the identity string, with (TEAM)
tools/ejmap/packaging/package_app.sh \
    build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app \
    build-ejmap/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe \
    ~/Desktop/ejmap-dist \
    "Developer ID Application: Sean Donoghue (8BT5F9B887)"
```

The script copies the app, puts the probe inside `Contents/MacOS`, signs the probe then the app
(`--options runtime --timestamp --entitlements tools/au_instantiate_probe/EchoJayProbe.entitlements`),
and verifies both strictly. It prints the two signatures; both must show `Authority=Developer ID
Application: ...` and the same `TeamIdentifier`. A keychain prompt here is normal the first time
(Always Allow). The timestamp needs the network.

## 3. Prove it, once

```
~/Desktop/ejmap-dist/ejmap.app/Contents/MacOS/ejmap --cert-preflight
```

prints the executable, the app's signature, the probe it will use (**must say "beside the executable:
the default"**), the probe's signature (team + cdhash), the cert root (`~/Library/ejmap/cert`), the
ledger, the iLok, the power. Exit 0 = the probe verifies. From here the mapper runbook applies and
nothing on this page is needed again until the code changes.

## What was tested where

Tested on the operator's Mac, 2 Oct 11:25: `package_app.sh` on the RelWithDebInfo app + the Release
probe, both signed with the same Developer ID; `--cert-preflight` from inside the packaged bundle found
the probe by default (team 8BT5F9B887, cdhash e3db6a2a…). Not tested here: the single-tree build of
both targets (this Mac's probe lives in `build-release/`); the from-scratch configure was tested 1 Oct.
