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

// ---- A RACK'S REFUSAL, IN WORDS - THE ONE COMPOSER (moved here 20 Oct 2026) ------------------
//
// It lived as a file-static in EJAgentExecutorDo.cpp and had ONE caller: the line on the step row.
// His 09:12 run showed why that was not enough - he opened Details on a refused step and read the
// edit protocol talking to whoever sent the ops:
//
//     the rack's ack: status=not_applied, reason=op 1 invalid: unknown operation "set_level",
//     slots now 2
//     op 1 invalid: unknown operation "set_level"
//
// *"A refused step shows one plain line ... with the detail behind Details, never the ack text."*
// So the CLIENT needs the same translation for what it discloses, and a second copy of it is how
// the line and the detail come to describe one refusal differently. Header-inline, by the same rule
// the rest of this file keeps: a gate can drive the shipped composer rather than a copy.
// ---- A RACK'S REFUSAL, IN WORDS (16 Oct 2026, Sean's step 8) ---------------------------------
//
// He read this on a failed line: "op 1 invalid: ...". It is ChainHost's edit pre-flight speaking in
// the vocabulary of the protocol it validates - the sender's vocabulary, not a person's. A person
// has no "op 1", and I4 now bans the phrase outright, so this is where the translation happens: the
// one seam where a rack's refusal becomes the line on the row.
//
// Narrow on purpose. The prefix is dropped and the DETAIL is kept, because the detail is the part
// that says what was wrong; the only phrase translated is the one piece of jargon inside it that a
// person cannot act on. Anything this does not recognise comes through exactly as the rack wrote it -
// a refusal nobody can read is better than a refusal nobody is shown.
inline juce::String plainRefusal (const juce::String& raw)
{
    auto t = raw.trim();
    if (t.isEmpty()) return t;
    // ---- LINE BY LINE, BECAUSE THE PREFIX IS RARELY FIRST (the green run said so) -------------
    // My first cut tested the WHOLE string, and the refusal that actually reaches a person is two
    // lines: "the batch was refused before it started\nop 1 invalid: "EchoJay Transient" not in the
    // loadable plugin list". The prefix was on the second line and came through untouched - the rig
    // caught it on the fixed build, which is the only reason this is right now.
    juce::StringArray out;
    for (auto line : juce::StringArray::fromLines (t))
    {
        auto l = line.trim();
        if (l.isEmpty()) continue;
        if (l.startsWithIgnoreCase ("op ") && l.containsIgnoreCase (" invalid:"))
        {
            auto detail = l.fromFirstOccurrenceOf (" invalid:", false, false).trim();
            if (detail.startsWithIgnoreCase ("unknown operation"))
            {
                const auto quoted = detail.fromFirstOccurrenceOf ("\"", false, false)
                                          .upToLastOccurrenceOf ("\"", false, false);
                // HIS WORDS, 20 Oct 09:12: *"A refused step shows one plain line ('That step isn't
                // something this rack can do.')"*. The old line named the op - "this rack has no way
                // to \"set level\"" - which reads as a capability statement about a verb the user
                // never typed. The STEP ROW already says which step it was ("Level match"), so the
                // line's job is only to say that it cannot be done.
                juce::ignoreUnused (quoted);
                detail = "That step isn't something this rack can do.";
            }
            // "not in the loadable plugin list" is the other one a person reads often, and it is
            // true but mechanical: it means the rack could not find that plugin to load.
            else if (detail.containsIgnoreCase ("not in the loadable plugin list"))
            {
                const auto quoted = detail.fromFirstOccurrenceOf ("\"", false, false)
                                          .upToLastOccurrenceOf ("\"", false, false);
                detail = (quoted.isNotEmpty() ? quoted : juce::String ("that plugin"))
                       + " is not one this rack can load here";
            }
            l = detail;
        }
        out.add (l);
    }
    if (out.isEmpty()) return t;
    // Led by the plain sentence when the rack's own first line is the protocol's, not a person's.
    auto joined = out.joinIntoString (" - ");
    if (joined.startsWithIgnoreCase ("the batch was refused"))
        joined = "I could not make that change" + joined.fromFirstOccurrenceOf ("started", false, false);
    return joined;
}


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
