#pragma once
// =============================================================================================
//  EJPluginCallWindow - "the message thread is inside a hosted plugin right now"
// =============================================================================================
//  WHY THIS EXISTS: his 08:58 crash on 24 Oct 2026, the third in the live view and the first that
//  was NOT the channel count.
//
//  The crash report holds both halves of it in one picture. The MAIN thread:
//
//      AudioUnitPluginWindowCocoa::AudioUnitPluginWindowCocoa
//        <- juce::AudioUnitPluginInstance::createEditor
//        <- ChainHost::createEditorForSlot
//        <- ChainListPanel::showInline
//        <- EchoJayEditor::shadowFollowSelection's lambda
//        <- ChainHost::asyncCreatePlugin's callback          (still inside the instantiate)
//
//  ...eighteen seconds deep in the Tube-Tech CL 1B's own interface, compiling Metal render
//  pipelines. And the RENDER thread, at the same instant:
//
//      Tube-Tech CL 1B's own render code, SIGSEGV at 0x0
//        <- AudioUnitRender
//        <- juce::AudioUnitPluginInstanceHeadless::processAudio
//        <- EchoJayProcessor::processBlock                   (the live view's copy, pumping)
//
//  Our own deadman wrote the same thing down independently: TWO marks were in flight when the
//  process died, "instantiate" and "editor creation", both naming that plugin.
//
//  So the sequence was ours, and it was backwards. shadowBegin switched the copy on - the audio
//  thread began rendering that plugin on the next block - and the VERY NEXT STATEMENT went off to
//  build a plugin window, on the message thread, for eighteen seconds. Three vendors have now died
//  in that window (WaveShell, UnFairchild, Softube), which is the shape of a rule being broken
//  rather than three bugs.
//
//  ---- THE RULE ------------------------------------------------------------------------------
//
//  THE LIVE COPY DOES NOT RENDER WHILE THE MESSAGE THREAD IS INSIDE A HOSTED PLUGIN.
//
//  Not "locks around the instance": the two calls were to two DIFFERENT instances of the same
//  plugin, so no per-instance lock could have seen it. What the two had in common was the plugin's
//  own per-process machinery, and the only thing that covers that is the window itself.
//
//  It costs nothing where it bites. A copy whose picture is still being built has no meters to
//  move yet, and a few skipped blocks on a view whose output is discarded are not audible by
//  construction - nothing here is in the audio path (that is the whole premise of the live view).
//
//  ---- WHO RAISES IT -------------------------------------------------------------------------
//
//  The count is raised by the death marks that already bracket exactly these windows (instantiate,
//  held through the caller's callback; editor creation; state restore), so every road that loads or
//  opens a plugin is covered without being asked - the marks and this window are one fact with two
//  readers. Editor DESTRUCTION is bracketed explicitly, because the deadman deliberately does not
//  mark it (a death there must not blacklist the plugin: see ChainHost's own note).
#include <atomic>

namespace echojay
{

/** How many hosted-plugin calls are in flight on a non-audio thread. One count for the process:
    the plugin machinery the crash went through is per-process, not per-instance. */
inline std::atomic<int>& pluginCallsInFlight() noexcept
{
    static std::atomic<int> n { 0 };
    return n;
}

/** The audio thread's question, and the only one it asks. */
inline bool insidePluginCall() noexcept
{
    return pluginCallsInFlight().load (std::memory_order_acquire) > 0;
}

/** RAII for a call into a hosted plugin from the message thread. */
struct PluginCallWindow
{
    PluginCallWindow() noexcept  { pluginCallsInFlight().fetch_add (1, std::memory_order_release); }
    ~PluginCallWindow() noexcept { pluginCallsInFlight().fetch_sub (1, std::memory_order_release); }
    PluginCallWindow (const PluginCallWindow&) = delete;
    PluginCallWindow& operator= (const PluginCallWindow&) = delete;
};

/** ---- WHOSE COPY IS ON SCREEN? (the second half of the 08:58 crash) ------------------------

    The crash's stack says `ChainHost::createEditorForSlot`, which is the BORROWED rack's mirror -
    not the live view's copy. So with a Link selected and a lease held (the ordinary state when he
    is working on a Link), the editor he was shown belonged to one instance of the plugin while the
    audio was being fed to ANOTHER. Two live copies of one plugin, and the picture was the one with
    no audio in it.

    That is also, on the evidence, what his 16:35 report was: *"The V2 shows the X-Limit's real
    interface, so the view comes up, but NOTHING moves."*  The channel count was a real crash and is
    really fixed; the STILL METERS were this - he was looking at the copy that was never fed.

    THE DECISION LIVES HERE so a test can construct the states and call it. That is not a
    preference: this same function's borrowed arm was dead code for a fortnight in Aug 2026 because
    the order of two early returns made it unreachable, and the lesson written beside it then was
    that an ordering decision belongs where a guard can ask it directly. */
enum class SlotEditorSource { None, LiveView, Borrowed, EditSession, LocalRack };

inline SlotEditorSource slotEditorSource (bool viewIsRemote,
                                          bool liveViewOnThisSlot,
                                          bool borrowActiveForView,
                                          bool editSessionOnThisSlot) noexcept
{
    // The V2's own rack: its slots are hosted here and their editors have always been the real
    // thing. Nothing below applies.
    if (! viewIsRemote) return SlotEditorSource::LocalRack;

    // THE LIVE VIEW WINS, AND THIS IS THE LINE THE CRASH CAME FROM. It used to sit last, below the
    // borrowed arm, so it was only ever reached when no lease was held - which is to say almost
    // never, on the one road it was built for. First, now, for the two reasons the crash gave:
    //   - it is the copy that is actually being fed audio, so it is the only one whose meters can
    //     move, which is the entire point of the feature;
    //   - taking it means no SECOND instance of the plugin is created or opened for this slot at
    //     all, so the overlap that killed the process cannot be built in the first place.
    // Narrow by construction: only for the exact uid and slot the copy is running on.
    if (liveViewOnThisSlot) return SlotEditorSource::LiveView;

    // A stage-1 edit session's slot has a local editing copy, and its editor is the point of it.
    if (editSessionOnThisSlot) return SlotEditorSource::EditSession;

    // The borrowed mirror, which is the right answer for every OTHER slot of a leased rack.
    if (borrowActiveForView) return SlotEditorSource::Borrowed;

    // A remote slot with nothing of ours standing in for it: the static picture and its one line.
    return SlotEditorSource::None;
}

}   // namespace echojay
