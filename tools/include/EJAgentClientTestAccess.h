#pragma once
//  ONE COPY OF THE CLIENT'S TEST DOOR (23 Oct 2026)
//
//  EJAgentClient declares `friend struct EJAgentClientTestAccess` so a harness can feed it frames
//  without a server. The struct itself lived inside tools/agent_client_guard/harness.cpp, so the
//  second harness that needed it (chat_store_guard, for B's one-run rule) would have had to copy
//  it - and two spellings of one test door is the same family of fault as two spellings of one
//  product fact. It lives here now, and both harnesses include it.
//
//  parseFrame is the PRODUCT's own parser (EJAgentProtocol.h), so a harness feeds exactly what the
//  wire would deliver.
#include <JuceHeader.h>
#include <functional>
#include "EJAgentClient.h"

struct EJAgentClientTestAccess
{
    static void setRecorder (EJAgentClient& c,
                             std::function<void (const juce::String&, const juce::String&)> fn)
    { c.postOverride_ = std::move (fn); }

    static void feed (EJAgentClient& c, const juce::String& event, const juce::String& data)
    { c.onFrame (c.generation_, echojay::agent::parseFrame (event, data)); }

    static void feedStale (EJAgentClient& c, const juce::String& event, const juce::String& data)
    { c.onFrame (c.generation_ - 1, echojay::agent::parseFrame (event, data)); }

    /** ---- A RUN THAT IS *WORKING* (his test 6 at 21:06, 23 Oct 2026) ------------------------
        The supersede gate reads `isWorking()`, and a guard has no server to make a round actually
        run: start() fails its POST and the client is FAILED before any frame matters. So the state
        is set directly, which is honest for a leg about the GATE rather than about the stream - the
        stream's own paths are what `feed` and `endStream` above are for. */
    static void setStateForTest (EJAgentClient& c, EJAgentClient::State s) { c.setState (s); }

    static void endStream (EJAgentClient& c, bool terminal, int status, const juce::String& err)
    { c.onStreamEnded (c.generation_, terminal, status, err); }
};
