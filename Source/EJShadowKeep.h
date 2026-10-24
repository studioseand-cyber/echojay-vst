#pragma once
//  EJShadowKeep.h — WHAT HAPPENS TO A LIVE COPY WE HAVE FINISHED WITH (his 14:27 crash, 24 Oct 2026)
//
//  THE ONE SENTENCE: a hosted third-party AU is never disposed mid-session, so the live view hands
//  its copy to this store instead of destroying it - parked for reuse if there is room, kept for
//  ever if there is not.
//
//  THE RULE IS SEAN'S, AND IT WAS ALREADY WRITTEN DOWN. ChainHost.cpp, his ruling of 6 Oct 2026:
//
//      "A HOSTED THIRD-PARTY AU IS NEVER DISPOSED MID-SESSION. The 18:32:09 SIGSEGV. A CL 1B
//       disposed at 18:31:24 on rack release left a Softube NSWindow observer registered, and 44 s
//       later the NLS Buss popout's window frame change posted a notification into it."
//
//  The borrow pool has obeyed that since the day it was written. The live view never did:
//  ShadowView::stop() ended in `dying.reset()`, and his log of 24 Oct has both behaviours one line
//  apart -
//
//      EJShadow:     the V2's copy of "Tube-Tech CL 1B" is released after 1819648 frame(s)
//      EJBorrowPool: PARKED "Tube-Tech CL 1B" - a third-party AU is never disposed mid-session
//
//  - and 108 seconds after that disposal his Logic died on the main thread, inside Softube's own
//  code, under CoreFoundation's notification callout, reading null+0x17, with no EchoJay frame on
//  the thread at all. Same vendor, same plugin, same shape as 18:32.
//
//  HONEST ABOUT THE PROOF. The bench legs that model the ordering (`teardown` in
//  tools/shadow_live_repro) did NOT reproduce his crash: the product's ordering survived 40 s with
//  the real plugin. So this fix does not rest on a reproduction - it rests on his own ruling, on
//  the use-after-free that the ordering half plainly is (proven red-to-green by SH12b, which needs
//  no plugin), and on the crash's own family resemblance. That distinction is written here rather
//  than smoothed over, because "we could not reproduce it" is part of what is known.
//
//  WHY PARK AT ALL, RATHER THAN ONLY KEEP. ChainHost's own reasoning, and it applies unchanged: a
//  cap on a never-freed store cannot free anything, so keeping alone grows with every close and
//  reopen. Parking is bounded by construction - one spare per plugin - and reuse makes selecting
//  the same slot again instant instead of a fresh instantiation.
//
//  WHAT IS NEVER REUSED: a copy whose editor is still attached. If that ever happens the caller
//  broke the ordering (the editor must go before its processor), and handing such an instance back
//  out would hide the fault. It is KEPT, so nothing is disposed, and it is kept where nothing can
//  draw it again.
#include <JuceHeader.h>
#include <memory>
#include <vector>

