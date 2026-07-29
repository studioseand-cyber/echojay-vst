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
PluginHost::LoadResult PluginHost::load (const juce::PluginDescription& desc)
{
    unload();

    LoadResult result;
    currentDesc = desc;

    juce::String error;
    instance = formatManager.createPluginInstance (desc, kSampleRate, kBlockSize, error);

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

    editor.reset (instance->createEditorIfNeeded());

    if (editor == nullptr)
    {
        result.outcome   = LoadOutcome::noEditor;
        result.detail    = "createEditorIfNeeded returned null despite hasEditor() true";
        result.hasEditor = false;
        return result;
    }

    // Zero-size retry. Two attempts, 200 ms apart, then a fallback size with the
    // reason recorded rather than a silent guess.
    for (int attempt = 0; attempt < 2 && editor->getWidth() <= 0; ++attempt)
    {
        juce::Thread::sleep (200);
        editor->resized();
    }

    if (editor->getWidth() <= 0 || editor->getHeight() <= 0)
    {
        editor->setSize (800, 600);
        result.detail = "editor reported zero size after two retries, fell back to 800x600";
    }

    result.outcome   = LoadOutcome::ok;
    result.hasEditor = true;
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
