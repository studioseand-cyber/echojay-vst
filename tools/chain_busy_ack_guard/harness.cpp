// chain_busy_ack_guard — 18 October 2026. THE ANSWERING SIDE of queue items 21, 16 and 13.
//
// `LinkProcessor::pollChainCommand` opened with `if (chainBuilding) return;`, above even the test for a command
// file. So a chain command that arrived while this rack was being rebuilt was neither applied, nor answered,
// nor logged - and the sender, after waiting its 8 s, told the user the rack "may be gone, or its track may be
// disabled". It was a live Link mid-build, and it applied that same command seconds later, after its outcome
// had already been reported as a failure. Three of the 17 Oct gate's S6 rows died this way.
//
// SILENCE IS NOT A VALID ANSWER ON THIS TRANSPORT, and that was already written down twice in this very
// function: the O2 note of 25 Sep says so about a seq that burned without an ack, and the Link's OWN plugin
// picker has always answered "A chain build is in progress" when a person adds a plugin mid-build. Only the
// remote road went quiet.
//
// SO THE LEGS ARE: while a rebuild is running, a command is ACKED "busy" and KEPT on disk; the busy ack is
// written once per command rather than once per poll; a sender that did not ask for busy acks still gets the
// old silence (which is what keeps today's Link safe to run under yesterday's main plugin); and when the build
// ends the kept command is applied FOR REAL and acked with its true outcome.
//
// WHY THE BUILD IS DRIVEN BY HAND. A built-in device loads SYNCHRONOUSLY (ChainHost::loadPluginAsync fires its
// callback inline for built-ins, and buildChainFromSpec then recurses inline), so a built-in-only build begins
// and ends inside one call and no poll can ever run during it. A build long enough to observe means loading
// real third-party AUs, which this suite does not do. The two state changes buildChainFromSpec makes at its
// ends are therefore made here instead - through the product's own publishBusy, not a copy of it - and leg (1)
// is what ties the flag to the real member: it sets the member the build sets and lets the product's own
// publishRegistrySnapshot publish it, so a build that forgot to raise the byte would still be caught by the
// heartbeat snapshot.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "ChainHost.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include <cstdio>
#include <memory>

struct EchoJayLinkSyncTestAccess
{
    static juce::String uid (LinkProcessor& p) { return p.instanceUid_; }
    static int slots (LinkProcessor& p)        { return p.chainHost.getNumSlots(); }
    // The two state changes buildChainFromSpec makes at its ends - the member, and the product's own publish.
    static void beginBuild (LinkProcessor& p)
    {
        p.chainBuilding        = true;
        p.chainBuildStartedMs_ = juce::Time::getMillisecondCounterHiRes();
        p.chainBusyAckedSeq_   = 0;
        p.publishBusy (true);
    }
    static void endBuild (LinkProcessor& p) { p.chainBuilding = false; p.publishBusy (false); }
    // The member alone, with no publish: leg (1) then asks the product to publish a snapshot and reads the byte.
    static void setBuildingMemberOnly (LinkProcessor& p, bool b) { p.chainBuilding = b; }
    static void snapshot (LinkProcessor& p) { p.publishRegistrySnapshot ("guard"); }
    // The registry row as a READER sees it - through LinkShm::readSlot, which is the function the main plugin's
    // refreshLinkRegistry uses, on the same bytes.
    static bool rowBusy (LinkProcessor& p)
    {
        LinkShm::SlotSnapshot s;
        if (p.regMap == nullptr || p.regSlotIdx < 0) return false;
        if (! LinkShm::readSlot (p.regMap, p.regSlotIdx, s)) return false;
        return s.busy;
    }
    static void poll (LinkProcessor& p) { p.pollChainCommand(); }
    // A uid is generated before the registry row is claimed, so a guard that waits only for the uid can run
    // before there is any row to publish into (which is how leg (1) first went red - mine, not the product's).
    static bool registered (LinkProcessor& p) { return p.regMap != nullptr && p.regSlotIdx >= 0; }
};
using TA = EchoJayLinkSyncTestAccess;

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms)
{ const double t0 = juce::Time::getMillisecondCounterHiRes();
  while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
  { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.004, false); } }

juce::String shmDir() { int e = 0; return LinkShm::resolveDir (e); }
juce::File cmdFileFor (const juce::String& uid) { return juce::File (shmDir() + "chain-cmd-" + uid + ".json"); }
juce::File ackFileFor (const juce::String& uid) { return juce::File (shmDir() + "chain-ack-" + uid + ".json"); }

