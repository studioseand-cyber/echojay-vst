# The latency LOG build (round 49, 5 Sep 2026) - a debug build, not the one you work in

What it is: EchoJay V2 (AU) built with `EJ_LATENCY_LOG=1`. It sounds and
behaves exactly like the installed build, and additionally writes

    ~/Library/Logs/EchoJay/latency.log

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
   times, from the same marker and from different ones. Also once from the
   top after a locate.
4. Quit Logic.
5. Double-click `restore_working_build.command` (puts the normal build back).
6. Send `~/Library/Logs/EchoJay/latency.log` (the whole file).

The log build is identified by its UUID (below); the working build is the
one `tools/install_local.sh build-release` installs.
