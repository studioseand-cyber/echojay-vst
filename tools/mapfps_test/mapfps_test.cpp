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
            // one declaration, three adds, one addArray to the card, one join
            // to the model: six. A seventh means a second consumer nobody
            // reviewed; a fifth means one of the tiers stopped being carried.
            check (n == 6, "6a PIN1: tierLines has exactly its six known uses",
                   "found " + juce::String (n));
        }
        check (codeOnly (ch).contains ("kept.addArray(tierLines);")
               && codeOnly (ch).contains ("s.settingsForModel = tierLines.joinIntoString(\"\\n\");"),
               "6a PIN1: both consumers take the SAME tier lines");
        // THE PROSE IS THE MODEL'S TO NOT HAVE (24 Aug). The card keeps
        // `kept`, which opens with setSlotSettings' description; the model
        // takes the tiers alone. If these ever converge again, description is
        // being handed back as state.
        check (codeOnly (ch).contains ("s.settings = kept.joinIntoString(\"\\n\");")
               && ! codeOnly (ch).contains ("s.settingsForModel = kept"),
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
            while ((at = code.indexOf (at, "settingsForModel.clear()")) >= 0) { ++n; at += 1; }
            check (n == 5, "6a PIN5: all five non-tiered settings writers clear the model copy",
                   "found " + juce::String (n) + " of 5");
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
            const auto nullJs = juce::JSON::toString (
                echojay::slotParamReadsFor (4, "Gone", nullptr), true);
            check (nullJs.contains ("\"readFailed\": true")
                   && nullJs.contains ("\"reads\": {}"),
                   "6c PIN2b: a NULL processor produces a readFailed slot", "got " + nullJs);
        }

        // PIN 3, structural — the client does NOT select, and the values do not
        // ride a block the model reads. Both are the contract, not a detail:
        // a port of the server's exposure rule was measured to miss the
        // governing switches the server had just added.
        {
            std::ifstream fch ("Source/ChainHost.cpp");
            std::stringstream sch; sch << fch.rdbuf();
            const auto ch = codeOnly (juce::String (sch.str()));
            check (ch.contains ("echojay::slotParamReadsFor("),
                   "6c PIN3: ChainHost serialises through the one shared helper");
            // NOT anchored on the /*readFailed*/ argument comment: codeOnly()
            // strips it, so the first version of this pin failed on its own
            // stripper rather than on the code. Anchor on the call and on the
            // log line, both of which survive comment stripping.
            check (ch.contains ("arr.add(echojay::slotParamReadsFor(i + 1, name, proc));"),
                   "6c PIN3: EVERY slot is added, unconditionally");
            check (! ch.contains ("if (proc == nullptr) continue;"),
                   "6c PIN3: no slot is dropped for having no instance");
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

                        // THROUGH slotParamReadsFor, THE SHIPPED PATH. An
                        // earlier version passed its own reader lambda to
                        // slotParamReadsVar, so it exercised a COPY of the read
                        // and a mutation that stopped the shipped reader ever
                        // touching the instance left this pin green. The one
                        // pin this contract exists for cannot be the one
                        // testing a reimplementation of the thing under test.
                        auto v = echojay::slotParamReadsFor (1, "bx_blackdist2", inst.get());
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

    std::cout << (failN == 0 ? "PASS" : "FAIL") << "  (" << passN << " ok, " << failN << " failed)\n";
    return failN == 0 ? 0 : 1;
}
