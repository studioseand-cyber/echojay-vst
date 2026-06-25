#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

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
    int          slotIdx;

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

    LinkCaptureChannel(const juce::String& n, int idx, double sr, int bs)
        : name(n), slotIdx(idx),
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
}

EchoJayProcessor::~EchoJayProcessor()
{
    ejTeardownLog("~EchoJayProcessor enter");

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
    meterEngine.prepare(sampleRate, samplesPerBlock);
    captureEngine.prepare(sampleRate, samplesPerBlock);
    waveformRecorder.prepare(sampleRate, samplesPerBlock);
    hostSampleRate_      = sampleRate;
    hostSamplesPerBlock_ = samplesPerBlock;
}

void EchoJayProcessor::releaseResources() { ejTeardownLog("releaseResources enter"); meterEngine.reset(); ejTeardownLog("releaseResources exit"); }

void EchoJayProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    
    // Track DAW transport state (play/stop)
    if (auto* playHead = getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            bool playing = pos->getIsPlaying();
            transportPlaying.store(playing);
            
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
            
            wasTransportPlaying = playing;
        }
    }
    
    const float* left = buffer.getNumChannels() >= 1 ? buffer.getReadPointer(0) : nullptr;
    const float* right = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : left;
    if (left == nullptr) return;

    // A/B playback: if playing ref, replace buffer BEFORE meters read it
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
        }
    }

    // Always feed live meters (now sees ref audio when AB is active)
    meterEngine.processBlock(left, right, buffer.getNumSamples());
    
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
    updateHostDisplay();
}

void EchoJayProcessor::setChannelTypePromptDismissed(bool dismissed)
{
    channelTypePromptDismissed = dismissed;
    updateHostDisplay();
}

// ============ Capture System ============

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
                linkCaptureChannels.push_back(
                    std::make_unique<LinkCaptureChannel>(
                        activeLinkSlots[i].displayName, i, sr, bs));
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
    snap.name = computePassName();   // reads passCounter+1 (no-project) or captureVersion (project)
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

    struct SaveThread : public juce::Thread
    {
        SaveThread(WaveformRecorder* rec, juce::File dir, juce::String name,
                   int idx, std::mutex* mtx, std::vector<CaptureSnapshot>* snaps,
                   std::vector<std::unique_ptr<LinkCaptureChannel>> lcs)
            : juce::Thread("EchoJay WAV Save"), recorder(rec), captureDir(dir),
              passName(name), snapIdx(idx), mutex(mtx), snapshots(snaps),
              linkChannels(std::move(lcs)) {}

        void run() override
        {
            // Host WAV
            recorder->saveToWAV(captureDir, passName);
            auto hostPath = recorder->getLastSavedPath();
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
            // Per-Link WAVs
            for (size_t i = 0; i < linkChannels.size(); ++i)
            {
                auto& lcc = linkChannels[i];
                lcc->waveformRecorder.saveToWAV(captureDir, passName + " - " + lcc->name);
                auto lp = lcc->waveformRecorder.getLastSavedPath();
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
        }

        WaveformRecorder* recorder;
        juce::File captureDir;
        juce::String passName;
        int snapIdx;
        std::mutex* mutex;
        std::vector<CaptureSnapshot>* snapshots;
        std::vector<std::unique_ptr<LinkCaptureChannel>> linkChannels;
    };

    saveThread = std::make_unique<SaveThread>(recorderPtr, captureDir, passName, snapIdx, mutexPtr, snapsPtr,
                                               std::move(movedLinkChannels));
    saveThread->startThread();

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
            ctx += "TONAL BALANCE: Very similar across the frequency range — no notable band differences.\n";
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
    ctx += "[BEGIN COMPARE CONTEXT — this block is a one-off comparison, NOT an ongoing mix discussion]\n";
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
    
    ctx += "\nINSTRUCTIONS: Only comment on differences that are genuinely significant. Small variations (< 1.5 LUFS, < 2dB crest, < 15% width) are normal and should be described as practically the same. Width is not a reliable metric — only flag if the difference is drastic. For tonal balance, speak in plain language ('your mix is heavier in the low end', 'the reference has more air on top') — do NOT quote dB values or band names like '200-600Hz' to the user. Focus on what the user should actually do differently to get closer to the reference. Be concise — 2-3 paragraphs max.\n";
    
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
                "because one is a short snippet and one is a full track — tonal balance especially can read very differently. "
                "Suggest they capture a longer section (ideally a full chorus and verse) for a more accurate comparison. "
                "Then proceed with the comparison but keep caveats in mind.]\n";
        }
        else if (capDur < 30.0f)
        {
            ctx += "[CAPTURE LENGTH: Your capture is only " + juce::String((int)capDur) + "s — mention briefly that the "
                "comparison is based on a short snippet and a longer capture would give a fuller picture, "
                "but proceed with the comparison.]\n";
        }
    }
    
    ctx += "[END COMPARE CONTEXT]\n";
    ctx += "[PERSISTENT NOTE: If the user later captures a new mix and asks about it, DO NOT treat the numbers in the compare block above as a previous version of that new mix. The compare block is a snapshot of this specific comparison, not part of an ongoing capture history. New captures have their own CURRENT MIX data — use only that for new-capture analysis.]\n";
    
    return ctx;
}

