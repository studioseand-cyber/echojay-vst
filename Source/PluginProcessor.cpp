#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FaderTaper.h"   // shared mixer-fader mute taper (P17)
#include "NativeClip.h"   // EchoJay_NSLog (memdiag)
#include "EchoJayWorkspace.h"   // runRoundTripSelfTest (C1/C3 verify)
#include "EedTempoClock.h"      // publishHostTempo (built-in tempo-synced devices)
#include "EedKeyEngine.h"       // offline key pass over captures (§5.2)
#include "EedKeyFeed.h"         // publish the resolved key for EchoJay Pitch (spec §6)
#include "EedKeyDetectorProcessor.h"   // the local-chain source of the key walk
#include <cmath>

// ---------------------------------------------------------------------------
// Memory diagnostics (compile flag, OFF by default). Set ECHOJAY_MEMDIAG=1 to
// log per-capture RSS, capture-buffer bytes, save-thread state, snapshot
// count, and accumulated chat bytes — for attributing per-pass retention.
#ifndef ECHOJAY_MEMDIAG
 #define ECHOJAY_MEMDIAG 0
#endif
#if ECHOJAY_MEMDIAG && JUCE_MAC
 #include <mach/mach.h>
#endif

// ---------------------------------------------------------------------------
// Teardown logging (diagnostic, OFF by default).
//
// Used to diagnose the Windows/Cubase freeze-on-removal (a loader-lock hang in
// GPU image teardown at DLL unload, since fixed). Left in place but disabled:
// set ECHOJAY_TEARDOWN_LOGGING to 1 to re-enable if a teardown issue ever needs
// investigating again. When enabled it appends timestamped lines to a small log
// file a remote tester can email back:
//   Windows: %APPDATA%\EchoJay\teardown.log
//   macOS:   ~/Library/Application Support/EchoJay/teardown.log
//   Linux:   ~/.echojay/teardown.log
// When disabled (the default) every ejTeardownLog() call is a no-op with no
// disk I/O.
// ---------------------------------------------------------------------------
// ============ Per-Link capture channel ============
// Owns a MeterEngine + WaveformRecorder for one Link stream during a capture.
// Heap-allocated (unique_ptr) so atomics can live in place without move.
struct EchoJayProcessor::LinkCaptureChannel
{
    juce::String name;
    juce::String uid;      // stable identity; live name resolved at compose
    int          slotIdx;
    int          placement = 0;   // Link's declared placement at capture start
                                  // (0 unset, 1 bus, 2 insert, 3 send) — the
                                  // key pass prefers a BUS channel (§5.2/5.3)

    // Drain buffers (pre-allocated at construction to avoid audio-thread allocs)
    std::vector<float> tmpBufL;
    std::vector<float> tmpBufR;

    // Spectrum accumulation (audio thread during capture, message thread after)
    std::array<float, 64> spectrumPeak {};
    std::array<float, 64> spectrumSum  {};
    int spectrumFrames = 0;

    // Per-capture accumulators (atomic: written audio thread, read message thread)
    std::atomic<float>     capPeakL          { 0.0f };
    std::atomic<float>     capPeakR          { 0.0f };
    std::atomic<double>    capSumSqL         { 0.0  };
    std::atomic<double>    capSumSqR         { 0.0  };
    std::atomic<double>    capGatedSumSqL    { 0.0  };
    std::atomic<double>    capGatedSumSqR    { 0.0  };
    std::atomic<long long> capTotalSamples   { 0    };
    std::atomic<long long> capGatedSamples   { 0    };
    std::atomic<double>    capWidthSum       { 0.0  };
    std::atomic<double>    capCorrSum        { 0.0  };
    std::atomic<int>       capGatedBufCount  { 0    };
    std::atomic<float>     capRunningPeakForGate { 0.0f };
    std::atomic<float>     capMaxMomentary   { -100.0f };
    std::atomic<float>     capMaxShortTerm   { -100.0f };

    MeterEngine      meterEngine;
    WaveformRecorder waveformRecorder;

    // Finalised on message thread after stopCapture
    std::array<float, 64> finalSpectrumPeak {};
    std::array<float, 64> finalAvgSpectrum  {};
    bool hasDualSpectrum = false;

    LinkCaptureChannel(const juce::String& n, const juce::String& u, int idx, double sr, int bs)
        : name(n), uid(u), slotIdx(idx),
          tmpBufL((size_t)std::max(bs * 2, 4096), 0.0f),
          tmpBufR((size_t)std::max(bs * 2, 4096), 0.0f)
    {
        spectrumPeak.fill(-120.0f);
        spectrumSum.fill(0.0f);
        meterEngine.prepare(sr, bs);
        waveformRecorder.prepare(sr, bs);
        waveformRecorder.startRecording();
    }

    // Non-copyable — managed via unique_ptr
    LinkCaptureChannel(const LinkCaptureChannel&) = delete;
    LinkCaptureChannel& operator=(const LinkCaptureChannel&) = delete;
};

// Accumulate one audio block into a link capture channel (audio thread).
static void accumulateLinkChannel(EchoJayProcessor::LinkCaptureChannel& lcc,
                                   const float* L, const float* R, int n)
{
    if (n <= 0) return;

    lcc.meterEngine.processBlock(L, R, n);
    lcc.waveformRecorder.processBlock(L, R, n);

    // Spectrum
    auto liveSpec = lcc.meterEngine.getMeterData().spectrum;
    for (int i = 0; i < 64; ++i)
    {
        if (liveSpec[(size_t)i] > lcc.spectrumPeak[(size_t)i])
            lcc.spectrumPeak[(size_t)i] = liveSpec[(size_t)i];
        lcc.spectrumSum[(size_t)i] += liveSpec[(size_t)i];
    }
    lcc.spectrumFrames++;

    float bufPeakL = 0.0f, bufPeakR = 0.0f;
    double bufSumSqL = 0.0, bufSumSqR = 0.0;
    for (int i = 0; i < n; ++i)
    {
        float aL = std::abs(L[i]);
        float aR = std::abs(R[i]);
        if (aL > bufPeakL) bufPeakL = aL;
        if (aR > bufPeakR) bufPeakR = aR;
        bufSumSqL += (double)L[i] * (double)L[i];
        bufSumSqR += (double)R[i] * (double)R[i];
    }

    // Running peaks
    float curPL = lcc.capPeakL.load();
    while (bufPeakL > curPL && !lcc.capPeakL.compare_exchange_weak(curPL, bufPeakL)) {}
    float curPR = lcc.capPeakR.load();
    while (bufPeakR > curPR && !lcc.capPeakR.compare_exchange_weak(curPR, bufPeakR)) {}
    float bufMax = std::max(bufPeakL, bufPeakR);
    float curG = lcc.capRunningPeakForGate.load();
    while (bufMax > curG && !lcc.capRunningPeakForGate.compare_exchange_weak(curG, bufMax)) {}

    // Total RMS
    double cur = lcc.capSumSqL.load();
    while (!lcc.capSumSqL.compare_exchange_weak(cur, cur + bufSumSqL)) {}
    cur = lcc.capSumSqR.load();
    while (!lcc.capSumSqR.compare_exchange_weak(cur, cur + bufSumSqR)) {}
    lcc.capTotalSamples.fetch_add(n);

    // Gate (gated RMS for individual channels)
    float runPeak = lcc.capRunningPeakForGate.load();
    float gate = std::max(runPeak * 0.00316f, 0.001f);
    if (bufMax > gate)
    {
        cur = lcc.capGatedSumSqL.load();
        while (!lcc.capGatedSumSqL.compare_exchange_weak(cur, cur + bufSumSqL)) {}
        cur = lcc.capGatedSumSqR.load();
        while (!lcc.capGatedSumSqR.compare_exchange_weak(cur, cur + bufSumSqR)) {}
        lcc.capGatedSamples.fetch_add(n);

        auto ld = lcc.meterEngine.getMeterData();
        cur = lcc.capWidthSum.load();
        while (!lcc.capWidthSum.compare_exchange_weak(cur, cur + (double)ld.width)) {}
        cur = lcc.capCorrSum.load();
        while (!lcc.capCorrSum.compare_exchange_weak(cur, cur + (double)ld.correlation)) {}
        lcc.capGatedBufCount.fetch_add(1);

        float m = ld.momentary;
        float cm = lcc.capMaxMomentary.load();
        while (m > cm && !lcc.capMaxMomentary.compare_exchange_weak(cm, m)) {}
        float st = ld.shortTerm;
        float cst = lcc.capMaxShortTerm.load();
        while (st > cst && !lcc.capMaxShortTerm.compare_exchange_weak(cst, st)) {}
    }
}

// Finalise a LinkCaptureChannel into a ChannelMeterData (message thread).
static ChannelMeterData finalizeLinkChannel(EchoJayProcessor::LinkCaptureChannel& lcc,
                                             float durationSeconds)
{
    ChannelMeterData result;
    result.name = lcc.name;
    result.uid  = lcc.uid;
    result.framesReceived = (int64_t) lcc.capTotalSamples.load();  // 0 = no frames
    // Channel thumbnail (handshake step a): the channel's OWN recorder, never
    // the host's. Only when real frames arrived (no picture for a dead feed).
    if (result.framesReceived != 0)
        result.thumbnail = lcc.waveformRecorder.getThumbnail();
    result.meterData = lcc.meterEngine.getMeterData();

    auto toDb = [](float lin) { return lin > 1e-10f ? 20.0f * std::log10(lin) : -100.0f; };

    float pL = lcc.capPeakL.load(), pR = lcc.capPeakR.load();
    result.meterData.peakL = result.meterData.peakMaxL = toDb(pL);
    result.meterData.peakR = result.meterData.peakMaxR = toDb(pR);

    // Gated RMS for Link channels
    long long rmsN = lcc.capGatedSamples.load();
    double sqL = lcc.capGatedSumSqL.load(), sqR = lcc.capGatedSumSqR.load();
    if (rmsN > 0)
    {
        result.meterData.rmsL = (float)(sqL / rmsN > 1e-20 ? 10.0 * std::log10(sqL / rmsN) : -100.0);
        result.meterData.rmsR = (float)(sqR / rmsN > 1e-20 ? 10.0 * std::log10(sqR / rmsN) : -100.0);
    }
    else { result.meterData.rmsL = result.meterData.rmsR = -100.0f; }

    float peakDb = std::max(result.meterData.peakL, result.meterData.peakR);
    float rmsDb  = std::max(result.meterData.rmsL,  result.meterData.rmsR);
    result.meterData.crestFactor = (peakDb > -90.0f && rmsDb > -90.0f)
        ? juce::jlimit(0.0f, 40.0f, peakDb - rmsDb) : 0.0f;

    int gatedBufs = lcc.capGatedBufCount.load();
    if (gatedBufs > 0)
    {
        result.meterData.width       = (float)(lcc.capWidthSum.load()  / gatedBufs);
        result.meterData.correlation = (float)(lcc.capCorrSum.load()   / gatedBufs);
    }
    result.meterData.momentaryMax = lcc.capMaxMomentary.load();
    result.meterData.shortTermMax = lcc.capMaxShortTerm.load();

    // Spectrum — peak for individual channels
    bool hasPeak = false;
    for (int i = 0; i < 64; ++i) if (lcc.spectrumPeak[(size_t)i] > -119.0f) { hasPeak = true; break; }
    lcc.finalSpectrumPeak = lcc.spectrumPeak;
    if (lcc.spectrumFrames > 0)
        for (int i = 0; i < 64; ++i)
            lcc.finalAvgSpectrum[(size_t)i] = lcc.spectrumSum[(size_t)i] / (float)lcc.spectrumFrames;
    lcc.hasDualSpectrum = hasPeak && (lcc.spectrumFrames > 0);

    if (hasPeak)        result.meterData.spectrum = lcc.spectrumPeak;
    else if (lcc.spectrumFrames > 0) result.meterData.spectrum = lcc.finalAvgSpectrum;

    juce::ignoreUnused(durationSeconds);
    return result;
}

#ifndef ECHOJAY_TEARDOWN_LOGGING
 #define ECHOJAY_TEARDOWN_LOGGING 0
#endif

void ejTeardownLog(const juce::String& msg)
{
   #if ECHOJAY_TEARDOWN_LOGGING
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    auto dir = appData.getChildFile("Application Support/EchoJay");
   #elif JUCE_WINDOWS
    auto dir = appData.getChildFile("EchoJay");
   #else
    auto dir = appData.getChildFile(".echojay");
   #endif
    dir.createDirectory();
    auto logFile = dir.getChildFile("teardown.log");
    auto line = juce::Time::getCurrentTime().toString(true, true, true, true)
              + "  " + msg + juce::newLine;
    logFile.appendText(line);
    DBG("[TEARDOWN] " << msg); // also emit to debugger in Debug builds
   #else
    juce::ignoreUnused(msg);
   #endif
}

EchoJayProcessor::EchoJayProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Session C: join the PROCESS-WIDE poller. Registered here rather than
    // from the editor, so the poll exists for the whole life of this instance
    // and does not depend on a window ever being opened, and so an editor
    // recreation neither starts nor stops it. The first instance in the host
    // starts the timer; every later one shares the same request.
    dashPoll->addClient(this, &api, [this]
    {
        if (onDashUnreadChanged) onDashUnreadChanged();
    });

#if JUCE_DEBUG
    // Workspace serialise/parse round-trip self-assert (Phase C1/C3): runs
    // once per process, result to stderr. DEBUG ONLY — release builds must
    // not carry test code or scan-time stderr noise; the runnable release
    // check is tools/workspace_roundtrip_test/.
    EchoJayWorkspace::runRoundTripSelfTest();
#endif

    // Auto-detect the host DAW so stock plugins for the DAW the user is
    // actually running get injected without them having to tick anything in
    // Settings. juce::PluginHostType identifies the host from the loading
    // process. Note: out-of-process / sandboxed hosts (some AUv3, plugin
    // bridges, separate scan processes) can report Unknown — in that case we
    // leave the detected DAW empty and fall back to the Settings selection.
    // Map the host to the same labels the Settings UI and stock catalogues
    // use ("Logic Pro", "Ableton Live", ...).
    {
        juce::PluginHostType host;
        juce::String label;
        if      (host.isLogic())       label = "Logic Pro";
        else if (host.isAbletonLive()) label = "Ableton Live";
        else if (host.isFruityLoops()) label = "FL Studio";
        else if (host.isProTools())    label = "Pro Tools";
        else if (host.isStudioOne())   label = "Studio One";
        else if (host.isCubase() || host.isNuendo()) label = "Cubase";
        // Other/Unknown hosts (Reaper, Bitwig, GarageBand, standalone, or an
        // unidentified out-of-process host) leave label empty -> Settings
        // selection is used instead.
        pluginScanner.setDetectedDaw(label);
    }

    // Report hosted-chain latency to the DAW — same mirror EchoJay Link has
    // always had (LinkProcessor ctor). Without this the DAW cannot
    // delay-compensate the track when a latent plugin (lookahead limiter,
    // linear-phase EQ) sits in the chain, and the master wet/dry's dry leg
    // would be aligned against a host timeline that is itself off.
    // ONE unlatch action, two triggers (13 Aug 2026): the scanner fires
    // this on ANY disabled-set change - the setter on the instance that
    // took the click, the file reload on every other instance - so the
    // recommendable feed rebuilds with fresh ticks either way. Wired here
    // because the processor owns both objects; the editor's timer is only
    // the file-watch pump.
    pluginScanner.onDisabledSetChanged = [this]
    {
        chainHost.invalidateRecommendable();
    };

    chainHost.onChainChanged = [this]
    {
        setLatencySamples(chainHost.getTotalLatencySamples());
        // The chain now produces different audio, so the held true peak / peak /
        // overs describe a signal that no longer exists (they were contradicting
        // a capture taken seconds later). Drop those holds; integrated LUFS / LRA
        // keep accumulating. onChainChanged fires from rebuildGraph(), which every
        // structural op routes through (load, clear, add, remove, bypass toggle,
        // reorder), so this covers all of them. Message-thread signal only.
        meterEngine.resetHolds();
    };

    // Hosted plugin settings survive a DAW save only if they were serialised
    // BEFORE the host asked for them: getStateInformation writes strings the
    // cache already holds and never calls into a hosted plugin. Enabled here
    // and not in ChainHost's constructor because EchoJay Link shares the
    // class and captures live in LinkProcessor::chainModelToVar instead.
    chainHost.setStateCacheEnabled(true);

    // Which wrapper this instance is (for the rack note in completeLoad),
    // and the one-time, loud log for the VST3-in-AU-host experiment.
    chainHost.setHostPluginFormat(wrapperType == wrapperType_AudioUnit ? juce::String("AudioUnit")
                                : wrapperType == wrapperType_VST3      ? juce::String("VST3")
                                                                       : juce::String());
    if (wrapperType == wrapperType_AudioUnit && ChainHost::vst3InAuHostExperiment())
    {
        static bool loggedOnce = false;   // once per host process, not per instance
        if (!loggedOnce)
        {
            loggedOnce = true;
            EchoJay_NSLog("EJVst3InAu: EXPERIMENT ON. ~/Library/EchoJay/vst3_in_au_host is set (with dev_mode): "
                          "VST3 plugins are OFFERED in the chain list and the model feed inside this AU host. "
                          "Experimental. Sessions and chains built with it hold VST3 slots: with the flag off "
                          "they still restore on this machine (the load path does not check format) and the "
                          "rack says so; on a machine without those VST3s they are skipped with a rack note.");
        }
    }

    // Defer plugin cache loading to background so constructor returns fast.
    // Tracked on loadThread (a std::thread member) so the destructor can join
    // it — see ~EchoJayProcessor. Bails immediately if shutdown began.
    loadThread = std::thread([this]() {
        if (isShuttingDown.load()) return;
        pluginScanner.loadCache();
        if (isShuttingDown.load()) return;
        pluginScanner.loadEnabledState();
        if (isShuttingDown.load()) return;
        pluginScanner.loadCustomFolders();
    });

    // Session adoption BEFORE any UI exists — synchronous, in the processor
    // constructor, so the prompt chain can never evaluate pre-adoption state
    // (the show-then-adopt race: prompt flashes for a tick, adoption lands,
    // stale prompt pixels linger). setStateInformation later overrides both
    // with the project's own serialised values, preserving precedence (a).
    {
        auto sp = ChainHost::getSessionProjectName();
        if (projectName.trim().isEmpty() && sp.isNotEmpty())
        {
            projectName = sp;
            projectPromptDismissed = true;
            EchoJay_NSLog(("EJPrompt: adopted session name pre-UI \"" + sp + "\"").toRawUTF8());
        }
        auto sg = ChainHost::getSessionGenre();
        if (!genrePromptDismissed && sg.isNotEmpty())
        {
            genre = sg;
            genrePromptDismissed = true;
            EchoJay_NSLog(("EJPrompt: adopted session genre pre-UI \"" + sg + "\"").toRawUTF8());
        }
    }

    // Self key detection (§6.1): when THIS channel's declared role is a
    // music bus, the plugin detects its own key on the Link's §5.1 duty
    // cycle — committed passes only, live chroma off (the Meters wheel
    // eases from the reading's chroma). Worker always runs (idle = one
    // 250 ms wakeup); the tap is only fed while the role qualifies, and
    // the 1 Hz timer only arms passes then too.
    selfKeyEngine_.setContinuous(false);
    selfKeyEngine_.setWindowSeconds(kSelfKeyWindowS);
    selfKeyEngine_.setLiveChromaEnabled(false);
    selfKeyWorker_.startThread();
    startTimer(1000);
}


