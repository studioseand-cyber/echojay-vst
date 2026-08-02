#include "PluginHost.h"

namespace ejmap
{

PluginHost::PluginHost (juce::AudioPluginFormatManager& fm)
    : juce::Thread ("ejmap silent pump"), formatManager (fm)
{
}

PluginHost::~PluginHost()
{
    // Best effort. If the pump will not stop we leak the instance rather than
    // free it under a live renderer; a leak at shutdown beats a SIGSEGV.
    unload();
}

//==============================================================================
PluginHost::LoadResult PluginHost::load (const juce::PluginDescription& desc, Watchdog& watchdog)
{
    LoadResult result;

    // THE STAKE, AT THE CHOKE POINT (the misplaced-guard class, third
    // instance). It used to be planted by CALLERS: 5 of 16 load sites in the
    // gate path did so, and kHs Gate and Weiss Deess died on one of the
    // eleven that did not, leaving nothing on disk naming them. A sixth
    // per-site beginLoad would only leave the seventh site to forget, so it
    // lives HERE, in the one function every load already goes through.
    // Callers that plant their own stake are harmless: beginLoad overwrites
    // inflight.json rather than nesting, and endLoad below closes whichever
    // is current.
    // THE ID IS NOT ASSEMBLED HERE. It is built by the component that owns
    // the scheme, because an identifier written for another component to key
    // on must come from that component's own constructor or a second scheme
    // gets invented -- twice, in this case. First a doubled "AudioUnit:
    // AudioUnit:..." prefix; then a "fix" that conditionally prefixed, which
    // was STILL wrong for VST3, whose fileOrIdentifier is a bundle path with
    // no format prefix to detect. Neither would key to any quarantine row.
    ScannedPlugin sp;
    sp.desc = desc;
    const juce::String stakeId = sp.pluginId();
    watchdog.getLedger().beginLoad (stakeId, desc.name, desc.manufacturerName,
                                    desc.pluginFormatName, desc.version, "load", "PluginHost::load");

    // Every early return below goes through this, so a non-ok outcome records
    // its DETAIL rather than having callers discard it on an early continue --
    // which is how the two load failures above became unexplainable.
    struct StakeCloser
    {
        Watchdog& w; const juce::String& id; const LoadResult& r;
        ~StakeCloser()
        {
            LedgerRecord rec;
            rec.pluginId = id;
            rec.stage    = "load";
            rec.outcome  = r.outcome;
            rec.detail   = r.detail;
            w.getLedger().endLoad (rec);
        }
    } stakeCloser { watchdog, stakeId, result };

    // Refuse to start a new load while the previous plugin's pump is still
    // rendering. Tearing down underneath it is what crashed on soothe2.
    if (! unload())
    {
        result.outcome = LoadOutcome::timeout;
        result.detail  = "previous plugin's audio pump did not stop within "
                           + juce::String (kPumpStopTimeoutMs) + "ms; nothing torn down";
        return result;
    }

    currentDesc = desc;

    const auto pid = desc.fileOrIdentifier;

    juce::String error;
    {
        // Instantiation runs plugin code. bloom hangs ejextract's worker in the
        // equivalent call; there is no reason to assume this one cannot hang.
        Watchdog::Scope guard (watchdog, "createPluginInstance", pid, desc.name,
                               desc.pluginFormatName, "load",
                               Watchdog::kInstantiateDeadlineMs);
        instance = formatManager.createPluginInstance (desc, kSampleRate, kBlockSize, error);
    }

    if (instance == nullptr)
    {
        // Distinguish a licence refusal from a generic failure. ejextract already
        // carries this outcome and the queue treats it differently: a licence
        // refusal is not the tester's fault and should not quarantine.
        const auto lower = error.toLowerCase();
        const bool licence = lower.contains ("licen") || lower.contains ("authoris")
                          || lower.contains ("authoriz") || lower.contains ("activat")
                          || lower.contains ("demo") || lower.contains ("trial");

        result.outcome = licence ? LoadOutcome::licenseRefused : LoadOutcome::timeout;
        result.detail  = error.isEmpty() ? "createPluginInstance returned null with no message" : error;
        return result;
    }

    instance->enableAllBuses();
    instance->setPlayConfigDetails (instance->getTotalNumInputChannels(),
                                    instance->getTotalNumOutputChannels(),
                                    kSampleRate, kBlockSize);
    instance->prepareToPlay (kSampleRate, kBlockSize);

    result.paramCount = instance->getParameters().size();
    if (result.paramCount == 0)
    {
        result.outcome = LoadOutcome::noParams;
        result.detail  = "plugin exposes no automatable parameters";
        return result;   // instance stays loaded: the human may still want to look
    }

    // The pump does NOT start here.
    //
    // It used to, on the reasoning that some editors paint nothing until they
    // have seen a callback. For a bridged AU that put an XPC render in flight
    // at the same time as an XPC view creation, and AUOOPRenderingClient does
    // not survive it: SIGSEGV at 0x0 in renderGetInput, on the pump thread,
    // reproducible on soothe2 on the FIRST load with no teardown involved.
    //
    // The editor is created and allowed to settle first, then the pump starts.
    // An editor that needs a callback to paint still gets one, a few hundred
    // milliseconds later.
    if (! instance->hasEditor())
    {
        result.outcome  = LoadOutcome::noEditor;
        result.detail   = "plugin reports hasEditor() false";
        result.hasEditor = false;
        return result;
    }

    {
        Watchdog::Scope guard (watchdog, "createEditorIfNeeded", pid, desc.name,
                               desc.pluginFormatName, "load",
                               Watchdog::kEditorCreateDeadlineMs);
        editor.reset (instance->createEditorIfNeeded());
    }

    if (editor == nullptr)
    {
        result.outcome   = LoadOutcome::noEditor;
        result.detail    = "createEditorIfNeeded returned null despite hasEditor() true";
        result.hasEditor = false;
        return result;
    }

    // ------------------------------------------------------------------
    // Editor-ready wait.
    //
    // The old test was `getWidth() <= 0`. A bridged AU editor reports 1x1 at
    // creation, and 1 is not <= 0, so the retry never ran, the 800x600 fallback
    // never fired, and a 1x1 editor was recorded as a successful load. The
    // window then sized itself to 1x1 while the human waited for a GUI.
    //
    // Wait for a size that is both non-degenerate AND stable across several
    // polls, so a mid-resize reading is never taken as final.
    //
    // The message loop MUST be pumped while waiting. The remote view connects
    // via XPC on the message thread; blocking it here would guarantee the very
    // timeout this loop is trying to avoid.
    const auto deadline = juce::Time::getMillisecondCounter()
                            + (juce::uint32) kEditorReadyTimeoutMs;
    int lastW = -1, lastH = -1, stable = 0;
    bool ready = false;

    {
        Watchdog::Scope guard (watchdog, "editor-ready wait", pid, desc.name,
                               desc.pluginFormatName, "load",
                               kEditorReadyTimeoutMs + 5000);

        auto* mm = juce::MessageManager::getInstanceWithoutCreating();

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (mm != nullptr && mm->isThisTheMessageThread())
                mm->runDispatchLoopUntil (kEditorPollMs);
            else
                juce::Thread::sleep (kEditorPollMs);

            const int w = editor->getWidth(), h = editor->getHeight();

            if (w >= kMinSensibleEditorPx && h >= kMinSensibleEditorPx)
            {
                if (w == lastW && h == lastH)
                {
                    if (++stable >= kEditorStablePolls) { ready = true; break; }
                }
                else
                {
                    stable = 0;
                    // Resizing is progress, not a hang. Do not let a plugin that
                    // is visibly working be killed for taking its time.
                    guard.heartbeat();
                }
            }
            else
            {
                stable = 0;
            }

            lastW = w; lastH = h;
        }
    }

