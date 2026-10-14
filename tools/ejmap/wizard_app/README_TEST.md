# EchoJay Wizard Test - for Kathy

This is the **release** wizard: sign in, upload and the free-month code. The server is a stand-in that lives inside the app, so **nothing leaves this Mac**. An orange banner on every screen says so.

The stand-in server starts with what Sean's cert 5 knows, so "already mapped" works the way the real server will. Its own folder and its TEST icon keep it apart from the Beta and the real app.

**1. Install.** Open **EchoJay Wizard Test.dmg**, drag **EchoJay Wizard Test** onto **Applications**, and close the window.

**Open it from Applications**, not from the disk image's window: a copy run from the disk image stops working when the image is ejected.

**2. Allow it to open (once).** In Terminal, paste this and press Return. Nothing appearing means it worked:

```
xattr -dr com.apple.quarantine "/Applications/EchoJay Wizard Test.app"
```

If it says "Operation not permitted": move the app to the Bin, install it again, and do this step before opening it.

**3. What to try**
- **Sign in.** Any email and any password work; they only name a pretend account. Then the consent screen, then the scan.
- **A small run: a full finish and the free month in under an hour.** By default the wizard scans the whole Mac, and the free month needs **25** new plugins (the server's rule). For a short test, open the settings file (`server_stub/config.json`, see below), replace its contents with this, save it, and open the wizard:

  ```json
  {
    "qualify_minimum": 25,
    "once_only": true,
    "test_only_plugins": ["DeEsser (m)", "RDeEsser (m)", "Sibilance (m)", "C1 gate (m)", "TransX Wide (m)"],
    "test_qualify_minimum": 3
  }
  ```
  - `test_only_plugins`: the wizard scans and measures only these five Waves plugins. They load on this Mac without a licence window and each measures in a minute or two.
  - `test_qualify_minimum`: the free month needs 3 accepted results instead of 25, a little below 5 in case one can't be measured.
  - Both keys exist only in the Test build; the beta and the real app ignore them. To go back to the whole Mac, set `"test_only_plugins": []` and remove `test_qualify_minimum`.
  - Below the minimum you see "Thanks - your plugins are already mapped" with **Contribute anyway (no reward)**.
- **Pause and reopen.** On the Measuring screen, press **Pause - carry on later**, or close the window and choose Pause. Then quit, open the wizard again, and it carries on the same run.
- **Send what's done.** Press **Send what's done (no free month)** part-way. The results go to the stand-in server with no code, and the account and this Mac have used their go.
- **Finish.** Let a small set run to the end. If the whole run finished and enough results passed, the stand-in server issues an **EJ-FREE-...** code.
- **Once only.** Opening it again after a send or a finish shows the reward or "You've already taken part".
- **Licence windows.** A plugin that isn't licensed here may flash a licence window once. Don't click it: the wizard closes it and never loads that plugin again (`engine/licence_windows.json`; Reset test forgets them).
- **"Waves plugins skipped - check Waves Central"** means one of Waves' shared plugin files didn't load, so the wizard skipped its other Waves plugins rather than wait on each. Open Waves Central, check the plugins and licences there, then open the wizard again.
- **Reset test** (bottom-left, on any screen while nothing is running - after a finish the wizard reopens on the reward screen) clears the pretend accounts, the Mac's record, codes, runs, uploads and the app's state, so you can test once-only and the reward again. It **keeps your settings file**.

**4. Where to look**

Everything lives in `~/Library/Application Support/EchoJay Wizard Test/`. In Finder, press ⌘⇧G and paste that path.

| File | What it is |
|---|---|
| `server_stub/config.json` | **The server's settings - edit me.** `qualify_minimum` (default 25), `once_only` (default true), and the Test build's `test_only_plugins` / `test_qualify_minimum` (above). |
| `server_stub/uploads/` | Every upload as it arrived (`direct_*.zip`) and its receipt (`*.receipt.json`): kind (checkpoint / sent_early / finished), accepted count, and whether it earned a code and why not. |
| `server_stub/runs/<run_id>.json` | The merged run: each plugin's latest state and whether it was accepted. |
| `server_stub/participation.json` | The accounts and Macs that have taken part (once-only). |
| `server_stub/promo_ledger.json`, `server_stub/outbox/` | The codes issued, and the "email" each would have sent. |
| `server_stub/db/` | What the server "already has" (the cert 5 snapshot). |
| `app.log`, `engine/wizard.log` | What the app and the measuring engine did, step by step. |
| `outbox/` | Uploads waiting while "offline" (empty unless a send failed). |

No times are shown on screen. The app works out how long a run will take and writes that to `app.log`, which is the place to look if you want it.
