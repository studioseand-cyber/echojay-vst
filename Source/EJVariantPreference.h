#pragma once
#include <JuceHeader.h>

// Which channel-config variant a collapsed base name should resolve to.
//
// WHY THIS EXISTS, MEASURED (27 Aug 2026). WaveShell registers one AU per
// channel configuration: "CLA-76 (m)" is 1-in/1-out, "CLA-76 (s)" is 2-in/2-out,
// "Abbey Road Chambers (m->s)" is 1-in/2-out. The feed offers the collapsed base
// name ("CLA-76"), which reaches nameMap through the stripParenthetical
// secondary key. That key was FIRST-WINS over an alphabetically sorted entries_,
// and "(m)" sorts before "(s)", so 199 of 289 Waves base names resolved to the
// MONO build.
//
// That is not cosmetic. The rack graph is unconditionally 2-in/2-out
// (ChainHost setPlayConfigDetails(2, 2, ...)), and a mono instance REFUSES
// setBusesLayout(2,2) -- measured, it stays 1x1. buildGraph then runs
//     for (int ch = nOut; ch < 2; ++ch)
//         graph_->addConnection({{prev, ch}, {stage.blend, ch}});   // passthrough
// so the right channel goes round the plugin undirected. A stereo source
// through a "mono" compressor gets its left channel compressed and its right
// channel dry: the two sides diverge, and nothing anywhere says so.
//
// (m->s) is WORSE, not better, despite emitting two channels. It is 1-in/2-out,
// so nOut == 2 and the passthrough loop above never runs: the right INPUT is
// discarded rather than bypassed. Measured on Abbey Road Chambers (m->s): fed a
// right-only signal, the output is silence.
//
// THE RANK, and why it is a static rule. Resolution happens in
// buildRecommendable, at scan time, with no track or rack in hand -- so the
// loader cannot follow "the rack's channel configuration". It does not need to:
// the graph is 2x2 unconditionally, at every one of its four
// setPlayConfigDetails sites, so there is exactly one configuration to be right
// for.
//
//   0  no suffix   the plain product name, always the best answer when it exists
//   1  (s)         2-in/2-out. The only variant that both reads and writes stereo
//   2  (m)         1-in/1-out. Right channel bypasses the plugin, dry but intact
//   3  (m->s)      1-in/2-out. Right channel is DISCARDED
//   4  anything    (5->5), (0->2), (6->2)... surround builds, wrong for a 2x2 rack
//
// A LOWER RANK NEVER MAKES A PLUGIN UNREACHABLE. The rule only chooses between
// registrations that already share a base name; a base name with nothing but a
// mono build still resolves to that mono build, because rank 2 beats an empty
// slot the same way rank 1 does. Measured: 5 of 289 Waves base names are (m)
// only, and they must keep working.
//
// Header-inline on purpose, the EJDialMissRows.h discipline: mapfps_test links
// the PREVIOUS build's SharedCode lib, so anything a pin exercises has to be
// compiled by the test TU itself rather than linked from a stale archive.
namespace echojay
{

/** The text inside a trailing "( ... )", or empty when there is none. */
inline juce::String channelVariantSuffix (const juce::String& fullName)
{
    auto s = fullName.trim();
    if (! s.endsWithChar (')')) return {};
    const int open = s.lastIndexOf (" (");
    if (open <= 0) return {};
    return s.substring (open + 2, s.length() - 1).trim();
}

/** Preference rank for a registration's FULL name; lower wins. See the table above. */
inline int channelVariantRank (const juce::String& fullName)
{
    const auto v = channelVariantSuffix (fullName).toLowerCase();
    if (v.isEmpty())    return 0;
    if (v == "s")       return 1;
    if (v == "m")       return 2;
    if (v == "m->s" || v == "m>s") return 3;
    return 4;
}

/** Should `candidate` replace `incumbent` as the description a base name resolves to?
    Strictly-better only, so equal ranks keep the incumbent and the first-wins
    order of entries_ still decides between two registrations of the same rank. */
inline bool channelVariantIsBetter (const juce::String& candidateFullName,
                                    const juce::String& incumbentFullName)
{
    return channelVariantRank (candidateFullName) < channelVariantRank (incumbentFullName);
}

} // namespace echojay