    result.editorWidth  = editor->getWidth();
    result.editorHeight = editor->getHeight();

    if (! ready)
    {
        // A remote view that never connects is a recorded outcome, not a hang
        // and not a silent 1x1 success.
        result.outcome   = LoadOutcome::timeout;
        result.hasEditor = false;
        result.detail    = "editor never reached a stable size >= "
                             + juce::String (kMinSensibleEditorPx) + "px within "
                             + juce::String (kEditorReadyTimeoutMs / 1000) + "s (last seen "
                             + juce::String (result.editorWidth) + "x"
                             + juce::String (result.editorHeight) + ")";
        return result;
    }

    // Editor is up and stable. Only now is it safe to render.
    pumpChannels = juce::jmax (2,
                               juce::jmax (instance->getTotalNumInputChannels(),
                                           instance->getTotalNumOutputChannels()));
    pumpEnabled = true;
    startThread (juce::Thread::Priority::normal);

    result.outcome   = LoadOutcome::ok;
    result.hasEditor = true;
    result.detail    = "editor settled at " + juce::String (result.editorWidth) + "x"
                         + juce::String (result.editorHeight);
    return result;
}

//==============================================================================
bool PluginHost::unload()
{
    pumpEnabled = false;

    // FULLY stopped before anything is freed. The old code passed a 2 s
    // timeout and then tore down regardless of the result, so a pump still
    // inside an XPC processBlock kept rendering a buffer that was about to be
    // reallocated. If it will not stop, we free nothing and say so.
    if (isThreadRunning() && ! stopThread (kPumpStopTimeoutMs))
    {
        jassertfalse;
        return false;
    }

    editor.reset();

    if (instance != nullptr)
    {
        const juce::ScopedLock sl (processLock);
        instance->releaseResources();
        instance.reset();
    }

    return true;
}

