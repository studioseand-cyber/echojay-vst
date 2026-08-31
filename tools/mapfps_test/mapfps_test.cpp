/*
  fpForIdentity self-test, born from a live defect: mapFps absent from every
  chain turn because the identity join missed on the version component for
  all 740 recommendable entries. The entries cache's AU version field
  carries the component triple ("aufx,76CM,ksWV") while identityToFp_ is
  keyed on real versions ("12.0.0"), so the exact key could never hit and
  buildMapFpsJson returned "{}" on every send. The same join was duplicated
  in getDialableRecommendableNames, so it shipped broken twice.

  Two halves:

  1. Behavioural: the helper's four outcomes, with the ambiguous branch
     kept deliberately even though it has no live trigger today. It is not
     dead code: the moment completeLoad fingerprints an updated binary the
     uid holds two version keys with two fps, and the refusal is what stops
     the fallback serving the wrong binary's controls.

  2. Structural: neither ChainHost call site may contain a direct
     identityToFp_ lookup. Both must go through the helper, because two
     hand-rolled copies of this join drifting apart is exactly how the bug
     came to exist in duplicate.

  The header under test is header-only inline code: including it IS the
  shipped implementation. The SharedCode link is for the JUCE symbols only.
*/

#include <JuceHeader.h>
#include "EJDialTally.h"         // dial-4 A8: header-inline, the shipped tally
#include "EJDialMissRows.h"      // A9 step 1: header-inline, the shipped row set
#include "EJSettingsClip.h"      // 6a: header-inline, the shipped model-side clip
#include "EJParamReads.h"        // 6c §8: header-inline, the shipped read serialiser
#include "EJRefusalLine.h"      // refusal bubble: header-inline, the shipped composer
#include "EJDisableReasons.h"  // disable provenance: header-inline, shipped
#include "EJVariantPreference.h" // Waves channel-variant rank: header-inline, shipped
#include "EJWavesAlias.h"       // Waves marketing-name alias: header-inline, shipped
#include "EJWavesRegistryFeed.h" // Waves feed source swap: header-inline, shipped
#include "EJNameLadder.h"       // the match ladder + its variant preference: header-inline, shipped
#include "EchoJayParamMaps.h"
#include "EchoJayParamApply.h"
#include "EchoJayHistoryTrim.h"
#include "EchoJayChannelChats.h"
#include "EchoJayAPI.h"          // history-resend pin runs the REAL buildChatRequestBody
#include "PluginScanner.h"
#include "PluginCatalog.h"
#include <fstream>
#include <sstream>

static int passN = 0, failN = 0;
static void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok) { ++passN; std::cout << "  ok    " << name << "\n"; }
    else    { ++failN; std::cout << "  FAIL  " << name
                                 << (detail.isNotEmpty() ? ("\n        " + detail) : juce::String()) << "\n"; }
}

static juce::PluginDescription makeDesc (const juce::String& format, int uid, const juce::String& version)
{
    juce::PluginDescription d;
    d.pluginFormatName = format;
    d.uniqueId         = uid;
    d.version          = version;
    return d;
}

// The one named door through EchoJayAPI's private section (friend in the
// header, CONTRACT_history_resend_pin.md): the pin runs the SHIPPED
// buildChatRequestBody from the linked lib, never a copy of it.
struct EchoJayAPIRequestPin
{
    static juce::String compose (EchoJayAPI& a, const juce::StringArray& roles,
                                 const juce::StringArray& contents)
    {
        return a.buildChatRequestBody (roles, contents, "sys", {});
    }
};

// Function body by signature: from the signature line to the first line
// that is exactly "}" at column 0. Every ChainHost method ends that way.
// A9 step 3. A "no path emits X" pin that greps RAW source cannot tell code
// from prose, so it reddens on the very comments that explain why X is retired
// — and the honest fix is to give the pin the subject it claims, not to censor
// the documentation. Strips // to end-of-line and /* */ blocks. String literals
// containing "//" would be mangled, which is fine: no pin using this looks for
// one, and the alternative is a full lexer for a grep.
static juce::String codeOnly (const juce::String& source)
{
    juce::String out;
    const auto raw = source.toStdString();
    bool inLine = false, inBlock = false;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const char c = raw[i];
        const char n = (i + 1 < raw.size()) ? raw[i + 1] : '\0';
        if (inLine)          { if (c == '\n') { inLine = false; out << c; } continue; }
        if (inBlock)         { if (c == '*' && n == '/') { inBlock = false; ++i; } continue; }
        if (c == '/' && n == '/') { inLine  = true; ++i; continue; }
        if (c == '/' && n == '*') { inBlock = true; ++i; continue; }
        out << c;
    }
    return out;
}

static juce::String functionBody (const juce::String& source, const juce::String& signature)
{
    const int start = source.indexOf (signature);
    if (start < 0) return {};
    const int end = source.indexOf (start, "\n}\n");
    if (end < 0) return {};
    return source.substring (start, end + 2);
}