// ===== Session C: dash poll logging =========================================
//
// A FILE, not juce::Logger::writeToLog, and the difference is the whole point
// of the instrumentation.
//
// No custom JUCE logger is installed anywhere in this codebase, so
// writeToLog falls through to JUCE's default, which on macOS writes to
// stderr. A plugin running inside Logic has its stderr swallowed by the host:
// the lines never reach Apple's unified log, so `log show` cannot see them,
// and there is nothing to tail. The first attempt at this shipped exactly
// that mistake and produced an empty log for a poll that was working.
//
// ejTeardownLog beside this already had the right shape and it is followed
// here: append to a file under Application Support, which survives process
// restarts and reads with `tail -f`. That matters more than usual for this
// particular log, because the question it answers spans editor recreations
// and therefore spans the moments you are least able to watch a console.
//
// Gated on ECHOJAY_DEV_TRANSPORT, which is OFF by default and which
// build-installer.sh never passes, so no release artefact contains it.
void ejDashLog(const juce::String& msg)
{
   #if ECHOJAY_DEV_TRANSPORT
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    auto dir = appData.getChildFile("Application Support/EchoJay");
   #elif JUCE_WINDOWS
    auto dir = appData.getChildFile("EchoJay");
   #else
    auto dir = appData.getChildFile(".echojay");
   #endif
    dir.createDirectory();
    dir.getChildFile("dash-poll.log")
       .appendText(juce::Time::getCurrentTime().toString(true, true, true, true)
                   + "  " + msg + juce::newLine);
   #else
    juce::ignoreUnused(msg);
   #endif
}

EchoJayProcessor::~EchoJayProcessor()
{
    // Stage 1 SOLO: the plugin is leaving the track with a session open.
    // Best-effort commit (fire and forget -- no ack can be awaited in a
    // destructor), then a normal end: the lease file is deleted so the Link
    // restores IMMEDIATELY rather than at the 3s expiry.
    if (editActive())
    {
        const juce::String b64 = editCaptureStateB64();
        int lerr = 0;
        const juce::String dir = LinkShm::resolveDir(lerr);
        if (b64.isNotEmpty() && dir.isNotEmpty())
        {
            auto* cmd = new juce::DynamicObject();
            cmd->setProperty("v",           1);
            cmd->setProperty("seq",         (int) (juce::Time::currentTimeMillis() / 1000));
            cmd->setProperty("commitSlot",  editSession_.slot0 + 1);
            cmd->setProperty("commitState", b64);
            // No baseSlots: a dying host cannot re-read the rack, and the
            // Link treats a missing array as a size mismatch and refuses --
            // which is the SAFE direction. The state is at worst not saved;
            // it is never applied to the wrong slot.
            juce::File(dir + "ctrl-cmd-" + editSession_.uid + ".json")
                .replaceWithText(juce::JSON::toString(juce::var(cmd), true));
        }
        editEnd(false);
    }
    ejTeardownLog("~EchoJayProcessor enter");
    stopTimer();   // §6.1 scheduler — no arm can fire into teardown

    // Deregister from the SHARED poller first, so no tick can notify into
    // members being torn down below. The poller itself keeps running for any
    // other EchoJay instance still loaded, and stops when the last one goes.
    dashPoll->removeClient(this);
    onDashUnreadChanged = nullptr;

    // Signal background work to stop touching members.
    isShuttingDown.store(true);

    // Tell the scanner to abort any in-flight scan/load before we join, so the
    // load thread returns quickly rather than finishing a full cache parse.
    pluginScanner.requestStop();
    ejTeardownLog("scanner stop requested");

    // Join the cache-load thread deterministically. Unlike the previous
    // fire-and-forget Thread::launch, this guarantees the worker is no longer
    // touching pluginScanner before members are destroyed below.
    if (loadThread.joinable())
    {
        ejTeardownLog("joining loadThread...");
        loadThread.join();
        ejTeardownLog("loadThread joined");
    }

    // Wait for any in-flight WAV save to finish before we destroy members.
    if (saveThread && saveThread->isThreadRunning())
    {
        ejTeardownLog("waiting for saveThread...");
        saveThread->waitForThreadToExit(5000);
        ejTeardownLog("saveThread done");
    }

    ejTeardownLog("~EchoJayProcessor exit (members destruct next)");

    // Link consumer — close synchronously (audio thread stopped)
    disconnectAllLinkSlotsNow();
    closeLinkRegistryNow();
}

bool EchoJayProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Only support mono and stereo. Rejects 5.1, 7.1, ambisonics, etc.
    // This is what stops Pro Tools showing the long list of channel layouts.
    auto in  = layouts.getMainInputChannelSet();
    auto out = layouts.getMainOutputChannelSet();
    
    if (in != out)
        return false;
    
    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo();
}

void EchoJayProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Bus trim smoothing: the Link's 30ms ramp, same feel both sides
    busGainSmoothed_.reset(sampleRate, 0.030);
    busGainSmoothed_.setCurrentAndTargetValue(
        EchoJayFader::gainForDb(busGainDb_.load(std::memory_order_relaxed)));
    meterEngine.prepare(sampleRate, samplesPerBlock);
    captureEngine.prepare(sampleRate, samplesPerBlock);
    selfKeyEngine_.prepare(sampleRate, samplesPerBlock);   // §6.1 self key tap
    abMeterEngine.prepare(sampleRate, samplesPerBlock);
    cmpMeter[0].prepare(sampleRate, samplesPerBlock);
    cmpMeter[1].prepare(sampleRate, samplesPerBlock);
    cmpTmpBuf.setSize(2, samplesPerBlock);
    cmpMixBuf.setSize(2, samplesPerBlock);
    cmpGainScratch.assign((size_t)juce::jmax(1, samplesPerBlock), 0.0f);
    waveformRecorder.prepare(sampleRate, samplesPerBlock);
    hostSampleRate_      = sampleRate;
    hostSamplesPerBlock_ = samplesPerBlock;
    chainHost.prepare(sampleRate, samplesPerBlock);
}

void EchoJayProcessor::releaseResources()
{
    ejTeardownLog("releaseResources enter");
    meterEngine.reset();
    chainHost.release();
    ejTeardownLog("releaseResources exit");
}

void EchoJayProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    audioBlocksProcessed_.fetch_add(1, std::memory_order_relaxed);   // host audio liveness
    juce::ScopedNoDenormals noDenormals;
    
    // Track DAW transport state (play/stop)
    if (auto* playHead = getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            bool playing = pos->getIsPlaying();
            transportPlaying.store(playing);

            // Self key scheduler (§6.1/§5.4): a position landing far from
            // where the last block left off is a section jump — the next
            // passive pass becomes due immediately.
            if (auto t = pos->getTimeInSeconds())
            {
                const double prev = transportTimeS_.load(std::memory_order_relaxed);
                if (playing && wasTransportPlaying
                    && std::abs(*t - prev) > kSelfKeyJumpSeconds)
                    selfKeyJump_.store(true, std::memory_order_relaxed);
                transportTimeS_.store(*t, std::memory_order_relaxed);
            }

            // Publish the host tempo for built-in devices that sync to it (the
            // Time cluster's delay). They live inside ChainHost's graph, which
            // has no playhead of its own, so this read — which we are doing
            // anyway — is their only route to a BPM. See EedTempoClock.h.
            if (auto bpm = pos->getBpm())
                echojay::publishHostTempo(*bpm);

            // Sync AB playback position to DAW transport
            if (abSyncToDAW.load() && abActive.load() && abSampleCount > 0)
            {
                if (playing)
                {
                    // DAW is playing — sync ref position to DAW timeline
                    if (abPlayingRef.load())
                    {
                        if (auto timeInSamples = pos->getTimeInSamples())
                        {
                            int dawPos = (int)*timeInSamples;
                            double ratio = abSampleRate / getSampleRate();
                            int abPos = (int)(dawPos * ratio);
                            if (abSampleCount > 0)
                                abPos = abPos % abSampleCount;
                            if (abPos < 0) abPos = 0;
                            abPlaybackPos = abPos;
                        }
                    }
                }
                // When DAW stops: do nothing — ref keeps playing freely,
                // pass goes silent (no DAW audio flowing). User toggles A/B
                // to switch between ref and pass (silence when DAW stopped).
                // DAW start: ref re-syncs position on next playing block.
            }
            
            // Auto-stop capture when transport stops (spacebar)
            if (wasTransportPlaying && !playing && captureState.load() == CaptureState::Capturing)
                stopCapture();

            // Sync compare streams to DAW transport on state TRANSITIONS only.
            // Running every block was stomping user-initiated play/pause from the button.
            if (cmpSyncToTransport.load() && !cmpBothCaptures.load()
                && playing != wasTransportPlaying)
            {
                for (int sl = 0; sl < 2; ++sl)
                {
                    if (!cmpStream[sl].loaded.load()) continue;
                    if (playing)
                        cmpStream[sl].playing.store(true);
                    else
                        cmpStream[sl].playing.store(false);
                }
            }

            // SYNC position-follow: reference slots track the DAW playhead
            // every block (host->reference time, zero offset by default plus
            // any captured manual offset). Continuous host motion assigns
            // continuous positions; loops/jumps snap by the SAME assignment
            // - no smoothing, no chasing.
            if (cmpSyncToTransport.load() && !cmpBothCaptures.load() && playing)
            {
                if (auto tSec = pos->getTimeInSeconds())
                {
                    cmpLastHostTimeSec.store(*tSec);
                    const double off = cmpSyncOffsetSec.load();
                    std::lock_guard<std::mutex> lock(cmpMutex);
                    for (int sl = 0; sl < 2; ++sl)
                    {
                        auto& st = cmpStream[sl];
                        if (!cmpSlotIsRef[sl].load() || !st.loaded.load()
                            || st.sampleCount <= 0 || st.sampleRate <= 0) continue;
                        const double lenSec = (double) st.sampleCount / st.sampleRate;
                        const double refSec = juce::jlimit(0.0, lenSec, *tSec + off);
                        st.playbackPos = juce::jmin((int)(refSec * st.sampleRate),
                                                    st.sampleCount - 1);
                        if (!st.playing.load()) st.playing.store(true);
                    }
                }
            }

            wasTransportPlaying = playing;
        }
    }
    
    const float* left = buffer.getNumChannels() >= 1 ? buffer.getReadPointer(0) : nullptr;
    const float* right = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : left;
    if (left == nullptr) return;

    // meterEngine (the Live meter / Meters+Visualisation / Link Monitor mix-bus
    // row) and the capture engine now read the POST-chain output - see the tap
    // AFTER chainHost.process at the end of processBlock. They used to read here,
    // pre-chain, which reported EchoJay's INPUT and made a CHAIN-tab limiter
    // invisible to a recapture (build-recapture-verify was measuring the
    // unprocessed input). The AB/compare monitor taps below stay pre-their-own
    // playback because they analyse the slot audio directly.

    // A/B playback: replace buffer with playback audio, then analyse it into
    // abMeterEngine so the Compare playing-slot panel has its own spectrum source.
    if (abActive.load() && abPlayingRef.load())
    {
        std::lock_guard<std::mutex> lock(abMutex);
        if (abSampleCount > 0 && abPlaybackPos < abSampleCount)
        {
            int numSamples = buffer.getNumSamples();
            int abChans = abBuffer.getNumChannels();
            double dawRate = getSampleRate();
            double ratio = (abSampleRate > 0 && dawRate > 0) ? abSampleRate / dawRate : 1.0;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                float* out = buffer.getWritePointer(ch);
                const float* src = (ch < abChans) ? abBuffer.getReadPointer(ch) : abBuffer.getReadPointer(0);

                for (int i = 0; i < numSamples; ++i)
                {
                    double srcPos = abPlaybackPos + i * ratio;
                    int idx = (int)srcPos;
                    if (idx >= abSampleCount - 1)
                    {
                        out[i] = 0.0f;
                        continue;
                    }
                    // Linear interpolation
                    float frac = (float)(srcPos - idx);
                    out[i] = src[idx] * (1.0f - frac) + src[idx + 1] * frac;
                }
            }
            abPlaybackPos += (int)(numSamples * ratio);
            if (abPlaybackPos >= abSampleCount)
                abPlaybackPos = abSampleCount; // will stop on next block

            // Re-read pointers since buffer was modified
            left = buffer.getReadPointer(0);
            right = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : left;

            // AB-only spectrum: used by Compare playing-slot panel
            abMeterEngine.processBlock(left, right, numSamples);
        }
    }
    
    // ===== Compare dual-stream: both advance + analyse into temp buffers =====
    // NEVER touches the main output buffer — DAW passthrough is always preserved.
    // The audible stream's audio is copied to the output buffer AFTER rendering.
    {
        int audible = cmpAudible.load();
        std::lock_guard<std::mutex> lock(cmpMutex);
        int numSamples = buffer.getNumSamples();
        double dawRate = getSampleRate();
        // Monitor crossfade (25 Jul 2026): the audible stream is MIXED with
        // the DAW signal through a per-sample 8ms gain ramp instead of a hard
        // memcpy replace, so engage / A-B switch / codec disengage can never
        // click. At gain 1 the result is byte-equivalent to the old replace.
        const float gStep = (dawRate > 0) ? (float)(1.0 / (0.008 * dawRate)) : 0.01f;
        if (cmpMixBuf.getNumSamples() < numSamples) cmpMixBuf.setSize(2, numSamples, false, false, true);
        if ((int)cmpGainScratch.size() < numSamples) cmpGainScratch.assign((size_t)numSamples, 0.0f);
        bool contributed = false;

        for (int sl = 0; sl < 2; ++sl)
        {
            auto& s = cmpStream[sl];
            if (!s.loaded.load() || s.sampleCount <= 0) continue;
            const bool rolling = s.playing.load();
            const float target = (rolling && sl == audible && !s.stopAtZero.load()) ? 1.0f : 0.0f;
            if (!rolling && s.monGain <= 0.0001f) continue;   // fully idle

            double ratio = (s.sampleRate > 0 && dawRate > 0) ? s.sampleRate / dawRate : 1.0;
            int chans = s.buffer.getNumChannels();

            // Always render into temp buffer (never the output buffer directly)
            for (int ch = 0; ch < 2; ++ch)
            {
                float* out = cmpTmpBuf.getWritePointer(ch);
                const float* src = (ch < chans) ? s.buffer.getReadPointer(ch) : s.buffer.getReadPointer(0);
                for (int i = 0; i < numSamples; ++i)
                {
                    double srcPos = s.playbackPos + i * ratio;
                    int idx = ((int)srcPos) % s.sampleCount;
                    int idx2 = (idx + 1) % s.sampleCount;
                    float frac = (float)(srcPos - (int)srcPos);
                    out[i] = src[idx] * (1.0f - frac) + src[idx2] * frac;
                }
            }
            if (rolling)
            {
                s.playbackPos = ((int)(s.playbackPos + numSamples * ratio)) % s.sampleCount;
                // Feed this stream's meter from the temp buffer
                cmpMeter[sl].processBlock(cmpTmpBuf.getReadPointer(0),
                                           cmpTmpBuf.getReadPointer(1), numSamples);
            }

            if (target > 0.0f || s.monGain > 0.0001f)
            {
                if (!contributed)
                {
                    cmpMixBuf.clear(0, 0, numSamples);
                    cmpMixBuf.clear(1, 0, numSamples);
                    std::fill(cmpGainScratch.begin(), cmpGainScratch.begin() + numSamples, 0.0f);
                    contributed = true;
                }
                float g = s.monGain;
                float* mixL = cmpMixBuf.getWritePointer(0);
                float* mixR = cmpMixBuf.getWritePointer(1);
                const float* tL = cmpTmpBuf.getReadPointer(0);
                const float* tR = cmpTmpBuf.getReadPointer(1);
                for (int i = 0; i < numSamples; ++i)
                {
                    g += juce::jlimit(-gStep, gStep, target - g);
                    cmpGainScratch[(size_t)i] += g;
                    mixL[i] += tL[i] * g;
                    mixR[i] += tR[i] * g;
                }
                s.monGain = g;
            }
            // Fade-out complete: self-stop on the audio thread (codec
            // disengage, editor-close safety). Buffer stays loaded; the next
            // load replaces it under the mutex.
            if (s.stopAtZero.load() && s.monGain <= 0.0001f)
            {
                s.playing.store(false);
                s.stopAtZero.store(false);
            }
        }

        if (contributed)
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                float* out = buffer.getWritePointer(ch);
                const float* mix = cmpMixBuf.getReadPointer(std::min(ch, 1));
                for (int i = 0; i < numSamples; ++i)
                {
                    const float gT = juce::jmin(1.0f, cmpGainScratch[(size_t)i]);
                    out[i] = out[i] * (1.0f - gT) + mix[i];
                }
            }
            left = buffer.getReadPointer(0);
            right = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : left;
        }

        // SYNC lockstep for two captures: keep positions aligned by fraction
        if (cmpSyncToTransport.load() && cmpBothCaptures.load())
        {
            auto& a = cmpStream[0];
            auto& b = cmpStream[1];
            bool aOk = a.loaded.load() && a.playing.load() && a.sampleCount > 0;
            bool bOk = b.loaded.load() && b.playing.load() && b.sampleCount > 0;
            if (aOk && bOk)
            {
                // Audible stream is the leader; follower matches its fractional position
                int leader = (audible >= 0 && audible <= 1) ? audible : 0;
                int follower = 1 - leader;
                auto& ls = cmpStream[leader];
                auto& fs = cmpStream[follower];
                double frac = (double)ls.playbackPos / (double)ls.sampleCount;
                fs.playbackPos = (int)(frac * fs.sampleCount) % fs.sampleCount;
            }
        }
    }

    
    // Link consumer: drain all active Links; route to capture channels when capturing.
    {
        const bool isCapturing = (captureState.load() == CaptureState::Capturing);
        const bool gotLccLock  = isCapturing && linkCaptureSpinLock.tryEnter();

        // O(N) slot→lcc lookup on stack
        LinkCaptureChannel* lccBySlot[kMaxLinkSlots] = {};
        if (gotLccLock)
            for (auto& c : linkCaptureChannels)
                if (c->slotIdx >= 0 && c->slotIdx < kMaxLinkSlots)
                    lccBySlot[c->slotIdx] = c.get();

        for (int li = 0; li < kMaxLinkSlots; ++li)
        {
            auto& ls = activeLinkSlots[li];
            if (!ls.lock.tryEnter()) continue;
            if (ls.map != nullptr)
            {
                // Stage 1 SOLO: the edited Link's ring belongs to the edit
                // session, not the drain. Consume into the edit scratch, run
                // it through the editing copy, and leave the result for the
                // end-of-block overwrite. Capture cannot also be running
                // (mutual exclusion at both start sites), so stealing the
                // slot from the drain corrupts nothing.
                if (li == editSession_.ringSlot.load(std::memory_order_relaxed)
                    && editSession_.audioOn.load(std::memory_order_acquire))
                {
                    if (editLock_.tryEnter())
                    {
                        if (editInst_ != nullptr)
                        {
                            const int want = buffer.getNumSamples();
                            // Bounded latency: a backlog past the trip point
                            // re-seeks to the cushion. This is the stage-0
                            // seek earning its keep; without it every missed
                            // block would ratchet the audition later forever.
                            auto* hdr = LinkShm::ringHeader(ls.map);
                            if (LinkShm::loadAcquire(&hdr->writeIdx)
                                  - LinkShm::loadRelaxed(&hdr->readIdx) > kEditReseekTrip)
                                LinkShm::ringSeekForward(ls.map, kEditCushionFrames);
                            // editBuf_ is PREALLOCATED at editBegin (8192
                            // frames); a host block larger than that skips
                            // the solo for the block rather than allocating
                            // on the audio thread.
                            if (editBuf_.getNumSamples() < want)
                            { editLock_.exit(); ls.lock.exit(); continue; }
                            const uint32_t n = LinkShm::ringConsume(ls.map,
                                                  editBuf_.getWritePointer(0),
                                                  editBuf_.getWritePointer(1), want);
                            ls.framesRead.fetch_add((int64_t) n, std::memory_order_relaxed);
                            // Underrun = silence for the missing tail, never
                            // stale samples.
                            for (int ch = 0; ch < 2; ++ch)
                                if ((int) n < want)
                                    editBuf_.clear(ch, (int) n, want - (int) n);
                            juce::MidiBuffer noMidi;
                            editInst_->processBlock(editBuf_, noMidi);
                        }
                        editLock_.exit();
                    }
                    ls.lock.exit();
                    continue;
                }
                LinkCaptureChannel* lcc = gotLccLock ? lccBySlot[li] : nullptr;
                if (lcc != nullptr)
                {
                    int nReq = std::min(buffer.getNumSamples(), (int)lcc->tmpBufL.size());
                    uint32_t n = LinkShm::ringConsume(ls.map,
                                                       lcc->tmpBufL.data(),
                                                       lcc->tmpBufR.data(), nReq);
                    if (n > 0)
                    {
                        ls.framesRead.fetch_add((int64_t)n, std::memory_order_relaxed);
                        accumulateLinkChannel(*lcc, lcc->tmpBufL.data(), lcc->tmpBufR.data(), (int)n);
                    }
                }
                else
                {
                    uint32_t n = LinkShm::ringConsume(ls.map, nullptr, nullptr,
                                                       buffer.getNumSamples());
                    if (n > 0)
                        ls.framesRead.fetch_add((int64_t)n, std::memory_order_relaxed);
                }
            }
            ls.lock.exit();
        }

        if (gotLccLock) linkCaptureSpinLock.exit();
    }

    // Silence detection (for UI state only — does NOT auto-stop capture)
    float peakL = 0, peakR = 0;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        peakL = std::max(peakL, std::abs(left[i]));
        peakR = std::max(peakR, std::abs(right[i]));
    }
    bool isSilent = (peakL < 0.0001f && peakR < 0.0001f);
    
    if (isSilent)
    {
        silenceCounter++;
        if (silenceCounter > (int)(getSampleRate() * 0.5 / buffer.getNumSamples()))
        {
            audioSilent.store(true);
            wasReceivingAudio = false;
        }
    }
    else
    {
        silenceCounter = 0;
        audioSilent.store(false);
        wasReceivingAudio = true;
    }

    // CHAIN: pass audio through hosted plugin (graph handles passthrough if none loaded)
    {
        juce::MidiBuffer emptyMidi;
        chainHost.process(buffer, emptyMidi);
    }

    // ===== BUS TRIM: post-chain, PRE-meter-tap (the Link's placement) =====
    // Everything below the tap reads the trimmed signal on purpose: the
    // meters, the capture and the AI's level context all describe what
    // actually LEAVES EchoJay. Bit-transparent at 0 dB (unity early-out
    // inside), so the untouched default changes nothing.
    applyBusGainSmoothed(buffer);

    // ===== POST-CHAIN TAP =====
    // meterEngine and the capture engine read the buffer AFTER chainHost has
    // processed it, so the Live meter, the Meters/Visualisation panels, the
    // Link Monitor mix-bus row AND every capture report what actually LEAVES
    // EchoJay (the chain output), not its input. With an empty/all-bypassed
    // chain, chainHost.process returned the buffer untouched, so this is
    // bit-identical to the old pre-chain tap. Nothing between the old tap and
    // here depended on the pre-chain buffer: the Link consumer reads Link
    // rings (not this buffer), and silence detection keeps its own pre-chain
    // read above. chainHost rewrites in place - re-read the pointers.
    left  = buffer.getReadPointer(0);
    right = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : left;
    meterEngine.processBlock(left, right, buffer.getNumSamples());

    // Self key tap (§6.1) — same tap point as the meters. Fed ONLY while the
    // declared role is a music bus: on a vocal or unknown role the engine
    // never even hears the channel, which is the 5.3 rule enforced at the
    // source (the disqualifier is "not the music", judged by declared role).
    // §7 exception: a user PIN of "this channel" overrides the role gate —
    // an explicit choice beats the inference (the role may be mis-declared).
    if (isMusicBusRole(channelType) || selfKeyForced_.load(std::memory_order_relaxed))
        selfKeyEngine_.pushBlock(left, right, buffer.getNumSamples());

    // Feed capture engine if capturing
    if (captureState.load() == CaptureState::Capturing)
    {
        captureEngine.processBlock(left, right, buffer.getNumSamples());
        waveformRecorder.processBlock(left, right, buffer.getNumSamples());
        // Accumulate both peak-hold AND average spectrum during capture.
        // At snapshot time we choose: peak for individual channels (transient sources),
        // average for full mix/master/buses (representative tonal balance).
        auto liveSpec = meterEngine.getMeterData().spectrum;
        for (int i = 0; i < 64; ++i)
        {
            if (liveSpec[(size_t)i] > spectrumPeak[(size_t)i])
                spectrumPeak[(size_t)i] = liveSpec[(size_t)i];
            spectrumSum[(size_t)i] += liveSpec[(size_t)i];
        }
        spectrumFrames++;
        
        // ============ Capture-window aggregation ============
        // Per-buffer accumulators for crest/RMS/peak/width/correlation. These
        // produce a TIME-WINDOWED measurement of the captured audio rather than
        // a single-instant reading from the meter engine. Without this, the
        // snapshot's crest/width/RMS were just whatever the meter happened to
        // read on the buffer when the user clicked Capture — which is why the
        // same vocal would produce wildly different snapshot values on different
        // captures (loud word vs sustained vowel vs breath gap).
        const int n = buffer.getNumSamples();
        if (n > 0)
        {
            // Per-sample peak and sum-of-squares for total RMS
            float bufPeakL = 0.0f, bufPeakR = 0.0f;
            double bufSumSqL = 0.0, bufSumSqR = 0.0;
            for (int i = 0; i < n; ++i)
            {
                float aL = std::abs(left[i]);
                float aR = std::abs(right[i]);
                if (aL > bufPeakL) bufPeakL = aL;
                if (aR > bufPeakR) bufPeakR = aR;
                bufSumSqL += (double)left[i] * (double)left[i];
                bufSumSqR += (double)right[i] * (double)right[i];
            }
            
            // Update running peaks (for snapshot AND for the gate threshold)
            float curPL = capPeakL.load();
            while (bufPeakL > curPL && !capPeakL.compare_exchange_weak(curPL, bufPeakL)) {}
            float curPR = capPeakR.load();
            while (bufPeakR > curPR && !capPeakR.compare_exchange_weak(curPR, bufPeakR)) {}
            float curGate = capRunningPeakForGate.load();
            float bufMaxBoth = std::max(bufPeakL, bufPeakR);
            while (bufMaxBoth > curGate && !capRunningPeakForGate.compare_exchange_weak(curGate, bufMaxBoth)) {}
            
            // Total RMS accumulation (every sample counts — silence dragging things
            // down IS the truthful RMS of the captured audio for full mixes)
            // Atomic add via compare-exchange loop
            double cur = capSumSqL.load();
            while (!capSumSqL.compare_exchange_weak(cur, cur + bufSumSqL)) {}
            cur = capSumSqR.load();
            while (!capSumSqR.compare_exchange_weak(cur, cur + bufSumSqR)) {}
            capTotalSamples.fetch_add(n);
            
            // Gate: this buffer "passes" if its peak is loud enough relative to
            // the running peak so far. Standard noise gate logic — anything more
            // than 50dB below running peak, or below -60dBFS absolute, doesn't
            // count toward gated measurements (RMS, width, correlation).
            float runningPeak = capRunningPeakForGate.load();
            float gateRel = runningPeak * 0.00316f;   // -50 dB relative (10^(-50/20))
            float gateAbs = 0.001f;                    // -60 dBFS absolute (10^(-60/20))
            float gateThreshold = std::max(gateRel, gateAbs);
            bool buffPassesGate = (bufMaxBoth > gateThreshold);
            
            if (buffPassesGate)
            {
                // Gated RMS accumulators (for percussive/element sources)
                cur = capGatedSumSqL.load();
                while (!capGatedSumSqL.compare_exchange_weak(cur, cur + bufSumSqL)) {}
                cur = capGatedSumSqR.load();
                while (!capGatedSumSqR.compare_exchange_weak(cur, cur + bufSumSqR)) {}
                capGatedSamples.fetch_add(n);
                
                // Gated width and correlation. Pull the meter engine's live values
                // for this buffer — they're instantaneous but we only sum them when
                // the buffer is loud enough to have meaningful stereo measurements.
                auto liveData = meterEngine.getMeterData();
                cur = capWidthSum.load();
                while (!capWidthSum.compare_exchange_weak(cur, cur + (double)liveData.width)) {}
                cur = capCorrSum.load();
                while (!capCorrSum.compare_exchange_weak(cur, cur + (double)liveData.correlation)) {}
                capGatedBufCount.fetch_add(1);

                // Highest momentary / short-term LUFS over the capture. Tracked
                // inside the gate so silence between phrases can't register as a
                // max (it can't anyway, but this keeps it consistent with the
                // other gated measurements and reuses the liveData we just read).
                float m = liveData.momentary;
                float curM = capMaxMomentary.load();
                while (m > curM && !capMaxMomentary.compare_exchange_weak(curM, m)) {}
                float st = liveData.shortTerm;
                float curST = capMaxShortTerm.load();
                while (st > curST && !capMaxShortTerm.compare_exchange_weak(curST, st)) {}
            }
        }
    }

    // ===== Stage 1 SOLO: the last word on the buffer =====================
    // While a remote edit session runs, the mix is REPLACED by the edited
    // channel: ring audio processed through the editing copy (filled in the
    // drain loop above). Ramped both ways (~30ms) so engage and release
    // never click, exactly the Compare crossfade's idiom. A tryEnter miss
    // leaves the mix untouched for one block, which errs toward the mix --
    // the safe direction.
    {
        const bool on = editSession_.audioOn.load(std::memory_order_acquire);
        editSoloMix_.setTargetValue(on ? 1.0f : 0.0f);
        if (on || editSoloMix_.getCurrentValue() > 0.0001f)
        {
            if (editLock_.tryEnter())
            {
                const int nS = buffer.getNumSamples();
                const bool have = editInst_ != nullptr
                               && editBuf_.getNumSamples() >= nS;
                for (int i = 0; i < nS; ++i)
                {
                    const float g = editSoloMix_.getNextValue();
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    {
                        const float solo = have
                            ? editBuf_.getSample(std::min(ch, 1), i) : 0.0f;
                        float* out = buffer.getWritePointer(ch);
                        out[i] = out[i] * (1.0f - g) + solo * g;
                    }
                }
                editLock_.exit();
            }
        }
        else
            editSoloMix_.skip(buffer.getNumSamples());
    }
}