//==============================================================================
void PluginHost::run()
{
    // The buffer is a LOCAL. It is allocated here, on the pump thread, and dies
    // with the thread. Nothing on the message thread can resize or free it
    // while processBlock is holding its channel pointers.
    juce::AudioBuffer<float> buffer (juce::jmax (2, pumpChannels.load()), kBlockSize);
    juce::MidiBuffer midi;

    // Phase lives on this thread with the buffer. Nothing else touches it.
    double phase = 0.0;
    const double phaseStep = juce::MathConstants<double>::twoPi * kTestToneHz / kSampleRate;

    // 512 frames at 48 kHz is 10.67 ms. Sleeping the same interval keeps the
    // plugin's internal clock roughly real-time, which matters to anything with
    // a metering or modulation display.
    const int intervalMs = (int) std::round (1000.0 * kBlockSize / kSampleRate);

    while (! threadShouldExit())
    {
        if (pumpEnabled.load())
        {
            const juce::ScopedLock sl (processLock);

            if (instance != nullptr)
            {
                midi.clear();

                if (testSignal.load())
                {
                    // Deliberately generated here rather than pre-rendered: the
                    // buffer is thread-local and the phase must stay continuous
                    // across blocks or the tone clicks and reads as transient
                    // content to anything level-detecting.
                    for (int n = 0; n < buffer.getNumSamples(); ++n)
                    {
                        const float v = kTestToneGain * (float) std::sin (phase);
                        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                            buffer.setSample (ch, n, v);
                        phase += phaseStep;
                        if (phase > juce::MathConstants<double>::twoPi)
                            phase -= juce::MathConstants<double>::twoPi;
                    }
                }
                else
                {
                    buffer.clear();
                }

                instance->processBlock (buffer, midi);
            }
        }

        wait (intervalMs);
    }
}

//==============================================================================
PluginIdentity PluginHost::getIdentity() const
{
    PluginIdentity id;
    id.format  = currentDesc.pluginFormatName;
    id.uid     = juce::String (currentDesc.uniqueId);
    id.name    = currentDesc.name;
    id.vendor  = currentDesc.manufacturerName;
    id.version = currentDesc.version;
    id.paramCount = instance != nullptr ? instance->getParameters().size() : 0;
    return id;
}

} // namespace ejmap
