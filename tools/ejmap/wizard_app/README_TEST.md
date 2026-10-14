# EchoJay Wizard Test - for Kathy

This is the **release** wizard: sign in, upload and the free-month code. The server is a stand-in that lives inside the app, so **nothing leaves this Mac**. An orange banner on every screen says so.

The stand-in server starts with what Sean's cert 5 knows, so "already mapped" works the way the real server will. Its own folder and its TEST icon keep it apart from the Beta and the real app.

**1. Install.** Open **EchoJay Wizard Test.dmg**, drag **EchoJay Wizard Test** onto **Applications**, and close the window.

**2. Allow it to open (once).** In Terminal, paste this and press Return. Nothing appearing means it worked:

```
xattr -dr com.apple.quarantine "/Applications/EchoJay Wizard Test.app"
```

If it says "Operation not permitted": move the app to the Bin, install it again, and do this step before opening it.

**3. What to try**
- **Sign in.** Any email and any password work; they only name a pretend account. Then the consent screen, then the scan.
- **A small set first.** The default needs **25** new plugins before the free month applies (the server's rule).
  - Below that, you see "Thanks - your plugins are already mapped" with **Contribute anyway (no reward)**.
  - To test the reward on a few plugins, lower `qualify_minimum` in the settings file below (to 2, say), save it, and open the wizard again.
- **Pause and reopen.** On the Measuring screen, press **Pause - carry on later**, or close the window and choose Pause. Then quit, open the wizard again, and it carries on the same run.
- **Send what's done.** Press **Send what's done (no free month)** part-way. The results go to the stand-in server with no code, and the account and this Mac have used their go.
- **Finish.** Let a small set run to the end. If the whole run finished and enough results passed, the stand-in server issues an **EJ-FREE-...** code.
- **Once only.** Opening it again after a send or a finish shows the reward or "You've already taken part".
- **Reset test** (bottom-left of the first screen) clears the pretend accounts, the Mac's record, codes, runs, uploads and the app's state, so you can test once-only and the reward again. It **keeps your settings file**.

**4. Where to look**

Everything lives in `~/Library/Application Support/EchoJay Wizard Test/`. In Finder, press ⌘⇧G and paste that path.

| File | What it is |
|---|---|
| `server_stub/config.json` | **The server's settings - edit me.** `qualify_minimum` (default 25), `once_only` (default true). |
| `server_stub/uploads/` | Every upload as it arrived (`direct_*.zip`) and its receipt (`*.receipt.json`): kind (checkpoint / sent_early / finished), accepted count, and whether it earned a code and why not. |
| `server_stub/runs/<run_id>.json` | The merged run: each plugin's latest state and whether it was accepted. |
| `server_stub/participation.json` | The accounts and Macs that have taken part (once-only). |
| `server_stub/promo_ledger.json`, `server_stub/outbox/` | The codes issued, and the "email" each would have sent. |
| `server_stub/db/` | What the server "already has" (the cert 5 snapshot). |
| `app.log`, `engine/wizard.log` | What the app and the measuring engine did, step by step. |
| `outbox/` | Uploads waiting while "offline" (empty unless a send failed). |

No times are shown on screen. The app works out how long a run will take and writes that to `app.log`, which is the place to look if you want it.
