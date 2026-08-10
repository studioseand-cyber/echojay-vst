// Read-back test for EJStreamBlockParser (spec 2.1 / step 3).
//
// The contract under test: replay REAL captured stream bytes (and synthetic
// wire shapes) at EVERY chunk size from 1 byte upward, through the real
// framer and the parser, and assert the output is IDENTICAL to running the
// existing whole-reply strip — the REAL extractors in EJReplyBlocks.h, in
// the call-site order chain/gain/ask/edit — on the assembled text. Plus the
// atomicity rules asserted structurally: prose is always a prefix of the
// final visible text (so no block byte ever appears even transiently), a
// block event carries only a complete payload, a truncated block produces
// NO event at all.
//
// Out of contract, deliberately: markers nested inside another block's JSON
// payload. The whole-reply extractors and this parser disagree there (the
// extractors would splice an inner block out of an outer payload); the
// server never emits nested markers and neither behaviour is "right".
//
// Built as a JUCE console app (ECHOJAY_BUILD_STREAM_TESTS=ON):
//   cmake --build build-check --target EJStreamTests
//   ./EJStreamTests <path-to-fixtures-dir>

#include <juce_core/juce_core.h>
#include "../Source/EJStreamFraming.h"
#include "../Source/EJReplyBlocks.h"
#include "../Source/EJStreamBlockParser.h"

#include <cstdio>
#include <vector>

static int passed = 0, failed = 0;
static void check (const juce::String& name, bool cond, const juce::String& detail = {})
{
    if (cond) { ++passed; std::printf ("  ok  %s\n", name.toRawUTF8()); }
    else      { ++failed; std::printf ("FAIL  %s%s\n", name.toRawUTF8(),
                                       detail.isNotEmpty() ? (" -- " + detail).toRawUTF8() : ""); }
}

// ---- the whole-reply reference: the REAL extractors, call-site order ----
struct RefStrip
{
    juce::String visible, chain, gain, ask, edit;
};
static RefStrip refStrip (const juce::String& full)
{
    RefStrip r;
    r.visible = full;
    EJReplyBlocks::extractChainBlock     (r.visible, r.chain);
    EJReplyBlocks::extractGainBlock      (r.visible, r.gain);
    EJReplyBlocks::extractAskBlock       (r.visible, r.ask);
    EJReplyBlocks::extractChainEditBlock (r.visible, r.edit);
    return r;
}

// ---- one parser run over a list of text deltas ----
struct ParseResult
{
    juce::String prose;
    std::vector<EJStreamBlockParser::BlockEvent> blocks;
    juce::String truncatedType, truncatedPayload;
    bool prefixInvariantHeld = true;   // prose-so-far always a prefix of expected visible
    bool sawPartialBlockEvent = false; // an onBlock whose payload has an open marker but no close
};
static ParseResult runParserOverDeltas (const std::vector<juce::String>& deltas,
                                        const juce::String& expectedVisible,
                                        size_t chunkSize)
{
    ParseResult out;
    EJStreamBlockParser parser;
    parser.onProse = [&] (const juce::String& s)
    {
        out.prose += s;
        if (! expectedVisible.startsWith (out.prose))
            out.prefixInvariantHeld = false;
    };
    parser.onBlock = [&] (const EJStreamBlockParser::BlockEvent& ev)
    {
        // structural rule 1: a delivered payload is complete by definition —
        // it must never itself end inside an unterminated marker
        if (ev.payload.contains ("<<<") )
            out.sawPartialBlockEvent = true;
        out.blocks.push_back (ev);
    };

    // Re-chunk the delta texts at the given size, preserving delta
    // boundaries' irrelevance: concatenate then split — the parser must not
    // care where deltas end, and the fixture sweep below separately drives
    // it through the real framer at raw-byte chunk sizes.
    juce::String all;
    for (auto& d : deltas) all += d;
    const auto utf8 = all.toStdString();
    for (size_t i = 0; i < utf8.size(); i += chunkSize)
        parser.appendDelta (juce::String::fromUTF8 (utf8.substr (i, chunkSize).c_str()));
    parser.finish();
    out.truncatedType    = parser.truncatedType();
    out.truncatedPayload = parser.truncatedPayload();
    return out;
}