namespace echojay
{
/** One copy the live view has finished with, filed under the plugin it is a copy of. */
struct ShadowKeptCopy
{
    juce::String                               key;
    std::unique_ptr<juce::AudioPluginInstance> inst;
};

namespace detail
{
// ---- THE POINTER IS STATIC; THE VECTOR IS NEVER DESTROYED --------------------------------------
//
// ChainHost::keepHostedNodeForever's note, and it is load-bearing here for the same reason: "a
// static vector OBJECT would be destroyed during process exit and would dispose these plugins
// there, which is the UAD-2-shaped crash under exit/__cxa_finalize_ranges already on file. A static
// POINTER has a trivial destructor, so the vector it points at - and every instance in it -
// outlives the process cleanly. Do not 'tidy' this into a static object."
inline std::vector<ShadowKeptCopy>& shadowParked()
{
    static auto* const store = new std::vector<ShadowKeptCopy>();
    return *store;
}
inline std::vector<std::unique_ptr<juce::AudioPluginInstance>>& shadowKeptForever()
{
    static auto* const store = new std::vector<std::unique_ptr<juce::AudioPluginInstance>>();
    return *store;
}
} // namespace detail

/** The bounds, the same shape and the same numbers as the borrow pool's (kBorrowPoolPerKey = 1,
    kBorrowPoolMaxTotal = 24): one spare per plugin, and a ceiling on the lot. */
constexpr int kShadowKeepPerKey   = 1;
constexpr int kShadowKeepMaxTotal = 24;

/** Which plugin is this a copy of? Name, format and unique id together, so two plugins that share a
    name (a WaveShell variant, say) are never handed each other's instance. */
inline juce::String shadowKeepKey (const juce::PluginDescription& d)
{
    return d.name + "|" + d.pluginFormatName + "|" + juce::String::toHexString (d.uniqueId);
}

/** Hand over a copy the live view has finished with. The caller passes ownership and keeps nothing.

    Returns true when it was KEPT - parked or kept for ever, but in either case NOT destroyed.
    Returns false only for a processor we wrote ourselves: those are ours, they tear down safely,
    and the caller may let them go. Message thread only, and the copy must already have stopped
    being rendered.

    `whereItWent` gets one plain sentence for the log, so a reader never has to infer which of the
    two stores took it. */
inline bool shadowKeepCopy (std::unique_ptr<juce::AudioProcessor> inst, juce::String& whereItWent)
{
    if (inst == nullptr) { whereItWent = "there was nothing to keep"; return false; }

    auto* hosted = dynamic_cast<juce::AudioPluginInstance*> (inst.get());
    if (hosted == nullptr)
    {
        // Ours. Built-ins dispose safely and always have; only somebody else's code is kept.
        whereItWent = "it is one of ours, so it is disposed here as it always was";
        return false;
    }

    // INERT FIRST, exactly as ChainHost does before it parks a node: suspended, and its render
    // resources released, so nothing it holds is live while it waits.
    inst->suspendProcessing (true);
    inst->releaseResources();

    const auto key = shadowKeepKey (hosted->getPluginDescription());
    const bool editorStillAttached = (inst->getActiveEditor() != nullptr);
    // Held as what it IS from here on, so nothing downstream has to cast it back: the store only
    // ever contains somebody else's plugin, which is the whole point of the store.
    inst.release();
    std::unique_ptr<juce::AudioPluginInstance> typed (hosted);

    int forThisKey = 0;
    for (const auto& p : detail::shadowParked())
        if (p.key == key) ++forThisKey;

    const bool room = forThisKey < kShadowKeepPerKey
                   && (int) detail::shadowParked().size() < kShadowKeepMaxTotal;

    if (room && ! editorStillAttached)
    {
        detail::shadowParked().push_back ({ key, std::move (typed) });
        whereItWent = "PARKED for reuse (" + juce::String ((int) detail::shadowParked().size())
                    + "/" + juce::String (kShadowKeepMaxTotal) + " parked)";
        return true;
    }

    detail::shadowKeptForever().push_back (std::move (typed));
    whereItWent = editorStillAttached
                ? juce::String ("KEPT for ever, and NOT offered for reuse - its editor was still "
                                "attached when it was handed over, which means the editor outlived "
                                "its processor somewhere and that is a fault of its own")
                : juce::String ("KEPT for ever - the park is full ("
                                + juce::String ((int) detail::shadowParked().size()) + "/"
                                + juce::String (kShadowKeepMaxTotal) + ")");
    return true;
}

/** A parked copy of this plugin, if there is one: suspended and unprepared, so the caller must
    un-suspend it and call prepareToPlay before the audio thread can meet it (which shadowBegin
    does for every copy, fresh or reused). nullptr when there is none. */
inline std::unique_ptr<juce::AudioPluginInstance> shadowTakeParked (const juce::PluginDescription& d)
{
    const auto key = shadowKeepKey (d);
    auto& parked = detail::shadowParked();
    for (size_t i = 0; i < parked.size(); ++i)
    {
        if (parked[i].key != key || parked[i].inst == nullptr) continue;
        if (parked[i].inst->getActiveEditor() != nullptr) continue;   // never reuse one still drawn
        auto taken = std::move (parked[i].inst);
        parked.erase (parked.begin() + (long) i);
        return taken;
    }
    return {};
}

/** How many copies are parked for reuse - for the log and for a guard's assertion. */
inline int shadowParkedCount() noexcept { return (int) detail::shadowParked().size(); }

/** How many are parked for one particular plugin (the per-key cap is 1). */
inline int shadowParkedCountFor (const juce::PluginDescription& d)
{
    const auto key = shadowKeepKey (d);
    int n = 0;
    for (const auto& p : detail::shadowParked()) if (p.key == key) ++n;
    return n;
}

/** How many were kept for ever rather than parked. */
inline int shadowKeptForeverCount() noexcept { return (int) detail::shadowKeptForever().size(); }
} // namespace echojay
