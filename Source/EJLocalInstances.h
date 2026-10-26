#pragma once

#include <JuceHeader.h>
#include "EJProcessGlobal.h"

#include <atomic>
#include <thread>
#include <unistd.h>
#include <cstring>

/** THE PROCESS-LOCAL DIRECTORY OF LIVE ECHOJAY INSTANCES, AND THE EXCLUSIVE EDITOR LEASE.
    Round 1 of docs/LIVE_VIEW_REAL_EDITOR_2026-10-26.md §2 (lifetime, lease and ordering).
    26 Oct 2026, his ruling: *"round 1 green in the rig before the V2 ever draws a lent editor"*.

    WHAT THIS IS FOR. Where the V2 and a Link share a process (Logic and Pro Tools, measured - §1 of
    the design), the live view can show the Link's OWN hosted plugin editor instead of a copy of the
    plugin. Everything that makes that safe is here, and nothing here draws anything: this round is
    the directory, the lease and the teardown orders, so the dangerous half is provable before a
    single lent editor reaches a window.

    THE RULES, which are the design's words made executable:

      1. ONE EDITOR PER INSTANCE. juce::AudioProcessor holds one activeEditor and two parents is
         undefined behaviour, so a slot's editor is leased EXCLUSIVELY and one place answers "who has
         it": heldBy() -> None | OwnWindow (the Link's own plugin window) | Borrowed (a V2's pane).
      2. THE OWNER CREATES AND DESTROYS IT. The borrower is handed a non-owning pointer and a token.
      3. THE EDITOR IS NEVER MOVED BETWEEN WINDOWS. Whoever shows it creates it; a hand-over is a
         destroy and a create. (This is not politeness: re-parenting a hosted AU's NSView through a
         nil window is what crashed Logic on 26 Oct - docs/LIVE_VIEW_PATH_26oct.md §4.1.)
      4. THE REVOKE ORDER IS FIXED, and it is the whole risk: tell the borrower to let go
         (synchronously, on the message thread), THEN destroy the editor, THEN park the instance.
         Every teardown door funnels through revokeSlot/revokeFrom/revokeAll so there is one order
         rather than one per door.
      5. A SLOT INDEX IS NOT A PLUGIN (the lesson of the morning of 26 Oct, four places deep). A
         structural change at slot n therefore revokes every lease from n upwards: the leases above
         it would otherwise point at a different plugin than the one they were taken on.
      6. A BORROWER THAT GOES WITHOUT RETURNING IS A BUG, not a case to tolerate - so it is made
         impossible rather than handled: the borrower de-registers in its own destructor
         (borrowerGone), which returns every lease it still holds.

    THREADS. Message thread only, every call. The table's own spinlock exists because two bundles
    reach it (the V2's image and the Link's), not because this is concurrent: the sections are a
    memcpy long, and every callback OUT of this file is made with the lock released. */
namespace echojay
{

/** The side that owns a hosted plugin and therefore its editor: a Link. */
struct EditorLender
{
    virtual ~EditorLender() = default;
    /** Message thread. Create this slot's editor if it does not exist yet and return it, or nullptr
        with a sentence in `why` (a built-in we draw ourselves, an editor that would not create). The
        LENDER keeps ownership. */
    virtual juce::AudioProcessorEditor* lenderCreateSlotEditor (int slot1Based, juce::String& why) = 0;
    /** Message thread. Destroy the editor this lender made for that slot. Called AFTER the borrower
        has let go, never before. */
    virtual void lenderDestroySlotEditor (int slot1Based) = 0;
    /** For the log and the rig's evidence: "EchoJay Link \"Kick\" (uid 4814a16004)". */
    virtual juce::String lenderDescribe() const = 0;
};

/** The side that shows one: a V2's live-view pane, or the Link's own plugin window. */
struct EditorBorrower
{
    virtual ~EditorBorrower() = default;
    /** Message thread, SYNCHRONOUS: let go of that editor now - it is destroyed as soon as this
        returns. Remove it from your window, drop every pointer to it, and do not post anything. */
    virtual void editorLeaseRevoked (const juce::String& uid, int slot1Based, const juce::String& why) = 0;
    virtual juce::String borrowerDescribe() const = 0;
};

enum class EditorHolder { None = 0, OwnWindow = 1, Borrowed = 2 };

inline const char* editorHolderName (EditorHolder h) noexcept
{
    switch (h)
    {
        case EditorHolder::OwnWindow: return "the Link's own window";
        case EditorHolder::Borrowed:  return "a borrower";
        case EditorHolder::None:      break;
    }
    return "nobody";
}

namespace detail
{
    constexpr int kMaxInstances      = 64;    // his biggest session has 16 Links
    constexpr int kMaxLeasesPerInst  = 4;     // the own window and a live view, with slack
    constexpr int kUidChars          = 24;    // a Link uid is 10 hex characters

