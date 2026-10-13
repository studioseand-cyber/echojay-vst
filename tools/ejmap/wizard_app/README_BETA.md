# EchoJay Wizard Beta: building it, and what testers do

## Building it (us)

```
cmake --build build-ejmap --target ejmap ejmap-wizard-beta EchoJayProbe -j4
tools/ejmap/wizard_app/assemble.sh build-ejmap <out folder> --beta --sign "Developer ID Application: Sean Donoghue (8BT5F9B887)" --dmg
```

This gives `EchoJay Wizard Beta.app` and `EchoJay Wizard Beta.dmg`. Inside the app:
- **Signatures:** the app, its engine (`ejmap`) and its probe (`EchoJayProbe`) are all signed with the team's Developer ID (8BT5F9B887), hardened runtime and a timestamp. The probe and the engine carry the probe's entitlements. The disk image is signed too.
- **The mock DB:** the 290 agreed references are in Contents/Resources.

**Not notarised.** Notarisation still waits on Apple's Developer Program agreement (the 403 in `docs/NOTARISATION_SETUP.md`). macOS therefore marks the app as "downloaded from the internet" and refuses to open it. Every tester has to do the one-time step below.

## What a tester does (send them this)

**1. Install.** Double-click **EchoJay Wizard Beta.dmg**. In the window that opens, drag **EchoJay Wizard Beta** onto the **Applications** folder. Then close that window.

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
- **The order:** read the short consent paragraph, tick **I agree**, and press Continue. The wizard then checks your plugins and measures the ones EchoJay doesn't know yet.
- **Leave it running.** The Mac stays awake. Closing the window stops it, and it carries on where it left off next time you open it.
- **Free space:** it needs at least 3 GB free.

**4. Send the results.** At the end it says "Your results are ready" and saves **EchoJay-Wizard-results-<date>.zip** on your Desktop.
- **Show in Finder** points at the file.
- Email that one file to **admin@signaturestudiosonline.com** (the address is on the screen too).
- The file holds plugin names, versions, controls and measurements: no audio, no files of yours, nothing personal.

## What only another Mac can test (not this one)

| what | why not here |
|---|---|
| A licence or iLok window at load being closed, never clicked, and the plugin filed quietly | This Mac's rules forbid loading the licence-bound vendors (SSL Native, Soundtoys, UAD, APB, kHs) and clicking a licence window; the window watch is shared with the cert, where it has filed PACE windows since 28 Sep |
| A Mac with no EJ Map store and no licence file | This Mac has `~/Library/ejmap`. The rehearsal uses a fresh store under the work folder, never this Mac's |
| Demo versions from other vendors | The dropout check was measured on Melda's demos only. Another vendor's demo may mute differently: noise, a beep, or a gap after more than 60 s |
| The quarantine step on a real download, and the "Open Anyway" path | A build made here carries no quarantine flag. An emailed or downloaded .dmg must be tried on another Mac |
| Real low disk | The rehearsal fakes free space (`EJ_WIZARD_FAKE_FREE_FILE`); a nearly full volume behaves for real only on a Mac that is nearly full |
| An Intel Mac | Built and tested on Apple silicon (arm64) only |
