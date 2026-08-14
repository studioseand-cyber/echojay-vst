#pragma once

// ===========================================================================
// Saved-chain recall: the PURE decision logic (14 Aug 2026).
//
// Free functions over juce_core only, no editor types, so the console test
// harness (Tests/test_stream_block_parser.cpp, EJStreamTests) can exercise
// the exact decisions the editor makes: the id gate, the replace-ask gate,
// and the chip payload read. The editor (PluginEditor.cpp handleChainRecall
// and the ASK chip onClick) calls these and adds only the side effects:
// logging, the ask shelf, and openSavedChain.
//
// The contract these encode:
//   - an id that does not resolve against the LOCAL list refuses; the
//     server already allowlisted the id against the same list, but the
//     plugin is the one doing the destructive thing, so it checks again.
//   - an empty rack loads directly, no question; a non-empty rack asks
//     first (through the ASK shelf; AlertWindow is banned on host-driven
//     paths, see the Logic behind-the-window note in PluginEditor.cpp).
// ===========================================================================

#include <juce_core/juce_core.h>

namespace EJRecall
{

// Parse a <<<ECHOJAY_CHAIN_RECALL>>> payload. id is required; name is
// display-only and may be empty. Returns false on anything unusable.
inline bool parseBlock (const juce::String& json,
                        juce::String& idOut, juce::String& nameOut)
{
    idOut.clear(); nameOut.clear();
    auto v = juce::JSON::parse (json);
    auto* o = v.getDynamicObject();
    if (o == nullptr) return false;
    idOut   = o->getProperty ("id").toString().trim();
    nameOut = o->getProperty ("name").toString().trim();
    return idOut.isNotEmpty();
}

// The recall_id a server-authored ASK choice carries ("" when absent).
// Reading it from the choice var, not a parallel array, so the chip index
// mapping (skipped empty labels, the 4-chip cap) cannot desynchronise the
// id from the label.
inline juce::String choiceRecallId (const juce::var& choice)
{
    auto* o = choice.getDynamicObject();
    return o != nullptr ? o->getProperty ("recall_id").toString().trim()
                        : juce::String();
}

enum class Decision { Refuse, LoadDirect, AskReplace };

inline Decision decide (bool idInLocalList, int rackSlots)
{
    if (! idInLocalList) return Decision::Refuse;
    return rackSlots > 0 ? Decision::AskReplace : Decision::LoadDirect;
}

} // namespace EJRecall
