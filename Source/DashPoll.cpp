#include "DashPoll.h"
#include "EchoJayAPI.h"

void DashPollShared::addClient (const void* owner, EchoJayAPI* api, std::function<void()> onChanged)
{
    JUCE_ASSERT_MESSAGE_THREAD

    for (auto& c : clients)
        if (c.owner == owner)
        {
            // Re-registration by the same owner replaces its callback rather
            // than adding a second entry. An editor recreation goes through
            // the processor, not through here, but a processor that somehow
            // registered twice would otherwise be notified twice forever.
            c.api = api;
            c.onChanged = std::move (onChanged);
            return;
        }

    clients.push_back ({ owner, api, std::move (onChanged) });

    if (! isTimerRunning())
    {
        ejDashLog ("[dash-poll] first client, starting the shared 20s timer");
        // Fire at once so a freshly loaded instance does not wait 20 seconds
        // for a badge the server could report immediately.
        tick();
        startTimer (20000);
    }
    else
    {
        ejDashLog ("[dash-poll] client added, now " + juce::String ((int) clients.size())
                   + " instances sharing ONE poll");
    }
}

void DashPollShared::removeClient (const void* owner)
{
    JUCE_ASSERT_MESSAGE_THREAD

    for (size_t i = 0; i < clients.size(); ++i)
        if (clients[i].owner == owner)
        {
            clients.erase (clients.begin() + (long) i);
            break;
        }

    if (clients.empty())
    {
        ejDashLog ("[dash-poll] last client gone, stopping the timer");
        stopTimer();
    }
}

void DashPollShared::tick()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (clients.empty())
        return;

    // A tick landing while the previous request is still out is DROPPED, not
    // queued. At a 20s interval against a 5s timeout this should not happen;
    // if it does, a queue would hide a stalled network behind a backlog.
    if (inFlight)
    {
        ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
                   + " SKIPPED, previous request still in flight");
        return;
    }

    // BORROWED, not owned. The first live client's api issues the request; see
    // the wrinkle note in DashPoll.h for why this class has no api of its own.
    auto* api = clients.front().api;
    if (api == nullptr)
        return;

    // Counted at FIRE, not at completion, because the question this answers is
    // "did the timer keep running", not "did the network work".
    ++tickCount;
    inFlight = true;

    ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
               + " firing for " + juce::String ((int) clients.size())
               + " instance(s), rev=" + juce::String (unread.rev)
               + " unread=" + juce::String (unread.total));

    // getJSON threads the request and marshals the completion back with
    // MessageManager::callAsync behind its own `alive` shared_ptr, so a
    // callback cannot fire into a destroyed api. `this` is safe to capture
    // because SharedResourcePointer keeps this object alive while any client
    // holds it, and a client cannot be destroyed without deregistering.
    api->pollCommunity ([this] (const juce::var& json, int status)
    {
        inFlight = false;

        if (status != 200)
        {
            // A failed poll changes NOTHING. The badge keeps its last known
            // counts rather than dropping to zero, because "we could not ask"
            // is not "there is nothing new".
            ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
                       + " http " + juce::String (status) + ", keeping last known counts");
            return;
        }

        const auto* obj = json.getDynamicObject();
        if (obj == nullptr)
            return;

        const juce::int64 rev = (juce::int64) (double) obj->getProperty ("rev");

        // D3.2: pending imports, a SECOND signal riding the same response.
        //
        // It cannot hide behind the rev check. `rev` is a GLOBAL counter,
        // bumped once per announcement and never per user (see getPoll in
        // lib/dash/community.js, and the per-user-write-on-broadcast error
        // that design exists to avoid). An import is a PER-USER event, so it
        // must not move a global counter, so rev will not move when one
        // lands. Comparing this field separately costs nothing: the response
        // is already here, already parsed, and still one round trip.
        //
        // ABSENT IS NOT ZERO. The backend slice that increments this has not
        // shipped, so the field is missing today and this stays -1, which is
        // "the server has never told us" rather than "the server said none".
        // Logged either way, so a client that is silently inert because the
        // field was never wired, or was wired under a different name, says so
        // in the log rather than simply never lighting up.
        const bool hasImports = obj->hasProperty ("imports");
        const int  imports    = hasImports ? (int) obj->getProperty ("imports") : -1;

        // M2.5: direct messages, the THIRD independent signal on this response.
        //
        // It cannot hide behind the rev check, for exactly the reason the
        // imports note above gives, and which this field silently inherited.
        // `rev` is GLOBAL, bumped once per announcement; `imports` moves only
        // when a chain import lands. A DM moves NEITHER. So before this
        // comparison existed, the early return below fired on every DM, the
        // assignments never ran, and `direct` stayed at its old value while
        // the server reported a real count. The field was already parsed and
        // already in Unread: it was simply unreachable.
        //
        // NO -1 SENTINEL, unlike imports. `direct` has been present in every
        // poll response since M1, which returned a hardcoded 0. "Absent" is
        // not a state it has. M2.1 changed the VALUE it carries, not whether
        // it is there.
        const int  direct       = (int) obj->getProperty ("direct");

        const bool revMoved     = (rev != unread.rev);
        const bool importsMoved = (imports != unread.imports);
        const bool directMoved  = (direct != unread.direct);

        // Nothing new means NO WORK: no state write, no generation bump, no
        // notification, and above all no dashboard payload fetch.
        if (! revMoved && ! importsMoved && ! directMoved)
        {
            ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
                       + " rev unchanged at " + juce::String (rev)
                       + ", imports unchanged at " + juce::String (imports)
                       + (hasImports ? "" : " (field ABSENT: backend slice not shipped)")
                       + ", direct unchanged at " + juce::String (direct)
                       + ", no work");
            return;
        }

        unread.rev           = rev;
        unread.imports       = imports;
        unread.direct        = direct;
        unread.total         = (int) obj->getProperty ("total");
        unread.announcements = (int) obj->getProperty ("announcements");
        unread.team          = (int) obj->getProperty ("team");
        ++generation;

        ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
                   + (revMoved ? " rev MOVED to " : " rev at ") + juce::String (rev)
                   + (importsMoved ? " imports MOVED to " : " imports at ")
                   + juce::String (imports)
                   + (directMoved ? " direct MOVED to " : " direct at ")
                   + juce::String (direct)
                   + " unread=" + juce::String (unread.total)
                   + " gen=" + juce::String (generation)
                   + ", notifying " + juce::String ((int) clients.size()) + " instance(s)");

        // Copied before iterating: a callback could in principle deregister
        // its owner, and mutating the vector under its own loop is the kind of
        // thing that only crashes on somebody else's machine.
        auto snapshot = clients;
        for (auto& c : snapshot)
            if (c.onChanged)
                c.onChanged();
    });
}
