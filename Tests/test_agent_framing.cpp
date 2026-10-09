// Standalone unit test for EJAgentFraming (no JUCE, no network):
//   c++ -std=c++17 Tests/test_agent_framing.cpp -o /tmp/ej_agent_framing && /tmp/ej_agent_framing
//
// The property the agent transport depends on, and the one the chat-stream
// splitter does NOT provide: the "event:" name of every frame survives, at
// every chunk boundary, and a frame with no event line comes back with an
// empty name (the type then rides inside the JSON - EJAgentProtocol.h reads both).

#include "../Source/EJAgentFraming.h"
#include <cstdio>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void check (const char* name, bool cond)
{
    if (cond) { ++passed; std::printf ("  ok  %s\n", name); }
    else      { ++failed; std::printf ("FAIL  %s\n", name); }
}

static std::vector<EJAgentSseEvent> feed (EJAgentFraming& f, const std::string& s, size_t chunkSize)
{
    std::vector<EJAgentSseEvent> out;
    for (size_t i = 0; i < s.size(); i += chunkSize)
    {
        auto part = s.substr (i, chunkSize);
        for (auto& p : f.appendChunk (part.data(), (int) part.size()))
            out.push_back (p);
    }
    return out;
}

int main()
{
    // AGENT_MODE_PLAN.md 1.3, verbatim shapes, plus a ping and a CRLF frame.
    const std::string wire =
        "event: round\n"
        "data: {\"sessionId\":\"ag_1\",\"round\":1}\n\n"
        ": ping\n\n"
        "event: delta\r\n"
        "data: {\"text\":\"Looking at the vocal...\"}\r\n\r\n"
        "event: tool_call\n"
        "data: {\"id\":\"tc_1\",\"name\":\"look\",\"args\":{\"what\":\"rack\"},\"approval\":\"free\"}\n\n"
        "data: {\"type\":\"delta\",\"text\":\"no event line\"}\n\n"
        "event: await\n"
        "data: {\"ids\":[\"tc_1\"]}\n\n";

    for (size_t chunk : { wire.size(), (size_t) 7, (size_t) 3, (size_t) 1 })
    {
        EJAgentFraming f;
        auto ev = feed (f, wire, chunk);
        char name[96];
        std::snprintf (name, sizeof name, "chunk %zu: 5 events (ping skipped)", chunk);
        check (name, ev.size() == 5);
        if (ev.size() != 5) continue;
        std::snprintf (name, sizeof name, "chunk %zu: event names survive in order", chunk);
        check (name, ev[0].event == "round" && ev[1].event == "delta" && ev[2].event == "tool_call" && ev[3].event == "" && ev[4].event == "await");
        std::snprintf (name, sizeof name, "chunk %zu: data payloads intact", chunk);
        check (name, ev[0].data == "{\"sessionId\":\"ag_1\",\"round\":1}"
                  && ev[1].data == "{\"text\":\"Looking at the vocal...\"}"
                  && ev[3].data == "{\"type\":\"delta\",\"text\":\"no event line\"}"
                  && ev[4].data == "{\"ids\":[\"tc_1\"]}");
        std::snprintf (name, sizeof name, "chunk %zu: nothing left pending", chunk);
        check (name, ! f.hasPartialFrame());
    }

    // An event line with no data dispatches nothing and does not leak its name into the next frame.
    {
        EJAgentFraming f;
        auto ev = feed (f, "event: await\n\nevent: done\ndata: {\"summary\":\"ok\"}\n\n", 1);
        check ("event without data is dropped", ev.size() == 1 && ev[0].event == "done" && ev[0].data == "{\"summary\":\"ok\"}");
    }

    // Multi-line data joins with '\n'; a name split across chunks still reads whole.
    {
        EJAgentFraming f;
        auto ev = feed (f, "event: delta\ndata: a\ndata: b\n\n", 2);
        check ("multi-line data joined", ev.size() == 1 && ev[0].event == "delta" && ev[0].data == "a\nb");
    }

    // A stream that ends mid-frame reports it.
    {
        EJAgentFraming f;
        auto ev = feed (f, "event: round\ndata: {\"round\":", 4);
        check ("mid-frame end: no event, partial reported", ev.empty() && f.hasPartialFrame());
    }

    std::printf ("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
