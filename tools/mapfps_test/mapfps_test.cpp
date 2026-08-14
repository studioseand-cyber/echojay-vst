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
#include "EchoJayParamMaps.h"
#include "EchoJayParamApply.h"
#include "EchoJayHistoryTrim.h"
#include "EchoJayChannelChats.h"
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

// Function body by signature: from the signature line to the first line
// that is exactly "}" at column 0. Every ChainHost method ends that way.
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

    std::cout << (failN == 0 ? "PASS" : "FAIL") << "  (" << passN << " ok, " << failN << " failed)\n";
    return failN == 0 ? 0 : 1;
}
