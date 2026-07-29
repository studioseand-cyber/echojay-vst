#include "PluginHost.h"

namespace ejmap
{

PluginHost::PluginHost (juce::AudioPluginFormatManager& fm)
    : juce::Thread ("ejmap silent pump"), formatManager (fm)
{
    pumpBuffer.setSize (2, kBlockSize);
}

PluginHost::~PluginHost()
{
    unload();
}

//==============================================================================
PluginHost::LoadResult PluginHost::load (const juce::PluginDescription& desc, Watchdog& watchdog)
{
    unload();

    LoadResult result;
    currentDesc = desc;

    const auto pid = desc.fileOrIdentifier;

    juce::String error;
    {
        // Instantiation runs plugin code. bloom hangs ejextract's worker in the
        // equivalent call; there is no reason to assume this one cannot hang.
        Watchdog::Scope guard (watchdog, "createPluginInstance", pid, desc.name,
                               desc.pluginFormatName, "load");
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

    pumpBuffer.setSize (juce::jmax (2, instance->getTotalNumOutputChannels()), kBlockSize);

    result.paramCount = instance->getParameters().size();
    if (result.paramCount == 0)
    {
        result.outcome = LoadOutcome::noParams;
        result.detail  = "plugin exposes no automatable parameters";
        return result;   // instance stays loaded: the human may still want to look
    }

    // Start the pump before creating the editor. Some editors paint nothing until
    // they have seen a callback.
    pumpEnabled = true;
    startThread (juce::Thread::Priority::normal);

    if (! instance->hasEditor())
    {
        result.outcome  = LoadOutcome::noEditor;
        result.detail   = "plugin reports hasEditor() false";
        result.hasEditor = false;
        return result;
    }

    {
        Watchdog::Scope guard (watchdog, "createEditorIfNeeded", pid, desc.name,
                               desc.pluginFormatName, "load");
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

    result.outcome   = LoadOutcome::ok;
    result.hasEditor = true;
    result.detail    = "editor settled at " + juce::String (result.editorWidth) + "x"
                         + juce::String (result.editorHeight);
    return result;
}

//==============================================================================
void PluginHost::unload()
{
    pumpEnabled = false;
    stopThread (2000);

    editor.reset();

    if (instance != nullptr)
    {
        const juce::ScopedLock sl (processLock);
        instance->releaseResources();
        instance.reset();
    }
}

//==============================================================================
void PluginHost::run()
{
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
                pumpBuffer.clear();
                pumpMidi.clear();
                instance->processBlock (pumpBuffer, pumpMidi);
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
