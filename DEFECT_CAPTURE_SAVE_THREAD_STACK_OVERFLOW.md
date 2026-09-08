# DEFECT (SEVERITY: DEFAULT PATH, every capture > 2 s on an untyped channel since 7 Aug): Pro Tools crashes at the end of a capture - stack overflow on our "EchoJay WAV Save" thread (filed 8 Sep 2026; FIXED in Build A)
Crash report: ~/Library/Logs/DiagnosticReports/Pro Tools-2026-09-08-111901.ips (copy in results_2026-09-06/). Not fixed in this pass.
## 1. The report
    Pro Tools 25.12.1.133, x86_64 under Rosetta (cpuType X86-64, translated=true), launched 11:05:20, crashed 11:19:01
    EXC_BAD_ACCESS (SIGBUS) KERN_PROTECTION_FAILURE at 0x0000000311b08ff8
    faulting thread 214 "EchoJay WAV Save" - OURS, a juce::Thread started by stopCapture(). NOT a realtime audio thread.
    backtrace:
      0 libsystem_pthread.dylib   ? + ?                                   (the stack probe)
      1 EchoJay V2                EchoJayProcessor::stopCapture()::SaveThread::run() + 24
      2 EchoJay V2                juce::Thread::createNativeThread(juce::Thread::Priority)::$_0::__invoke(void*) + 744
      3 libsystem_pthread.dylib   ? + ?
      4 libsystem_pthread.dylib   ? + ?
    vmRegionInfo: 0x311b08ff8 is in --->  Stack Guard  311b08000-311b09000 [4K] ---/rwx ;  Stack 311b09000-311b8b000 [520K]
    thread 214 rsp = 0x311b88f30; rsp - fault = 524,088 bytes (0.50 MB): the fault is the guard page BELOW the thread's stack.
    Loaded images: EchoJay V2 x86_64 7FBB47DE-2227-3B85-BC0C-E1B472111669 (= installed v7 V2, x86_64 slice), EchoJay Link
    x86_64 1000546C-F8E6-33AD-B752-35439E8A3970 (= installed v8 Link, x86_64 slice). Both match what is on disk.
    Main thread was painting (EchoJayEditor::paintStereoPanel) - unrelated. 41 "EchoJay Key Analysis" threads (one per Link
    instance, EedKeyWorker) parked in WaitableEvent::wait - unrelated to the crash.
    The two earlier Pro Tools reports (6 Sep 17:53, 21:52) are a different crash: KERN_INVALID_ADDRESS in ProTools_Red/xpc,
    no EchoJay frame.
## THE CAUSE - a 2 MB object on a 512 KB thread stack; NOT the Link count, NOT the registry
    PluginProcessor.cpp SaveThread::run():   echojay::KeyEngine eng;     (a local)
    sizeof(echojay::KeyEngine) = 2,097,784 bytes (measured; EedKeyEngine.h:239/257: std::array<float, 1<<19> ring_ = 2 MB)
    juce::Thread("EchoJay WAV Save") uses osDefaultStackSize = 0 -> the pthread default, 512 KB (the report shows 520K).
    run()+24 is the prologue reserving the frame; the probe touches the guard page; SIGBUS. Deterministic.
    In the code since 28743f4 (7 Aug 2026, "captures carry their key - offline pass at capture time (5.2)").
    WHEN IT FIRES (PluginProcessor.cpp:3077-3094 + the run() guard): the capture is longer than 2 s AND a key source
    qualifies: a captured Link with placement == 1 (bus) that recorded samples, OR the host channel typed FullMix /
    MasterBus / MusicBus / InstrumentBus. With no qualifying source keySrcIdx stays -2 and the KeyEngine is never built.
    That is why one-Link captures on a vocal survived and a 45-Link capture (some Links placed as bus) did not. The Link
    COUNT is not the variable; the presence of ONE qualifying key source is.
## 2. Bisect by count - not run by me (Pro Tools is Sean's live session). Prediction from the mechanism, to be confirmed:
    crashes at 10, 20, 30 and 45 alike whenever a bus-placed Link (or a bus-typed host) is in the capture and the capture
    exceeds 2 s; never crashes at any count when no key source qualifies. Protocol for Sean: same session, capture > 5 s,
    (a) 10 Links all placed insert/send-return, host channel not a bus type -> expect no crash; (b) add ONE Link placed
    as bus -> expect crash at end of capture. Two captures decide it.
    WORKING CONFIGURATION FOR TODAY (no code): keep every Link's placement at insert or send return (not bus) and the main
    channel type off the four bus types, OR keep captures under 2 s. The 45-Link payload itself is fine (10,474 B measured).
## 3. Bounds audit of the capture path (16 -> 256 slots)
    startCapture (PluginProcessor.cpp:2851)   for (int i = 0; i < kMaxLinkSlots; ++i)      kMaxLinkSlots = kRegMaxSlots = 256   ok
    linkCaptureChannels                        std::vector<std::unique_ptr<LinkCaptureChannel>>   unbounded                   ok
    processBlock capture loop (:914-1156)      walks the live list liveSlotIdx_/liveSlotCount_ (sized kMaxLinkSlots)          ok
    spectrum loops (:1323, :3003, :3011)       for (int i = 0; i < 64; ++i)   = spectrum BINS (std::array<float,64>), not Links ok
    LinkCaptureChannel                         std::array<float,64> spectrum, std::vector tmpBuf                              ok
    No 16 / 32 / 40 bound survives on the capture path. 45 Links cross nothing here.
