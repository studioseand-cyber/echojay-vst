# EchoJay V2 Release Runbook (macOS signing, notarization, Windows CI)

This is the packaging and signing sequence for a beta or release build. It
captures the identities and profiles that previously lived only in shell
history. Keep it updated when identities or scripts change.

## 0. Versions

Single source of truth is `CMakeLists.txt`:

- Main plugin: `project(EchoJay VERSION x.y.z)` (currently 2.23.0)
- Link: `set(LINK_VERSION "a.b.c")` (currently 0.8.4)

`build-installer.sh` carries matching `VERSION` / `LINK_VERSION` variables,
and `package-dmg.sh` carries `VERSION`. Bump all of them together.

After install, verify in the plugin: Settings shows the main version, and the
Link editor reports its own. Clean-load discipline for local testing: full
quit the DAW, then

```
killall AudioComponentRegistrar AUHostingServiceXPC_arrow 2>/dev/null
~/reinstall-v2.sh   # local dev install of both AU components from build/
```

## 1. Build (Release, universal)

```
cmake --build build --config Release
```

Artefacts (verified names the scripts depend on):

- `build/EchoJay_artefacts/Release/VST3/EchoJay V2.vst3` (id com.echojay.plugin.v2)
- `build/EchoJay_artefacts/Release/AU/EchoJay V2.component` (id com.echojay.plugin.v2)
- `build/EchoJay_artefacts/Release/AAX/EchoJay V2.aaxplugin`
- `build/EchoJayLink_artefacts/Release/VST3/EchoJay Link.vst3` (id com.echojay.link)
- `build/EchoJayLink_artefacts/Release/AU/EchoJay Link.component` (id com.echojay.link)

Sanity check both AUs before packaging:

```
auval -v aufx EcJ2 Ecjy   # EchoJay V2
auval -v aufx EjLk Ecjy   # EchoJay Link
```

## 2. AAX / PACE signing (before building the installer)

AAX must be PACE-signed or Pro Tools will refuse to load it. Requirements:

- PACE Eden tools installed (wraptool). AS OF 6 Sep 2026: Eden 6.0.1 GM,
  and wraptool is NOT on PATH - its absolute path is
  `/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool`
  (an earlier note said Versions/5; that is stale and cost time). The Eden
  tools come from PACE Central, which is reached from a BUTTON INSIDE iLok
  License Manager, not from a web portal - that is the thing nobody could find.
- The AAX SDK: CMake looks at `~/AAX_SDK` (Interfaces/AAX.h); a differently
  named unpack is passed as `-DAAX_SDK_PATH=<root>` instead of being moved
  or reshaped. 6 Sep 2026: SDK 2.9.0 (`aax-sdk-2-9-0`, AAX.h at
  Interfaces/AAX.h) configured with `-DAAX_SDK_PATH`; the configure line to
  QUOTE is "AAX SDK found at <path>" - without it AAX is silently absent.
- The iLok (physical) plugged in, holding the signing credential
- PACE account: `seand123`
- wcguid: `B4184F90-2F4F-11F1-A9B9-00505692C25A` - CONFIRMED FROM PACE CENTRAL
  on 6 Sep 2026 (the button inside iLok License Manager; the authoritative
  source, not this file): wrap configuration for product "Echojay", created
  2026-04-03, SDK Version 5 (an EDEN 5 config), experience "Signing Only"
  (digitally sign true, encrypt false, Fusion false, non-Beta wrapper). The
  July 2026 bundles were wrapped by Eden 5.10.5 against it - the
  demonstrated-good pairing. Eden 6 against this v5 config is UNTESTED as of
  this note; any wraptool error naming an SDK, wrapper or Eden version is
  that fork, resolved by a PACE support request for a v6 config or by Eden
  5.10.5 alongside - never by guessing. The bundle itself does not record its
  wcguid (its dsig carries the publisher and product GUIDs only), so this
  value can only ever be re-checked in PACE Central.
- Apple signing identity used by wraptool:
  `Developer ID Application: Sean Donoghue (8BT5F9B887)`

Canonical wraptool invocation (wraptool prompts for the iLok password, so it
is run from a Terminal with a TTY, by a person). Every flag below was checked
against Eden 6.0.1's `sign --help` on 6 Sep 2026: --verbose (-V), --account,
--wcguid (-G), --signid (-I), --in, --out all exist unchanged; no flag was
renamed and none gained a required companion. Eden 6 also offers `verify`
(`wraptool verify --in <bundle>`), used as the post-sign check together with
`codesign -dv --verbose=4`.

