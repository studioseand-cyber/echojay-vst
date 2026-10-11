// borrow_migration_guard - 11 October 2026, with plan section 6's removals.
//
// SEAN'S RULE, in his words: "a borrowed copy saved by an older build must never be lost (the 4 Oct case) and
// never doubled." This guard is that rule, both halves, on the real EchoJayProcessor - and the new fact that
// makes the rule finite: nothing lives only on this side any more.
//
// THE 4 OCTOBER CASE, because it is the reason any of this exists. A chain built on a Link track while this
// instance held the lease lived ONLY in borrowHost_ - "a chain build lands in the SESSION (the borrowed host),
// never on the Link". Save without switching racks first and the rack was in no save format anywhere: this
// side wrote only its own chainHost, and the Link wrote its own (empty) chainModel. Sean lost a chain that way.
// The fix carried the open borrowed rack in the save under `borrowedRacks`, and handed it back when that Link
// was first seen - tie-broken on the LINK'S OWN TWO NUMBERS, both from its saved chunk, which is Sean's own
// ruling and his own reason: "non-empty Link wins" would restore a stale chain when he rebuilt on a rack that
// already had one and saved without switching.
//
//   chainRev == leaseBaseRev  the Link's rack did not move after the borrow began -> OUR copy is newer, restore
//   different / either -1     a hand-back or a delete happened on the Link -> the LINK is authoritative, drop
//
// WHAT CHANGED TODAY. A build on a Link rack is now a COMMAND (borrowPushBuiltChain), sent and acked when the
// user makes it, exactly as every other structural edit already was. So the Link has the rack before any save
// is possible, and `borrowedRacks` is no longer WRITTEN. The reader stays, for the projects that still carry
// one - which is the migration, and is what legs (3)-(5) are about.
//
// THE LEGS
//   (1) a save taken with a rack addressed and slots in the mirror writes NO borrowedRacks key - there is
//       nothing that lives only here, so there is nothing to carry;
//   (2) ...and the rack reached the Link as a COMMAND: the queue holds a push and a chain-cmd is on disk for
//       that uid, carrying the build's ops. This is the leg that makes (1) safe rather than merely tidy;
//   (3) MIGRATION, NEVER LOST: an older build's blob, whose Link's chunk says rev == leaseBaseRev, is handed
//       back - a chain-cmd appears for that uid carrying the saved slots. This is the 4 Oct case;
//   (4) MIGRATION, NEVER DOUBLED: the same blob, whose Link says its rack MOVED after the borrow began, is
//       DROPPED - no chain-cmd, because the Link is authoritative and a deleted rack stays deleted;
//   (5) ...and the degenerate reading is the safe one: a chunk that cannot state both numbers (an older Link,
//       or no borrow open at save time) drops the copy rather than guessing;
//   (6) and the entry WAITS for its Link rather than deciding without it, which is what makes both load
//       orders work - this instance restored before its Link, or after it.
//
// PROVEN BOTH DIRECTIONS, 11 Oct 2026. Leg (1) is the new claim, so it was run against a KNOWN-BAD build:
// the previous build's write restored verbatim into getStateInformation, the V2 archive rebuilt, and this
// harness reports "FAIL (1) ... [present]". A guard for a removal that cannot see the thing it removed is
// unfalsifiable. Legs (3) and (4) carry their own two directions by construction - the same hand-built blob,
// two sidecars, opposite verdicts - which is the tie-break itself rather than a restatement of it.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include <cstdio>
#include <memory>
struct EchoJayAlignTestAccess { static void setLinks (EchoJayProcessor& p, std::vector<EchoJayProcessor::LinkSlotInfo> v)
    {   // The processor's 1 Hz timer rebuilds linkSlotInfos from the real registry (empty under the isolated
        // home) and would wipe the injected rows. The guard owns this input; the product path is untouched.
        static_cast<juce::Timer&> (p).stopTimer(); p.linkSlotInfos = std::move (v); } };
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms)
{ const double t0 = juce::Time::getMillisecondCounterHiRes();
  while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }
juce::String shmDir() { int e = 0; return LinkShm::resolveDir (e); }
juce::File cmdFile (const juce::String& uid) { return juce::File (shmDir() + "chain-cmd-" + uid + ".json"); }

// A V2 state blob as an OLDER BUILD wrote it: the ordinary keys this build still writes, plus the
// `borrowedRacks` array that this build has stopped writing. Built by hand on purpose - the point of a
// migration test is to read a shape this binary can no longer produce.
juce::MemoryBlock oldBuildBlobWithBorrowedRack (const juce::String& uid, int baseRev,
                                                const juce::StringArray& slotNames)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    // THE RICHER FIELDS MATTER AND ARE HERE ON PURPOSE. ui_guard's save-fix block used to assert that a live
    // save carried each slot's OWN GAINS, "which were in no save format at all - so every compressor build's
    // level match was lost on reopen". This build cannot produce such a save any more, so the fields come from
    // a hand-built blob instead - and they are still asserted to ride the restore command, because that loss
    // is the one a user would hear.
    juce::Array<juce::var> chain;
    for (int i = 0; i < slotNames.size(); ++i)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty ("name", slotNames[i]);
        so->setProperty ("settings", juce::String());
        so->setProperty ("bypassed", i == 1);          // slot 2 bypassed, as a build's hold leaves it
        so->setProperty ("wet",       i == 0 ? 0.40 : 1.0);
        so->setProperty ("outGainDb", i == 1 ? -3.5 : 0.0);
        so->setProperty ("preTrimDb", i == 1 ?  2.0 : 0.0);
        chain.add (juce::var (so));
    }
    auto* entry = new juce::DynamicObject();
    entry->setProperty ("uid",       uid);
    entry->setProperty ("baseRev",   baseRev);
    entry->setProperty ("masterWet", 1.0);
    entry->setProperty ("chain",     chain);
    juce::Array<juce::var> racks; racks.add (juce::var (entry));
    root->setProperty ("borrowedRacks", racks);
    const auto json = juce::JSON::toString (juce::var (root), true);
    juce::MemoryBlock mb; mb.append (json.toRawUTF8(), json.getNumBytesAsUTF8());
    return mb;
}

// The Link's published sidecar, with the two numbers the tie-break rests on. Written by hand for the same
// reason: these are what that Link's SAVED CHUNK said, and this process has no Link in it.
void writeLinkSidecar (const juce::String& uid, int restoredChainRev, int restoredLeaseBaseRev)
{
    LinkShm::RackSidecar rc;
    rc.valid = true;
    rc.uid   = uid;
    rc.revision = 7;
    rc.restoredChainRev     = restoredChainRev;
    rc.restoredLeaseBaseRev = restoredLeaseBaseRev;
    LinkShm::writeRackSidecar (shmDir(), rc);
}