// ============ Channel Type Detection ============

juce::String EchoJayProcessor::getEffectiveChannelName() const
{
    if (channelType == ChannelType::Other && customChannelName.isNotEmpty())
        return customChannelName;
    return channelTypeNames[(int)channelType];
}

void EchoJayProcessor::setProjectName(const juce::String& name)
{
    if (name != projectName)
    {
        projectName = name;
        captureVersion = 1; // reset version whenever the project name changes
        markStateDirty();
    }
}

juce::String EchoJayProcessor::computePassName() const
{
    juce::String proj = projectName.trim();
    if (proj.isEmpty())
        return "Pass " + juce::String(passCounter + 1);   // passCounter is 0-based; +1 gives 1-based display
    return proj + " v" + juce::String(juce::jmax(1, captureVersion));
}

void EchoJayProcessor::setChannelType(ChannelType t)
{
    channelType = t;
    markStateDirty();
}

void EchoJayProcessor::setChannelTypePromptDismissed(bool dismissed)
{
    channelTypePromptDismissed = dismissed;
    markStateDirty();
}

// ============ Self key detection (§6.1) ============
// The Link's §5.1 scheduler on the main plugin's own channel, gated on the
// DECLARED ROLE being a music bus. Message thread, 1 Hz.
void EchoJayProcessor::updateTrackProperties(const TrackProperties& props)
{
    if (!props.name.has_value()) return;   // colour-only update carries no name
    const juce::String n = juce::String(*props.name).trim();
    {
        const juce::ScopedLock sl(hostTrackNameLock_);
        if (hostTrackNamePending_ == n) return;
        hostTrackNamePending_ = n;
    }
    hostTrackNameDirty_.store(true, std::memory_order_release);
}

void EchoJayProcessor::applyHostTrackNameIfDirty()
{
    if (!hostTrackNameDirty_.exchange(false, std::memory_order_acq_rel)) return;
    juce::String n;
    { const juce::ScopedLock sl(hostTrackNameLock_); n = hostTrackNamePending_; }
    chainHost.setHostTrackName(n);   // the guard (change, or late first name) lives there
}

void EchoJayProcessor::timerCallback()
{
    applyHostTrackNameIfDirty();
    scheduleSelfKeyPass();

    // Keep the KeyFeed alive without an editor. EchoJay Pitch follows the
    // session key through KeyFeed; when the ONLY publisher was the editor's
    // timer, closing the plugin window froze the key at its last value — a
    // stale key applied with confidence, which is exactly the failure the
    // chromatic fallback exists to prevent. 1 Hz is plenty: key changes are
    // rare and the corrector cross-fades them over 300 ms anyway.
    publishKeyFeed(collectKeySources());
}

void EchoJayProcessor::scheduleSelfKeyPass()
{
    const uint32_t now     = juce::Time::getMillisecondCounter();
    const bool     playing = transportPlaying.load(std::memory_order_relaxed);

    // §5.4 invalidation: section jump, or play again after a long stop.
    bool invalidated = false;
    if (selfKeyJump_.exchange(false, std::memory_order_relaxed))
        invalidated = true;
    if (playing && ! selfKeyWasPlaying_
        && selfKeyLastPlayingMs_ != 0
        && now - selfKeyLastPlayingMs_ > kSelfKeyLongGapMs)
        invalidated = true;
    if (playing) selfKeyLastPlayingMs_ = now;
    selfKeyWasPlaying_ = playing;
    if (invalidated) lastSelfKeyArmMs_ = 0;

    if (selfKeyEngine_.isCollecting())
    {
        // Transport stopped under a pass: cancel after a few seconds so the
        // window never spans two play stretches (previous reading is kept).
        if (! playing)
        {
            if (++selfKeyStallTicks_ >= 5)
            {
                selfKeyStallTicks_ = 0;
                selfKeyEngine_.cancelAnalysis();
            }
        }
        else selfKeyStallTicks_ = 0;
        return;
    }
    selfKeyStallTicks_ = 0;

    if (! isMusicBusRole(channelType)
        && ! selfKeyForced_.load(std::memory_order_relaxed))
        return;                                  // not the music, not pinned
    if (! playing) return;
    if (meterEngine.getMeterData().momentary < kSelfKeyFloorLufs) return;

    const bool due = lastSelfKeyArmMs_ == 0
                  || now - lastSelfKeyArmMs_ >= kSelfKeyIntervalMs;
    if (! due) return;

    lastSelfKeyArmMs_ = now;
    selfKeyEngine_.startAnalysis();
    selfKeyWorker_.notify();
    EchoJay_NSLog("EJKey: self passive pass armed (music-bus role)");
}

void EchoJayProcessor::armSelfKeyAnalysis()
{
    lastSelfKeyArmMs_ = juce::Time::getMillisecondCounter();
    selfKeyEngine_.startAnalysis();
    selfKeyWorker_.notify();
    EchoJay_NSLog("EJKey: self RE-ANALYSE armed");
}

void EchoJayProcessor::setKeySourcePin(const juce::String& pinId,
                                       const juce::String& label)
{
    keySourcePin_      = pinId;
    keySourcePinLabel_ = label;
    selfKeyForced_.store(pinId == "self", std::memory_order_relaxed);
    markStateDirty();
    EchoJay_NSLog(("EJKey: source pin -> "
                   + (pinId.isEmpty() ? juce::String("Auto")
                                      : pinId + " (\"" + label + "\")")).toRawUTF8());
}

