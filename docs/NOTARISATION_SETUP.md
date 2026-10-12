# Notarisation: where it stands and the one-time fix (12 Oct 2026)

## What was checked on this Mac

- **The keychain profile EXISTS.** `RELEASE.md` names it `EchoJayNotarize` (team 8BT5F9B887). Apple accepts its credentials: an unknown profile or a wrong password would fail differently.
- **Apple REFUSES to notarise.** `xcrun notarytool history --keychain-profile EchoJayNotarize` returns:
  ```
  Error: HTTP status code: 403. A required agreement is missing or has expired. This request requires an in-effect agreement
  that has not been signed or has expired. Ensure your team has signed the necessary legal agreements and that they are not expired.
  ```
  So the blocker is the developer account, not this Mac.
- Per Kathy's instruction, **nothing was submitted and no credentials were created or changed.** Notarise + staple was **not** added to the cut, because it cannot be run or confirmed until this is fixed.

## The one-time fix (Sean's team)

1. **The Account Holder** of team 8BT5F9B887 signs in at https://developer.apple.com/account and accepts the updated **Apple Developer Program License Agreement**. Only the Account Holder can accept it. The Developer ID certificate's name, "Sean Donoghue", suggests that is Sean, but check under Membership. If the banner also mentions App Store Connect agreements, accept those at https://appstoreconnect.apple.com → Business.
2. On this Mac, check:
   ```
   xcrun notarytool history --keychain-profile EchoJayNotarize
   ```
   It should print a submission list or "No submission history". Any HTTP error means it is not fixed yet.

### Only if the profile ever has to be recreated

Choose one of the two. Neither is needed today.
- **A. Apple ID + app-specific password.**
  - The Apple ID must be a member of team 8BT5F9B887 (Account Holder, Admin or Developer role).
  - Its owner creates the password at https://account.apple.com → Sign-In and Security → App-Specific Passwords.
  - Then:
  ```
  xcrun notarytool store-credentials EchoJayNotarize --apple-id <apple-id-email> --team-id 8BT5F9B887 --password <app-specific-password>
  ```
- **B. App Store Connect API key.** This is better for a build machine: not tied to one person's password, and revocable.
  - The Account Holder or an Admin creates a **Team Key** at https://appstoreconnect.apple.com → Users and Access → Integrations → App Store Connect API, with the **Developer** role.
  - Download the `.p8` (it can be downloaded once only) and note the Key ID and Issuer ID.
  - Then:
  ```
  xcrun notarytool store-credentials EchoJayNotarize --key ~/private_keys/AuthKey_<KEYID>.p8 --key-id <KEYID> --issuer <ISSUER-UUID>
  ```

## What the cut will do once `history` works

This is not built yet; it is the next step after the check above passes. The steps are the same for `ejmap.app` and `EchoJay Wizard.app`.
1. **Sign.**
   - `ejmap.app` is already signed with Developer ID, hardened runtime and a timestamp (`package_app.sh`), with the probe inside, signed with its entitlements.
   - **The wizard app is not.** `assemble.sh` signs it and its engine ad hoc. It needs the same Developer ID signing, inside out: probe, engine, then the app.
2. **Submit and wait:**
   ```
   ditto -c -k --keepParent "<app>" "<app>.zip"
   xcrun notarytool submit "<app>.zip" --keychain-profile EchoJayNotarize --wait
   ```
   If it fails: `xcrun notarytool log <id> --keychain-profile EchoJayNotarize`.
3. **Staple:** `xcrun stapler staple "<app>"`, then re-zip for sending.
4. **Confirm on a quarantined copy.** Copy the app, then flag every file as an email would:
   ```
   find <copy> -exec xattr -w com.apple.quarantine "0083;$(printf %x $(date +%s));Mail;" {} \;
   ```
   Then run `spctl -a -vvv -t exec "<copy>"`. It must say `accepted` and `source=Notarized Developer ID`. `xcrun stapler validate "<copy>"` must say `The validate action worked!`.

After that, Sean's builds open without the quarantine step. The runbook's Step 0 stays until a notarised build has reached him.

## What un-notarised builds do today (reproduced on this Mac, 12 Oct)

| copy of the 97de4b8e package | result |
|---|---|
| every file flagged, as an email unzip leaves it | macOS holds the app at launch. syspolicyd logged `Prompt shown (6, 0), waiting for response` for `ai.echojay.ejmap` (team 8BT5F9B887). The process never ran; it was killed after 5 min. The prompt is the "can't be verified" window, which ends the app when dismissed: Sean's "killed at launch" |
| only the probe flagged | The preflight refuses: `quarantine : FLAGGED - the probe still carries the download's quarantine flag ...`, the FIX line, exit 3 |
| a fresh flagged copy, then `xattr -dr` before any launch | 12 flags → 0. The copy then launches, and the preflight says `quarantine : ok - no quarantine flag (the app, its executable, the probe)`, exit 0 |
| the copy macOS had already prompted for, then `xattr -dr` | **`Operation not permitted`** on every file. That copy had gained `com.apple.macl`. Hence the runbook's "delete, unzip again, clear BEFORE opening" |
