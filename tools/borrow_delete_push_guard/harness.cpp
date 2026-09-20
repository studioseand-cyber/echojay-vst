// borrow_delete_push_guard (20 Sep 2026): deletes on a BORROWED Link rack (the main plugin's BorrowHost) - when do they reach the
// Link? As coded, ONLY at deselect/close (borrowApplyAndRelease computes and sends the structure plan). A host save in between
// saves the Link's pre-delete chain. RED as it stands: no ctrl command with a structPlan exists before the release.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include <cstdio>
struct EchoJayAlignTestAccess { static LinkShm::StructureEdit::Plan plan (EchoJayProcessor& p) { return p.buildStructurePlan(); } static int origins (EchoJayProcessor& p) { return (int) p.borrowSlotOrigin_.size(); } };
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    int e = 0; const juce::String dir = LinkShm::resolveDir (e);
    std::printf ("borrow_delete_push_guard: deletes on a borrowed rack vs the Link (shared dir %s)\n", dir.toRawUTF8());
    const auto* eq = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ"); if (! eq) { std::printf ("no EchoJay EQ\n"); return 2; }
    EchoJayProcessor bp; bp.prepareToPlay (48000.0, 512);
    const juce::String uid = "uid-del";
    juce::File cmd (dir + "ctrl-cmd-" + uid + ".json"); cmd.deleteFile(); juce::File (dir + "ctrl-ack-" + uid + ".json").deleteFile();
    bp.borrowEngageBegin (uid, "lease-del", true, true); pumpMs (200);
    auto* bh = bp.borrowHost(); check (bh != nullptr && bp.borrowUid() == uid, "a borrow session is engaged on " + uid);
    if (! bh) return 2;
    for (int i = 0; i < 3; ++i) EchoJayBorrowHostTestAccess::loadBuiltin (*bh, BuiltinDeviceRegistry::descriptionFor (*eq));
    pumpMs (200); std::printf ("  borrowed host: %d slot(s), origins tracked: %d\n", bh->getNumSlots(), EchoJayAlignTestAccess::origins (bp));
    check (bh->getNumSlots() == 3, "three slots on the borrowed host");
    for (int i = bh->getNumSlots() - 1; i >= 0; --i) bh->removeSlot (i);
    pumpMs (500);
    check (bh->getNumSlots() == 0, "every slot deleted on the borrowed host");
    const bool pushedBeforeRelease = cmd.existsAsFile() && cmd.loadFileAsString().contains ("structPlan");
    check (pushedBeforeRelease, "deletes reach the Link IMMEDIATELY: a ctrl command carrying a structPlan exists before deselect (RED as it stands: the plan is computed and sent only by borrowApplyAndRelease)", cmd.existsAsFile() ? "file exists" : "no ctrl-cmd file");
    const auto plan = EchoJayAlignTestAccess::plan (bp);
    std::printf ("  plan computed now: %d op(s)\n", (int) plan.ops.size());
    bp.borrowApplyAndRelease (true); pumpMs (800);
    const bool pushedAtRelease = cmd.existsAsFile() && cmd.loadFileAsString().contains ("structPlan");
    std::printf ("  at deselect: ctrl-cmd %s%s\n", pushedAtRelease ? "WRITTEN with a structPlan" : "not written", pushedAtRelease ? "" : " (an empty plan from a session whose originals were seeded outside the pull sends nothing - see EJStruct lines)");
    cmd.deleteFile();
    std::printf ("\n==== borrow_delete_push_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
