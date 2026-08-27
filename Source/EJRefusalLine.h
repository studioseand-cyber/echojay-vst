#pragma once
#include <JuceHeader.h>
#include <vector>

// The refusal sentence the chat bubble shows when the server refused to add
// one or more plugins to a chain.
//
// EXTRACTED FROM announceRefusedOps (27 Aug 2026) so the gate can assert the
// TEXT rather than the source. It lived inline in a PluginEditor member, which
// the test cannot call: mapfps_test links the previous build's SharedCode lib,
// so anything a pin exercises has to be header-inline and compiled by the test
// TU itself. Nothing about the wording changed in the move -- the two branches
// below are byte-for-byte what shipped -- except the remedy clause, which is
// now conditional. See refusalClauseApplies.
namespace echojay
{

struct RefusedOp
{
    juce::String name;     // plugin name, as the server spelled it
    juce::String reason;   // the SERVER's sentence. Never paraphrased here.
    juce::String caseId;   // "not_owned" | "no_map" | "unresolved", or empty
};

// WHICH REFUSALS THE AUTO-DIAL TOGGLE ACTUALLY GOVERNS.
//
// "Turn off only-suggest-plugins-EchoJay-can-auto-dial" is a real remedy for a
// plugin the user HAS and EchoJay cannot dial. For a plugin they do not have it
// is a false one: no setting installs a plugin, and sending them to Settings to
// hunt for a toggle is the same wrong-thing-to-look-for that the "no settings
// map" sentence used to be for that case.
//
// FOUR STATES, ALL DELIBERATE:
//   no_map, unresolved  the toggle governs the outcome        -> clause rides
//   not_owned           nothing in Settings can change it     -> no clause
//   empty               a server older than 558fc1d, which had one case and one
//                       sentence, and that sentence was the no_map one. Absent
//                       is therefore NOT unknown, it is no_map by construction,
//                       and suppressing there would drop a correct remedy from
//                       every refusal such a server sends.
//
// Per TURN, not per name: the clause is appended to one composite sentence
// covering every refused name, so a mixed turn cannot carry the clause for some
// names and not others. It rides when at least one refusal is governed, which
// mirrors `out.refused.some(...)` in the server's api/_auto-dial-check.js.
inline bool refusalClauseApplies (const std::vector<RefusedOp>& ops)
{
    for (const auto& op : ops)
    {
        const auto c = op.caseId.trim();
        if (c.isEmpty() || c == "no_map" || c == "unresolved")
            return true;
    }
    return false;
}

// Composes the whole bubble line. Names are deduped in arrival order, first
// spelling wins, and an op with no name is skipped -- unchanged from the
// original collect loop. Returns empty when there is nothing to say.
inline juce::String refusalLineFor (const std::vector<RefusedOp>& ops)
{
    juce::StringArray names, reasons, perName;
    for (const auto& op : ops)
    {
        const auto nm = op.name.trim();
        if (nm.isEmpty() || names.contains(nm)) continue;
        const auto why = op.reason.trim();
        names.add(nm);
        reasons.addIfNotAlreadyThere(why);
        perName.add(why.isEmpty() ? nm : nm + " (" + why + ")");
    }
    if (names.isEmpty()) return {};

    const bool one = names.size() == 1;
    juce::String line;
    if (reasons.size() == 1 && reasons[0].isNotEmpty())
        // One reason for all of them, so it reads as a sentence instead of
        // repeating itself per name. Before 27 Aug this was the ONLY shape the
        // server emitted; with three cases a mixed turn now reaches the other.
        line = names.joinIntoString(", ") + (one ? " was not added: " : " were not added: ")
             + reasons[0] + ".";
    else
        // Mixed or missing reasons: each name carries its own, the same
        // fallback the dropped_controls copy uses.
        line = perName.joinIntoString("; ") + (one ? " was not added." : " were not added.");

    if (refusalClauseApplies (ops))
        line += one ? " Turn off \"only suggest plugins EchoJay can auto-dial\" in Settings"
                      " if you want it anyway."
                    : " Turn off \"only suggest plugins EchoJay can auto-dial\" in Settings"
                      " if you want them anyway.";
    return line;
}

} // namespace echojay
