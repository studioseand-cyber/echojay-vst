#pragma once

// ===========================================================================
// The reply-block extractors — THE whole-reply strip (spec 2.1 / step 3).
//
// Moved VERBATIM out of EchoJayAPI.cpp (8 Aug 2026) so the incremental
// parser's read-back test can run the REAL implementation, not a copy that
// could drift — the same single-source rule as buildChatRequestBody. The
// EchoJayAPI static methods now delegate here; every call site is
// behaviourally untouched. Only the signature lines changed (static class
// methods became free functions in this namespace); the bodies are the
// exact bytes that shipped.
//
// Delimiters: keep in sync with api/_blocks.js BLOCK_TYPES (the canonical
// registry) and the extract*BlockWeb functions in public/app.html.
//
// Tolerant-truncation contract, shared by all four: a complete block is
// extracted and stripped including both delimiters (prefix trimEnd'd,
// suffix kept verbatim); an opening delimiter with no close strips from
// the delimiter to end of reply so raw JSON never leaks into chat, and
// the partial payload is still returned for callers that can salvage it.
// ===========================================================================

#include <juce_core/juce_core.h>

namespace EJReplyBlocks
{

inline bool extractChainBlock(juce::String& replyInOut, juce::String& chainJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_CHAIN>>>";
    const juce::String kClose = "<<<END_CHAIN>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        // Complete block — extract JSON and strip entire block including delimiters
        chainJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut   = replyInOut.substring(0, start).trimEnd()
                     + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        // Truncated: opening delimiter found but no closing tag.
        // ALWAYS strip everything from <<<ECHOJAY_CHAIN>>> to end of reply so
        // raw JSON never leaks into the visible chat message.
        chainJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut   = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

inline bool extractGainBlock(juce::String& replyInOut, juce::String& gainJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_GAIN>>>";
    const juce::String kClose = "<<<END_GAIN>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        gainJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd()
                    + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        gainJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

// CHAIN_EDIT ops block (CHAIN_AI_BUILD_SPEC Phase 1c). Same tolerant
// truncation semantics as the other extractors.
inline bool extractChainEditBlock(juce::String& replyInOut, juce::String& editJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_CHAIN_EDIT>>>";
    const juce::String kClose = "<<<END_CHAIN_EDIT>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        editJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd()
                    + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        editJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut  = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

// ASK question/choices block (CHAIN_AI_BUILD_SPEC Phase 1b). Same tolerant
// truncation semantics as the chain/gain extractors.
inline bool extractAskBlock(juce::String& replyInOut, juce::String& askJsonOut)
{
    const juce::String kOpen  = "<<<ECHOJAY_ASK>>>";
    const juce::String kClose = "<<<END_ASK>>>";

    int start = replyInOut.indexOf(kOpen);
    if (start < 0) return false;

    int jsonStart = start + (int)kOpen.length();
    int end = replyInOut.indexOf(start, kClose);

    if (end >= 0)
    {
        askJsonOut = replyInOut.substring(jsonStart, end).trim();
        replyInOut = replyInOut.substring(0, start).trimEnd()
                   + replyInOut.substring(end + (int)kClose.length());
    }
    else
    {
        // Truncated: strip from the opening delimiter so raw JSON never
        // shows in chat; the (partial) payload may still parse.
        askJsonOut = replyInOut.substring(jsonStart).trim();
        replyInOut = replyInOut.substring(0, start).trimEnd();
    }
    return true;
}

} // namespace EJReplyBlocks
