#pragma once
// EJKnobGesture (18 Sep 2026, loudness loop): how many knob gestures are open right now. ChainWetKnob (and every
// knob derived from it, including the built-in device knobs) counts itself in on mouseDown and out on mouseUp /
// double-click. The loudness loop never trims while this is non-zero.
#include <atomic>
namespace echojay {
inline std::atomic<int>& openKnobGestures() { static std::atomic<int> n { 0 }; return n; }
inline bool knobGestureOpen() { return openKnobGestures().load (std::memory_order_relaxed) > 0; }
inline void knobGestureBegan() { openKnobGestures().fetch_add (1, std::memory_order_relaxed); }
inline void knobGestureEnded() { int v = openKnobGestures().load (std::memory_order_relaxed); while (v > 0 && ! openKnobGestures().compare_exchange_weak (v, v - 1, std::memory_order_relaxed)) {} }
}
