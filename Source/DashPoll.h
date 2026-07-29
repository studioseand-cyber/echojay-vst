#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>

class EchoJayAPI;

/**
    THE community poll. One timer and one request per 20 seconds for the whole
    process, however many EchoJay instances are loaded.

    WHY PROCESS-WIDE. Every instance was asking the same question and getting
    the same answer: the unread counts are per ACCOUNT, and every instance in a
    host shares one account and one settings file. A session with eight EchoJay
    inserts was making eight identical requests per tick. Refcounting removes
    the duplication rather than rationing it, which a backoff would have done.

    WHY NOT ON THE EDITOR, still. Logic destroys and recreates the editor on
    every Link window switch, so a timer there restarts its interval constantly
    and the counts reset. That coupling is what the whole design avoids and it
    must not come back. This object outlives editors AND processors.

    juce::SharedResourcePointer gives the lifetime for free: created on first
    use, destroyed when the last holder releases it, refcounted and thread safe
    at construction. Nothing here is a leaked static.

    THE WRINKLE, and how it is answered. The poller needs an EchoJayAPI to make
    a request, but every api belongs to a processor that can be removed from
    the session at any time. Giving this class its OWN EchoJayAPI would have
    added yet another instance calling saveSettings() at teardown, which is the
    settings-file corruption argument that moved the client to the processor in
    the first place, and at static-destruction time it would be worse. So it
    BORROWS: clients register their api, and a tick uses the first live one.
    The refcount guarantees at least one client exists while this object does,
    and EchoJayAPI's own `alive` flag already stops an in-flight callback from
    firing into a destroyed owner.

    MESSAGE THREAD ONLY. Registration, deregistration, the timer and the
    completion all run there, so the client list needs no lock. getJSON does
    its own threading and marshals back with MessageManager::callAsync.
*/
class DashPollShared final : private juce::Timer
{
public:
    DashPollShared() = default;
    ~DashPollShared() override { stopTimer(); }

    struct Unread
    {
        int total = 0, announcements = 0, team = 0, direct = 0;
        /** Monotonic server counter. Starts at -1 rather than 0 because 0 is a
            legitimate server value: starting there would make the very first
            poll look unchanged and skip its own update. */
        juce::int64 rev = -1;
    };

    /** Register. `owner` is an opaque identity for removal only and is never
        dereferenced. The first client starts the timer and fires a tick at
        once, so a freshly loaded instance does not wait 20 seconds. */
    void addClient (const void* owner, EchoJayAPI* api, std::function<void()> onChanged);

    /** Deregister. The last client out stops the timer. */
    void removeClient (const void* owner);

    const Unread& getUnread() const noexcept        { return unread; }

    /** Bumped only when the counts actually change, so a listener can tell
        whether what it drew is stale without diffing four fields. */
    int getGeneration() const noexcept              { return generation; }

    /** Ticks FIRED since this object was created, never reset. This is the
        number that answers the Link window question: if it keeps climbing
        across an editor recreation, the timer is where it belongs. */
    juce::int64 getTickCount() const noexcept       { return tickCount; }

    /** How many instances are sharing this one poller. Logged per tick, so
        loading a second EchoJay visibly does NOT add a second request. */
    int getClientCount() const noexcept             { return (int) clients.size(); }

private:
    void timerCallback() override                   { tick(); }
    void tick();

    struct Client
    {
        const void* owner = nullptr;
        EchoJayAPI* api = nullptr;
        std::function<void()> onChanged;
    };

    std::vector<Client> clients;
    Unread unread;
    int generation = 0;
    juce::int64 tickCount = 0;
    bool inFlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DashPollShared)
};

/** File logger for the poll, gated on ECHOJAY_DEV_TRANSPORT. Defined in
    PluginProcessor.cpp beside ejTeardownLog, whose shape it follows: a plugin
    inside Logic has its stderr swallowed, so juce::Logger::writeToLog produces
    nothing readable. */
void ejDashLog (const juce::String& msg);