// A v:2 command exactly as EchoJayProcessor::writeChainEditCommand writes one, with the busy-ack flag the
// sender declares (or without it, which is every main plugin built before 18 Oct 2026).
void writeCmd (const juce::String& uid, int seq, bool wantBusyAck, const juce::String& addName)
{
    juce::Array<juce::var> ops;
    auto* op = new juce::DynamicObject();
    op->setProperty ("op", "add");
    op->setProperty ("name", addName);
    ops.add (juce::var (op));
    auto* cmd = new juce::DynamicObject();
    cmd->setProperty ("v", 2);
    cmd->setProperty ("seq", seq);
    cmd->setProperty ("editOps", juce::var (ops));
    cmd->setProperty ("baseSlots", juce::var (juce::Array<juce::var>()));
    cmd->setProperty ("sourceNote", "guard");
    if (wantBusyAck) cmd->setProperty ("wantBusyAck", true);
    cmdFileFor (uid).replaceWithText (juce::JSON::toString (juce::var (cmd), true));
}

juce::var ackOf (const juce::String& uid) { return juce::JSON::parse (ackFileFor (uid).loadFileAsString()); }

// WAIT FOR THE ANSWER, NOT FOR THE SIDE EFFECT. The first cut of legs (5) and (6) waited for the rack's slot
// count to grow and then read the ack - and the product writes the ack ~25 ms AFTER the slot lands (the slot
// appears at ChainHost's "built-in added as slot 0", the ack at EJLink's "applied add count=0->1"). So both
// legs read the PREVIOUS ack and went red against a product that was behaving exactly as designed. A test that
// races the thing it measures reports on its own timing; this one waits for the verdict it is asserting about.
bool waitForVerdict (const juce::String& uid, int seq, double maxMs)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < maxMs)
    {
        pumpMs (50);
        const auto a = ackOf (uid);
        if ((int) a.getProperty ("seq", -1) == seq
            && a.getProperty ("status", juce::var()).toString().isNotEmpty()
            && a.getProperty ("status", juce::var()).toString() != "busy")
            return true;
    }
    return false;
}