    struct LeaseRow
    {
        int      slot1;       // 0 = this row is free
        int      kind;        // EditorHolder
        void*    borrower;    // EditorBorrower*, non-owning
        unsigned token;
    };

    struct InstanceRow
    {
        char  uid[kUidChars];
        void* lender;         // EditorLender*, non-owning
        int   pid;
        LeaseRow leases[kMaxLeasesPerInst];
    };

    struct Table
    {
        // Zeroed memory is a free lock and token 0: the block comes from calloc, and nothing here is
        // constructed, because no image may construct a table another image is already using.
        std::atomic<int> spin;
        unsigned         nextToken;
        InstanceRow      rows[kMaxInstances];
    };

    /** The one table in this process, or nullptr if another EchoJay build got there first with a
        different layout (EJProcessGlobal.h says why that is answered rather than ignored). */
    inline Table* table (bool* createdOut = nullptr) noexcept
    {
        static bool  created = false;
        static auto* t = (Table*) echojay::processGlobal ("echojayLocalInstances_v1",
                                                          sizeof (Table), &created);
        if (createdOut != nullptr) *createdOut = created;
        return t;
    }

    struct Guard
    {
        Table* t;
        explicit Guard (Table* tt) : t (tt)
        {
            if (t == nullptr) return;
            int expected = 0;
            while (! t->spin.compare_exchange_weak (expected, 1, std::memory_order_acquire))
            { expected = 0; std::this_thread::yield(); }
        }
        ~Guard() { if (t != nullptr) t->spin.store (0, std::memory_order_release); }
        Guard (const Guard&) = delete;
        Guard& operator= (const Guard&) = delete;
    };

    inline InstanceRow* findRow (Table* t, const juce::String& uid) noexcept
    {
        if (t == nullptr || uid.isEmpty()) return nullptr;
        const auto u = uid.toRawUTF8();
        for (auto& r : t->rows)
            if (r.lender != nullptr && std::strncmp (r.uid, u, kUidChars - 1) == 0)
                return &r;
        return nullptr;
    }
}

/** The directory. add/drop on the owner's own thread of life (its constructor and destructor);
    find on the message thread, and only ever in the same process. */
class LocalInstances
{
public:
    /** Registers (or re-registers under a new uid - a Link regenerates its uid on a registry
        collision). Returns false when the table is full or unavailable, which the caller SAYS. */
    static bool add (const juce::String& uid, EditorLender* lender)
    {
        auto* t = detail::table();
        if (t == nullptr || lender == nullptr || uid.isEmpty()) return false;
        detail::Guard g (t);
        // A lender that is already in the table under another uid moves, rather than appearing twice.
        for (auto& r : t->rows)
            if (r.lender == lender)
            {
                juce::zerostruct (r.uid);
                uid.copyToUTF8 (r.uid, detail::kUidChars);
                r.pid = (int) ::getpid();
                return true;
            }
        for (auto& r : t->rows)
            if (r.lender == nullptr)
            {
                juce::zerostruct (r);
                uid.copyToUTF8 (r.uid, detail::kUidChars);
                r.lender = lender;
                r.pid = (int) ::getpid();
                return true;
            }
        return false;
    }

    /** The owner's destructor, FIRST act. Leases are revoked by EditorLease::revokeAll before this -
        see LinkProcessor's destructor, where the two are one step. */
    static void drop (EditorLender* lender)
    {
        auto* t = detail::table();
        if (t == nullptr || lender == nullptr) return;
        detail::Guard g (t);
        for (auto& r : t->rows)
            if (r.lender == lender)
                juce::zerostruct (r);
    }

    /** The live instance with that uid IN THIS PROCESS, or nullptr. The pid test is the same
        question echojay::embedAllowed asks of the sidecar, asked of the thing itself. */
    static EditorLender* find (const juce::String& uid)
    {
        auto* t = detail::table();
        if (t == nullptr) return nullptr;
        detail::Guard g (t);
        auto* r = detail::findRow (t, uid);
        if (r == nullptr || r->pid != (int) ::getpid()) return nullptr;
        return (EditorLender*) r->lender;
    }

    static int count()
    {
        auto* t = detail::table();
        if (t == nullptr) return 0;
        detail::Guard g (t);
        int n = 0;
        for (const auto& r : t->rows) if (r.lender != nullptr) ++n;
        return n;
    }

    /** True when this process has a shared table at all. False means another EchoJay build made one
        with a different shape, and every road here says so and falls back. */
    static bool shared() { return detail::table() != nullptr; }