void injectLink (EchoJayProcessor& p, const juce::String& uid, const juce::String& name)
{
    std::vector<EchoJayProcessor::LinkSlotInfo> v;
    EchoJayProcessor::LinkSlotInfo li;
    li.uid = uid; li.name = name; li.active = true; li.connected = true; li.channels = 2;
    v.push_back (li);
    EchoJayAlignTestAccess::setLinks (p, v);
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    // ISOLATION IS A PRECONDITION FOR EVIDENCE: a private state home or this harness proves nothing about
    // files it did not write.
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_borrow_migration_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("borrow_migration_guard: never lost, never doubled - and nothing lives only on this side any more\n");
    if (shmDir().isEmpty()) { std::printf ("  FAIL  the shared Link folder did not resolve\n"); return 1; }

    const auto dEq = BuiltinDeviceRegistry::descriptionFor (*BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ"));

    std::printf ("== (1)(2) a save taken with a rack addressed: nothing lives only here ==\n");
    {
        const juce::String uid = "lnk_mig_live";
        auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap;
        p.prepareToPlay (48000.0, 512);
        injectLink (p, uid, "Kick");
        p.borrowEngageBegin (uid, "lease-mig-1", true, true);
        pumpMs (100);
        auto* bh = p.borrowHostIfActiveFor (uid);
        check (bh != nullptr, "(1) the session engaged and the mirror exists");
        if (bh != nullptr)
        {
            EchoJayBorrowHostTestAccess::loadBuiltin (*bh, dEq);
            EchoJayBorrowHostTestAccess::loadBuiltin (*bh, dEq);
            pumpMs (200);
            check (bh->getNumSlots() == 2, "(1) two slots built into the mirror", juce::String (bh->getNumSlots()));

            cmdFile (uid).deleteFile();
            // The build path's own conversion and the build path's own command - the two functions the
            // editor calls, so this leg exercises the shipped road and not a copy of it.
            juce::Array<juce::var> arr;
            for (int i = 0; i < 2; ++i)
            { auto* eo = new juce::DynamicObject(); eo->setProperty ("name", "EchoJay EQ"); arr.add (juce::var (eo)); }
            const auto built = ChainHost::chainArrayToReplaceOps (arr, 0);
            const auto wire  = ChainHost::editOpsToWireVar (built);
            const int seq = p.borrowPushBuiltChain (wire, {}, 0, 2);
            check (seq > 0, "(2) a build ENQUEUES a command for that rack", juce::String (seq));
            pumpMs (150);
            check (cmdFile (uid).existsAsFile(),
                   "(2) ...and it is WRITTEN to the Link's command channel at once, not held until deselect");
            const auto cmd = juce::JSON::parse (cmdFile (uid).loadFileAsString());
            const auto* ce = cmd.getProperty ("editOps", juce::var()).getArray();
            check (ce != nullptr && ce->size() == 2,
                   "(2) ...carrying the build's own ops (two adds, 1-based wire)",
                   ce != nullptr ? juce::String (ce->size()) + " op(s)" : juce::String ("(no editOps)"));

            juce::MemoryBlock mb; p.getStateInformation (mb);
            const auto st = juce::JSON::parse (juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize()));
            check (! st.hasProperty ("borrowedRacks"),
                   "(1) THE CLAIM: a save with a rack addressed and slots in the mirror writes NO borrowedRacks "
                   "key - the Link has the rack, so a second record of it would be the only thing that could "
                   "disagree with it",
                   st.hasProperty ("borrowedRacks") ? juce::String ("present") : juce::String ("absent"));
        }
        p.borrowRelease (false);
    }

    std::printf ("== (3) MIGRATION, never lost: the 4 October case ==\n");
    {
        const juce::String uid = "lnk_mig_keep";
        writeLinkSidecar (uid, /*chainRev*/ 12, /*leaseBaseRev*/ 12);   // the Link did not move after the borrow began
        cmdFile (uid).deleteFile();
        auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap;
        const auto blob = oldBuildBlobWithBorrowedRack (uid, 12, { "EchoJay EQ", "EchoJay EQ", "EchoJay EQ" });
        p.setStateInformation (blob.getData(), (int) blob.getSize());
        p.prepareToPlay (48000.0, 512);
        pumpMs (200);
        check (p.pendingBorrowRestore_.count (uid) == 1,
               "(3) the older build's borrowedRacks entry is PARSED and pending", juce::String ((int) p.pendingBorrowRestore_.size()));
        // (6) and it waits: no Link in the registry yet, so nothing is decided.
        p.applyPendingBorrowRestores();
        check (! cmdFile (uid).existsAsFile() && p.pendingBorrowRestore_.count (uid) == 1,
               "(6) with that Link not yet present NOTHING is decided - the entry waits, which is what makes "
               "both load orders work");
        injectLink (p, uid, "Vocal");
        p.applyPendingBorrowRestores();
        check (cmdFile (uid).existsAsFile(),
               "(3) once the Link is seen and its chunk says rev == leaseBaseRev, the saved rack is HANDED BACK");
        const auto cmd = juce::JSON::parse (cmdFile (uid).loadFileAsString());
        const auto* ch = cmd.getProperty ("chain", juce::var()).getArray();
        check (ch != nullptr && ch->size() == 3,
               "(3) ...with all three slots, through the Link's own build parser",
               ch != nullptr ? juce::String (ch->size()) + " slot(s)" : juce::String ("(no chain)"));
        if (ch != nullptr && ch->size() == 3)
        {
            auto* s0 = (*ch)[0].getDynamicObject();
            auto* s1 = (*ch)[1].getDynamicObject();
            check (s1 != nullptr && std::abs ((double) s1->getProperty ("outGainDb") + 3.5) < 0.01,
                   "(3) ...carrying the slot's OUT gain, so a compressor build's level match survives the reopen",
                   s1 != nullptr ? s1->getProperty ("outGainDb").toString() : juce::String ("(no slot)"));
            check (s1 != nullptr && std::abs ((double) s1->getProperty ("preTrimDb") - 2.0) < 0.01,
                   "(3) ...and its PRE-trim",
                   s1 != nullptr ? s1->getProperty ("preTrimDb").toString() : juce::String());
            check (s1 != nullptr && (bool) s1->getProperty ("bypassed"), "(3) ...and its bypass");
            check (s0 != nullptr && std::abs ((double) s0->getProperty ("wet") - 0.40) < 0.01,
                   "(3) ...and the other slot's wet",
                   s0 != nullptr ? s0->getProperty ("wet").toString() : juce::String());
        }
        check (p.pendingBorrowRestore_.count (uid) == 0, "(3) ...and the entry is consumed: ONE attempt, never a loop");
    }

    std::printf ("== (4) MIGRATION, never doubled: the Link's rack moved after the borrow began ==\n");
    {
        const juce::String uid = "lnk_mig_drop";
        writeLinkSidecar (uid, /*chainRev*/ 19, /*leaseBaseRev*/ 12);   // a hand-back, or a delete, happened there
        cmdFile (uid).deleteFile();
        auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap;
        const auto blob = oldBuildBlobWithBorrowedRack (uid, 12, { "EchoJay EQ", "EchoJay EQ" });
        p.setStateInformation (blob.getData(), (int) blob.getSize());
        p.prepareToPlay (48000.0, 512);
        pumpMs (200);
        injectLink (p, uid, "Snare");
        p.applyPendingBorrowRestores();
        check (! cmdFile (uid).existsAsFile(),
               "(4) THE CLAIM: the copy is DROPPED, not applied - the Link is authoritative, which is what keeps "
               "a rack the user deleted deleted, and is the only thing standing between this migration and a "
               "doubled rack");
        check (p.pendingBorrowRestore_.count (uid) == 0, "(4) ...and the entry is gone, so it cannot fire later");
    }

    std::printf ("== (5) the degenerate reading is the safe one ==\n");
    {
        const juce::String uid = "lnk_mig_old";
        writeLinkSidecar (uid, /*chainRev*/ 12, /*leaseBaseRev*/ -1);   // an older Link, or no borrow was open at save
        cmdFile (uid).deleteFile();
        auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap;
        const auto blob = oldBuildBlobWithBorrowedRack (uid, 12, { "EchoJay EQ" });
        p.setStateInformation (blob.getData(), (int) blob.getSize());
        p.prepareToPlay (48000.0, 512);
        pumpMs (200);
        injectLink (p, uid, "Guitar");
        p.applyPendingBorrowRestores();
        check (! cmdFile (uid).existsAsFile(),
               "(5) a chunk that cannot state BOTH numbers keeps what the Link has, rather than guessing");
    }

    std::printf ("\n==== borrow_migration_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
