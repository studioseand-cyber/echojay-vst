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
#include <algorithm>   // 24 Oct: planRemoteUndoRemoves sorts descending
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
    bool undo() { return step (undo_, redo_, true, -1); }
    /** ---- A WALK WITH A FLOOR (his 09:15 fault, 24 Oct 2026) ---------------------------------

        *"Undo all removed the WRONG plugin ... it removed the EchoJay EQ from slot 1 and left the
         bx_opto."*

        His log, in one line: *"undo all walked the history back to the run's checkpoint - 1 step(s),
        depth now 0 (the run started at 4)"*.  The run started at depth 4 and the walk ended at ZERO.
        Four entries belonging to EARLIER runs were popped and one of them was applied - an add of
        the EchoJay EQ from before he ever typed this prompt - so "undo this run" deleted a plugin
        this run had never touched.

        The crossing happens INSIDE one undo(): the caller's loop checks the depth against the
        checkpoint before each call, but step() walks past every entry it cannot apply (the 14 Oct
        fix 4(b), which is right for a single press) and keeps going until one applies. With the
        run's own entries unidentifiable, "keeps going" meant "reaches into the history before the
        run". The caller could not have prevented it, because by the time it saw the depth again the
        plugin was gone.

        So the floor belongs to the walker, which is the only thing that knows where it is. At or
        below it, nothing is popped, nothing is applied, and everything already walked past is put
        back exactly as it was. That is his rule in the walker's own terms: *"if it can't identify
        the instance for certain, it removes nothing and says so plainly."* */
    bool undoNotBelow (int floorDepth) { return step (undo_, redo_, true, floorDepth); }
    /** Whether the LAST undo()/redo() actually reached its target. step() returns true whenever the stack was
        not empty - including when apply() skipped because the target had gone - so "the stack moved" and "the
        change was reversed" are two different facts and a caller that reports the first as the second is
        lying. The agent's undo op reads this (Sean's ruling 4, 14 Oct 2026); the UI reads the status line. */
    bool lastApplyOk() const noexcept { return lastApplyOk_; }
    bool redo() { return step (redo_, undo_, false, -1); }
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
    /** FIX 4(b) (Sean, 14 Oct): A DEAD ENTRY MUST NOT COST THE USER A PRESS.
        This popped ONE entry, and when its target had gone it wrote "Undo skipped: ..." and returned true -
        so the press was spent and nothing was undone. Sean pressed twice and burned two dead entries:
          17:46:33  EJUndo: Undo skipped: remove NLS Buss (s) - its target is no longer present
          17:46:37  EJUndo: Undo skipped: remove UAD UA 1176LN Rev E - its target is no longer present
        It now walks on to the next entry that CAN be applied, and says how many it passed. Bounded by the
        stack itself (every iteration pops), so it cannot spin; and it stops the moment one applies, so it
        can never undo two real steps for one press - which would be the opposite fault and a worse one. */
    bool step (std::deque<UndoEntry>& from, std::deque<UndoEntry>& to, bool toBefore, int floorDepth)
    {
        if (from.empty()) return false;
        juce::StringArray skippedLabels;
        std::vector<UndoEntry> dead;        // popped, could not be applied - see the restore below
        for (;;)
        {
            // ---- THE FLOOR, CHECKED BEFORE EVERY POP (his 09:15 fault, 24 Oct 2026) ----------
            //
            // Not once on entry: the walk below pops repeatedly, and it was the SECOND and later
            // pops that crossed into the history from before the run. See undoNotBelow.
            if (floorDepth >= 0 && (int) from.size() <= (size_t) floorDepth)
            {
                for (auto it = dead.rbegin(); it != dead.rend(); ++it) from.push_back (std::move (*it));
                lastApplyOk_ = false;
                if (status)
                    status (skippedLabels.isEmpty()
                                ? juce::String ("There is nothing of this run left to undo.")
                                : juce::String ("I could not undo this run: ")
                                  + juce::String (skippedLabels.size()) + " step"
                                  + (skippedLabels.size() == 1 ? "" : "s")
                                  + " point at something that is no longer where it was ("
                                  + skippedLabels.joinIntoString (", ")
                                  + "), and I will not remove anything else in their place");
                if (onChanged) onChanged();
                return true;
            }
            UndoEntry e = std::move (from.back()); from.pop_back();
            bool ok = false;
            { ScopedSuppress s (*this); ok = apply ? apply (e, toBefore) : false; }
            lastApplyOk_ = ok;
            const juce::String label = e.label;
            if (ok)
            {
                to.push_back (std::move (e));
                while ((int) to.size() > kDepth) to.pop_front();
                // The entries stepped over go to the same place the applied one does: they have been walked
                // past, and leaving them on the undo stack would hand the next press the same dead ends.
                for (auto it = dead.rbegin(); it != dead.rend(); ++it)
                {
                    to.push_back (std::move (*it));
                    while ((int) to.size() > kDepth) to.pop_front();
                }
                // What was undone, and what it had to step over to get there. Said in one line, because two
                // lines about one press reads as two things having happened.
                if (! skippedLabels.isEmpty() && status)
                    status ((toBefore ? "Undone: " : "Redone: ") + label + " (skipped "
                            + juce::String (skippedLabels.size())
                            + " step" + (skippedLabels.size() == 1 ? "" : "s")
                            + " whose target is no longer present: " + skippedLabels.joinIntoString (", ") + ")");
                if (onChanged) onChanged();
                return true;
            }
            dead.push_back (std::move (e));
            skippedLabels.add (label);
            if (from.empty())
            {
                // NOTHING COULD BE APPLIED, SO NOTHING IS SPENT. The entries go back exactly as they were:
                // a press that can do nothing must not empty the history, because every one of those steps
                // becomes undoable again the moment its Link is back - which is what remote_ops_guard's
                // STAGE 5 means by "a skip loses nothing and the user can try again". The older behaviour
                // popped one dead entry per press and reported success, which is the half of Sean's item 4
                // that lost his pre-gain move; walking the stack fixes that, and restoring it here is what
                // stops the fix from costing the user their history.
                for (auto it = dead.rbegin(); it != dead.rend(); ++it) from.push_back (std::move (*it));
                if (status)
                    status (juce::String (toBefore ? "Nothing can be undone right now: " : "Nothing can be redone right now: ")
                            + juce::String (skippedLabels.size()) + " step" + (skippedLabels.size() == 1 ? "" : "s")
                            + " on the history point at something no longer present ("
                            + skippedLabels.joinIntoString (", ") + "), and they are kept in case it comes back");
                if (onChanged) onChanged();
                return true;
            }
        }
    }
    std::deque<UndoEntry> undo_, redo_;
    int suppressed_ = 0;
    bool lastApplyOk_ = false;
};

