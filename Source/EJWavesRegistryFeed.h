#pragma once
#include <JuceHeader.h>
#include "ChainHost.h"            // stripParenthetical + RecommendableEntry
#include "EJVariantPreference.h"  // the channel-variant rank, unchanged and reused
#include <set>

// Which Waves products the AI feed offers, taken from the machine's own AU scan
// rather than from the curated catalog.
//
// THE SEAM, MEASURED (28 Aug 2026). PluginScanner cannot see inside WaveShell,
// so it injects 69 curated MARKETING names (expandWavesCatalog). Until now that
// list was the GATE: buildRecommendable walks the SCANNER's rows, so a Waves
// product the catalog does not name could not enter the feed however plainly it
// was installed. This machine's registry holds 614 WaveShell registrations
// collapsing to 289 products; 64 of them were in the feed, so the gate was
// hiding 225 installed, licensed, rackable plugins from the model -- and, of
// the 69 curated names, 5 are absent from the live feed themselves (four SSL
// rows lose the manufacturer "Waves" to PluginScanner's brand-prefix rule, so
// 66de26d's alias never runs for them). As a gate it was wrong in both
// directions at once.
//
// ChainHost's entries_ is the honest source: it is what this host actually
// registered, and by the time buildRecommendable hands it here as `loadable` it
// is arch-gated, crash-blacklist-gated, oversize-gated, format-filtered and
// session-load-failure-gated. Nothing in this header re-decides any of that.
//
// THE CATALOG IS KEPT, DEMOTED FROM A GATE TO A RANKING. A catalog row is a
// scanner row, so it still resolves in buildRecommendable's main loop, at its
// own position, under the marketing name real sessions use; these rows are
// appended after it. That ordering is what the 69 now do, and it is worth
// keeping: they encode which Waves plugins turn up in real sessions, which no
// enumeration recovers. They are also the only Waves names cold start has --
// with no chain_entries.xml there is no registry to read, buildRecommendable
// resolves nothing, and the editor falls back to injecting the scanner's full
// list.
//
// SCOPED BY THE AU MANUFACTURER CODE, NOT BY A NAME. WaveShell registers every
// Waves product as an AudioUnit under Waves' four-character vendor code, and
// JUCE puts that code last in the AU plugin identifier:
//     "AudioUnit:Effects/aufx,STES,ksWV"
//                             ^^^^  ^^^^ manufacturer
//                             subtype
// Measured on this machine's 2,261-row registry: 'ksWV' selects 614 rows,
// manufacturerName == "Waves" selects the same 614, and the two sets are equal.
// The code is preferred anyway, for the reason in the parked WaveShell-name
// item: the other two WaveShell matches in this codebase key on strings Waves
// owns and re-spells (a bundle filename, a marketing vendor string), and a
// V12->V15 reorg is a concrete trigger for both. A registered AU vendor code is
// the one Waves identifier a rename cannot move without breaking every session
// that ever saved one of their plugins. It also refuses "Wavesfactory", a
// different vendor whose NAME contains the string.
//
// VST3 IS OUT OF SCOPE BY MEASUREMENT, not by choice: the VST3 WaveShell is a
// single thin shell row in entries_ (its members are enumerated on demand, one
// module at a time), so there are no per-product VST3 rows here to select. If
// P16's out-of-process enumeration ever lands them they will arrive with no AU
// identifier and this predicate will not claim them, which is the conservative
// direction and a deliberate one.
//
// Header-inline on purpose, the EJVariantPreference.h discipline: mapfps_test
// links the PREVIOUS build's SharedCode lib, so anything a pin exercises has to
// be compiled by the test TU itself rather than linked from a stale archive.
// Everything that DECIDES lives here for that reason; the call site in
// buildRecommendable is an append loop with no decisions left in it.
namespace echojay
{

/** Waves' registered AudioUnit manufacturer code. */
inline const char* wavesAuManufacturerCode() { return "ksWV"; }

/** True when this description is a product registered by WaveShell.

    Reads the manufacturer field of JUCE's AU plugin identifier, which is the
    last comma-separated token of "AudioUnit:<group>/<type>,<subtype>,<manu>".
    Anything that is not an AudioUnit, or whose identifier does not carry that
    code, is not ours. */
inline bool isWaveShellRegistration (const juce::PluginDescription& d)
{
    if (d.pluginFormatName != "AudioUnit") return false;
    if (! d.fileOrIdentifier.startsWith ("AudioUnit:")) return false;
    return d.fileOrIdentifier.fromLastOccurrenceOf (",", false, false).trim()
             == wavesAuManufacturerCode();
}

/** One feed row: the product name the model is offered, and the registration
    that name will load. */
struct WavesFeedRow
{
    juce::String            name;   // collapsed base name, e.g. "CLA-76"
    juce::PluginDescription desc;   // the chosen registration, e.g. "CLA-76 (s)"
};

struct WavesFeedRows
{
    std::vector<WavesFeedRow> rows;            // to append, in registry order
    int products        = 0;                   // products the registry offers
    int alreadyOffered  = 0;                   // already in the feed under another name
    int nameTakenByOther= 0;                   // name already used for a DIFFERENT plugin
    int untickedInSettings = 0;                // the user unticked this product
};

/** The Waves products this registry offers that the feed does not carry yet.

    COLLAPSED TO THE PRODUCT NAME. WaveShell registers one AU per channel
    configuration -- "CLA-76 (m)", "CLA-76 (s)" -- and the feed carries the base
    name, because that is the name the product has, the name a user types, and
    the name the model's training data holds. Measured here: 614 registrations
    collapse to 289 products, every suffixed row in the whole 2,261-row registry
    is a Waves row (614 of 614), and exactly one name in the 990-row scanner feed
    carries a parenthetical -- so the suffix is a WaveShell artefact rather than
    a feed convention, and collapsing it costs nothing anywhere else.

    WHICH REGISTRATION a product resolves to is decided by the rank in
    EJVariantPreference.h and nothing else: the rack graph is unconditionally
    2-in/2-out, so "(s)" is the right build wherever it exists, and a base name
    with only a mono registration still resolves, to that mono build. Reusing
    that header rather than re-deciding here is the point -- 9c4f629 measured
    the rule and 5abe833 measured the fallbacks, and there must not be a second
    copy of it to drift.

    TWO REFUSALS, BOTH ABOUT NOT SAYING THE SAME THING TWICE. A product already
    in the feed under a catalog marketing name is not offered again under its
    shell name (one plugin, two names, is worse than one name). A base name
    already used by a DIFFERENT registration is left to whoever holds it: the
    feed is a name list, and a name that means two plugins means neither. Both
    are counted rather than silently dropped.

    THE FIRST REFUSAL KEYS ON THE PRODUCT, NOT ON THE REGISTRATION, and that is
    a measured correction rather than caution: keyed on the registration it let
    ONE product through twice on this machine, because the feed's catalog row
    had landed on a DIFFERENT channel build of it than the rank picks here. The
    model would then have been offered two names for one plugin, at two channel
    configurations, with nothing to choose between them. A product is offered
    once, whatever variant of it is already there.

    A THIRD REFUSAL: THE SETTINGS TICK (30 Aug 2026). The scanner walk skips a
    row the user unticked; this path walked `loadable`, which is filtered for
    format, session load failure, crash blacklist and architecture -- and for
    nothing the user chose. So unticking a Waves plugin did not remove it from
    the feed. It RENAMED it: the disabled scanner row left `resolved`, its
    product therefore left offeredProducts, and this function added the product
    straight back under its shell base name. An exclusion that silently becomes
    a rename is worse than one that fails loudly.

    WHY THE CALLER PASSES PRODUCT NAMES AND NOT UIDS, which is the whole reason
    the check could not simply be copied from the scanner walk. Scanner uids are
    makeUid(name, manufacturer) over the CATALOG MARKETING name; the
    registrations here carry the SHELL name. "API 2500" keys api_2500_waves and
    "API-2500 (s)" keys api-2500_waves -- one hyphen apart and never equal.
    Measured on this machine: of 289 registry products only 31 produce a uid any
    scanner row also produces, so a uid test would have missed 258 of them and
    looked like it worked. The caller resolves each unticked row through the
    SAME name ladder it uses to offer one, and passes the product names that
    ladder lands on; identity is thereby established the one way this codebase
    has ever established it between these two vocabularies.

    THE CALLER HANDS OVER REGISTRATIONS, NOT PRODUCTS, and that split is the
    point: deciding which registrations are Waves and how a registration
    collapses to a product are THIS header's decisions, and the call site must
    not hold a second copy of either. The caller resolves an unticked row to a
    registration -- the one thing only it can do, because only it has the name
    ladder -- and this function does the rest. */
inline WavesFeedRows wavesRegistryFeedRows (
    const juce::Array<juce::PluginDescription>& loadable,
    const std::vector<ChainHost::RecommendableEntry>& alreadyResolved,
    const std::vector<juce::PluginDescription>& untickedRegistrations = {})
{
    // The unticked registrations, collapsed the same way everything else here
    // is. A non-WaveShell registration in this list is ignored rather than
    // trusted: the caller resolves every disabled row it has, of any vendor,
    // and scope is decided here.
    std::set<juce::String> untickedProducts;
    for (const auto& d : untickedRegistrations)
        if (isWaveShellRegistration (d))
            untickedProducts.insert (ChainHost::stripParenthetical (d.name));

    WavesFeedRows out;

    // What the feed already says. displayName is exactly what buildRecommendable
    // pushed into pushedNames, and the product is read off the RESOLVED
    // registration rather than off that display name -- the display name is a
    // marketing name whose spelling shares nothing with the shell's ("SSL
    // E-Channel" is "SSLChannel (s)"), so comparing names would see none of it.
    std::set<juce::String> offeredNames, offeredProducts;
    for (const auto& r : alreadyResolved)
    {
        offeredNames.insert (r.displayName);
        if (isWaveShellRegistration (r.desc))
            offeredProducts.insert (ChainHost::stripParenthetical (r.desc.name));
    }

    // Collapse the registrations to products, best variant per product, in
    // registry order (entries_ is sorted, so the order is stable and the
    // appended block reads alphabetically).
    juce::StringArray order;
    std::map<juce::String, juce::PluginDescription> best;
    for (const auto& d : loadable)
    {
        if (! isWaveShellRegistration (d)) continue;
        const auto base = ChainHost::stripParenthetical (d.name);
        auto it = best.find (base);
        if (it == best.end())
        {
            order.add (base);
            best.emplace (base, d);
        }
        else if (channelVariantIsBetter (d.name, it->second.name))
        {
            it->second = d;
        }
    }
    out.products = order.size();

    for (const auto& base : order)
    {
        const auto& d = best[base];
        if (! offeredProducts.insert (base).second)
        {
            ++out.alreadyOffered;
            continue;
        }
        // AFTER alreadyOffered on purpose: a product still carried by an ENABLED
        // catalog row is offered by that row, and counting it as unticked here
        // would say the user refused something they are still being shown.
        if (untickedProducts.count (base) > 0)
        {
            ++out.untickedInSettings;
            continue;
        }
        if (! offeredNames.insert (base).second)
        {
            ++out.nameTakenByOther;
            continue;
        }
        out.rows.push_back ({ base, d });
    }
    return out;
}

} // namespace echojay