```
/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool sign --verbose \
  --account seand123 \
  --wcguid B4184F90-2F4F-11F1-A9B9-00505692C25A \
  --signid "Developer ID Application: Sean Donoghue (8BT5F9B887)" \
  --in  "build-release/EchoJay_artefacts/Release/AAX/EchoJay V2.aaxplugin" \
  --out "build-release/EchoJay_artefacts/Release/AAX/signed/EchoJay V2.aaxplugin"
```
OUT-OF-PLACE, deliberately: --out is a different path from --in, so the
unsigned original survives and a half-completed wrap costs a retry, not a
rebuild. Then `wraptool verify --in <the signed bundle>` and
`codesign -dv --verbose=4 <the signed bundle>`; only a bundle that passes
both goes to /Library/Application Support/Avid/Audio/Plug-Ins/. Same for
`EchoJayLink_artefacts/Release/AAX/EchoJay Link.aaxplugin` -> its own
`signed/`. The build directory is whichever one built the bundle.
--signid must match `security find-identity -v -p codesigning` on the
signing Mac character for character.

Without the iLok present this step fails; VST3/AU packaging can proceed
without it (deselect AAX in the installer or accept the unsigned-AAX build
for non-Pro-Tools betas).

### THIS MAC CANNOT SIGN AAX (finding, 6 Sep 2026 - it cost most of an afternoon)
wraptool 6.0.1 is installed on the MacBook, but the Eden Tools licence on
iLok_1AC756 and the SDK-5 wrap config (above) are Eden 5 generation, and 6.0.1
demands a Fusion Tools licence for its operations: every wraptool call here
(info, verify, and sign would be the same) fails with
`WrapToolException::MissingFusionToolsLicense: No valid license found in an
iLok USB or iLok cloud`. There is also NO Developer ID Application certificate
in this Mac's keychains (`security find-identity -v -p codesigning`: 0), and
full Xcode is not installed (command line tools only). Eden 5 is NOT offered
on the current PACE download page (only "PACE Code Signing for AAX SDK Mac
v6.0.1"). SIGNING HAPPENS ON THE OTHER MAC - the one with a working Eden, the
licence and the certificate, where the July 2026 bundles were signed:
wraptool signs an already-built bundle, so that machine needs no SDK, no
JUCE and no repo; the unsigned bundles travel as ditto archives with
`ditto -c -k --sequesterRsrc --keepParent` (see the handoff note pattern in
MERGE_2026-09-06.md). OPEN QUESTIONS FOR PACE, for whoever raises them: renew
or confirm the Eden Tools licence, and obtain either Eden 5.10.5 (the
demonstrated-good tool for the SDK-5 config) or a Fusion Tools licence for 6.

## 3. Build the installer .pkg

```
./build-installer.sh
```

Produces `EchoJay-V2-v<VERSION>-Installer.pkg` containing five selectable
components: EchoJay V2 VST3 / AU / AAX and EchoJay Link VST3 / AU. The AU
components run a postinstall that kills AudioComponentRegistrar so hosts
rescan; the AAX component clears the Pro Tools validation cache.

## 4. Sign the .pkg (Developer ID Installer)

```
productsign --sign "Developer ID Installer: Sean Donoghue (8BT5F9B887)" \
  "EchoJay-V2-v<VERSION>-Installer.pkg" \
  "EchoJay-V2-v<VERSION>-Installer-Signed.pkg"
```

Note the identity difference: the .pkg is signed with the Developer ID
INSTALLER certificate; bundles/binaries (and wraptool's --signid) use the
Developer ID APPLICATION certificate. Both are under team 8BT5F9B887.

## 5. Notarize + staple

The notarytool keychain profile is `EchoJayNotarize` (stored via
`xcrun notarytool store-credentials EchoJayNotarize` on this machine; recreate
it with the Apple ID + app-specific password + team 8BT5F9B887 if missing).

```
xcrun notarytool submit "EchoJay-V2-v<VERSION>-Installer-Signed.pkg" \
  --keychain-profile EchoJayNotarize --wait
xcrun stapler staple "EchoJay-V2-v<VERSION>-Installer-Signed.pkg"
```

`--wait` blocks until Apple returns Accepted. If it returns Invalid, fetch the
log with `xcrun notarytool log <submission-id> --keychain-profile EchoJayNotarize`.

## 6. Wrap in DMG

```
./package-dmg.sh
```

Picks up the `-Installer-Signed.pkg` (falls back to unsigned if absent, so run
it AFTER step 5) and produces `EchoJay-V2-v<VERSION>.dmg` with the pkg named
"Install EchoJay V2.pkg" inside.

## 7. Windows

Windows builds run in GitHub Actions: `.github/workflows/build-windows.yml`
in this repo, triggered by pushing a `v*` tag or manual workflow dispatch.
It builds the VST3 with MSVC + JUCE 8.0.12 and packages an Inno Setup
installer uploaded as a workflow artifact (and attached to the GitHub release
on tag builds).

Historical/public releases live on the `studioseand-cyber/echojay-vst`
GitHub repo (the in-app update URLs in the web app's vst-config point at its
releases).

KNOWN GAP (as of 2.23.0): the workflow still stages v1 artefact names
(`EchoJay.vst3`, AppVersion 1.6.2) and does not build EchoJay Link. It must
be updated to the "EchoJay V2" artefact names and to include Link before a
real v2 Windows release.

## Carry into the public release notes

**Hosted plugin settings now survive a DAW save, and DOWNGRADING loses them.**
From the Session B state work, a saved session carries each hosted plugin's
own settings in a new `chainSlotState` key alongside the existing
`chainSlotsXml` identity block.

- Opening an older session in this build works exactly as before: the chain
  rebuilds and the plugins load at their defaults, because there was nothing
  saved to restore.
- Opening a NEW session in an OLDER build also works: the chain rebuilds with
  order, bypass and wet intact, and the older build simply ignores the
  settings key.
- But if that older build then RE-SAVES the project, the settings are gone
  for good. An older build cannot preserve a key it has never heard of, and
  no change on our side can retrofit it. Anyone downgrading mid-project needs
  to know this before they hit Cmd-S.

The mitigation lives at a different layer: a chain saved through the API keeps
its state regardless of what a downgraded build does to the session file,
which is an argument for shipping both halves of Session B together.

## Release gate: the dev transport must not be in the artefact

`ECHOJAY_DEV_TRANSPORT` must be OFF for any release artefact. The Session B
development transport (preview base URL + the Vercel protection-bypass
secret) is compiled out unless `ECHOJAY_DEV_TRANSPORT` is defined, which
happens automatically in Debug and otherwise only when someone passes
`-DECHOJAY_DEV_TRANSPORT=ON`. `build-installer.sh` never does.

THE CACHE TRAP: that option is a CMake cache variable, so once any configure
run sets it ON it stays ON in that build directory for every later build,
including Release builds, until someone flips it back. A 2.26.02 pkg shipped
with the transport compiled in for exactly this reason: the cache had been ON
for days and nothing re-asserted it. Check the cache directly:

```sh
grep ECHOJAY_DEV_TRANSPORT build/CMakeCache.txt   # must say OFF for a release
```

and flip it off without a full reconfigure via
`cmake -DECHOJAY_DEV_TRANSPORT=OFF build` (then rebuild).

The secret grants access to every protected deployment on the project, so
also verify the ARTEFACT rather than trusting the source or the cache. The
one-number form of the check is:

```sh
strings "<component>/Contents/MacOS/EchoJay V2" | grep -c "x-vercel-protection-bypass"
```

which must print `0` for a release build. The fuller sweep checks what
actually shipped, which is the only check that answers the question:

```sh
BIN="build/EchoJay_artefacts/Release/AU/EchoJay V2.component/Contents/MacOS/EchoJay V2"
for pat in protection-bypass vercel.app dev.json protectionBypass; do
  printf "%-20s " "$pat"
  strings "$BIN" | grep -qi "$pat" && echo "FAIL" || echo "clean"
done
strings "$BIN" | grep -c "www.echojay.ai"   # must be > 0
```

All four must print `clean`, and the production endpoint must still be present.
Run it against the AU, the VST3 and the AAX. Last verified clean on 2.23.49.

## Quick checklist

1. Bump versions (CMakeLists + build-installer.sh + package-dmg.sh)
2. `cmake --build build --config Release`
3. auval both plugins
4. wraptool sign the AAX (iLok in)
5. `./build-installer.sh`
6. `productsign` (Developer ID Installer, 8BT5F9B887)
7. `notarytool submit --keychain-profile EchoJayNotarize --wait` + `stapler staple`
8. `./package-dmg.sh`
9. Tag `v<VERSION>` and push for the Windows CI build
10. Install from the DMG on a clean account; verify Settings shows the version
