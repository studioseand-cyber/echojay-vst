# 2026-10-17c — the §25 fix, shipped by the first QUICK build

## Installed

**Yes, over 17b, at 21:56.** The V2 changed in both formats; the Link binary did not change at
all, and that is correct rather than a failed copy — the Link target compiles none of the files
this fix touched (`CMakeLists.txt:547` lists its sources; `EJAgent*`, `PluginEditor` and
`EchoJayAPI` are not among them).

    EchoJay V2.component    9CD06900-DC9F-3A6D-A8DA-7D44C3079E8A   (17b was 602E2FA8)
    EchoJay V2.vst3         A15250EF-3FCD-3CCD-9711-871CD1B0168F   (17b was E9DD38DE)
    EchoJay Link.component  3DA1BAB8-0B7E-3031-A6D2-1636EB177674   (unchanged)
    EchoJay Link.vst3       B3A9F0DF-2A31-32B6-BC1B-13DA3370925C   (unchanged)

The binaries were compiled at `abff3616`; `git diff abff3616 HEAD -- Source/ CMakeLists.txt` is
empty and nothing in `Source/` is uncommitted, so what is in `~/Library` is HEAD's source tree.
The newest file in `Source/` is 21:30 and the link finished 21:36, so nothing was edited into the
build. AUHostingService was killed twice after the copy.

**Logic was closed for it, and needs to be closed for ten seconds for each QUICK install from
now on** — `tools/quick.sh` refuses to install while Logic Pro is running rather than writing
under a live host.

## The real times, which is what you asked for

| | what it did | real time |
|---|---|---|
| QUICK run 1 | cold-ish incremental build of the four bundles after a header change, then the S4 guards | **build 6m 02s, guards 0m 53s** |
| QUICK run 2 | nothing to rebuild, guards again, install, UUID read-back | **0m 10s end to end** |

Target was 15 minutes. The honest reading of those two numbers: when a header changes, QUICK is
about seven minutes and all of it is the compiler; when only a `.cpp` changes it is well under a
minute. Neither run included the matrix rows — see the limit below.

## What the fix does

1. **`look(inventory)` is page-scoped before the gate.** The won't-load entries are folded into
   the page (at most 12, each `why` capped at 72 characters) together with `notLoadableTotal`,
   and only then does the single `fitUnder8K` run. Previously the full list was appended after
   the gate, which is how a result that had passed the size check went out at 58 KB.
2. **The client compacts before the wire.** An oversized result goes through
   `compactResult` and, if it is still over, the step reports `too_large_to_send` rather than
   posting bytes the server will refuse.
3. **Retry and the next message never resend a refused result.** `retry()` refuses
   `result_too_large` / `too_large_to_send` / `invalid_result` and clears the stored body, and
   `start()` clears it too. That is the bug in your screenshot where Retry and a new message both
   failed with the same `toolu_01VtSX…`.
4. **Stops are in plain words.** `plainStopLine()` translates the stop code; raw contract text
   and tool ids stay in the log.
5. **A visible marker for which server you are on**, bottom-right in the editor, silent on
   production.

Guards covering all of that: `SZ1` (the inherited 8 KB gate: 58730 → 7276 bytes), `SL1`–`SL4`
(plain stop lines, both directions), `RC1`/`RC1b`/`RC2` (no resend), `A5`/`A5c`. 124 ok.

## Which server your Logic is on

Production, not B's local server. The line that proves it is in
`~/Library/EchoJay/logs/echojay-2.log` at 20:45:13:

    EJNet: base=https://www.echojay.ai source=built-in devTransport=off bypass=absent

`~/Library/EchoJay/dev_base_url.txt` does not exist on this Mac. 17b does read that file when it
is there — the `source=` field in that same line is the proof either way: `source=file` means the
override was used, `source=built-in` means it was not. With this build you no longer need the log:
if the marker bottom-right is blank you are on production.

## The server side, as it stands tonight

B's §69 fix is in: `b9ea4f6` on `echojay-saas` main, **deployed to production at 21:37** (its own
`DEPLOYS.md` row), and both local servers were restarted onto it at 21:29 and 21:33 — so the local
tree and production are the same server code for this. Server-side, an oversized result is now
trimmed and the step goes on, and the `result_too_large` frame is gone.

That means your retest has both halves: the plugin no longer builds an oversized inventory answer,
and if anything else ever does, the server trims it instead of killing the run. The two fixes are
independent on purpose — either one alone would have stopped the dead run you saw.

## If you do want your Logic on B's local server

B's D29 shim is up on **3215** (it answers `/local/health` now), and it seeds a dev record for a
production-signed token, so your normal sign-in works against it. To switch:

    echo "http://127.0.0.1:3215" > ~/Library/EchoJay/dev_base_url.txt   # on
    rm ~/Library/EchoJay/dev_base_url.txt                              # off

**Then restart Logic.** The override is read once per process, on the first request, so a file
written while Logic is already up has no effect on that session — and that is the one way this
switch can look broken when it is not. The proof either way is the same line in the log, and now
also the marker bottom-right: `source=file` and the URL on screen means you are on 3215,
blank means production.

## What this tier did not test, said plainly

**This is not a gate.** QUICK ran the S4 guards and nothing else. The matrix rows — including
`RP8K`, the new several-hundred-plugin row seeded from your own machine — were not part of either
install run; that row is running now, and the full gate on this tree is the overnight job. Until
the full gate is green there is no `ship_2026-10-17c` folder and no `GREEN.txt`.

**To go back to 17b**, reinstall from `ship_2026-10-17b/` — all four bundles are in it with their
UUIDs recorded in its `INSTALLED.txt`.
