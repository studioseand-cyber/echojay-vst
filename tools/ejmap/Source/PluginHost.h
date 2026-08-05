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

        MUST NOT BE CALLED FROM A TIMER CALLBACK. The editor-ready wait below runs
        a nested dispatch loop, and CoreFoundation traps when one is started from
        timer context: SIGTRAP in CFRunLoopRunSpecific. Button clicks and
        MessageManager::callAsync are ordinary message context and are fine. It
        also must not be called before the message loop is running, where the
        nested loop silently fails to dispatch and every editor needing a layout
        cycle appears never to settle.

        Returns only once the editor has reached a stable, non-degenerate size,
        or the editor-ready deadline expires. A bridged AU editor reports 1x1 at
        createEditorIfNeeded and reaches its real size about 2.5 s later when the
        remote view connects; the previous getWidth() <= 0 test passed a 1x1
        editor straight through as ok.
    */
    /** openEditor=false loads the plugin and STOPS BEFORE THE EDITOR.

        A plugin that shows a dialog does it when its editor opens, and the
        editor-ready wait pumps the message loop, so the alert appears and sits
        there until the watchdog kills the process. Measured on this machine:
        every UAD failure row reads "no return from editor-ready wait after
        25.0s; process terminated by watchdog".

        THE CONTROLS SWEEP NEVER NEEDS THE EDITOR -- it reads parameters. Only
        the panel capture does. So an unattended sweep loads without one and
        the whole class of load-time modals goes away, not UAD's specifically.
        The capture becomes an opt-in second pass over plugins that do not
        block, which is a smaller thing to lose than a night.
    */
    LoadResult load (const juce::PluginDescription& desc, Watchdog& watchdog,
                     bool openEditor = true);

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

    /** Returns false if the audio pump could not be stopped. In that case
        NOTHING is torn down: the instance stays loaded and alive rather than
        being freed underneath a thread that is still rendering it.
    */
    bool unload();

    juce::AudioPluginInstance* getInstance() const noexcept { return instance.get(); }

    /** Owned by the host. Null until load() succeeds with hasEditor. */
    juce::AudioProcessorEditor* getEditor() const noexcept { return editor.get(); }

    /** Detaches the editor so a Component can take ownership for display. */
    std::unique_ptr<juce::AudioProcessorEditor> releaseEditor() { return std::move (editor); }

    PluginIdentity getIdentity() const;

    static constexpr double kSampleRate = 48000.0;
    static constexpr int    kBlockSize  = 512;

    /** Feeds a test signal instead of silence. OFF BY DEFAULT, and it must stay
        that way for capture.

        The pump exists so editors that only paint under a live callback will
        paint, and silence is the right default: a signal changes what the plugin
        DOES while the human is mapping it, which is its own problem. Compressors
        would be gain-reducing, meters would be moving, and a parameter the plugin
        drives from the signal would look like a control the human touched.

        It exists because the noise mask cannot otherwise be demonstrated. A
        gain-reduction readout has nothing to move it under silence, so the mask
        came back empty on every plugin measured and the guard protecting capture
        from meters had never been observed protecting anything. This is also
        M9's render harness arriving early rather than a detour.

        1 kHz sine at -12 dBFS. Loud enough to move a meter, quiet enough not to
        clip anything downstream.
    */
    void setTestSignalEnabled (bool shouldPlay) noexcept { testSignal = shouldPlay; }
    bool isTestSignalEnabled() const noexcept            { return testSignal.load(); }

    static constexpr double kTestToneHz    = 1000.0;
    static constexpr float  kTestToneGain  = 0.251f;   // -12 dBFS

    /** Pauses the silent pump. The probe render in M9 takes the processor
        exclusively and must not race the pump.
    */
    void setPumpEnabled (bool shouldPump) noexcept { pumpEnabled = shouldPump; }

    /** Pauses the pump AND drains the in-flight block before returning.

        Required around anything that MUTATES the instance from another thread
        (the set-then-read sweep, a state restore): setStateInformation is not
        specified to be callable against a concurrent processBlock, so running
        them unserialised is a contract violation whether or not a given
        plugin happens to survive it.

        CORRECTED ATTRIBUTION. The first version of this comment claimed the
        elysia mpressor SIGSEGV as this race, "measured". The next two crash
        reports disproved that: the message thread was still blocked INSIDE
        this function (second report) or the sweep had not begun (third,
        masktest, no mutation anywhere), while the pump died in mpressor's own
        render each time. mpressor crashes under this pump with nothing else
        touching it -- a plugin compatibility fact recorded in its quarantine
        entry, not evidence about this race. The pause stands on the API
        contract, which needs no crash to justify it.

        Clearing the flag alone is not enough: the pump may be mid-block, and
        that block still races the first mutation. Taking processLock after
        clearing the flag blocks until the in-flight processBlock returns;
        subsequent iterations see the flag and skip processing entirely.
    */
    void pausePumpForMutation() noexcept
    {
        pumpEnabled = false;
        const juce::ScopedLock sl (processLock);   // drain the in-flight block
    }

    void resumePumpAfterMutation() noexcept { pumpEnabled = true; }

private:
    void run() override;   // silent pump

    juce::AudioPluginFormatManager& formatManager;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::PluginDescription currentDesc;

    juce::CriticalSection processLock;
    std::atomic<bool> pumpEnabled { false };
    std::atomic<bool> testSignal  { false };

    /** Channel count for the pump's buffer, published BEFORE the thread starts
        and never changed while it runs. The buffer itself is a local inside
        run(), owned solely by the pump thread.

        It used to be a member that load() resized from the message thread with
        no lock. A bridged AU renders out of process, so processBlock hands the
        buffer's channel pointers across an XPC boundary; reallocating it
        underneath that produced a SIGSEGV at 0x0 in renderGetInput, on the
        pump thread, in AUOOPRenderingClient::pullOneInput. A null check inside
        the lock could not have helped: the pointer was read after the check.
    */
    std::atomic<int> pumpChannels { 2 };

    /** How long unload() waits for the pump to stop. Generous, because a
        bridged plugin's processBlock is an XPC round trip; if it still has not
        stopped we refuse to tear down rather than racing it.
    */
    static constexpr int kPumpStopTimeoutMs = 5000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace ejmap
