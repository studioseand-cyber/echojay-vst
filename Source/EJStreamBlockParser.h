#pragma once

// ===========================================================================
// Incremental block parser for streamed replies (spec 2.1 / step 3).
//
// Sits between streamChat's text deltas and whatever renders (step 4 —
// nothing yet). Enforces the atomicity rules by CONSTRUCTION, not by the
// caller's discipline:
//
//   1. NOTHING acts on a block until its closing marker arrives. Block
//      bytes are withheld from onProse the moment an opening marker is
//      confirmed, and onBlock fires exactly once, with the complete
//      payload, only when the close marker has been seen. A half-arrived
//      block CANNOT reach applyChainEdits or the Build button through this
//      class, because it is never surfaced anywhere — a stream that dies
//      mid-block ends with the partial payload quarantined in
//      truncatedType()/truncatedPayload(), never in an onBlock call.
//   2. Prose streams; blocks do not. onProse carries only displayable
//      prose increments; the whole block resolves at once in onBlock, so a
//      chain list can never grow one plugin at a time.
//   3. Every marker-delimited block is atomic: CHAIN, CHAIN_EDIT,
//      CHAIN_RECALL, GAIN, ASK — the same set the whole-reply extractors
//      handle. Rule 1 is exactly why a half-arrived RECALL can never reach
//      the load path: it is never surfaced anywhere until its close marker.
//   4. The concatenation of every onProse string is BYTE-IDENTICAL to what
//      the whole-reply strip (EJReplyBlocks, the real extractors, in the
//      call-site order chain/gain/ask/edit) leaves behind on the assembled
//      text. That is held by a read-back test, not by this comment:
//      Tests/test_stream_block_parser.cpp replays real captured stream
//      bytes at every chunk size from 1 upward and diffs the outputs.
//
// Mechanics of rule 4, because two whole-reply quirks must be reproduced
// exactly:
//   - the extractors trimEnd() the prose BEFORE a block and keep the text
//     after it verbatim. Incrementally that means trailing whitespace can
//     never be emitted eagerly: the parser withholds the trailing
//     whitespace run plus any partial opening-marker prefix until the next
//     bytes prove what they are. If a marker follows, the whitespace is
//     dropped (that is trimEnd); if prose follows, it is flushed verbatim;
//     if the stream ends, finish() flushes it verbatim (the extractors do
//     not trim a reply with no block).
//   - the extractors take only the FIRST occurrence of each type; a second
//     block of the same type stays in the visible text as literal prose.
//     The parser reproduces that by dropping a type from its marker sets
//     once consumed — the duplicate then flows through as ordinary prose.
//
// Threading: NOT thread-safe, single consumer. Feed it from the message
// thread (streamChat's callbacks land there). Callbacks fire synchronously
// from appendDelta()/finish().
// ===========================================================================

#include <juce_core/juce_core.h>
#include <functional>

class EJStreamBlockParser
{
public:
    struct BlockEvent
    {
        juce::String type;      // "chain" | "chain_edit" | "chain_recall" | "gain" | "ask"
        juce::String payload;   // complete inner JSON, trimmed — never partial
    };

    std::function<void(const juce::String& prose)> onProse;
    std::function<void(const BlockEvent&)> onBlock;

    // ---- Slot-level reporting (STAGED_CHAIN_SPEC section 4) --------------
    // Fires when a CHAIN slot's JSON object has closed and parsed cleanly,
    // in wire order, index 0-based. A slot that does not parse is never
    // reported; nor is anything before its closing brace.
    //
    // RULE 1 IS UNCHANGED AND THIS DOES NOT WEAKEN IT. onBlock still fires
    // exactly once, on <<<END_CHAIN>>>, with the complete payload, and it
    // remains the ONLY thing applyChainEdits and the Build button ever see.
    // onSlot is a rendering signal and nothing else: a stream that dies
    // after three slots has fired onSlot three times and onBlock zero
    // times, which is precisely the honest state — rows on screen, no
    // button, nothing safe to build.
    //
    // Chain slots only. gain/ask/chain_edit payloads are not decomposed:
    // they carry no per-slot rendering and inventing one would be a second
    // parser to keep in sync with a schema that has no need of it.
    std::function<void(int index, const juce::String& slotJson)> onSlot;

