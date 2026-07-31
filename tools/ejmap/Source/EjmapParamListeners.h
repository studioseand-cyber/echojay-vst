/*
  EjmapParamListeners.h

  M2 pass three: the listener layer. What the plugin SAYS was touched, next to
  what the poll SAW move.

  WHY GESTURES ARE THE SIGNAL. The poll's one blindness is attribution: it sees
  a delta vector, and one control writing two parameters, a link mirroring a
  value onto its partner, and two parameters that merely correlate are one
  event to it. parameterGestureChanged is the only source in the hosting API
  that names the parameter a human began a gesture ON -- the touched control,
  as opposed to its followers. Established from JUCE 8.0.12 source, not
  assumed: AU GUI touches arrive as kAudioUnitEvent_Begin/EndParameterChange-
  Gesture and are forwarded to beginChangeGesture()/endChangeGesture()
  (juce_AudioUnitPluginFormatImpl.h eventCallback), VST3 touches as
  beginEdit/endEdit (juce_VST3PluginFormatImpl.h), and both notify
  AudioProcessorParameter::Listener synchronously.

  WHAT GESTURES CANNOT BE TRUSTED FOR. Firing is voluntary. A known class of
  plugins never sends gestures; a mirroring plugin may send them on both sides
  of a link, or neither. So gesture evidence RESOLVES a multi-parameter move
  only when exactly one moved index gestured, corroborates a single capture
  when it agrees, and decides nothing when it is silent or ambiguous. The poll
  stays the detector; this layer is the disambiguator, exactly as the plan
  specified.

  THREADING, from the same source reading. Listener callbacks run synchronously
  on whatever thread called setValueNotifyingHost / beginChangeGesture, under
  the parameter's own listenerLock. In this tool that is the message thread
  (GUI touches, synthetic test writes), but nothing in the contract promises
  it, so: value-change tallies are lock-free atomics sized at attach, and
  gesture events go in a small locked list that the callback only appends to.
  Nothing here calls back into the parameter, which would re-take its lock.

  DETACH BEFORE UNLOAD, the same teardown rule the pump SIGSEGV taught: the
  listeners hang off the instance's parameter objects, so they must be removed
  while the instance is still alive. The destructor detaches as a backstop, but
  the unload paths call detach() explicitly, in order, like capture.stop().
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace ejmap
{

class ParamListenerBank : private juce::AudioProcessorParameter::Listener
{
public:
    ParamListenerBank() = default;
    ~ParamListenerBank() override { detach(); }

    void attach (juce::AudioPluginInstance& inst)
    {
        detach();

        const auto& params = inst.getParameters();
        numParams = params.size();
        counts    = std::make_unique<std::atomic<juce::uint32>[]> ((size_t) juce::jmax (1, numParams));
        for (int i = 0; i < numParams; ++i)
            counts[(size_t) i].store (0, std::memory_order_relaxed);

        {
            const juce::ScopedLock sl (lock);
            gestures.clearQuick();
            gesturesDropped = 0;
        }

        instance = &inst;
        for (auto* p : params)
            p->addListener (this);
    }

    void detach()
    {
        if (instance == nullptr)
            return;

        for (auto* p : instance->getParameters())
            p->removeListener (this);
        instance = nullptr;
    }

    bool attached() const noexcept { return instance != nullptr; }

    //==========================================================================
    /** Distinct indices whose gesture BEGAN at or after the given time. */
    juce::Array<int> gestureBeginsSince (juce::uint32 t) const
    {
        juce::Array<int> out;
        const juce::ScopedLock sl (lock);
        for (const auto& g : gestures)
            if (g.starting && g.atMs >= t)
                out.addIfNotAlreadyThere (g.index);
        return out;
    }

    /** Any gesture traffic (begin or end) on this index since the given time.
        This is the promotion probe's question -- "did the plugin report a
        touch here" -- and an end without a begin is still a touch.
    */
    bool sawGestureOn (int index, juce::uint32 since) const
    {
        const juce::ScopedLock sl (lock);
        for (const auto& g : gestures)
            if (g.index == index && g.atMs >= since)
                return true;
        return false;
    }

    juce::uint32 valueChangesOn (int index) const
    {
        return juce::isPositiveAndBelow (index, numParams)
                 ? counts[(size_t) index].load (std::memory_order_relaxed)
                 : 0;
    }

    /** Recorded rather than silently truncating: a full event list drops its
        oldest entry, and the count of drops says how much history is gone.
    */
    int droppedEvents() const { const juce::ScopedLock sl (lock); return gesturesDropped; }

private:
    struct GestureEvent
    {
        int index = -1;
        juce::uint32 atMs = 0;
        bool starting = false;
    };

    static constexpr int kMaxGestureEvents = 512;

    void parameterValueChanged (int idx, float) override
    {
        if (juce::isPositiveAndBelow (idx, numParams))
            counts[(size_t) idx].fetch_add (1, std::memory_order_relaxed);
    }

    void parameterGestureChanged (int idx, bool starting) override
    {
        const juce::ScopedLock sl (lock);
        if (gestures.size() >= kMaxGestureEvents)
        {
            gestures.remove (0);
            ++gesturesDropped;
        }
        gestures.add ({ idx, juce::Time::getMillisecondCounter(), starting });
    }

    juce::AudioPluginInstance* instance = nullptr;
    int numParams = 0;
    std::unique_ptr<std::atomic<juce::uint32>[]> counts;

    mutable juce::CriticalSection lock;
    juce::Array<GestureEvent> gestures;
    int gesturesDropped = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamListenerBank)
};

} // namespace ejmap