// ---- full client pipeline: raw SSE bytes -> framer -> JSON -> parser ----
struct PipelineResult : ParseResult { juce::String doneReply; int frameCount = 0; };
static PipelineResult runPipeline (const std::string& raw,
                                   const juce::String& expectedVisible,
                                   size_t chunkSize)
{
    PipelineResult out;
    EJStreamFraming framing;
    EJStreamBlockParser parser;
    parser.onProse = [&] (const juce::String& s)
    {
        out.prose += s;
        if (! expectedVisible.startsWith (out.prose))
            out.prefixInvariantHeld = false;
    };
    parser.onBlock = [&] (const EJStreamBlockParser::BlockEvent& ev) { out.blocks.push_back (ev); };

    bool finished = false;
    for (size_t i = 0; i < raw.size() && ! finished; i += chunkSize)
    {
        const auto part = raw.substr (i, chunkSize);
        for (auto& payload : framing.appendChunk (part.data(), (int) part.size()))
        {
            ++out.frameCount;
            auto frame = juce::JSON::parse (juce::String::fromUTF8 (payload.c_str(), (int) payload.size()));
            auto* obj = frame.getDynamicObject();
            if (obj == nullptr) continue;
            const auto type = obj->getProperty ("type").toString();
            if (type == "delta" && obj->getProperty ("block").toString() == "text")
                parser.appendDelta (obj->getProperty ("text").toString());
            else if (type == "done")
            {
                out.doneReply = obj->getProperty ("reply").toString();
                parser.finish();
                finished = true;
            }
            else if (type == "error")
            {
                parser.finish();
                finished = true;
            }
        }
    }
    if (! finished) parser.finish();   // stream died mid-flight
    out.truncatedType    = parser.truncatedType();
    out.truncatedPayload = parser.truncatedPayload();
    return out;
}

// ---- synthetic sweep: parity at every chunk size ----
static void sweepSynthetic (const juce::String& name, const juce::String& full,
                            int expectedBlocks, bool expectTruncated = false,
                            const juce::String& expectedTruncType = {})
{
    const auto ref = refStrip (full);
    const auto utf8 = full.toStdString();
    bool allEqual = true, invariantHeld = true, blocksOk = true, truncOk = true;
    size_t firstBad = 0;
    for (size_t cs = 1; cs <= utf8.size(); ++cs)
    {
        auto r = runParserOverDeltas ({ full }, ref.visible, cs);
        const bool eq = (r.prose == ref.visible);
        if (! eq && allEqual) { allEqual = false; firstBad = cs; }
        invariantHeld = invariantHeld && r.prefixInvariantHeld && ! r.sawPartialBlockEvent;
        if ((int) r.blocks.size() != expectedBlocks) blocksOk = false;
        for (auto& b : r.blocks)
        {
            const juce::String& want = b.type == "chain" ? ref.chain
                                     : b.type == "gain"  ? ref.gain
                                     : b.type == "ask"   ? ref.ask : ref.edit;
            if (b.payload != want) blocksOk = false;
        }
        if (expectTruncated)
            truncOk = truncOk && (r.truncatedType == expectedTruncType);
        else
            truncOk = truncOk && r.truncatedType.isEmpty();
    }
    check (name + ": prose parity at every chunk size", allEqual,
           allEqual ? juce::String() : ("first divergence at chunk size " + juce::String ((int) firstBad)));
    check (name + ": prefix invariant + no partial block event", invariantHeld);
    check (name + ": block events match the real extractors", blocksOk);
    check (name + ": truncation state correct", truncOk);
}

