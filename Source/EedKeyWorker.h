/*
    EedKeyWorker.h  —  the background cadence for an EedKeyEngine.

    The engine is JUCE-free and deliberately does not own a thread; whoever
    hosts it decides where update() runs. Both hosts (the Key Detector chain
    device and the Link's frame publisher) want exactly the same thing — poll
    update() a few times a second off the audio and message threads — so the
    thread lives here once.

    A committed pass over a 30 s window costs on the order of 100 ms; that is
    nothing on this thread and a visible stutter anywhere else. When no new
    audio has arrived the engine's update() returns almost immediately, so an
    idle worker costs a wakeup every 250 ms and nothing more.
*/

#pragma once

#include <JuceHeader.h>
#include "EedKeyEngine.h"

class EedKeyWorker : public juce::Thread
{
public:
    explicit EedKeyWorker (echojay::KeyEngine& engine)
        : juce::Thread ("EchoJay Key Analysis"), engine_ (engine) {}

    ~EedKeyWorker() override
    {
        // Generous: the timeout has to outlast a worst-case pass in flight.
        stopThread (3000);
    }

    // Millisecond stamp (juce::Time::getMillisecondCounter clock) of the last
    // time the engine's published reading CHANGED. 0 = never. This is what
    // turns into the frame's keyAgeMs / the feed's "age: 4 s" — the engine
    // itself is JUCE-free and cannot timestamp.
    juce::uint32 lastChangeMs() const noexcept
    {
        return lastChangeMs_.load (std::memory_order_relaxed);
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            if (engine_.update())
                lastChangeMs_.store (juce::Time::getMillisecondCounter(),
                                     std::memory_order_relaxed);
            wait (250);
        }
    }

private:
    echojay::KeyEngine& engine_;
    std::atomic<juce::uint32> lastChangeMs_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedKeyWorker)
};