// ---------------------------------------------------------------------------
// The key precedence walk (KEY_DETECTOR_SPEC §9, PITCH_CORRECTION_SPEC §6).
// Moved here from the editor: the walk reads only processor state, and the
// KeyFeed it drives must be published whether or not a window is open. The
// editor delegates to this method for its UI cache, so there is still exactly
// ONE ranking in the codebase.
// ---------------------------------------------------------------------------
EchoJayProcessor::KeySources EchoJayProcessor::collectKeySources()
{
    KeySources out;
    const juce::uint32 nowMs   = juce::Time::getMillisecondCounter();
    const juce::int64  nowWall = juce::Time::currentTimeMillis();

    // Root in octave 2, the octave the feed's note-maths examples use, for
    // sources that carry tuning but not the analysed octave.
    auto rootHzOct2 = [] (float tuningHz, int root)
    { return tuningHz * std::pow (2.0f, (float) (36 + root - 69) / 12.0f); };

    // ---- 1. the newest capture with an offline reading (§5.2) -------------
    {
        const auto snaps = getSnapshots();
        for (int i = (int) snaps.size() - 1; i >= 0; --i)
        {
            const auto& s = snaps[(size_t) i];
            if (! s.keyValid) continue;
            KeySourceReading k;
            k.kind   = KeySourceReading::Kind::Capture;
            k.pinId  = "capture";
            k.name   = s.name;
            k.detail = s.keySourceName
                     + (s.keySourcePlacement == 1 ? " (bus Link)" : " (this channel)");
            k.root = s.keyRoot; k.minor = s.keyMinor; k.conf = s.keyConfidence;
            k.tuningHz    = s.keyTuningHz > 0.0f ? s.keyTuningHz : 440.0f;
            k.tuningCents = s.keyTuningCents;
            k.rootHz      = rootHzOct2 (k.tuningHz, k.root);
            k.ageMs = s.timestamp > 0 && nowWall > s.timestamp
                        ? (juce::uint32) juce::jmin<juce::int64> (nowWall - s.timestamp,
                                                                  0x7fffffff)
                        : 0;
            k.committed = true;
            k.analysedSeconds = s.durationSeconds;
            k.hasChroma = true; k.chroma = s.keyChroma;
            k.altRoot = s.keyAltRoot; k.altMinor = s.keyAltMinor; k.altScore = s.keyAltScore;
            out.all.push_back (std::move (k));
            break;                       // newest keyed capture only
        }
    }

    // ---- 2./3. Bus-grade readings, then channel ---------------------------
    // Bus grade is Link frames with placement==bus PLUS this plugin's own
    // passive reading when its declared role IS a music bus (§6.1) — same
    // engine, same duty cycle, and the channel is by declaration the music.
    std::vector<KeySourceReading> busLinks, chanLinks, tail;
    {
        // "this channel" always EXISTS as a menu entry (§7.1). It joins the
        // BUS tier only when the declared role is a music bus AND a reading
        // exists; a non-music role lists greyed with the reason (§7.1) and
        // is still pinnable (§7.2 — the role may be mis-declared).
        const auto ct = getChannelType();
        const bool roleMusic = selfKeyRoleIsMusic();
        const auto r = getSelfKeyEngine().getReading();
        KeySourceReading k;
        k.kind   = KeySourceReading::Kind::SelfBus;
        k.pinId  = "self";
        k.name   = "this channel";
        k.detail = channelTypeNames[(int) ct];
        k.hasReading = r.valid;
        if (r.valid)
        {
            k.root = r.root; k.minor = r.minor; k.conf = r.confidence;
            k.tuningHz = r.tuningHz; k.tuningCents = r.tuningCents;
            k.rootHz = r.rootHz;
            const auto stamp = selfKeyChangeMs();
            k.ageMs = stamp != 0 ? nowMs - stamp : 0;
            k.committed = r.committed;
            k.analysedSeconds = r.analysedSeconds;
            k.hasChroma = true; k.chroma = r.chroma;
            if (r.numAlternates > 0)
            {
                k.altRoot  = r.alternates[0].root;
                k.altMinor = r.alternates[0].minor;
                k.altScore = r.alternates[0].score;
            }
        }
        if (! roleMusic)
        {
            k.poisoned = true;
            k.unusableReason = channelTypeNames[(int) ct] + " role - not the music";
        }
        if (roleMusic && r.valid) busLinks.push_back (std::move (k));
        else                      tail.push_back (std::move (k));
    }
    for (const auto& e : getLinkDisplayList())
    {
        const auto& li = e.info;
        LinkMeterFrame f;
        const bool haveFrame = li.regIdx >= 0
                            && readLinkMeterFrame(li.regIdx, f);

        KeySourceReading k;
        k.name = e.displayName;   k.uid = li.uid;
        k.pinId = "link:" + li.uid;
        k.placement = li.placement;
        k.kind = li.placement == 1 ? KeySourceReading::Kind::BusLink
                                   : KeySourceReading::Kind::ChannelLink;
        if (haveFrame && frameHasKey(f))
        {
            k.root = (int) f.keyRoot; k.minor = f.keyIsMinor != 0;
            k.conf = f.keyConfidence;
            k.tuningHz    = f.keyTuningHz > 0.0f ? f.keyTuningHz : 440.0f;
            k.tuningCents = 1200.0f * std::log2 (k.tuningHz / 440.0f);
            k.rootHz      = rootHzOct2 (k.tuningHz, k.root);
            k.ageMs = f.keyAgeMs;
            k.committed = true;          // the passive pass is a committed pass
            (k.kind == KeySourceReading::Kind::BusLink ? busLinks : chanLinks)
                .push_back (std::move (k));
        }
        else if (li.uid.isNotEmpty())
        {
            // Exists, no reading yet — a menu entry, never a precedence one.
            k.hasReading = false;
            tail.push_back (std::move (k));
        }
    }
    auto byConf = [] (const KeySourceReading& a, const KeySourceReading& b)
    { return a.conf > b.conf; };
    std::sort (busLinks.begin(),  busLinks.end(),  byConf);
    // Channel-grade: declared channel (2) beats send return (3) beats unset.
    std::sort (chanLinks.begin(), chanLinks.end(),
               [] (const KeySourceReading& a, const KeySourceReading& b)
               {
                   auto rank = [] (int p) { return p == 2 ? 0 : p == 3 ? 1 : 2; };
                   if (rank (a.placement) != rank (b.placement))
                       return rank (a.placement) < rank (b.placement);
                   return a.conf > b.conf;
               });
    for (auto& k : busLinks)  out.all.push_back (std::move (k));
    for (auto& k : chanLinks) out.all.push_back (std::move (k));

    // ---- 4. the local chain's own Key Detector (§4) -----------------------
    {
        const auto ct = getChannelType();
        const bool vocal = ct == ChannelType::LeadVocal
                        || ct == ChannelType::BackingVocal
                        || ct == ChannelType::Adlibs
                        || ct == ChannelType::VocalBus;
        auto& ch = getChainHost();
        for (int i = 0; i < ch.getNumSlots(); ++i)
            if (auto* kd = dynamic_cast<EedKeyDetectorProcessor*>(ch.getSlotProcessor(i)))
            {
                const auto r = kd->engine().getReading();
                KeySourceReading k;
                k.kind  = KeySourceReading::Kind::LocalChain;
                k.pinId = "chain";
                k.name  = "this chain";
                k.detail = getChannelType() == ChannelType::Other
                               ? juce::String() : channelTypeNames[(int) ct];
                k.hasReading = r.valid;
                if (r.valid)
                {
                    k.root = r.root; k.minor = r.minor; k.conf = r.confidence;
                    k.tuningHz = r.tuningHz; k.tuningCents = r.tuningCents;
                    k.rootHz = r.rootHz;
                    const auto stamp = kd->readingChangeMs();
                    k.ageMs = stamp != 0 ? nowMs - stamp : 0;
                    k.committed = r.committed;
                    k.analysedSeconds = r.analysedSeconds;
                    k.hasChroma = true; k.chroma = r.chroma;
                    if (r.numAlternates > 0)
                    {
                        k.altRoot  = r.alternates[0].root;
                        k.altMinor = r.alternates[0].minor;
                        k.altScore = r.alternates[0].score;
                    }
                }
                // §5.3 restated by §6.1: the disqualifier is "not the
                // music", judged by declared role — a vocal-role channel's
                // chain reading is never preferred automatically.
                k.poisoned = vocal;
                if (vocal)
                    k.unusableReason = channelTypeNames[(int) ct]
                                     + " role - not the music";
                if (r.valid) out.all.push_back (std::move (k));
                else         tail.push_back (std::move (k));
                break;                   // one detector speaks for the chain
            }
    }

    // Existence-only entries (§7.1) close the list: visible in the menu,
    // invisible to precedence (hasReading false or poisoned).
    for (auto& k : tail) out.all.push_back (std::move (k));

    if (out.all.empty()) return out;

    // ---- AUTO: first entry WITH a reading and not poisoned, in precedence
    // order — except that a STALE capture yields to a live bus-grade
    // reading (§5.3 point 1).
    for (int i = 0; i < (int) out.all.size(); ++i)
        if (out.all[(size_t) i].hasReading && ! out.all[(size_t) i].poisoned)
        { out.autoIdx = i; break; }
    if (out.autoIdx >= 0
        && out.all[(size_t) out.autoIdx].kind == KeySourceReading::Kind::Capture
        && out.all[(size_t) out.autoIdx].ageMs > kCaptureKeyFreshMs)
    {
        for (int i = 0; i < (int) out.all.size(); ++i)
            if (out.all[(size_t) i].hasReading
                && (out.all[(size_t) i].kind == KeySourceReading::Kind::BusLink
                 || out.all[(size_t) i].kind == KeySourceReading::Kind::SelfBus))
            { out.autoIdx = i; break; }
    }
    out.primaryIdx = out.autoIdx;

    // ---- §7.2: a PIN overrides precedence, including the poisoning rule —
    // an explicit choice beats an inferred one. A pinned source that is
    // GONE is stated (pinMissing), never silently replaced; a pinned source
    // that exists but has no reading yet shows as waiting, not as Auto.
    if (const juce::String pin = getKeySourcePin(); pin.isNotEmpty())
    {
        int idx = -1;
        for (int i = 0; i < (int) out.all.size(); ++i)
            if (out.all[(size_t) i].pinId == pin) { idx = i; break; }
        if (idx < 0)
        {
            out.pinMissing = true;
            out.pinMissingLabel = getKeySourcePinLabel();
        }
        else
        {
            out.pinnedIdx = idx;
            if (out.all[(size_t) idx].hasReading)
            {
                out.primaryIdx = idx;
                out.userSelected = true;
            }
            else
                out.primaryIdx = -1;     // pinned, waiting for a reading
        }
    }

    // Disagreement among sources WITH readings is information (§9).
    if (const auto* p = out.primary())
        for (const auto& s : out.all)
            if (s.hasReading && ! s.poisoned
                && (s.root != p->root || s.minor != p->minor))
                out.disagree = true;

    return out;
}

// Publish the ALREADY-RESOLVED primary into the process-wide KeyFeed that
// EchoJay Pitch (spec §6) follows. The precedence walk stays in
// collectKeySources() and is never re-implemented downstream, so the device,
// the [DETECTED KEY] feed block and the Meters panel can never rank sources
// differently. Runs from the processor's 1 Hz timer — NOT only from the
// editor's — so the corrector keeps tracking the key with the window closed.
void EchoJayProcessor::publishKeyFeed(const KeySources& sources)
{
    echojay::DetectedKeyFact fact;
    if (const auto* p = sources.primary())
    {
        fact.valid      = true;
        fact.root       = p->root;
        fact.minor      = p->minor;
        fact.confidence = p->conf;
        fact.tuningHz   = p->tuningHz > 0.0f ? p->tuningHz : 440.0f;
        fact.ageMs      = p->ageMs;
        fact.fromBus    = p->kind == KeySourceReading::Kind::BusLink
                       || p->kind == KeySourceReading::Kind::SelfBus;
        const auto nm = p->name.isNotEmpty() ? p->name : juce::String ("EchoJay");
        nm.copyToUTF8 (fact.sourceName, (int) sizeof (fact.sourceName));
    }
    echojay::KeyFeed::instance().publish (fact);
}

// ============ Capture System ============

// =============================================================================
//  Stage 1 remote editing: session lifecycle (message thread)
// =============================================================================
struct EchoJayProcessor::EditLeaseTimer : juce::Timer
{
    EchoJayProcessor& p;
    explicit EditLeaseTimer(EchoJayProcessor& proc) : p(proc) {}
    void timerCallback() override { p.renewEditLease(); }
};

void EchoJayProcessor::editBegin(const juce::String& uid, int slot0,
                                 const juce::String& name, const juce::String& fmt,
                                 std::unique_ptr<juce::AudioProcessor> inst,
                                 const juce::String& leaseId)
{
    if (inst == nullptr) return;
    // Prepared BEFORE it is installed: the audio thread must never meet a
    // plugin that has not seen prepareToPlay.
    inst->prepareToPlay(hostSampleRate_ > 0 ? hostSampleRate_ : 44100.0,
                        hostSamplesPerBlock_ > 0 ? hostSamplesPerBlock_ : 512);
    editBuf_.setSize(2, 8192);          // audio-thread capacity, allocated HERE
    editSession_.uid        = uid;
    editSession_.slot0      = slot0;
    editSession_.leaseId    = leaseId;
    editSession_.pluginName = name;
    editSession_.pluginFormat = fmt;
    editSession_.beganMs    = juce::Time::getMillisecondCounter();

    // Which ring slot carries this Link.
    int ringSlot = -1;
    for (int i = 0; i < kMaxLinkSlots; ++i)
        if (activeLinkSlots[i].map != nullptr && activeLinkSlots[i].uid == uid)
            { ringSlot = i; break; }
    if (ringSlot >= 0)
    {
        const juce::SpinLock::ScopedLockType sl(activeLinkSlots[ringSlot].lock);
        if (activeLinkSlots[ringSlot].map != nullptr)
            LinkShm::ringSeekForward(activeLinkSlots[ringSlot].map, kEditCushionFrames);
    }
    editSession_.ringSlot.store(ringSlot, std::memory_order_relaxed);

    {
        const juce::SpinLock::ScopedLockType sl(editLock_);
        editInst_ = std::move(inst);
    }
    editSession_.audioOn.store(true, std::memory_order_release);

    renewEditLease();                   // the file appears NOW, not in 1s
    if (editLeaseTimer_ == nullptr)
        editLeaseTimer_ = std::make_unique<EditLeaseTimer>(*this);
    editLeaseTimer_->startTimer((int) LinkShm::kLeaseRenewMs);
}

void EchoJayProcessor::renewEditLease()
{
    if (!editActive()) return;
    int err = 0;
    const juce::String dir = LinkShm::resolveDir(err);
    if (dir.isEmpty()) return;
    auto* o = new juce::DynamicObject();
    o->setProperty("v",       1);
    o->setProperty("leaseId", editSession_.leaseId);
    o->setProperty("slot",    editSession_.slot0 + 1);   // 1-based on the wire
    o->setProperty("tMs",     juce::Time::currentTimeMillis());
    juce::File(LinkShm::leasePath(dir, editSession_.uid))
        .replaceWithText(juce::JSON::toString(juce::var(o), true));
}

juce::String EchoJayProcessor::editCaptureStateB64()
{
    const juce::SpinLock::ScopedLockType sl(editLock_);
    if (editInst_ == nullptr) return {};
    juce::MemoryBlock mb;
    try { editInst_->getStateInformation(mb); } catch (...) { return {}; }
    return LinkShm::stateToB64(mb);   // ONE codec pairing, see LinkShm.h
}

juce::AudioProcessorEditor* EchoJayProcessor::editCreateEditor()
{
    const juce::SpinLock::ScopedLockType sl(editLock_);
    if (editInst_ == nullptr) return nullptr;
    try { return editInst_->createEditor(); } catch (...) { return nullptr; }
}

void EchoJayProcessor::editEnd(bool keepState)
{
    if (!editActive()) return;
    if (keepState)
    {
        // The lease died under us (the Link won and restored itself). The
        // user's edits must not die with it: capture them so a reopen can
        // seed from here instead of pulling the Link's now-reverted state.
        editSession_.keptStateB64 = editCaptureStateB64();
        editSession_.keptUid      = editSession_.uid;
        editSession_.keptSlot0    = editSession_.slot0;
    }
    if (editLeaseTimer_) editLeaseTimer_->stopTimer();
    int err = 0;
    const juce::String dir = LinkShm::resolveDir(err);
    if (dir.isNotEmpty())
        juce::File(LinkShm::leasePath(dir, editSession_.uid)).deleteFile();

    // Ramp out first, destroy a beat later: the audio thread may be inside
    // the instance right now, and AU views tear timers down in dealloc (the
    // AMEK lesson). audioOn drops the target to 0; the instance leaves the
    // audio path under the lock; destruction happens after the ramp is over.
    editSession_.audioOn.store(false, std::memory_order_release);
    editSession_.uid.clear();
    editSession_.slot0 = -1;
    editSession_.ringSlot.store(-1, std::memory_order_relaxed);

    // Shared_ptr shim so the deferred lambda owns the instance without this
    // processor holding a dangling unique_ptr meanwhile.
    std::shared_ptr<juce::AudioProcessor> dying;
    {
        const juce::SpinLock::ScopedLockType sl(editLock_);
        dying = std::shared_ptr<juce::AudioProcessor>(std::move(editInst_));
    }
    if (dying != nullptr)
        juce::Timer::callAfterDelay(120, [dying]() mutable
        {
            if (dying) { try { dying->releaseResources(); } catch (...) {} }
            dying.reset();
        });
}

void EchoJayProcessor::startCapture()
{
    captureEngine.reset();
    waveformRecorder.startRecording();
    captureStartTime = juce::Time::currentTimeMillis();
    captureSampleCount = 0;
    spectrumPeak.fill(-120.0f);
    spectrumSum.fill(0.0f);
    spectrumFrames = 0;
    
    // Reset capture aggregators
    capPeakL.store(0.0f);
    capPeakR.store(0.0f);
    capSumSqL.store(0.0);
    capSumSqR.store(0.0);
    capGatedSumSqL.store(0.0);
    capGatedSumSqR.store(0.0);
    capTotalSamples.store(0);
    capGatedSamples.store(0);
    capWidthSum.store(0.0);
    capCorrSum.store(0.0);
    capGatedBufCount.store(0);
    capRunningPeakForGate.store(0.0f);
    capMaxMomentary.store(-100.0f);
    capMaxShortTerm.store(-100.0f);
    
    // CAPTURE EXCLUSION (stage 1): capture drains every Link ring and an
    // edit session owns one of them with seeks; both on one single-reader
    // ring would corrupt the capture. Whoever is FIRST wins; the second is
    // refused with a message naming the first (the editor states it).
    if (editActive())
    {
        captureState.store(CaptureState::Idle);
        return;
    }
    captureState.store(CaptureState::Capturing);

    // Snapshot active Link slots for multi-channel capture
    {
        const juce::SpinLock::ScopedLockType sl(linkCaptureSpinLock);
        linkCaptureChannels.clear();
        double sr = hostSampleRate_;
        int    bs = hostSamplesPerBlock_;
        for (int i = 0; i < kMaxLinkSlots; ++i)
        {
            if (activeLinkSlots[i].map != nullptr && activeLinkSlots[i].displayName.isNotEmpty())
            {
                auto lcc = std::make_unique<LinkCaptureChannel>(
                    activeLinkSlots[i].displayName, activeLinkSlots[i].uid, i, sr, bs);
                // Stamp the Link's declared placement from the registry cache
                // (refreshed by the editor timer) — the offline key pass
                // prefers a bus channel over everything else (§5.2).
                for (const auto& si : linkSlotInfos)
                    if (si.uid.isNotEmpty() && si.uid == lcc->uid)
                        { lcc->placement = si.placement; break; }
                linkCaptureChannels.push_back(std::move(lcc));
            }
        }
    }
}

