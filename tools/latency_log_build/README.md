# The latency LOG build (round 49, 5 Sep 2026) - a debug build, not the one you work in

What it is: EchoJay V2 (AU) built with `EJ_LATENCY_LOG=1`. It sounds and
behaves exactly like the installed build, and additionally writes

    /Users/SeanD/echojay-vst/latency-logs/latency.log

(a folder the cloud side reads directly - nothing to locate or send; the
install script moves any previous log aside as latency.previous.log).

with a timestamp on every line for:
  - every latency report to the host (`setLatencySamples`): old value, new
    value, which code reported it, and whether it changed;
  - every chain-host graph rebuild: the debounce being armed, firing, what
    changed (which slot, old -> new), and every rebuildGraph;
  - the first 50 audio blocks after playback starts: block index, the
    latency the host has been told, the chain's total, whether a rebuild is
    pending, the transport position; plus PLAY/STOP edges, prepareToPlay on
    every layer, and the host's reset() calls.

## How to run it (five minutes)
1. Quit Logic.
2. Double-click `install_log_build.command` (it copies the log build over
   `~/Library/Audio/Plug-Ins/Components/EchoJay V2.component`, clears the AU
   cache and kills the AU hosting service).
3. Open the session, press PLAY, let it run ~3 s, STOP. Do that four or five
   times, including once after jumping to a marker.
4. Quit Logic.
5. Double-click `restore_working_build.command` (puts the normal build back).
6. Nothing to send: the log is in the repo folder and is read from there.

The log build is identified by its UUID (below); the working build is the
one `tools/install_local.sh build-release` installs.

## Identity (5 Sep 2026)
  LOG build     AU arm64 93D76F1C-A7C8-3ECA-A260-82E348067B9C   (build-latencylog, EJ_LATENCY_LOG=ON, log -> echojay-vst/latency-logs/latency.log)
  WORKING build AU arm64 230EA288-CE5E-3B68-A493-A081951C7D06   (build-release, what is installed)
Deliverable (connected folder): /Users/SeanD/echojay-vst/latency-logs/build/


## Round 53 - the capture that closes the item (L1, L2, L5 of the ruling)
The log build here is the FIXED build (queue-to-next-stop). Logic quit,
`install_log_build.command`, then, with the EchoJay window CLOSED the whole
time and NO EchoJay Link in the session yet:
  1. press PLAY; while playing, insert an EchoJay Link on another track;
     keep playing ~30 s; STOP;
  2. PLAY again ~10 s; STOP;
  3. PLAY; while playing, remove that Link; STOP.
Quit Logic, `restore_working_build.command`. Nothing to send.
What the log should show: "borrow budget WANTED ON (pending ...)" during
play 1 and NO setLatencySamples line until its STOP, where ONE line
"COMMITTED ON at PluginProcessor commit at STOPPED block" appears (old 1800
new 18184); play 2 constant at 18184 for all 50 blocks; in play 3 "WANTED
OFF (pending)" and the commit only at its STOP.
  LOG build     AU arm64 6E91E383-2713-3385-A42E-8D903B88C372
  WORKING build AU arm64 7F0618CD-0ED5-3E98-9C4F-3E447E10B72B  (installed)