std::unique_ptr<LinkProcessor> makeLink (const char* nm)
{
    auto l = std::make_unique<LinkProcessor>();
    l->linkName = nm; l->markTypedNameAuthoritative();
    l->prepareToPlay (48000.0, 512);
    for (int i = 0; i < 400 && TA::uid (*l).isEmpty(); ++i) pumpMs (10);
    for (int i = 0; i < 400 && ! TA::registered (*l); ++i) pumpMs (10);
    return l;
}
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("chain_busy_ack_guard: a chain command that arrives mid-rebuild is answered, not ignored "
                 "(shared dir %s)\n", shmDir().toRawUTF8());

    // An `add` resolves through BuiltinDeviceRegistry, which a static archive only fills if something references
    // it. An empty registry would abort every add and this guard would charge the product for its own link line.
    const auto* eq = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ");
    if (eq == nullptr)
    { std::printf ("HARNESS PRECONDITION FAILED: no built-in device registry in this binary, so no add can resolve\n"); return 2; }

    auto link = makeLink ("Guard Kick");
    const auto uid = TA::uid (*link);
    if (uid.isEmpty()) { std::printf ("HARNESS PRECONDITION FAILED: the Link never claimed a uid\n"); return 2; }
    if (! TA::registered (*link))
    { std::printf ("HARNESS PRECONDITION FAILED: the Link never claimed a registry row, so there is no byte to read\n"); return 2; }
    std::printf ("    uid=%s, rack holds %d slot(s)\n", uid.toRawUTF8(), TA::slots (*link));
    cmdFileFor (uid).deleteFile(); ackFileFor (uid).deleteFile();

    // ---- (1) THE REGISTRY CARRIES "I AM REBUILDING" -----------------------------------------------------
    std::printf ("== (1) the busy byte comes from the real member, through the product's own snapshot ==\n");
    {
        check (! TA::rowBusy (*link), "(1) fixture: an idle rack publishes busy=0");
        TA::setBuildingMemberOnly (*link, true);
        TA::snapshot (*link);
        const bool up = TA::rowBusy (*link);
        TA::setBuildingMemberOnly (*link, false);
        TA::snapshot (*link);
        const bool down = TA::rowBusy (*link);
        check (up && ! down,
               "(1) publishRegistrySnapshot publishes chainBuilding into registry byte 126, so the heartbeat "
               "keeps it true even if a build path forgot to raise it - and a reader can tell BUSY from DEAD",
               juce::String ("during=") + (up ? "1" : "0") + " after=" + (down ? "1" : "0"));
    }

    // ---- (2) A COMMAND MID-REBUILD IS ANSWERED AND KEPT -------------------------------------------------
    std::printf ("== (2) a command arriving mid-rebuild: acked BUSY, and NOT consumed ==\n");
    const int seqBusy = 77001;
    {
        TA::beginBuild (*link);
        writeCmd (uid, seqBusy, true, "EchoJay EQ");
        pumpMs (600);
        const auto a = ackOf (uid);
        const auto st = a.getProperty ("status", juce::var()).toString();
        check ((int) a.getProperty ("seq", -1) == seqBusy && st == "busy",
               "(2) THE FIX: the rack answers \"busy\" for this seq instead of going silent. The silence is what "
               "the sender turned into \"it may be gone, or its track may be disabled\" about a live Link",
               "status=\"" + st + "\" seq=" + a.getProperty ("seq", juce::var()).toString());
        check (a.getProperty ("reason", juce::var()).toString().contains ("queued"),
               "(2) ...and the reason says the command is queued rather than refused",
               a.getProperty ("reason", juce::var()).toString());
        check (cmdFileFor (uid).existsAsFile(),
               "(2) the command FILE IS STILL THERE: a busy ack is not a consumption, so the build finishes and "
               "the command is applied for real afterwards",
               cmdFileFor (uid).existsAsFile() ? "kept" : "CONSUMED - the edit would be lost");
        check (TA::slots (*link) == 0,
               "(2) ...and nothing was applied while the rebuild was running",
               juce::String (TA::slots (*link)) + " slot(s)");
    }

    // ---- (3) ONCE PER COMMAND, NOT ONCE PER POLL --------------------------------------------------------
    std::printf ("== (3) the busy ack is written once per seq, not once per poll ==\n");
    {
        ackFileFor (uid).deleteFile();
        pumpMs (700);              // ~7 polls at the 100 ms cadence
        check (! ackFileFor (uid).existsAsFile(),
               "(3) the ack is not rewritten on every poll - a rack rebuilding for thirty seconds would "
               "otherwise write three hundred acks and three hundred log lines",
               ackFileFor (uid).existsAsFile() ? "rewritten" : "written once");
    }

    // ---- (4) A SENDER THAT DID NOT ASK STILL GETS THE OLD SILENCE ---------------------------------------
    std::printf ("== (4) a main plugin from before today (no wantBusyAck) is answered exactly as it was ==\n");
    {
        cmdFileFor (uid).deleteFile(); ackFileFor (uid).deleteFile();
        const int seqOld = 77002;
        writeCmd (uid, seqOld, false, "EchoJay EQ");
        pumpMs (600);
        check (! ackFileFor (uid).existsAsFile() && cmdFileFor (uid).existsAsFile(),
               "(4) no busy ack for a sender that cannot understand one (it reads any status but \"ok\" as a "
               "refusal), and its command is still kept for the end of the build - so this change is additive",
               juce::String (ackFileFor (uid).existsAsFile() ? "acked" : "silent") + ", command "
                   + (cmdFileFor (uid).existsAsFile() ? "kept" : "consumed"));
    }

    // ---- (5) WHEN THE BUILD ENDS, THE KEPT COMMAND IS APPLIED FOR REAL ----------------------------------
    std::printf ("== (5) the build ends: the kept command is applied and acked with its TRUE outcome ==\n");
    {
        cmdFileFor (uid).deleteFile(); ackFileFor (uid).deleteFile();
        const int seqReal = 77003;
        writeCmd (uid, seqReal, true, "EchoJay EQ");
        pumpMs (400);
        check (ackOf (uid).getProperty ("status", juce::var()).toString() == "busy",
               "(5) fixture: it is busy-acked first");
        TA::endBuild (*link);
        const bool answered = waitForVerdict (uid, seqReal, 10000);
        const auto a  = ackOf (uid);
        const auto st = a.getProperty ("status", juce::var()).toString();
        std::printf ("    final ack: status=%s slotsAfter=%s\n", st.toRawUTF8(),
                     a.getProperty ("slotsAfter", juce::var()).toString().toRawUTF8());
        check (TA::slots (*link) == 1,
               "(5) the command survived the rebuild and was applied when the rack was free",
               juce::String (TA::slots (*link)) + " slot(s)");
        check (answered && (int) a.getProperty ("seq", -1) == seqReal && st != "busy",
               "(5) ...and the busy ack was overwritten by the real verdict under the same seq, which is what "
               "the waiting side reports to the user",
               "status=\"" + st + "\"");
        check (! cmdFileFor (uid).existsAsFile(),
               "(5) ...and the command file is consumed exactly once, when it is really applied");
    }

    // ---- (6) THE IDLE PATH IS UNTOUCHED -----------------------------------------------------------------
    std::printf ("== (6) with no rebuild running, nothing about the old road has changed ==\n");
    {
        cmdFileFor (uid).deleteFile(); ackFileFor (uid).deleteFile();
        const int seqIdle = 77004;
        const int before = TA::slots (*link);
        writeCmd (uid, seqIdle, true, "EchoJay EQ");
        const bool answered = waitForVerdict (uid, seqIdle, 10000);
        const auto a = ackOf (uid);
        check (answered && TA::slots (*link) > before && (int) a.getProperty ("seq", -1) == seqIdle
                 && a.getProperty ("status", juce::var()).toString() != "busy",
               "(6) an idle rack applies a command and acks its outcome, with no busy ack in the way",
               "status=\"" + a.getProperty ("status", juce::var()).toString() + "\", "
                   + juce::String (TA::slots (*link)) + " slot(s)");
        check (! TA::rowBusy (*link), "(6) ...and the registry says it is not busy");
    }

    cmdFileFor (uid).deleteFile(); ackFileFor (uid).deleteFile();
    std::printf ("\n==== chain_busy_ack_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