void EchoJayProcessor::stopCapture()
{
    if (captureState.load() != CaptureState::Capturing) return;
    captureState.store(CaptureState::Complete);
    waveformRecorder.stopRecording();
    
    // Compute pass name BEFORE incrementing so the display value matches the counter.
    // Then increment only the counter that was actually used.
    CaptureSnapshot snap;
    snap.id   = juce::String(juce::Time::currentTimeMillis());
    // Item 1: single name source. The editor stamped the chat-revision name
    // at press; use it so the snapshot, the review label and the card cannot
    // diverge. computePassName() (captureVersion) is the fallback only.
    snap.name = nextCaptureName_.isNotEmpty() ? nextCaptureName_ : computePassName();
    snap.channelScopeUid = nextCaptureScopeUid_;   // item 1: robust scope stamp
    nextCaptureName_.clear(); nextCaptureScopeUid_.clear();
    if (projectName.trim().isEmpty())
        passCounter++;               // "Pass N" used → next will be "Pass N+1"
    else
        captureVersion++;
    snap.channelType = channelType;
    snap.customChannelName = customChannelName;
    
    // Start with the meter engine's data — this provides correctly-integrated
    // values that need long buffers (LUFS Integrated, LUFS Range) and the
    // momentary/short-term displays. Then OVERRIDE the per-buffer aggregable
    // values (peak, RMS, crest, width, correlation) with our time-windowed
    // measurements computed across the whole capture.
    snap.averagedData = captureEngine.getMeterData();
    
    {
        // ============ Finalize time-windowed measurements ============
        // Different channel types want different aggregation strategies:
        //   - Mix Bus / Master Bus / Music Bus: full-window RMS (silence is part
        //     of the dynamic story for sustained full-range content)
        //   - Everything else: gated RMS (vocals, drums, instruments naturally
        //     have silence/quiet sections that shouldn't drag down RMS)
        //   - Width and correlation: ALWAYS gated (silence has no stereo info)
        //   - Peak: ALWAYS the absolute max over the capture
        //   - Crest: peak / RMS, where the RMS choice follows the channel type rule
        bool useFullWindowRms = (channelType == ChannelType::FullMix ||
                                  channelType == ChannelType::MasterBus ||
                                  channelType == ChannelType::MusicBus);
        
        long long totalN = capTotalSamples.load();
        long long gatedN = capGatedSamples.load();
        int gatedBufN = capGatedBufCount.load();
        
        // Peak L/R: absolute max over capture (as dBFS)
        float pL = capPeakL.load();
        float pR = capPeakR.load();
        auto toDb = [](float lin) { return lin > 1e-10f ? 20.0f * std::log10(lin) : -100.0f; };
        snap.averagedData.peakL = toDb(pL);
        snap.averagedData.peakR = toDb(pR);
        snap.averagedData.peakMaxL = snap.averagedData.peakL;
        snap.averagedData.peakMaxR = snap.averagedData.peakR;
        
        // RMS L/R: full-window or gated depending on channel type (as dBFS)
        long long rmsN = useFullWindowRms ? totalN : gatedN;
        double sumSqL = useFullWindowRms ? capSumSqL.load() : capGatedSumSqL.load();
        double sumSqR = useFullWindowRms ? capSumSqR.load() : capGatedSumSqR.load();
        if (rmsN > 0)
        {
            double meanSqL = sumSqL / (double)rmsN;
            double meanSqR = sumSqR / (double)rmsN;
            snap.averagedData.rmsL = (float)(meanSqL > 1e-20 ? 10.0 * std::log10(meanSqL) : -100.0);
            snap.averagedData.rmsR = (float)(meanSqR > 1e-20 ? 10.0 * std::log10(meanSqR) : -100.0);
        }
        else
        {
            snap.averagedData.rmsL = -100.0f;
            snap.averagedData.rmsR = -100.0f;
        }
        
        // Crest factor: peak / RMS (in dB, that's peak_dB - rms_dB).
        // Use the louder channel's peak vs the louder channel's RMS for a stable single value.
        float peakDb = std::max(snap.averagedData.peakL, snap.averagedData.peakR);
        float rmsDb = std::max(snap.averagedData.rmsL, snap.averagedData.rmsR);
        if (peakDb > -90.0f && rmsDb > -90.0f)
        {
            float crestDb = peakDb - rmsDb;
            // Sanity clamp — real-world crest is roughly 3-30 dB; outside that
            // suggests a measurement issue and the meter strip should not show
            // impossible values like 128 or 200 dB.
            snap.averagedData.crestFactor = juce::jlimit(0.0f, 40.0f, crestDb);
        }
        else
        {
            snap.averagedData.crestFactor = 0.0f;
        }
        
        // Width and correlation: always gated, averaged over loud-enough buffers
        if (gatedBufN > 0)
        {
            snap.averagedData.width = (float)(capWidthSum.load() / (double)gatedBufN);
            snap.averagedData.correlation = (float)(capCorrSum.load() / (double)gatedBufN);
        }
        // If no buffer passed the gate (mostly silent capture), leave width/corr
        // at whatever the meter engine had as its default — better than zero.

        // Highest momentary / short-term LUFS reached over the capture.
        snap.averagedData.momentaryMax = capMaxMomentary.load();
        snap.averagedData.shortTermMax = capMaxShortTerm.load();
    }
    
    snap.timestamp = juce::Time::currentTimeMillis();
    snap.durationSeconds = (float)(juce::Time::currentTimeMillis() - captureStartTime) / 1000.0f;
    
    // Waveform thumbnail from recorder
    auto thumb = waveformRecorder.getThumbnail();
    for (auto& pt : thumb)
        snap.waveformThumbnail.push_back(std::max(std::abs(pt.maxVal), std::abs(pt.minVal)));
    
    // EQ curve — choose spectrum method based on channel type:
    // - Mix Bus / Master Bus: use AVERAGE (sustained full-frequency content,
    //   representative tonal balance, not skewed by one loud section)
    // - Everything else (including buses like Drum Bus, Vocal Bus): use PEAK-HOLD
    //   (captures actual frequency content without gaps/silence diluting the reading.
    //   Even buses like Drum Bus are fundamentally transient — averaging kills them.)
    bool useAverage = (channelType == ChannelType::FullMix || 
                       channelType == ChannelType::MasterBus ||
                       channelType == ChannelType::MusicBus ||
                       channelType == ChannelType::InstrumentBus);
    
    bool hasPeakData = false;
    for (int i = 0; i < 64; ++i)
        if (spectrumPeak[(size_t)i] > -119.0f) { hasPeakData = true; break; }
    
    // Always preserve BOTH spectra on the snapshot for per-band crest analysis.
    // Per-band crest (peak - avg) is what lets us tell hi-hats from 808s, etc.
    if (hasPeakData)
        snap.peakSpectrum = spectrumPeak;
    if (spectrumFrames > 0) {
        for (int i = 0; i < 64; ++i)
            snap.avgSpectrum[(size_t)i] = spectrumSum[(size_t)i] / (float)spectrumFrames;
    }
    snap.hasDualSpectrum = (hasPeakData && spectrumFrames > 0);
    
    if (!useAverage && hasPeakData) {
        snap.eqCurve = spectrumPeak;
        snap.averagedData.spectrum = spectrumPeak;
    } else if (spectrumFrames > 0) {
        snap.eqCurve = snap.avgSpectrum;
        snap.averagedData.spectrum = snap.eqCurve;
    } else {
        snap.eqCurve = snap.averagedData.spectrum;
    }

    // ── Multi-channel: finalize Link channels ──────────────────────────────
    // Lock to ensure the audio thread has finished its last capture block.
    // captureState is already Complete so audio thread won't re-enter.
    {
        const juce::SpinLock::ScopedLockType sl(linkCaptureSpinLock);
        if (!linkCaptureChannels.empty())
        {
            // Channel 0 = host
            ChannelMeterData hostCh;
            hostCh.name = snap.getChannelDisplayName();
            hostCh.meterData = snap.averagedData;
            snap.channels.push_back(hostCh);
            // Channels 1..N = Links
            for (auto& lcc : linkCaptureChannels)
            {
                lcc->waveformRecorder.stopRecording();
                snap.channels.push_back(finalizeLinkChannel(*lcc, snap.durationSeconds));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshots.push_back(snap);
    }
    
    // Auto-save WAV in background thread (tracked so destructor can wait)
    // Wait for any previous save to finish first
    if (saveThread && saveThread->isThreadRunning())
        saveThread->waitForThreadToExit(5000);
    
    auto passName = snap.name;
    auto captureDir = getCaptureFolder();
    int snapIdx = (int)snapshots.size() - 1;
    auto* recorderPtr = &waveformRecorder;
    auto* mutexPtr = &snapshotMutex;
    auto* snapsPtr = &snapshots;
    
    // Move link channels into save thread (captureState = Complete, audio thread done)
    auto movedLinkChannels = std::move(linkCaptureChannels);

    // ── Offline key pass source (KEY_PRECONDITION_SPEC.md §5.2/§5.3) ──────
    // Decide WHICH recorded channel the key pass may read, before the save
    // thread owns the buffers. The rule is strict because a wrong source is
    // worse than none: a BUS Link channel is the music; the host channel
    // qualifies only when this channel is declared a mix/instrumental bus.
    // A vocal (or any unknown stem) is never analysed for key — monophonic,
    // sliding, often pitch-corrected: confidently wrong.
    int keySrcIdx = -2;                 // -2 none, -1 host, >=0 link channel
    juce::String keySrcName;
    int keySrcPlacement = 0;
    for (size_t i = 0; i < movedLinkChannels.size(); ++i)
        if (movedLinkChannels[i]->placement == 1
            && movedLinkChannels[i]->waveformRecorder.getRecordedSampleCount() > 0)
        {
            keySrcIdx = (int) i;
            keySrcName = movedLinkChannels[i]->name;
            keySrcPlacement = 1;
            break;
        }
    if (keySrcIdx == -2
        && (channelType == ChannelType::FullMix
            || channelType == ChannelType::MasterBus
            || channelType == ChannelType::MusicBus
            || channelType == ChannelType::InstrumentBus))
    {
        keySrcIdx = -1;
        keySrcName = snap.getChannelDisplayName();
        keySrcPlacement = 0;
    }

    struct SaveThread : public juce::Thread
    {
        SaveThread(WaveformRecorder* rec, juce::File dir, juce::String name,
                   int idx, std::mutex* mtx, std::vector<CaptureSnapshot>* snaps,
                   std::vector<std::unique_ptr<LinkCaptureChannel>> lcs,
                   std::function<void()>* onDone,
                   int keySrc, juce::String keyName, int keyPlace)
            : juce::Thread("EchoJay WAV Save"), recorder(rec), captureDir(dir),
              passName(name), snapIdx(idx), mutex(mtx), snapshots(snaps),
              linkChannels(std::move(lcs)), onDoneCb(onDone),
              keySrcIdx(keySrc), keySrcName(std::move(keyName)),
              keySrcPlacement(keyPlace) {}

        void run() override
        {
            // ── Offline key pass (§5.2) — BEFORE any releaseAudioBuffer.
            // The offline path is allowed to be slower and better than the
            // live one: up to three 30 s windows, HPSS on, best confidence
            // wins (~0.5 s of background time for a long capture). Result is
            // written into the snapshot under the mutex; an invalid reading
            // leaves keyValid=false, which the feed reports as absence.
            if (keySrcIdx >= -1)
            {
                WaveformRecorder* srcRec =
                    keySrcIdx < 0 ? recorder
                                  : &linkChannels[(size_t) keySrcIdx]->waveformRecorder;
                const auto* buf = srcRec->getRecordedBuffer();
                const int   n   = srcRec->getRecordedSampleCount();
                if (buf != nullptr && n > (int) (2.0 * srcRec->getRecordedSampleRate()))
                {
                    echojay::KeyEngine eng;
                    eng.prepare(srcRec->getRecordedSampleRate(), 512);
                    const auto kr = eng.analyseBufferOffline(
                        buf->getReadPointer(0),
                        buf->getNumChannels() > 1 ? buf->getReadPointer(1) : nullptr,
                        std::min(n, buf->getNumSamples()));
                    if (kr.valid)
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        if (snapIdx >= 0 && snapIdx < (int)snapshots->size())
                        {
                            auto& s = (*snapshots)[(size_t)snapIdx];
                            s.keyValid       = true;
                            s.keyRoot        = kr.root;
                            s.keyMinor       = kr.minor;
                            s.keyConfidence  = kr.confidence;
                            s.keyTuningHz    = kr.tuningHz;
                            s.keyTuningCents = kr.tuningCents;
                            s.keyChroma      = kr.chroma;
                            if (kr.numAlternates > 0)
                            {
                                s.keyAltRoot  = kr.alternates[0].root;
                                s.keyAltMinor = kr.alternates[0].minor;
                                s.keyAltScore = kr.alternates[0].score;
                            }
                            s.keySourceName      = keySrcName;
                            s.keySourcePlacement = keySrcPlacement;
                        }
                        char nm[24];
                        echojay::KeyEngine::keyName(kr.root, kr.minor, nm, sizeof(nm));
                        EchoJay_NSLog(("EJCapture: offline key pass -> "
                                       + juce::String(nm) + " conf "
                                       + juce::String(kr.confidence, 2)
                                       + " from \"" + keySrcName + "\"").toRawUTF8());
                    }
                    else
                        EchoJay_NSLog("EJCapture: offline key pass found nothing tonal");
                }
            }

            // Host WAV
            recorder->saveToWAV(captureDir, passName);
            auto hostPath = recorder->getLastSavedPath();
            // Free the capture audio NOW, write succeeded or not — nothing
            // references it after this point (playback plays the WAV file,
            // the display uses the thumbnail). Holding it until the next
            // capture was the per-pass RSS retention.
            recorder->releaseAudioBuffer();
            if (hostPath.isNotEmpty())
            {
                std::lock_guard<std::mutex> lock(*mutex);
                if (snapIdx >= 0 && snapIdx < (int)snapshots->size())
                {
                    (*snapshots)[(size_t)snapIdx].wavFilePath = hostPath;
                    if (!(*snapshots)[(size_t)snapIdx].channels.empty())
                        (*snapshots)[(size_t)snapIdx].channels[0].wavFilePath = hostPath;
                }
            }
            // Per-Link WAVs — release each channel's audio right after its
            // write so peak memory during multi-Link saves stays one
            // channel's worth
            for (size_t i = 0; i < linkChannels.size(); ++i)
            {
                auto& lcc = linkChannels[i];
                lcc->waveformRecorder.saveToWAV(captureDir, passName + " - " + lcc->name);
                auto lp = lcc->waveformRecorder.getLastSavedPath();
                lcc->waveformRecorder.releaseAudioBuffer();
                if (lp.isNotEmpty())
                {
                    std::lock_guard<std::mutex> lock(*mutex);
                    if (snapIdx >= 0 && snapIdx < (int)snapshots->size())
                    {
                        auto& chs = (*snapshots)[(size_t)snapIdx].channels;
                        size_t ci = i + 1;   // channel 0 = host
                        if (ci < chs.size())
                            chs[ci].wavFilePath = lp;
                    }
                }
            }
            // Drop the Link channels HERE, not in the destructor: this
            // Thread object stays alive as the saveThread member until the
            // NEXT capture replaces it, and it must not keep the channels
            // (and whatever they own) alive for that whole time.
            linkChannels.clear();

            // Handshake completion (step b): every WAV written and the
            // snapshot's channel paths populated under mutex. Notify the
            // message thread so a channel review can adopt its WAV filename.
            // callAsync copies the std::function* by value; the processor
            // outlives the save thread, so the wire is safe to read there.
            std::function<void()>* cb = onDoneCb;
            juce::MessageManager::callAsync([cb]()
            {
                if (cb && *cb) (*cb)();
            });
        }

        WaveformRecorder* recorder;
        juce::File captureDir;
        juce::String passName;
        int snapIdx;
        std::mutex* mutex;
        std::vector<CaptureSnapshot>* snapshots;
        std::vector<std::unique_ptr<LinkCaptureChannel>> linkChannels;
        std::function<void()>* onDoneCb;
        int keySrcIdx;
        juce::String keySrcName;
        int keySrcPlacement;
    };

    saveThread = std::make_unique<SaveThread>(recorderPtr, captureDir, passName, snapIdx, mutexPtr, snapsPtr,
                                               std::move(movedLinkChannels), &onCaptureSaveComplete,
                                               keySrcIdx, keySrcName, keySrcPlacement);
    saveThread->startThread();

#if ECHOJAY_MEMDIAG
    {
        size_t rssBytes = 0;
       #if JUCE_MAC
        mach_task_basic_info info;
        mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&info, &cnt) == KERN_SUCCESS)
            rssBytes = (size_t)info.resident_size;
       #endif
        size_t chatBytes = 0;
        for (auto& c : chatContents) chatBytes += c.getNumBytesAsUTF8();
        EchoJay_NSLog(("EJMemDiag: post-capture rss="
            + juce::String((juce::int64)(rssBytes / (1024 * 1024))) + "MB"
            + " recorderBuf=" + juce::String((juce::int64)(waveformRecorder.getAllocatedBytes() / 1024)) + "KB"
            + " snapshots=" + juce::String((int)snapshots.size())
            + " chatMsgs=" + juce::String(chatContents.size())
            + " chatBytes=" + juce::String((juce::int64)chatBytes)
            + " saveRunning=" + juce::String((saveThread && saveThread->isThreadRunning()) ? 1 : 0)).toRawUTF8());
    }
#endif

    autoFeedbackReady.store(true);
}

void EchoJayProcessor::resetCapture()
{
    captureState.store(CaptureState::Idle);
    captureEngine.reset();
    waveformRecorder.reset();
}

float EchoJayProcessor::getCaptureDuration() const
{
    if (captureState.load() != CaptureState::Capturing) return 0.0f;
    return (float)(juce::Time::currentTimeMillis() - captureStartTime) / 1000.0f;
}

std::vector<CaptureSnapshot> EchoJayProcessor::getSnapshots() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return snapshots;
}

void EchoJayProcessor::setNextCapture(const juce::String& name, const juce::String& scopeUid)
{
    nextCaptureName_     = name;
    nextCaptureScopeUid_ = scopeUid;
}

CaptureSnapshot EchoJayProcessor::getLatestSnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (snapshots.empty()) return {};
    return snapshots.back();
}

int EchoJayProcessor::getSnapshotCount() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return (int)snapshots.size();
}

// ============ WAV Save ============

juce::File EchoJayProcessor::getCaptureFolder() const
{
    // Try to use the DAW project folder first, fall back to Documents/EchoJay/Captures
    juce::File projectDir;

    // JUCE doesn't give us the DAW project folder directly, so use
    // a subfolder next to wherever the plugin state file would be saved.
    // Fallback: ~/Documents/EchoJay/Captures
    projectDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                     .getChildFile("EchoJay")
                     .getChildFile("Captures");
    return projectDir;
}

juce::String EchoJayProcessor::saveCaptureWAV()
{
    auto snaps = getSnapshots();
    juce::String passName = snaps.empty() ? "Capture" : snaps.back().name;
    return waveformRecorder.saveToWAV(getCaptureFolder(), passName);
}

// ============ Compare Context Builders ============

namespace {
    // ONE derivation of the compare figures, shared by the model's text table
    // (figBlock) and the client-rendered figure card (buildCompareFiguresJson),
    // so a visual can never disagree with the numbers the model reasons from.
    // Sentinels are preserved: an unavailable reading stays at its sentinel
    // (int/tp -100, lra 0, psr/plr/bandRel -999, overs -1) and renders/serialises
    // as N/A, never a fabricated zero.
    struct CompareFig {
        float integrated = -100.0f, lra = 0.0f, tp = -100.0f, psr = -999.0f,
              plr = -999.0f, crest = 0.0f, width = 0.0f, corr = 0.0f;
        int   overs = -1;
        std::array<float, 6> bandRel = { -999, -999, -999, -999, -999, -999 };
        bool  bandValid = false;
    };
    CompareFig computeCompareFig(const MeterData& m)
    {
        CompareFig f;
        f.integrated = m.integrated;
        f.lra        = m.loudnessRange;
        f.tp = juce::jmax(m.truePeakMaxL, m.truePeakMaxR);
        if (f.tp <= -99.0f) f.tp = juce::jmax(m.truePeakL, m.truePeakR);
        f.psr = (m.psr > -99.0f) ? m.psr
              : (m.shortTermTruePeak > -99.0f && m.shortTerm > -99.0f)
                    ? (m.shortTermTruePeak - m.shortTerm) : -999.0f;
        f.plr = (m.plr > -99.0f) ? m.plr
              : (f.tp > -99.0f && m.integrated > -99.0f) ? (f.tp - m.integrated) : -999.0f;
        f.crest = m.crestFactor;
        f.width = m.width;
        f.corr  = m.correlation;
        f.overs = m.oversCount;
        float sum = 0.0f; int n = 0;
        for (float v : m.macroBandDb) if (v > -119.0f) { sum += v; ++n; }
        if (n > 0)
        {
            const float mean = sum / (float)n;
            f.bandValid = true;
            for (int i = 0; i < 6; ++i)
                f.bandRel[(size_t)i] = m.macroBandDb[(size_t)i] > -119.0f
                    ? m.macroBandDb[(size_t)i] - mean : -999.0f;
        }
        return f;
    }

    // Aggregate 64 log-spaced spectrum bins (20Hz–20kHz) into 6 musical bands.
    // Bins are already in dB. We average in the linear (power) domain to avoid
    // log-domain skew, then convert back to dB.
    // Band boundaries (bin indices, inclusive):
    //   Sub      20–60 Hz   bins  0–9
    //   Low      60–200 Hz  bins 10–20
    //   Low-mid  200–600 Hz bins 21–30
    //   Mid      600–2k Hz  bins 31–41
    //   High-mid 2k–6k Hz   bins 42–52
    //   High     6k–20k Hz  bins 53–63
    struct BandLevels { float sub, low, lowMid, mid, highMid, high; };
    
    inline float avgDb(const std::array<float, 64>& s, int lo, int hi)
    {
        double sumLin = 0.0;
        int n = 0;
        for (int i = lo; i <= hi; ++i) {
            double db = (double)s[(size_t)i];
            if (db < -100.0) db = -100.0; // clamp floor
            sumLin += std::pow(10.0, db / 10.0);
            ++n;
        }
        if (n == 0 || sumLin <= 1e-20) return -100.0f;
        return (float)(10.0 * std::log10(sumLin / (double)n));
    }
    
    BandLevels computeBands(const std::array<float, 64>& s)
    {
        return {
            avgDb(s,  0,  9),
            avgDb(s, 10, 20),
            avgDb(s, 21, 30),
            avgDb(s, 31, 41),
            avgDb(s, 42, 52),
            avgDb(s, 53, 63)
        };
    }
    
    // Append plain-language tonal diff lines. Only flags bands where the
    // difference exceeds 2 dB — below that is noise. "Your mix has more/less X"
    // is phrased from the user's perspective relative to the reference.
    void appendTonalDiff(juce::String& ctx,
                         const std::array<float, 64>& mixSpec,
                         const std::array<float, 64>& refSpec,
                         const juce::String& mixLabel,
                         const juce::String& refLabel)
    {
        auto mb = computeBands(mixSpec);
        auto rb = computeBands(refSpec);
        
        struct BandDiff { const char* name; float mix; float ref; };
        BandDiff diffs[6] = {
            { "sub (below 60Hz)",         mb.sub,     rb.sub     },
            { "lows (60-200Hz)",          mb.low,     rb.low     },
            { "low-mids (200-600Hz)",     mb.lowMid,  rb.lowMid  },
            { "mids (600Hz-2kHz)",        mb.mid,     rb.mid     },
            { "high-mids (2-6kHz)",       mb.highMid, rb.highMid },
            { "highs (above 6kHz)",       mb.high,    rb.high    }
        };
        
        // Check if mix has any signal at all — if floor everywhere, skip
        bool mixHasSignal = false, refHasSignal = false;
        for (auto& d : diffs) {
            if (d.mix > -80.0f) mixHasSignal = true;
            if (d.ref > -80.0f) refHasSignal = true;
        }
        if (!mixHasSignal || !refHasSignal) {
            ctx += "TONAL BALANCE: Not enough signal to compare frequency content.\n";
            return;
        }
        
        // Normalise both spectra by their loudest band so overall-level
        // differences (already covered by LUFS) don't dominate the tonal diff.
        float mixMax = -200.0f, refMax = -200.0f;
        for (auto& d : diffs) {
            if (d.mix > mixMax) mixMax = d.mix;
            if (d.ref > refMax) refMax = d.ref;
        }
        
        juce::String tonalLines;
        int flagged = 0;
        for (auto& d : diffs) {
            float mixRel = d.mix - mixMax;
            float refRel = d.ref - refMax;
            float delta = mixRel - refRel; // positive = mix has more in this band
            if (std::abs(delta) >= 2.0f) {
                juce::String line = "- ";
                if (delta > 0)
                    line += mixLabel + " has more " + d.name + " than " + refLabel
                          + " (+" + juce::String(delta, 1) + " dB relative)";
                else
                    line += mixLabel + " has less " + d.name + " than " + refLabel
                          + " (" + juce::String(delta, 1) + " dB relative)";
                line += "\n";
                tonalLines += line;
                ++flagged;
            }
        }
        
        if (flagged == 0) {
            ctx += "TONAL BALANCE: Very similar across the frequency range - no notable band differences.\n";
        } else {
            ctx += "TONAL BALANCE DIFFERENCES (relative, already normalised for overall level):\n";
            ctx += tonalLines;
        }
    }
}

