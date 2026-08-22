#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <vector>

// ============================================================================
// dial-4 A8: the attempt tally — the denominator becomes marginal.
// HANDOVER/CONTRACT_racked_slot_controls.md §A8; the client half is specified
// ONLY by that text.
//
// Everything here is PURE and HEADER-INLINE on purpose: tools/mapfps_test
// compiles these directly, so a new symbol is never paired with the previous
// build's lib (the stale-lib trap, third sighting), and the two A8 pins the
// contract names are assertable without an editor or a rack.
//
// The tally is a DELTA since the last successful send (A8.4): accumulated at
// the settle walkers and the refine sites, persisted beside the dial-3
// watermark, staged into the A7.3 envelope at body build, and subtracted only
// on send success. One entry per slot identity (format|uid) touched — the
// A8.2 cut; app_version rides the carrying body, never the entries.
// ============================================================================
namespace echojay
{

struct DialAttemptTally
{
    struct Entry
    {
        juce::String format, uid;   // "" | "" is legal: identityless refine
                                    // contributions count under format|| and
                                    // the server flags them, never drops (A2)
        int          applies   = 0; // slot-turns COMPLETED, clean included
        juce::int64  requested = 0; // sum of per-slot-turn requested counts
    };
    std::vector<Entry> entries;

    Entry& entryFor (const juce::String& format, const juce::String& uid)
    {
        for (auto& e : entries)
            if (e.format == format && e.uid == uid) return e;
        entries.push_back ({ format, uid, 0, 0 });
        return entries.back();
    }

    /** The one accumulation primitive. bumpApplies encodes A8.1a's once-rule:
        exactly one of a slot-turn's sites passes true. Negative counts floor
        to 0 (the A8.6 junk guard, applied at the source too). */
    void note (const juce::String& format, const juce::String& uid,
               int requestedCount, bool bumpApplies)
    {
        auto& e = entryFor (format, uid);
        if (bumpApplies) ++e.applies;
        e.requested += juce::jmax (0, requestedCount);
    }

    bool empty() const
    {
        return std::all_of (entries.begin(), entries.end(),
                            [] (const Entry& e) { return e.applies == 0 && e.requested == 0; });
    }

    int totalApplies() const
    {
        int n = 0; for (auto& e : entries) n += e.applies; return n;
    }
    juce::int64 totalRequested() const
    {
        juce::int64 n = 0; for (auto& e : entries) n += e.requested; return n;
    }

    /** Subtract a shipped snapshot (A8.4: reset only on send success — the
        caller subtracts exactly what it staged, so applies accumulated
        BETWEEN stage and success survive). Entries at zero are dropped. */
    void subtract (const DialAttemptTally& shipped)
    {
        for (const auto& s : shipped.entries)
        {
            auto& e = entryFor (s.format, s.uid);
            e.applies   = juce::jmax (0, e.applies - s.applies);
            e.requested = juce::jmax ((juce::int64) 0, e.requested - s.requested);
        }
        entries.erase (std::remove_if (entries.begin(), entries.end(),
                           [] (const Entry& e) { return e.applies == 0 && e.requested == 0; }),
                       entries.end());
    }

    /** The wire "attempts" array (A8.1). Caller caps at 32 BEFORE calling
        (overflow waits for the next turn, never truncates silently). */
    juce::var toAttemptsVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& e : entries)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("format",    e.format);
            o->setProperty ("uid",       e.uid);
            o->setProperty ("applies",   e.applies);
            o->setProperty ("requested", (double) e.requested);
            arr.add (juce::var (o));
        }
        return juce::var (arr);
    }

    static DialAttemptTally fromAttemptsVar (const juce::var& v)
    {
        DialAttemptTally t;
        if (auto* arr = v.getArray())
            for (auto& ev : *arr)
                if (auto* o = ev.getDynamicObject())
                {
                    auto& e = t.entryFor (o->getProperty ("format").toString(),
                                          o->getProperty ("uid").toString());
                    e.applies   += juce::jmax (0, (int) o->getProperty ("applies"));
                    e.requested += juce::jmax ((juce::int64) 0,
                                       (juce::int64) (double) o->getProperty ("requested"));
                }
        return t;
    }
};

// ----------------------------------------------------------------------------
// The site policies — the A8.1a once-rule and the A8.1b exclusion as
// FUNCTIONS, so the call sites share one implementation and the pins test the
// thing the sites actually call rather than a restatement of it.
// ----------------------------------------------------------------------------

/** A8.1b: a built-in slot-turn contributes NOTHING. What can never appear in
    the numerator does not belong in the denominator. Client-enforced and
    server-unverifiable — the pin in tools/mapfps_test is the only place this
    can ever be caught. */
inline bool dialTallyAdmits (bool isBuiltinSlot) { return ! isBuiltinSlot; }

/** A8.1b, CORRECTED 22 Aug (Sean's ruling): built-ins leave the ROWS as well
    as the tally, so the clause's premise — built-ins can never reach the
    numerator — becomes true rather than accepted-as-violated. Same admission
    rule as the tally, stated separately so each side's exclusion is pinned
    against the function its emitter actually calls. The server cannot enforce
    this: an empty uid is also a legitimate unknown-third-party shape. */
inline bool dialRowAdmits (bool isBuiltinSlot) { return ! isBuiltinSlot; }

/** A7.2's "keys" count semantic, THE one implementation: top-level flat keys,
    plus entries of "controls", plus "bands" elements — the same granularity
    applySettings reports one result per. A8.1a binds the tally to this
    semantic at every keys-sourced site: getDialInfos' fallback used to count
    TOP-LEVEL PROPERTIES (a controls object with five entries counted as one),
    so the numerator and denominator diverged structurally on exactly the
    slots the apply never saw. Rows and tally both count through here now. */
inline int requestedEntryCount (const juce::var& structured)
{
    auto* o = structured.getDynamicObject();
    if (o == nullptr) return 0;
    int n = 0;
    for (auto& kv : o->getProperties())
    {
        const auto k = kv.name.toString();
        if (k == "controls")
        {
            if (auto* co = kv.value.getDynamicObject()) n += co->getProperties().size();
        }
        else if (k == "bands")
        {
            if (auto* ba = kv.value.getArray()) n += ba->size();
        }
        else
            ++n;
    }
    return n;
}

/** The settle-walker site: one slot-turn whose dial state settled (clean
    included). Bumps applies — this is the site that owns the once-rule's
    single increment. */
inline void noteDialApplySite (DialAttemptTally& t, const juce::String& format,
                               const juce::String& uid, int requestedCount,
                               bool isBuiltinSlot)
{
    if (! dialTallyAdmits (isBuiltinSlot)) return;
    t.note (format, uid, requestedCount, true);
}

/** The refine site: dropped_controls entries, absent from report.size() by
    construction (A7.2), identityless on purpose (the rows are too — a wrong
    uid is worse than an empty one). opReachesApply is the A8.1a once-rule:
    an op whose apply also runs contributes requested ONLY (the apply site
    already bumped applies); a receipt-consumed op never reaches the apply,
    so this is its one site and it bumps. The natural implementation bumps at
    both sites and double-counts the population — the pin exists for that. */
inline void noteDialRefineSite (DialAttemptTally& t, bool opReachesApply,
                                int droppedCount, bool isBuiltinSlot)
{
    if (! dialTallyAdmits (isBuiltinSlot) || droppedCount <= 0) return;
    t.note ({}, {}, droppedCount, ! opReachesApply);
}

} // namespace echojay
