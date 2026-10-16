# What the rig can do with Links today, and what I am changing

**Written for Sean, 16 Oct 2026, before the scenario runner.** Answering his question in order: what exists
today, what each matrix Link target needs, and what will still not be reproducible when I am done.

---

## 1. Today, in plain English

**One process, four plugin instances.** `tools/host_rig/host_rig` is a single host process that loads the
**built bundles** and keeps them running:

| Instance | What it is | Named |
|---|---|---|
| V2 (AU) | the main plugin, the one with the chat and the agent | "Mix Bus (V2)" |
| Link 1 (AU) | a real EchoJay Link instance | "Kick (Link)" |
| Link 2 (AU) | a second real Link instance | "Vocal (Link)" |
| V2 (VST3) | the same plugin on the VST3 road, for window-close rows only | "Mix Bus (V2, VST3)" |

So **two Links already exist** and they are real: they register themselves in the Link registry, publish
their own rack sidecars, take chain-edit commands over the file channel and answer them, and pass audio.
Every V2 to Link scenario the rig runs today goes over the product's real channel - the same files and the
same shared-memory ring that Logic uses - because that channel is file and shm based and does not care
whether the two ends are in one process.

**What is genuinely exercised today**

- A Link appearing in the registry and being selected as the viewed rack (`selectLink`).
- A Link being made the chat's working channel (`workOn`), and the agent's target following it.
- The agent adding to a Link over the chain-cmd / chain-ack road, with the ack waited for.
- A Link's own sidecar being read back by the V2 (slot count, names, the `landed` figures).
- A real third-party plugin being added **to a Link** by the agent (row T1, MTransient until this morning,
  Renaissance Axx from now on).
- The V2's editor being destroyed and rebuilt (on the VST3 road) while a turn is in flight.

**What is NOT exercised today, and this is the honest list**

1. **Each Link in its own process.** All four instances share one process, one message thread and one audio
   pump. The channel crossing is real; the *address space* crossing is not. Nothing has ever proved that the
   Link code path works when the V2 cannot see its memory, and three of this week's bugs lived exactly
   there (the maps-wanted channel, the sidecar `landed` field, the stale mirror).
2. **A Link hosting real third-party plugins before a test starts.** Today a Link's rack is empty until the
   agent puts something in it, and what it puts in is usually an EchoJay device. Matrix targets T2 to T4
   want Links that are already holding Renaissance Axx and Vertigo VSM-3.
3. **The rack-selection lock (T3).** The rig never opens two V2 editors, so "this rack is selected on X -
   deselect there to edit here" has never been produced. The code is there and so is the refusal text; no
   test has ever seen it.
4. **A Link's own window.** The rig opens the V2's editor only. A Link's editor has never been opened in a
   test, so "Link window open versus closed" is untested, including the case where the Link's own window is
   what republishes its sidecar.
5. **The V2's editor closing while a Link change lands.** Item 0b's rows close the V2 mid-run, but never
   with a Link ack in flight, which is the case where a change lands into a window that no longer exists.

---

## 2. What I am building (this round)

### 2.1 Links in their own processes - `tools/link_host`

A small host whose only job is to be one Link: it loads the Link bundle, prepares it, pumps audio in a
loop, and takes its own file commands (open its editor, close it, build a rack of named plugins, report
what it holds). It runs with the **same `ECHOJAY_STATE_HOME`** as the rig, which is how the two processes
find each other - exactly as two Logic AU instances do through the shared directory.

The runner launches two of them (Kick, Vocal), waits for them in the registry, and runs every T2 to T4
scenario against them. `EJ_RIG_LINK_PROCESS=0` keeps the old in-process Links for the audio and meter
scenarios (see the timing note below).

**Why this matters more than it sounds:** a separate process is the only way to prove the V2 is not
quietly reading a Link's memory. Every fault I fixed this week on that channel would have been caught a
day earlier by it.

### 2.2 What timing difference remains, stated plainly

Logic hosts AUs out of process (AUHostingService), but it still renders them from **one audio graph**: the
host asks the V2 for a block and asks the Link for a block, in order, on the same clock. Two separate rig
processes each pump their own blocks on their own clock, so:

- **What matches Logic:** address-space isolation, real IPC, real file and shm contention, the plugin
  having to survive the other end being slow or gone, and the ordering of *events* through the channel.
- **What does not match Logic:** the two ends are not block-locked. In Logic, V2 block N and Link block N
  belong to the same render pass; in two rig processes they drift, so anything that depends on the two
  racks seeing the same audio instant (level matching across the pair, a meter comparison between V2 and
  Link, sample-accurate latency reporting) is **not** reproduced faithfully.

So the matrix runs **both ways**: the channel, lock, window and view scenarios on the two-process setup,
and the audio, meter and level scenarios in process, where the block interleave is exactly Logic's. Each
scenario in the report says which setup it ran on. Anything that needs the real thing - Logic's own
AUHostingService lifecycle, its editor teardown on the AU road, its audio-device changes - goes on the
smoke-test list rather than being faked here.

### 2.3 The other three gaps

- **T3, the rack-selection lock.** The second Link process holds the lock on its own rack (it selects it),
  and the V2 then tries to edit it: the expected result is the refusal sentence, not a silent no-op. The
  runner asserts the sentence the user would read.
- **Link window open versus closed.** `link_host` opens and closes its own editor on command, so each
  Link scenario runs twice where it matters, and a republish that only happens with a window open is
  visible as a difference rather than as an intermittent failure.
- **V2 closed while a Link change lands.** The runner sends a Link edit, closes the V2's editor inside the
  ack window, reopens it, and asserts the change is on screen once, in order, with no duplicate.

---

## 3. What stays on the Logic smoke test

Not because it is hard here, but because the rig genuinely cannot be the thing it is imitating:

1. Logic destroying an AU's editor on its own schedule (the AU road cannot close a window in process).
2. AUHostingService restarts and audio-device changes mid-run.
3. Logic's own plugin scan and its licence dialogs (the standing rule: the rig never scans).
4. Anything about how it looks and sounds to a person: whether a hint reads clearly, whether a compressor
   setting sounds right, whether a figure on the meter matches what the eye sees.