## 4. Work on the audio thread - three findings, none the crash
    (a) stopCapture() IS CALLED FROM processBlock: PluginProcessor.cpp:688-689
        if (wasTransportPlaying && !playing && captureState.load() == CaptureState::Capturing) stopCapture();
        stopCapture builds a CaptureSnapshot (juce::String ids/names), moves the Link channel vector, and does
        saveThread = std::make_unique<SaveThread>(...); saveThread->startThread();   (:3237-3240)  - heap allocation and a
        pthread CREATE on the audio thread, every time a capture ends by transport stop. Not the crash (the crash is inside
        the new thread), but a realtime violation that scales with nothing and bites at random.
    (b) WaveformRecorder::processBlock (WaveformRecorder.cpp:93-97) calls ensureCapacity -> audioBuffer.setSize(2, n, true,
        true, false): a heap reallocation WITH COPY of the whole recording, on the audio thread, every kGrowChunkSamples =
        441,000 samples (~10 s). Each LinkCaptureChannel owns one (accumulateLinkChannel :110 -> lcc.waveformRecorder.
        processBlock). With 45 Links that is 46 growing buffers reallocated inside processBlock every ~10 s of capture: a
        dropout risk that DOES scale with the count, but not a crash.
    (c) linkCaptureSpinLock.tryEnter() (:915) is non-blocking; no file I/O and no sidecar read happens in the capture path
        of processBlock (sidecars are read on the 1 Hz timer).
## 5. Synchrony and timeouts
    Capture collection is synchronous and local: stopCapture hands every Link channel to ONE save thread which writes the
    WAVs sequentially to local disk; no network, no per-instance timeout needed, none present. The capture-analysis turn
    then rides the chat stream send (EchoJayAPI.cpp ~1829-1875): connect timeout 60 s ONLY, readIntoMemoryBlock with NO
    READ DEADLINE - the same pattern already filed for chat; capture shares it. (The reviewer's :137-147 now holds
    mintDashboardHandoff; the no-deadline read moved to the stream send.)
## Fix shape, for the ruling (NOT built): heap-allocate the KeyEngine in run() (std::make_unique) or give the save thread
an explicit 8 MB stack; either is one line, main-plugin (V2) only. Findings 4(a)/4(b) are separate items.

## RULING (8 Sep, 11:4x) and Build A
Fix = heap-allocate the KeyEngine, scoped to run() (std::make_unique). The 8 MB stack is REJECTED: a magic number sized to
today's object, leaves a 2 MB local in place for the next caller, re-breaks silently if the object grows or the call nests.
## Item 2 - why only now: the condition is UNCHANGED since 7 Aug, and it was never exercised
    git log -L on both halves of the key-source rule (PluginProcessor.cpp:3098 placement == 1; :3087-3090 the four
    channel types) returns exactly one commit: 28743f4, 7 Aug 2026, Sean's "captures carry their key". Kathy's merge did
    not touch it. Nothing widened; not a regression. Note what the rule means in practice: the DEFAULT channel type is
    FullMix (PluginProcessor.h:1104), which QUALIFIES - so any capture longer than 2 s on an untyped channel has walked
    into this since 7 Aug. The harness negative control below crashes the same way on arm64 with NO Link at all.
## Item 6 - FILED, NOT FIXED: audio-thread work in the capture path (do not touch today)
    (a) stopCapture() from processBlock on transport stop (PluginProcessor.cpp:688-689): CaptureSnapshot with juce::Strings,
        std::make_unique<SaveThread>, startThread() - heap + pthread create on the audio thread.
    (b) WaveformRecorder::ensureCapacity -> audioBuffer.setSize(..., keepExisting) every kGrowChunkSamples = 441,000
        samples (~10 s): a copying reallocation of the whole recording on the audio thread, one per recorder; at 45 Links,
        46 of them (accumulateLinkChannel :110 feeds each Link's recorder from processBlock).
    PREDICTION for Sean to confirm or refute by ear during the shoot: an audible click or dropout at roughly ten-second
    intervals during long captures, worse with more Links; and a possible click at the instant a capture ends on
    transport stop.
## Item 7 - filed against the existing no-read-deadline defect, not a new one
    The capture-analysis turn rides the chat stream send: connect timeout 60 s only, readIntoMemoryBlock with no read
    deadline (EchoJayAPI.cpp ~1829-1875). Same habit, same bug, same fix as the dashboard wedge already recorded in
    MERGE_2026-09-06.md ("has NO deadline - it ends on EOF, on error, or when the USER cancels"). One entry.
## Item 8 - the count: FIVE WERE NEVER PLACED, none failed to claim
    v8 session log (pid 32225): Pro Tools delivered exactly 40 distinct track names ("host DELIVERED track name"); the
    registry holds 40 live rows; the two sets are identical (0 delivered-without-row, 0 row-without-delivery); zero lines
    matching STILL FULL / regFull / no free slot / claim failed. 80 constructions, 69 re-mints, 9 ghost adoptions, all
    converging on 40. The session has 40 Links, not 45. Not a capacity or claim-gate defect.

## Severity, corrected (ruling 8 Sep 12:0x): this was the DEFAULT path, not a corner case
FullMix is the default channel type and qualifies as a key source, so every capture longer than 2 s on an untyped
channel has taken this path since 7 Aug. The harness negative control reproduces it on arm64 with no Link at all
(harness-2026-09-08-115100.ips, thread "EchoJay WAV Save", KERN_PROTECTION_FAILURE). Earlier host reports re-examined:
the two 6 Sep Pro Tools reports carry no EchoJay frame in any thread and no save thread - not this defect, correctly
Avid's; no Logic / AUHostingService report exists since 7 Aug. FIXED in Build A (KeyEngine heap-allocated, scoped to
run()); leg: build_a_legs_test crash (pre-fix Bus error 10, after: key pass completes).