    void appendDelta (const juce::String& text)
    {
        jassert (! finished);   // a finished parser is done; make a new one
        if (finished) return;   // and in release it hard-ignores late deltas
        pending += text;
        pump (false);
    }

    // Call exactly once, when the stream settles (done or error). Flushes
    // withheld prose; an unclosed block becomes the truncated record below
    // and NEVER an onBlock event (rule 1).
    void finish()
    {
        if (finished) return;
        pump (true);
        finished = true;
    }

    bool insideBlock() const noexcept        { return inBlock >= 0; }
    // Set only after finish(), only when the stream died inside a block.
    // The payload is quarantined here for callers that can salvage it
    // (mirrors the extractors' truncated out-param) — surfacing it as a
    // failed build is the caller's job, per done.chainBlock (spec 3.1).
    juce::String truncatedType() const       { return truncatedType_; }
    juce::String truncatedPayload() const    { return truncatedPayload_; }

private:
    struct Marker { const char* type; juce::String open, close; bool consumed = false; };
    static constexpr int kNumMarkers = 5;
    Marker markers[kNumMarkers] =
    {
        // Longer chain-family siblings FIRST is not required for correctness
        // (complete-marker matches are unambiguous: "<<<ECHOJAY_CHAIN>>>" is
        // not a prefix of "<<<ECHOJAY_CHAIN_EDIT>>>" or
        // "<<<ECHOJAY_CHAIN_RECALL>>>"), but keep them first so nobody
        // "simplifies" a future scan into prefix matching.
        { "chain_recall", "<<<ECHOJAY_CHAIN_RECALL>>>", "<<<END_CHAIN_RECALL>>>" },
        { "chain_edit",   "<<<ECHOJAY_CHAIN_EDIT>>>",   "<<<END_CHAIN_EDIT>>>" },
        { "chain",        "<<<ECHOJAY_CHAIN>>>",        "<<<END_CHAIN>>>" },
        { "gain",         "<<<ECHOJAY_GAIN>>>",         "<<<END_GAIN>>>" },
        { "ask",          "<<<ECHOJAY_ASK>>>",          "<<<END_ASK>>>" },
    };

    juce::String pending;           // unemitted tail (prose mode)
    juce::String blockBuf;          // inner bytes of the open block
    int inBlock = -1;               // index into markers, -1 = prose mode
    bool finished = false;
    juce::String truncatedType_, truncatedPayload_;
    int slotScanFrom_ = 0;          // blockBuf index already scanned for slots
    int slotsReported_ = 0;         // next onSlot index

    // Walk blockBuf from slotScanFrom_ and report every COMPLETE top-level
    // object inside the "chain" array. Brace-depth with string/escape
    // awareness, because a settings string may legally contain braces or
    // quotes and a naive scan would close a slot early — reporting a
    // partial, which is the one thing this must never do.
    //
    // Anything not yet closed is left for a later pump: the scan cursor
    // only ever advances past a slot that was fully formed and parsed.
    void scanClosedSlots()
    {
        const int arrAt = blockBuf.indexOf ("\"chain\"");
        if (arrAt < 0) return;
        const int openBracket = blockBuf.indexOf (arrAt, "[");
        if (openBracket < 0) return;
        if (slotScanFrom_ < openBracket + 1) slotScanFrom_ = openBracket + 1;

        int i = slotScanFrom_, depth = 0, objStart = -1;
        bool inStr = false, esc = false;
        const int n = blockBuf.length();
        for (; i < n; ++i)
        {
            const juce::juce_wchar c = blockBuf[i];
            if (esc) { esc = false; continue; }
            if (inStr) { if (c == '\\') esc = true; else if (c == '"') inStr = false; continue; }
            if (c == '"') { inStr = true; continue; }
            if (c == '{') { if (depth == 0) objStart = i; ++depth; continue; }
            if (c == '}')
            {
                if (depth > 0 && --depth == 0 && objStart >= 0)
                {
                    const auto slot = blockBuf.substring (objStart, i + 1);
                    // Parse-gate: a slot that does not parse is not reported,
                    // and the cursor still advances so it is not retried.
                    if (juce::JSON::parse (slot).isObject() && onSlot)
                        onSlot (slotsReported_, slot);
                    ++slotsReported_;
                    slotScanFrom_ = i + 1;
                    objStart = -1;
                }
                continue;
            }
            // The chain array's own closing bracket at depth 0 ends the scan.
            if (c == ']' && depth == 0) { slotScanFrom_ = i; return; }
        }
    }

