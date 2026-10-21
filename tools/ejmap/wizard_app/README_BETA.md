# EchoJay Wizard Beta: building it, and what testers do

## Building it (us)

```
cmake -B build-ejmap -DEJ_WIZARD_BETA_SERVER_URL=<staging base URL> -DEJ_WIZARD_BETA_CLIENT_ID=<public OAuth client id>
cmake --build build-ejmap --target ejmap ejmap-wizard-beta EchoJayProbe -j4
tools/ejmap/wizard_app/assemble.sh build-ejmap <out folder> --beta --sign "Developer ID Application: Sean Donoghue (8BT5F9B887)" --dmg
```

This gives `EchoJay Wizard Beta.app` and `EchoJay Wizard Beta.dmg`.

**The server, per build (21 Oct):** the beta talks to the server named at configure time (staging = Sean's test server, live later; contract: `FOR_SEAN_21oct/WIZARD_SERVER_CONTRACT_v1.md`). Without one it says "not connected yet". `assemble.sh` refuses to pack an app that carries anything like a token or a key (`scripts/check_bundle_secrets.sh`).

Inside the app:
- **Signatures:** the app, its engine (`ejmap`) and its probe (`EchoJayProbe`) are all signed with the team's Developer ID (8BT5F9B887), hardened runtime and a timestamp. The probe and the engine carry the probe's entitlements. The disk image is signed too.
- **The mock DB:** the 290 agreed references are in Contents/Resources.

**Not notarised.** Notarisation still waits on Apple's Developer Program agreement (the 403 in `docs/NOTARISATION_SETUP.md`). macOS therefore marks the app as "downloaded from the internet" and refuses to open it. Every tester has to do the one-time step below.

## What a tester does (send them this)

**1. Install.** Double-click **EchoJay Wizard Beta.dmg**. In the window that opens, drag **EchoJay Wizard Beta** onto the **Applications** folder. Then close that window.

**Open it from Applications**, not from the disk image's window: a copy run from the disk image stops working when the image is ejected.

**2. Allow it to open (once).** macOS blocks apps it hasn't checked yet, so this one-off step tells it the wizard is fine.
1. Open **Terminal**. Press ⌘ and Space together, type **Terminal**, and press Return.
2. Copy this whole line, paste it into the Terminal window, and press Return:

   ```
   xattr -dr com.apple.quarantine "/Applications/EchoJay Wizard Beta.app"
   ```
   Nothing appears when it works; that is normal.
3. To check, paste this line and press Return. It must show **nothing**:

   ```
   xattr -lr "/Applications/EchoJay Wizard Beta.app" | grep quarantine
   ```
4. Close Terminal.

If step 2 says **"Operation not permitted"**, macOS has already tried to open that copy and now protects it:
1. Drag **EchoJay Wizard Beta** from Applications to the Bin.
2. Install it again from the .dmg (step 1).
3. Do step 2 before opening anything.

**3. Run it.** Open **EchoJay Wizard Beta** from Applications.
- **Sign in:** press **Sign in with EchoJay**. Your browser opens on echojay.ai: sign in there, check the code matches the one in the wizard, and approve. The wizard never sees your password.
- **The order:** read the consent, tick **I agree**, and press Continue. The wizard lists your plugins, asks EchoJay which it already knows ("N already known, M to measure"), and measures the rest.
- **Leave it running, or pause.** It runs by itself in the background - best left running overnight - and the Mac stays awake. Each result is sent to EchoJay as soon as it's measured; if you're offline they wait on the Mac and go when you're back. **Pause - carry on later** stops it; opening the wizard again carries on. **Finish and send what's done** ends the run early.
- **Licence windows:** some plugins may briefly show a licence or sign-in window. That's normal - please don't click it; the wizard closes it, moves on, and won't load that plugin again.
- **"Waves plugins skipped - check Waves Central"** means one of Waves' shared plugin files didn't load, so the wizard skipped its other Waves plugins rather than wait on each. Open Waves Central, check the plugins and licences there, then open the wizard again.
- **Free space:** it needs at least 3 GB free.

**4. The end.** When the run is over the wizard tells EchoJay, and EchoJay decides the free month: if you qualify, the wizard shows your code (**Copy code**). Nothing to email.
- Questions or problems: email **admin@signaturestudiosonline.com**.
- What is sent: your plugin list (names, makers, versions, formats), the measurements and each measured plugin's controls, and the wizard's and macOS's versions. No audio, no files of yours, nothing personal.

## What only another Mac can test (not this one)

| what | why not here |
|---|---|
| A licence or iLok window at load being closed, never clicked, and the plugin filed quietly | This Mac's rules forbid loading the licence-bound vendors (SSL Native, Soundtoys, UAD, APB, kHs) and clicking a licence window; the window watch is shared with the cert, where it has filed PACE windows since 28 Sep |
| A Mac with no EJ Map store and no licence file | This Mac has `~/Library/ejmap`. The rehearsal uses a fresh store under the work folder, never this Mac's |
| Demo versions from other vendors | The dropout check was measured on Melda's demos only. Another vendor's demo may mute differently: noise, a beep, or a gap after more than 60 s |
| The quarantine step on a real download, and the "Open Anyway" path | A build made here carries no quarantine flag. An emailed or downloaded .dmg must be tried on another Mac |
| Real low disk | The rehearsal fakes free space (`EJ_WIZARD_FAKE_FREE_FILE`); a nearly full volume behaves for real only on a Mac that is nearly full |
| An Intel Mac | Built and tested on Apple silicon (arm64) only |