/** ---- WHICH SLOTS DOES AN UNDO OF A REMOTE ADD REMOVE? (his 09:15 rule, 24 Oct 2026) ----------

    *"undo removes exactly the plugin instance the run added, found by its identity (the slot's
     instance id or uid recorded at the add), never by position. If it can't identify the instance
     for certain, it removes nothing and says so plainly."*

    A POSITION CANNOT BE RECORDED AT ALL, which is what his fault proved: a positioned add does not
    land where an append would, and even an append's number stops being true the moment anything
    below it moves. So the entry carries the NAMES and this resolves them against the rack as it is
    at the moment of the undo.

    THE WIRE HAS NO PER-INSTANCE ID. A sidecar slot carries the PLUGIN's identity (name, fp, uid,
    version) and two copies of one plugin share every one of them. So "exactly one slot holds this
    name" is the strongest honest claim available, and when two do, this says Ambiguous and the
    caller removes nothing - his rule's second half, and the only answer that cannot delete the
    wrong plugin.

    A PURE FUNCTION, deliberately: his fault was an ordering decision buried in a road no test could
    drive, and this house has that lesson written in three places already. The guard constructs the
    racks - moved, shrunk, duplicated - and calls this. */
struct RemoteUndoPlan
{
    enum class Outcome
    {
        Ok,              // every name resolved to exactly one slot
        Ambiguous,       // at least one name is on the rack twice: nothing is removed
        NothingLeft,     // nothing this entry added is on the rack any more
        RackUnreadable   // no rack to check against: nothing is removed
    };
    Outcome outcome = Outcome::NothingLeft;
    juce::Array<int>  slots1Based;    // DESCENDING, so no remove shifts a slot a later remove names
    juce::StringArray gone;           // names no slot holds
    juce::StringArray ambiguous;      // names more than one slot holds, with where
};

inline RemoteUndoPlan planRemoteUndoRemoves (const juce::StringArray& rackSlotNames,
                                             bool rackReadable,
                                             const juce::StringArray& addedNames)
{
    RemoteUndoPlan plan;
    if (! rackReadable) { plan.outcome = RemoteUndoPlan::Outcome::RackUnreadable; return plan; }
    for (const auto& raw : addedNames)
    {
        const auto want = raw.trim();
        if (want.isEmpty()) continue;
        juce::Array<int> holders;
        for (int i = 0; i < rackSlotNames.size(); ++i)
            if (rackSlotNames[i].trim().equalsIgnoreCase (want)) holders.add (i + 1);
        if (holders.isEmpty()) { plan.gone.add (want); continue; }
        if (holders.size() > 1)
        {
            juce::StringArray where;
            for (auto h : holders) where.add ("slot " + juce::String (h));
            plan.ambiguous.add (want + " (" + where.joinIntoString (" and ") + ")");
            continue;
        }
        plan.slots1Based.addIfNotAlreadyThere (holders[0]);
    }
    // AMBIGUITY POISONS THE WHOLE ENTRY, not just its own name. Removing the ones it could identify
    // and leaving the rest would be a half-undo nobody asked for, and the user would have no way of
    // knowing which half ran.
    if (! plan.ambiguous.isEmpty())
    {
        // AND THE PLAN CARRIES NOTHING TO REMOVE, not merely an outcome saying not to. The names it
        // HAD resolved were still sitting in the list, so any future caller that read the slots
        // without first reading the outcome would remove half of them - which is the same shape of
        // trap as the one that deleted his EchoJay EQ. RU3 caught it here.
        plan.slots1Based.clear();
        plan.outcome = RemoteUndoPlan::Outcome::Ambiguous;
        return plan;
    }
    if (plan.slots1Based.isEmpty()) { plan.outcome = RemoteUndoPlan::Outcome::NothingLeft; return plan; }
    std::sort (plan.slots1Based.begin(), plan.slots1Based.end(), std::greater<int>());
    plan.outcome = RemoteUndoPlan::Outcome::Ok;
    return plan;
}

} // namespace echojay
