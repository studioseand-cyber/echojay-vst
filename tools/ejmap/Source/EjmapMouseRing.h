/*
  EjmapMouseRing.h

  Where the mouse was, sampled independently of the parameter poll.

  WHY IT IS NOT PART OF THE POLL. Reading the mouse costs nothing and touches no
  plugin, so it has no business being coupled to a sweep that costs 35 ms on a
  bridged plugin with 633 parameters. The plan sampled the mouse "at the tick
  that detects the change", which ties ui_hint fidelity to the calibrated rate:
  at 8.6 Hz the mouse may have left the control 100 ms before the tick fired.

  So the ring runs at its own 60 Hz and the poll asks it, after the fact, where
  the mouse was at the moment the change was detected.

  PREFER THE LAST MOUSE-DOWN BEFORE THE DETECTION. The instant the human grabbed
  the control is a better answer than wherever the pointer happened to be when a
  poll tick fired. A drag ends with the pointer somewhere along the travel, and a
  release may be followed by the mouse moving away entirely; the grab point is
  what identifies the control. Falling back to the most recent position covers
  host automation, MIDI learn and typed entry, where there is no grab at all.

  A hint whose mouse was outside the editor is recorded as null. The plan is
  explicit about that and it is right: inventing a coordinate is worse than
  admitting there isn't one.
*/

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <vector>

namespace ejmap
{

class MouseRing : private juce::Timer
{
public:
    static constexpr int    kRateHz     = 60;
    static constexpr double kDepthSecs  = 2.0;

    struct Sample
    {
        juce::uint32 atMs = 0;
        juce::Point<int> screenPos;
        bool isDown = false;
        juce::String screen;      // display identity at that moment
    };

    MouseRing() { startTimerHz (kRateHz); }
    ~MouseRing() override { stopTimer(); }

    /** The best answer for a change detected at detectedAtMs: the most recent
        mouse-DOWN at or before it, else the most recent sample. Returns false
        when the ring has nothing old enough, which is a real answer.
    */
    bool bestFor (juce::uint32 detectedAtMs, Sample& out) const
    {
        const juce::ScopedLock sl (lock);

        const Sample* fallback = nullptr;
        const Sample* grab     = nullptr;

        for (const auto& s : ring)
        {
            if (s.atMs > detectedAtMs)
                continue;

            if (fallback == nullptr || s.atMs > fallback->atMs)
                fallback = &s;

            if (s.isDown && (grab == nullptr || s.atMs > grab->atMs))
                grab = &s;
        }

        const Sample* pick = grab != nullptr ? grab : fallback;
        if (pick == nullptr)
            return false;

        out = *pick;
        return true;
    }

    int depth() const { const juce::ScopedLock sl (lock); return (int) ring.size(); }

    /** Display identity for a screen point: index plus scale, because two
        displays at different scale factors are normal here and a bare index
        would not say which geometry the coordinate belongs to.
    */
    static juce::String screenIdFor (juce::Point<int> p)
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* d = displays.getDisplayForPoint (p);
        if (d == nullptr)
            return "unknown";

        int index = 0;
        for (int i = 0; i < displays.displays.size(); ++i)
            if (&displays.displays.getReference (i) == d)
                { index = i; break; }

        return "display" + juce::String (index) + "@" + juce::String (d->scale, 2) + "x";
    }

private:
    void timerCallback() override
    {
        Sample s;
        s.atMs      = juce::Time::getMillisecondCounter();
        s.screenPos = juce::Desktop::getInstance().getMousePosition();
        s.isDown    = juce::Desktop::getInstance().getMainMouseSource().isDragging()
                       || juce::ModifierKeys::currentModifiers.isLeftButtonDown();
        s.screen    = screenIdFor (s.screenPos);

        const juce::ScopedLock sl (lock);
        ring.push_back (s);

        const juce::uint32 cutoff = s.atMs > (juce::uint32) (kDepthSecs * 1000.0)
                                      ? s.atMs - (juce::uint32) (kDepthSecs * 1000.0) : 0;
        while (! ring.empty() && ring.front().atMs < cutoff)
            ring.erase (ring.begin());
    }

    mutable juce::CriticalSection lock;
    std::vector<Sample> ring;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MouseRing)
};

} // namespace ejmap