    void emitProse (const juce::String& s)
    {
        if (s.isNotEmpty() && onProse)
            onProse (s);
    }

    void pump (bool atEnd)
    {
        for (;;)
        {
            if (inBlock >= 0)
            {
                blockBuf += pending;
                pending.clear();

                // Report any chain slot that has CLOSED since the last pump.
                // Runs before the close-marker check so slots are reported
                // as they land rather than in a burst at the end.
                if (juce::String (markers[inBlock].type) == "chain")
                    scanClosedSlots();

                const int q = blockBuf.indexOf (markers[inBlock].close);
                if (q < 0)
                {
                    if (atEnd)
                    {
                        // Rule 1's terminal case: the block never closed.
                        // Quarantine, do NOT fire onBlock. The prose before
                        // the opening marker was already emitted trimEnd'd,
                        // which is exactly the extractors' truncated branch.
                        truncatedType_    = markers[inBlock].type;
                        truncatedPayload_ = blockBuf.trim();
                    }
                    return;
                }

                BlockEvent ev { markers[inBlock].type,
                                blockBuf.substring (0, q).trim() };
                pending  = blockBuf.substring (q + (int) markers[inBlock].close.length());
                blockBuf.clear();
                markers[inBlock].consumed = true;   // first occurrence only, like the extractors
                inBlock = -1;
                if (onBlock)
                    onBlock (ev);
                continue;   // the remainder may hold more prose/markers
            }

            // ---- prose mode ----
            // Earliest complete, unconsumed opening marker in the buffer.
            int foundIdx = -1, foundPos = -1;
            for (int i = 0; i < kNumMarkers; ++i)
            {
                if (markers[i].consumed) continue;
                const int p = pending.indexOf (markers[i].open);
                if (p >= 0 && (foundPos < 0 || p < foundPos))
                {
                    foundPos = p;
                    foundIdx = i;
                }
            }

            if (foundIdx >= 0)
            {
                // trimEnd on the prefix = the extractors' strip. The
                // holdback below guarantees the whitespace run adjacent to
                // this marker was never emitted early, so trimming the
                // buffered prefix trims ALL of it.
                emitProse (pending.substring (0, foundPos).trimEnd());
                pending = pending.substring (foundPos + (int) markers[foundIdx].open.length());
                inBlock = foundIdx;
                continue;
            }

            if (atEnd)
            {
                // No marker ever completed: the tail (including trailing
                // whitespace and any partial-marker lookalike) is literal
                // prose, verbatim — the extractors leave it untouched too.
                emitProse (pending);
                pending.clear();
                return;
            }

            // Holdback: withhold the longest suffix that is a proper prefix
            // of an unconsumed opening marker, plus the whitespace run
            // before it. Everything ahead of that is safe to emit now.
            const int len = pending.length();
            int best = 0;
            for (int i = 0; i < kNumMarkers; ++i)
            {
                if (markers[i].consumed) continue;
                const int maxL = juce::jmin ((int) markers[i].open.length() - 1, len);
                for (int L = maxL; L > best; --L)
                {
                    if (pending.endsWith (markers[i].open.substring (0, L)))
                    {
                        best = L;
                        break;
                    }
                }
            }
            int holdStart = len - best;
            while (holdStart > 0
                   && juce::CharacterFunctions::isWhitespace (pending[holdStart - 1]))
                --holdStart;

            emitProse (pending.substring (0, holdStart));
            pending = pending.substring (holdStart);
            return;
        }
    }
};