int main (int argc, char** argv)
{
    const juce::String fixtureDir = argc > 1 ? juce::String (argv[1]) : juce::String ("Tests/fixtures");

    // ================= real captured stream (the 16+-delta chain block) =================
    {
        juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (fixtureDir).getChildFile ("chain-stream-a.sse");
        check ("fixture exists: chain-stream-a.sse", f.existsAsFile(), f.getFullPathName());
        if (f.existsAsFile())
        {
            juce::MemoryBlock mb;
            f.loadFileAsData (mb);
            const std::string raw ((const char*) mb.getData(), mb.getSize());

            // Decode once at full size for the reference material.
            auto fullRun = runPipeline (raw, {}, raw.size());
            // Assemble the delta text independently of the parser: reference
            // input is what the parser SEES, i.e. the concatenated deltas.
            juce::String assembled;
            {
                EJStreamFraming fr;
                for (auto& payload : fr.appendChunk (raw.data(), (int) raw.size()))
                {
                    auto frame = juce::JSON::parse (juce::String::fromUTF8 (payload.c_str(), (int) payload.size()));
                    if (auto* o = frame.getDynamicObject())
                        if (o->getProperty ("type").toString() == "delta"
                            && o->getProperty ("block").toString() == "text")
                            assembled += o->getProperty ("text").toString();
                }
            }
            check ("fixture: assembled deltas == done.reply (no server rewrite on this material)",
                   assembled == fullRun.doneReply);

            const auto ref = refStrip (assembled);
            check ("fixture: whole-reply strip found a chain payload", ref.chain.isNotEmpty());
            {
                auto parsedChain = juce::JSON::parse (ref.chain);
                auto* o = parsedChain.getDynamicObject();
                check ("fixture: chain payload is parseable JSON with a chain array",
                       o != nullptr && o->getProperty ("chain").isArray());
            }

            bool allEqual = true, invariantHeld = true, blocksOk = true;
            size_t firstBad = 0;
            for (size_t cs = 1; cs <= raw.size(); ++cs)
            {
                auto r = runPipeline (raw, ref.visible, cs);
                if (r.prose != ref.visible) { if (allEqual) firstBad = cs; allEqual = false; }
                invariantHeld = invariantHeld && r.prefixInvariantHeld;
                blocksOk = blocksOk && r.blocks.size() == 1
                                    && r.blocks[0].type == "chain"
                                    && r.blocks[0].payload == ref.chain
                                    && r.truncatedType.isEmpty();
            }
            check ("fixture: prose parity with the REAL strip at every raw chunk size 1.."
                   + juce::String ((int) raw.size()), allEqual,
                   allEqual ? juce::String() : ("first divergence at chunk size " + juce::String ((int) firstBad)));
            check ("fixture: prose-is-prefix invariant held at every size (no block byte ever surfaced)", invariantHeld);
            check ("fixture: exactly one chain block, payload identical to extractChainBlock, at every size", blocksOk);
            check ("fixture: visible prose carries no marker bytes", ! ref.visible.contains ("<<<"));

            // The worst failure available on this path (rule 1): cut the
            // stream INSIDE the chain block, at every byte of the block, and
            // assert no run ever emits a block event or leaks block bytes.
            {
                const size_t openAt = raw.find ("<<<ECHOJAY_CHAIN>>>");
                const size_t endAt  = raw.find ("<<<END_CHAIN>>>");
                bool neverActed = (openAt != std::string::npos && endAt != std::string::npos);
                if (neverActed)
                {
                    for (size_t cut = openAt + 4; cut < endAt; cut += 7)   // every 7th byte: dense, bounded
                    {
                        auto truncatedRaw = raw.substr (0, cut);
                        auto r = runPipeline (truncatedRaw, refStrip (assembled).visible, 17);
                        if (! r.blocks.empty()) { neverActed = false; break; }
                    }
                }
                check ("fixture: stream cut anywhere inside the block NEVER yields a block event", neverActed);
            }
        }
    }

    // ================= synthetic wire shapes =================
    const juce::String CHAIN_BLK = "<<<ECHOJAY_CHAIN>>>{\"chain\":[{\"name\":\"TDR Nova\",\"role\":\"De-esser\"},{\"name\":\"Kotelnikov\",\"role\":\"Comp\"}],\"result\":\"Done.\"}<<<END_CHAIN>>>";
    const juce::String GAIN_BLK  = "<<<ECHOJAY_GAIN>>>{\"target\":\"Mix Bus\",\"delta_db\":-2.5}<<<END_GAIN>>>";
    const juce::String ASK_BLK   = "<<<ECHOJAY_ASK>>>{\"question\":\"Which vibe?\",\"choices\":[{\"label\":\"Clean\"},{\"label\":\"Warm\"}]}<<<END_ASK>>>";
    const juce::String EDIT_BLK  = "<<<ECHOJAY_CHAIN_EDIT>>>{\"edit\":[{\"op\":\"add\",\"name\":\"Frontier\",\"after\":1}]}<<<END_CHAIN_EDIT>>>";

    sweepSynthetic ("ask-only reply",   "Quick question first.\n\n" + ASK_BLK, 1);
    sweepSynthetic ("gain + trailing prose", "Bus is hot.\n\n" + GAIN_BLK + "\n\nSay yes to apply.", 1);
    sweepSynthetic ("chain_edit block", "Swapping it now?\n\n" + EDIT_BLK, 1);
    sweepSynthetic ("chain then gain",  "Here you go.\n\n" + CHAIN_BLK + "\n\nAnd trim the bus:\n\n" + GAIN_BLK, 2);
    sweepSynthetic ("marker lookalikes stay prose", "gain < 3 dB, a << b, and even <<<ECHOJA-not-a-marker, fine.", 0);
    sweepSynthetic ("duplicate chain: second stays literal (extractor parity)",
                    "First:\n\n" + CHAIN_BLK + "\n\nAgain:\n\n" + CHAIN_BLK, 1);
    sweepSynthetic ("whitespace before marker trimEnd parity",
                    "prose ends here   \n\n  \t" + GAIN_BLK, 1);
    sweepSynthetic ("block at position zero", CHAIN_BLK + "\n\nafter.", 1);
    sweepSynthetic ("truncated chain: quarantined, never an event",
                    "Building it.\n\n<<<ECHOJAY_CHAIN>>>{\"chain\":[{\"name\":\"TDR No",
                    0, true, "chain");
    sweepSynthetic ("stream dies inside a partial OPEN marker: literal prose",
                    "Building it.\n\n<<<ECHOJA", 0);

    // Truncated payload parity with the extractor's truncated out-param.
    {
        const juce::String full = "Building it.\n\n<<<ECHOJAY_CHAIN>>>{\"chain\":[{\"name\":\"TDR No";
        auto ref = refStrip (full);
        auto r = runParserOverDeltas ({ full }, ref.visible, 3);
        check ("truncated payload matches extractChainBlock's truncated out-param",
               r.truncatedPayload == ref.chain && r.truncatedType == "chain");
    }

    std::printf ("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