    /** Did THIS bundle create the table (the first EchoJay in the process)? Evidence for the rig
        row that proves the two bundles meet in one table rather than two. */
    static bool createdHere()
    {
        bool created = false;
        detail::table (&created);
        return created;
    }

    /** "2 live: uid 4814a16004, uid 9c2f110a33" - for the log and the rig's state. */
    static juce::String describe()
    {
        auto* t = detail::table();
        if (t == nullptr) return "no shared table in this process";
        juce::StringArray bits;
        {
            detail::Guard g (t);
            for (const auto& r : t->rows)
                if (r.lender != nullptr)
                {
                    juce::String b ("uid ");
                    b << juce::String (r.uid) << " pid " << r.pid;
                    int held = 0;
                    for (const auto& l : r.leases) if (l.slot1 != 0) ++held;
                    if (held > 0) b << " (" << held << " lease(s))";
                    bits.add (b);
                }
        }
        return juce::String (bits.size()) + " live" + (bits.isEmpty() ? juce::String() : ": " + bits.joinIntoString (", "));
    }
};

/** The exclusive lease on one slot's editor. Every function here is message thread only, and every
    one of them calls out to a lender or a borrower with the table's lock RELEASED. */
class EditorLease
{
public:
    struct Taken
    {
        juce::AudioProcessorEditor* editor = nullptr;
        unsigned    token = 0;
        juce::String why;                       // why not, when editor is nullptr
        bool ok() const noexcept { return editor != nullptr; }
    };

    /** Take the lease on uid's slot and get its real editor. Refused - with a sentence - when the
        instance is not in this process, when someone already holds that slot, or when the plugin
        will not give an editor. Recorded BEFORE the editor is created, so a re-entrant call (an AU
        view that spins the message loop as it builds) is refused rather than served twice. */
    static Taken take (const juce::String& uid, int slot1Based, EditorBorrower* borrower, EditorHolder as)
    {
        Taken out;
        if (slot1Based < 1 || borrower == nullptr || as == EditorHolder::None)
        { out.why = "a lease needs a slot, a borrower and a holder"; return out; }

        auto* t = detail::table();
        if (t == nullptr) { out.why = "another EchoJay build owns the shared table in this process"; return out; }

        EditorLender* lender = nullptr;
        unsigned token = 0;
        {
            detail::Guard g (t);
            auto* r = detail::findRow (t, uid);
            if (r == nullptr)       { out.why = "no live EchoJay Link with that id in this process"; return out; }
            if (r->pid != (int) ::getpid()) { out.why = "that Link is in another process"; return out; }

            detail::LeaseRow* free = nullptr;
            for (auto& l : r->leases)
            {
                if (l.slot1 == slot1Based)
                {
                    out.why = juce::String ("slot ") + juce::String (slot1Based) + " is already open in "
                            + editorHolderName ((EditorHolder) l.kind);
                    return out;
                }
                if (l.slot1 == 0 && free == nullptr) free = &l;
            }
            if (free == nullptr) { out.why = "that Link already has as many editors open as it allows"; return out; }

            token = ++t->nextToken;
            free->slot1    = slot1Based;
            free->kind     = (int) as;
            free->borrower = borrower;
            free->token    = token;
            lender = (EditorLender*) r->lender;
        }

        juce::String why;
        auto* ed = lender != nullptr ? lender->lenderCreateSlotEditor (slot1Based, why) : nullptr;
        if (ed == nullptr)
        {
            // Nothing to show: give the lease straight back, so a refusal leaves no holder behind.
            clearLease (uid, slot1Based, token);
            out.why = why.isNotEmpty() ? why : juce::String ("that plugin would not open an editor here");
            return out;
        }
        out.editor = ed;
        out.token  = token;
        return out;
    }

    /** The borrower returns it: the editor is destroyed by its owner. `token` must be the one take()
        gave, so a stale return cannot close somebody else's editor. */
    static bool giveBack (const juce::String& uid, int slot1Based, unsigned token)
    {
        auto* t = detail::table();
        if (t == nullptr) return false;
        EditorLender* lender = nullptr;
        {
            detail::Guard g (t);
            auto* r = detail::findRow (t, uid);
            if (r == nullptr) return false;
            for (auto& l : r->leases)
                if (l.slot1 == slot1Based && l.token == token)
                {
                    l = {};
                    lender = (EditorLender*) r->lender;
                    break;
                }
            if (lender == nullptr) return false;
        }
        lender->lenderDestroySlotEditor (slot1Based);
        return true;
    }

