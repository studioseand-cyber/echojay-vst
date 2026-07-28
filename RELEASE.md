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

- PACE Eden tools installed (wraptool)
- The iLok (physical) plugged in, holding the signing credential
- PACE account: `seand123`
- wcguid: `B4184F90-2F4F-11F1-A9B9-00505692C25A`
- Apple signing identity used by wraptool:
  `Developer ID Application: Sean Donoghue (8BT5F9B887)`

Canonical wraptool invocation (adjust paths per PACE Eden docs if the tool
version changes; wraptool prompts for the iLok password):

```
wraptool sign --verbose \
  --account seand123 \
  --wcguid B4184F90-2F4F-11F1-A9B9-00505692C25A \
  --signid "Developer ID Application: Sean Donoghue (8BT5F9B887)" \
  --in  "build/EchoJay_artefacts/Release/AAX/EchoJay V2.aaxplugin" \
  --out "build/EchoJay_artefacts/Release/AAX/EchoJay V2.aaxplugin"
```

Without the iLok present this step fails; VST3/AU packaging can proceed
without it (deselect AAX in the installer or accept the unsigned-AAX build
for non-Pro-Tools betas).

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

The Session B development transport (preview base URL + the Vercel
protection-bypass secret) is compiled out unless `ECHOJAY_DEV_TRANSPORT` is
defined, which happens automatically in Debug and otherwise only when someone
passes `-DECHOJAY_DEV_TRANSPORT=ON`. `build-installer.sh` never does.

That secret grants access to every protected deployment on the project, so
verify the ARTEFACT rather than trusting the source. This checks what actually
shipped, which is the only check that answers the question:

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