juce::String EchoJayProcessor::buildCompareContext(const CaptureSnapshot& capture, const ReferenceResult& reference) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& a = capture.averagedData;
    auto& b = reference.data;
    
    juce::String ctx;
    ctx += "[BEGIN COMPARE CONTEXT - this block is a one-off comparison, NOT an ongoing mix discussion]\n";
    ctx += "[AI COMPARE REQUEST: Your mix (" + capture.name + ") vs Reference (" + reference.name + ")]\n\n";
    ctx += "YOUR MIX: Int " + ff(a.integrated) + " LUFS | Crest " + juce::String(a.crestFactor, 1) + " dB";
    if (a.width < 10.0f || a.width > 55.0f) ctx += " | Width " + juce::String(a.width, 1) + "%";
    ctx += "\nREFERENCE: Int " + ff(b.integrated) + " LUFS | Crest " + juce::String(b.crestFactor, 1) + " dB";
    if (b.width < 10.0f || b.width > 55.0f) ctx += " | Width " + juce::String(b.width, 1) + "%";
    
    // Only flag meaningful differences
    ctx += "\n\nKEY DIFFERENCES (only mention if significant):\n";
    float lufsDiff = b.integrated - a.integrated;
    if (std::abs(lufsDiff) > 1.5f)
        ctx += "- Loudness: " + juce::String(lufsDiff, 1) + " dB difference\n";
    float crestDiff = b.crestFactor - a.crestFactor;
    if (std::abs(crestDiff) > 2.0f)
        ctx += "- Dynamics: Crest differs by " + juce::String(crestDiff, 1) + " dB\n";
    float widthDiff = b.width - a.width;
    if (std::abs(widthDiff) > 15.0f)
        ctx += "- Width: " + juce::String(widthDiff, 1) + "% difference\n";
    
    // Tonal balance — aggregated spectrum bands, both have spectrum data available.
    // Note: reference spectrum lives in reference.eqCurve (averaged across full file),
    // not reference.data.spectrum (which is just the last frame's snapshot).
    ctx += "\n";
    appendTonalDiff(ctx, a.spectrum, reference.eqCurve, "your mix", "the reference");
    
    ctx += "\nINSTRUCTIONS: Only comment on differences that are genuinely significant. Small variations (< 1.5 LUFS, < 2dB crest, < 15% width) are normal and should be described as practically the same. Width is not a reliable metric - only flag if the difference is drastic. For tonal balance, speak in plain language ('your mix is heavier in the low end', 'the reference has more air on top') - do NOT quote dB values or band names like '200-600Hz' to the user. Focus on what the user should actually do differently to get closer to the reference. Be concise - 2-3 paragraphs max.\n";
    
    // Length-based caveats — comparing a snippet against a full track (or vice
    // versa) skews tonal balance and dynamics readings, so the AI must mention this.
    float capDur = capture.durationSeconds;
    float refDur = reference.durationSeconds;
    if (capDur > 0 && refDur > 0)
    {
        float ratio = (capDur > refDur) ? (capDur / refDur) : (refDur / capDur);
        bool eitherShort = (capDur < 30.0f) || (refDur < 30.0f);
        if (eitherShort && ratio > 2.5f)
        {
            ctx += "[LENGTH MISMATCH: Your capture is " + juce::String((int)capDur) + "s, the reference is " 
                + juce::String((int)refDur) + "s. Open the response by telling the user this comparison may be misleading "
                "because one is a short snippet and one is a full track - tonal balance especially can read very differently. "
                "Suggest they capture a longer section (ideally a full chorus and verse) for a more accurate comparison. "
                "Then proceed with the comparison but keep caveats in mind.]\n";
        }
        else if (capDur < 30.0f)
        {
            ctx += "[CAPTURE LENGTH: Your capture is only " + juce::String((int)capDur) + "s - mention briefly that the "
                "comparison is based on a short snippet and a longer capture would give a fuller picture, "
                "but proceed with the comparison.]\n";
        }
    }
    
    ctx += "[END COMPARE CONTEXT]\n";
    ctx += "[PERSISTENT NOTE: If the user later captures a new mix and asks about it, DO NOT treat the numbers in the compare block above as a previous version of that new mix. The compare block is a snapshot of this specific comparison, not part of an ongoing capture history. New captures have their own CURRENT MIX data - use only that for new-capture analysis.]\n";
    
    return ctx;
}

juce::String EchoJayProcessor::buildCompareContext(const CaptureSnapshot& a, const CaptureSnapshot& b) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& da = a.averagedData;
    auto& db = b.averagedData;
    
    juce::String ctx;
    ctx += "[BEGIN COMPARE CONTEXT - this block is a one-off comparison, NOT an ongoing mix discussion]\n";
    ctx += "[AI COMPARE REQUEST: " + a.name + " vs " + b.name + "]\n\n";
    ctx += a.name + ": Int " + ff(da.integrated) + " LUFS | Crest " + juce::String(da.crestFactor, 1) + " dB";
    if (da.width < 10.0f || da.width > 55.0f) ctx += " | Width " + juce::String(da.width, 1) + "%";
    ctx += "\n" + b.name + ": Int " + ff(db.integrated) + " LUFS | Crest " + juce::String(db.crestFactor, 1) + " dB";
    if (db.width < 10.0f || db.width > 55.0f) ctx += " | Width " + juce::String(db.width, 1) + "%";
    
    // Only flag meaningful differences
    ctx += "\n\nKEY DIFFERENCES (only mention if significant):\n";
    float lufsDiff = db.integrated - da.integrated;
    if (std::abs(lufsDiff) > 1.5f)
        ctx += "- Loudness: " + juce::String(lufsDiff, 1) + " dB difference\n";
    else
        ctx += "- Loudness: practically the same\n";
    float crestDiff = db.crestFactor - da.crestFactor;
    if (std::abs(crestDiff) > 2.0f)
        ctx += "- Dynamics: Crest differs by " + juce::String(crestDiff, 1) + " dB\n";
    else
        ctx += "- Dynamics: practically the same\n";
    float widthDiff = db.width - da.width;
    if (std::abs(widthDiff) > 15.0f)
        ctx += "- Width: " + juce::String(widthDiff, 1) + "% difference\n";
    
    // Tonal balance — aggregated spectrum bands
    ctx += "\n";
    appendTonalDiff(ctx, da.spectrum, db.spectrum, a.name, b.name);
    
    ctx += "\nINSTRUCTIONS: Only comment on differences that are genuinely significant. Small variations (< 1.5 LUFS, < 2dB crest, < 15% width) are normal measurement noise and should be described as practically the same - do NOT suggest changes for metrics that haven't meaningfully changed. Width is not reliable enough to suggest changes unless the difference is drastic (> 15%). For tonal balance, speak in plain language ('more low end', 'brighter on top') - do NOT quote dB values or band names like '200-600Hz' to the user. If the passes are essentially the same, say so and ask what they changed or what they're trying to achieve. Be concise - 2-3 paragraphs max.\n";
    
    // Length-based caveats
    float aDur = a.durationSeconds, bDur = b.durationSeconds;
    if (aDur > 0 && bDur > 0)
    {
        float ratio = (aDur > bDur) ? (aDur / bDur) : (bDur / aDur);
        bool eitherShort = (aDur < 30.0f) || (bDur < 30.0f);
        if (eitherShort && ratio > 2.5f)
        {
            ctx += "[LENGTH MISMATCH: " + a.name + " is " + juce::String((int)aDur) + "s, " + b.name + " is " 
                + juce::String((int)bDur) + "s. Open by telling the user this comparison may be misleading because "
                "one capture is a short snippet and the other is much longer - tonal balance especially won't read accurately. "
                "Suggest they capture matching sections for a fairer comparison, then proceed with the comparison.]\n";
        }
        else if (aDur < 30.0f && bDur < 30.0f)
        {
            ctx += "[SHORT CAPTURES: Both passes are under 30s - mention briefly that short snippets can give a partial "
                "picture, but proceed with the comparison.]\n";
        }
    }
    
    ctx += "[END COMPARE CONTEXT]\n";
    ctx += "[PERSISTENT NOTE: If the user later captures a new pass and asks about it, DO NOT treat the numbers in the compare block above as a previous version of that new capture. This compare is a snapshot of two specific passes at one moment. New captures have their own CURRENT MIX data - use only that for new-capture analysis.]\n";
    
    return ctx;
}

juce::String EchoJayProcessor::buildCompareContext(const MeterData& da, const MeterData& db,
                                                   const juce::String& la, const juce::String& lb,
                                                   float durA, float durB, bool numbersOnly) const
{
    juce::String ctx;
    ctx += "[BEGIN COMPARE CONTEXT - this block is a one-off comparison, NOT an ongoing mix discussion]\n";
    ctx += "[AI COMPARE REQUEST: " + la + " vs " + lb + "]\n\n";

    // The two modes differ ONLY in the instruction below; the DATA is
    // identical. figBlock emits the full per-source figure set the pipeline
    // carries, each with an N/A fallback so a figure a source never measured
    // reads as N/A rather than a fabricated zero. (LRA 0 = unavailable per the
    // meter convention; band crest / overs / PSR / PLR / macro bands carry
    // their own -1 / -999 / -120 sentinels.)
    auto na1 = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto figBlock = [&](const juce::String& label, const MeterData& m)
    {
        const CompareFig f = computeCompareFig(m);   // SAME values the card renders
        auto bc = [](float v) { return v >= 0.0f ? juce::String(v, 1) : juce::String("N/A"); };
        juce::String s;
        s += label + ":\n";
        s += "  Integrated: " + na1(f.integrated) + " LUFS\n";
        s += "  LRA: " + (f.lra > 0.0f ? juce::String(f.lra, 1) + " LU" : juce::String("N/A")) + "\n";
        s += "  True peak: " + na1(f.tp) + " dBTP\n";
        s += "  PSR: " + (f.psr > -99.0f ? juce::String(f.psr, 1) + " dB" : juce::String("N/A")) + "\n";
        s += "  PLR: " + (f.plr > -99.0f ? juce::String(f.plr, 1) + " dB" : juce::String("N/A")) + "\n";
        s += "  Crest: " + juce::String(f.crest, 1) + " dB\n";
        s += "  Width: " + juce::String(f.width, 1) + " %\n";
        s += "  Correlation: " + juce::String(f.corr, 2) + "\n";
        s += "  Inter-sample overs: " + (f.overs >= 0 ? juce::String(f.overs) : juce::String("N/A")) + "\n";
        // Band crest (text only; not a card family) stays sourced from m.
        s += "  Band crest (low/mid/high): " + bc(m.bandCrestSub) + " / " + bc(m.bandCrestMid)
           + " / " + bc(m.bandCrestTop) + " dB\n";
        // Band relatives: each pink-referenced octave band vs the source's own
        // band average - the tonal-balance figures the analysis prose quotes.
        if (f.bandValid)
        {
            auto rel = [&](int i) {
                return f.bandRel[(size_t)i] > -99.0f
                    ? juce::String(f.bandRel[(size_t)i], 1) : juce::String("N/A");
            };
            s += "  Band relatives vs avg (sub/low/low-mid/mid/high-mid/air): "
               + rel(0) + " / " + rel(1) + " / " + rel(2) + " / " + rel(3) + " / "
               + rel(4) + " / " + rel(5) + " dB\n";
        }
        else
            s += "  Band relatives vs avg: N/A\n";
        return s;
    };

    ctx += "METER FIGURES (these are ALREADY displayed to the user in a figure card - "
           "here for YOUR reference; do NOT restate them):\n";
    ctx += figBlock(la, da);
    ctx += figBlock(lb, db);

    if (numbersOnly)
    {
        ctx += "\nThe figures above are already shown to the user in the card. Do NOT restate "
               "any of them. Reply with ONLY a single short sentence: that \"" + la + "\" and \""
               + lb + "\" are DIFFERENT sources, not two versions of the same audio. No narrative, "
               "no interpretation, no chain.\n";

        if (durA > 0 && durB > 0)
        {
            const float ratio = (durA > durB) ? (durA / durB) : (durB / durA);
            const bool eitherShort = (durA < 30.0f) || (durB < 30.0f);
            if (eitherShort && ratio > 2.5f)
                ctx += "[LENGTH MISMATCH: " + la + " is " + juce::String((int)durA) + "s, " + lb
                     + " is " + juce::String((int)durB) + "s - the two cover very different "
                       "amounts of audio; note that alongside the figures.]\n";
        }

        ctx += "[END COMPARE CONTEXT]\n";
        return ctx;
    }

    // Prose (narrative) mode: SAME figures, free to interpret. A significance
    // summary and the band-difference narrative help it lead with what matters.
    ctx += "\nKEY DIFFERENCES (only mention if significant):\n";
    const float lufsDiff = db.integrated - da.integrated;
    ctx += std::abs(lufsDiff) > 1.5f
        ? ("- Loudness: " + juce::String(lufsDiff, 1) + " dB difference\n")
        : juce::String("- Loudness: practically the same\n");
    const float crestDiff = db.crestFactor - da.crestFactor;
    ctx += std::abs(crestDiff) > 2.0f
        ? ("- Dynamics: Crest differs by " + juce::String(crestDiff, 1) + " dB\n")
        : juce::String("- Dynamics: practically the same\n");
    const float widthDiff = db.width - da.width;
    if (std::abs(widthDiff) > 15.0f)
        ctx += "- Width: " + juce::String(widthDiff, 1) + "% difference\n";

    ctx += "\n";
    appendTonalDiff(ctx, da.spectrum, db.spectrum, la, lb);

    ctx += "\nINSTRUCTIONS: The figures above are ALREADY shown to the user in a figure card, "
           "so do NOT restate them - no tables, no lists of numbers. Interpret only: what the "
           "differences mean, what to check, and what to do. Comment only on differences that "
           "are genuinely significant; small variations (< 1.5 LUFS, < 2dB crest, < 15% width) "
           "are normal measurement noise. For tonal balance, speak in plain language ('more low "
           "end', 'brighter on top') - do NOT quote dB values or band names. Be concise - 2-3 "
           "paragraphs max.\n";

    if (durA > 0 && durB > 0)
    {
        const float ratio = (durA > durB) ? (durA / durB) : (durB / durA);
        const bool eitherShort = (durA < 30.0f) || (durB < 30.0f);
        if (eitherShort && ratio > 2.5f)
            ctx += "[LENGTH MISMATCH: " + la + " is " + juce::String((int)durA) + "s, " + lb
                 + " is " + juce::String((int)durB) + "s - tonal balance especially will not read "
                   "accurately across such different lengths; say so, then proceed.]\n";
    }

    ctx += "[END COMPARE CONTEXT]\n";
    return ctx;
}

juce::String EchoJayProcessor::buildCompareFiguresJson(const MeterData& da, const MeterData& db,
                                                       const juce::String& la, const juce::String& lb,
                                                       bool crossScope) const
{
    // The figure CARD's data - built client-side at compose time from the two
    // MeterData structs, NOT from anything the model returns (a visual that
    // disagreed with the measurement is the failure class this sequence closes).
    // Uses the SAME computeCompareFig as the model's text table. Each figure is
    // written ONLY when present, so an unavailable reading is ABSENT in the JSON
    // and the card draws N/A - never a fabricated zero. cross:true marks a
    // cross-scope pairing (different sources) so the card draws no delta.
    auto src = [](const juce::String& label, const CompareFig& f)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("label", label);
        if (f.integrated > -99.0f) o->setProperty("int",   f.integrated);
        if (f.lra        >   0.0f) o->setProperty("lra",   f.lra);
        if (f.tp         > -99.0f) o->setProperty("tp",    f.tp);
        if (f.psr        > -99.0f) o->setProperty("psr",   f.psr);
        if (f.plr        > -99.0f) o->setProperty("plr",   f.plr);
        o->setProperty("crest", f.crest);   // always measured for a real capture
        o->setProperty("width", f.width);
        o->setProperty("corr",  f.corr);
        if (f.overs >= 0)          o->setProperty("overs", f.overs);
        if (f.bandValid)
        {
            juce::Array<juce::var> b;
            for (int i = 0; i < 6; ++i) b.add(f.bandRel[(size_t)i]);   // -999 = that band N/A
            o->setProperty("bands", b);
        }
        return juce::var(o);
    };
    auto* root = new juce::DynamicObject();
    root->setProperty("a", src(la, computeCompareFig(da)));
    root->setProperty("b", src(lb, computeCompareFig(db)));
    if (crossScope) root->setProperty("cross", true);
    return juce::JSON::toString(juce::var(root), true);
}

juce::String EchoJayProcessor::buildCompareContext(const ReferenceResult& a, const ReferenceResult& b) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& da = a.data;
    auto& db = b.data;
    
    juce::String ctx;
    ctx += "[BEGIN COMPARE CONTEXT - this block is a one-off comparison of REFERENCE tracks, NOT the user's mix]\n";
    ctx += "[AI COMPARE REQUEST: Reference A (" + a.name + ") vs Reference B (" + b.name + ")]\n\n";
    ctx += a.name + ": Int " + ff(da.integrated) + " LUFS | Crest " + juce::String(da.crestFactor, 1) + " dB";
    if (da.width < 10.0f || da.width > 55.0f) ctx += " | Width " + juce::String(da.width, 1) + "%";
    ctx += "\n" + b.name + ": Int " + ff(db.integrated) + " LUFS | Crest " + juce::String(db.crestFactor, 1) + " dB";
    if (db.width < 10.0f || db.width > 55.0f) ctx += " | Width " + juce::String(db.width, 1) + "%";
    
    // Only flag meaningful differences
    ctx += "\n\nKEY DIFFERENCES (only mention if significant):\n";
    float lufsDiff = db.integrated - da.integrated;
    if (std::abs(lufsDiff) > 1.5f)
        ctx += "- Loudness: " + juce::String(lufsDiff, 1) + " dB difference\n";
    else
        ctx += "- Loudness: practically the same\n";
    float crestDiff = db.crestFactor - da.crestFactor;
    if (std::abs(crestDiff) > 2.0f)
        ctx += "- Dynamics: Crest differs by " + juce::String(crestDiff, 1) + " dB\n";
    else
        ctx += "- Dynamics: practically the same\n";
    float widthDiff = db.width - da.width;
    if (std::abs(widthDiff) > 15.0f)
        ctx += "- Width: " + juce::String(widthDiff, 1) + "% difference\n";
    
    // Tonal balance — averaged spectrum from eqCurve (not data.spectrum, which is
    // just the last frame's snapshot for references)
    ctx += "\n";
    appendTonalDiff(ctx, a.eqCurve, b.eqCurve, a.name, b.name);
    
    ctx += "\nINSTRUCTIONS: The user is comparing two reference tracks (not their own mix) - this is usually to understand what separates two sounds they like, or to pick which one to aim for. Focus on what's genuinely different between the two. For tonal balance, speak in plain language ('more low end', 'brighter on top') - do NOT quote dB values or band names like '200-600Hz'. Do NOT suggest 'changes the user should make' since these aren't their mixes. Be concise - 2-3 paragraphs max.\n";
    
    // Length-based caveats — references are usually full tracks but worth checking
    float aDur = a.durationSeconds, bDur = b.durationSeconds;
    if (aDur > 0 && bDur > 0)
    {
        float ratio = (aDur > bDur) ? (aDur / bDur) : (bDur / aDur);
        if (((aDur < 30.0f) || (bDur < 30.0f)) && ratio > 2.5f)
        {
            ctx += "[LENGTH MISMATCH: " + a.name + " is " + juce::String((int)aDur) + "s, " + b.name + " is " 
                + juce::String((int)bDur) + "s. Mention briefly that a short clip vs a full track will skew tonal-balance "
                "comparisons, then proceed.]\n";
        }
    }
    
    ctx += "[END COMPARE CONTEXT]\n";
    ctx += "[PERSISTENT NOTE: The two tracks above are REFERENCE tracks - not the user's own mix. If the user later captures their mix and asks about it, DO NOT treat these reference numbers as a previous version of their capture. Reference tracks and user captures are separate things.]\n";
    
    return ctx;
}

