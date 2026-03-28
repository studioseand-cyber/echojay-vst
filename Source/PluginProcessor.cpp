#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

EchoJayProcessor::EchoJayProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Defer plugin cache loading to background so constructor returns fast
    juce::Thread::launch([this]() {
        pluginScanner.loadCache();
    });
}

EchoJayProcessor::~EchoJayProcessor()
{
    // Wait for any in-flight WAV save to finish before we destroy members
    if (saveThread && saveThread->isThreadRunning())
        saveThread->waitForThreadToExit(5000);
}

void EchoJayProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    meterEngine.prepare(sampleRate, samplesPerBlock);
    captureEngine.prepare(sampleRate, samplesPerBlock);
    waveformRecorder.prepare(sampleRate, samplesPerBlock);
}

void EchoJayProcessor::releaseResources() { meterEngine.reset(); }

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
            
            // Auto-stop capture when transport stops (spacebar)
            if (wasTransportPlaying && !playing && captureState.load() == CaptureState::Capturing)
                stopCapture();
            
            wasTransportPlaying = playing;
        }
    }
    
    const float* left = buffer.getNumChannels() >= 1 ? buffer.getReadPointer(0) : nullptr;
    const float* right = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : left;
    if (left == nullptr) return;

    // Always feed live meters
    meterEngine.processBlock(left, right, buffer.getNumSamples());
    
    // Feed capture engine if capturing
    if (captureState.load() == CaptureState::Capturing)
    {
        captureEngine.processBlock(left, right, buffer.getNumSamples());
        waveformRecorder.processBlock(left, right, buffer.getNumSamples());
        // Accumulate live spectrum for averaged EQ curve
        auto liveSpec = meterEngine.getMeterData().spectrum;
        for (int i = 0; i < 64; ++i)
            spectrumSum[(size_t)i] += liveSpec[(size_t)i];
        spectrumFrames++;
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
    spectrumSum.fill(0.0f);
    spectrumFrames = 0;
    captureState.store(CaptureState::Capturing);
}

void EchoJayProcessor::stopCapture()
{
    if (captureState.load() != CaptureState::Capturing) return;
    captureState.store(CaptureState::Complete);
    waveformRecorder.stopRecording();
    
    passCounter++;
    
    CaptureSnapshot snap;
    snap.id = juce::String(juce::Time::currentTimeMillis());
    snap.name = "Pass " + juce::String(passCounter);
    snap.channelType = channelType;
    snap.customChannelName = customChannelName;
    snap.averagedData = captureEngine.getMeterData();
    snap.timestamp = juce::Time::currentTimeMillis();
    snap.durationSeconds = (float)(juce::Time::currentTimeMillis() - captureStartTime) / 1000.0f;
    
    // Waveform thumbnail from recorder
    auto thumb = waveformRecorder.getThumbnail();
    for (auto& pt : thumb)
        snap.waveformThumbnail.push_back(std::max(std::abs(pt.maxVal), std::abs(pt.minVal)));
    
    // EQ curve — use averaged live spectrum accumulated during entire capture
    if (spectrumFrames > 0) {
        for (int i = 0; i < 64; ++i)
            snap.eqCurve[(size_t)i] = spectrumSum[(size_t)i] / (float)spectrumFrames;
        snap.averagedData.spectrum = snap.eqCurve;
    } else {
        snap.eqCurve = snap.averagedData.spectrum;
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
    
    struct SaveThread : public juce::Thread
    {
        SaveThread(WaveformRecorder* rec, juce::File dir, juce::String name,
                   int idx, std::mutex* mtx, std::vector<CaptureSnapshot>* snaps)
            : juce::Thread("EchoJay WAV Save"), recorder(rec), captureDir(dir),
              passName(name), snapIdx(idx), mutex(mtx), snapshots(snaps) {}
        
        void run() override
        {
            recorder->saveToWAV(captureDir, passName);
            auto savedPath = recorder->getLastSavedPath();
            if (savedPath.isNotEmpty())
            {
                std::lock_guard<std::mutex> lock(*mutex);
                if (snapIdx >= 0 && snapIdx < (int)snapshots->size())
                    (*snapshots)[(size_t)snapIdx].wavFilePath = savedPath;
            }
        }
        
        WaveformRecorder* recorder;
        juce::File captureDir;
        juce::String passName;
        int snapIdx;
        std::mutex* mutex;
        std::vector<CaptureSnapshot>* snapshots;
    };
    
    saveThread = std::make_unique<SaveThread>(recorderPtr, captureDir, passName, snapIdx, mutexPtr, snapsPtr);
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

juce::String EchoJayProcessor::buildCompareContext(const CaptureSnapshot& capture, const ReferenceResult& reference) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& a = capture.averagedData;
    auto& b = reference.data;
    
    juce::String ctx;
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
    
    ctx += "\nINSTRUCTIONS: Only comment on differences that are genuinely significant. Small variations (< 1.5 LUFS, < 2dB crest, < 15% width) are normal and should be described as practically the same. Width is not a reliable metric — only flag if the difference is drastic. Focus on what the user should actually do differently to get closer to the reference. Be concise — 2-3 paragraphs max.\n";
    
    return ctx;
}

juce::String EchoJayProcessor::buildCompareContext(const CaptureSnapshot& a, const CaptureSnapshot& b) const
{
    auto ff = [](float v) { return v > -99.0f ? juce::String(v, 1) : juce::String("N/A"); };
    auto& da = a.averagedData;
    auto& db = b.averagedData;
    
    juce::String ctx;
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
    
    ctx += "\nINSTRUCTIONS: Only comment on differences that are genuinely significant. Small variations (< 1.5 LUFS, < 2dB crest, < 15% width) are normal measurement noise and should be described as practically the same — do NOT suggest changes for metrics that haven't meaningfully changed. Width is not reliable enough to suggest changes unless the difference is drastic (> 15%). If the passes are essentially the same, say so and ask what they changed or what they're trying to achieve. Be concise — 2-3 paragraphs max.\n";
    
    return ctx;
}

// ============ State ============

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

void EchoJayProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    try {
    auto state = std::make_unique<juce::DynamicObject>();
    state->setProperty("genre", genre);
    state->setProperty("channelType", (int)channelType);
    state->setProperty("customChannelName", customChannelName);
    state->setProperty("channelTypePromptDismissed", channelTypePromptDismissed);
    state->setProperty("passCounter", passCounter);
    
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
    
    juce::String json = juce::JSON::toString(juce::var(state.release()), true);
    destData.append(json.toRawUTF8(), json.getNumBytesAsUTF8());
    } catch (...) {}
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
