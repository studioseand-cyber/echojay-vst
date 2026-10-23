#pragma once
// =============================================================================================
//  EJShadowTap - the muted shadow's audio tap (stage A of docs/PLAN_LIVE_SHADOW_2026-10-20.md)
// =============================================================================================
//  Sean's design, 20 Oct 2026: *"The Link streams that slot's input audio to the V2 (shared memory,
//  as the sidecar does for data), and the V2's hosted copy of THAT slot processes it in parallel
//  with its output discarded, so the copy's own meters and GR move like the real one's."*
//
//  ---- THE TAP POINT ALREADY EXISTS, AND IS ALREADY THE RIGHT ONE -----------------------------
//
//  The plan called the slot's input tap "the one genuinely new piece". It is less than that:
//  ChainHost's SlotPreTrim node already taps precisely this signal, and its own comment says why it
//  sits where it does (21t-k item 3, the 28 Sep ruling):
//
//      *"THE IN TAP SITS HERE, AFTER THE PRE-TRIM. It used to ride the blend's DRY leg, which is the
//      signal before this node, so the level sensor measured EchoJay's own staging as if the plugin
//      had done it ... The sensor must measure the PLUGIN, so the in tally is taken from the signal
//      the plugin actually hears."*
//
//  "The signal the plugin actually hears" is the shadow's tap, word for word. So this is a SECOND
//  SINK on an existing node rather than a new node in the graph - no topology change, no extra
//  latency, and nothing to get wrong about ordering.
//
//  ---- ZERO COST WHEN NOTHING IS SHADOWED ----------------------------------------------------
//
//  Sean: *"Only the slot in view is shadowed; stop when he leaves it."*  So exactly one slot is
//  tapped at a time, and the rest must pay nothing. One object is shared by every slot's pre-trim
//  node; each node knows its own index and asks a single relaxed atomic whether it is the one. When
//  no slot is wanted that is one load per slot per block and no copy at all - not "small", nothing.
//
//  ---- AND IT IS THE AUDIO THREAD, SO IT ALLOCATES AND LOCKS NOTHING ------------------------
//
//  The ring is a file-backed mmap opened and closed on the MESSAGE thread; the audio thread sees it
//  only through one atomic pointer. A null pointer is the off state and the only state the audio
//  thread has to understand. The pointer is never freed while a block may be inside push(): the
//  owner clears it, then waits out a block before unmapping (see LinkProcessor's close path, the
//  same deferred-munmap discipline the drain ring already uses).
#include <JuceHeader.h>
#include <atomic>
#include "LinkShm.h"

namespace echojay
{

/** One per rack, shared by every slot's pre-trim node. The audio thread reads two atomics and, when
    it is the wanted slot and a ring is mapped, copies its block in. Everything else is the message
    thread's. */
class ShadowTap
{
public:
    /** THE MESSAGE THREAD'S HALF. `map` is a mapped audio-ring file (LinkShm's layout, the same one
        the drain ring uses) or nullptr to stop. `slot0` is the slot whose input to copy, or -1 for
        none. Setting the slot without a ring, or a ring without a slot, is OFF - both are needed. */
    void setRing (void* map) noexcept { ring_.store (map, std::memory_order_release); }
    void setSlot (int slot0) noexcept { slot_.store (slot0, std::memory_order_release); }
    void stop() noexcept
    {
        // THE SLOT FIRST, THEN THE RING. A block that is already inside push() has passed the slot
        // test, so clearing the ring first could leave it reading a pointer the owner is about to
        // unmap; clearing the slot first means the next block cannot enter at all, and the owner
        // then waits a block before unmapping. Order is the whole of the safety here.
        slot_.store (-1, std::memory_order_release);
        ring_.store (nullptr, std::memory_order_release);
    }
    int  slot() const noexcept { return slot_.load (std::memory_order_relaxed); }
    bool active() const noexcept
    { return slot_.load (std::memory_order_relaxed) >= 0 && ring_.load (std::memory_order_relaxed) != nullptr; }