juce::String EchoJayProcessor::buildCompareContext(const CaptureSnapshot& a, const CaptureSnapshot& b) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& da = a.averagedData;
    auto& db = b.averagedData;
    
    juce::String ctx;
    ctx += "[BEGIN COMPARE CONTEXT — this block is a one-off comparison, NOT an ongoing mix discussion]\n";
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
    
    ctx += "\nINSTRUCTIONS: Only comment on differences that are genuinely significant. Small variations (< 1.5 LUFS, < 2dB crest, < 15% width) are normal measurement noise and should be described as practically the same — do NOT suggest changes for metrics that haven't meaningfully changed. Width is not reliable enough to suggest changes unless the difference is drastic (> 15%). For tonal balance, speak in plain language ('more low end', 'brighter on top') — do NOT quote dB values or band names like '200-600Hz' to the user. If the passes are essentially the same, say so and ask what they changed or what they're trying to achieve. Be concise — 2-3 paragraphs max.\n";
    
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
                "one capture is a short snippet and the other is much longer — tonal balance especially won't read accurately. "
                "Suggest they capture matching sections for a fairer comparison, then proceed with the comparison.]\n";
        }
        else if (aDur < 30.0f && bDur < 30.0f)
        {
            ctx += "[SHORT CAPTURES: Both passes are under 30s — mention briefly that short snippets can give a partial "
                "picture, but proceed with the comparison.]\n";
        }
    }
    
    ctx += "[END COMPARE CONTEXT]\n";
    ctx += "[PERSISTENT NOTE: If the user later captures a new pass and asks about it, DO NOT treat the numbers in the compare block above as a previous version of that new capture. This compare is a snapshot of two specific passes at one moment. New captures have their own CURRENT MIX data — use only that for new-capture analysis.]\n";
    
    return ctx;
}

juce::String EchoJayProcessor::buildCompareContext(const ReferenceResult& a, const ReferenceResult& b) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& da = a.data;
    auto& db = b.data;
    
    juce::String ctx;
    ctx += "[BEGIN COMPARE CONTEXT — this block is a one-off comparison of REFERENCE tracks, NOT the user's mix]\n";
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
    
    ctx += "\nINSTRUCTIONS: The user is comparing two reference tracks (not their own mix) — this is usually to understand what separates two sounds they like, or to pick which one to aim for. Focus on what's genuinely different between the two. For tonal balance, speak in plain language ('more low end', 'brighter on top') — do NOT quote dB values or band names like '200-600Hz'. Do NOT suggest 'changes the user should make' since these aren't their mixes. Be concise — 2-3 paragraphs max.\n";
    
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
    ctx += "[PERSISTENT NOTE: The two tracks above are REFERENCE tracks — not the user's own mix. If the user later captures their mix and asks about it, DO NOT treat these reference numbers as a previous version of their capture. Reference tracks and user captures are separate things.]\n";
    
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

// ============ State Persistence ============

void EchoJayProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    ejTeardownLog("getStateInformation enter");
    try {
    auto state = std::make_unique<juce::DynamicObject>();
    state->setProperty("genre", genre);
    state->setProperty("channelType", (int)channelType);
    state->setProperty("customChannelName", customChannelName);
    state->setProperty("channelTypePromptDismissed", channelTypePromptDismissed);
    state->setProperty("passCounter", passCounter);
    state->setProperty("projectName", projectName);
    state->setProperty("captureVersion", captureVersion);
    
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
            if (obj->hasProperty("customChannelName"))
                customChannelName = obj->getProperty("customChannelName").toString();
            // Restore dismissed — if field exists use it, otherwise derive from channel type
            if (obj->hasProperty("channelTypePromptDismissed"))
                channelTypePromptDismissed = (bool)obj->getProperty("channelTypePromptDismissed");
            else
                channelTypePromptDismissed = (channelType != ChannelType::FullMix);
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
                                              const juce::String& displayName, float sr)
{
    if (i < 0 || i >= kMaxLinkSlots) return;
    if (linkResolvedDir.isEmpty()) return;
    // Close existing mapping for this slot if any
    disconnectLinkAudioSlot(i);

    int   fd  = -1;
    void* map = LinkShm::openRingConsumer(linkResolvedDir, audioFilename, fd);
    if (!map) return;

    {
        const juce::SpinLock::ScopedLockType sl(activeLinkSlots[i].lock);
        activeLinkSlots[i].map    = map;
        activeLinkSlots[i].fd     = fd;
        activeLinkSlots[i].shmKey = audioFilename;  // track filename for change detection
        activeLinkSlots[i].displayName = displayName;
    }
    activeLinkSlots[i].framesRead.store(0);
    juce::ignoreUnused(sr); // stored per-slot in linkSlotInfos for UI
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

        // Connect audio ring if not open (or if filename changed)
        if (activeLinkSlots[i].shmKey != snap.audioFilename || activeLinkSlots[i].map == nullptr)
            connectLinkAudioSlot(i, snap.audioFilename, snap.displayName, snap.sampleRate);

        const bool connected = activeLinkSlots[i].map != nullptr;
        int64_t frames = activeLinkSlots[i].framesRead.load(std::memory_order_relaxed);

        LinkSlotInfo info;
        info.name       = snap.displayName;
        info.connected  = connected;
        info.sampleRate = snap.sampleRate;
        info.framesRead = frames;
        newInfos.push_back(std::move(info));
    }

    linkSlotInfos = std::move(newInfos);

    // Update consumer diagnostics
    consumerDiag.activeSlotCount = (int)linkSlotInfos.size();
    juce::StringArray names;
    for (const auto& s : linkSlotInfos) names.add(s.name);
    consumerDiag.nameList = names.joinIntoString(", ");
}
