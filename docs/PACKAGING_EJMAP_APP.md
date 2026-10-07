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
#   the FOLLOW-UP build (tone checks + its own re-sweeps, 5 Oct early): 17ebf114 - docs/SEAN_MAC_TONECHECK.md; v1.8-v2.1 rules,
#   the 4 Oct rules from Sean's zip (rounds 1-3), the sidechain left unconnected + the evidence-based re-sweep, inert, tuner plan v2,
#   Sean's stepped rule; re-derives from the batch's traces, re-sweeps only the rows whose plan (or policy reading) changed
#   the PHASE B build (the follow-up + --phaseb-all / --phaseb-status, 5 Oct evening): b0258a7b - docs/SEAN_MAC_TONECHECK.md step 2;
#   its certification results equal 17ebf114's (step 6 evidence in tools/ejmap/cert-traces/2026-10-05-phaseb/) except inert = needs_licence
#   and a sidechain re-sweep no longer repeated nightly. 17ebf114 STAYS THE FALLBACK: if anything about b0258a7b is in doubt, build 17ebf114.
#   THE 6 OCT BUILD (Kathy's rulings 1-3 and the write fix): 804a0a56 - docs/SEAN_MAC_TONECHECK.md steps 1 and 2.
#   DO NOT USE 17ebf114 OR b0258a7b FOR THE COMPRESSOR FOLLOW-UP: both carry the write bug (the re-derive dropped every record's
#   sweep-time writes, so a tone check wrote Zip's ratio at 1:1 and the VBC profiles said mix 0 %) and the collapse bug (the
#   landing read wrote the picked candidate's view over the record). 804a0a56 repairs both from the records' own traces.
#   TONIGHT'S BUILD (6 Oct evening, Kathy's amendment): 9d3a1972 - the follow-up untouched (derive-only equal to 804a0a56 on Sean's zip and
#   the rehearsal set: 0 differences), plus Phase B: --redo gain-cal,timing (the gain spec v0.1 verdicts + acceptance re-measure and an
#   ej_gain_profile/1 draft; the timing spec v0.1 second pass, scaled hold, gr_shift_db, the pick as the amount and a `time` draft) and
#   --redo nothing_nominated (measurement nominates when the lexicon finds nothing). 804a0a56 IS THE FALLBACK for tonight.
#   TONIGHT'S BUILD, FINAL (6 Oct 16:12, Sean's ruling on the Vocal Compressor): 0d2ccd5d - as 9d3a1972 plus: an attempted, unmeasured
#   detector exports as detector_f null / detector_f_source "unknown" (reason in notes), measured profiles carry no detector_f_source,
#   a null detector_f is tone-checked at both L_ref values, the detector retries at the -27 dBFS position. 9d3a1972 IS THE FALLBACK.
#   THE 7 OCT BUILD (Kathy's PLAN CHANGE list 1-6, NEXT BUILD A-F, Sean's UAD licence rulings, the strip's compressor section; the
#   overnight report docs/REPORT_2026-10-07_overnight.md): 0f175c51 - certification derive-only EQUAL to 0d2ccd5d on Sean's current folder,
#   his 4 Oct zip, cert_sc and cert_tc35 except the "detector_f_source": "measured" key on measured profiles (tools/ejmap/cert-traces/2026-10-07-equality/).
#   New: --redo tuners / gain-all / uad / no_pool / multiband / strips / combined / material / frequency / samplerate, --categorise-propose,
#   --licence-check / --licence-stamp / --uad-preflight, --zip --since. 0d2ccd5d IS THE FALLBACK for the 7 Oct build.
#   THE 7 OCT BUILD, FINAL (Kathy's "before Sean gets it": the licence file governs only its vendors' plugins, the LC / GE ruling, A1's three
#   reads at a scaled hold): dc77d0a5 - derive-only EQUAL to 0d2ccd5d on the four sets except "detector_f_source" (cert-traces/2026-10-07-equality/).
#   0f175c51 IS THE FALLBACK. The runbook's top section ("Running the 7 Oct build") is the one Sean follows.
git -C "$REPO" fetch && git -C "$REPO" checkout dc77d0a5     # branch feat/ejmap-cert; the commit after it is this documentation (0f175c51: the fallback)
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

**6 Oct 16:12, tonight's build, final (0d2ccd5d):** rebuilt from the clean checkout (stamp `0d2ccd5d`), packaged into
`~/Desktop/ejmap-dist-6oct-c`, both signatures Developer ID / team 8BT5F9B887; preflight exit 0; from the packaged app Zip through
the follow-up (1.98 dB PASS, the A/B the same) and Lindell SBC through `--phaseb-all --category timing`. Derive-only against
9d3a1972 on Sean's current folder, his 4 Oct zip, cert_sc and cert_tc35: rows, records, controls and logs identical; the one
difference is the `detector_f_source` key removed from every measured profile (Sean's amendment). The unknown-detector path
rehearsed live on Lindell SBC with an injected unmeasurable detector.

**6 Oct 15:00, tonight's build (9d3a1972):** rebuilt from the clean checkout (stamp `9d3a1972`), packaged into
`~/Desktop/ejmap-dist-6oct-b`, both signatures Developer ID / team 8BT5F9B887; preflight exit 0 from inside the bundle; from the
packaged app, Lindell SBC through `--phaseb-all --category gaincal --category timing` (19 s + 11 s, the gain-profile draft and
the timing draft written), `--phaseb-status` read back. Certification derive-only equal to 804a0a56 on Sean's 4 Oct zip (181
rows, 245 records, 45 profiles), cert_sc and cert_tc35: 0 differences.

**6 Oct 12:46, the build 804a0a56:** both targets rebuilt from the clean checkout (`EjmapBuildInfo.h` stamps `804a0a56`,
`strings` finds it in the binary), packaged with `package_app.sh` into `~/Desktop/ejmap-dist-6oct`, both signatures Developer
ID / team 8BT5F9B887 with timestamps; `--cert-preflight` from inside the bundle finds the probe beside the executable (cdhash
a3358115…), exit 0; from the packaged app, the follow-up over a copy of Sean's folder sliced to Zip: the writes restored from
the traces, the trace's policy line `self-keyed (as EchoJay 04e)`, GR 1.98 dB at g = 2 PASS, the A/B 1.98 → 1.98 "the same".

**5 Oct 18:06, the PHASE B build (b0258a7b):** both targets rebuilt from the clean checkout (`EjmapBuildInfo.h` stamps
`b0258a7b`, no `-dirty`; `strings` finds it in the binary), packaged with `package_app.sh` into `~/Desktop/ejmap-dist-phaseb`,
both signatures Developer ID / team 8BT5F9B887 with timestamps; `--cert-preflight` from inside the bundle finds the probe
beside the executable (cdhash 36e1198a…), exit 0; from the packaged app, `--phaseb-all` on one product wrote its row with
that probe's cdhash and gzip traces `gunzip -t` opens, and `--phaseb-status` read it back.

Tested on the operator's Mac, 2 Oct 11:25: `package_app.sh` on the RelWithDebInfo app + the Release
probe, both signed with the same Developer ID; `--cert-preflight` from inside the packaged bundle found
the probe by default (team 8BT5F9B887, cdhash e3db6a2a…). Not tested here: the single-tree build of
both targets (this Mac's probe lives in `build-release/`); the from-scratch configure was tested 1 Oct.
