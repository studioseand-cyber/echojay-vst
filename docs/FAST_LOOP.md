# The fast loop: one issue at a time (from 18 Oct 2026)

**Read this after every compaction.** It is how Sean, the coordinator, A (plugin) and B (server) work now. It replaced
"full gate, install, deploy, then Sean checks", which took half a day per fix. On the first morning it took a fix from
report to passing in Logic in 10 to 35 minutes.

## The loop
1. **Sean tests in Logic** and reports the FIRST thing that goes wrong (a screenshot), not the whole list.
2. **The coordinator** sends ONE short message to whichever session owns it: A for plugin UI and behaviour, B for
   anything the server decides or says.
3. **The owner fixes only that, with the smallest change that does it.**
   - **A:** `tools/quick.sh "<what changed>" "<guards>" "<rows>"`, which runs an incremental build, only the guards the
     change touches, then installs. That is about 3 minutes for a .cpp change, 7 to 10 with a header. Say "close Logic"
     right before installing; the script refuses to install under a live host.
   - **B:** a sim row red first, then the fix, live on 3215 straight away (the server reloads changed code, so there is
     no Logic restart).
4. **The owner replies with one line:**
   - A: `ready: do X in Logic, expect Y`
   - B: `live: ask X, expect Y`
5. **Sean retests just that**, then moves to the next test.

A and B work in parallel. Each takes one issue at a time and queues the rest in order.

## The rules
- No batching and no extra scope while Sean is waiting. Write-ups come later.
- No rig runs or builds that load plugins while Sean is in Logic. Code and guards only.
- Bigger work (queue items, Kathy's readers) only when nothing from Sean is open.
- **After a compaction, carry straight on.** Re-read this file, finish the item in hand and report it. Never sit idle
  waiting for a prompt. (On 18 Oct, A compacted at 12:35 with a finished build and sat for an hour.)
- **Never hold a finished fix to batch it with the next one.** Install it or make it live, report it, then start the next.
- Never show codes, tool ids, contract text or OS errors in the chat. Plain sentences only, with OS errors under Details.

## The setup (already done on Sean's Mac)
- **Logic talks to B's local server.** The file `~/Library/EchoJay/dev_base_url.txt` contains
  `http://127.0.0.1:3215`, and is read once per Logic launch.
  - The plugin shows `server: 127.0.0.1:3215` bottom right. Blank means production.
  - Back to production: `rm ~/Library/EchoJay/dev_base_url.txt`, then restart Logic.
- **Sign-in on 3215:** press Log in with any email and password. You become the dev test account "EchoJay dev (local
  3215)" on the Studio tier. This only happens on the local server, behind FORGE_DEV_OPEN.
- **Ports:**
  - 3215 is Sean's; it restarts only when a fix lands.
  - 3213 is the harness's (B's live matrix).
  - 3211 is A's rig.
  - The watchdog covers all three.
- **The plugin syncs its plugin list at launch.** After a server change about inventory or identities, restart the
  plugin (quit and reopen Logic).

## What still runs the slow way, and when
- **A's full gate (S1 to S6):** overnight and before any build goes to beta, never before a quick test.
- **Production:** one batch deploy once Logic is green.
  - B pushes, then Sean runs this within 20 minutes, with nothing pushed in between:
    `cd ~/echojay-saas && git stash -u && scripts/deploy-prod.sh && git stash pop`
  - Then B runs its post-deploy checks.
- **Overnight:** one heavy job at a time (A's gate, or Kathy's EJ Map packs). Never two at once.

## Results, 18 Oct morning
| issue | owner | result |
|---|---|---|
| Sign-in failed on 3215 | B | dev sign-in on the local server; passed |
| "Thinking..." didn't show at once on send | A | passed |
| Aiming at a Link didn't switch the rack, banner or chat folder | A | passed |
| "What compressors do I have?" gave an unclear count | B | names plus four whole-inventory numbers, all types; passed |
| "session_expired" | B | the server now saves the session before streaming; passed |
| Only 9 "can set" (matcher, plus Kathy's files) | B | 49 named, 8 can't run; passed |
| kHs won't load: up-front honesty, Try anyway | A/B | passed (small follow-ups queued) |
| Production's profile index listed only 1 profile | B | fixed; goes out in the next batch deploy |

## Queue at the time of writing
- **A:**
  1. One big inventory page (up to 2,000 names) and the can't-run plugins by name and type (FOR_A 76).
  2. Show the closing once, in full (no copy above the ASK label, no cut-off copy), and no bare "ask" on the waiting card.
  3. Item 22: the borrowed dial read waits on BUSY.
  4. The I9c rig run when Sean takes a break.
- **B:**
  1. Read plugin identities from the sync (so Kathy's levels reach all ~1,052 plugins).
  2. The kHs ask: the OS error goes under Details only, and real shapers appear as chips.
  3. Kathy's readers later.
- **Sean:**
  1. Tests: the cl1b typed name, a sentence into a waiting run, new chat mid-run, the editor tab and mirror, the chat list.
  2. Then Kathy's five-compressor Logic check (SEAN_LOGIC_CHECK_5.txt, about 50 minutes).
  3. Tonight: `recheck_gaps_18oct.sh`.
