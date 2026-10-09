#pragma once

// ===========================================================================
// SSE byte-to-event splitter for /api/agent/start and /api/agent/step.
//
// WHY A SECOND SPLITTER. EJStreamFraming (the /api/chat-stream one) encodes a
// contract fact of THAT endpoint: "the server never sends named event: lines",
// so it skips them. AGENT_MODE_PLAN.md 1.3 writes the agent frames as
//
//     event: tool_call
//     data: {"id":"tc_1","name":"look",...}
//
// so the frame TYPE travels on the event line, and a splitter that drops it
// would hand every frame back as an anonymous JSON object. This one keeps the
// pair {event, data}. It is otherwise the same discipline, deliberately:
// JUCE-free (std:: only) so Tests/test_agent_framing.cpp runs with a bare
// compiler; arbitrary chunk boundaries; comments (": ping") skipped; data
// lines joined with '\n'; dispatch on the blank line.
//
// It knows NOTHING about the JSON. EJAgentProtocol.h reads the pair and also
// accepts a "type" field inside the JSON, so the client is correct whichever
// of the two shapes B ships (that is open question 1 in the handoff).
// ===========================================================================

#include <string>
#include <vector>

struct EJAgentSseEvent
{
    std::string event;   // the "event:" field, "" when the server sent none
    std::string data;    // the joined "data:" payload
};

class EJAgentFraming
{
public:
    // Feed one socket chunk; returns every COMPLETE event this chunk finished,
    // in wire order.
    std::vector<EJAgentSseEvent> appendChunk (const char* bytes, int numBytes)
    {
        std::vector<EJAgentSseEvent> out;
        if (bytes == nullptr || numBytes <= 0)
            return out;

        pending.append (bytes, (size_t) numBytes);

        size_t nl;
        while ((nl = pending.find ('\n')) != std::string::npos)
        {
            std::string line = pending.substr (0, nl);
            pending.erase (0, nl + 1);
            if (! line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty())
            {
                // event boundary: an event with no data line carries nothing
                // (the SSE spec says to drop it), so only haveData dispatches.
                if (haveData)
                    out.push_back ({ eventName, eventData });
                eventName.clear();
                eventData.clear();
                haveData = false;
                continue;
            }

            if (line.compare (0, 5, "data:") == 0)
            {
                std::string payload = line.substr (5);
                if (! payload.empty() && payload.front() == ' ')
                    payload.erase (0, 1);
                if (haveData)
                    eventData += '\n';
                eventData += payload;
                haveData = true;
                continue;
            }

            if (line.compare (0, 6, "event:") == 0)
            {
                std::string name = line.substr (6);
                if (! name.empty() && name.front() == ' ')
                    name.erase (0, 1);
                eventName = name;
                continue;
            }

            // comment (": ping"), "id:", "retry:" or any unknown field: skipped.
        }
        return out;
    }

    // True when bytes have arrived that are not yet a dispatched event: a
    // stream that ends in this state ended mid-frame.
    bool hasPartialFrame() const
    {
        return haveData || ! pending.empty() || ! eventName.empty();
    }

private:
    std::string pending;     // bytes since the last complete line
    std::string eventName;   // the event: field of the event being accumulated
    std::string eventData;   // its data lines
    bool haveData = false;
};