void EchoJayProcessor::renameSnapshot(int index, const juce::String& newName)
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (index >= 0 && index < (int)snapshots.size())
        snapshots[(size_t)index].name = newName;
}

void EchoJayProcessor::deleteSnapshot(int index)
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (index >= 0 && index < (int)snapshots.size())
        snapshots.erase(snapshots.begin() + index);
}

// ============ A/B Playback ============

void EchoJayProcessor::loadABFile(const juce::String& wavPath, double startOffsetSeconds)
{
    juce::File file(wavPath);
    if (!file.existsAsFile()) return;
    
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (!reader) return;
    
    // Read entire file into buffer
    juce::AudioBuffer<float> newBuf((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read(&newBuf, 0, (int)reader->lengthInSamples, 0, true, true);
    
    // Calculate start position from offset
    int startPos = (int)(startOffsetSeconds * reader->sampleRate);
    if (startPos >= (int)reader->lengthInSamples)
        startPos = 0;
    
    {
        std::lock_guard<std::mutex> lock(abMutex);
        abBuffer = std::move(newBuf);
        abSampleCount = abBuffer.getNumSamples();
        abSampleRate = reader->sampleRate;
        abPlaybackPos = startPos;
        abFilePath = wavPath;
    }
    
    abActive.store(true);
    abPlayingRef.store(true);
}

void EchoJayProcessor::stopAB()
{
    abActive.store(false);
    abPlayingRef.store(false);
    abPaused.store(false);
    abPlaybackPos = 0;
}

void EchoJayProcessor::pauseAB()
{
    // Pause: stop outputting ref but keep position
    abPlayingRef.store(false);
    abPaused.store(true);
    abPausedByTransport.store(false); // user-initiated pause
}

void EchoJayProcessor::resumeAB()
{
    // Resume: start outputting ref from where we paused
    if (abActive.load() && abPaused.load()) {
        abPlayingRef.store(true);
        abPaused.store(false);
        abPausedByTransport.store(false);
    }
}

// ============ Compare dual-stream playback ============

void EchoJayProcessor::loadCompareFile(int slot, const juce::String& wavPath)
{
    if (slot < 0 || slot > 1) return;
    juce::File file(wavPath);
    if (!file.existsAsFile()) return;

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file));
    if (!reader) return;

    juce::AudioBuffer<float> newBuf((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read(&newBuf, 0, (int)reader->lengthInSamples, 0, true, true);

    {
        std::lock_guard<std::mutex> lock(cmpMutex);
        auto& s = cmpStream[slot];
        s.buffer = std::move(newBuf);
        s.sampleCount = s.buffer.getNumSamples();
        s.sampleRate = reader->sampleRate;
        s.playbackPos = 0;
        s.filePath = wavPath;
        s.monGain = 0.0f;              // new content fades IN (no click)
        s.stopAtZero.store(false);
    }
    cmpStream[slot].loaded.store(true);
    cmpStream[slot].playing.store(false);  // don't auto-play; wait for user or transport
    cmpMeter[slot].reset();
    EchoJay_NSLog(("EJCmp: loaded slot=" + juce::String(slot)
                   + " samples=" + juce::String(cmpStream[slot].sampleCount)
                   + " sr=" + juce::String(cmpStream[slot].sampleRate, 0)
                   + " file=" + juce::File(wavPath).getFileName()).toRawUTF8());
}

void EchoJayProcessor::fadeOutCompareStreams()
{
    // Click-free disengage: monitor gain ramps to zero in processBlock (8ms),
    // then each stream self-stops on the audio thread. Callers clear
    // cmpAudible first so nothing re-targets gain 1.
    cmpStream[0].stopAtZero.store(true);
    cmpStream[1].stopAtZero.store(true);
}

void EchoJayProcessor::stopCompareStream(int slot)
{
    if (slot < 0 || slot > 1) return;
    cmpStream[slot].loaded.store(false);
    cmpStream[slot].playing.store(false);
    cmpStream[slot].playbackPos = 0;
    if (cmpAudible.load() == slot)
        cmpAudible.store(-1);
}

void EchoJayProcessor::stopAllCompare()
{
    for (int i = 0; i < 2; ++i)
    {
        cmpStream[i].loaded.store(false);
        cmpStream[i].playing.store(false);
        cmpStream[i].playbackPos = 0;
    }
    cmpAudible.store(-1);
}

// ============ State Persistence ============

bool EchoJayProcessor::applyBusGainSmoothed(juce::AudioBuffer<float>& buffer)
{
    // LinkProcessor::applyGainSmoothed, verbatim shape: target from the
    // atomic, optional snap (state restore must not ramp from unity), unity
    // early-out for bit-transparency at 0 dB, per-sample ramp while
    // smoothing. No locks, no allocation.
    const float targetLin =
        EchoJayFader::gainForDb(busGainDb_.load(std::memory_order_relaxed));

    if (busGainSnapPending_.exchange(false, std::memory_order_acq_rel))
        busGainSmoothed_.setCurrentAndTargetValue(targetLin);
    else
        busGainSmoothed_.setTargetValue(targetLin);

    if (!busGainSmoothed_.isSmoothing()
        && busGainSmoothed_.getCurrentValue() == 1.0f)
        return false;

    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (busGainSmoothed_.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = busGainSmoothed_.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
                buffer.getWritePointer(ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain(busGainSmoothed_.getCurrentValue());
    }
    return true;
}

void EchoJayProcessor::setBusGainDb(float db, bool snapSmoothing)
{
    db = juce::jlimit(kBusGainMinDb, kBusGainMaxDb, db);
    busGainDb_.store(db, std::memory_order_relaxed);
    if (snapSmoothing)
        busGainSnapPending_.store(true, std::memory_order_relaxed);
    markStateDirty();
}

void EchoJayProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    ejTeardownLog("getStateInformation enter");
    try {
    auto state = std::make_unique<juce::DynamicObject>();
    state->setProperty("genre", genre);
    state->setProperty("genrePromptDismissed", genrePromptDismissed);
    state->setProperty("projectPromptDismissed", projectPromptDismissed);
    state->setProperty("channelType", (int)channelType);
    state->setProperty("customChannelName", customChannelName);
    // §7: key source pin — per instance, survives a session reload
    state->setProperty("keySourcePin", keySourcePin_);
    state->setProperty("keySourcePinLabel", keySourcePinLabel_);
    state->setProperty("channelTypePromptDismissed", channelTypePromptDismissed);
    state->setProperty("passCounter", passCounter);
    state->setProperty("projectName", projectName);
    state->setProperty("captureVersion", captureVersion);
    // Layout choice, not transient UI state: how you want the window is worth
    // writing into a project file. Read back guarded, so an older project
    // loads expanded and the migration is silent (no version bump).
    state->setProperty("chatSidebarCollapsed", chatSidebarCollapsed);
    // Link mixer view controls, same reasoning as the line above: how you want
    // the mixer laid out is a layout choice worth writing into a project file.
    state->setProperty("linkMixerContent", (int)linkMixerContent);
    state->setProperty("linkMixerWide", linkMixerWide);
    state->setProperty("linkFaderMode", (int)linkFaderMode);
    state->setProperty("busGainDb", busGainDb_.load(std::memory_order_relaxed));
    // Pre-chain gain persists with the SESSION (source-specific, so it never
    // travels in a shared chain: buildChainSlotsVar omits it). userSet rides
    // along so a reopen keeps a hand-set value from being auto-overwritten.
    state->setProperty("chainPreGainDb",      chainHost.getPreGainDb());
    state->setProperty("chainPreGainUserSet", chainHost.isPreGainUserSet());

    // Serialise snapshots — copy under lock, serialise outside
    std::vector<CaptureSnapshot> snapsCopy;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapsCopy = snapshots;
    }
    
    juce::Array<juce::var> snapsArr;
    for (auto& s : snapsCopy)
    {
            auto obj = std::make_unique<juce::DynamicObject>();
            obj->setProperty("id", s.id);
            obj->setProperty("name", s.name);
            obj->setProperty("channelType", (int)s.channelType);
            obj->setProperty("customChannelName", s.customChannelName);
            obj->setProperty("timestamp", s.timestamp);
            obj->setProperty("durationSeconds", s.durationSeconds);
            obj->setProperty("wavFilePath", s.wavFilePath);
            
            // Meter data
            auto m = std::make_unique<juce::DynamicObject>();
            m->setProperty("integrated", s.averagedData.integrated);
            m->setProperty("loudnessRange", s.averagedData.loudnessRange);
            m->setProperty("rmsL", s.averagedData.rmsL);
            m->setProperty("rmsR", s.averagedData.rmsR);
            m->setProperty("peakL", s.averagedData.peakL);
            m->setProperty("peakR", s.averagedData.peakR);
            m->setProperty("truePeakL", s.averagedData.truePeakL);
            m->setProperty("truePeakR", s.averagedData.truePeakR);
            m->setProperty("crestFactor", s.averagedData.crestFactor);
            m->setProperty("dcOffset", s.averagedData.dcOffset);
            m->setProperty("width", s.averagedData.width);
            m->setProperty("correlation", s.averagedData.correlation);
            m->setProperty("momentary", s.averagedData.momentary);
            m->setProperty("shortTerm", s.averagedData.shortTerm);
            obj->setProperty("meters", juce::var(m.release()));
            
            // Spectrum
            juce::Array<juce::var> specArr;
            for (int i = 0; i < 64; ++i)
                specArr.add(s.averagedData.spectrum[(size_t)i]);
            obj->setProperty("spectrum", specArr);
            
            // EQ curve
            juce::Array<juce::var> eqArr;
            for (int i = 0; i < 64; ++i)
                eqArr.add(s.eqCurve[(size_t)i]);
            obj->setProperty("eqCurve", eqArr);
            
            // Waveform (store every 2nd point to save space)
            juce::Array<juce::var> wfArr;
            for (int i = 0; i < (int)s.waveformThumbnail.size(); i += 2)
                wfArr.add(s.waveformThumbnail[(size_t)i]);
            obj->setProperty("waveform", wfArr);

            // Detected key (§5.2) — written only when a pass succeeded, so a
            // restore of an older save simply has no key (keyValid=false).
            if (s.keyValid)
            {
                auto k = std::make_unique<juce::DynamicObject>();
                k->setProperty("root",       s.keyRoot);
                k->setProperty("minor",      s.keyMinor);
                k->setProperty("confidence", s.keyConfidence);
                k->setProperty("tuningHz",   s.keyTuningHz);
                k->setProperty("tuningCents", s.keyTuningCents);
                juce::Array<juce::var> chromaArr;
                for (int i = 0; i < 12; ++i)
                    chromaArr.add(s.keyChroma[(size_t)i]);
                k->setProperty("chroma",     chromaArr);
                k->setProperty("altRoot",    s.keyAltRoot);
                k->setProperty("altMinor",   s.keyAltMinor);
                k->setProperty("altScore",   s.keyAltScore);
                k->setProperty("sourceName", s.keySourceName);
                k->setProperty("sourcePlacement", s.keySourcePlacement);
                obj->setProperty("detectedKey", juce::var(k.release()));
            }

            snapsArr.add(juce::var(obj.release()));
        }
    state->setProperty("snapshots", snapsArr);
    
    // Serialise chat history
    juce::Array<juce::var> chatArr;
    for (auto& entry : chatHistory)
    {
        auto chatObj = std::make_unique<juce::DynamicObject>();
        chatObj->setProperty("role", entry.role);
        chatObj->setProperty("content", entry.content);
        chatObj->setProperty("hasWaveform", entry.hasWaveform);
        chatObj->setProperty("durationSeconds", entry.durationSeconds);
        chatObj->setProperty("lufs", entry.lufs);
        chatObj->setProperty("wavFilename", entry.wavFilename);
        chatObj->setProperty("wavFilePath", entry.wavFilePath);
        if (entry.hasWaveform && !entry.waveform.empty())
        {
            juce::Array<juce::var> wfArr;
            for (int i = 0; i < (int)entry.waveform.size(); i += 2) // every 2nd point to save space
                wfArr.add(entry.waveform[(size_t)i]);
            chatObj->setProperty("waveform", wfArr);
        }
        chatArr.add(juce::var(chatObj.release()));
    }
    state->setProperty("chatHistory", chatArr);
    
    // Serialise chat roles/contents for AI context
    juce::Array<juce::var> rolesArr, contentsArr;
    for (auto& r : chatRoles) rolesArr.add(r);
    for (auto& c : chatContents) contentsArr.add(c);
    state->setProperty("chatRoles", rolesArr);
    state->setProperty("chatContents", contentsArr);
    
    // Serialise reference track paths so they persist across channel changes / DAW save
    auto refs = refAnalyser.getReferences();
    juce::Array<juce::var> refsArr;
    for (auto& ref : refs)
        refsArr.add(ref.path);
    state->setProperty("referencePaths", refsArr);
    
    // Visual mode state
    state->setProperty("visualPreset", visualPreset);
    state->setProperty("visualTheme", visualTheme);
    state->setProperty("visualModeOn", visualModeOn);

    // CHAIN state. chainSlotsXml is slot IDENTITY and its format is frozen:
    // byte for byte what every build since v2.4.0 has written, so a session
    // saved here still opens in an older build. The hosted plugins' own
    // settings ride in a SEPARATE top-level key, which an older build
    // ignores (every read below in setStateInformation is hasProperty-gated)
    // and which is absent entirely when there is nothing to say.
    //
    // Nothing here calls into a hosted plugin. getCachedSlotStatesVar only
    // serialises strings the cache already holds, refreshed ahead of time on
    // the message thread, because a hosted getStateInformation can take
    // seconds and this callback is teardown-adjacent.
    state->setProperty("chainSlotsXml", chainHost.getSlotsStateXml());
    auto chainSlotState = chainHost.getCachedSlotStatesVar(
                              ChainHost::kSessionStateMaxSlotBytes,
                              ChainHost::kSessionStateMaxTotalBytes,
                              "this session");
    if (!chainSlotState.isVoid())
        state->setProperty("chainSlotState", chainSlotState);
    // VST3 parameter values beside the blob (17 Aug 2026): a SIBLING key, so
    // chainSlotState keeps its per-slot base64 strings and an older build
    // reads it exactly as before. Absent when no VST3 slot has one.
    if (auto chainSlotParams = chainHost.getCachedSlotParamsVar(); !chainSlotParams.isVoid())
        state->setProperty("chainSlotParams", chainSlotParams);
    // Running level tally (17 Aug 2026): a sibling key, never inside the
    // frozen chainSlotsXml. Carries the host track name for the restore
    // guard. An older build ignores it.
    state->setProperty("chainLevels", chainHost.getLevelsStateVar(chainHost.getHostTrackName()));
    state->setProperty("chainWarningDismissed", chainHost.chainWarningDismissed);
    // Saved chain identity: written only when there IS one, so a session
    // with no saved chain grows no keys and still reads identically in an
    // older build.
    if (savedChainId.isNotEmpty())
    {
        state->setProperty("savedChainId",   savedChainId);
        state->setProperty("savedChainName", savedChainName);
    }

    juce::String json = juce::JSON::toString(juce::var(state.release()), true);
    destData.append(json.toRawUTF8(), json.getNumBytesAsUTF8());
    } catch (...) {}
    ejTeardownLog("getStateInformation exit");
}

void EchoJayProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    try {
    juce::String json = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    auto parsed = juce::JSON::parse(json);
    
    // Try new JSON format first
    if (parsed.isObject())
    {
        auto* obj = parsed.getDynamicObject();
        if (obj)
        {
            genre = obj->getProperty("genre").toString();
            if (genre.isEmpty()) genre = "hip-hop";
            channelType = static_cast<ChannelType>((int)obj->getProperty("channelType"));
            // §7: restore the key source pin (absent on older saves = Auto)
            if (obj->hasProperty("keySourcePin"))
            {
                keySourcePin_      = obj->getProperty("keySourcePin").toString();
                keySourcePinLabel_ = obj->getProperty("keySourcePinLabel").toString();
                selfKeyForced_.store(keySourcePin_ == "self", std::memory_order_relaxed);
            }
            if (obj->hasProperty("customChannelName"))
                customChannelName = obj->getProperty("customChannelName").toString();
            // Restore dismissed — if field exists use it, otherwise derive from channel type
            if (obj->hasProperty("channelTypePromptDismissed"))
                channelTypePromptDismissed = (bool)obj->getProperty("channelTypePromptDismissed");
            else
                channelTypePromptDismissed = (channelType != ChannelType::FullMix);
            // Genre flag: saves made before it existed derive from the channel
            // flag — the genre prompt always followed the channel prompt, so
            // an answered channel implies an answered genre (avoids one
            // spurious re-prompt on every pre-existing project).
            if (obj->hasProperty("genrePromptDismissed"))
                genrePromptDismissed = (bool)obj->getProperty("genrePromptDismissed");
            else
                genrePromptDismissed = channelTypePromptDismissed;
            // Absent in every project saved before this shipped, which is
            // exactly the silent migration: the member keeps its false
            // default and the sidebar opens expanded, as it always has.
            // DELIBERATELY NO else: an absent flag must leave the member at
            // its default. Do not append one, and do not insert a new
            // read-back between the if/else pair ABOVE: that is how f43a698
            // handed the genre flag's else to this if, re-firing the genre
            // prompt on every project saved 6-29 Jul 2026.
            if (obj->hasProperty("chatSidebarCollapsed"))
                chatSidebarCollapsed = (bool)obj->getProperty("chatSidebarCollapsed");
            // Link mixer view controls. Same guarded, else-less shape: absent
            // means a save that predates the mixer, which opens in the
            // defaults (narrow strips, numbers). The saved integer goes
            // through linkMixerContentFromSaved, the ONE migration authority:
            // saves from before the 8b layout pass persisted 1 for the
            // now-removed meter mode, and that maps to Numbers (the meter is
            // permanent chrome now); 2 stays Chain; anything else is Numbers.
            if (obj->hasProperty("linkMixerContent"))
                linkMixerContent = linkMixerContentFromSaved(
                    (int)obj->getProperty("linkMixerContent"));
            if (obj->hasProperty("linkMixerWide"))
                linkMixerWide = (bool)obj->getProperty("linkMixerWide");
            if (obj->hasProperty("linkFaderMode"))
                linkFaderMode = ((int)obj->getProperty("linkFaderMode") == 1)
                                    ? LinkFaderMode::Pre : LinkFaderMode::Post;
            // Bus trim: guarded and else-less (absent = older save = 0 dB
            // default untouched); snap the smoother so a restored trim does
            // not ramp in from unity on the first block.
            if (obj->hasProperty("busGainDb"))
                setBusGainDb((float)(double)obj->getProperty("busGainDb"),
                             /*snapSmoothing=*/true);
            // Pre-chain gain restored FROZEN (never recomputed on reopen).
            // userSet first so setPreGainDb below records the right state.
            if (obj->hasProperty("chainPreGainDb"))
                chainHost.setPreGainDb((float)(double)obj->getProperty("chainPreGainDb"),
                                       obj->hasProperty("chainPreGainUserSet")
                                           && (bool)obj->getProperty("chainPreGainUserSet"));
            // Project prompt: saves that predate the flag derive from having
            // a name (named project = question already answered)
            if (obj->hasProperty("projectPromptDismissed"))
                projectPromptDismissed = (bool)obj->getProperty("projectPromptDismissed");
            else
                projectPromptDismissed = projectName.isNotEmpty();
            passCounter = (int)obj->getProperty("passCounter");
            if (obj->hasProperty("projectName"))
                projectName = obj->getProperty("projectName").toString();
            if (obj->hasProperty("captureVersion"))
                captureVersion = juce::jmax(1, (int)obj->getProperty("captureVersion"));
            
            // Restore snapshots
            auto snapsVar = obj->getProperty("snapshots");
            if (auto* snapsArr = snapsVar.getArray())
            {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                snapshots.clear();
                
                for (auto& sv : *snapsArr)
                {
                    auto* so = sv.getDynamicObject();
                    if (!so) continue;
                    
                    CaptureSnapshot s;
                    s.id = so->getProperty("id").toString();
                    s.name = so->getProperty("name").toString();
                    s.channelType = static_cast<ChannelType>((int)so->getProperty("channelType"));
                    if (so->hasProperty("customChannelName"))
                        s.customChannelName = so->getProperty("customChannelName").toString();
                    s.timestamp = (juce::int64)(double)so->getProperty("timestamp");
                    s.durationSeconds = (float)(double)so->getProperty("durationSeconds");
                    s.wavFilePath = so->getProperty("wavFilePath").toString();
                    
                    // Meters
                    if (auto* mo = so->getProperty("meters").getDynamicObject())
                    {
                        s.averagedData.integrated = (float)(double)mo->getProperty("integrated");
                        s.averagedData.loudnessRange = (float)(double)mo->getProperty("loudnessRange");
                        s.averagedData.rmsL = (float)(double)mo->getProperty("rmsL");
                        s.averagedData.rmsR = (float)(double)mo->getProperty("rmsR");
                        s.averagedData.peakL = (float)(double)mo->getProperty("peakL");
                        s.averagedData.peakR = (float)(double)mo->getProperty("peakR");
                        s.averagedData.truePeakL = (float)(double)mo->getProperty("truePeakL");
                        s.averagedData.truePeakR = (float)(double)mo->getProperty("truePeakR");
                        s.averagedData.crestFactor = (float)(double)mo->getProperty("crestFactor");
                        s.averagedData.dcOffset = (float)(double)mo->getProperty("dcOffset");
                        s.averagedData.width = (float)(double)mo->getProperty("width");
                        s.averagedData.correlation = (float)(double)mo->getProperty("correlation");
                        s.averagedData.momentary = (float)(double)mo->getProperty("momentary");
                        s.averagedData.shortTerm = (float)(double)mo->getProperty("shortTerm");
                    }
                    
                    // Spectrum
                    if (auto* specArr = so->getProperty("spectrum").getArray())
                        for (int i = 0; i < std::min(64, (int)specArr->size()); ++i)
                            s.averagedData.spectrum[(size_t)i] = (float)(double)(*specArr)[i];
                    
                    // EQ curve
                    if (auto* eqArr = so->getProperty("eqCurve").getArray())
                        for (int i = 0; i < std::min(64, (int)eqArr->size()); ++i)
                            s.eqCurve[(size_t)i] = (float)(double)(*eqArr)[i];
                    
                    // Waveform
                    if (auto* wfArr = so->getProperty("waveform").getArray())
                        for (auto& v : *wfArr)
                            s.waveformThumbnail.push_back((float)(double)v);

                    // Detected key (§5.2) — absent on older saves
                    if (auto* ko = so->getProperty("detectedKey").getDynamicObject())
                    {
                        s.keyValid       = true;
                        s.keyRoot        = (int)ko->getProperty("root");
                        s.keyMinor       = (bool)ko->getProperty("minor");
                        s.keyConfidence  = (float)(double)ko->getProperty("confidence");
                        s.keyTuningHz    = (float)(double)ko->getProperty("tuningHz");
                        s.keyTuningCents = (float)(double)ko->getProperty("tuningCents");
                        if (auto* ca = ko->getProperty("chroma").getArray())
                            for (int i = 0; i < std::min(12, (int)ca->size()); ++i)
                                s.keyChroma[(size_t)i] = (float)(double)(*ca)[i];
                        s.keyAltRoot     = ko->hasProperty("altRoot")
                                             ? (int)ko->getProperty("altRoot") : -1;
                        s.keyAltMinor    = (bool)ko->getProperty("altMinor");
                        s.keyAltScore    = (float)(double)ko->getProperty("altScore");
                        s.keySourceName  = ko->getProperty("sourceName").toString();
                        s.keySourcePlacement = (int)ko->getProperty("sourcePlacement");
                    }

                    snapshots.push_back(s);
                }
            }
        }
        
        // Restore chat history
        if (auto* chatArr = obj->getProperty("chatHistory").getArray())
        {
            chatHistory.clear();
            for (auto& entry : *chatArr)
            {
                if (auto* chatObj = entry.getDynamicObject())
                {
                    ChatEntry ce;
                    ce.role = chatObj->getProperty("role").toString();
                    ce.content = chatObj->getProperty("content").toString();
                    ce.hasWaveform = (bool)chatObj->getProperty("hasWaveform");
                    ce.durationSeconds = (float)(double)chatObj->getProperty("durationSeconds");
                    ce.lufs = (float)(double)chatObj->getProperty("lufs");
                    ce.wavFilename = chatObj->getProperty("wavFilename").toString();
                    ce.wavFilePath = chatObj->getProperty("wavFilePath").toString();
                    if (ce.hasWaveform)
                    {
                        if (auto* wfArr = chatObj->getProperty("waveform").getArray())
                            for (auto& v : *wfArr)
                                ce.waveform.push_back((float)(double)v);
                    }
                    chatHistory.push_back(ce);
                }
            }
        }
        
        // Restore chat roles/contents for AI context
        if (auto* rolesArr = obj->getProperty("chatRoles").getArray())
        {
            chatRoles.clear();
            for (auto& r : *rolesArr)
                chatRoles.add(r.toString());
        }
        if (auto* contentsArr = obj->getProperty("chatContents").getArray())
        {
            chatContents.clear();
            for (auto& c : *contentsArr)
                chatContents.add(c.toString());
        }
        
        // Restore reference tracks — re-analyse from saved file paths
        if (auto* refsArr = obj->getProperty("referencePaths").getArray())
        {
            std::vector<juce::File> refFiles;
            for (auto& rp : *refsArr)
            {
                juce::File f(rp.toString());
                if (f.existsAsFile())
                    refFiles.push_back(f);
            }
            if (!refFiles.empty())
                refAnalyser.analyseFiles(refFiles, [](bool, const juce::String&) {});
        }
        
        // Restore visual mode state
        if (obj->hasProperty("visualPreset"))
            visualPreset = (int)obj->getProperty("visualPreset");
        if (obj->hasProperty("visualTheme"))
            visualTheme = (int)obj->getProperty("visualTheme");
        if (obj->hasProperty("visualModeOn"))
            visualModeOn = (bool)obj->getProperty("visualModeOn");

        // Restore CHAIN state
        if (obj->hasProperty("chainWarningDismissed"))
            chainHost.chainWarningDismissed = (bool)obj->getProperty("chainWarningDismissed");

        // Saved chain identity. Absent on every session written before B.1,
        // which simply means nothing is loaded and Save behaves as Save As.
        if (obj->hasProperty("savedChainId"))
            savedChainId = obj->getProperty("savedChainId").toString();
        if (obj->hasProperty("savedChainName"))
            savedChainName = obj->getProperty("savedChainName").toString();

        // Restore chain slots on message thread (after audio is set up).
        // Support both new "chainSlotsXml" key and old single-plugin "chainLoadedDescXml"
        // so existing sessions that stored one plugin still restore correctly.
        juce::String slotsXml;
        if (obj->hasProperty("chainSlotsXml"))
            slotsXml = obj->getProperty("chainSlotsXml").toString();
        else if (obj->hasProperty("chainLoadedDescXml"))
            chainLoadedDescXml = obj->getProperty("chainLoadedDescXml").toString();

        // Hosted plugin settings, saved alongside the identity XML since the
        // Session B state work. A session written before that has no such
        // key and restores exactly as it always did: identity only, plugins
        // at their defaults.
        juce::var chainSlotState;
        if (obj->hasProperty("chainSlotState"))
            chainSlotState = obj->getProperty("chainSlotState");
        juce::var chainSlotParams;   // absent on every session before 17 Aug 2026: nothing applied
        if (obj->hasProperty("chainSlotParams"))
            chainSlotParams = obj->getProperty("chainSlotParams");
        // Running level tally: chain in/out land at once, per-slot tallies
        // are held pending and land as each slot restores. Guarded by the
        // host track name inside setPendingLevelsState; the name the host
        // has reported so far (if any) is what it is compared against.
        juce::var chainLevels;
        if (obj->hasProperty("chainLevels"))
            chainLevels = obj->getProperty("chainLevels");

        if (slotsXml.isNotEmpty())
        {
            juce::MessageManager::callAsync([this, slotsXml, chainSlotState, chainSlotParams, chainLevels] {
                applyHostTrackNameIfDirty();
                if (!chainLevels.isVoid())
                    chainHost.setPendingLevelsState(chainLevels, chainHost.getHostTrackName());
                chainHost.tryRestoreSlotsFromXml(slotsXml, chainSlotState, chainSlotParams);
            });
        }
        else if (!chainLevels.isVoid())
        {
            // No slots to restore, but the chain in/out tally still describes
            // this track's level (an empty rack build wants it)
            juce::MessageManager::callAsync([this, chainLevels] {
                applyHostTrackNameIfDirty();
                chainHost.setPendingLevelsState(chainLevels, chainHost.getHostTrackName());
            });
        }
        else if (chainLoadedDescXml.isNotEmpty())
        {
            // Old single-plugin format: wrap in CHAIN_SLOTS for restore
            auto xml = chainLoadedDescXml;
            juce::MessageManager::callAsync([this, xml] {
                // Build a CHAIN_SLOTS wrapper around the old single-desc XML
                juce::String wrapped = "<CHAIN_SLOTS><SLOT bypassed=\"0\">" + xml + "</SLOT></CHAIN_SLOTS>";
                chainHost.tryRestoreSlotsFromXml(wrapped);
            });
        }

        return;
    }
    
    // Fallback: try old binary XML format
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr) {
        juce::ValueTree vstate = juce::ValueTree::fromXml(*xml);
        genre = vstate.getProperty("genre", "hip-hop").toString();
        channelType = static_cast<ChannelType>((int)vstate.getProperty("channelType", 0));
        customChannelName = vstate.getProperty("customChannelName", "").toString();
        channelTypePromptDismissed = (bool)vstate.getProperty("channelTypePromptDismissed", false);
        // Legacy XML saves predate the genre flag — derive as in the JSON path
        genrePromptDismissed = (bool)vstate.getProperty("genrePromptDismissed",
                                                        channelTypePromptDismissed);
    }
    } catch (...) {}
}