    /** THE AUDIO THREAD'S HALF, called from the slot's pre-trim node with the signal the plugin is
        about to hear. Returns true when the block was copied, which is what a guard asserts.

        `mySlot` is the caller's own index, so the test is one comparison and no lookup. A mono rack
        duplicates L into R, exactly as the drain ring does, so the V2's copy is handed proper
        dual-mono rather than a dead right channel. */
    bool push (int mySlot, const float* l, const float* r, int numSamples) noexcept
    {
        if (numSamples <= 0 || l == nullptr) return false;
        if (slot_.load (std::memory_order_relaxed) != mySlot) return false;
        auto* map = ring_.load (std::memory_order_acquire);
        if (map == nullptr) return false;
        const float* ch[2] = { l, r != nullptr ? r : l };
        LinkShm::ringProduce (map, ch, 2, numSamples);
        produced_.fetch_add ((juce::uint64) numSamples, std::memory_order_relaxed);
        return true;
    }

    /** How many frames have been copied since this tap was last started. The V2 reads it through
        the sidecar to tell "the shadow is running and silent" from "the shadow is not running" -
        two states that look identical on a meter and must never be confused (the same rule the GR
        window keeps: zero frames means NOTHING WAS MEASURED, never "no signal"). */
    juce::uint64 framesProduced() const noexcept { return produced_.load (std::memory_order_relaxed); }
    void resetFrames() noexcept { produced_.store (0, std::memory_order_relaxed); }

private:
    std::atomic<void*>        ring_ { nullptr };
    std::atomic<int>          slot_ { -1 };
    std::atomic<juce::uint64> produced_ { 0 };
};

/** CAN THIS SLOT BE TAPPED AT ALL? (23 Oct 2026, from the rig's own B9)

    The tap lives on the slot's PRE-TRIM NODE, and ChainHost::rebuildGraph puts that node in the
    graph for a slot that has a plugin and is not bypassed - its own line is
    `for (auto& s : slots_) if (!s.bypassed && s.node)`. A slot that fails either test never sees a
    block, so no frame can ever reach the tap.

    Saying "streaming slot 1" about such a slot is a claim that cannot come true, and the rig read
    exactly that: the Link acked `streaming` and the V2 ran its copy for twenty-five seconds on
    ZERO frames, with no line anywhere saying why. Zero frames and "not started" looked identical,
    which is the shape this house calls unfalsifiable - and it is the same rule the GR window keeps:
    nothing measured is never reported as "no signal".

    So the question is asked BEFORE the claim, and the answer carries its reason in the words the V2
    will show him. Kept here, next to the tap whose rule it states, so the Link's refusal and the
    guard's leg read the same function rather than two copies of one condition. */
inline bool slotCanStream (bool hasPlugin, bool bypassed, juce::String* whyNot = nullptr)
{
    if (! hasPlugin)
    {
        if (whyNot != nullptr) *whyNot = "that slot has no plugin loaded on the Link";
        return false;
    }
    if (bypassed)
    {
        if (whyNot != nullptr) *whyNot = "that slot is bypassed on the Link, so no audio reaches it";
        return false;
    }
    return true;
}

/** The shadow ring's filename for a Link, beside the drain ring's `audio_<name>.bin`. A SEPARATE
    FILE, deliberately: LinkShm's ring has ONE read cursor, so capture, solo and the edit audition
    are already mutually exclusive on the drain ring (its own header says so - *"readIdx is one
    cursor, so a seeking consumer and a capture on the same Link are mutually exclusive"*). Adding
    the shadow as a fourth consumer of that ring would mean the shadow silently stealing frames from
    a bounce, or switching itself off whenever he captures. Its own file costs one mmap and owes
    nothing to anybody. */
inline juce::String shadowRingFileNameFor (const juce::String& uid)
{
    juce::String safe;
    for (auto c : uid)
        safe << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                                                               : juce::String ("_"));
    return "shadow_" + safe + ".bin";
}

}   // namespace echojay
