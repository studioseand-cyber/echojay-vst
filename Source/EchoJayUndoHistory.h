// EchoJayUndoHistory.h — ONE plugin-wide undo history for EchoJay V2 (21n item 3, 22 Sep 2026, design accepted as ten lines).
//
// Entries are DATA, not closures: {kind, target, before, after, label, timeMs, coalesceKey}. The processor's dispatcher
// (EchoJayProcessor::applyUndoEntry) knows every kind and finds the target by identity at apply time: a rack by uid ("" =
// the local rack), a slot by identity "uid|name" (index as the fallback), a Link by uid. An entry whose target is no
// longer present is SKIPPED with a status line, never applied to a different target. 50 deep. A push with the same
// coalesceKey inside 300 ms extends the previous entry's `after` (one entry per knob or hosted-window gesture). Pushes
// while an undo/redo is being applied, or inside a ScopedSuppress (session load), record nothing.
//
// Kinds (what `before` / `after` hold):
//   chain       the 21m rack snapshot {slots, state, params} of the rack `target` (after = captured when undone)
//   wet / trim / keep   a float / float / bool on the slot `target`
//   dial        {index: normalised} maps of the slot's hosted parameters before / after an assistant apply
//   gesture     one hosted parameter {index, value} before / after the user's own gesture in the plugin window
//   linkActive / linkGain   bool / float on the Link `target` (re-sent as a ctrl-cmd)
//   alias       the V2 alias string for the Link `target`
//   loop        the Level slot's gain_db before / after a loop apply (the loop's own Undo pill becomes an entry)
//   group       (item 4) each member's trim before / after a group level move
#pragma once
#include <JuceHeader.h>
#include <deque>
#include <functional>
namespace echojay
{
struct UndoEntry
{
    juce::String kind, target, label, coalesceKey;
    juce::var    before, after;
    juce::int64  timeMs = 0;
};
class UndoHistory
{
public:
    static constexpr int kDepth = 50;
    static constexpr int kCoalesceMs = 300;
    // the dispatcher: apply `e` toward `before` (undo) or `after` (redo); false = the target is absent (skipped)
    std::function<bool (UndoEntry&, bool toBefore)> apply;
    std::function<void (const juce::String&)> status;   // one line the user sees (skips)
    std::function<void()> onChanged;                    // buttons / tooltips follow

    void push (UndoEntry e)
    {
        if (suppressed_ > 0) return;
        if (e.timeMs == 0) e.timeMs = juce::Time::currentTimeMillis();
        if (e.coalesceKey.isNotEmpty() && ! undo_.empty())
        {
            auto& last = undo_.back();
            if (last.coalesceKey == e.coalesceKey && last.kind == e.kind && last.target == e.target && e.timeMs - last.timeMs <= kCoalesceMs)
            { last.after = e.after; last.timeMs = e.timeMs; redo_.clear(); if (onChanged) onChanged(); return; }
        }
        undo_.push_back (std::move (e));
        while ((int) undo_.size() > kDepth) undo_.pop_front();
        redo_.clear();
        if (onChanged) onChanged();
    }
    bool undo() { return step (undo_, redo_, true); }
    bool redo() { return step (redo_, undo_, false); }
    bool canUndo() const { return ! undo_.empty(); }
    bool canRedo() const { return ! redo_.empty(); }
    int  undoDepth() const { return (int) undo_.size(); }
    int  redoDepth() const { return (int) redo_.size(); }
    juce::String undoLabel() const { return undo_.empty() ? juce::String() : undo_.back().label; }
    juce::String redoLabel() const { return redo_.empty() ? juce::String() : redo_.back().label; }
    const UndoEntry* top() const { return undo_.empty() ? nullptr : &undo_.back(); }
    bool applying() const { return suppressed_ > 0; }
    struct ScopedSuppress { UndoHistory& h; explicit ScopedSuppress (UndoHistory& hh) : h (hh) { ++h.suppressed_; } ~ScopedSuppress() { --h.suppressed_; } };
    void clear() { undo_.clear(); redo_.clear(); if (onChanged) onChanged(); }
private:
    bool step (std::deque<UndoEntry>& from, std::deque<UndoEntry>& to, bool toBefore)
    {
        if (from.empty()) return false;
        UndoEntry e = std::move (from.back()); from.pop_back();
        bool ok = false;
        { ScopedSuppress s (*this); ok = apply ? apply (e, toBefore) : false; }
        if (! ok && status) status (juce::String (toBefore ? "Undo skipped: " : "Redo skipped: ") + e.label + " - its target is no longer present");
        to.push_back (std::move (e));
        while ((int) to.size() > kDepth) to.pop_front();
        if (onChanged) onChanged();
        return true;
    }
    std::deque<UndoEntry> undo_, redo_;
    int suppressed_ = 0;
};
} // namespace echojay
