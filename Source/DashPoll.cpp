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

        // THE REV CHECK, which is the point of this endpoint. If the monotonic
        // counter has not moved there is nothing new, so do nothing at all: no
        // state write, no generation bump, no notification, and above all no
        // dashboard payload fetch.
        if (rev == unread.rev)
        {
            ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
                       + " rev unchanged at " + juce::String (rev) + ", no work");
            return;
        }

        unread.rev           = rev;
        unread.total         = (int) obj->getProperty ("total");
        unread.announcements = (int) obj->getProperty ("announcements");
        unread.team          = (int) obj->getProperty ("team");
        unread.direct        = (int) obj->getProperty ("direct");
        ++generation;

        ejDashLog ("[dash-poll] tick " + juce::String (tickCount)
                   + " rev MOVED to " + juce::String (rev)
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