int main()
{
    std::cout << "fpForIdentity join + wiring self-test\n";

    const int uidA = 0x1234abcd, uidB = 0x22224444;

    // ---- 1. Behavioural ----
    {
        std::map<juce::String, juce::String> index;
        index["AudioUnit|1234abcd|1.0.0"] = "fpOld";

        auto oc = echojay::FpLookup::miss;

        // THE LIVE SHAPE. Triple in the version field, real version in the
        // index: exact misses, uid is unambiguous, fallback serves the fp.
        check (echojay::fpForIdentity (index, makeDesc ("AudioUnit", uidA, "aufx,XXXX,YYYY"), &oc) == "fpOld"
               && oc == echojay::FpLookup::uidFallback,
               "triple-version desc falls back to the single fp under its uid");

        // Exact still wins when the version key actually matches.
        check (echojay::fpForIdentity (index, makeDesc ("AudioUnit", uidA, "1.0.0"), &oc) == "fpOld"
               && oc == echojay::FpLookup::exact,
               "matching version key resolves exact, not fallback");

        // Unknown uid: miss, empty.
        check (echojay::fpForIdentity (index, makeDesc ("AudioUnit", uidB, "1.0.0"), &oc).isEmpty()
               && oc == echojay::FpLookup::miss,
               "unknown uid is a miss");

        // Format is part of the pair: a VST3 desc must not borrow an AU fp.
        check (echojay::fpForIdentity (index, makeDesc ("VST3", uidA, "aufx,XXXX,YYYY"), &oc).isEmpty()
               && oc == echojay::FpLookup::miss,
               "same uid under another format is a miss, not a fallback");
    }

    // ---- The ambiguous branch (no live trigger today, kept on purpose) ----
    {
        std::map<juce::String, juce::String> index;
        index["AudioUnit|1234abcd|1.0.0"] = "fpOld";
        index["AudioUnit|1234abcd|2.0.0"] = "fpNew";

        auto oc = echojay::FpLookup::miss;

        // One uid, two version keys, two fps: the post-update state once
        // completeLoad has fingerprinted the new binary. The fallback must
        // refuse rather than pick either.
        check (echojay::fpForIdentity (index, makeDesc ("AudioUnit", uidA, "aufx,XXXX,YYYY"), &oc).isEmpty()
               && oc == echojay::FpLookup::ambiguous,
               "one uid under two version keys with two fps refuses (ambiguous)");

        // But an exact hit inside an ambiguous pair still resolves: the
        // version key IS the discriminator when it works.
        check (echojay::fpForIdentity (index, makeDesc ("AudioUnit", uidA, "2.0.0"), &oc) == "fpNew"
               && oc == echojay::FpLookup::exact,
               "exact version key still resolves inside an ambiguous pair");

        // Two version keys AGREEING on one fp is not ambiguous: same
        // binary indexed twice must not lose its fallback.
        index["AudioUnit|1234abcd|2.0.0"] = "fpOld";
        check (echojay::fpForIdentity (index, makeDesc ("AudioUnit", uidA, "aufx,XXXX,YYYY"), &oc) == "fpOld"
               && oc == echojay::FpLookup::uidFallback,
               "two version keys agreeing on one fp keep the fallback");
    }

    // ---- The zero-uid guard (13 Aug 2026) ----
    // Every thin VST3 scan row carries uniqueId=0, so all of them share the
    // identity prefix VST3|0|: a COLLIDING key, not a missing one. One
    // fingerprint indexed under it would be handed by the uid fallback to
    // every zero-uid VST3 plugin on the machine (the two-fp case refuses as
    // ambiguous and is safe; the one-fp case is the dangerous and likelier
    // one). uniqueId==0 is therefore NO IDENTITY: refused before any
    // lookup, exact-shaped keys included.
    {
        std::map<juce::String, juce::String> index;
        index["VST3|0|1.0.0"] = "fpPoison";

        auto oc = echojay::FpLookup::miss;
        check (echojay::fpForIdentity (index, makeDesc ("VST3", 0, "1.0.0"), &oc).isEmpty()
               && oc == echojay::FpLookup::noUid,
               "zero uid refuses even an exact-shaped key against a poisoned index");
        check (echojay::fpForIdentity (index, makeDesc ("VST3", 0, "2.0.0"), &oc).isEmpty()
               && oc == echojay::FpLookup::noUid,
               "zero uid refuses the prefix fallback that would serve the poison");
    }

    // ---- Stale-map ladder state table (12 Aug 2026) ----
    // No live trigger on this machine (exact=0 means nothing here has ever
    // diverged and re-matched), so the table is pinned here and rehearsed
    // once with a planted fp before it is trusted. See STALE_LADDER notes.
    {
        using echojay::StaleRung;
        using echojay::staleLadderAtLoad;
        using echojay::staleLadderAtResolution;

        // Same fp: nothing happens. Not a correction, not a fetch.
        {
            const auto st = staleLadderAtLoad ("fpA", "fpA", false);
            check (st.rung == StaleRung::noDivergence
                   && ! st.correctIndex && ! st.kickRefetch && ! st.markSlot,
                   "load matching the index is no divergence and writes nothing");
        }
        // Empty index entry: new knowledge, not staleness. Correct the
        // index, but no refetch and no slot mark - there is no stale map
        // anywhere to hold against.
        {
            const auto st = staleLadderAtLoad ({}, "fpA", false);
            check (st.rung == StaleRung::firstIndex
                   && st.correctIndex && ! st.kickRefetch && ! st.markSlot,
                   "first index corrects the index without fetch or mark");
        }
        // Divergence with the live fp's map already cached: no fetch, but
        // NOT a dial (12 Aug 2026, rung A rehearsal lied here) - the slot
        // is marked and the verdict waits for the apply. No wholesale
        // refusal any more (same day, removed): real divergence is the same
        // uid at another version, where names and ranges usually match, and
        // wrong VALUES are now refused individually by the range check
        // below, on every turn.
        {
            const auto st = staleLadderAtLoad ("fpWrongButReal", "fpLive", true);
            check (st.rung == StaleRung::mapHeld
                   && st.correctIndex && ! st.kickRefetch && st.markSlot,
                   "divergence with the live map held defers the verdict and marks");
        }
        // THE LADDER PROPER: divergence, live map absent. Correct, fetch,
        // mark, hold.
        {
            const auto st = staleLadderAtLoad ("fpOld", "fpNew", false);
            check (st.rung == StaleRung::refetch
                   && st.correctIndex && st.kickRefetch && st.markSlot,
                   "divergence without the live map corrects, fetches and marks");
        }
        // Resolution: the dial result outranks the map bookkeeping. Once
        // the apply has run, dialled/undialled derive from whether anything
        // WROTE, never from map presence.
        check (staleLadderAtResolution (true, true, true, true) == StaleRung::dialled,
               "apply ran and wrote: dialled");
        check (staleLadderAtResolution (false, true, true, true) == StaleRung::dialled,
               "the dial verdict stands whatever the fetch bookkeeping says");
        check (staleLadderAtResolution (true, true, true, false) == StaleRung::undialled,
               "apply ran against a present map and wrote nothing: undialled, not dialled");
        check (staleLadderAtResolution (true, true, false, false) == StaleRung::mapHeld,
               "map present but apply not yet run keeps the verdict deferred");
        check (staleLadderAtResolution (false, false, false, false) == StaleRung::refetch,
               "unanswered fetch keeps the slot held");
        check (staleLadderAtResolution (true, false, false, false) == StaleRung::unmapped,
               "answered without the map resolves to unmapped (card speaks)");
    }

    // ---- Range validation (12 Aug 2026, replaces the whole-set refusal) ----
    // Clamp-to-rail-and-report-applied was the actual defect: Release 100
    // (bx_townhouse's millisecond vocabulary) moved CLA-76's seven-position
    // knob to position 7 and read APPLIED. The refusal lives at the value
    // level, every turn, with a guard band sized for float32-vs-decimal
    // bound mismatch (parts-per-million to <0.1%) against a defect class of
    // span multiples.
    {
        using echojay::valueWithinMappedRange;
        check (! valueWithinMappedRange (100.0f, 1.0f, 7.0f),
               "the defect: 100 against [1 .. 7] refuses");
        check (valueWithinMappedRange (7.0f, 1.0f, 7.0f),
               "an exact upper-bound ask is in range");
        check (valueWithinMappedRange (20.0f, 1.0f, 19.9999981f),
               "a documented max against its float32-rendered bound is in range");
        check (! valueWithinMappedRange (7.2f, 1.0f, 7.0f),
               "3% over the top refuses (outside the 0.1%-of-span band)");
        check (! valueWithinMappedRange (0.5f, 1.0f, 7.0f),
               "below the bottom refuses");
        check (valueWithinMappedRange (-18.0f, -60.0f, 0.0f),
               "negative-range in-range value passes (threshold-style table)");
    }

    // ---- Tick-state single store (13 Aug 2026) ----
    // The enabled flag is DERIVED from disabledUids at read (stampEnabled),
    // never stored: two copies of the tick state disagreed live (483 uids
    // on disk against 418 mirrored flags, the resolver reading the stale
    // side while the user's unticks went nowhere). The pin the incident
    // demands: change the disabled set, restamp, enabled MOVES.
    {
        std::vector<ScannedPlugin> list(2);
        list[0].uid = "waves_thing_waves";
        list[1].uid = "fabfilter_thing_fabfilter";
        std::set<juce::String> disabled;

        PluginScanner::stampEnabled (list, disabled);
        check (list[0].enabled && list[1].enabled,
               "empty disabled set stamps everything enabled");

        disabled.insert ("waves_thing_waves");
        PluginScanner::stampEnabled (list, disabled);
        check (! list[0].enabled && list[1].enabled,
               "adding a uid to the set and restamping moves enabled");

        disabled.erase ("waves_thing_waves");
        PluginScanner::stampEnabled (list, disabled);
        check (list[0].enabled,
               "removing it and restamping moves it back");

        // The accounting identity (13 Aug 2026, the Brainworx 56): enabled
        // must equal rows minus rows-whose-uid-is-disabled, counted over
        // ROWS, duplicates included. When live arithmetic broke this
        // (enabled=931 against rows=1551 and 564 disabled), the cause was a
        // second uid vocabulary, and nobody was checking the subtraction.
        std::vector<ScannedPlugin> dup(3);
        dup[0].uid = "same_uid_vendor";
        dup[1].uid = "same_uid_vendor";
        dup[2].uid = "other_vendor";
        std::set<juce::String> dis { "same_uid_vendor" };
        PluginScanner::stampEnabled (dup, dis);
        int enabledN = 0, matchN = 0;
        for (const auto& p : dup)
        {
            if (p.enabled) ++enabledN;
            if (dis.count (p.uid) > 0) ++matchN;
        }
        check (enabledN == (int) dup.size() - matchN && enabledN == 1 && matchN == 2,
               "enabled equals rows minus disabled-matching rows, duplicates counted per row");
    }

    // ---- makeUid: one identity per plugin, whatever path built it ----
    // The Brainworx 56: three inline uid constructions with different
    // normalization depths minted two identities for one plugin (scan-era
    // plugin_alliance, load-era brainworx), and the disabled set held both.
    // makeUid applies the full pipeline and is idempotent, so raw and
    // already-normalized inputs agree.
    {
        check (echojay::makeUid ("bx_boom", "Plugin Alliance")
                   == echojay::makeUid ("bx_boom", "Brainworx"),
               "the seam that shipped: Plugin Alliance and Brainworx inputs make one uid");
        check (echojay::makeUid ("bx_boom", "Plugin Alliance") == "bx_boom_brainworx",
               "and it is the canonical (load-era) spelling");
        check (echojay::makeUid ("Abbey Road Plates (s)", "Waves")
                   == echojay::makeUid ("Abbey Road Plates", "Waves"),
               "channel-variant names collapse to one uid");
    }

    // ---- Disabled-set migration: adds and canonicalises, never removes ----
    {
        using MC = PluginScanner::MigrationCounts;
        std::map<juce::String, juce::String> legacy {
            { "bx_boom_plugin_alliance", "bx_boom_brainworx" } };

        std::set<juce::String> s1 { "bx_boom_plugin_alliance" };
        MC c1 = PluginScanner::migrateDisabledSet (s1, legacy);
        check (c1.rewritten == 1 && c1.collapsed == 0
               && s1.count ("bx_boom_brainworx") == 1
               && s1.count ("bx_boom_plugin_alliance") == 0,
               "legacy-only entry is rewritten; the exclusion survives under canonical");

        std::set<juce::String> s2 { "bx_boom_plugin_alliance", "bx_boom_brainworx" };
        MC c2 = PluginScanner::migrateDisabledSet (s2, legacy);
        check (c2.collapsed == 1 && c2.rewritten == 0
               && s2.size() == 1 && s2.count ("bx_boom_brainworx") == 1,
               "both spellings collapse to canonical, dropping neither exclusion");

        std::set<juce::String> s3 { "mystery_uid_unknown_vendor" };
        MC c3 = PluginScanner::migrateDisabledSet (s3, legacy);
        check (c3.rewritten == 0 && c3.collapsed == 0 && s3.size() == 1,
               "an entry matching no known legacy spelling is left untouched");
    }

    // ---- Tick-change propagation: one action, two triggers (13 Aug) ----
    // The gap that shipped the dead half: the gate pinned stampEnabled (the
    // derivation) and covered NEITHER trigger, so a propagation path with
    // zero exercises passed as "not needed" - dropSubstitutions, live. Both
    // triggers must fire the ONE notify: the setter (the writing instance,
    // whose own save cannot inform it through the file watch) and the file
    // reload (every other instance). setPluginEnabled writes no files;
    // applyReloadedDisabledSet is the file trigger's decision core minus
    // the disk shell, which the retained EJScan lines observe live.
    {
        PluginScanner sc;
        int fired = 0;
        sc.onDisabledSetChanged = [&fired] { ++fired; };

        sc.setPluginEnabled ("waves_thing_waves", false);
        check (fired == 1, "setter trigger: an actual untick fires the notify");
        sc.setPluginEnabled ("waves_thing_waves", false);
        check (fired == 1, "setter trigger: a no-op untick does not re-fire");
        sc.setPluginEnabled ("waves_thing_waves", true);
        check (fired == 2, "setter trigger: re-ticking fires again");

        std::set<juce::String> fresh { "other_thing_vendor" };
        check (sc.applyReloadedDisabledSet (std::move (fresh)) && fired == 3,
               "file trigger: a changed reloaded set fires the notify");
        std::set<juce::String> same { "other_thing_vendor" };
        check (! sc.applyReloadedDisabledSet (std::move (same)) && fired == 3,
               "file trigger: the writer seeing its own save (changed=n) does not fire");
    }

    // ---- 2. Structural: both call sites go through the helper ----
    {
        // build_and_run.sh runs from the repo root; read the shipped source.
        std::ifstream f ("Source/ChainHost.cpp");
        std::stringstream ss;
        ss << f.rdbuf();
        const juce::String src (ss.str());
        check (src.isNotEmpty(), "Source/ChainHost.cpp readable from repo root");

        const auto mapFps   = functionBody (src, "juce::String ChainHost::buildMapFpsJson");
        const auto dialable = functionBody (src, "juce::StringArray ChainHost::getDialableRecommendableNames");
        check (mapFps.isNotEmpty(),   "buildMapFpsJson body found");
        check (dialable.isNotEmpty(), "getDialableRecommendableNames body found");

        check (! mapFps.contains ("identityToFp_.find"),
               "buildMapFpsJson has no direct identityToFp_.find");
        check (! dialable.contains ("identityToFp_.find"),
               "getDialableRecommendableNames has no direct identityToFp_.find");
        check (mapFps.contains ("fpForIdentity"),
               "buildMapFpsJson goes through fpForIdentity");
        check (dialable.contains ("fpForIdentity"),
               "getDialableRecommendableNames goes through fpForIdentity");

        // Feed name-uniqueness (13 Aug 2026, evening): the resolver
        // collapses same-name scanner rows first-wins with a counted
        // duplicates bucket, because 77 vendor-string seams beyond the
        // catalog's rules put the same name in the feed twice. With this
        // in place, EJMapFps' dupSameFp/dupDiffFp buckets should read ZERO
        // and become the live regression detector for the collapse.
        const auto buildRec = functionBody (src, "void ChainHost::buildRecommendable");
        check (buildRec.contains ("pushedNames"),
               "buildRecommendable collapses duplicate names into the duplicates counter");

        // Model-number keying pin (20 Aug 2026). c3ad9be added the
        // trailingModelNumber guard on 28 July with NO test, and it was
        // silently bypassed for three weeks because the exact-match path
        // short-circuits past it — buildRecommendable's stem keying handed
        // "AMEK EQ 250" rows the EQ 200 entry the whole time. A pin that
        // names the predicate is the difference between the guard existing
        // and the guard being protected: simplifying the keying back to bare
        // stems now fails here instead of shipping. This is a POOR SUBSTITUTE
        // for behavioural coverage — the keying lives in lambdas inside the
        // member function and cannot be exercised without extraction, which
        // belongs to the identity-keying work (findVst3Alternative /
        // popoutOnlyKey), not to this pin.
        check (buildRec.contains ("trailingModelNumber"),
               "buildRecommendable keys the feed map through the c3ad9be model-number predicate");

        // Stale-map ladder wiring: the decisions must come from the pinned
        // pure functions above, not a re-derived inline comparison, or the
        // table this test asserts stops describing the shipped behaviour.
        const auto load   = functionBody (src, "void ChainHost::completeLoad");
        const auto settle = functionBody (src, "bool ChainHost::settleStaleRung");
        check (load.isNotEmpty(),   "completeLoad body found");
        check (settle.isNotEmpty(), "settleStaleRung body found");
        check (load.contains ("staleLadderAtLoad"),
               "completeLoad decides the load rung through staleLadderAtLoad");
        check (settle.contains ("staleLadderAtResolution"),
               "settleStaleRung decides the outcome through staleLadderAtResolution");

        // Tick-state wiring: ScannedPlugin::enabled may be ASSIGNED only
        // inside stampEnabled (header-inline) - any second assignment site
        // is the mirror growing back, the fourth two-stores-disagreeing
        // defect in a week.
        {
            auto slurp = [] (const char* path)
            {
                std::ifstream fs (path);
                std::stringstream sst;
                sst << fs.rdbuf();
                return juce::String (sst.str());
            };
            auto countAssigns = [] (const juce::String& s)
            {
                int n = 0;
                for (int pos = s.indexOf ("p.enabled ="); pos >= 0;
                     pos = s.indexOf (pos + 1, "p.enabled ="))
                    ++n;
                return n;
            };
            const auto scannerCpp = slurp ("Source/PluginScanner.cpp");
            const auto scannerHdr = slurp ("Source/PluginScanner.h");
            check (countAssigns (scannerCpp) == 0,
                   "no p.enabled assignment anywhere in PluginScanner.cpp");
            check (countAssigns (scannerHdr) == 1
                   && scannerHdr.contains ("stampEnabled"),
                   "exactly one p.enabled assignment in the header (inside stampEnabled)");
            const auto getBody = functionBody (scannerCpp, "std::vector<ScannedPlugin> PluginScanner::getPlugins");
            check (getBody.contains ("stampEnabled"),
                   "getPlugins stamps from the authority set");

            // The one unlatch action must be WIRED: the processor installs
            // onDisabledSetChanged -> invalidateRecommendable at
            // construction. Without this line both triggers fire into a
            // null callback and the whole mechanism is correct and
            // unreached again.
            const auto procSrc = slurp ("Source/PluginProcessor.cpp");
            check (procSrc.contains ("onDisabledSetChanged")
                   && procSrc.contains ("invalidateRecommendable"),
                   "the processor wires the notify to the resolver unlatch");

            // Dedupe wiring (13 Aug 2026, evening): product EQUALITY must
            // use the same vocabulary as identity - addPlugin's merge and
            // loadCache's dedupe both decide via makeUid, so the 57
            // Brainworx doubles (and the Devil-Loc name-formatting pair)
            // cannot re-split under a third opinion about sameness.
            {
                const auto addBody  = functionBody (scannerCpp, "void PluginScanner::addPlugin");
                const auto loadBody = functionBody (scannerCpp, "void PluginScanner::loadCache");
                check (addBody.contains ("makeUid(name, manufacturer) == existing.uid"),
                       "addPlugin merges cross-format rows by makeUid equality");
                check (loadBody.contains ("loadedByUid"),
                       "loadCache dedupes cached rows by uid so old caches converge");
            }

            // makeUid wiring: every assignment to a uid FIELD must go
            // through makeUid. Any inline concatenation is a second uid
            // vocabulary waiting for its own Brainworx 56.
            {
                int uidAssigns = 0, viaMakeUid = 0;
                for (int pos = scannerCpp.indexOf (".uid = "); pos >= 0;
                     pos = scannerCpp.indexOf (pos + 1, ".uid = "))
                {
                    ++uidAssigns;
                    const auto lineEnd = scannerCpp.indexOf (pos, "\n");
                    if (scannerCpp.substring (pos, lineEnd).contains ("makeUid"))
                        ++viaMakeUid;
                }
                check (uidAssigns > 0 && uidAssigns == viaMakeUid,
                       "every uid field assignment in PluginScanner.cpp goes through makeUid");
            }
        }

        // Range-check wiring: applyOne must refuse through the pinned
        // helper, not re-derive a clamp - clamp-to-rail-and-report-applied
        // is the exact defect this replaced.
        std::ifstream fa ("Source/EchoJayParamApply.h");
        std::stringstream sa;
        sa << fa.rdbuf();
        const juce::String applySrc (sa.str());
        const auto applyOneBody = functionBody (applySrc, "inline ApplyResult applyOne");
        check (applyOneBody.isNotEmpty(), "applyOne body found");
        check (applyOneBody.contains ("valueWithinMappedRange"),
               "applyOne refuses out-of-range values through valueWithinMappedRange");
    }

    // ---- Chat history trim (EchoJayHistoryTrim.h, header-inline) ----
    // Born live: the newest turn (with its injections) measured 61-70KB
    // against a shared 60000-byte payload budget, so the budget was negative
    // before the walk started and history was 0 msgs on EVERY request. The
    // property under test: the newest message's size is charged nowhere, so
    // however large it grows, history admission is unaffected.
    {
        using echojay::trimChatHistory;

        // THE LIVE SHAPE: a newest message far larger than the total budget
        // must still admit the full (small) history.
        {
            const std::vector<int>  sz  { 900, 1100, 70000 };  // user, assistant, newest user
            const std::vector<char> usr { 1, 0, 1 };
            const auto r = trimChatHistory (sz, usr, 12, 24000);
            check (r.firstIdx == 0 && r.kept == 2
                   && r.droppedByCap == 0 && r.droppedByBudget == 0 && r.droppedByRole == 0,
                   "newest larger than the whole budget still admits history",
                   "firstIdx=" + juce::String (r.firstIdx) + " kept=" + juce::String (r.kept));
        }

        // History exceeding its own budget still trims oldest-first.
        {
            const std::vector<int>  sz  { 20000, 10000, 70000 };
            const std::vector<char> usr { 1, 1, 1 };
            const auto r = trimChatHistory (sz, usr, 12, 24000);
            check (r.firstIdx == 1 && r.droppedByBudget == 1 && r.kept == 1,
                   "history byte budget drops the oldest message first");
        }

        // The 12-message cap keeps at most 11 history messages plus newest.
        {
            const std::vector<int>  sz  ((size_t) 20, 100);
            const std::vector<char> usr ((size_t) 20, 1);
            const auto r = trimChatHistory (sz, usr, 12, 24000);
            check (r.firstIdx == 8 && r.droppedByCap == 8 && r.kept == 11,
                   "message-count cap still bounds history at 11 + newest");
        }

        // Role alignment: a leading assistant turn is skipped so messages[]
        // opens on a user turn; degenerate all-assistant history sends the
        // newest alone rather than an illegal first role.
        {
            const std::vector<int>  sz  { 500, 70000 };
            const std::vector<char> usr { 0, 1 };
            const auto r = trimChatHistory (sz, usr, 12, 24000);
            check (r.firstIdx == 1 && r.droppedByRole == 1 && r.kept == 0,
                   "leading assistant turn is skipped, newest still sent");
        }

        // Null trim: everything fits, nothing dropped, counters say so.
        {
            const std::vector<int>  sz  { 300, 400, 500 };
            const std::vector<char> usr { 1, 0, 1 };
            const auto r = trimChatHistory (sz, usr, 12, 24000);
            check (r.kept == 2 && r.total == 2
                   && r.droppedByCap == 0 && r.droppedByBudget == 0 && r.droppedByRole == 0,
                   "null trim reports kept == total with zero drops");
        }

        // Single-message conversation: no history, newest sent, no crash.
        {
            const std::vector<int>  sz  { 70000 };
            const std::vector<char> usr { 1 };
            const auto r = trimChatHistory (sz, usr, 12, 24000);
            check (r.firstIdx == 0 && r.kept == 0 && r.total == 0,
                   "single-message conversation trims to nothing safely");
        }

        // Structural: buildChatRequestBody must decide through the helper,
        // not through a re-derived inline walk, and the newest message's
        // size must not feed any budget arithmetic there.
        {
            std::ifstream fapi ("Source/EchoJayAPI.cpp");
            std::stringstream sapi;
            sapi << fapi.rdbuf();
            const juce::String apiSrc (sapi.str());
            const auto body = functionBody (apiSrc, "juce::String EchoJayAPI::buildChatRequestBody");
            check (body.contains ("echojay::trimChatHistory"),
                   "buildChatRequestBody decides through trimChatHistory");
            check (! body.contains ("maxPayloadBytes"),
                   "the shared payload budget (newest charged against history) is gone");
        }

        // ---- History resend pin (CONTRACT_history_resend_pin.md, 21 Aug
        // 2026, saas-authored). The server's offer_accepted brief
        // suppression counts registry-resolvable plugin names in the prior
        // assistant turn AS RESENT ON THE CHAT BODY (threshold 3); a client
        // that truncates history starves the count and the suppression
        // silently never fires — no error, just the brief-after-consent
        // defect back with everything green. These four run the REAL
        // buildChatRequestBody through the one friend hook, never a
        // reimplementation. Case 1 is equality WITH length on purpose:
        // "contains" would pass on a truncated string.
        {
            // THE COPY, NOT A MODEL (contract's own words): the real
            // proposal turn, verbatim from the saas suite's fixture
            // (scripts/test-chain-briefs.mjs; store provenance
            // preview:ej:replies:2026-08-21, t=1787292066245,
            // rid=44rnh-1787292040906-9e61d094ada3, 1,575 chars). Both
            // repos pin the same artefact. No historyStripMarkers text and
            // no "\n\n[" anywhere in it, so nothing strips.
            const juce::String prose = juce::String::fromUTF8(R"EJFIX(Here's a solid mix bus chain for what you're working on. The current three slots are being replaced entirely, so this is a full rebuild.

The order is: tonal shaping first, then glue compression, then multiband control for the low end and upper mids, then harmonic texture, then a true-peak limiter at the end.

Slot 1 - AMEK EQ 200. A musical, console-style EQ to shape the tonal balance before anything squeezes it. High-pass around 30 Hz to clear subsonic garbage, a gentle low-mid scoop around 300-400 Hz if the mix is thick, and a small high shelf lift around 12 kHz for air.

Slot 2 - SSL Native Bus Compressor 2. The classic glue compressor. 4:1 ratio, attack around 30 ms so the transients breathe through, release on auto, threshold set to about 2-4 dB of gain reduction on the loudest sections. This is the glue.

Slot 3 - Drawmer 1973. A three-band compressor to handle anything the bus comp misses in specific frequency zones - tighten the low end without touching the mids, or pull back a harsh upper mid if the mix needs it.

Slot 4 - Black Box Analog Design HG-2. Second or third harmonic saturation to give the bus warmth and density. Drive it lightly - you want the colour, not obvious distortion.

Slot 5 - Newfangled Elevate. Adaptive limiting and loudness maximiser at the end of the chain. Set your ceiling to -0.3 dBTP with true-peak on, and use it to bring the mix to a competitive loudness without squashing the dynamics the compressors preserved.

That is five slots: EQ, glue, multiband, saturation, limiter. Want me to put that together as a chain?)EJFIX");
            check (prose.length() == 1575,
                   "the artefact is intact: 1,575 chars as stored",
                   juce::String (prose.length()));

            // Compose a real request whose history holds the assistant turn
            // under test, and return what the wire carries for it. Three
            // tiny turns: nothing trims (count cap 12, byte budget 24000),
            // so the messages array is [system, user, assistant, newest].
            auto sentAssistant = [] (const juce::String& assistantTurn) -> juce::String
            {
                EchoJayAPI api;
                juce::StringArray roles    { "user", "assistant", "user" };
                juce::StringArray contents { "build me a mix bus", assistantTurn, "yes" };
                const auto body = EchoJayAPIRequestPin::compose (api, roles, contents);
                auto v = juce::JSON::parse (body);
                auto msgs = v.getProperty ("messages", juce::var());
                if (msgs.size() != 4
                    || msgs[2].getProperty ("role", juce::var()).toString() != "assistant")
                    return "<<WRONG SHAPE: " + juce::String (msgs.size()) + " messages>>";
                return msgs[2].getProperty ("content", juce::var()).toString();
            };

            // 1. Full-length survival: byte-identical AND equal length.
            {
                const auto sent = sentAssistant (prose);
                check (sent.length() == prose.length() && sent == prose,
                       "history assistant turn resends byte-identical at full length",
                       "sent " + juce::String (sent.length()) + " of "
                           + juce::String (prose.length()) + " chars");
            }
            // 2. The strip boundary is markers, not length: cut exactly AT
            //    the marker, every byte before it intact — a future "tidy"
            //    length cap cannot hide inside the strip.
            {
                const auto marker = EchoJayAPI::historyStripMarkers()[0];
                const auto sent = sentAssistant (prose + marker
                                    + " - injection contents that must not resend]");
                check (sent == prose,
                       "history strip cuts exactly at the marker, prose before it byte-identical",
                       "sent " + juce::String (sent.length()) + " chars, expected "
                           + juce::String (prose.length()));
            }
            // 3. CURRENT BEHAVIOUR, PINNED: assistant turns are stripped
            //    too — strippedContent() has no role check — so a marker
            //    LITERAL (with its \n\n prefix) inside assistant prose cuts
            //    the turn there. The server tolerates either behaviour but
            //    counts names in exactly this text, so whichever holds must
            //    not change silently. A bracket phrase WITHOUT the newline
            //    prefix is not a marker and survives whole.
            {
                const auto marker = EchoJayAPI::historyStripMarkers()[4]; // "\n\n[CURRENT CHAIN"
                const auto cut = sentAssistant (prose + marker + " echoed injection text");
                check (cut == prose,
                       "assistant turns are marker-stripped too (current behaviour, pinned)");
                const juce::String inlinePhrase = "see the [CURRENT CHAIN block above for slots. ";
                const auto kept = sentAssistant (prose + inlinePhrase);
                check (kept == prose + inlinePhrase,
                       "a bracket phrase without the newline prefix is not a marker and survives");
            }
            // 4. The classify carrier stays separate: the chat body builder
            //    never composes priorAssistant — the short excerpt class is
            //    legal on the classify body and only there. The behavioural
            //    half of this pin is case 1 above (full length on the chat
            //    array); this half pins that the two carriers share no code.
            {
                std::ifstream fapi2 ("Source/EchoJayAPI.cpp");
                std::stringstream sapi2;
                sapi2 << fapi2.rdbuf();
                const juce::String apiSrc2 (sapi2.str());
                const auto bodyFn2 = functionBody (apiSrc2,
                    "juce::String EchoJayAPI::buildChatRequestBody");
                check (bodyFn2.isNotEmpty() && ! bodyFn2.contains ("priorAssistant"),
                       "chat body builder never composes priorAssistant");
                check (apiSrc2.contains ("setProperty(\"priorAssistant\", req.priorAssistant)"),
                       "the classify carrier composes priorAssistant at its own site");
            }
        }
    }

    // ---- Channel-chat selection (EchoJayChannelChats.h, header-inline) ----
    // A channel owns many chats (14 Aug 2026); "the" chat for a channel is
    // the most recent by ACTIVITY: updatedAt when non-empty, else created.
    // The old first-match lookup is exactly what made "New chat + assign"
    // append to the channel's existing conversation.
    {
        struct StubChat { juce::String linkUid, trackName, updatedAt, created; };
        using echojay::latestChatForLink;
        const juce::String uid ("linkA"), proj ("Song 1");

        // No match at all.
        {
            const std::vector<StubChat> chats {
                { "linkB", proj, "", "2026-08-01T10:00:00.000Z" },
                { uid, "Song 2", "", "2026-08-02T10:00:00.000Z" },
            };
            const auto p = latestChatForLink (chats, uid, proj);
            check (p.index == -1 && p.matches == 0,
                   "no (linkUid, trackName) match returns index -1, matches 0");
        }

        // One match: chosen regardless of position.
        {
            const std::vector<StubChat> chats {
                { "linkB", proj, "", "2026-08-05T10:00:00.000Z" },
                { uid, proj, "", "2026-08-01T10:00:00.000Z" },
            };
            const auto p = latestChatForLink (chats, uid, proj);
            check (p.index == 1 && p.matches == 1, "single match wins");
        }

        // Several matches where the newest by updatedAt is NOT the newest by
        // created: activity must win over creation (and over vector order --
        // the store prepends by creation).
        {
            const std::vector<StubChat> chats {
                { uid, proj, "2026-08-10T10:00:00.000Z", "2026-08-09T10:00:00.000Z" },   // created latest
                { uid, proj, "2026-08-14T10:00:00.000Z", "2026-08-01T10:00:00.000Z" },   // oldest created, newest ACTIVITY
                { uid, proj, "2026-08-12T10:00:00.000Z", "2026-08-05T10:00:00.000Z" },
            };
            const auto p = latestChatForLink (chats, uid, proj);
            check (p.index == 1 && p.matches == 3,
                   "newest by updatedAt beats newest by created",
                   "picked index " + juce::String (p.index));
        }

        // updatedAt empty on one side: created stands in for it, and a
        // never-stamped chat can still win on a newer created.
        {
            const std::vector<StubChat> chats {
                { uid, proj, "2026-08-10T10:00:00.000Z", "2026-08-02T10:00:00.000Z" },
                { uid, proj, "",                          "2026-08-13T10:00:00.000Z" },  // no updatedAt; created decides
            };
            const auto p = latestChatForLink (chats, uid, proj);
            check (p.index == 1 && p.matches == 2,
                   "empty updatedAt falls back to created for that chat");
        }

        // Structural: the editor's lookup decides through the helper -- a
        // re-derived inline first-match loop is how the one-chat-per-channel
        // behaviour existed in the first place.
        {
            std::ifstream fed ("Source/PluginEditor.cpp");
            std::stringstream sed_;
            sed_ << fed.rdbuf();
            const juce::String edSrc (sed_.str());
            const auto body = functionBody (edSrc, "juce::String EchoJayEditor::latestChannelChatId");
            check (body.contains ("echojay::latestChatForLink"),
                   "latestChannelChatId decides through latestChatForLink");
            check (! edSrc.contains ("findChannelChatId"),
                   "the first-match lookup name is gone from PluginEditor.cpp");
        }
    }

    // ---- dial-4 A8: the attempt tally, the two contract pins -----------------
    // CONTRACT_racked_slot_controls.md A8.1a / A8.1b name these two pins
    // explicitly; the built-in one is client-enforced and server-unverifiable,
    // so this suite is the only place it can ever be caught.
    {
        std::cout << "dial-4 A8 attempt tally:\n";
        using echojay::DialAttemptTally;

        // PIN 1 (A8.1a): one slot-turn that touches BOTH sites — the apply
        // walker (surviving settings, report.size()==3) and the refine site
        // (2 entries refine-dropped before the apply). applies must increment
        // EXACTLY once while requested carries both sites' counts. The
        // natural implementation bumps at each site and double-counts the
        // population while leaving requested correct: the rate survives and
        // the readability number lies.
        {
            DialAttemptTally t;
            echojay::noteDialApplySite  (t, "AudioUnit", "3d30727d", 3, /*builtin*/ false);
            echojay::noteDialRefineSite (t, /*opReachesApply*/ true, 2, /*builtin*/ false);
            check (t.totalApplies() == 1,
                   "A8.1a both-sites slot-turn: applies incremented exactly once",
                   "applies=" + juce::String (t.totalApplies()));
            check (t.totalRequested() == 5,
                   "A8.1a both-sites slot-turn: requested carries both sites' counts (3+2)",
                   "requested=" + juce::String (t.totalRequested()));
            // The refine contribution is identityless BY DESIGN (its rows are
            // too): it counts under format|| and the server flags it.
            check (t.entryFor ("AudioUnit", "3d30727d").applies == 1
                   && t.entryFor ("", "").applies == 0
                   && t.entryFor ("", "").requested == 2,
                   "A8.1a refine contribution is requested-only, under the empty identity");
        }
        // The receipt-consumed op never reaches the apply: its refine site is
        // its ONLY site, so there it does bump.
        {
            DialAttemptTally t;
            echojay::noteDialRefineSite (t, /*opReachesApply*/ false, 4, /*builtin*/ false);
            check (t.totalApplies() == 1 && t.totalRequested() == 4,
                   "A8.1a receipt-consumed op: the refine site bumps, once");
        }

        // PIN 2 (A8.1b): a built-in slot-turn contributes NOTHING — not to
        // applies, not to requested, through either site. A client that gets
        // this wrong biases every mixed-rack rate downward with no signal
        // anywhere, and most racks carry built-ins.
        {
            DialAttemptTally t;
            echojay::noteDialApplySite  (t, "EchoJay", "", 6, /*builtin*/ true);
            echojay::noteDialRefineSite (t, false, 3,        /*builtin*/ true);
            check (t.empty(), "A8.1b built-in slot-turn contributes nothing to the tally");
            check (echojay::dialTallyAdmits (false) && ! echojay::dialTallyAdmits (true),
                   "A8.1b admission rule: third-party in, built-in out");
            // Corrected 22 Aug (Sean's ruling): built-ins leave the ROWS as
            // well, so the clause's premise becomes true. The server cannot
            // catch a violation — an empty uid is also a legitimate
            // unknown-third-party shape — so this pin is the only witness.
            check (echojay::dialRowAdmits (false) && ! echojay::dialRowAdmits (true),
                   "A8.1b row admission: same rule, rows side");
        }

        // A8.1a NORMALIZATION: the keys-path count is A7.2's ENTRY semantic
        // — flat keys, plus entries of "controls", plus "bands" elements —
        // through the ONE shared implementation. The defect this pins out:
        // getDialInfos' fallback counted top-level properties, so a controls
        // object with five entries counted as ONE and the denominator
        // diverged structurally on exactly the slots the apply never saw.
        {
            auto obj = [] { return new juce::DynamicObject(); };
            juce::var flat (obj());
            flat.getDynamicObject()->setProperty ("attack_ms", 3);
            flat.getDynamicObject()->setProperty ("ratio", 4);
            check (echojay::requestedEntryCount (flat) == 2,
                   "entry count: flat keys count one each");

            juce::var five (obj());
            {
                auto* co = obj();
                for (int i = 0; i < 5; ++i)
                    co->setProperty ("c" + juce::String (i), i);
                five.getDynamicObject()->setProperty ("controls", juce::var (co));
            }
            check (echojay::requestedEntryCount (five) == 5,
                   "entry count: a controls object with five entries is FIVE, not one",
                   "got " + juce::String (echojay::requestedEntryCount (five)));

            juce::var mixed (obj());
            mixed.getDynamicObject()->setProperty ("mix_pct", 30);
            {
                auto* co = obj();
                co->setProperty ("Threshold", -18);
                co->setProperty ("Ratio", 4);
                mixed.getDynamicObject()->setProperty ("controls", juce::var (co));
                juce::Array<juce::var> bands;
                bands.add (juce::var (obj()));
                bands.add (juce::var (obj()));
                mixed.getDynamicObject()->setProperty ("bands", juce::var (bands));
            }
            check (echojay::requestedEntryCount (mixed) == 5,
                   "entry count: flat + controls entries + bands elements (1+2+2)",
                   "got " + juce::String (echojay::requestedEntryCount (mixed)));
            check (echojay::requestedEntryCount (juce::var()) == 0,
                   "entry count: non-object counts zero");
        }

        // Lifecycle: round-trip (persistence shape) and subtract-on-success
        // (A8.4: applies accumulated between stage and success survive).
        {
            DialAttemptTally t;
            echojay::noteDialApplySite (t, "AudioUnit", "aa", 5, false);
            echojay::noteDialApplySite (t, "VST3",      "bb", 2, false);
            auto rt = DialAttemptTally::fromAttemptsVar (t.toAttemptsVar());
            check (rt.totalApplies() == 2 && rt.totalRequested() == 7
                   && rt.entryFor ("VST3", "bb").requested == 2,
                   "tally round-trips through its wire/persist shape");

            auto staged = t;                                   // snapshot at body build
            echojay::noteDialApplySite (t, "AudioUnit", "aa", 4, false);  // lands mid-flight
            t.subtract (staged);                               // send succeeded
            check (t.totalApplies() == 1 && t.totalRequested() == 4
                   && t.entryFor ("AudioUnit", "aa").applies == 1,
                   "subtract-on-success keeps slot-turns accumulated between stage and reply");
            t.subtract (t);
            check (t.entries.empty(), "fully shipped tally drops to no entries");
        }

        // Structural: the wiring uses the policy functions with the RIGHT
        // once-rule arguments — the double-count lives at the call sites,
        // not in the struct, so the sites are pinned by source.
        {
            std::ifstream fed ("Source/PluginEditor.cpp");
            std::stringstream sed_;
            sed_ << fed.rdbuf();
            const juce::String edSrc (sed_.str());
            check (edSrc.contains ("noteDialTallyRefine(plug, /*opReachesApply*/ true"),
                   "apply-path refine site passes opReachesApply=true (requested-only)");
            check (edSrc.contains ("/*opReachesApply*/ false, dc->size())"),
                   "receipt-consumed refine site passes opReachesApply=false (its only site)");
            check (edSrc.contains ("noteDialTallyFromInfo(di);"),
                   "the settle walkers accumulate the population beside the rows");

            // A8.1b rows side: the exclusion lives at the ONE emitter, and
            // every slot-walker call carries di.builtin — a full-form call
            // still ending at requestedSource would be a site the refusal
            // cannot see.
            check (edSrc.contains ("if (! echojay::dialRowAdmits(builtinSlot)) return;"),
                   "A8.1b: logDialMiss refuses built-in rows via dialRowAdmits");
            // A9 §7: this literal changed when also_reasons was appended after
            // di.builtin. The BEHAVIOUR it guards is unchanged — the emitter
            // must still pass di.builtin — so the literal moves and the
            // mutation check re-proves it goes red.
            check (edSrc.contains ("di.builtin, row.alsoReasons);"),
                   "A8.1b: slot walkers pass di.builtin into the row emitter");
            check (! edSrc.contains ("di.requestedCount, di.requestedSource);"),
                   "A8.1b: no walker row call left without the builtin flag");

            // A8.8: the two censored reasons now carry requested, so the
            // batch builder's pre-contract skip cannot silently eat them.
            // unusable_map rides the walkers' full-form call (above); the
            // receipt-path refine row carries its op entry count, dc->size().
            check (! edSrc.contains ("\"unusable_map\", di.manual);"),
                   "A8.8: no bare unusable_map row call remains (requested now rides)");
            check (edSrc.contains ("juce::String(), juce::String(), dc->size(), \"keys\","),
                   "A8.8: receipt-path refine row ships requested=dc->size(), source keys");
        }
        // The keys-path count semantic, wired: getDialInfos' fallback goes
        // through the shared entry-count implementation (not the top-level
        // property count), and unusableMap is keys-designated even when the
        // apply loop ran (A8.8 — report 0 hid the row; A7.1 bars requested 0
        // from every rate).
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch_;
            sch_ << fch.rdbuf();
            const juce::String chSrc (sch_.str());
            check (chSrc.contains ("echojay::requestedEntryCount(s.structuredSettings)"),
                   "A8.1a: getDialInfos' keys fallback uses the shared entry count");
            // A9 §7 shape, second instance: this literal moved because step 3
            // split unusableMap four ways. The BEHAVIOUR it guards is
            // unchanged — all four inherit the keys designation (§3a) — so the
            // literal follows the code and the mutation re-proves it goes red.
            check (chSrc.contains ("s.dialRequestedCount >= 0 && ! wasUnusableMap"),
                   "A8.8: the ex-unusableMap statuses are keys-sourced by designation");
        }
    }

    // ---- A9 step 1: ONE dial-miss row author, three callers -----------------
    // Three settle walkers used to compose this row set by hand and had
    // drifted apart; the divergence was invisible because they are per-turn
    // ALTERNATIVES, so no single turn could show two of them disagreeing.
    {
        std::cout << "A9 step 1 dial-miss row set:\n";
        using Status = ChainHost::DialStatus;   // ChainHost is GLOBAL, not echojay::

        auto slot = [] (Status st,
                        juce::StringArray manual      = {},
                        juce::StringArray outOfRange  = {},
                        juce::StringArray readbackMiss = {},
                        juce::StringArray unconfirmed  = {},
                        juce::String staleIndexedFp    = {})
        {
            ChainHost::SlotDialInfo di;
            di.name            = "Solid EQ";
            di.fp              = "733068b9";
            di.format          = "VST3";
            di.uid             = "1a9dbcc1";
            di.status          = st;
            di.manual          = manual;
            di.outOfRange      = outOfRange;
            di.readbackMiss    = readbackMiss;
            di.unconfirmed     = unconfirmed;
            di.staleIndexedFp  = staleIndexedFp;
            di.requestedCount  = 1;
            di.requestedSource = "keys";
            return di;
        };
        auto render = [] (const ChainHost::SlotDialInfo& di)
        {
            juce::String s;
            for (const auto& r : echojay::dialMissRowsFor (di))
                s << (s.isEmpty() ? "" : ";") << r.reason << "[" << r.names.joinIntoString (",") << "]";
            return s;
        };

        // PIN 1: the 21688 break. A slot carrying BOTH a range refusal and a
        // readback mismatch used to report only the range refusal — the
        // readback row's existence depended on an unrelated field being
        // empty. Both findings are now reported, every time.
        {
            const auto di = slot (Status::writesRejected, { "Default" }, { "Attack" }, { "Default" });
            const auto got = render (di);
            check (got.contains ("readback_mismatch[Default]"),
                   "A9: readbackMiss + outOfRange together STILL emits the readback row",
                   "got " + got);
            // UPDATED BY STEP 2. Step 1 asserted a standalone unusable_map row
            // beside these two; the partition folds that verdict into each
            // control's row as also_reasons, so two controls give two rows and
            // the rollup claims neither. The 21688 guarantee this pin exists
            // for is untouched: the readback row is still here.
            check (got.contains ("out_of_range[Attack]") && ! got.contains ("unusable_map["),
                   "A9: the range refusal rides its own row and the verdict claims neither",
                   "got " + got);
            check (echojay::dialMissRowsFor (di).size() == 2,
                   "A9: two controls, two rows, neither suppressing the other",
                   "got " + juce::String ((int) echojay::dialMissRowsFor (di).size()));
        }
        // ...and with outOfRange EMPTY the readback row is still the only row
        // for that control. Step 1 expected a separate unusable_map row here;
        // step 2 carries it as also_reasons instead (pinned below).
        check (render (slot (Status::writesRejected, { "Default" }, {}, { "Default" }))
                   == "readback_mismatch[Default]",
               "A9: the outOfRange-empty case is one row for the one control");

        // PIN 2: the full reason set, so a caller cannot omit one. These are
        // exactly the reasons walker 3 used to drop on the floor.
        check (render (slot (Status::partial, { "attack" }, {}, {}, { "release" }))
                   == "stale_display_kept[release];partial[attack]",
               "A9: stale_display_kept is in the shared set (walker 3 omitted it)");
        check (render (slot (Status::noMap, { "ratio" }, { "attack" }))
                   == "out_of_range[attack];no_map[ratio]",
               "A9: out_of_range is in the shared set (walker 3 omitted it)");
        check (render (slot (Status::noMap, { "ratio" }, {}, {}, {}, "deadbeef"))
                   == "stale_unmapped[ratio]",
               "A9: a stale-ladder noMap is stale_unmapped (walker 3 said no_map)");
        check (render (slot (Status::pending, { "ratio" })) == "map_fetch_timeout[ratio]",
               "A9: pending maps to map_fetch_timeout");
        check (render (slot (Status::applied)).isEmpty()
               && render (slot (Status::none)).isEmpty(),
               "A9: a clean or untouched slot emits nothing");
        // The presence condition for the readback evidence: readbackMiss can
        // still hold the PREVIOUS apply's labels on a slot a later sweep
        // re-marked noMap/pending (it is cleared only inside the apply loop),
        // and a row asserting a readback this turn never performed would be a
        // stale claim. This is a presence test, NOT the outOfRange coupling.
        check (! render (slot (Status::noMap, { "x" }, {}, { "stale" })).contains ("readback_mismatch"),
               "A9: no readback row under a verdict whose apply never ran this turn");

        // PIN 3, structural: all three walkers route through the one emitter
        // and none of them builds a slot row by hand. Keyed on the
        // slot-derived call shape `logDialMiss(di.name` — the slotless rows
        // (edit_add_no_dial, edit_set_no_dial) and the two refine sites
        // legitimately still call logDialMiss and must NOT be caught here.
        {
            std::ifstream fed ("Source/PluginEditor.cpp");
            std::stringstream sed_;
            sed_ << fed.rdbuf();
            const juce::String edSrc (sed_.str());

            const char* walkers[] = { "void EchoJayEditor::finishChainBubbleWhenDialSettled",
                                      "void EchoJayEditor::finishEditBubbleWhenDialSettled",
                                      "void EchoJayEditor::logDialMissesWhenSettled" };
            for (const char* w : walkers)
            {
                const auto body = functionBody (edSrc, w);
                const juce::String nm (juce::String (w).fromLastOccurrenceOf ("::", false, false));
                check (body.isNotEmpty(), juce::String ("A9: found walker ") + nm);
                check (body.contains ("emitDialMissRows(di);"),
                       "A9: " + nm + " emits through the shared author");
                check (! body.contains ("logDialMiss(di.name"),
                       "A9: " + nm + " builds NO slot row by hand");
            }
            // The emitter itself is the only place a slot row is written.
            check (functionBody (edSrc, "void EchoJayEditor::emitDialMissRows")
                       .contains ("echojay::dialMissRowsFor(di)"),
                   "A9: the emitter derives its reasons from the shared author");
        }
    }

    // ---- A9 step 2: the partition ------------------------------------------
    // One declined CONTROL, one row. Further reasons for that same control
    // ride as also_reasons. The key is control identity — index when >= 0,
    // else the label — so one control that failed twice merges, while two
    // controls that failed differently stay apart.
    {
        std::cout << "A9 step 2 partition:\n";
        using Status = ChainHost::DialStatus;

        // Fixtures are the four label arrays, which is all the emitter reads.
        // An index-keyed per-control carrier was built for this and stripped:
        // the only state where it changes the answer is one control carrying
        // two causes, and that state has no code path (the causes are
        // exclusive per result, and one parameter cannot be reached twice
        // under one semanticLabel).
        auto slotWith = [] (Status st, juce::StringArray manual,
                            juce::StringArray readbackMiss = {},
                            juce::StringArray outOfRange   = {},
                            juce::StringArray unconfirmed  = {})
        {
            ChainHost::SlotDialInfo di;
            di.name = "Solid EQ"; di.fp = "733068b9";
            di.format = "VST3";   di.uid = "1a9dbcc1";
            di.status = st;
            di.manual       = std::move (manual);
            di.readbackMiss = std::move (readbackMiss);
            di.outOfRange   = std::move (outOfRange);
            di.unconfirmed  = std::move (unconfirmed);
            di.requestedCount = 1; di.requestedSource = "keys";
            return di;
        };
        // reason[names]{also,reasons} — also_reasons made visible, because its
        // ABSENCE is one of the things being pinned.
        auto show = [] (const ChainHost::SlotDialInfo& di)
        {
            juce::String s;
            for (const auto& r : echojay::dialMissRowsFor (di))
            {
                s << (s.isEmpty() ? "" : ";") << r.reason << "[" << r.names.joinIntoString (",") << "]";
                if (! r.alsoReasons.isEmpty()) s << "{" << r.alsoReasons.joinIntoString (",") << "}";
            }
            return s;
        };

        // PIN 1 — the Solid EQ case exactly (events.jsonl:166-167). One
        // control, the verdict plus readbackMiss: ONE row, reason
        // readback_mismatch, also_reasons ["writes_rejected"]. Two rows here is
        // the over-unity that put the identity's rate at 2.00 (§1b).
        //
        // STEP 3 MADE THIS DUE. §6 step 2 said the also_reasons value would be
        // ["unusable_map"] at step 2 and become ["writes_rejected"] "only after
        // step 3 splits the reason". The measured row carried manual
        // ["Default"], so the result loop ran and it was the 3609 case, which
        // is now writesRejected.
        {
            const auto di = slotWith (Status::writesRejected, { "Default" }, { "Default" });
            check (show (di) == "readback_mismatch[Default]{writes_rejected}",
                   "A9.2 PIN1 Solid EQ: one row, readback_mismatch, also writes_rejected",
                   "got " + show (di));
            check (echojay::dialMissRowsFor (di).size() == 1,
                   "A9.2 PIN1 Solid EQ: exactly one row for the one control");
        }

        // PIN 2 — the same overlap under `partial`, the largest reason in the
        // corpus (§1a). A control in both manual and readbackMiss is ONE row.
        {
            const auto di = slotWith (Status::partial, { "attack" }, { "attack" });
            check (show (di) == "readback_mismatch[attack]{partial}",
                   "A9.2 PIN2 partial: manual + readbackMiss yields one row",
                   "got " + show (di));
        }

        // PIN 3 — TWO CONTROLS, TWO ROWS. One band refused for range, another
        // band landed wrong; both collapse to the label "freq". It reads like
        // a contradiction of PIN 1 and is not: PIN 1 stops ONE control being
        // reported twice, this keeps TWO controls from being reported once.
        // Partitioning on the label alone would turn an accidentally correct 2
        // into a newly wrong 1 (§2).
        {
            const auto di = slotWith (Status::partial, { "freq" }, { "freq" }, { "freq" });
            check (echojay::dialMissRowsFor (di).size() == 2,
                   "A9.2 PIN3 two causes on one label yield TWO rows",
                   "got " + juce::String ((int) echojay::dialMissRowsFor (di).size())
                   + " -> " + show (di));
            check (show (di).contains ("out_of_range[freq]")
                   && show (di).contains ("readback_mismatch[freq]"),
                   "A9.2 PIN3 both causes survive under one name",
                   "got " + show (di));
            // PIN 3b WAS HERE AND IS DELIBERATELY GONE. It asserted that two
            // controls sharing a label under ONE cause stay one row. That
            // cannot be written as a fixture: addIfNotAlreadyThere collapsed
            // the two into a single array entry upstream
            // (ChainHost.cpp, the four addIfNotAlreadyThere calls), so a
            // SlotDialInfo carrying "two controls, same cause, same label" is
            // indistinguishable from one carrying one control. The property is
            // therefore STRUCTURAL rather than tested — the emitter cannot
            // un-collapse what it cannot see — and the undercount it protects
            // is pre-existing and parked (§2). A pin here would have asserted
            // a fixture's shape, not the code's behaviour.
        }

        // PIN 4 — a control with one reason carries no also_reasons KEY at
        // all. Not an empty array: absence must stay distinguishable.
        {
            const auto di = slotWith (Status::partial, { "ratio" });   // verdict only, no cause
            check (show (di) == "partial[ratio]",
                   "A9.2 PIN4: a single-reason row carries no also_reasons",
                   "got " + show (di));
            check (echojay::dialMissRowsFor (di)[0].alsoReasons.isEmpty(),
                   "A9.2 PIN4: alsoReasons is empty, so logDialMiss writes no key");
        }

        // Wiring: the emitter passes alsoReasons, and logDialMiss writes the
        // key only when non-empty.
        {
            std::ifstream fed ("Source/PluginEditor.cpp");
            std::stringstream sed_;
            sed_ << fed.rdbuf();
            const juce::String edSrc (sed_.str());
            check (edSrc.contains ("di.builtin, row.alsoReasons);"),
                   "A9.2: the emitter passes each row's alsoReasons");
            check (edSrc.contains ("if (! alsoReasons.isEmpty())"),
                   "A9.2: logDialMiss writes also_reasons only when non-empty");
            check (edSrc.contains ("r->setProperty(\"also_reasons\", ar);"),
                   "A9.2: the batch builder carries also_reasons onto the wire row");
        }
    }

    // ---- A9 step 3: unusable_map splits four ways, and the name retires ----
    // The old value had FOUR producers with four different owners and the name
    // described only the first. Each now reaches its own status at the site
    // that knows the fact, so the reason is a DECISION rather than an inference
    // from whether some array happens to be empty — which was the same defect
    // A9 step 1 removed (§1d: a row's identity must not depend on an unrelated
    // field).
    {
        std::cout << "A9 step 3 unusable_map split:\n";
        using Status = ChainHost::DialStatus;

        auto s3 = [] (Status st, juce::StringArray manual = {},
                      juce::StringArray readbackMiss = {})
        {
            ChainHost::SlotDialInfo di;
            di.name = "Solid EQ"; di.fp = "733068b9";
            di.format = "VST3";   di.uid = "1a9dbcc1";
            di.status = st;
            di.manual = std::move (manual);
            di.readbackMiss = std::move (readbackMiss);
            di.requestedCount = 1; di.requestedSource = "keys";
            return di;
        };
        auto render3 = [] (const ChainHost::SlotDialInfo& di)
        {
            juce::String out;
            for (const auto& r : echojay::dialMissRowsFor (di))
                out << (out.isEmpty() ? "" : ";") << r.reason
                    << "[" << r.names.joinIntoString (",") << "]";
            return out;
        };

        // PIN 1 — the 3603 case reaches map_no_coverage. report.empty(), so
        // the result loop never ran and `manual` is necessarily empty; the
        // rollup row is the only thing there is to say (§2).
        check (render3 (s3 (Status::mapNoCoverage, { "Default" })) == "map_no_coverage[Default]",
               "A9.3 PIN1: the 3603 case reaches map_no_coverage",
               "got " + render3 (s3 (Status::mapNoCoverage, { "Default" })));

        // PIN 2 — the 3609 case reaches writes_rejected.
        check (render3 (s3 (Status::writesRejected, { "Default" })) == "writes_rejected[Default]",
               "A9.3 PIN2: the 3609 case reaches writes_rejected",
               "got " + render3 (s3 (Status::writesRejected, { "Default" })));

        // PIN 3 — the 3494 case reaches map_identity_mismatch. The map does not
        // belong to this plugin and was refused before its contents were read,
        // so neither of the other two is a true sentence about it.
        check (render3 (s3 (Status::mapIdentityMismatch, { "Default" }))
                   == "map_identity_mismatch[Default]",
               "A9.3 PIN3: the 3494 case reaches map_identity_mismatch",
               "got " + render3 (s3 (Status::mapIdentityMismatch, { "Default" })));

        // PIN 4 — §1c, the tightening the old status could not express. Only
        // the case that ATTEMPTED a write may pair with readback_mismatch. A
        // readback row under the other two would assert evidence that turn
        // never gathered.
        check (render3 (s3 (Status::writesRejected, { "x" }, { "x" })) == "readback_mismatch[x]",
               "A9.3 PIN4: writes_rejected DOES pair with readback_mismatch",
               "got " + render3 (s3 (Status::writesRejected, { "x" }, { "x" })));
        check (render3 (s3 (Status::mapNoCoverage, { "x" }, { "x" })) == "map_no_coverage[x]",
               "A9.3 PIN4: map_no_coverage never pairs with readback_mismatch",
               "got " + render3 (s3 (Status::mapNoCoverage, { "x" }, { "x" })));
        check (render3 (s3 (Status::mapIdentityMismatch, { "x" }, { "x" }))
                   == "map_identity_mismatch[x]",
               "A9.3 PIN4: map_identity_mismatch never pairs with readback_mismatch",
               "got " + render3 (s3 (Status::mapIdentityMismatch, { "x" }, { "x" })));

        // PIN 5 — 3416 emits NO ROW AT ALL. builtinPayloadUnmatched is an enum
        // value with no wire reason.
        //
        // THE FIXTURE IS DELIBERATELY HOSTILE. A built-in slot never fills
        // dialManual (only the mapped apply loop at ChainHost.cpp does), so the
        // REALISTIC fixture has every array empty — and would emit nothing
        // whatever the verdict, making the pin vacuous and unable to go red.
        // Handing it labels it cannot really have is what makes the missing
        // verdict the ONLY thing stopping the row, so the pin has a subject.
        check (echojay::dialMissRowsFor (s3 (Status::builtinPayloadUnmatched,
                                             { "Default", "Attack" })).empty(),
               "A9.3 PIN5: builtinPayloadUnmatched yields ZERO rows from the emitter",
               "got " + render3 (s3 (Status::builtinPayloadUnmatched, { "Default", "Attack" })));
        // ...and the realistic all-empty case too, so the pin covers both.
        check (echojay::dialMissRowsFor (s3 (Status::builtinPayloadUnmatched)).empty(),
               "A9.3 PIN5: a real built-in slot (all arrays empty) yields zero rows");

        // PIN 6 — the name is RETIRED, not reassigned: no path emits the
        // literal, and the enum does not carry the value either. A retired name
        // left in the enum is a store holding a fact we have declared false.
        {
            std::ifstream fh ("Source/ChainHost.h");
            std::stringstream sh_; sh_ << fh.rdbuf();
            const juce::String chHdr (sh_.str());
            std::ifstream fc ("Source/ChainHost.cpp");
            std::stringstream sc_; sc_ << fc.rdbuf();
            const juce::String chSrc3 (sc_.str());
            std::ifstream fr ("Source/EJDialMissRows.h");
            std::stringstream sr_; sr_ << fr.rdbuf();
            const juce::String rowSrc (sr_.str());
            std::ifstream fe ("Source/PluginEditor.cpp");
            std::stringstream se_; se_ << fe.rdbuf();
            const juce::String edSrc3 (se_.str());

            // codeOnly, not raw: the retired name SHOULD still appear in the
            // comments that record why it was retired, and in the historical
            // prose already scattered through ChainHost. What must not survive
            // is a line of code.
            check (! codeOnly (rowSrc).contains ("\"unusable_map\""),
                   "A9.3 PIN6: the emitter emits no unusable_map reason");
            check (! codeOnly (edSrc3).contains ("\"unusable_map\""),
                   "A9.3 PIN6: no walker emits an unusable_map reason");
            check (! codeOnly (chHdr).contains ("unusableMap"),
                   "A9.3 PIN6: DialStatus no longer carries the retired value");
            check (! codeOnly (chSrc3).contains ("DialStatus::unusableMap"),
                   "A9.3 PIN6: no site assigns or reads the retired status");
            // ...and the stripper is not vacuously passing: it must still SEE
            // the four live assignments, and must still find the retired name
            // in the raw text it just filtered out of the code.
            check (chHdr.contains ("unusableMap") && chSrc3.contains ("DialStatus::unusableMap"),
                   "A9.3 PIN6: the retired name survives in PROSE (so the pin above has a subject)");

            // The four assignment sites, each named where the fact is known.
            check (chSrc3.contains ("s.dialStatus = DialStatus::mapNoCoverage;"),
                   "A9.3 PIN7: 3603 assigns mapNoCoverage");
            check (chSrc3.contains ("s.dialStatus = DialStatus::writesRejected;"),
                   "A9.3 PIN7: 3609 assigns writesRejected");
            check (chSrc3.contains ("s.dialStatus = DialStatus::mapIdentityMismatch;"),
                   "A9.3 PIN7: 3494 assigns mapIdentityMismatch");
            check (chSrc3.contains ("s.dialStatus = DialStatus::builtinPayloadUnmatched;"),
                   "A9.3 PIN7: 3416 assigns builtinPayloadUnmatched");
        }
    }

    // ---- 6a: the card and the model stop sharing one string -----------------
    // s.settings answers to the 9 Aug rule (a successful write shows nothing
    // extra on the CARD, because an internal proof class surfaced as
    // user-facing doubt would fire on the whole setread corpus). The MODEL has
    // the opposite requirement: never read a requested value as slot state.
    // One field could not serve both, so the tiering moved to its own field.
    // These pins guard the three things that make the split safe rather than a
    // second place to disagree.
    {
        std::cout << "6a card/model settings split:\n";
        std::ifstream fch ("Source/ChainHost.cpp");
        std::stringstream sch; sch << fch.rdbuf();
        const juce::String ch (sch.str());
        std::ifstream fhh ("Source/ChainHost.h");
        std::stringstream shh; shh << fhh.rdbuf();
        const juce::String chH (shh.str());
        std::ifstream fap ("Source/EchoJayAPI.cpp");
        std::stringstream sap; sap << fap.rdbuf();
        const juce::String api (sap.str());

        // PIN 1 — ONE AUTHOR for the tiering. The Landed/Asked/Refused
        // partition is decided at exactly one site and composed once; both
        // fields receive that SAME composition. Two `kept.joinIntoString`
        // calls, or a second "Asked, not verified: " literal, would mean two
        // places deciding what counts as landed, which is worse than the one
        // string we started with.
        check (codeOnly (ch).contains ("juce::StringArray tierLines;"),
               "6a PIN1: the tier lines are composed exactly once");
        {
            int n = 0, at = 0;
            const auto code = codeOnly (ch);
            while ((at = code.indexOf (at, "Asked, not verified: ")) >= 0) { ++n; at += 1; }
            check (n == 1, "6a PIN1: exactly ONE site names the asked tier",
                   "found " + juce::String (n));
            // NOT a bare "kept.joinIntoString" count: `kept` is also a local
            // in the chain-blacklist writer, and that unrelated second hit is
            // what this pin found first. Count the tiered string's CONSUMERS
            // instead — exactly two assignments, the card and the model, both
            // from the one composition above.
            n = 0; at = 0;
            while ((at = code.indexOf (at, "tierLines")) >= 0) { ++n; at += 1; }
            // one declaration, three adds, one addArray to the card: five.
            // The model no longer takes a joined string here -- it takes the
            // structured tiers and composes at injection time, which is the
            // only moment the live reads exist to suppress against.
            check (n == 5, "6a PIN1: tierLines has exactly its five known uses",
                   "found " + juce::String (n));
        }
        check (codeOnly (ch).contains ("kept.addArray(tierLines);")
               && codeOnly (ch).contains ("s.modelLandedBits  = landedBits;"),
               "6a PIN1: both consumers take the SAME tier lines");
        // THE PROSE IS THE MODEL'S TO NOT HAVE (24 Aug). The card keeps
        // `kept`, which opens with setSlotSettings' description; the model
        // takes the tiers alone. If these ever converge again, description is
        // being handed back as state.
        check (codeOnly (ch).contains ("s.settings = kept.joinIntoString(\"\\n\");")
               && ! codeOnly (ch).contains ("modelLandedBits  = kept"),
               "6a PIN1: the card keeps the prose and the model does not");

        // PIN 2 — THE CARD IS UNTOUCHED. The 9 Aug writer must survive
        // byte-for-byte: same source, same shape, still overwriting.
        check (codeOnly (ch).contains
                   ("s.settings = \"Applied automatically\\n\" + appliedSummary.joinIntoString(\", \");"),
               "6a PIN2: the card writer is unchanged");
        check (codeOnly (ch).contains ("auto line = echojay::formatSemanticSetting(r.semantic, r.requestedValue);"),
               "6a PIN2: the card still echoes the REQUESTED value (9 Aug rule)");

        // PIN 3 — appliedSummary's CARDINALITY is untouched, so
        // dialAppliedCount and the A9 partial/writesRejected verdict cannot
        // move as a side effect of a display change. One add, and the count
        // still reads off .size().
        {
            int n = 0, at = 0;
            const auto code = codeOnly (ch);
            while ((at = code.indexOf (at, "appliedSummary.add")) >= 0) { ++n; at += 1; }
            check (n == 1, "6a PIN3: appliedSummary has exactly one add site",
                   "found " + juce::String (n));
        }
        check (codeOnly (ch).contains ("s.dialAppliedCount = (int) appliedSummary.size();"),
               "6a PIN3: dialAppliedCount still counts appliedSummary");

        // PIN 4 — the model's line reads the model's field, and the Link wire
        // struct did NOT grow a member to carry it.
        // Not a bare name check: a mutation that keeps the PARAMETER and stops
        // USING it would slip past that. Pin the selection itself.
        check (codeOnly (api).contains ("slotModelSettings"),
               "6a PIN4: the formatter takes the model settings array");
        check (codeOnly (api).contains ("? (*slotModelSettings)[i].trim()"),
               "6a PIN4: the model's line actually SELECTS the model field");
        check (codeOnly (api).contains ("modelSettings.add(s.settingsForModel);"),
               "6a PIN4: the local adapter fills it index-parallel");
        check (codeOnly (api).contains ("rack.slots.push_back({ s.name, s.format, s.settings, s.bypassed, s.wet });"),
               "6a PIN4: RackSidecarSlot is still built from the same five fields");
        {
            std::ifstream fsh ("Source/LinkShm.h");
            std::stringstream ssh; ssh << fsh.rdbuf();
            check (! codeOnly (juce::String (ssh.str())).contains ("settingsForModel"),
                   "6a PIN4: the Link wire struct did not grow a field");
        }

        // PIN 5 — a dial echo cannot outlive the map it describes. Every
        // non-tiered writer of `settings` clears the model copy; an absent
        // clear would leave the model reading a tiering for a slot whose card
        // has since moved on.
        {
            int n = 0, at = 0;
            const auto code = codeOnly (ch);
            while ((at = code.indexOf (at, "clearModelTiers(")) >= 0) { ++n; at += 1; }
            // five call sites plus the definition; the declaration is in the
            // header and is not counted here.
            check (n == 6, "6a PIN5: all five non-tiered settings writers clear the model copy",
                   "found " + juce::String (n) + " (want 5 sites + 1 definition)");
        }
        check (codeOnly (chH).contains ("juce::String settingsForModel;"),
               "6a PIN5: SlotInfo carries the field to its readers");


        // PIN 6 — TRUNCATION ANNOUNCES ITSELF. Behavioural, against the
        // SHIPPED helper (header-inline, so this compiles the same bytes the
        // plugin runs rather than the previous build's lib copy). A cap that
        // cuts silently reintroduces the omission the tiering exists to
        // remove, and it cuts at the TAIL, where the unverified group lives.
        {
            const auto under = juce::String::repeatedString ("x", echojay::kModelSettingsClip);
            const auto over  = juce::String::repeatedString ("x", echojay::kModelSettingsClip + 1);
            check (echojay::clipModelSettings (under) == under,
                   "6a PIN6: a string AT the cap is returned untouched");
            check (! echojay::clipModelSettings (under).contains ("[CLIPPED"),
                   "6a PIN6: an unclipped string carries NO marker");
            check (echojay::clipModelSettings (over).contains ("[CLIPPED"),
                   "6a PIN6: a string OVER the cap says it was clipped");
            check (echojay::clipModelSettings (over).startsWith (
                       over.substring (0, echojay::kModelSettingsClip)),
                   "6a PIN6: the kept prefix is exactly the cap, then the marker");
            // The number itself is sized from K_PER_PLUGIN = 12, not from the
            // thirteen observed applies. Pinned so a later reader cannot quietly
            // shrink it back to something that only clears the sample.
            check (echojay::kModelSettingsClip == 500,
                   "6a PIN6: the cap is 500, sized from the 12-control bound");
        }
    }

    // ---- dev_mode body dump: one gate, read per send ------------------------
    // The dump exists to diff the EXACT outgoing body against server logs, and
    // it had never fired in a DAW. Two independent reasons, both silent, and a
    // missing dump file is indistinguishable from a turn that was never sent.
    {
        std::cout << "dev_mode body dump gate:\n";
        std::ifstream fap ("Source/EchoJayAPI.cpp");
        std::stringstream sap; sap << fap.rdbuf();
        const juce::String api (sap.str());

        check (codeOnly (api).contains ("if (ChainHost::devModeActive())"),
               "dump: the gate is the SHARED dev predicate, not a second copy");
        // The sandbox fallback is the whole reason the shared predicate exists;
        // if it ever loses the absolute path, the dump goes dark in-DAW again
        // and this pin is the only thing that would notice.
        {
            std::ifstream fch ("Source/ChainHost.h");
            std::stringstream sch; sch << fch.rdbuf();
            check (codeOnly (juce::String (sch.str())).contains ("/Users/SeanD/.echojay_dev"),
                   "dump: devModeActive still checks the absolute sandbox-proof path");
        }
        // Per send, not once per process: creating dev_mode after the plugin
        // loaded must take effect on the NEXT turn, not the next restart.
        {
            const auto body = functionBody (api, "juce::String EchoJayAPI::buildChatRequestBody");
            check (body.isNotEmpty(), "dump: found buildChatRequestBody");
            check (! body.contains ("static const bool devMode"),
                   "dump: the gate is not cached for the process lifetime");
            // A FAILED WRITE MUST SAY SO. Fixing the gate alone would have
            // moved the silence to the write: userDocumentsDirectory redirects
            // into the same container the flag check did, and the old code
            // logged success without ever looking at replaceWithText's result.
            check (body.contains ("const bool wrote = dirOk && f.replaceWithText(body);"),
                   "dump: the write result is actually checked");
            check (body.contains ("dev_mode body dump FAILED"),
                   "dump: a refused write logs a FAILED line, not a success line");
            check (body.contains ("EJChat: dev_mode body dump -> "),
                   "dump: a successful write still names the resolved path");
        }
    }

    // ---- 6c: the client sends values and does NOT select ---------------------
    {
        std::cout << "6c current values on the wire:\n";

        // PIN 1 — the four states 8d prints from, kept apart by the ENCODING.
        // The empty string is not a sentinel: it is literally what the plugin
        // returned, so it cannot collide with a real display value the way
        // "?" or "-" could.
        {
            auto v = echojay::slotParamReadsVar (1, "Test", 3,
                        [] (int i) { return i == 1 ? juce::String() : juce::String ("v") + juce::String (i); });
            const auto js = juce::JSON::toString (v, true);
            check (js.contains ("\"0\": \"v0\"") && js.contains ("\"2\": \"v2\""),
                   "6c PIN1: a read control carries its display text", "got " + js);
            check (js.contains ("\"1\": \"\""),
                   "6c PIN1: a control read but UNREADABLE is the empty string", "got " + js);
            check (! js.contains ("\"3\":"),
                   "6c PIN1: an absent control has no key at all", "got " + js);
            check (js.contains ("\"truncated\": false") && js.contains ("\"readFailed\": false"),
                   "6c PIN1: both flags ride every entry, false when nothing happened", "got " + js);
            check (js.contains ("\"slot\": 1") && js.contains ("\"name\": \"Test\"")
                   && js.contains ("\"reads\":"),
                   "6c PIN1: 8a's key names exactly -- slot, name, reads", "got " + js);
        }

        // PIN 2 — the backstop flags itself (8e), so the server can tell a CUT
        // from a MISS. 8d cannot print its fourth state without this flag.
        {
            auto v = echojay::slotParamReadsVar (2, "Huge", echojay::kMaxParamReadsPerSlot + 7,
                        [] (int i) { return juce::String (i); });
            const auto js = juce::JSON::toString (v, true);
            check (js.contains ("\"truncated\": true"),
                   "6c PIN2: a cut slot says truncated", "got tail");
            check (js.contains ("\"" + juce::String (echojay::kMaxParamReadsPerSlot - 1) + "\":"),
                   "6c PIN2: everything up to the cap is still carried");
            check (echojay::kMaxParamReadsPerSlot == 1024,
                   "6c PIN2: the cap is 1024, above 126 of the 128 cached maps");
        }

        // PIN 2b — readFailed: the slot is PRESENT and empty, never absent.
        // 8d gives absence its own meaning (stale client, print as today), so
        // a slot that exists and could not be read must not borrow it.
        {
            auto v = echojay::slotParamReadsVar (3, "Dead", 40,
                        [] (int) { return juce::String ("never called"); }, /*readFailed*/ true);
            const auto js = juce::JSON::toString (v, true);
            check (js.contains ("\"readFailed\": true"),
                   "6c PIN2b: a non-responding slot says readFailed", "got " + js);
            check (js.contains ("\"reads\": {}"),
                   "6c PIN2b: readFailed carries an EMPTY reads object", "got " + js);
            check (js.contains ("\"truncated\": false"),
                   "6c PIN2b: readFailed is not also reported as a cut", "got " + js);
            check (js.contains ("\"slot\": 3"),
                   "6c PIN2b: the slot still appears, so the server knows it exists");
            // BEHAVIOURAL, through the shipped decision: a null processor is
            // readFailed. The earlier version of this pin searched ChainHost's
            // source for the readFailed call, and a mutation that dead-coded
            // the branch left the call in place and the pin green. The
            // decision moved into the header so it can be DRIVEN, not read.
            bool failed = false; int total = -1;
            const auto none = echojay::readAllParamDisplays (nullptr, failed, total);
            check (failed && none.isEmpty() && total == 0,
                   "6c PIN2b: a NULL processor reads as readFailed with nothing",
                   "failed=" + juce::String ((int) failed) + " n=" + juce::String (none.size())
                   + " total=" + juce::String (total));
        }

        // PIN 3, structural — the client does NOT select, and the values do not
        // ride a block the model reads. Both are the contract, not a detail:
        // a port of the server's exposure rule was measured to miss the
        // governing switches the server had just added.
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch; sch << fch.rdbuf();
            const auto ch = codeOnly (juce::String (sch.str()));
            check (ch.contains ("echojay::readAllParamDisplays(getSlotProcessor(i),"),
                   "6c PIN3: the sweep goes through the ONE shared read");
            check (ch.contains ("echojay::slotParamReadsVar("),
                   "6c PIN3: ChainHost serialises through the one shared helper");
            // Anchored on code, not on the /*readFailed*/ argument comment:
            // codeOnly() strips comments, so an earlier version of this pin
            // failed on its own stripper rather than on the code.
            // SCOPED to the reads builder. The unscoped version searched the
            // whole file and went red when buildFallbackLookupJson added its
            // own, legitimate, "no instance means no param counts to send"
            // skip. A pin that fails on an unrelated function is measuring
            // the file, not the behaviour.
            {
                const auto rb = functionBody (juce::String (sch.str()),
                                              "juce::String ChainHost::buildSlotParamReadsJson");
                check (rb.isNotEmpty(), "6c PIN3: found the reads builder");
                check (! rb.contains ("if (proc == nullptr) continue;"),
                       "6c PIN3: no slot is dropped for having no instance");
            }
            check (ch.contains ("readFailed -- no hosted instance"),
                   "6c PIN3: and the client LOGS it, so a silent rack is visible");
            check (! ch.contains ("K_PER_PLUGIN") && ! ch.contains ("classifyControl"),
                   "6c PIN3: no copy of the server's exposure rule lives here");

            std::ifstream fap ("Source/EchoJayAPI.cpp");
            std::stringstream sap; sap << fap.rdbuf();
            const auto api = codeOnly (juce::String (sap.str()));
            check (api.contains ("\",\\\"slotParamReads\\\":\" + nextChatParamReads_"),
                   "6c PIN3: the body carries the field, named slotParamReads");
            check (api.contains ("nextChatParamReads_.clear();"),
                   "6c PIN3: consumed per send, like mapFps");
            // The field must NOT be spliced into the injection the model reads.
            const auto inj = functionBody (juce::String (sap.str()),
                "juce::String EchoJayAPI::buildCurrentChainInjection(const LinkShm::RackSidecar& rack,");
            check (inj.isNotEmpty(), "6c PIN3: found the injection builder");
            check (! inj.contains ("slotParamReads"),
                   "6c PIN3: the reads never enter [CURRENT CHAIN]");
        }

        // PIN 4 — TURN D, THE ONE THAT HAS FAILED FOUR TIMES.
        // A control moved by hand, to a value EchoJay never applied, must
        // appear in the field with that value. Driven through the SHIPPED
        // serialiser on a REAL hosted instance; skips cleanly when the plugin
        // is not installed, the tools/bridged_readback_test convention.
        {
            juce::AudioPluginFormatManager fm;
            juce::addDefaultFormatsToManager (fm);
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto* f : fm.getFormats())
                if (f->getName() == "AudioUnit")
                    f->findAllTypesForFile (found, "AudioUnit:Effects/aufx,bxbl,Brwx");
            if (found.isEmpty())
                std::cout << "  SKIP  6c PIN4 turn-D (bx_blackdist2 AU not installed)\n";
            else
            {
                juce::String err;
                auto inst = fm.createPluginInstance (*found[0], 44100.0, 512, err);
                if (inst == nullptr)
                    std::cout << "  SKIP  6c PIN4 turn-D (instance refused: " << err << ")\n";
                else
                {
                    inst->prepareToPlay (44100.0, 512);
                    juce::AudioBuffer<float> buf (juce::jmax (2, inst->getTotalNumInputChannels()), 512);
                    juce::MidiBuffer midi;
                    auto render = [&] { for (int b = 0; b < 8; ++b) { buf.clear(); midi.clear(); inst->processBlock (buf, midi); } };
                    render();
                    auto& ps = inst->getParameters();
                    const int idx = 3;   // "Vol" on this map
                    if (! juce::isPositiveAndBelow (idx, ps.size()))
                        std::cout << "  SKIP  6c PIN4 turn-D (no parameter " << idx << ")\n";
                    else
                    {
                        const auto before = ps[idx]->getCurrentValueAsText().trim();
                        // BY HAND: a bare gesture on the instance, nothing to do
                        // with EchoJay's apply path. This is the user turning a
                        // knob, which is exactly what every failed turn missed.
                        const float target = (ps[idx]->getValue() > 0.5f) ? 0.23f : 0.77f;
                        ps[idx]->beginChangeGesture();
                        ps[idx]->setValueNotifyingHost (target);
                        ps[idx]->endChangeGesture();
                        render();
                        const auto after = ps[idx]->getCurrentValueAsText().trim();

                        // THE SHIPPED SEQUENCE: the same read ChainHost's
                        // sweep makes, then the same serialiser it feeds. An
                        // earlier version passed its OWN reader lambda, so it
                        // exercised a copy and a mutation that stopped the
                        // shipped reader touching the instance left this pin
                        // green. The one pin this contract exists for cannot
                        // be the one testing a reimplementation.
                        bool rf = false; int total = 0;
                        const auto disp = echojay::readAllParamDisplays (inst.get(), rf, total);
                        auto v = echojay::slotParamReadsVar (1, "bx_blackdist2", total,
                                    [&disp] (int i) { return disp[i]; }, rf);
                        const auto js = juce::JSON::toString (v, true);

                        check (after != before,
                               "6c PIN4 turn-D: the hand move actually changed the display",
                               "before \"" + before + "\" after \"" + after + "\"");
                        check (js.contains ("\"" + juce::String (idx) + "\": \"" + after + "\""),
                               "6c PIN4 turn-D: the hand-set value RIDES slotParamReads",
                               "wanted \"" + after + "\" in " + js);
                        // SCOPED TO THE MOVED KEY. An unscoped search for the
                        // old text collides with unrelated controls that happen
                        // to share it -- measured: indices 1 and 2 also read
                        // "5.0" here, so the first version of this pin failed on
                        // its own fixture rather than on the code.
                        check (before.isEmpty()
                               || ! js.contains ("\"" + juce::String (idx) + "\": \"" + before + "\""),
                               "6c PIN4 turn-D: that control no longer reports its pre-move value",
                               "stale \"" + before + "\" still at index " + juce::String (idx) + " in " + js);
                    }
                    inst->releaseResources();
                }
            }
        }
    }

    // ---- 6c suppression: one control, one number --------------------------
    // Measured defect: slotParamReads said index 3 was "8.2" (the user's knob)
    // and [CURRENT CHAIN] said "Asked, not verified: Vol -> 3.7", both in the
    // same body, and the reply quoted 3.7. Labelling the stale one did not
    // save it. Where a live read exists, the echo carries NO value.
    {
        std::cout << "6c suppression (one control, one number):\n";
        std::ifstream fch ("Source/ChainHost.cpp");
        std::stringstream sch; sch << fch.rdbuf();
        const auto ch = codeOnly (juce::String (sch.str()));
        std::ifstream fed ("Source/PluginEditor.cpp");
        std::stringstream sed_; sed_ << fed.rdbuf();
        const auto ed = codeOnly (juce::String (sed_.str()));

        // PIN A — the suppression is CONDITIONAL on an actual read, and the
        // three ways it must NOT fire are each explicit.
        check (ch.contains ("if (paramIndex < 0) return false;"),
               "6cS PIN A: no index means no join, so the echo is KEPT");
        check (ch.contains ("if (s.liveReadFailed) return false;"),
               "6cS PIN A: a readFailed slot KEEPS its echo (only source we have)");
        check (ch.contains ("if (paramIndex >= s.liveReads.size()) return false;"),
               "6cS PIN A: an index the sweep never reached KEEPS its echo");
        check (ch.contains ("return s.liveReads[paramIndex].trim().isNotEmpty();"),
               "6cS PIN A: an EMPTY read is not a live value, so the echo is kept");

        // PIN B — REFUSED entries are never suppressed. "asked 50.00, range is
        // [1.00..7.00], left manual" records a rejected request, not a claim
        // about where the knob sits, and that number exists nowhere else.
        {
            const auto body = functionBody (juce::String (sch.str()),
                                            "juce::String ChainHost::modelSettingsForSlot");
            check (body.isNotEmpty(), "6cS PIN B: found the composer");
            check (body.contains ("keep(s.modelLandedBits, s.modelLandedIdx)")
                   && body.contains ("keep(s.modelAskedBits,  s.modelAskedIdx)"),
                   "6cS PIN B: Landed and Asked are filtered");
            check (! body.contains ("keep(s.modelRefusedBits"),
                   "6cS PIN B: Refused is NEVER filtered");
            check (body.contains ("modelRefusedBits.isEmpty())  lines.add(kRefusedPrefix"),
                   "6cS PIN B: Refused rides untouched, with its number");
        }

        // PIN C — ONE SWEEP, and it is taken before the injection is built.
        // If the injection suppressed on one read and the body shipped a
        // different read, the two-stores defect would move rather than go.
        check (ed.contains ("chainHost.refreshSlotParamReads();"),
               "6cS PIN C: the sweep is taken at the injection site");
        check (ch.contains ("[&s](int p) { return s.liveReads[p]; }"),
               "6cS PIN C: the wire serialises the SAME cache the suppression read");
        {
            int n = 0, at = 0;
            while ((at = ch.indexOf (at, "getCurrentValueAsText")) >= 0) { ++n; at += 1; }
            check (n == 0, "6cS PIN C: ChainHost.cpp takes no read of its own",
                   "found " + juce::String (n));
        }

        // PIN D — THE CARD IS UNTOUCHED. The 6a split exists so this change
        // cannot reach it, and these are the two lines that would have to move
        // for it to have done so.
        check (ch.contains ("s.settings = kept.joinIntoString(\"\\n\");"),
               "6cS PIN D: the card's tiered writer is byte-identical");
        check (ch.contains ("s.settings = \"Applied automatically\\n\" + appliedSummary.joinIntoString(\", \");"),
               "6cS PIN D: the card's applied writer is byte-identical");
        check (ch.contains ("auto line = echojay::formatSemanticSetting(r.semantic, r.requestedValue);"),
               "6cS PIN D: the card still echoes the REQUESTED value (9 Aug rule)");

        // PIN E — ONE AUTHOR. The tiering is still decided in the apply-time
        // block; the composer only drops entries and joins. If it ever grew a
        // decision about what a value IS, that would be a second author.
        check (ch.contains ("s.modelLandedBits  = landedBits;"),
               "6cS PIN E: the partition is authored at apply time, as before");
        {
            const auto body = functionBody (juce::String (sch.str()),
                                            "juce::String ChainHost::modelSettingsForSlot");
            check (! body.contains ("requestedValue") && ! body.contains ("landedText")
                   && ! body.contains ("semanticLabel"),
                   "6cS PIN E: the composer decides nothing about VALUES, only what to drop");
        }
    }

    // ---- 6c fallback: suppression must not be undone by borrowing the card --
    // THE TURN THAT FAILED. slotParamReads said index 3 was "3.3"; the model's
    // line read "Applied automatically\nVol 7". The suppression had WORKED --
    // every tier entry had a live read and was dropped -- and the empty result
    // hit 6a's fallback, which served the card string carrying the very number
    // just removed. Empty meant two different things and the reader could not
    // tell them apart.
    {
        std::cout << "6c fallback (empty tiers + a live read):\n";
        std::ifstream fap ("Source/EchoJayAPI.cpp");
        std::stringstream sap; sap << fap.rdbuf();
        const auto apiRaw = juce::String (sap.str());
        const auto api = codeOnly (apiRaw);
        std::ifstream fch ("Source/ChainHost.cpp");
        std::stringstream sch; sch << fch.rdbuf();
        const auto ch = codeOnly (juce::String (sch.str()));

        // PIN A — the decision is "was this slot read?", never "is the string
        // empty?". Both halves pinned, because the second is what regressed.
        check (api.contains ("const bool slotWasRead = (slotHasLiveReads != nullptr"),
               "6cF PIN A: the reader asks whether the slot was READ");
        check (api.contains (": (slotWasRead ? juce::String() : s.settings.trim());"),
               "6cF PIN A: a slot that WAS read emits nothing rather than the card string");
        check (! api.contains ("(*slotModelSettings)[i].trim().isNotEmpty())\n"
                               "                            ? (*slotModelSettings)[i].trim()\n"
                               "                            : s.settings.trim();"),
               "6cF PIN A: the emptiness-keyed fallback is gone");

        // PIN B — THE readFailed FALLBACK SURVIVES, unchanged. There the card
        // really is the only source, and 8d leaves the model where the
        // deployed 6b sentence still applies.
        check (ch.contains ("return ! s.liveReadFailed && ! s.liveReads.isEmpty();"),
               "6cF PIN B: a readFailed slot reports NO live reads, so it still falls back");
        check (ch.contains ("if (s.liveReadFailed) return false;"),
               "6cF PIN B: and its per-control echo is still kept");

        // PIN C — the flag actually travels. A correct rule that never reaches
        // the formatter is the shape that produced this bug in the first place.
        check (ch.contains ("slotHasLiveReads(i) });")
               && ch.contains ("modelSettingsForSlot(i), slotHasLiveReads(i) };"),
               "6cF PIN C: SlotInfo carries the flag from both readers");
        check (api.contains ("hasLiveReads.add(s.hasLiveReads);")
               && api.contains ("&hasLiveReads);"),
               "6cF PIN C: the adapter fills it and passes it index-parallel");

        // PIN D — THE PROSE. s.settings carries setSlotSettings' description,
        // which 6a deliberately kept out of the model's string; the same
        // fallback had been reintroducing it. Closing the fallback closes both
        // the value and the prose, and only where a reading exists.
        check (ch.contains ("clearModelTiers(slots_[i]);"),
               "6cF PIN D: setSlotSettings still clears the tiers (prose is not tiering)");
        check (api.contains ("slotWasRead ? juce::String()"),
               "6cF PIN D: so a read slot gets neither the card's value NOR its prose");
    }

    // ---- card separator: a label gets a colon, a number does not ------------
    {
        std::cout << "card separator (formatSemanticSetting):\n";
        using echojay::formatSemanticSetting;
        auto F = [] (const char* k, const juce::var& v) { return formatSemanticSetting (k, v); };

        // PIN 1 — EVERY NUMERIC RENDER IS BYTE-IDENTICAL. The card is
        // user-facing and governed by the 9 Aug silence decision; this change
        // is allowed to alter label rendering and nothing else. Each suffix
        // path and the ratio special case, exactly as the header documents.
        check (F ("ratio", "4:1")            == "ratio 4:1",         "sep PIN1: ratio", "got " + F ("ratio", "4:1"));
        check (F ("threshold_db", -18)       == "threshold -18dB",   "sep PIN1: _db",   "got " + F ("threshold_db", -18));
        check (F ("attack_ms", 40)           == "attack 40ms",       "sep PIN1: _ms",   "got " + F ("attack_ms", 40));
        check (F ("freq_hz", 1200)           == "freq 1200Hz",       "sep PIN1: _hz",   "got " + F ("freq_hz", 1200));
        check (F ("mix_pct", 25)             == "mix 25%",           "sep PIN1: _pct",  "got " + F ("mix_pct", 25));
        check (F ("reverb_decay_s", 2)       == "reverb decay 2s",   "sep PIN1: _s",    "got " + F ("reverb_decay_s", 2));
        check (F ("Thresh", -3)              == "Thresh -3",         "sep PIN1: bare numeric", "got " + F ("Thresh", -3));
        check (F ("Vol", 7)                  == "Vol 7",             "sep PIN1: bare int",     "got " + F ("Vol", 7));
        // The one that made the guard necessary: semanticToFloat parses a
        // STRING into a number, so a numeric-as-string can reach an anchored
        // control. It must still render with a space.
        check (F ("Vol", "7")                == "Vol 7",             "sep PIN1: numeric-as-STRING keeps the space", "got " + F ("Vol", "7"));
        check (F ("Vol", "-3.5")             == "Vol -3.5",          "sep PIN1: signed numeric-as-string too",      "got " + F ("Vol", "-3.5"));

        // PIN 2 — the three label shapes, taken from the corpus measurement.
        check (F ("Band 1 Used", "Used")     == "Band 1 Used: Used",
               "sep PIN2: a name ENDING in its label (Pro-Q 3, 442 such pairs)", "got " + F ("Band 1 Used", "Used"));
        check (F ("In", "In")                == "In: In",
               "sep PIN2: a name EQUAL to its label (API-2500, 6 such)",         "got " + F ("In", "In"));
        check (F ("Bias", "Normal")          == "Bias: Normal",
               "sep PIN2: a label UNRELATED to the name",                        "got " + F ("Bias", "Normal"));
        // And the state it must stay distinguishable from.
        check (F ("Band 1 Used", "Unused")   == "Band 1 Used: Unused",
               "sep PIN2: the other state of the same control stays distinct",   "got " + F ("Band 1 Used", "Unused"));

        // PIN 3 — THE TIERED LINE DOES NOT CONVERGE. It renders modes for the
        // model as `reads "Used"`, and that is a different line for a
        // different reader. If these two ever merge, one of them is wrong.
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch; sch << fch.rdbuf();
            const auto ch = codeOnly (juce::String (sch.str()));
            check (ch.contains ("+ arrow + \"reads \\\"\" + r.landedText.trim() + \"\\\"\""),
                   "sep PIN3: the tiered Landed form is untouched");
            check (! ch.contains ("formatSemanticSetting(r.semantic, r.landedText)"),
                   "sep PIN3: the tier does not start using the card's composer");
        }

        // PIN 4 — THE MIRROR HOLDS. semanticLabel says it mirrors
        // formatSemanticSetting's SUFFIX handling; a separator change touches
        // no suffix, and semanticLabel never sees a value at all.
        check (echojay::semanticLabel ("threshold_db") == "threshold"
               && echojay::semanticLabel ("attack_ms")  == "attack"
               && echojay::semanticLabel ("freq_hz")    == "freq"
               && echojay::semanticLabel ("mix_pct")    == "mix"
               && echojay::semanticLabel ("reverb_decay_s") == "reverb decay"
               && echojay::semanticLabel ("ratio")      == "ratio",
               "sep PIN4: semanticLabel's suffix stripping still matches the composer's");
        check (echojay::semanticLabel ("Band 1 Used") == "Band 1 Used",
               "sep PIN4: and it still returns a bare label, never a separator");
    }

    // ---- product fallback: dial by name, never assert the number -----------
    // MAP_CARRY_FORWARD measured 539 two-version products: the control SURFACE
    // held everywhere, the ANCHORS drifted on ~19%. So a prior version's map is
    // worth dialling through and its numbers are not worth asserting.
    {
        std::cout << "product fallback (anchors_unverified):\n";
        std::ifstream fpa ("Source/EchoJayParamApply.h");
        std::stringstream spa; spa << fpa.rdbuf();
        const auto pa = codeOnly (juce::String (spa.str()));
        std::ifstream fch ("Source/ChainHost.cpp");
        std::stringstream sch; sch << fch.rdbuf();
        const auto chRaw = juce::String (sch.str());
        const auto ch = codeOnly (chRaw);

        // PIN 1 — the tag is read ONCE, where the whole map is visible, and
        // threaded like staleDisplayReads rather than looked up again.
        check (pa.contains ("const bool anchorsUnverified = (bool) map.getProperty (\"anchors_unverified\", false);"),
               "fb PIN1: the tag is read once, in applySettings");
        {
            int n = 0, at = 0;
            while ((at = pa.indexOf (at, "anchors_unverified")) >= 0) { ++n; at += 1; }
            check (n == 1, "fb PIN1: exactly ONE site reads the tag",
                   "found " + juce::String (n));
        }
        check (pa.contains ("r.anchorsUnverified = anchorsUnverified;"),
               "fb PIN1: every applyOne result carries it, set before any return");

        // PIN 2 — THE HONESTY FLOOR DOES NOT REACH AN UNVERIFIED DIAL. The
        // 9 Aug rule says a successful write shows nothing extra; silence
        // there means "landed as asked", which on ~19% drift is a claim we
        // cannot make.
        check (ch.contains ("if (r.anchorsUnverified)")
               && ch.contains ("s.dialApproximate.addIfNotAlreadyThere"),
               "fb PIN2: an approximate dial is collected, not silent");
        check (ch.contains ("\": approximate - mapped from \""),
               "fb PIN2: the CARD names it approximate and says where from");
        check (ch.contains ("\" (approximate, mapped from \""),
               "fb PIN2: the MODEL's line is annotated too");
        // The card's 9 Aug writer is untouched for verified maps.
        check (ch.contains ("auto line = echojay::formatSemanticSetting(r.semantic, r.requestedValue);"),
               "fb PIN2: the verified card path is byte-identical");

        // PIN 3 — a verified map is UNCHANGED. The flag defaults false at every
        // hop, so an untagged map cannot reach any approximate branch.
        check (pa.contains ("bool anchorsUnverified = false)"),
               "fb PIN3: the parameter defaults false");
        check (pa.contains ("bool         anchorsUnverified = false;"),
               "fb PIN3: the result field defaults false");
        {
            std::ifstream fh ("Source/ChainHost.h");
            std::stringstream sh; sh << fh.rdbuf();
            check (codeOnly (juce::String (sh.str())).contains ("bool anchorsUnverified = false; };"),
                   "fb PIN3: the report field defaults false");
        }

        // PIN 4 — the fp integrity check stays a keying-bug catcher. Only a
        // TAGGED map is exempt; an untagged mismatch still refuses.
        check (ch.contains ("const bool servedAsFallback = (bool) it->second.getProperty(\"anchors_unverified\", false);"),
               "fb PIN4: the exemption is keyed on the tag");
        check (ch.contains ("if (mapFp != s.fp && ! servedAsFallback)"),
               "fb PIN4: an UNtagged fp mismatch still refuses the apply");

        // PIN 5 — the delivery half asks the right question. lookupTiered
        // refuses every candidate with \"no_count_sent\" unless param_count
        // rides the body, so a body without it would silently never resolve.
        check (ch.contains ("root->setProperty(\"mode\", \"lookup\");"),
               "fb PIN5: the map-returning mode, not exists");
        check (ch.contains ("o->setProperty(\"param_count\", params.size());"),
               "fb PIN5: param_count rides, or every candidate is refused");
        check (ch.contains ("o->setProperty(\"param_names\", names);"),
               "fb PIN5: param_names rides, for the name guard");
        // Both builders now compose through fallbackEntryForSlot, so the
        // mapless-only rule is asserted where it actually lives -- one place,
        // reached by the per-slot trigger and the rack-wide sweep alike.
        {
            const auto body = codeOnly (functionBody (chRaw, "juce::var ChainHost::fallbackEntryForSlot"));
            check (body.isNotEmpty(), "fb PIN5: found the single fallback composer");
            check (body.contains ("paramMaps_.find(s.fp) != paramMaps_.end()"),
                   "fb PIN5: only MAPLESS fps are asked about");
        }
        check (ch.contains ("paramMaps_[wantFp] = map;"),
               "fb PIN5: stored under the fp that ASKED, which the apply looks up by");
    }

    // ---- tagged-path name assertion: the client's own guard ---------------
    // The fp-integrity exemption is only safe while something proves the index
    // still means what the map says. The server's nameGuard does; this layer
    // could not see it, so it now checks for itself.
    {
        std::cout << "fallback name assertion:\n";
        std::ifstream fpa ("Source/EchoJayParamApply.h");
        std::stringstream spa; spa << fpa.rdbuf();
        const auto paRaw = juce::String (spa.str());
        const auto pa = codeOnly (paRaw);
        std::ifstream fch ("Source/ChainHost.cpp");
        std::stringstream sch; sch << fch.rdbuf();
        const auto ch = codeOnly (juce::String (sch.str()));

        // PIN A — reachable ONLY on the tagged path, and the untagged write
        // path gains no name check at all.
        {
            const auto body = functionBody (paRaw, "inline ApplyResult applyOne");
            check (body.isNotEmpty(), "na PIN A: found applyOne");
            check (body.contains ("if (anchorsUnverified)"),
                   "na PIN A: the assertion is behind the tag");
            int n = 0, at = 0;
            const auto cb = codeOnly (body);
            while ((at = cb.indexOf (at, "normNameServerRule")) >= 0) { ++n; at += 1; }
            check (n == 2, "na PIN A: exactly one comparison, on that one path",
                   "found " + juce::String (n) + " normNameServerRule uses");
            check (! cb.contains ("getName") || cb.indexOf ("getName") > cb.indexOf ("if (anchorsUnverified)"),
                   "na PIN A: the live name is read only inside the tagged branch");
        }

        // PIN B — a genuine mismatch refuses and records an honest miss: the
        // same decline shape as a name that never resolved, never a write.
        check (pa.contains ("|| normNameServerRule (live) != normNameServerRule (want))"),
               "na PIN B: mismatch is the refusal condition");
        check (pa.contains ("-- not written, dial by hand\";"),
               "na PIN B: the miss says it was not written and names the remedy");
        check (pa.contains ("if (live.isEmpty()"),
               "na PIN B: FAILS CLOSED -- an unreadable live name is a mismatch");

        // PIN C — the client normalization IS the server's definition, in one
        // place, so the two cannot drift into accidental agreement.
        check (pa.contains ("inline juce::String normNameServerRule (const juce::String& raw)"),
               "na PIN C: the server's rule exists as its own function");
        check (paRaw.contains ("String(n == null ? \"\" : n).trim().toLowerCase().replace(/\\s+/g, \" \")"),
               "na PIN C: the server's source line is quoted beside it");
        check (! pa.contains ("normalizeControlName (live)"),
               "na PIN C: it does NOT use normalizeControlName, which omits the lowercasing");

        // PIN D — identical SOURCING. One constant feeds both the param_names
        // the server verified and the name this re-reads.
        check (pa.contains ("inline constexpr int kParamNameQueryLen = 128;"),
               "na PIN D: one definition of the name length");
        check (pa.contains ("param->getName (kParamNameQueryLen)"),
               "na PIN D: the assertion reads at that length");
        check (ch.contains ("params[p]->getName(echojay::kParamNameQueryLen)"),
               "na PIN D: param_names is composed at the same length");
    }

    // ---- fallback DELIVERY: the leg must fire on a mapless dial -----------
    // The first live test failed with zero EJFallback lines. The leg was bound
    // to the exact-map fetch completion, which fires before the plugin is
    // racked (empty body, silent return) and is then suppressed forever by
    // mapsRequested_. These pins are the ones that would have caught it.
    {
        std::cout << "fallback delivery leg:\n";
        std::ifstream fch ("Source/ChainHost.cpp");
        std::stringstream sch; sch << fch.rdbuf();
        const auto chRaw = juce::String (sch.str());
        const auto ch = codeOnly (chRaw);
        std::ifstream fhh ("Source/ChainHost.h");
        std::stringstream shh; shh << fhh.rdbuf();
        const auto chH = codeOnly (juce::String (shh.str()));
        std::ifstream fed ("Source/PluginEditor.cpp");
        std::stringstream sed_; sed_ << fed.rdbuf();
        const auto ed = codeOnly (juce::String (sed_.str()));

        // PIN 1 — THE TRIGGER IS THE MAPLESS DIAL. The ask is composed inside
        // setSlotStructuredSettings, the function that handles a dial arriving
        // for a racked slot, not inside any fetch completion.
        {
            const auto body = functionBody (chRaw, "void ChainHost::setSlotStructuredSettings");
            check (body.isNotEmpty(), "fd PIN1: found setSlotStructuredSettings");
            const auto cb = codeOnly (body);
            check (cb.contains ("buildFallbackLookupJsonForSlot(i)"),
                   "fd PIN1: the mapless dial composes the fallback ask");
            check (cb.contains ("onNeedFallbackMaps(body)"),
                   "fd PIN1: and issues it");
            check (cb.contains ("paramMaps_.find(fp) == paramMaps_.end()"),
                   "fd PIN1: only when the fp has no exact map");
        }

        // PIN 2 — ITS OWN LEDGER. Keying off mapsRequested_ is the exact defect:
        // the prefetch fills that set before the rack exists.
        check (chH.contains ("juce::StringArray fallbackRequested_;"),
               "fd PIN2: the fallback has its own requested set");
        {
            const auto body = codeOnly (functionBody (chRaw, "void ChainHost::setSlotStructuredSettings"));
            check (body.contains ("! fallbackRequested_.contains(fp)")
                   || body.contains ("!fallbackRequested_.contains(fp)"),
                   "fd PIN2: the fallback is gated on ITS set, not mapsRequested_");
        }
        check (! ed.contains ("buildFallbackLookupJson()"),
               "fd PIN2: the leg no longer hangs off the exact-map fetch completion");

        // PIN 3 — RE-APPLY ON ARRIVAL, mirroring storeParamMaps. Without it the
        // map lands in the cache and the waiting slot never dials.
        {
            const auto body = codeOnly (functionBody (chRaw, "void ChainHost::storeFallbackMaps"));
            check (body.isNotEmpty(), "fd PIN3: found storeFallbackMaps");
            check (body.contains ("applyStructuredIfReady(i, DialTrigger::mapArrived)"),
                   "fd PIN3: an arriving fallback map re-runs the apply");
            check (body.contains ("pendingMapFps_.removeString"),
                   "fd PIN3: and clears the in-flight marker it set");
            check (body.contains ("onSlotSettingsChanged()"),
                   "fd PIN3: and repaints when something actually dialled");
        }

        // PIN 4 — NEVER SILENT. Both the empty-body path and the success path
        // must speak, or "never fired" and "fired empty" look the same again.
        check (ch.contains ("EJFallback: slot ") && ch.contains ("not asked -- "),
               "fd PIN4: a slot that cannot be asked about says why");
        check (ch.contains ("EJFallback: nothing to ask across "),
               "fd PIN4: an empty rack-wide body says why");
        check (ch.contains ("EJFallback: asking for slot "),
               "fd PIN4: the ask itself is logged before it goes");
        {
            std::ifstream fap ("Source/EchoJayAPI.cpp");
            std::stringstream sap; sap << fap.rdbuf();
            check (codeOnly (juce::String (sap.str())).contains ("EJFallback: lookup 200, "),
                   "fd PIN4: a SUCCESSFUL lookup logs too, not only a failure");
        }
    }

    // ---- refusal remedy: the clause only where the toggle governs ---------
    // The server's 558fc1d split one refusal sentence into three cases. The
    // Settings clause is a remedy for exactly two of them; on an ownership
    // refusal it offered a toggle that cannot make the user own a plugin.
    {
        std::cout << "refusal remedy clause:\n";
        const juce::String kOwned  = "it is not in this project's plugin list, so EchoJay cannot add it";
        const juce::String kNoMap  = "auto-dial mode is on and EchoJay has no settings map for this plugin";
        const juce::String kUnres  = "EchoJay has a settings map for this plugin under a different spelling"
                                     " of its name and will not guess which";
        const juce::String kClause = " Turn off \"only suggest plugins EchoJay can auto-dial\" in Settings"
                                     " if you want it anyway.";
        const juce::String kClauseN = " Turn off \"only suggest plugins EchoJay can auto-dial\" in Settings"
                                      " if you want them anyway.";

        // PIN 1 — AN OWNERSHIP REFUSAL RENDERS WITHOUT THE CLAUSE. This is the
        // pin the change exists for.
        {
            const auto line = echojay::refusalLineFor ({ { "Fresh Air", kOwned, "not_owned" } });
            check (line == "Fresh Air was not added: " + kOwned + ".",
                   "refusal PIN1: the ownership line is the server's sentence and stops there");
            check (! line.contains ("Turn off"),
                   "refusal PIN1: and carries NO Settings remedy");
        }

        // PIN 2 — A NO-MAP REFUSAL RENDERS WITH IT. The case the sentence was
        // always true for, and the one the toggle really governs.
        {
            const auto line = echojay::refusalLineFor ({ { "Fresh Air", kNoMap, "no_map" } });
            check (line == "Fresh Air was not added: " + kNoMap + "." + kClause,
                   "refusal PIN2: the no-map line keeps the Settings remedy");
        }

        // PIN 3 — UNRESOLVED IS GOVERNED TOO. A map exists; turning the filter
        // off does put the plugin back on the table.
        check (echojay::refusalLineFor ({ { "Auto-Tune EFX", kUnres, "unresolved" } })
                 .endsWith (kClause),
               "refusal PIN3: a spelling refusal keeps the remedy");

        // PIN 4 — AN OLDER SERVER SENDS NO CASE, and its one sentence was the
        // no_map one. Absent must not be read as unknown, or every refusal from
        // such a server loses a remedy that was correct.
        check (echojay::refusalLineFor ({ { "Fresh Air", kNoMap, "" } }).endsWith (kClause),
               "refusal PIN4: an absent case still gets the remedy");

        // PIN 5 — MIXED TURN. The clause is per TURN, because one sentence
        // covers every name; it rides when at least one refusal is governed,
        // which mirrors the server's some(). Plural wording, and the per-name
        // branch, because the reasons differ.
        {
            const auto line = echojay::refusalLineFor ({ { "Fresh Air", kOwned, "not_owned" },
                                                         { "Pro-Q 3",   kNoMap, "no_map" } });
            check (line == "Fresh Air (" + kOwned + "); Pro-Q 3 (" + kNoMap + ")"
                           " were not added." + kClauseN,
                   "refusal PIN5: mixed turn names each reason and keeps the remedy once");
        }

        // PIN 6 — ALL-OWNERSHIP PLURAL still suppresses. The bug reached the
        // plural wording too, so the plural clause needs its own subject.
        {
            const auto line = echojay::refusalLineFor ({ { "Fresh Air", kOwned, "not_owned" },
                                                         { "Sausage Fattener", kOwned, "not_owned" } });
            check (line == "Fresh Air, Sausage Fattener were not added: " + kOwned + ".",
                   "refusal PIN6: two unowned plugins, one sentence, no remedy");
        }

        // PIN 7 — THE CLIENT NEVER AUTHORS A REASON. The case decides the
        // clause; the sentence is the server's, copied through untouched.
        {
            std::ifstream fr ("Source/EJRefusalLine.h");
            std::stringstream sr; sr << fr.rdbuf();
            const auto rl = codeOnly (juce::String (sr.str()));
            check (rl.isNotEmpty(), "refusal PIN7: found the composer header");
            check (! rl.contains ("no settings map") && ! rl.contains ("plugin list"),
                   "refusal PIN7: the client holds no copy of the server's sentences");
        }

        // PIN 8 — NOTHING TO SAY RENDERS NOTHING, and a nameless op is skipped
        // rather than rendering a bare reason.
        check (echojay::refusalLineFor ({}).isEmpty(),
               "refusal PIN8: no refusals, no bubble");
        check (echojay::refusalLineFor ({ { "", kNoMap, "no_map" } }).isEmpty(),
               "refusal PIN8: a nameless op is not a sentence");

        // PIN 9 — THE EDITOR READS THE CASE OFF THE WIRE AND USES THE COMPOSER.
        // Pins 1-8 call refusalLineFor directly and would all stay green with
        // announceRefusedOps still composing its own line, which is the hole
        // the server's own 558fc1d pin 10 names.
        {
            std::ifstream fe ("Source/PluginEditor.cpp");
            std::stringstream se; se << fe.rdbuf();
            const auto ed = codeOnly (juce::String (se.str()));
            check (ed.contains ("r->getProperty(\"case\").toString().trim()"),
                   "refusal PIN9: announceRefusedOps reads the case off the wire");
            check (ed.contains ("echojay::refusalLineFor(ops)"),
                   "refusal PIN9: and renders through the pinned composer");
            check (! ed.contains ("line += one ? \" Turn off"),
                   "refusal PIN9: the unconditional append is gone");
        }
    }

    // ---- Waves channel-variant preference --------------------------------
    // WaveShell registers one AU per channel config. The feed offers the
    // COLLAPSED base name, and the base key used to be first-wins over an
    // alphabetically sorted entries_, so "(m)" beat "(s)" for 199 of 289 base
    // names -- a mono instance in a 2x2 rack, right channel round the plugin.
    {
        std::cout << "waves channel-variant preference:\n";

        // PIN 1 -- THE RANK ITSELF. Order is the whole decision, so it is
        // asserted as an order and not as four independent numbers.
        check (echojay::channelVariantRank ("Foo")            == 0, "var PIN1: unsuffixed is best");
        check (echojay::channelVariantRank ("Foo (s)")        == 1, "var PIN1: (s) next");
        check (echojay::channelVariantRank ("Foo (m)")        == 2, "var PIN1: (m) after (s)");
        check (echojay::channelVariantRank ("Foo (m->s)")     == 3, "var PIN1: (m->s) after (m)");
        check (echojay::channelVariantRank ("Foo (5->5)")     == 4, "var PIN1: surround last");
        check (echojay::channelVariantRank ("Foo (S)")        == 1, "var PIN1: suffix match is case-insensitive");
        check (echojay::channelVariantIsBetter ("F (s)", "F (m)"),  "var PIN1: (s) beats (m)");
        check (! echojay::channelVariantIsBetter ("F (m)", "F (s)"),"var PIN1: and not the reverse");
        check (! echojay::channelVariantIsBetter ("F (s)", "F (s)"),
               "var PIN1: STRICTLY better only, so equal rank keeps the incumbent");

        // (m->s) RANKS BELOW (m) ON PURPOSE, and this pin names the rule it is
        // testing. (m->s) is 1-in/2-out: measured, it refuses setBusesLayout(2,2)
        // and, because nOut == 2, buildGraph's `for (ch = nOut; ch < 2)`
        // passthrough never runs, so the right INPUT is discarded rather than
        // bypassed. (m) is 1-in/1-out and leaves the right channel dry but
        // intact. Fed a right-only signal, Abbey Road Chambers (m->s) returned
        // silence. So where a base name offers {m, m->s} and no (s) -- IR-L,
        // IR1, the PRS amps, the UltraPitch family, 8 of 289 -- the rule is
        // KEEP THE CHANNEL, and (m) wins.
        check (echojay::channelVariantIsBetter ("IR-L (m)", "IR-L (m->s)"),
               "var PIN4: with no (s), (m) beats (m->s) because (m->s) DISCARDS the right input");

        // The base-name collapse, exactly as ChainHost does it. Mirrors the
        // shipped loop; var PIN5 pins that the shipped loop still has this
        // shape, so a change there reddens this fixture rather than passing
        // against a stale mirror.
        auto collapse = [] (const juce::StringArray& registrations, const juce::String& base)
        {
            juce::String winner;
            for (const auto& r : registrations)
            {
                if (ChainHost::stripParenthetical (r) != base) continue;
                if (winner.isEmpty() || echojay::channelVariantIsBetter (r, winner))
                    winner = r;
            }
            return winner;
        };

        // PIN 2 -- CLA-76, THROUGH REAL REGISTRATIONS off this machine's
        // chain_entries.xml rather than invented strings. CLA-76 has NO
        // unsuffixed registration: only (m) and (s). It resolved to (m).
        {
            std::ifstream f (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                 .getChildFile ("Library/EchoJay/chain_entries.xml")
                                 .getFullPathName().toStdString());
            if (! f.good())
            {
                std::cout << "  SKIP  var PIN2 (no chain_entries.xml on this machine)\n";
            }
            else
            {
                std::stringstream ss; ss << f.rdbuf();
                auto doc = juce::XmlDocument::parse (juce::String (ss.str()));
                juce::StringArray regs;
                if (doc != nullptr)
                    for (auto* c : doc->getChildIterator())
                        regs.add (c->getStringAttribute ("name"));
                check (regs.size() > 0, "var PIN2: read the real registration list");
                check (regs.contains ("CLA-76 (m)") && regs.contains ("CLA-76 (s)"),
                       "var PIN2: CLA-76 really is registered (m) and (s), with no bare name");
                check (! regs.contains ("CLA-76"),
                       "var PIN2: and there is no unsuffixed CLA-76 to shortcut the choice");
                check (collapse (regs, "CLA-76") == "CLA-76 (s)",
                       "var PIN2: CLA-76 collapses to the STEREO registration");

                // PIN 3 -- A MONO-ONLY BASE NAME MUST STILL RESOLVE. 5 of 289
                // are (m) with nothing else; preferring stereo must never make
                // one of those unreachable.
                check (regs.contains ("GTR Tuner (m)") && ! regs.contains ("GTR Tuner (s)"),
                       "var PIN3: GTR Tuner is registered mono-only");
                check (collapse (regs, "GTR Tuner") == "GTR Tuner (m)",
                       "var PIN3: a mono-only base name still resolves, to its mono build");

                // PIN 4b -- the {m, m->s} case against real registrations.
                check (collapse (regs, "UltraPitch Shift") == "UltraPitch Shift (m)",
                       "var PIN4: {m, m->s} with no (s) resolves to (m), on real data");
            }
        }

        // PIN 5 -- THE REAL CALLER. mapfps_test links the PREVIOUS build's
        // SharedCode lib, so calling ChainHost::buildRecommendable here would
        // exercise the code as it was before this change and prove nothing
        // about it. What CAN be asserted without a rebuild is that the shipped
        // call site routes the base key through this header and leaves the
        // exact-name key first-wins -- the same wiring-pin shape the refusal
        // work used, and the pin that reddens if someone reverts the site while
        // leaving the helper green.
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch; sch << fch.rdbuf();
            const auto ch = codeOnly (juce::String (sch.str()));
            check (ch.contains ("if (base != d.name) insertPreferredBase(base, d);"),
                   "var PIN5: the base-name key goes through the preferring insert");
            check (ch.contains ("echojay::channelVariantIsBetter(d.name, itK->second.name)")
                   && ch.contains ("echojay::channelVariantIsBetter(d.name, itS->second.name)"),
                   "var PIN5: both the model key and the stem consult the shipped rank");
            check (ch.contains ("insertName(d.name, d);"),
                   "var PIN5: the EXACT-name key stays first-wins, untouched");
            check (ch.contains ("#include \"EJVariantPreference.h\""),
                   "var PIN5: and it includes the header it is pinned against");
        }
    }

    // ---- Waves marketing-name alias --------------------------------------
    // The scanner injects Waves' MARKETING names; the shell registers shorter
    // ones. 35 of 69 ticked Waves rows resolved to nothing, so installed
    // plugins were invisible to the model.
    {
        std::cout << "waves marketing-name alias:\n";
        // The real registration base names off this machine's scan.
        juce::StringArray bases;
        {
            auto f = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                         .getChildFile ("Library/EchoJay/chain_entries.xml");
            if (auto doc = juce::XmlDocument::parse (f))
                for (auto* c : doc->getChildIterator())
                    if (c->getStringAttribute ("manufacturer") == "Waves")
                        bases.addIfNotAlreadyThere (
                            ChainHost::stripParenthetical (c->getStringAttribute ("name")));
        }
        if (bases.isEmpty())
        {
            std::cout << "  SKIP  waves alias pins (no chain_entries.xml on this machine)\n";
        }
        else
        {
            // PIN 1 -- THE RENAISSANCE FAMILY. Vox and Compressor come from the
            // derived initialism rule, Reverb from the explicit table because
            // "Reverb" contracts to "Verb" and "RReverb" does not exist.
            check (echojay::wavesAliasFor ("Renaissance Vox", bases) == "RVox",
                   "wa PIN1: Renaissance Vox -> RVox");
            check (echojay::wavesAliasFor ("Renaissance Compressor", bases) == "RCompressor",
                   "wa PIN1: Renaissance Compressor -> RCompressor");
            check (echojay::wavesAliasFor ("Renaissance Reverb", bases) == "RVerb",
                   "wa PIN1: Renaissance Reverb -> RVerb");
            // RENAISSANCE EQUALIZER IS DELIBERATELY REFUSED, and this pin
            // records that as the decision it is. The shell registers REQ 2,
            // REQ 4 and REQ 6: three products differing in band count. There is
            // no "REQ". Resolving to any one of them would load a plugin the
            // user did not ask for, so it stays out of the feed until somebody
            // decides which band count "Renaissance Equalizer" means.
            // Renaissance Equalizer was refused as ambiguous until an operator
            // chose the rule (28 Aug 2026): FULLEST VARIANT. REQ 6 of REQ 2/4/6.
            check (echojay::wavesAliasFor ("Renaissance Equalizer", bases) == "REQ 6",
                   "wa PIN1: Renaissance Equalizer -> REQ 6 (fullest of REQ 2/4/6)");

            // PIN 2 -- A ROW WITH NO ALIAS IS UNTOUCHED. The alias must answer
            // nothing for a name it does not know, so the caller's existing
            // resolution is what stands.
            check (echojay::wavesAliasFor ("CLA-76", bases).isEmpty()
                   || echojay::wavesAliasFor ("CLA-76", bases) == "CLA-76",
                   "wa PIN2: a name that already resolves is never re-pointed elsewhere");
            check (echojay::wavesAliasFor ("Not A Real Plugin At All", bases).isEmpty(),
                   "wa PIN2: an unknown name yields no alias");

            // PIN 3 -- FULLEST VARIANT, by operator decision. Each of these
            // marketing names covers several registrations differing only in
            // capacity, and the rule is to offer the most capable. The TARGET
            // SPELLINGS are the shell's, not the marketing ones: API-550B keeps
            // the prefix, Doubler4 has no space, TransX Multi has no hyphen.
            check (echojay::wavesAliasFor ("API 550", bases) == "API-550B",
                   "wa PIN3: API 550 -> API-550B (fullest of A/B)");
            check (echojay::wavesAliasFor ("SuperTap", bases) == "SuperTap 6-Taps",
                   "wa PIN3: SuperTap -> SuperTap 6-Taps (fullest of 2/6)");
            check (echojay::wavesAliasFor ("Doubler", bases) == "Doubler4",
                   "wa PIN3: Doubler -> Doubler4 (fullest of 2/4)");
            check (echojay::wavesAliasFor ("Trans-X", bases) == "TransX Multi",
                   "wa PIN3: Trans-X -> TransX Multi");
            check (echojay::wavesAliasFor ("NLS Non-Linear Summer", bases) == "NLS Channel",
                   "wa PIN3: NLS Non-Linear Summer -> NLS Channel");
            // AND THE ABSENCE IS STILL AN ABSENCE. Abbey Road TG is not
            // installed under any spelling; no table entry can invent it, and
            // this pin stops it being tabled to something that merely sounds close.
            check (echojay::wavesAliasFor ("Abbey Road TG Mastering Chain", bases).isEmpty(),
                   "wa PIN3: Abbey Road TG still refuses -- absent, not ambiguous");

            // PIN 3b -- THE ALNUM-EQUALITY TIE, on a SYNTHETIC registry. Two
            // registrations whose names differ only in punctuation do not occur
            // in this machine's corpus, so the real data cannot exercise this
            // branch and a mutation that made it guess reddened nothing. The
            // fixture supplies the case the corpus lacks.
            {
                const juce::StringArray twoWays { "Foo-Bar", "Foo Bar" };
                check (echojay::wavesAliasFor ("FooBar", twoWays).isEmpty(),
                       "wa PIN3b: two registrations with one alnum key refuse rather than guess");
                const juce::StringArray oneWay { "Foo-Bar" };
                check (echojay::wavesAliasFor ("FooBar", oneWay) == "Foo-Bar",
                       "wa PIN3b: and a single such registration still resolves");
            }

            // PIN 4 -- LONGEST PREFIX, not first or shortest. "Q1" also
            // prefixes "Q10 Equalizer"; picking it would load the wrong EQ.
            check (echojay::wavesAliasFor ("Q10 Equalizer", bases) == "Q10",
                   "wa PIN4: Q10 Equalizer -> Q10, not the Q1 that also prefixes it");
            check (echojay::wavesAliasFor ("H-Comp Hybrid Compressor", bases) == "H-Comp",
                   "wa PIN4: the shell name is a leading run of the marketing name");
            check (echojay::wavesAliasFor ("PuigTec EQP-1A", bases) == "PuigTec EQP1A",
                   "wa PIN4: punctuation-only drift resolves");

            // PIN 5 -- FEED GROWTH IS EXACTLY THE NEWLY RESOLVED SET, no more.
            // Drive every one of the 69 curated names through the alias and
            // count: anything that already resolves must not be aliased, and
            // the aliased set must not collide two names onto one entry.
            {
                juce::StringArray targets; int aliased = 0, collided = 0;
                for (const auto& e : echojay::wavesCatalog())
                {
                    const auto a2 = echojay::wavesAliasFor (juce::String (e.name), bases);
                    if (a2.isEmpty()) continue;
                    ++aliased;
                    if (targets.contains (a2)) ++collided;
                    targets.add (a2);
                }
                check (aliased == 68, "wa PIN5: 68 of the 69 curated names get an answer");
                check (69 - aliased == 1,
                       "wa PIN5: and exactly 1 refuses, Abbey Road TG, which is not installed");
                check (collided == 0, "wa PIN5: no two scanner rows alias onto one entry");

                // THE ADDITIVE PROPERTY, asserted rather than argued. 34 of
                // those 62 are names that ALREADY resolve; the call site never
                // consults the alias for them. Even if it did, every one maps
                // to the product it already resolves to, so the alias cannot
                // re-point a working row. Checked by: whenever a catalog name
                // is alphanumerically equal to a registry base name, the alias
                // must return THAT base and nothing else.
                int selfChecked = 0, selfWrong = 0;
                for (const auto& e : echojay::wavesCatalog())
                {
                    const juce::String nm (e.name);
                    juce::String same;
                    for (const auto& b : bases)
                        if (echojay::wavesAlnumKey (b) == echojay::wavesAlnumKey (nm)) same = b;
                    if (same.isEmpty()) continue;
                    ++selfChecked;
                    if (echojay::wavesAliasFor (nm, bases) != same) ++selfWrong;
                }
                check (selfChecked > 0, "wa PIN5: found catalog names that already match a registration");
                check (selfWrong == 0,
                       "wa PIN5: a name that already matches is never re-pointed by the alias");
            }

            // PIN 6 -- THE (m)/(s) BEHAVIOUR IS UNCHANGED. The alias returns a
            // BASE name and hands it back to lookupName, so the variant choice
            // still belongs to EJVariantPreference.h and nothing here bypasses it.
            check (echojay::channelVariantIsBetter ("RVox (s)", "RVox (m)"),
                   "wa PIN6: the variant rank still decides, alias or not");
            check (! echojay::wavesAliasFor ("Renaissance Vox", bases).contains ("("),
                   "wa PIN6: an alias yields a BASE name, never a variant");
        }

        // PIN 7 -- THE CALL SITE. Additive by construction: consulted only when
        // the existing lookups have already missed, and gated to Waves rows.
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch; sch << fch.rdbuf();
            const auto ch = codeOnly (juce::String (sch.str()));
            check (ch.contains ("if (it == nameMap.end() && sp.manufacturer == \"Waves\")"),
                   "wa PIN7: the alias runs ONLY on a miss, and only for Waves rows");
            check (ch.contains ("it = lookupName(aliased);"),
                   "wa PIN7: its answer goes back through the SAME lookup");
            check (ch.contains ("echojay::wavesAliasFor(sp.name, registryBaseNames)"),
                   "wa PIN7: through the pinned header");
        }
    }

    // ---- The SECOND Waves seam: nameable but unloadable --------------------
    // 66de26d/5abe833 taught buildRecommendable the marketing->shell alias, so
    // 30 Waves rows entered the feed. Nothing taught the CHAIN-EDIT path, which
    // validates with resolveByName against entries_ (shell names) while the feed
    // offers displayName (marketing names). Every one of those 30 was nameable
    // by the model and refused by the validator:
    //     Not applied: op 1 invalid: "API 550" not in the loadable plugin list
    // resolveOfferedName is the bridge. These pins run the REAL ChainHost off
    // the REAL registry, not a model of it.
    {
        std::cout << "offered-name resolution (chain-edit seam):\n";
        const juce::ScopedJuceInitialiser_GUI juceInit;
        ChainHost host;

        if (host.getNumPlugins() == 0)
        {
            std::cout << "  SKIP  offered-name pins (no chain_plugins.xml on this machine)\n";
        }
        else
        {
            // The feed exactly as the scanner builds it for Waves: all 69 curated
            // MARKETING names under manufacturer "Waves" (expandWavesCatalog), fed
            // through the real buildRecommendable so the real alias runs.
            std::vector<ScannedPlugin> enabled;
            for (const auto& e : echojay::wavesCatalog())
            {
                ScannedPlugin sp;
                sp.name         = juce::String (e.name);
                sp.manufacturer = "Waves";
                sp.format       = "VST3/AU";
                sp.category     = juce::String (e.category);
                sp.path         = "WaveShell";
                sp.uid          = "waves_" + sp.name.toLowerCase().replaceCharacter (' ', '_');
                sp.enabled      = true;
                enabled.push_back (sp);
            }
            host.buildRecommendable (enabled, {});
            const auto offered = host.getRecommendableNames();

            // ---- PIN A: the three names from the report, end to end ----------
            // Each is a MARKETING name the feed offers and the validator refused.
            // The assertion is on the base name, so the (m)/(s) preference stays
            // free to pick the variant it always picked.
            auto baseOf = [&host] (const juce::String& n)
            {
                return ChainHost::stripParenthetical (host.resolveOfferedName (n).name);
            };
            if (! offered.contains ("API 550"))
            {
                std::cout << "  SKIP  PIN A (API 550 not in this machine's feed)\n";
            }
            else
            {
                check (baseOf ("API 550") == "API-550B",
                       "of PIN A: \"API 550\" resolves for the validator -> API-550B");
                check (baseOf ("Renaissance Equalizer") == "REQ 6",
                       "of PIN A: \"Renaissance Equalizer\" -> REQ 6");
                check (baseOf ("Renaissance Vox") == "RVox",
                       "of PIN A: \"Renaissance Vox\" -> RVox");
                // And the refusal it produced before the bridge existed. This is
                // the exact call the validator makes; empty is the refusal.
                check (host.resolveByName ("API 550", {}).name.isEmpty(),
                       "of PIN A: the registry alone still cannot see \"API 550\" "
                       "(so the bridge, not a re-scan, is what fixed it)");
            }

            // ---- PIN B: ADDITIVE BY CONSTRUCTION, asserted over the feed -----
            // For every offered name the registry CAN resolve, the bridge must
            // return the identical row -- same uid, same format. This is the
            // requirement that forced resolveByName to run FIRST: the opposite
            // order re-points "TAL-BassLine-101" from the VST3 it resolves to
            // today onto the AU twin the feed offers, and six rows share it.
            int checkedSame = 0, repointed = 0;
            juce::String firstRepoint;
            for (const auto& n : offered)
            {
                const auto direct = host.resolveByName (n, {});
                if (direct.name.isEmpty()) continue;          // bridge territory
                const auto bridged = host.resolveOfferedName (n);
                ++checkedSame;
                if (bridged.uniqueId != direct.uniqueId
                    || bridged.pluginFormatName != direct.pluginFormatName
                    || bridged.name != direct.name)
                {
                    ++repointed;
                    if (firstRepoint.isEmpty())
                        firstRepoint = n + ": " + direct.name + " -> " + bridged.name;
                }
            }
            check (checkedSame > 0, "of PIN B: the additive sweep had rows to check",
                   "checked=" + juce::String (checkedSame));
            check (repointed == 0,
                   "of PIN B: no name that resolves today resolves anywhere else",
                   "re-pointed " + juce::String (repointed) + " of "
                       + juce::String (checkedSame) + "; first: " + firstRepoint);

            // ---- PIN C: a row NOT in the feed resolves exactly as today ------
            // "RVox" is the SHELL name: in the registry, never a displayName, so
            // it never touches recommendable_ and must come back byte-identical.
            {
                const auto direct  = host.resolveByName ("RVox", {});
                const auto bridged = host.resolveOfferedName ("RVox");
                check (! offered.contains ("RVox"),
                       "of PIN C: \"RVox\" is a shell name, not an offered name");
                check (direct.name.isNotEmpty() && bridged.name == direct.name
                           && bridged.uniqueId == direct.uniqueId,
                       "of PIN C: a name outside the feed still resolves as today",
                       direct.name + " vs " + bridged.name);
                // And a name in neither is still a miss, with no invented answer.
                check (host.resolveOfferedName ("Not A Real Plugin At All").name.isEmpty(),
                       "of PIN C: an unknown name is still a miss");
            }
        }

        // ---- PIN D: THE WIRING, which is what actually broke ----------------
        // The alias logic was already right and already pinned; three call sites
        // reaching past it is what shipped the defect. Pin the sites, not just
        // the function they should call.
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch; sch << fch.rdbuf();
            const auto src = juce::String (sch.str());
            const auto ch  = codeOnly (src);

            // The out-param is part of the pinned text on purpose (31 Aug
            // 2026): the sites must not only go through the bridge, they must
            // ASK it for the ambiguity candidates. Dropping &ambiguous would
            // still resolve names correctly and would silently take the
            // candidate list out of the message the user reads.
            check (ch.contains ("resolveOfferedName(op.name, &why, &ambiguous)"),
                   "of PIN D: the chain-edit ops resolve through the bridge, asking for the candidates");
            // add + replace in the dry run, and the runtime apply: three sites,
            // and the dry run agreeing with the apply is the point of the guard.
            int n = 0, at = 0;
            while ((at = ch.indexOf (at, "resolveOfferedName(op.name")) >= 0) { ++n; at += 20; }
            check (n == 3, "of PIN D: all THREE op sites (add, replace, apply) use it",
                   "found " + juce::String (n));
            check (! ch.contains ("resolveByName(op.name"),
                   "of PIN D: and none of them still calls resolveByName directly");

            // THE ORDER IS THE INVARIANT. resolveByName must be consulted before
            // recommendable_ inside the bridge; reversing it is what re-points.
            const auto body = functionBody (src, "juce::PluginDescription ChainHost::resolveOfferedName");
            check (body.isNotEmpty(), "of PIN D: resolveOfferedName found in source");
            const int iDirect = body.indexOf ("resolveByName(rawName");
            const int iFeed   = body.indexOf ("recommendable_");
            check (iDirect >= 0 && iFeed > iDirect,
                   "of PIN D: the registry is consulted BEFORE the feed table",
                   "resolveByName@" + juce::String (iDirect) + " recommendable_@" + juce::String (iFeed));

            // RESTORE IS DELIBERATELY NOT ON THE BRIDGE. Saved chains carry
            // desc.name (buildChainSlotsVar writes the slot's own registration),
            // so a displayName lookup answers a question restore never asks.
            const auto restore = functionBody (src, "void ChainHost::restoreSavedChain");
            check (restore.contains ("resolveByName(name, {}, nullptr, &why)"),
                   "of PIN D: restore still resolves saved SHELL names directly");
            const auto slotsVar = functionBody (src, "juce::var ChainHost::buildChainSlotsVar");
            check (slotsVar.contains ("setProperty(\"plugin\",       s.desc.name)"),
                   "of PIN D: and what it restores is desc.name, which is why");

            // THE FEED PATH IS UNCHANGED: the bridge reads recommendable_ and
            // never writes it, so buildRecommendable owns it exactly as before.
            const auto bodyRec = functionBody (src, "juce::PluginDescription ChainHost::resolveOfferedName");
            check (! bodyRec.contains ("recommendable_.push_back")
                   && ! bodyRec.contains ("recommendable_.clear"),
                   "of PIN D: the bridge only READS the feed table");
        }
    }

    // ---- Waves candidates from the REGISTRY, not the catalog ---------------
    // Step 2 of the feed source swap. The catalog was the MEMBERSHIP TEST for
    // Waves: buildRecommendable walks scanner rows, so a Waves product the 69
    // curated names did not name could not enter the feed however plainly it
    // was installed. EJWavesRegistryFeed.h moves membership to the machine's
    // own AU scan and leaves the catalog as a ranking. Everything that decides
    // is header-inline, so these pins run the SHIPPED bytes against the REAL
    // registry; the call site is pinned structurally, because the lib this test
    // links was built before the call site existed.
    {
        std::cout << "waves from the registry (feed source swap):\n";
        const juce::ScopedJuceInitialiser_GUI juceInit;
        ChainHost host;

        // `loadable` as buildRecommendable sees it: the withheld rows are
        // already gone (getFilteredPlugins calls the same withholdReasonLocked
        // at the same lock), the format filter is empty, and a fresh process
        // has no session load failures. collapseTwins=false so nothing is
        // hidden before the predicate gets to judge it.
        const auto loadable = host.getFilteredPlugins ({}, {}, /*collapseTwins*/ false);

        if (loadable.isEmpty())
        {
            std::cout << "  SKIP  waves-registry pins (no chain_entries.xml on this machine)\n";
        }
        else
        {
            // ---- wr PIN1: THE SCOPE, and that it is not a name match --------
            int byCode = 0, byVendor = 0, codeButNotVendor = 0, vendorButNotCode = 0;
            for (const auto& d : loadable)
            {
                const bool code   = echojay::isWaveShellRegistration (d);
                const bool vendor = (d.manufacturerName == "Waves");
                byCode   += code   ? 1 : 0;
                byVendor += vendor ? 1 : 0;
                if (code && ! vendor) ++codeButNotVendor;
                if (vendor && ! code) ++vendorButNotCode;
            }
            check (byCode > 0, "wr PIN1: the AU manufacturer code selects rows at all",
                   "selected " + juce::String (byCode));
            check (codeButNotVendor == 0 && vendorButNotCode == 0,
                   "wr PIN1: 'ksWV' and manufacturer==\"Waves\" select the SAME set",
                   "code=" + juce::String (byCode) + " vendor=" + juce::String (byVendor)
                       + " code-only=" + juce::String (codeButNotVendor)
                       + " vendor-only=" + juce::String (vendorButNotCode));
            {
                // The case the corpus cannot supply on demand: a vendor whose
                // NAME contains "Waves" and whose rows are not WaveShell's.
                // Synthetic, so it runs on every machine, and real if present.
                juce::PluginDescription wf;
                wf.name = "Cassette"; wf.manufacturerName = "Wavesfactory";
                wf.pluginFormatName = "VST3";
                wf.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/Cassette.vst3";
                check (! echojay::isWaveShellRegistration (wf),
                       "wr PIN1: a 'Wavesfactory' row is NOT a WaveShell registration");
                juce::PluginDescription au;   // and the shape it does accept
                au.name = "CLA-76 (s)"; au.manufacturerName = "Waves";
                au.pluginFormatName = "AudioUnit";
                au.fileOrIdentifier = "AudioUnit:Effects/aufx,76CS,ksWV";
                check (echojay::isWaveShellRegistration (au),
                       "wr PIN1: and an AU identifier ending ksWV IS one");
                juce::PluginDescription other;
                other.name = "Thing"; other.manufacturerName = "Someone";
                other.pluginFormatName = "AudioUnit";
                other.fileOrIdentifier = "AudioUnit:Effects/aufx,THNG,SMNE";
                check (! echojay::isWaveShellRegistration (other),
                       "wr PIN1: another vendor's AudioUnit is not claimed");
            }

            // ---- The rows the shipped header offers, off the real registry --
            const auto fresh = echojay::wavesRegistryFeedRows (loadable, {});

            // ---- wr PIN2: THE COLLAPSE --------------------------------------
            {
                // CASE-INSENSITIVE, mirroring wavesProductKey, and derived
                // through JUCE's own compare rather than through the helper so
                // the count stays independent of the function under test.
                // Case-sensitive this counted 289 where the header now counts
                // 288: "Nx Ambisonics" and "NX Ambisonics" are one product.
                juce::StringArray distinctBases;
                for (const auto& d : loadable)
                    if (echojay::isWaveShellRegistration (d))
                        distinctBases.addIfNotAlreadyThere (ChainHost::stripParenthetical (d.name),
                                                            /*ignoreCase*/ true);
                check (fresh.products == distinctBases.size(),
                       "wr PIN2: one product per distinct base name",
                       juce::String (fresh.products) + " vs " + juce::String (distinctBases.size()));
                check (fresh.products < byCode,
                       "wr PIN2: and that is FEWER than the registrations",
                       juce::String (fresh.products) + " products from "
                           + juce::String (byCode) + " registrations");
                int suffixed = 0;
                for (const auto& r : fresh.rows)
                    if (echojay::channelVariantSuffix (r.name).isNotEmpty()) ++suffixed;
                check (suffixed == 0,
                       "wr PIN2: no offered name carries a channel suffix",
                       juce::String (suffixed) + " suffixed of "
                           + juce::String ((int) fresh.rows.size()));
                // Against a fresh feed every product is offered, so the two
                // numbers reconcile and neither refusal is firing by accident.
                check ((int) fresh.rows.size() + fresh.alreadyOffered
                           + fresh.nameTakenByOther + fresh.untickedInSettings
                           == fresh.products,
                       "wr PIN2: rows + refusals accounts for every product");
            }

            // ---- wr PIN3: THE VARIANT, so 9c4f629 is not undone -------------
            {
                auto chosenFor = [&fresh] (const juce::String& base) -> juce::String
                {
                    for (const auto& r : fresh.rows) if (r.name == base) return r.desc.name;
                    return {};
                };
                juce::StringArray regs;
                for (const auto& d : loadable)
                    if (echojay::isWaveShellRegistration (d)) regs.add (d.name);

                if (! regs.contains ("CLA-76 (s)"))
                {
                    std::cout << "  SKIP  wr PIN3 (CLA-76 not registered on this machine)\n";
                }
                else
                {
                    check (chosenFor ("CLA-76") == "CLA-76 (s)",
                           "wr PIN3: a product with (m) and (s) is offered as the STEREO build",
                           chosenFor ("CLA-76"));
                    check (chosenFor ("GTR Tuner") == "GTR Tuner (m)",
                           "wr PIN3: a mono-only product still reaches the feed, as (m)",
                           chosenFor ("GTR Tuner"));
                    check (chosenFor ("UltraPitch Shift") == "UltraPitch Shift (m)",
                           "wr PIN3: {m, m->s} resolves to (m), which keeps the right channel",
                           chosenFor ("UltraPitch Shift"));
                }
                // AND THE AGGREGATE, which is what a witness cannot say: no
                // offered row loses to another registration of its own product.
                int beaten = 0; juce::String firstBeaten;
                for (const auto& r : fresh.rows)
                    for (const auto& d : loadable)
                        if (echojay::isWaveShellRegistration (d)
                            && ChainHost::stripParenthetical (d.name).equalsIgnoreCase (r.name)
                            && echojay::channelVariantIsBetter (d.name, r.desc.name))
                        {
                            ++beaten;
                            if (firstBeaten.isEmpty())
                                firstBeaten = r.desc.name + " beaten by " + d.name;
                        }
                check (beaten == 0,
                       "wr PIN3: every offered row is the best variant of its product",
                       juce::String (beaten) + " beaten; first: " + firstBeaten);
            }

            // ---- wr PIN4: an UNCATALOGUED product reaches the feed ----------
            {
                auto inCatalog = [] (const juce::String& base)
                {
                    for (const auto& e : echojay::wavesCatalog())
                        if (echojay::wavesAlnumKey (juce::String (e.name))
                                == echojay::wavesAlnumKey (base))
                            return true;
                    return false;
                };
                int uncatalogued = 0; juce::String witness;
                for (const auto& r : fresh.rows)
                    if (! inCatalog (r.name))
                    {
                        ++uncatalogued;
                        if (witness.isEmpty()) witness = r.name;
                    }
                check (uncatalogued > 0,
                       "wr PIN4: products the catalog never named now reach the model",
                       juce::String (uncatalogued) + " of "
                           + juce::String ((int) fresh.rows.size()) + "; first: " + witness);
                // The named witness, so a count cannot drift into vacuity.
                bool hasPuigchild = false;
                for (const auto& r : fresh.rows)
                    if (r.name.startsWith ("Puigchild")) hasPuigchild = true;
                bool registryHasPuigchild = false;
                for (const auto& d : loadable)
                    if (echojay::isWaveShellRegistration (d) && d.name.startsWith ("Puigchild"))
                        registryHasPuigchild = true;
                check (hasPuigchild == registryHasPuigchild,
                       "wr PIN4: Puigchild 670 is installed, uncatalogued, and offered",
                       registryHasPuigchild ? "registered" : "not on this machine");
            }

            // ---- wr PIN5: nothing but Waves comes in through this door ------
            {
                int notWaves = 0;
                for (const auto& r : fresh.rows)
                    if (! echojay::isWaveShellRegistration (r.desc)) ++notWaves;
                check (notWaves == 0,
                       "wr PIN5: every appended row IS a WaveShell registration",
                       juce::String (notWaves) + " were not");
                // The rows are the only thing the call site appends, so a
                // non-Waves plugin cannot enter the feed through this path even
                // if its name collides with a Waves product's.
                std::set<juce::String> names, ids;
                int dupName = 0, dupId = 0;
                for (const auto& r : fresh.rows)
                {
                    if (! names.insert (r.name).second) ++dupName;
                    if (! ids.insert (r.desc.createIdentifierString()).second) ++dupId;
                }
                check (dupName == 0 && dupId == 0,
                       "wr PIN5: one row per name and one row per registration",
                       "dupName=" + juce::String (dupName) + " dupId=" + juce::String (dupId));
            }

            // ---- wr PIN6: COLD START still offers the 69 --------------------
            {
                const auto none = echojay::wavesRegistryFeedRows ({}, {});
                check (none.rows.empty() && none.products == 0,
                       "wr PIN6: with no registry the header adds nothing");
                check (echojay::wavesCatalog().size() == 69,
                       "wr PIN6: the curated catalog is still 69 names",
                       juce::String ((int) echojay::wavesCatalog().size()));
                std::ifstream fsc ("Source/PluginScanner.cpp");
                std::stringstream ssc; ssc << fsc.rdbuf();
                const auto sc = codeOnly (juce::String (ssc.str()));
                check (sc.contains ("for (const auto& e : echojay::wavesCatalog())"),
                       "wr PIN6: and the scanner still injects it (Settings, profile, prompt)");
                std::ifstream fed ("Source/PluginEditor.cpp");
                std::stringstream sed_; sed_ << fed.rdbuf();
                const auto ed = codeOnly (juce::String (sed_.str()));
                check (ed.contains ("EchoJayAPI::buildPluginInjection(\n"
                                    "            processorRef.getPluginScanner().getFullPluginList())")
                       || ed.contains ("buildPluginInjection("),
                       "wr PIN6: the no-feed fallback still injects the scanner's full list");
            }

            // ---- wr PIN7: THE WIRING, which is what actually ships ----------
            {
                std::ifstream fch ("Source/ChainHost.cpp");
                std::stringstream sch; sch << fch.rdbuf();
                const auto src = juce::String (sch.str());
                const auto ch  = codeOnly (src);
                check (ch.contains ("#include \"EJWavesRegistryFeed.h\""),
                       "wr PIN7: buildRecommendable includes the header it is pinned against");
                const auto body = functionBody (src, "void ChainHost::buildRecommendable");
                check (body.contains ("echojay::wavesRegistryFeedRows(loadable, resolved, untickedRegistrations)"),
                       "wr PIN7: it calls the header with the gated entries, the feed so far, AND the unticked set");
                // The set is not decorative: it must be FILLED from the disabled
                // branch, or the argument is an empty set that always agrees.
                check (body.contains ("untickedRegistrations.push_back"),
                       "wr PIN7: and the unticked list is filled from the disabled branch");
                check (body.contains ("resolved.push_back({ r.name, r.desc });"),
                       "wr PIN7: and APPENDS the rows");
                // No decision at the site: the header owns scope, variant and
                // both refusals, so a mutation there reddens the pins above.
                const auto after = body.fromFirstOccurrenceOf ("wavesRegistryFeedRows", false, false);
                check (! after.contains ("isWaveShellRegistration")
                       && ! after.contains ("channelVariantIsBetter")
                       && ! after.contains ("stripParenthetical"),
                       "wr PIN7: no second copy of the decision at the call site");
                // ORDER: after the scanner walk (so the catalog's rows keep
                // their names and positions) and before the cache is filled.
                const int iLoop   = body.indexOf ("resolved.push_back({ sp.name, it->second });");
                const int iWaves  = body.indexOf ("wavesRegistryFeedRows");
                const int iCache  = body.indexOf ("recommendable_          = std::move(resolved);");
                check (iLoop >= 0 && iWaves > iLoop && iCache > iWaves,
                       "wr PIN7: appended AFTER the scanner rows and BEFORE the cache is filled",
                       "loop@" + juce::String (iLoop) + " waves@" + juce::String (iWaves)
                           + " cache@" + juce::String (iCache));
                check (body.contains ("+ \", feed=\" + juce::String((int) resolved.size())"),
                       "wr PIN7: the coverage line reports the feed total it now differs from");
            }

            // ---- wr PIN8: THE REAL CALLER, on this machine's real inputs ----
            // buildRecommendable comes from the linked lib, so this is the feed
            // AS IT SHIPS TODAY. What it can prove without a rebuild: the rows
            // the header offers are names today's feed does NOT carry, and the
            // composition adds no duplicate name and no duplicate registration.
            {
                PluginScanner sc;
                sc.loadCache();
                auto rows = sc.getPlugins();
                std::set<juce::String> disabled;
                {
                    auto f = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                 .getChildFile ("Application Support/EchoJay/plugin_disabled.json");
                    if (auto arr = juce::JSON::parse (f.loadFileAsString()).getArray())
                        for (auto& v : *arr) disabled.insert (v.toString());
                }
                PluginScanner::stampEnabled (rows, disabled);

                if (rows.empty())
                {
                    std::cout << "  SKIP  wr PIN8 (no plugin_cache.json on this machine)\n";
                }
                else
                {
                    host.buildRecommendable (rows, {});
                    const auto before = host.getRecommendableNames();
                    // The REAL feed table, names and registrations both, which
                    // is exactly what the shipped call site passes. So these
                    // numbers are the composition as it will ship, off a
                    // `before` the linked lib produced from this machine's own
                    // scanner cache and registry.
                    const auto add = echojay::wavesRegistryFeedRows (
                        loadable, host.recommendableEntries());

                    int alreadyInFeed = 0;
                    for (const auto& r : add.rows)
                        if (before.contains (r.name)) ++alreadyInFeed;
                    check (alreadyInFeed == 0,
                           "wr PIN8: every offered product is a name today's feed does NOT carry",
                           juce::String (alreadyInFeed) + " were already there");
                    // AND NOT UNDER ANOTHER NAME EITHER, which is the refusal a
                    // name comparison cannot see: 64 of these products are in
                    // today's feed under a catalog MARKETING name whose spelling
                    // shares nothing with the shell's ("SSL E-Channel" is
                    // "SSLChannel (s)"). Offering both is one plugin twice.
                    int wavesBefore = 0, reoffered = 0, feedProducts = 0;
                    juce::String firstReoffered;
                    {
                        std::set<juce::String> feedRegs;
                        for (const auto& e : host.recommendableEntries())
                        {
                            if (! echojay::isWaveShellRegistration (e.desc)) continue;
                            ++wavesBefore;
                            // KEYED ON THE PRODUCT, not the registration: a
                            // catalog row can hold a different channel build of
                            // the same plugin, and one did.
                            feedRegs.insert (ChainHost::stripParenthetical (e.desc.name)
                                                 .toLowerCase());
                        }
                        for (const auto& r : add.rows)
                            if (feedRegs.count (ChainHost::stripParenthetical (r.desc.name)
                                                    .toLowerCase()) > 0)
                            {
                                ++reoffered;
                                if (firstReoffered.isEmpty())
                                    firstReoffered = r.name + " = " + r.desc.name;
                            }
                        feedProducts = (int) feedRegs.size();
                        // A gap between the two says today's feed already
                        // carries one product under two names, which the
                        // product-keyed refusal above depends on seeing.
                        if (feedProducts != wavesBefore)
                        {
                            std::set<juce::String> seen;
                            for (const auto& e : host.recommendableEntries())
                                if (echojay::isWaveShellRegistration (e.desc)
                                    && ! seen.insert (ChainHost::stripParenthetical (e.desc.name)
                                                          .toLowerCase()).second)
                                    std::cout << "  MEASURED  today's feed carries \""
                                              << e.displayName << "\" -> " << e.desc.name
                                              << " twice over\n";
                        }
                    }
                    check (reoffered == 0,
                           "wr PIN8: nor a PRODUCT today's feed already carries under another name",
                           juce::String (reoffered) + " re-offered; first: " + firstReoffered);
                    // NON-EMPTINESS IS A FIXTURE QUESTION, NOT A MACHINE ONE
                    // (re-shaped 29 Aug 2026). This used to assert
                    // `! add.rows.empty()` against the live plugin set, which
                    // made a pin about the FUNCTION depend on how many Waves
                    // products happened to be missing from this machine's feed.
                    // It went red the moment the corpus improved and the
                    // re-offer set emptied - a green-to-red on good news, and it
                    // would break again on any scan. The machine-dependent
                    // checks above still run on the real feed, because "adds no
                    // duplicate" is only meaningful against real data; what
                    // moves to a fixture is the one assertion that needs a
                    // product to be MISSING in order to hold.
                    std::cout << "  MEASURED  live re-offer set: " << (int) add.rows.size()
                              << " row(s) of " << add.products << " product(s)\n";
                    {
                        // One WaveShell registration the feed does not carry:
                        // the function must offer it. isWaveShellRegistration
                        // keys on the AU identifier's trailing manufacturer code.
                        juce::PluginDescription w;
                        w.name             = "Fixture Widget (s)";
                        w.pluginFormatName = "AudioUnit";
                        w.fileOrIdentifier = juce::String ("AudioUnit:Effects/aufx,FxWd,")
                                           + echojay::wavesAuManufacturerCode();
                        juce::Array<juce::PluginDescription> fixtureLoadable;
                        fixtureLoadable.add (w);
                        const auto none = echojay::wavesRegistryFeedRows (fixtureLoadable, {});
                        check (none.rows.size() == 1 && none.rows[0].name == "Fixture Widget",
                               "wr PIN8: a WaveShell product absent from the feed IS offered, collapsed to its base name",
                               juce::String ((int) none.rows.size()) + " row(s)");
                        // And the same product, already in the feed, is not re-offered.
                        std::vector<ChainHost::RecommendableEntry> already;
                        already.push_back ({ "Fixture Widget", w });
                        const auto dup = echojay::wavesRegistryFeedRows (fixtureLoadable, already);
                        check (dup.rows.empty(),
                               "wr PIN8: and the same product already in the feed is NOT re-offered",
                               juce::String ((int) dup.rows.size()) + " row(s)");

                        // ---- wr PIN9: THE SETTINGS TICK REACHES THIS PATH ----
                        // The gap this closes: `loadable` is filtered for
                        // format, session failure, crash blacklist and arch, and
                        // for nothing the user chose. An unticked Waves plugin
                        // was therefore not removed from the feed but RENAMED --
                        // the disabled scanner row left `resolved`, so its
                        // product left offeredProducts, so this function added
                        // it straight back under the shell base name.
                        {
                            std::vector<juce::PluginDescription> unticked;
                            unticked.push_back (w);          // the registration, "(s)" and all
                            const auto off = echojay::wavesRegistryFeedRows (
                                fixtureLoadable, {}, unticked);
                            check (off.rows.empty() && off.untickedInSettings == 1,
                                   "wr PIN9: an UNTICKED product is not offered by the registry path",
                                   juce::String ((int) off.rows.size()) + " row(s), unticked="
                                       + juce::String (off.untickedInSettings));
                            // RE-TICKING RESTORES IT. Same fixture, same call,
                            // empty set: the refusal must be a function of the
                            // set and of nothing sticky.
                            const auto back = echojay::wavesRegistryFeedRows (
                                fixtureLoadable, {}, {});
                            check (back.rows.size() == 1
                                       && back.rows[0].name == "Fixture Widget"
                                       && back.untickedInSettings == 0,
                                   "wr PIN9: and re-ticking restores it",
                                   juce::String ((int) back.rows.size()) + " row(s)");
                            // A DIFFERENT product being unticked changes nothing:
                            // the refusal keys on the product, not on "something
                            // was unticked".
                            juce::PluginDescription otherW;
                            otherW.name             = "Some Other Product (s)";
                            otherW.pluginFormatName = "AudioUnit";
                            otherW.fileOrIdentifier = juce::String ("AudioUnit:Effects/aufx,OthP,")
                                                    + echojay::wavesAuManufacturerCode();
                            const auto un = echojay::wavesRegistryFeedRows (
                                fixtureLoadable, {}, { otherW });
                            check (un.rows.size() == 1 && un.untickedInSettings == 0,
                                   "wr PIN9: a TICKED product still appears when a different one is unticked",
                                   juce::String ((int) un.rows.size()) + " row(s)");
                            // SCOPE IS THE HEADER'S, so a NON-WaveShell
                            // registration in the unticked list is ignored, not
                            // trusted. The caller hands over every disabled row
                            // it resolves, of any vendor -- if this leaked, one
                            // unticked Melda plugin whose base name collided
                            // would silently suppress a Waves product.
                            juce::PluginDescription notWaves;
                            notWaves.name             = "Fixture Widget (s)";
                            notWaves.pluginFormatName = "AudioUnit";
                            notWaves.fileOrIdentifier = "AudioUnit:Effects/aufx,FxWd,Nope";
                            const auto ign = echojay::wavesRegistryFeedRows (
                                fixtureLoadable, {}, { notWaves });
                            check (ign.rows.size() == 1 && ign.untickedInSettings == 0,
                                   "wr PIN9: a non-WaveShell registration in the unticked list is IGNORED",
                                   juce::String ((int) ign.rows.size()) + " row(s)");
                            // An already-offered product is refused as
                            // alreadyOffered, NOT counted as unticked: it is
                            // still being shown, by the enabled row that offers
                            // it, so calling it a user refusal would be a lie.
                            const auto both = echojay::wavesRegistryFeedRows (
                                fixtureLoadable, already, unticked);
                            check (both.rows.empty() && both.alreadyOffered == 1
                                       && both.untickedInSettings == 0,
                                   "wr PIN9: a product still offered by an enabled row counts as alreadyOffered, not unticked",
                                   juce::String ("alreadyOffered=") + juce::String (both.alreadyOffered)
                                       + " unticked=" + juce::String (both.untickedInSettings));
                        }
                    }

                    // ---- wr PIN10: THE UNTICK, END TO END, ON REAL DATA ----
                    // The fixture pins prove the header refuses. This proves the
                    // WHOLE PATH refuses: a real scanner row, unticked, rebuilt
                    // through the shipped buildRecommendable, absent from the
                    // feed under BOTH of its names -- the catalog marketing name
                    // the scanner walk offers it by, and the shell base name the
                    // registry path would re-add it under. Those two names are
                    // the entire bug: before this, unticking swapped one for the
                    // other and called it removed.
                    {
                        // A ticked row that resolves to a WaveShell registration.
                        const ScannedPlugin* victim = nullptr;
                        juce::String product;
                        for (const auto& e : host.recommendableEntries())
                        {
                            if (! echojay::isWaveShellRegistration (e.desc)) continue;
                            for (const auto& r : rows)
                                if (r.enabled && r.name == e.displayName) { victim = &r; break; }
                            if (victim != nullptr)
                            { product = ChainHost::stripParenthetical (e.desc.name); break; }
                        }
                        if (victim == nullptr)
                        {
                            std::cout << "  SKIP  wr PIN10 (no ticked Waves row in this machine's feed)\n";
                        }
                        else
                        {
                            const auto victimName = victim->name;
                            auto flipped = rows;
                            for (auto& r : flipped) if (r.name == victimName) r.enabled = false;

                            host.buildRecommendable (flipped, {});
                            const auto names = host.getRecommendableNames();
                            check (! names.contains (victimName),
                                   "wr PIN10: an unticked plugin is gone from the feed by the SCANNER name",
                                   victimName);
                            check (! names.contains (product),
                                   "wr PIN10: and gone by the REGISTRY base name too (the rename this fixes)",
                                   product + " still offered");
                            // The registration must not survive under any third
                            // spelling either: nothing in the feed may still
                            // point at this product.
                            int stillPointing = 0;
                            for (const auto& e : host.recommendableEntries())
                                if (echojay::isWaveShellRegistration (e.desc)
                                    && ChainHost::stripParenthetical (e.desc.name) == product)
                                    ++stillPointing;
                            check (stillPointing == 0,
                                   "wr PIN10: and no feed row points at the unticked product at all",
                                   juce::String (stillPointing) + " row(s) still point at " + product);

                            // RE-TICKING RESTORES IT, and restores the whole feed:
                            // a suppression that leaks would show up as a feed
                            // that never comes back to its original size.
                            host.buildRecommendable (rows, {});
                            const auto restored = host.getRecommendableNames();
                            check (restored.contains (victimName),
                                   "wr PIN10: re-ticking restores it", victimName);
                            check (restored.size() == before.size(),
                                   "wr PIN10: and restores the feed exactly, no residue",
                                   juce::String (restored.size()) + " vs " + juce::String (before.size()));
                        }
                    }

                    juce::StringArray after = before;
                    for (const auto& r : add.rows) after.add (r.name);
                    juce::StringArray uniq;
                    for (const auto& n : after) uniq.addIfNotAlreadyThere (n);
                    check (uniq.size() == after.size(),
                           "wr PIN8: the composed feed carries no duplicate name",
                           juce::String (after.size() - uniq.size()) + " duplicates");
                    // before is a PREFIX of after: nothing is reordered, nothing
                    // is dropped, which is the whole non-Waves argument.
                    bool prefix = (after.size() >= before.size());
                    for (int i = 0; prefix && i < before.size(); ++i)
                        prefix = (after[i] == before[i]);
                    check (prefix,
                           "wr PIN8: today's feed survives byte-for-byte as the head of the new one");

                    const auto blockBefore = EchoJayAPI::buildChainInjection (before);
                    const auto blockAfter  = EchoJayAPI::buildChainInjection (after);
                    // WHERE THE 69 ACTUALLY LAND, which "N of 69 resolve"
                    // never said: a curated name can be in the feed and
                    // pointing at somebody else's plugin.
                    {
                        int inFeed = 0, toWaves = 0, toOther = 0, absent = 0;
                        juce::String firstOther;
                        for (const auto& e : echojay::wavesCatalog())
                        {
                            const juce::String n (e.name);
                            const ChainHost::RecommendableEntry* hit = nullptr;
                            for (const auto& r : host.recommendableEntries())
                                if (r.displayName == n) { hit = &r; break; }
                            if (hit == nullptr)
                            {
                                ++absent;
                                std::cout << "  MEASURED  curated name absent from the feed: \""
                                          << n << "\"\n";
                                continue;
                            }
                            ++inFeed;
                            if (echojay::isWaveShellRegistration (hit->desc)) ++toWaves;
                            else
                            {
                                ++toOther;
                                if (firstOther.isEmpty())
                                    firstOther = n + " -> " + hit->desc.name + " ["
                                               + hit->desc.manufacturerName + "]";
                            }
                        }
                        check (toOther == 0,
                               "wr PIN9: no curated Waves name points at another vendor's plugin",
                               juce::String (toOther) + " do; first: " + firstOther);
                        std::cout << "  MEASURED  of the 69 curated names, " << inFeed
                                  << " are in the feed (" << toWaves << " on WaveShell rows, "
                                  << toOther << " elsewhere), " << absent << " absent\n";
                    }
                    std::cout << "  MEASURED  Waves rows in the feed before: " << wavesBefore
                              << " (catalog marketing names), covering "
                              << (int) feedProducts << " products; after: "
                              << (feedProducts + (int) add.rows.size()) << " of "
                              << add.products << " registered\n"
                              << "  MEASURED  feed " << before.size() << " -> " << after.size()
                              << " names (+" << (after.size() - before.size()) << "), "
                              << add.products << " Waves products registered, "
                              << add.rows.size() << " added, "
                              << add.alreadyOffered << " already offered, "
                              << add.nameTakenByOther << " name held elsewhere\n"
                              << "  MEASURED  [AVAILABLE PLUGINS] block "
                              << blockBefore.getNumBytesAsUTF8() << " -> "
                              << blockAfter.getNumBytesAsUTF8() << " bytes (+"
                              << (blockAfter.getNumBytesAsUTF8() - blockBefore.getNumBytesAsUTF8())
                              << ")\n";
                }
            }
        }
    }

    // ---- The match ladder prefers the stereo registration ------------------
    // 9c4f629 taught the FEED to collapse a base name onto the stereo build and
    // left the LADDER taking whichever row sorted first. Measured live: a build
    // turn loaded CLA-76 (s), an add op naming the same plugin loaded CLA-76
    // (m). The ladder is what add/replace, bare-name recall and every Link path
    // resolve through, so this is where the variant has to be decided.
    //
    // The ladder is header-inline now, so these pins run the SHIPPED code and
    // compare it name-by-name against the lib's copy of the same rule.
    //
    // RE-POINTED 31 Aug 2026, for the reason cb19bfe re-pointed nl PIN1. This
    // block was written while the linked archive predated 28d3f53, so
    // ChainHost::resolveByName WAS the pre-change ladder and the table below
    // was a genuine before/after. The archive has been rebuilt since; the lib
    // now carries the same ladder the header does, so those columns are no
    // longer before and after. They are LIB and HEADER: two compilations of
    // one rule, and the table asserts they AGREE. A before/after reading of it
    // would be reading a difference that no longer exists.
    {
        std::cout << "name ladder (channel variant in the collapsing rungs):\n";
        const juce::ScopedJuceInitialiser_GUI juceInit;
        ChainHost host;

        // The pools resolveByName builds, reproduced through the ONE public
        // accessor that applies the same withhold gate and the same
        // AU-preferring collapse. Built-ins are dropped because resolveByName
        // answers them before the pool is ever searched.
        auto poolOf = [&host] (const juce::String& formatFilter)
        {
            auto rows = host.getFilteredPlugins ({}, formatFilter,
                                                 /*collapseTwins*/ formatFilter.isEmpty());
            juce::Array<juce::PluginDescription> out;
            for (const auto& d : rows)
                if (d.pluginFormatName != juce::String (ChainHost::kBuiltinFormat))
                    out.add (d);
            return out;
        };
        const auto poolEmpty = poolOf ({});
        const auto poolAU    = poolOf ("AudioUnit");

        // The feed table, built by the LINKED lib from this machine's real
        // scanner cache: path A reads it directly and never touches the ladder,
        // so it has to be the real one for the table below to mean anything.
        {
            PluginScanner sc;
            sc.loadCache();
            auto rows = sc.getPlugins();
            std::set<juce::String> disabled;
            {
                auto f = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("Application Support/EchoJay/plugin_disabled.json");
                if (auto arr = juce::JSON::parse (f.loadFileAsString()).getArray())
                    for (auto& v : *arr) disabled.insert (v.toString());
            }
            PluginScanner::stampEnabled (rows, disabled);
            if (! rows.empty()) host.buildRecommendable (rows, {});
        }

        if (poolEmpty.isEmpty())
        {
            std::cout << "  SKIP  name-ladder pins (no chain_entries.xml on this machine)\n";
        }
        else
        {
            // resolveByName's preamble, which this commit does not touch, plus
            // the shipped ladder. nl PIN7 pins that the shipped function still
            // derives raw/base/manu this way and calls exactly this.
            auto ladder = [] (const juce::Array<juce::PluginDescription>& pool,
                              const juce::String& name)
            {
                const auto raw  = name.trim();
                const auto base = ChainHost::stripParenthetical (raw);
                juce::String manu;
                if (raw.endsWithChar (')') && raw.contains (" ("))
                    manu = raw.fromLastOccurrenceOf (" (", false, false)
                              .dropLastCharacters (1).trim();
                juce::String how;
                const int i = echojay::matchInPool (pool, raw, base, manu, how);
                return i >= 0 ? pool.getReference (i) : juce::PluginDescription();
            };

            // ---- nl PIN1: the reported case, RE-POINTED 29 Aug 2026 ------
            // This pin was a WITNESS. It asserted that the shipped resolver
            // returned the MONO build while the ladder returned STEREO, and it
            // carried its own instruction: "if this ever goes green-to-red the
            // defect was fixed elsewhere". That is exactly what happened.
            // resolveOfferedName("CLA-76") now returns "CLA-76 (s)", the same
            // answer the ladder gives, so the divergence this pin existed to
            // witness is gone. Re-pointed rather than deleted: the coverage now
            // asserts the CORRECT behaviour (both paths resolve, and agree on
            // stereo), so a regression to the mono build reddens it again.
            //
            // WHAT FIXED IT: 28d3f53, "Prefer the stereo registration in the
            // resolver, not only in the feed" (29 Aug 07:27), which replaced this
            // ladder's match rungs with EJNameLadder.h. The witness pin and the
            // fix arrived in the SAME commit, so it was green only while the
            // gate still linked a lib built before it -- `now` came from the
            // stale archive and `then` from the newly compiled header. Rebuild
            // the lib and the two converge, which is the whole point of the
            // change and is what reddened the pin. Standing hazard, not a
            // surprise: mapfps_test links the PREVIOUS build's SharedCode.
            //
            // RULED OUT on the way, and recorded so nobody re-runs it: the 29 Aug
            // identity merge (chain_fp_scan.json 90 -> 853, client index
            // 238 -> 899). resolveOfferedName cannot see it - its body references
            // neither identityToFp_ nor paramMaps_ (measured: zero). It was the
            // obvious candidate and it was the wrong one.
            {
                const auto now  = host.resolveOfferedName ("CLA-76");   // shipped, from the lib
                const auto then = ladder (poolEmpty, "CLA-76");         // the ladder, compiled here
                std::cout << "  MEASURED  add op \"CLA-76\": " << now.name
                          << " -> " << then.name << "\n";
                check (then.name == "CLA-76 (s)",
                       "nl PIN1: an add op naming \"CLA-76\" resolves to the STEREO registration",
                       then.name);
                check (now.name == "CLA-76 (s)",
                       "nl PIN1: and the shipped resolver returns STEREO too (was MONO before 29 Aug)",
                       now.name);
                check (now.name == then.name,
                       "nl PIN1: shipped resolver and ladder no longer diverge on this name",
                       now.name + " vs " + then.name);
            }

            // ---- nl PIN2: the EXACT rung is untouched --------------------
            // Not a witness: every registration in the corpus, asked for by its
            // own full name, must come back byte-identical to what the shipped
            // resolver returns today.
            {
                check (ladder (poolEmpty, "CLA-76 (m)").name == "CLA-76 (m)",
                       "nl PIN2: \"CLA-76 (m)\" asked for by exact name still resolves to MONO",
                       ladder (poolEmpty, "CLA-76 (m)").name);
                int checkedExact = 0, movedExact = 0; juce::String firstMoved;
                for (const auto& d : poolEmpty)
                {
                    const auto now  = host.resolveByName (d.name, {});
                    const auto then = ladder (poolEmpty, d.name);
                    ++checkedExact;
                    if (now.name != then.name || now.uniqueId != then.uniqueId
                        || now.pluginFormatName != then.pluginFormatName)
                    {
                        ++movedExact;
                        if (firstMoved.isEmpty())
                            firstMoved = d.name + ": " + now.name + " -> " + then.name;
                    }
                }
                check (movedExact == 0 && checkedExact > 0,
                       "nl PIN2: every registration asked for by its OWN name resolves unchanged",
                       juce::String (movedExact) + " of " + juce::String (checkedExact)
                           + " moved; first: " + firstMoved);
            }

            // ---- nl PIN3: a mono-only product still resolves --------------
            {
                check (ladder (poolEmpty, "GTR Tuner").name == "GTR Tuner (m)",
                       "nl PIN3: a mono-only product still resolves, to its mono build",
                       ladder (poolEmpty, "GTR Tuner").name);
                check (ladder (poolEmpty, "UltraPitch Shift").name == "UltraPitch Shift (m)",
                       "nl PIN3: {m, m->s} still resolves to (m), which keeps the right channel",
                       ladder (poolEmpty, "UltraPitch Shift").name);
                // The rule RANKS, it never filters: nothing that resolved
                // before may resolve to nothing now.
                //
                // NARROWED 31 Aug 2026, and the narrowing is the point rather
                // than an exception to it. A tie between DIFFERENT PRODUCTS is
                // now refused instead of picked, so "never turns a resolution
                // into a miss" is no longer true as written -- it is true of
                // RANKING, which is what this pin was always about, and false
                // of the ambiguity refusal that sits beside it. A name lost to
                // ambiguity is counted separately and must come with its
                // candidates; a name lost to anything else is still zero.
                int lost = 0, lostAmbiguous = 0; juce::String firstLost;
                for (const auto& d : poolEmpty)
                {
                    const auto b = ChainHost::stripParenthetical (d.name);
                    if (host.resolveByName (b, {}).name.isNotEmpty()
                        && ladder (poolEmpty, b).name.isEmpty())
                    {
                        juce::String how; juce::StringArray amb;
                        echojay::matchInPool (poolEmpty, b, ChainHost::stripParenthetical (b),
                                              {}, how, &amb);
                        if (amb.size() > 1) { ++lostAmbiguous; continue; }
                        ++lost; if (firstLost.isEmpty()) firstLost = b;
                    }
                }
                check (lost == 0,
                       "nl PIN3: ranking never turns a resolution into a miss",
                       juce::String (lost) + " lost; first: " + firstLost);
                std::cout << "  MEASURED  names newly refused as ambiguous on this machine: "
                          << lostAmbiguous << "\n";
            }

            // ---- nl PIN11: THE FEED'S DIRECTION-A REFUSAL -----------------
            // buildRecommendable's lookupName refuses a stem hit where the
            // REQUEST carries a bare trailing number and the ENTRY carries
            // none. The keying lives in lambdas inside a member function and
            // cannot be called directly (the limitation the 20 Aug model-number
            // pin records), so this REBUILDS the two-tier map the way
            // buildRecommendable builds it -- insertName on the full name,
            // insertPreferredBase on the stripped base -- and asks it real
            // lookups. Pairwise "does this look like direction A" is NOT the
            // same question and gets a different answer: it cannot see that
            // tier 1 already matched some other entry, and it keys the entry
            // by its full name where the map keys it by its base.
            {
                auto tnum = [] (const juce::String& n)
                { return echojay::trailingModelNumber (n); };
                auto stem = [] (const juce::String& n)
                { return echojay::normalizeName (n); };
                auto modelKeyOf = [&] (const juce::String& n)
                {
                    const auto num = tnum (n);
                    return num.isNotEmpty() ? stem (n) + "\n" + num : stem (n);
                };
                struct Tiered { juce::String name; int tier = 0; };   // 0 = miss
                auto buildMap = [&] (const juce::Array<juce::PluginDescription>& entries)
                {
                    std::map<juce::String, juce::PluginDescription> m;
                    auto insertName = [&] (const juce::String& n, const juce::PluginDescription& d)
                    {
                        const auto k = modelKeyOf (n);
                        if (m.find (k) == m.end()) m[k] = d;
                        const auto st = stem (n);
                        if (st != k && m.find (st) == m.end()) m[st] = d;
                    };
                    auto insertBase = [&] (const juce::String& b, const juce::PluginDescription& d)
                    {
                        const auto k = modelKeyOf (b);
                        auto itK = m.find (k);
                        if (itK == m.end() || echojay::channelVariantIsBetter (d.name, itK->second.name))
                            m[k] = d;
                        const auto st = stem (b);
                        if (st != k)
                        {
                            auto itS = m.find (st);
                            if (itS == m.end() || echojay::channelVariantIsBetter (d.name, itS->second.name))
                                m[st] = d;
                        }
                    };
                    for (const auto& d : entries)
                    {
                        insertName (d.name, d);
                        const auto b = ChainHost::stripParenthetical (d.name);
                        if (b != d.name) insertBase (b, d);
                    }
                    return m;
                };
                // lookupName, both tiers, INCLUDING the direction-A refusal.
                auto lookup = [&] (const std::map<juce::String, juce::PluginDescription>& m,
                                   const juce::String& n) -> Tiered
                {
                    auto it = m.find (modelKeyOf (n));
                    if (it != m.end()) return { it->second.name, 1 };
                    it = m.find (stem (n));
                    if (it != m.end())
                    {
                        const auto numIn = tnum (ChainHost::stripParenthetical (n));
                        const auto numEn = tnum (ChainHost::stripParenthetical (it->second.name));
                        if (numIn.isNotEmpty() && numEn.isNotEmpty() && numIn != numEn) return {};
                        if (numIn.isNotEmpty() && numEn.isEmpty())                      return {};
                        return { it->second.name, 2 };
                    }
                    return {};
                };
                auto row = [] (const char* name, const char* sub)
                {
                    juce::PluginDescription d;
                    d.name = name; d.pluginFormatName = "AudioUnit";
                    d.fileOrIdentifier = juce::String ("AudioUnit:Effects/aufx,") + sub + ",Fxtr";
                    return d;
                };
                auto poolOfNames = [&] (std::initializer_list<std::pair<const char*, const char*>> l)
                {
                    juce::Array<juce::PluginDescription> a;
                    for (auto& kv : l) a.add (row (kv.first, kv.second));
                    return a;
                };

                // TIER 2 IS ONE SHAPE, and the guard's safety rests on it: with
                // no bare trailing number the model key and the bare stem are
                // the same string, so tier 2 is unreachable.
                check (modelKeyOf ("bx_digital V2") == stem ("bx_digital V2"),
                       "nl PIN11: a v-prefixed version is NOT a model number, so its key IS the stem",
                       modelKeyOf ("bx_digital V2"));
                check (modelKeyOf ("DeEsser 2") != stem ("DeEsser 2"),
                       "nl PIN11: a BARE trailing number splits the model key from the stem",
                       modelKeyOf ("DeEsser 2").replace ("\n", "|"));

                // DIRECTION A: request carries a number the entry lacks. The
                // entry is a CHANNEL VARIANT, so it reaches the map under its
                // stripped base -- which is exactly how the live case arrives.
                {
                    const auto m = buildMap (poolOfNames ({{"Fixture Dess (m)","FdsM"},
                                                           {"Fixture Dess (s)","FdsS"}}));
                    // TIER 1, not 2: "Fixture Dess" carries no bare digit, so
                    // its model key IS its stem -- the same property the guard
                    // rests on, seen from the other side. What matters here is
                    // that it BINDS, and to the stereo variant.
                    check (lookup (m, "Fixture Dess").tier == 1
                           && lookup (m, "Fixture Dess").name == "Fixture Dess (s)",
                           "nl PIN11: the un-numbered base still binds, to the stereo variant",
                           lookup (m, "Fixture Dess").name + " tier "
                               + juce::String (lookup (m, "Fixture Dess").tier));
                    check (lookup (m, "Fixture Dess 2").tier == 0,
                           "nl PIN11: request carries a number the entry lacks -> REFUSED (direction A)",
                           lookup (m, "Fixture Dess 2").name);
                }
                // DIRECTION B: the entry carries the number, the request does
                // not. Still binds -- this is the half that earns the tolerance.
                {
                    const auto m = buildMap (poolOfNames ({{"Fixture Q 3","FQ3a"}}));
                    check (lookup (m, "Fixture Q").tier != 0
                           && lookup (m, "Fixture Q").name == "Fixture Q 3",
                           "nl PIN11: entry carries a number the request lacks -> still binds (direction B)",
                           lookup (m, "Fixture Q").name);
                }
                // TWO DIFFERENT numbers stay the c3ad9be guard's business.
                {
                    const auto m = buildMap (poolOfNames ({{"Fixture EQ 200","FE20"}}));
                    check (lookup (m, "Fixture EQ 250").tier == 0,
                           "nl PIN11: two DIFFERENT numbers stay refused by the c3ad9be guard",
                           lookup (m, "Fixture EQ 250").name);
                }
                // THE ARM THAT PINS THE bx_ PAIR OUT OF THE GUARD'S REACH.
                // bx_digital V2 -> V3 and bx_XL -> bx_XL V2 are CORRECT
                // bindings carried by the version-token strip, not the digit
                // strip, and their vendors disagree (Brainworx against Plugin
                // Alliance, developer against distributor) so a vendor-keyed
                // guard would have refused them. They resolve on TIER 1, where
                // this guard never runs.
                {
                    const auto m = buildMap (poolOfNames ({{"bx_digital V3","bxd3"},
                                                           {"bx_XL V2","bxx2"}}));
                    check (lookup (m, "bx_digital V2").tier == 1
                           && lookup (m, "bx_digital V2").name == "bx_digital V3",
                           "nl PIN11: bx_digital V2 binds V3 on TIER 1, out of the guard's reach",
                           lookup (m, "bx_digital V2").name);
                    check (lookup (m, "bx_XL").tier == 1
                           && lookup (m, "bx_XL").name == "bx_XL V2",
                           "nl PIN11: and bx_XL binds bx_XL V2 the same way",
                           lookup (m, "bx_XL").name);
                }
                // A NUMBERED REQUEST WITH ITS OWN ENTRY hits tier 1 and is
                // never offered to the guard -- the UAD "... 180" shape, which
                // a pairwise reading of this rule gets wrong.
                {
                    const auto m = buildMap (poolOfNames ({{"Fixture Mic Collection","FMC0"},
                                                           {"Fixture Mic Collection 180","FM18"}}));
                    check (lookup (m, "Fixture Mic Collection 180").tier == 1
                           && lookup (m, "Fixture Mic Collection 180").name == "Fixture Mic Collection 180",
                           "nl PIN11: a numbered request WITH its own entry resolves on tier 1, unrefused",
                           lookup (m, "Fixture Mic Collection 180").name);
                }

                // THE STRUCTURAL ARM: the shipped guard exists, refuses, and
                // says so out loud. A silent refusal is indistinguishable from
                // one that never fired.
                {
                    std::ifstream fch ("Source/ChainHost.cpp");
                    std::stringstream sch; sch << fch.rdbuf();
                    const auto body = functionBody (juce::String (sch.str()),
                                                    "void ChainHost::buildRecommendable");
                    check (body.contains ("if (numIn.isNotEmpty() && numEn.isEmpty())"),
                           "nl PIN11: the shipped feed path carries the direction-A refusal");
                    check (body.contains ("EJScan: [name-guard] REFUSED"),
                           "nl PIN11: and LOGS every refusal, name and candidate both");
                }

                // THE LIVE ARM: a real two-tier lookup for every scanner row
                // against this machine's real registry, counting what the
                // guard actually removes.
                {
                    PluginScanner sc; sc.loadCache();
                    auto rows = sc.getPlugins();
                    if (rows.empty() || poolEmpty.isEmpty())
                        std::cout << "  SKIP  nl PIN11 live arm (no scanner cache on this machine)\n";
                    else
                    {
                        const auto live = buildMap (poolEmpty);
                        juce::StringArray refused;
                        for (const auto& sp : rows)
                        {
                            const auto n = sp.name.trim();
                            if (n.isEmpty()) continue;
                            if (lookup (live, n).tier != 0) continue;      // bound, nothing removed
                            auto it = live.find (stem (n));
                            if (it == live.end()) continue;                // a plain miss, not a refusal
                            const auto numIn = tnum (ChainHost::stripParenthetical (n));
                            const auto numEn = tnum (ChainHost::stripParenthetical (it->second.name));
                            if (numIn.isNotEmpty() && numEn.isEmpty())
                                refused.addIfNotAlreadyThere (n + " -> " + it->second.name);
                        }
                        std::cout << "  MEASURED  direction-A bindings the guard removes: "
                                  << refused.size() << " (" << refused.joinIntoString ("; ") << ")\n";
                        check (refused.size() == 1,
                               "nl PIN11: the guard removes exactly ONE binding on this machine",
                               juce::String (refused.size()) + ": " + refused.joinIntoString ("; "));
                        check (! refused.isEmpty() && refused[0].startsWith ("DeEsser 2 -> DeEsser"),
                               "nl PIN11: and it is the cross-vendor DeEsser binding, named",
                               refused.joinIntoString ("; "));
                    }
                }
            }

            // ---- nl PIN10: AN AMBIGUOUS TIE CANNOT RESOLVE TO A PICK ------
            // A FIXTURE, and deliberately so: this machine's registry has no
            // different-product tie left to reach (the 16 ties its corpus can
            // reach are all one product's channel builds, and the one case
            // difference was folded by 6cea1f7). A pin that needs an instance
            // the corpus does not have is a pin that measures the machine, not
            // the rule -- the call wr PIN8 already makes. The live corpus is
            // still measured, in nl PIN3 above, and must read zero.
            {
                auto row = [] (const char* name, const char* sub)
                {
                    juce::PluginDescription d;
                    d.name = name; d.pluginFormatName = "AudioUnit";
                    d.fileOrIdentifier = juce::String ("AudioUnit:Effects/aufx,") + sub + ",Fxtr";
                    return d;
                };
                auto ask = [] (const juce::Array<juce::PluginDescription>& pool,
                               const juce::String& name, juce::StringArray& amb)
                {
                    juce::String how;
                    const int i = echojay::matchInPool (pool, name.trim(),
                                                        ChainHost::stripParenthetical (name.trim()),
                                                        {}, how, &amb);
                    return i;
                };

                // FOUR different products collapsing to one normalised stem,
                // the "UAD Neve 1073 / 1081 / 1084 / 31102" shape: normalizeName
                // strips the trailing number as a version, and a request that
                // carries no number cannot be split by the model-number guard.
                juce::Array<juce::PluginDescription> four;
                four.add (row ("Fixture Neve 1073", "FN73"));
                four.add (row ("Fixture Neve 1081", "FN81"));
                four.add (row ("Fixture Neve 1084", "FN84"));
                four.add (row ("Fixture Neve 31102", "FN02"));
                {
                    juce::StringArray amb;
                    const int i = ask (four, "Fixture Neve", amb);
                    check (i < 0,
                           "nl PIN10: a tie between DIFFERENT products refuses, it does not pick",
                           i < 0 ? juce::String ("refused")
                                 : "picked \"" + four.getReference (i).name + "\"");
                    // THE CAP THAT MUST NOT BE INHERITED. resolveByName's fuzzy
                    // "closest:" list stops at 3 and would drop one of these
                    // four; this is a different computation and names them all.
                    check (amb.size() == 4,
                           "nl PIN10: and it names EVERY candidate, with no cap at three",
                           juce::String (amb.size()) + ": " + amb.joinIntoString (", "));
                    check (amb.contains ("Fixture Neve 31102"),
                           "nl PIN10: including the fourth, which a cap of 3 would have dropped");
                    const auto text = ChainHost::ambiguousNameText (amb);
                    check (text.contains ("Fixture Neve 1073") && text.contains ("Fixture Neve 31102")
                           && text.contains ("4 installed plugins"),
                           "nl PIN10: and the user-facing clause carries all four", text);
                }
                // Asked for by its OWN name, the exact rung answers as it always did.
                {
                    juce::StringArray amb;
                    const int i = ask (four, "Fixture Neve 1081", amb);
                    check (i >= 0 && four.getReference (i).name == "Fixture Neve 1081",
                           "nl PIN10: and an EXACT name is untouched by the refusal",
                           i >= 0 ? four.getReference (i).name : juce::String ("refused"));
                }
                // ONE product, several channel builds: still resolves, to the
                // stereo one. This is the 14-tie population the refusal must
                // not touch.
                {
                    juce::Array<juce::PluginDescription> variants;
                    variants.add (row ("Fixture Widget (m)", "FWdM"));
                    variants.add (row ("Fixture Widget (s)", "FWdS"));
                    juce::StringArray amb;
                    const int i = ask (variants, "Fixture Widget", amb);
                    check (i >= 0 && variants.getReference (i).name == "Fixture Widget (s)"
                           && amb.isEmpty(),
                           "nl PIN10: a tie between CHANNEL BUILDS of one product still resolves, to (s)",
                           i >= 0 ? variants.getReference (i).name : juce::String ("refused"));
                }
                // A case difference is NOT a collision -- 6cea1f7's evidence:
                // aufx,NX4B and aufx,NX4D are one product Waves spelled two
                // ways. Both rank 4, so this is a genuine tie, and it must
                // still pick rather than refuse.
                {
                    juce::Array<juce::PluginDescription> cased;
                    cased.add (row ("Fixture Nx Thing (4->2)", "FNxB"));
                    cased.add (row ("Fixture NX Thing (4->4)", "FNxD"));
                    juce::StringArray amb;
                    const int i = ask (cased, "Fixture Nx Thing", amb);
                    check (i >= 0 && amb.isEmpty(),
                           "nl PIN10: a tie that is only a CASE difference is one product, and resolves",
                           i >= 0 ? cased.getReference (i).name : juce::String ("REFUSED"));
                }
                // The wiring: the three edit-op sites must ASK for the
                // candidates, or the refusal reaches the user as bare silence.
                {
                    std::ifstream fch ("Source/ChainHost.cpp");
                    std::stringstream sch; sch << fch.rdbuf();
                    const auto body = functionBody (juce::String (sch.str()),
                                                    "void ChainHost::applyChainEdits");
                    check (body.contains ("resolveOfferedName(op.name, &why, &ambiguous)"),
                           "nl PIN10: the edit-op sites ask resolveOfferedName for the candidates");
                    check (body.contains ("ambiguousNameText(ambiguous)"),
                           "nl PIN10: and put them in the message the user reads");
                    // ONE author for the clause: a second copy is a second
                    // wording, and they drift.
                    const auto ch = codeOnly (juce::String (sch.str()));
                    check (ch.contains ("juce::String ChainHost::ambiguousNameText"),
                           "nl PIN10: and the clause has exactly one author");
                }
            }

            // ---- nl PIN4: a Link edit op, which has no feed to fall back on
            {
                ChainHost linkHost;   // never calls buildRecommendable, as LinkProcessor never does
                check (linkHost.recommendableEntries().empty(),
                       "nl PIN4: a Link-shaped host has an EMPTY feed table");
                const auto now  = linkHost.resolveOfferedName ("CLA-76");
                const auto then = ladder (poolEmpty, "CLA-76");
                check (then.name == "CLA-76 (s)",
                       "nl PIN4: a Link edit op naming \"CLA-76\" resolves to STEREO",
                       now.name + " -> " + then.name);
                std::ifstream flp ("Source/LinkProcessor.cpp");
                std::stringstream slp; slp << flp.rdbuf();
                const auto lp = codeOnly (juce::String (slp.str()));
                check (! lp.contains ("buildRecommendable"),
                       "nl PIN4: and LinkProcessor still never builds one, so the ladder is all it has");
                check (lp.contains ("chainHost.resolveByName(name, chainFormatFilter(), &matchLog)"),
                       "nl PIN4: the Link's chain build resolves through the same ladder");
            }

            // ---- nl PIN5: nothing that is not a channel variant moves ------
            // Every name in the corpus, and every name the feed publishes, old
            // resolver against new. The ONLY rows allowed to move are WaveShell
            // registrations, and only onto a better variant of themselves.
            {
                juce::StringArray names;
                for (const auto& d : poolEmpty)
                {
                    names.addIfNotAlreadyThere (d.name);
                    names.addIfNotAlreadyThere (ChainHost::stripParenthetical (d.name));
                }
                int moved = 0, movedNonWaves = 0, movedOffProduct = 0;
                juce::String firstNonWaves, firstOffProduct;
                for (const auto& n : names)
                {
                    const auto now  = host.resolveByName (n, {});
                    const auto then = ladder (poolEmpty, n);
                    if (now.name == then.name && now.uniqueId == then.uniqueId
                        && now.pluginFormatName == then.pluginFormatName) continue;
                    ++moved;
                    if (! echojay::isWaveShellRegistration (now)
                        || ! echojay::isWaveShellRegistration (then))
                    {
                        ++movedNonWaves;
                        if (firstNonWaves.isEmpty())
                            firstNonWaves = n + ": " + now.name + " -> " + then.name;
                    }
                    else if (ChainHost::stripParenthetical (now.name)
                             != ChainHost::stripParenthetical (then.name))
                    {
                        ++movedOffProduct;
                        if (firstOffProduct.isEmpty())
                            firstOffProduct = n + ": " + now.name + " -> " + then.name;
                    }
                }
                check (movedNonWaves == 0,
                       "nl PIN5: no non-Waves name resolves anywhere else",
                       juce::String (movedNonWaves) + " moved; first: " + firstNonWaves);
                check (movedOffProduct == 0,
                       "nl PIN5: and no name moves to a DIFFERENT product, only a better variant",
                       juce::String (movedOffProduct) + " moved; first: " + firstOffProduct);
                std::cout << "  MEASURED  " << moved << " of " << names.size()
                          << " names in the corpus resolve somewhere new, all of them"
                             " WaveShell variants of the same product\n";
            }

            // ---- nl PIN6: the format filter composes, it is not bypassed ---
            {
                int notAU = 0; juce::String firstNotAU;
                const auto waves = echojay::wavesRegistryFeedRows (poolEmpty, {});
                for (const auto& r : waves.rows)
                {
                    const auto d = ladder (poolAU, r.name);
                    if (d.name.isNotEmpty() && d.pluginFormatName != "AudioUnit")
                    { ++notAU; if (firstNotAU.isEmpty()) firstNotAU = r.name + " -> " + d.pluginFormatName; }
                }
                check (notAU == 0,
                       "nl PIN6: under an AudioUnit filter the ladder answers with AudioUnits only",
                       juce::String (notAU) + "; first: " + firstNotAU);
                // A VST3-filtered pool holds no WaveShell rows on this machine
                // (the shell is one thin row until enumerated), so the
                // preference has nothing to rank and refuses rather than
                // reaching past the filter into the AU rows.
                const auto poolVst3 = poolOf ("VST3");
                int wavesInVst3 = 0;
                for (const auto& d : poolVst3)
                    if (echojay::isWaveShellRegistration (d)) ++wavesInVst3;
                check (wavesInVst3 == 0 && ladder (poolVst3, "CLA-76").name.isEmpty(),
                       "nl PIN6: and a VST3 filter still resolves no Waves product at all",
                       juce::String (wavesInVst3) + " waves rows in the VST3 pool");
            }

            // ---- nl PIN7: the wiring ---------------------------------------
            {
                std::ifstream fch ("Source/ChainHost.cpp");
                std::stringstream sch; sch << fch.rdbuf();
                const auto src = juce::String (sch.str());
                const auto ch  = codeOnly (src);
                check (ch.contains ("#include \"EJNameLadder.h\""),
                       "nl PIN7: ChainHost includes the ladder it is pinned against");
                const auto body = functionBody (src, "juce::PluginDescription ChainHost::resolveByName");
                // Six arguments since 31 Aug 2026: the trailing out-param is
                // how an ambiguous tie reaches the caller as candidates rather
                // than as a bare miss. Pinned WITH it, because a five-argument
                // call still compiles and still resolves -- it just loses the
                // refusal's only content.
                check (body.contains ("echojay::matchInPool(pool, raw, base, manu, how, ambig)"),
                       "nl PIN7: resolveByName searches BOTH pools through the shared ladder");
                check (! body.contains ("how = \"exact\"")
                       && ! body.contains ("how = \"normalised\""),
                       "nl PIN7: and keeps no second copy of the rungs");
                // The preamble the pins mirror: raw, base and the manufacturer
                // hint. If this moves, the `ladder` lambda above is measuring
                // something the shipped path no longer does.
                check (body.contains ("auto base = stripParenthetical(raw);")
                       && body.contains ("manu = raw.fromLastOccurrenceOf(\" (\", false, false)"),
                       "nl PIN7: the request preamble is still raw / base / manufacturer hint");
                check (body.contains ("if (formatFilter.isEmpty())")
                       && body.contains ("cands = collapseAuPreferring(cands);"),
                       "nl PIN7: the pool is still filtered and collapsed BEFORE the ladder sees it");
                std::ifstream fnl ("Source/EJNameLadder.h");
                std::stringstream snl; snl << fnl.rdbuf();
                const auto nl = codeOnly (juce::String (snl.str()));
                check (nl.contains ("channelVariantIsBetter"),
                       "nl PIN7: the ladder ranks through the SHIPPED 9c4f629 rule, not a copy");
            }

            // ---- THE TABLE: six paths, 289 products, before and after -------
            {
                const auto waves = echojay::wavesRegistryFeedRows (poolEmpty, {});
                ChainHost linkHost;   // no feed, as in a Link process
                // Lib / Header, not Before / After: both sides are the
                // post-28d3f53 ladder now, one linked and one compiled here.
                struct Row { const char* label; int monoLib = 0, monoHdr = 0,
                             wrongLib = 0, wrongHdr = 0,
                             missLib = 0, missHdr = 0; };
                Row A  { "A  build, exact branch      " };
                Row Ad { "A' build, fallback          " };
                Row B  { "B  add / replace            " };
                Row D  { "D  recall, bare name        " };
                Row H  { "H  Link edit ops            " };
                Row I  { "I  Link chain build         " };
                // Two different questions, and only the second is a defect.
                // "mono" counts a mono or (m->s) build; a product registered
                // ONLY in mono is supposed to land there. "wrong" counts a
                // build worse than the best that product actually offers,
                // which is what the ladder was getting wrong.
                auto tally = [] (Row& r, const juce::PluginDescription& viaLib,
                                 const juce::PluginDescription& viaHdr,
                                 const juce::PluginDescription& best)
                {
                    auto mono = [] (const juce::PluginDescription& d)
                    {
                        const int rank = echojay::channelVariantRank (d.name);
                        return rank == 2 || rank == 3;   // (m) or (m->s)
                    };
                    auto worse = [&best] (const juce::PluginDescription& d)
                    {
                        return echojay::channelVariantIsBetter (best.name, d.name);
                    };
                    if (viaLib.name.isEmpty()) ++r.missLib;
                    else { if (mono (viaLib)) ++r.monoLib; if (worse (viaLib)) ++r.wrongLib; }
                    if (viaHdr.name.isEmpty()) ++r.missHdr;
                    else { if (mono (viaHdr)) ++r.monoHdr; if (worse (viaHdr)) ++r.wrongHdr; }
                };
                for (const auto& p : waves.rows)
                {
                    const auto& n = p.name;
                    // A: the feed's stored description, which the ladder never
                    // touches -- both columns are the same read.
                    juce::PluginDescription fed;
                    for (const auto& e : host.recommendableEntries())
                        if (e.displayName.trim().equalsIgnoreCase (n)) { fed = e.desc; break; }
                    tally (A, fed, fed, p.desc);
                    tally (Ad, host.resolveByName (n, "AudioUnit"), ladder (poolAU, n), p.desc);
                    // B composes exactly as resolveOfferedName does: ladder
                    // first, the feed's own table only on a miss.
                    auto bAfter = ladder (poolEmpty, n);
                    if (bAfter.name.isEmpty())
                        for (const auto& e : host.recommendableEntries())
                            if (e.displayName.trim().equalsIgnoreCase (n)) { bAfter = e.desc; break; }
                    tally (B, host.resolveOfferedName (n), bAfter, p.desc);
                    tally (D, host.resolveByName (n, {}), ladder (poolEmpty, n), p.desc);
                    tally (H, linkHost.resolveOfferedName (n), ladder (poolEmpty, n), p.desc);
                    tally (I, linkHost.resolveByName (n, "AudioUnit"), ladder (poolAU, n), p.desc);
                }
                std::cout << "  MEASURED  per path, over " << waves.products
                          << " Waves products: mono/(m->s) lib | header,"
                             " then WORSE-THAN-BEST, then unresolved\n";
                for (const Row* r : { &A, &Ad, &B, &D, &H, &I })
                    std::cout << "  MEASURED    " << r->label
                              << "mono " << r->monoLib << " | " << r->monoHdr
                              << " | wrong " << r->wrongLib << " | " << r->wrongHdr
                              << " | miss " << r->missLib << " | " << r->missHdr << "\n";
                check (Ad.wrongHdr == 0 && B.wrongHdr == 0 && D.wrongHdr == 0
                       && H.wrongHdr == 0 && I.wrongHdr == 0 && A.wrongHdr == 0,
                       "nl PIN8: NO path loads a worse build than the product offers",
                       "A=" + juce::String (A.wrongHdr) + " A'=" + juce::String (Ad.wrongHdr)
                           + " B=" + juce::String (B.wrongHdr) + " D=" + juce::String (D.wrongHdr)
                           + " H=" + juce::String (H.wrongHdr) + " I=" + juce::String (I.wrongHdr));
                check (Ad.monoHdr == B.monoHdr && B.monoHdr == D.monoHdr
                       && D.monoHdr == H.monoHdr && H.monoHdr == I.monoHdr,
                       "nl PIN8: every ladder path agrees on the same products",
                       juce::String (Ad.monoHdr) + " mono-only products, and they must stay mono");
                check (A.missHdr == A.missLib && Ad.missHdr == Ad.missLib
                       && B.missHdr == B.missLib && D.missHdr == D.missLib
                       && H.missHdr == H.missLib && I.missHdr == I.missLib,
                       "nl PIN8: and the two compilations resolve the same products");
                // THE ARM THE RENAME EARNS: with one rule on both sides the
                // columns must now MATCH, which the old before/after labels
                // could not have asserted. A divergence here means the linked
                // archive is stale again -- the standing hazard this file is
                // built around -- and every reading below it is then a
                // reading of two different ladders.
                check (A.wrongLib == A.wrongHdr && Ad.wrongLib == Ad.wrongHdr
                       && B.wrongLib == B.wrongHdr && D.wrongLib == D.wrongHdr
                       && H.wrongLib == H.wrongHdr && I.wrongLib == I.wrongHdr
                       && Ad.monoLib == Ad.monoHdr && B.monoLib == B.monoHdr
                       && D.monoLib == D.monoHdr && H.monoLib == H.monoHdr
                       && I.monoLib == I.monoHdr,
                       "nl PIN8: the linked lib and the compiled header agree -- if this reddens, REBUILD");
            }

            // ---- nl PIN9: is insertPreferredBase now redundant? -------------
            // A reads recommendable_ DIRECTLY (loadByRecommendedName's first
            // branch) and never calls the ladder, so the feed's stored
            // description is the only preference A has. The measurement that
            // says so: the feed's descriptions and the ladder's answers now
            // AGREE everywhere, which is the property that was broken, and the
            // mutation that reverts insertPreferredBase reddens A alone.
            {
                int disagree = 0; juce::String firstDisagree;
                juce::StringArray disagreeNames;   // display names, for the known-set check
                for (const auto& e : host.recommendableEntries())
                {
                    if (! echojay::isWaveShellRegistration (e.desc)) continue;
                    const auto viaLadder = ladder (poolEmpty, e.displayName);
                    if (viaLadder.name.isEmpty()) continue;   // feed-only name, PIN5 covers it
                    if (viaLadder.name != e.desc.name)
                    {
                        ++disagree;
                        disagreeNames.addIfNotAlreadyThere (e.displayName);
                        if (firstDisagree.isEmpty())
                            firstDisagree = e.displayName + ": feed " + e.desc.name
                                          + " vs ladder " + viaLadder.name;
                    }
                }
                // FIXED 31 Aug 2026, and the quarantine is retired.
                //
                // THE RETRACTION FIRST. This block used to say the two rows
                // were "two DIFFERENT Waves products" that collide on one
                // normalised base name. That was wrong, and nothing measured
                // it -- it was inferred from the names. The registry says one
                // product:
                //     Nx Ambisonics (4->2)   aufx,NX4B,ksWV   uid 445e056c
                //     NX Ambisonics (4->4)   aufx,NX4D,ksWV   uid 445e056a
                // a shared NX4 product code with the channel configuration in
                // the trailing character, which is the shape of every Waves
                // family here (B360 is B3M4/B3S4/B354/B364/B384, Abbey Road
                // Chambers is STEM/STES/STEX). Two genuinely different
                // products carry different product CODES: GTR Stomp 2/4/6 are
                // GC2*/GC4*/GCN*, API-2500 and API-560 are APC* and AE5*.
                // Same vendor, same version, adjacent uids. It is ONE product,
                // two channel configurations, and a name Waves capitalised two
                // ways.
                //
                // THE CAUSE, WHICH WAS ALSO NAMED WRONG. This was recorded as
                // a tie-break defect -- both rows rank 4, so channelVariantIsBetter
                // is false either way and iteration order decides. True, and
                // not the cause. The cause is that the two sides KEY THE SAME
                // POPULATION DIFFERENTLY: EJWavesRegistryFeed.h keyed products
                // case-SENSITIVELY (289 products) while the ladder keys them
                // case-INSENSITIVELY through normalizeName (288). The feed
                // therefore offered two products where the ladder could only
                // ever answer one, and the row it carried was not the row the
                // ladder returned. Fixed by folding the feed's product key
                // (wavesProductKey); the ladder's equal-rank first-wins rule is
                // untouched, because it is the no-change guarantee that keeps
                // every single-candidate name resolving exactly as it did.
                //
                // The known-set below is consequently EMPTY, and an empty
                // known-set is the shape this pin has to be careful about: a
                // for-loop over nothing checks nothing. The count arm at the
                // bottom is what actually holds it, and it fails on an empty
                // array rather than passing by having no work to do.
                std::cout << "  MEASURED  feed/ladder disagreements: " << disagree
                          << " (" << disagreeNames.joinIntoString (", ") << ")\n";
                // A juce::StringArray rather than a C array: `const char*
                // k[] = {}` is not even legal C++, and the shape has to be one
                // that can legally hold nothing now that nothing is expected.
                static const juce::StringArray kKnownDisagree {};
                juce::StringArray unexpected;
                for (const auto& d : disagreeNames)
                    if (! kKnownDisagree.contains (d)) unexpected.add (d);
                juce::StringArray missing;
                for (const auto& k : kKnownDisagree)
                    if (! disagreeNames.contains (k)) missing.add (k);
                check (unexpected.isEmpty(),
                       "nl PIN9: no feed/ladder disagreement outside the known set (which is now empty)",
                       juce::String (unexpected.size()) + " unexpected: " + unexpected.joinIntoString (", ")
                           + "; first overall: " + firstDisagree);
                check (missing.isEmpty(),
                       "nl PIN9: and every known disagreement is still there -- if this reddens it was FIXED, retire the exception",
                       "no longer disagreeing: " + missing.joinIntoString (", "));
                // THE ARM AN EMPTY KNOWN-SET NEEDS. Both loops above iterate a
                // set that is now empty, so both pass by having nothing to
                // check -- vacuous, not verified. This one asserts the count
                // directly and is the only arm that can fail when the known
                // set holds nothing. It reddens if ANY disagreement appears,
                // including the one that was just fixed coming back.
                check (disagreeNames.size() == kKnownDisagree.size(),
                       "nl PIN9: the feed and the ladder agree on every product they both name",
                       juce::String (disagreeNames.size()) + " disagreeing name(s), "
                           + juce::String (kKnownDisagree.size()) + " known; "
                           + juce::String (disagree) + " disagreeing entr(ies)");
                std::ifstream fch ("Source/ChainHost.cpp");
                std::stringstream sch; sch << fch.rdbuf();
                const auto ch = codeOnly (juce::String (sch.str()));
                check (ch.contains ("if (base != d.name) insertPreferredBase(base, d);"),
                       "nl PIN9: and insertPreferredBase STAYS -- A never consults the ladder");
                const auto load = functionBody (juce::String (sch.str()),
                                                "void ChainHost::loadByRecommendedName");
                check (load.indexOf ("recommendable_") >= 0
                       && load.indexOf ("recommendable_") < load.indexOf ("resolveByName"),
                       "nl PIN9: which is the reason -- A reads the feed table BEFORE the resolver");
            }
        }
    }

    // ---- DISABLE PROVENANCE + THE THREE-OUTCOME MODAL (29 Aug 2026) -------
    // plugin_disabled.json recorded WHY nothing, so two rows carrying the
    // load-failure signature (SSL Native X-EQ 2, Weiss Deess) could not be
    // attributed. Half of this is behavioural (the sidecar), half structural
    // (the modal's three outcomes and its disclosure), because a modal's
    // button wiring has no seam a unit test can reach.
    {
        std::cout << "disable provenance + fail modal:\n";

        // ---- dp PIN1: the sidecar round-trips, and UNKNOWN stays unknown ----
        // Written to a scratch HOME so the pin never touches the real file.
        {
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("ejdp_" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
            tmp.getChildFile ("EchoJay").createDirectory();
            const auto real = echojay::disableReasonsFile();
            // The helper keys off userApplicationDataDirectory, so drive it
            // through the real path but restore whatever was there.
            const bool had = real.existsAsFile();
            const auto saved = had ? real.loadFileAsString() : juce::String();
            if (had) real.deleteFile();

            echojay::recordDisableReasons ({ "uid_a", "uid_b" }, echojay::kDisableWhySettings);
            echojay::recordDisableReasons ({ "uid_c" },          echojay::kDisableWhyLoadFailure);
            check (echojay::disableReasonFor ("uid_a") == "settings",
                   "dp PIN1: a Settings untick records why", echojay::disableReasonFor ("uid_a"));
            check (echojay::disableReasonFor ("uid_c") == "load-failure",
                   "dp PIN1: a load-failure exclusion records why", echojay::disableReasonFor ("uid_c"));
            // THE LEGACY ROW. A uid disabled before this file existed has no
            // entry, and that must read as UNKNOWN rather than as a default.
            check (echojay::disableReasonFor ("uid_written_before_the_field").isEmpty(),
                   "dp PIN1: a row predating the field reads UNKNOWN, not a default reason");
            // Re-enabling drops the reason rather than leaving it to outlive
            // the row it describes.
            echojay::clearDisableReasons ({ "uid_a" });
            check (echojay::disableReasonFor ("uid_a").isEmpty(),
                   "dp PIN1: re-enabling clears the reason");
            check (echojay::disableReasonFor ("uid_b") == "settings",
                   "dp PIN1: and clearing one leaves the others alone");
            // The main file's shape is untouched by all of this: it is a bare
            // uid array and every existing reader does json.getArray().
            {
                auto dis = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                              .getChildFile ("Application Support/EchoJay/plugin_disabled.json");
                if (dis.existsAsFile())
                    check (juce::JSON::parse (dis.loadFileAsString()).getArray() != nullptr,
                           "dp PIN1: plugin_disabled.json is STILL a bare uid array (no reader broken)");
                else
                    std::cout << "  SKIP  dp PIN1 array-shape (no plugin_disabled.json here)\n";
            }
            real.deleteFile();
            if (had) real.replaceWithText (saved);
            tmp.deleteRecursively();
        }

        // ---- dp PIN2: BOTH doors record, and they record DIFFERENT things --
        {
            std::ifstream fe ("Source/PluginEditor.cpp");
            std::stringstream se; se << fe.rdbuf();
            const auto ed = codeOnly (juce::String (se.str()));
            std::ifstream fc ("Source/PluginChecklist.cpp");
            std::stringstream sc2; sc2 << fc.rdbuf();
            const auto cl = codeOnly (juce::String (sc2.str()));
            check (ed.contains ("recordDisableReasons({ p.uid }, echojay::kDisableWhyLoadFailure)")
                   || ed.contains ("recordDisableReasons({ p.uid },\n                                          echojay::kDisableWhyLoadFailure)"),
                   "dp PIN2: the load-failure door stamps load-failure");
            check (cl.contains ("recordDisableReasons(toDisable, echojay::kDisableWhySettings)"),
                   "dp PIN2: the Settings door stamps settings");
            check (cl.contains ("clearDisableReasons(toEnable)"),
                   "dp PIN2: and Settings clears the reason on re-enable");
        }

        // ---- dp PIN3: THREE OUTCOMES, AND NO TWO ARE SYNONYMS --------------
        // The ruling that forced this: the modal blamed a licence (transient)
        // and offered only permanent exclusion or nothing, so a user with an
        // unplugged dongle had no correct button.
        {
            std::ifstream fe ("Source/PluginEditor.cpp");
            std::stringstream se; se << fe.rdbuf();
            const auto raw = juce::String (se.str());
            const auto body = functionBody (raw, "void EchoJayEditor::showNextFailPrompt");
            check (body.isNotEmpty(), "dp PIN3: showNextFailPrompt found");
            check (body.contains ("showYesNoCancelBox"),
                   "dp PIN3: it is a THREE-button box, not the old two-button one");
            check (body.contains ("\"Don't suggest again\", \"Not now\", \"Keep it\""),
                   "dp PIN3: the three buttons, in order");
            // Three DISTINCT actions. If any two collapsed, the middle button
            // would be decoration - the exact defect being fixed.
            check (body.contains ("disablePluginByName(name)"),
                   "dp PIN3: outcome 1 persists (disablePluginByName)");
            check (body.contains ("excludeFromFeedThisSession(name)"),
                   "dp PIN3: outcome 2 is session-only (excludeFromFeedThisSession)");
            check (body.contains ("chainFailSessionSeen_.insert(name)"),
                   "dp PIN3: outcome 3 only stops the asking (chainFailSessionSeen_)");
            const int i1 = body.indexOf ("disablePluginByName(name)");
            const int i2 = body.indexOf ("excludeFromFeedThisSession(name)");
            const int i3 = body.indexOf ("chainFailSessionSeen_.insert(name)");
            check (i1 >= 0 && i2 >= 0 && i3 >= 0 && i1 != i2 && i2 != i3 && i1 != i3,
                   "dp PIN3: no two outcomes are the same call");
            // THE DISCLOSURE. Persistence the user is not told about is the
            // whole complaint; the body must say what it does and where to undo.
            check (body.contains ("unticks it in Settings"),
                   "dp PIN3: the body discloses that it unticks in Settings");
            check (body.contains ("re-tick it there any time"),
                   "dp PIN3: and says where to undo it");
            // The session door must write NOTHING.
            // codeOnly FIRST: this body's comment explains that it does NOT
            // call setPluginEnabled/saveEnabledState, and a raw grep reads that
            // prose as the code it forbids. The helper exists for exactly this.
            const auto sess = functionBody (codeOnly (raw),
                                            "void EchoJayEditor::excludeFromFeedThisSession");
            check (sess.isNotEmpty() && ! sess.contains ("saveEnabledState")
                   && ! sess.contains ("setPluginEnabled"),
                   "dp PIN3: \"Not now\" writes nothing - no save, no untick");
        }

        // ---- dp PIN4: the dead batch door is GONE --------------------------
        {
            std::ifstream fe ("Source/PluginEditor.cpp");
            std::stringstream se; se << fe.rdbuf();
            const auto ed = codeOnly (juce::String (se.str()));
            // Same trap: the deletion note inside this function names
            // disablePluginByName while explaining that it no longer calls it.
            const auto tapped = functionBody (codeOnly (juce::String (se.str())),
                                              "void EchoJayEditor::onResultChipTapped");
            check (tapped.isNotEmpty() && ! tapped.contains ("disablePluginByName"),
                   "dp PIN4: the batch chip handler no longer excludes anything");
            check (! ed.contains ("for (const auto& n : m.excludeNames)"),
                   "dp PIN4: and its loop over excludeNames is deleted");
        }

        // ---- dp PIN5: the spec says what the product does -------------------
        {
            std::ifstream fs2 ("CHAIN_AI_BUILD_SPEC.md");
            std::stringstream ss2; ss2 << fs2.rdbuf();
            const auto spec = juce::String (ss2.str());
            check (spec.contains ("NEVER exclude AUTOMATICALLY and NEVER exclude"),
                   "dp PIN5: the spec forbids automatic and batch exclusion");
            check (spec.contains ("AMENDED 29 Aug 2026"),
                   "dp PIN5: and records that it was amended, with the reasoning");
            check (! spec.contains ("exclusion is session-scoped in-memory only"),
                   "dp PIN5: the old rule the product violated is gone, not left contradicting it");
        }
    }

    std::cout << (failN == 0 ? "PASS" : "FAIL") << "  (" << passN << " ok, " << failN << " failed)\n";
    return failN == 0 ? 0 : 1;
}
