# The ValhallaDelay crash: whose it is, and what we own

27 October 2026. Sean's question, in his words: *"find whether it's our teardown order (destroying the Link
or its hosted plugin while the plugin's own thread runs) or genuinely Valhalla's, from the two crash reports
and a rig repro; fix if ours, with a guard."*

**It is not ours, and it is not on track delete either.** It happens on the way out of the process, while
static destructors run, and the only thing of ours in the story is the door our own test host leaves by.
That door is now `_Exit`, with a probe that proves both directions.

## What the reports actually say

There are not two reports, there are **eleven**, one identity, 26 Oct 23:50:03 through 27 Oct 01:39:26:

    EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x0
    faulting thread named "JUCE Timer":
        pthread_mutex_lock
        ValhallaDelay + 0x1e3a38
        ValhallaDelay + 0x1e5ac8      (0x1e5a54 in one of them - the same function, another call site)
        ValhallaDelay + 0x1c2874
        _pthread_start
        thread_start

The frames above are the plugin's own statically linked JUCE: a thread it started itself, locking a mutex
whose address is zero. No EchoJay frame appears in it.

**And the main thread, in every one of the eleven:**

    start -> dyld4::LibSystemHelpersWrapper::exit(int) -> exit -> __cxa_finalize_ranges
          -> wvWavesV15_0_70_71::ServerClient::WCBIClientWithWLSThread::~WCBIClientWithWLSThread
          -> stop_thread() -> std::thread::join -> _pthread_join

So the process had already left `main` and was running static destructors. dyld runs an image's terminators
in reverse load order, so ValhallaDelay's statics - it loaded later - were already destroyed, while
WaveShell 15.0's destructor held the process open joining a reporting thread. ValhallaDelay's timer thread
went round its loop once more in that window and found its lock gone.

The rig's own log puts the verdict at 01:39:26.392 and the crash capture at 01:39:26.602 - **after every
row**. The overnight report called this "deleting a track with ValhallaDelay hosted takes the process down".
That attribution was wrong and this corrects it.

## Why our teardown cannot be the cause

`ChainHost::~ChainHost` does not destroy a hosted third-party instance at all. It says so in the log, in
that same pass:

    ChainHost: teardown KEPT 3 hosted plugin instance(s) alive on purpose
    [ValhallaDelay, TDR SlickEQ M, AUMatrixReverb] - disposing a third-party AU inside AP_Close is what
    aborts on the Softube CL 1B.

That is the ruled answer to the 5 Oct AP_Close abort, and `tools/teardown_dispose_guard` holds both halves
of it. The instance, and therefore every thread it owns, is alive **by design** when the process leaves.
There is no ordering of ours to change: we never destroy it, and we cannot stop another image's threads.

## The repro, in a process with nothing of ours in it

`tools/hosted_exit_probe` instantiates one AU by identifier, opens and closes **its own editor** three times
in a plain `juce::DocumentWindow`, leaves the instance alive, and then leaves the process by one of two
doors. No ChainHost, no EchoJayProcessor, no lease, no re-homing, no live view.

    tools/hosted_exit_probe/build_and_run.sh 3

| subject | returning from main | `_Exit` |
|---|---|---|
| ValhallaDelay | **3 of 3 died** | 3 of 3 clean |
| TDR SlickEQ M | 0 of 3 died | 3 of 3 clean |
| FabFilter Pro-C 3 | 0 of 3 died | 3 of 3 clean |
| AUMatrixReverb | 0 of 3 died | 3 of 3 clean |
| AUDelay | 0 of 3 died | 3 of 3 clean |

The probe's crash report is the same identity as the eleven, down to the thread name and the three
ValhallaDelay offsets, with the main thread inside `exit -> __cxa_finalize_ranges -> <a slow destructor>`.
ValhallaDelay is the only one of the five that does this.

Two things the probe had to learn before it reproduced anything, both recorded in its source:

1. **The editor matters.** Without one, neither door died: a plugin's timer thread only exists once
   something in it starts a `juce::Timer`, and its editor is the usual thing that does.
2. **The lingering destructor matters.** A process that leaves instantly never opens the window. The probe
   registers a static destructor that takes 1500 ms (`EJ_PROBE_EXIT_LINGER_MS`), which is what WaveShell's
   thread join was doing in the rig.

It also does **not** render audio, and that is deliberate: handing a music effect a plain stereo buffer is
not a layout. Pro-C 3 walked off the end of one inside `AudioUnitRender` on the first attempt - the same
place AMEK's Mastering Compressor took `au_instantiate_probe`'s render step down on 18 Sep. "JUCE pulls
every declared bus" is the rule, and giving a plugin a correct layout is ChainHost's job, not a door
probe's.

## What we fixed

`tools/host_rig/host_rig.mm` now ends with `std::fflush(nullptr); std::_Exit(...)` instead of returning from
`main`, with the reasoning above written at that line. A test host has nothing to do in static destruction:
the verdict is printed, the matrix report is written, and the OS reclaims the rest.
`tools/au_instantiate_probe` has left by that door since 21 Sep for the same reason.

The guard is `tools/hosted_exit_probe/build_and_run.sh`, and it is RED in both of the ways that matter: if
the known-bad door stops dying it says *"THIS RUN IS NO EVIDENCE, not a pass"*, and if the `_Exit` door is
not clean on any subject it fails outright.

## What is left, and it is Valhalla's

In a host that calls `exit()` with ValhallaDelay loaded - which is what an application quitting does - this
race exists whatever we do. It is worth knowing that our leak rule lengthens the window: a plugin the user
puts on a track directly is disposed by the host when the track goes, while a copy we host is kept until the
process ends. The alternative is the AP_Close abort, which is worse and is already ruled on. Nothing here is
a reason to change it, and a crash during the host's own exit, after everything is saved, in the plugin's own
thread, is not ours to chase.

## One thing found on the way: MCompressor is PACE-wrapped

`/Library/Audio/Plug-Ins/Components/MCompressor.component/Contents/__Pace_Eden.bundle` exists. MCompressor
was named overnight as one of the three screened third-party subjects, and the comment claimed EJPaceCheck
would refuse it "if that ever changes". It could not: that list names **identifiers**, and the check was only
ever applied to the bundle paths in the catalogue set beside it. So the rig loaded a licence-bound plugin in
its own process overnight, against Sean's standing rule.

Both halves are fixed: every identifier the rig enumerates now goes through `echojay::refuseIfPaceWrapped`
and is named in the refusal line, and **FabFilter Pro-C 3** takes the third maker's place (large custom
interface, licence file, no server; it was already screened clean and already in the rig's catalogue set).
The third-party subject set is now ValhallaDelay (Valhalla DSP), TDR SlickEQ M (Tokyo Dawn, JUCE-built) and
Pro-C 3 (FabFilter). MeldaProduction is no longer represented: MTransient is not installed on this Mac.
