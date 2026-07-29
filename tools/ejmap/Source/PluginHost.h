/*
  PluginHost.h

  Instantiates one plugin, prepares it, keeps it fed with silence so editors that
  only paint under a live callback will paint, and hosts its native editor
  inline.

  Deliberate choices:

    - No AudioDeviceManager and no real output device. A mapping tool that grabs
      the audio hardware fights the DAW the tester also has open. A worker thread
      pumping silent blocks satisfies every plugin that needs a callback, and the
      offline probe render in M9 uses the same processBlock path.

    - Zero-size editors get two retries before falling back. Several plugins size
      themselves only after their first paint, and treating that as no_editor
      loses real plugins.

    - Every failure path returns a LoadOutcome. There is no code path that fails
      quietly.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "EjmapSchema.h"
#include "PluginScanner.h"
#include "EjmapWatchdog.h"

namespace ejmap
{

class PluginHost : private juce::Thread
{
public:
    PluginHost (juce::AudioPluginFormatManager& fm);
    ~PluginHost() override;

    struct LoadResult
    {
        LoadOutcome outcome = LoadOutcome::timeout;
        juce::String detail;
        int paramCount = 0;
        bool hasEditor = false;

        /** Editor bounds at the moment the result was recorded, once the size
            settled. ui_hint normalises against these, so they must be the
            settled size and not the 1x1 an XPC editor reports at creation.
        */
        int editorWidth = 0, editorHeight = 0;
    };

    /** Blocking. Caller must have written inflight.json first.

        Returns only once the editor has reached a stable, non-degenerate size,
        or the editor-ready deadline expires. A bridged AU editor reports 1x1 at
        createEditorIfNeeded and reaches its real size about 2.5 s later when the
        remote view connects; the previous getWidth() <= 0 test passed a 1x1
        editor straight through as ok.
    */
    LoadResult load (const juce::PluginDescription& desc, Watchdog& watchdog);

    /** Anything smaller in either axis is a placeholder, not an editor. 1x1 is
        what an unconnected NSRemoteView reports; measured on UAD Antares
        Auto-Tune RT and Cymatics Lotus.
    */
    static constexpr int kMinSensibleEditorPx = 32;

    /** How long to wait for the editor to settle. Generous: it covers an XPC
        connection, and a plugin that is merely slow should not be recorded as
        broken. Measured connect time on this machine was ~2.5 s.
    */
    static constexpr int kEditorReadyTimeoutMs = 20000;

    /** Size must be unchanged across this many consecutive polls before it
        counts as settled, so a mid-resize reading is never taken as final.
    */
    static constexpr int kEditorStablePolls = 3;
    static constexpr int kEditorPollMs      = 100;

    void unload();

    juce::AudioPluginInstance* getInstance() const noexcept { return instance.get(); }

    /** Owned by the host. Null until load() succeeds with hasEditor. */
    juce::AudioProcessorEditor* getEditor() const noexcept { return editor.get(); }

    /** Detaches the editor so a Component can take ownership for display. */
    std::unique_ptr<juce::AudioProcessorEditor> releaseEditor() { return std::move (editor); }

    PluginIdentity getIdentity() const;

    static constexpr double kSampleRate = 48000.0;
    static constexpr int    kBlockSize  = 512;

    /** Pauses the silent pump. The probe render in M9 takes the processor
        exclusively and must not race the pump.
    */
    void setPumpEnabled (bool shouldPump) noexcept { pumpEnabled = shouldPump; }

private:
    void run() override;   // silent pump

    juce::AudioPluginFormatManager& formatManager;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::PluginDescription currentDesc;

    juce::AudioBuffer<float> pumpBuffer;
    juce::MidiBuffer pumpMidi;
    juce::CriticalSection processLock;
    std::atomic<bool> pumpEnabled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace ejmap