juce::AudioProcessorEditor* EchoJayProcessor::createEditor() { return new EchoJayEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EchoJayProcessor(); }

// =============================================================================
//  EchoJay Link — consumer implementation (stage 2: registry auto-discovery)
// =============================================================================

void EchoJayProcessor::ensureLinkRegistryOpen()
{
    if (linkRegMap) return;

    // Resolve directory once — reuse for audio slot opens
    if (linkResolvedDir.isEmpty())
    {
        int dirErr = 0;
        linkResolvedDir = LinkShm::resolveDir(dirErr);
        if (linkResolvedDir.isEmpty())
        {
            consumerDiag.regKey    = "(no writable dir)";
            consumerDiag.regOpened = false;
            consumerDiag.regErrno  = dirErr;
            return;
        }
    }

    consumerDiag.regKey = linkResolvedDir;
    int err = 0;
    linkRegMap = LinkShm::openRegistry(linkResolvedDir, linkRegFd, err);
    consumerDiag.regOpened = (linkRegMap != nullptr);
    consumerDiag.regErrno  = err;
}

void EchoJayProcessor::closeLinkRegistryNow()
{
    LinkShm::closeRegistry(linkRegMap, linkRegFd);
    linkRegMap = nullptr;
    linkRegFd  = -1;
}

void EchoJayProcessor::connectLinkAudioSlot(int i, const juce::String& audioFilename,
                                              const juce::String& displayName, float sr,
                                              const juce::String& uid)
{
    if (i < 0 || i >= kMaxLinkSlots) return;
    if (linkResolvedDir.isEmpty()) return;
    // Close existing mapping for this slot if any
    disconnectLinkAudioSlot(i);

    int   fd  = -1;
    void* map = LinkShm::openRingConsumer(linkResolvedDir, audioFilename, fd);
    if (!map) return;
    // Bind to the INODE we actually mapped (stale-ring detection).
    const LinkShm::FileIdentity id = LinkShm::fdIdentity(fd);

    {
        const juce::SpinLock::ScopedLockType sl(activeLinkSlots[i].lock);
        activeLinkSlots[i].map    = map;
        activeLinkSlots[i].fd     = fd;
        activeLinkSlots[i].shmKey = audioFilename;  // track filename for change detection
        activeLinkSlots[i].displayName = displayName;
        activeLinkSlots[i].uid     = uid;
        activeLinkSlots[i].boundId = id;
    }
    activeLinkSlots[i].framesRead.store(0);
    juce::ignoreUnused(sr); // stored per-slot in linkSlotInfos for UI
}

juce::String EchoJayProcessor::resolveLinkDisplayName(const juce::String& uid) const
{
    if (uid.isEmpty()) return {};
    for (const auto& e : getLinkDisplayList())   // Phase N precedence chain
        if (e.info.uid == uid) return e.displayName;
    return {};
}

void EchoJayProcessor::disconnectLinkAudioSlot(int i)
{
    if (i < 0 || i >= kMaxLinkSlots) return;
    void* old = nullptr;
    int   fd  = -1;
    {
        const juce::SpinLock::ScopedLockType sl(activeLinkSlots[i].lock);
        old                      = activeLinkSlots[i].map;
        fd                       = activeLinkSlots[i].fd;
        activeLinkSlots[i].map   = nullptr;
        activeLinkSlots[i].fd    = -1;
        activeLinkSlots[i].shmKey = {};
        activeLinkSlots[i].displayName = {};
        activeLinkSlots[i].uid     = {};
        activeLinkSlots[i].boundId = {};
    }
    if (!old) return;
    // Deferred munmap — audio thread may be inside ringConsume with old pointer
    juce::Timer::callAfterDelay(50, [old, fd]()
    {
        LinkShm::closeRing(old, fd, {}, /*doUnlink=*/false);
    });
}

void EchoJayProcessor::disconnectAllLinkSlotsNow()
{
    for (int i = 0; i < kMaxLinkSlots; ++i)
    {
        void* old = nullptr;
        int   fd  = -1;
        {
            const juce::SpinLock::ScopedLockType sl(activeLinkSlots[i].lock);
            old                      = activeLinkSlots[i].map;
            fd                       = activeLinkSlots[i].fd;
            activeLinkSlots[i].map   = nullptr;
            activeLinkSlots[i].fd    = -1;
            activeLinkSlots[i].shmKey = {};
            activeLinkSlots[i].displayName = {};
        }
        if (old) LinkShm::closeRing(old, fd, {}, false);
    }
}

void EchoJayProcessor::refreshLinkRegistry()
{
    // Message thread only. Called from editor timer ~2 Hz.
    ensureLinkRegistryOpen();
    if (!linkRegMap) return;

    std::vector<LinkSlotInfo> newInfos;

    for (int i = 0; i < kMaxLinkSlots; ++i)
    {
        LinkShm::SlotSnapshot snap;
        const bool slotActive = LinkShm::readSlot(linkRegMap, i, snap);

        if (!slotActive)
        {
            // Slot went inactive — disconnect audio if we had it open
            if (activeLinkSlots[i].map != nullptr)
                disconnectLinkAudioSlot(i);
            slotProbeStates[i] = {};
            continue;
        }

        // Stale detection: heartbeat must climb
        auto& ps = slotProbeStates[i];
        if (snap.heartbeat == ps.lastHb)
        {
            ps.staleCycles++;
            if (ps.staleCycles >= kRegStaleCycles)
            {
                // Producer appears to have crashed — reap the slot
                disconnectLinkAudioSlot(i);
                LinkShm::reapSlot(linkRegMap, i);
                ps = {};
                continue;
            }
        }
        else
        {
            ps.lastHb     = snap.heartbeat;
            ps.staleCycles = 0;
        }

        // Connect the audio ring only while the Link's capture role is
        // ACTIVE — inactive Links stay registered (visible, remotely
        // re-activatable) but publish no ring
        if (snap.active)
        {
            // Reconnect on filename change, no mapping, OR a STALE INODE: the
            // producer may have reopened the ring at the SAME filename (new
            // inode after unlink), leaving our mapping pointed at a dead
            // inode that returns the pre-reopen ring's OLD audio then
            // silence. Identity mismatch = treat as stale, rebind to live.
            bool staleInode = false;
            if (activeLinkSlots[i].map != nullptr && activeLinkSlots[i].boundId.valid)
            {
                const auto cur = LinkShm::pathIdentity(linkResolvedDir + snap.audioFilename);
                staleInode = cur.valid && cur != activeLinkSlots[i].boundId;
            }
            if (activeLinkSlots[i].shmKey != snap.audioFilename
                || activeLinkSlots[i].map == nullptr || staleInode)
                connectLinkAudioSlot(i, snap.audioFilename, snap.displayName,
                                     snap.sampleRate, snap.instanceUid);
        }
        else if (activeLinkSlots[i].map != nullptr)
        {
            disconnectLinkAudioSlot(i);
        }

        const bool connected = snap.active && activeLinkSlots[i].map != nullptr;
        int64_t frames = activeLinkSlots[i].framesRead.load(std::memory_order_relaxed);

        LinkSlotInfo info;
        info.name       = snap.displayName;
        info.uid        = snap.instanceUid;
        info.connected  = connected;
        info.active     = snap.active;
        info.sampleRate = snap.sampleRate;
        info.framesRead = frames;
        info.regIdx     = i;    // frame lookup key for readLinkMeterFrame
        info.gainDb     = snap.gainDb;   // Link's built-in gain (0 from old Links)
        info.placement  = snap.placement;   // 0 unset, 1 bus, 2 insert
        info.dialCapable = snap.dialCapable;
        newInfos.push_back(std::move(info));
    }

    linkSlotInfos = std::move(newInfos);

    // Update consumer diagnostics
    consumerDiag.activeSlotCount = (int)linkSlotInfos.size();
    juce::StringArray names;
    for (const auto& s : linkSlotInfos) names.add(s.name);
    consumerDiag.nameList = names.joinIntoString(", ");
}

std::vector<EchoJayProcessor::LinkDisplayEntry>
EchoJayProcessor::getLinkDisplayList() const
{
    // Same order + "Untitled N" numbering as the Link Monitor row list, so a
    // given instance keeps ONE label everywhere. Named first (alphabetical),
    // then untitled (stable by uid). Numbering runs over the full set.
    auto sorted = linkSlotInfos;   // copy, message thread
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const LinkSlotInfo& a, const LinkSlotInfo& b)
        {
            const bool au = a.name.isEmpty(), bu = b.name.isEmpty();
            if (au != bu) return bu;                    // named first
            if (au) return a.uid < b.uid;               // untitled: stable by uid
            return a.name.compareIgnoreCase(b.name) < 0;
        });

    std::vector<LinkDisplayEntry> out;
    out.reserve(sorted.size());
    int untitledCount = 0;
    for (auto& s : sorted)
    {
        juce::String display = s.name;
        if (display.isEmpty())
            display = ++untitledCount > 1 ? "Untitled " + juce::String(untitledCount)
                                          : juce::String("Untitled");
        out.push_back({ display, s });
    }
    return out;
}

bool EchoJayProcessor::readLinkMeterFrame(int regIdx, LinkMeterFrame& out)
{
    // Message thread only (editor paint/timer) — same discipline as
    // refreshLinkRegistry, which owns linkRegMap
    if (linkRegMap == nullptr) return false;
    return LinkShm::readMeterFrame(linkRegMap, regIdx, out);
}
