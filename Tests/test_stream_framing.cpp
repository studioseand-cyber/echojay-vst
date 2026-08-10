// Standalone unit test for EJStreamFraming (no JUCE, no network):
//   c++ -std=c++17 Tests/test_stream_framing.cpp -o /tmp/ej_framing && /tmp/ej_framing
//
// Exercises the property the transport depends on: arbitrary chunk
// boundaries never lose, duplicate, or reorder frames.

#include "../Source/EJStreamFraming.h"
#include <cstdio>
#include <string>
#include <vector>

static int passed = 0, failed = 0;
static void check (const char* name, bool cond)
{
    if (cond) { ++passed; std::printf ("  ok  %s\n", name); }
    else      { ++failed; std::printf ("FAIL  %s\n", name); }
}

static std::vector<std::string> feed (EJStreamFraming& f, const std::string& s, size_t chunkSize)
{
    std::vector<std::string> out;
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
    const std::string wire =
        "data: {\"type\":\"start\",\"model\":\"m\"}\n\n"
        ": ping\n\n"
        "data: {\"type\":\"delta\",\"block\":\"text\",\"text\":\"Hel\"}\n\n"
        "data: {\"type\":\"delta\",\"block\":\"text\",\"text\":\"lo\"}\n\n"
        "data: {\"type\":\"done\",\"reply\":\"Hello\"}\n\n";

    // 1. whole thing in one chunk
    {
        EJStreamFraming f;
        auto frames = feed (f, wire, wire.size());
        check ("one chunk: 4 frames (ping skipped)", frames.size() == 4);
        check ("one chunk: order preserved",
               frames.size() == 4
               && frames[0].find ("start") != std::string::npos
               && frames[3].find ("done") != std::string::npos);
        check ("one chunk: no partial left", ! f.hasPartialFrame());
    }

    // 2. every possible chunk size, byte-by-byte upward: same frames
    {
        bool allGood = true;
        for (size_t cs = 1; cs <= wire.size(); ++cs)
        {
            EJStreamFraming f;
            auto frames = feed (f, wire, cs);
            if (frames.size() != 4 || f.hasPartialFrame()
                || frames[1] != "{\"type\":\"delta\",\"block\":\"text\",\"text\":\"Hel\"}")
            { allGood = false; break; }
        }
        check ("all chunk sizes 1..N produce identical frames", allGood);
    }

    // 3. split INSIDE the data: prefix
    {
        EJStreamFraming f;
        auto a = f.appendChunk ("dat", 3);
        auto b = f.appendChunk ("a: {\"x\":1}\n\n", 12);
        check ("prefix straddling chunks", a.empty() && b.size() == 1 && b[0] == "{\"x\":1}");
    }

    // 4. CRLF line endings
    {
        EJStreamFraming f;
        std::string crlf = "data: {\"y\":2}\r\n\r\n";
        auto frames = f.appendChunk (crlf.data(), (int) crlf.size());
        check ("CRLF tolerated", frames.size() == 1 && frames[0] == "{\"y\":2}");
    }

    // 5. multi-line data joined with \n (SSE spec; server doesn't send it,
    //    parser must not corrupt it)
    {
        EJStreamFraming f;
        std::string multi = "data: line1\ndata: line2\n\n";
        auto frames = f.appendChunk (multi.data(), (int) multi.size());
        check ("multi-line data joined", frames.size() == 1 && frames[0] == "line1\nline2");
    }

    // 6. unknown field lines skipped, frame still delivered
    {
        EJStreamFraming f;
        std::string s = "event: something\nid: 7\ndata: {\"z\":3}\n\n";
        auto frames = f.appendChunk (s.data(), (int) s.size());
        check ("unknown SSE fields skipped", frames.size() == 1 && frames[0] == "{\"z\":3}");
    }

    // 7. stream dying mid-frame is detectable
    {
        EJStreamFraming f;
        std::string s = "data: {\"type\":\"delta\",\"tex";
        auto frames = f.appendChunk (s.data(), (int) s.size());
        check ("mid-frame death leaves partial flag", frames.empty() && f.hasPartialFrame());
    }

    std::printf ("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
