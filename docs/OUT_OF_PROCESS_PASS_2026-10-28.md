# The out-of-process pass, run properly for the first time (28 Oct 2026)

Round 36 ran the rig in **both** configurations against one archive (`19070a63`, V2 `7A103FED`):

| pass | configuration | verdict |
|---|---|---|
| 1 | `EJ_RIG_NO_LINK_HOST=1` — the V2 and both Links in ONE process, **which is what Logic and Pro Tools do** | **GREEN**, 0 failed / 212 passed / 14 skipped |
| 2 | two `link_host` processes, each hosting a real Link (the matrix's T2-T4) | **RED**, 10 failed / 203 passed / 14 skipped |

Pass 2 is the one that had never run on this machine until 27 Oct (`build_and_run.sh` records why:
the quick tree's archive is arm64-only and `link_host` needs the caller's slices). It built here,
launched two Links — `659385983e` "Kick" with two plugins, `ad8b8c313c` "Vocal" with one — and turned
up ten reds. **Not one of them is a regression**: they are what nobody had looked at.

## The ten, and they are three causes

### 1. Seven are a missing configuration check in the RH rows — fixed

`RH1`–`RH6` and `RH9` fail, and the failures are preconditions, not products: RH1's is *"did every
subject load on the Link's rack"*, RH6's is *"could not identify which of the rig's tracks is that
Link"*. The rig's own Tracks **are** the in-process Links; an out-of-process Link is not one of them,
and `build_and_run.sh` already states the rule these rows are about: *"the real editor is only ever
offered for a Link in OUR process"*.

**The LV rows already do this correctly** — the log has it in their own words:

```
(LV) 659385983e is connected but is not in this process' directory - using dd5152116a
```

So the fix is the pattern that already exists one family over: the RH block skips loudly when its
subject (`lvUid`) is one of `gLaunchedLinkUids`. Seven false reds become one honest skip that names
the configuration and says where those rows did run. Written and queued; the same file's rule for the
other direction is *"the rows that cannot run in it skip loudly"*, and nine false reds in the pass
that proves the T2 scenarios is the mirror of the fault that rule exists to prevent.

### 2. Two are the chain command never being acked by a Link in another process

`SCEN A4` and `SCEN A4b` both end *"the plugin in the other process now shows NOTHING READ BACK"*, and
the cause is in the log one line up:

```
EJAgent: step tc_tp FAILED code=not_applied "no chain-ack-659385983e.json with seq 1793192895 appeared"
```

So the write went out and **no ack came back**. A4b's own sentence names why it matters: *"this is the
channel with no shared memory to fall back on"*. Whether `link_host` services the Link's command loop
at all is the first question, and it is a question about the harness before it is one about the
product — the ack road is the Link's own, and it is proven daily by the in-process passes.

**Owed, not guessed.** The next step is the instrument, not a theory: have `link_host` say whether it
polls for commands and what it saw, because "the command never arrived", "it arrived and was refused"
and "it was applied and the ack was not written" are three different faults and this log cannot tell
them apart.

### 3. One is a liveness refusal, and it is the registry finding again

`SCEN G1` fails with the V2's own words:

> *"THE V2 REFUSED TO AIM AT IT (that uid is not live/that uid is not live) while its process IS up
> holding a rack — so the strip was never going to mirror anything, and that refusal is the fault, not
> the empty strip."*

`linkUidLive` is `uid in getLinkDisplayList() && info.connected`, and that list is written only by
`refreshLinkRegistry()` — whose callers, as round 34's instrument established, **are tab switches and
apply paths, not a clock**. That finding was raised for Sean's ruling then; this is what it costs: the
same run saw `659385983e` as live at 13:01 (`EJSlotMeter: 659385983e - the cards are LIVE`) and
refused to aim at it at 13:12. A list that only refreshes when something else happens to touch it is
a list that is right at the moment it is written and unfalsifiable afterwards.

## What this means for the install

Sean's condition is *"every live-view, control and settings row green, none skipped"*, and those rows
are about the configuration his hosts give: **one process**. That pass is green, with nothing skipped
in those families, and M1 — the last live-view skip — now runs and passes. So the install stands on
pass 1 plus S4/S5.

Pass 2's reds are recorded here rather than folded into that decision, because mixing them would mean
either blocking an install on seven false reds or quietly ignoring three real findings.
