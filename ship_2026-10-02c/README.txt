ship_2026-10-02c — INSTALL THIS. Built 12:53, 2 Oct, from feat/comp-profiles @ a85786e.

  EchoJay V2.component     67M   9FF3DAA7-F996-336B-8E68-11A4049FA1E6
  EchoJay Link.component   49M   24A5EB79-CD65-3F25-850A-9AC336394766

Both UUIDs were read back from the placed bundles and match the build tree. Unsigned.

INSTALL
  P=/Users/SeanD/echojay-vst/ship_2026-10-02c
  rsync -a --delete "$P/EchoJay V2.component"   ~/Library/Audio/Plug-Ins/Components/
  rsync -a --delete "$P/EchoJay Link.component" ~/Library/Audio/Plug-Ins/Components/
  killall -9 AUHostingService 2>/dev/null; true
Verify by the UUIDs above, not by file dates. Without the AUHostingService kill the host keeps serving the
previous binary and the log will describe code you are not running.

WHAT IS FIXED FROM 02b (your 11:35 findings 1 and 2)

1. The pre-flight probe no longer loads a PACE-wrapped bundle, so building a chain with the Tube-Tech CL 1B
   cannot raise the macOS "lower security settings" prompt. The CL 1B's bundle carries __Pace_Eden.bundle; the
   probe now refuses it before instantiating and exits 3, which the host reads as "not evidence - in-host create
   proceeds". The plugin still loads in Logic exactly as it did before the probe existed.
   TRADE, stated: a PACE-wrapped plugin is no longer pre-flighted, so it loses hang protection. Signing the probe
   with your Developer ID restores it and is the other half of the fix - NOT done, it needs your identity.
   With a signed probe, set EJ_PROBE_ALLOW_PACE=1 to re-enable probing.

2. A compressor whose hold made up more than 6 dB now says so:
     "Set as dialled, taking about 12 dB off: too much, check the threshold
      (my output trim is at its +12 dB ceiling, so it may be taking off more than this). Output +12.0 dB."
   3 dB of make-up still reports a plain "level matched". A non-dynamics slot is never accused of compressing.

NOT in this build
  3. The stop/start-Logic-to-hear-processed-audio issue is NOT fixed. Nothing in the tree addresses it: there is
     no setLatencySamples in PluginProcessor.cpp or LinkProcessor.cpp at all, and letter (h) - the latency budget
     - was queued and never built. Send the log and it is next.
  4. AVOX SYBIL's silent drop is DIAGNOSED but NOT fixed, because the fix changes which plugins get withheld and
     that is your call. Cause: its real AU identifier is "AudioUnit:Effects/aufx,AnVD,VST " - 32 characters,
     because Antares' manufacturer OSType is literally 'VST ' with a trailing space. Your chain_blacklist.txt
     line is the same string TRIMMED to 31 characters. ChainHost::isBlacklisted is an exact match, so it returns
     false: SYBIL is offered in the feed and not refused at load, then fails for real. ChainHost.cpp:7397 trims
     the path on read, and the file was written trimmed too. Any plugin whose OSType ends in a space is affected,
     not just this one.

FLAGS (unchanged)
  touch ~/Library/EchoJay/comp_profiles_on.txt            # measured compressor profiles ON (default OFF)
  echo "https://..." > ~/Library/EchoJay/dev_base_url.txt # point at a preview server (default: production)