    static EditorHolder heldBy (const juce::String& uid, int slot1Based, juce::String* whoOut = nullptr)
    {
        auto* t = detail::table();
        if (t == nullptr) return EditorHolder::None;
        EditorBorrower* b = nullptr;
        EditorHolder kind = EditorHolder::None;
        {
            detail::Guard g (t);
            auto* r = detail::findRow (t, uid);
            if (r == nullptr) return EditorHolder::None;
            for (const auto& l : r->leases)
                if (l.slot1 == slot1Based)
                {
                    kind = (EditorHolder) l.kind;
                    b = (EditorBorrower*) l.borrower;
                    break;
                }
        }
        if (whoOut != nullptr)
            *whoOut = kind == EditorHolder::None ? juce::String ("nobody")
                    : (b != nullptr ? b->borrowerDescribe() : juce::String (editorHolderName (kind)));
        return kind;
    }

    /** THE ONE ORDER (rule 4): the borrower lets go, then the owner destroys the editor. Used by
        every teardown door, so there is no second order anywhere. */
    static int revokeFrom (const juce::String& uid, int firstSlot1, const juce::String& why)
    {
        auto* t = detail::table();
        if (t == nullptr) return 0;

        struct Hit { int slot1; EditorBorrower* borrower; };
        Hit hits[detail::kMaxLeasesPerInst] {};
        int n = 0;
        EditorLender* lender = nullptr;
        {
            detail::Guard g (t);
            auto* r = detail::findRow (t, uid);
            if (r == nullptr) return 0;
            lender = (EditorLender*) r->lender;
            for (auto& l : r->leases)
                if (l.slot1 != 0 && l.slot1 >= firstSlot1)
                {
                    hits[n++] = { l.slot1, (EditorBorrower*) l.borrower };
                    l = {};
                }
        }
        for (int i = 0; i < n; ++i)
        {
            if (hits[i].borrower != nullptr)
                hits[i].borrower->editorLeaseRevoked (uid, hits[i].slot1, why);   // synchronous
            if (lender != nullptr)
                lender->lenderDestroySlotEditor (hits[i].slot1);
        }
        return n;
    }

    static int revokeSlot (const juce::String& uid, int slot1Based, const juce::String& why)
    {
        auto* t = detail::table();
        if (t == nullptr) return 0;
        // One slot only: revokeFrom's sweep would also take the slots above it, which is right for a
        // structural change and wrong for "this one editor must close".
        struct Hit { int slot1; EditorBorrower* borrower; };
        Hit hit {};
        EditorLender* lender = nullptr;
        {
            detail::Guard g (t);
            auto* r = detail::findRow (t, uid);
            if (r == nullptr) return 0;
            lender = (EditorLender*) r->lender;
            for (auto& l : r->leases)
                if (l.slot1 == slot1Based)
                { hit = { l.slot1, (EditorBorrower*) l.borrower }; l = {}; break; }
        }
        if (hit.slot1 == 0) return 0;
        if (hit.borrower != nullptr) hit.borrower->editorLeaseRevoked (uid, hit.slot1, why);
        if (lender != nullptr) lender->lenderDestroySlotEditor (hit.slot1);
        return 1;
    }

    /** The instance is going away. Its destructor's first act, with the directory drop. */
    static int revokeAll (const juce::String& uid, const juce::String& why)
    {
        return revokeFrom (uid, 1, why);
    }

    /** The borrower's own destructor (rule 6): return anything it still holds, wherever it is. This
        can only ever find something if a borrower died without returning, which is a bug - so it
        says so rather than cleaning up quietly. */
    static int borrowerGone (EditorBorrower* borrower)
    {
        auto* t = detail::table();
        if (t == nullptr || borrower == nullptr) return 0;

        struct Hit { juce::String uid; int slot1; EditorLender* lender; };
        juce::Array<Hit> hits;
        {
            detail::Guard g (t);
            for (auto& r : t->rows)
            {
                if (r.lender == nullptr) continue;
                for (auto& l : r.leases)
                    if (l.slot1 != 0 && l.borrower == borrower)
                    {
                        hits.add ({ juce::String (r.uid), l.slot1, (EditorLender*) r.lender });
                        l = {};
                    }
            }
        }
        for (const auto& h : hits)
            if (h.lender != nullptr)
                h.lender->lenderDestroySlotEditor (h.slot1);
        return hits.size();
    }

private:
    static void clearLease (const juce::String& uid, int slot1Based, unsigned token)
    {
        auto* t = detail::table();
        if (t == nullptr) return;
        detail::Guard g (t);
        auto* r = detail::findRow (t, uid);
        if (r == nullptr) return;
        for (auto& l : r->leases)
            if (l.slot1 == slot1Based && l.token == token) { l = {}; return; }
    }
};

} // namespace echojay
