#pragma once

// ===========================================================================
// SSE byte-to-frame splitter for /api/chat-stream (STREAMING_REASONING_SPEC
// 3.1, step 2).
//
// Deliberately JUCE-free (std:: only) so it unit-tests standalone with a
// bare compiler — see Tests/test_stream_framing.cpp. The transport feeds it
// raw socket chunks; it returns complete SSE data payloads, each one JSON
// per the wire contract. It knows NOTHING about the JSON inside.
//
// Contract facts it encodes (server side is api/chat-stream.js):
//   - every frame is a data-only event: "data: {json}\n\n"
//   - keepalives are comment lines (": ping") and carry nothing
//   - the server never sends named "event:" lines, but a line-oriented
//     parser must skip unknown line types rather than choke, so we follow
//     the SSE spec: accumulate consecutive data lines, dispatch on the
//     blank line, join multi-line data with '\n' (the server never sends
//     multi-line data today; tolerating it costs nothing)
//   - chunk boundaries are arbitrary: a frame can arrive split anywhere,
//     including inside the "data:" prefix itself
// ===========================================================================

#include <string>
#include <vector>

class EJStreamFraming
{
public:
    // Feed one socket chunk; returns every COMPLETE data payload this chunk
    // finished. Payloads come back in wire order.
    std::vector<std::string> appendChunk (const char* bytes, int numBytes)
    {
        std::vector<std::string> out;
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
                // event boundary: dispatch whatever data lines accumulated
                if (haveData)
                {
                    out.push_back (eventData);
                    eventData.clear();
                    haveData = false;
                }
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

            // comment (": ping") or any unknown field line: skipped.
        }
        return out;
    }

    // True when bytes have arrived that are not yet a dispatched frame —
    // a stream that ends in this state ended mid-frame.
    bool hasPartialFrame() const
    {
        return haveData || ! pending.empty();
    }

private:
    std::string pending;    // bytes since the last complete line
    std::string eventData;  // data lines of the event being accumulated
    bool haveData = false;
};
