// NEGATIVE CONTROL for P1/P2: the same two orderings against the committed
// pre-provenance Link (58f36d1). Old API only.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>
static void pump (int n) { for (int t = 0; t < n; ++t) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
static void drain() { pump (5); }
static juce::String slotName (void* reg, int i) { return i >= 0 ? juce::String::fromUTF8 (LinkShm::regSlots (reg)[i].displayName) : juce::String ("(no slot)"); }
static void nameFromHost (LinkProcessor& l, const char* n) { juce::AudioProcessor::TrackProperties tp; tp.name = std::make_optional (juce::String (n)); l.updateTrackProperties (tp); }
int main()
{
    echojay::requireIsolationOrDie ("link_provenance_negctl.cpp");
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI init;
    int err = 0; const auto dir = LinkShm::resolveDir (err); int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (! reg) { std::printf ("registry not mappable\n"); return 99; }
    std::printf ("negative control on the pre-provenance Link: layout %d slots\n", kRegMaxSlots);
    auto a = std::make_unique<LinkProcessor>(); a->linkName = "Vox"; a->prepareToPlay (48000.0, 512); nameFromHost (*a, "Track A"); a->updateShmState(); pump (40);
    juce::MemoryBlock chunkA; a->getStateInformation (chunkA);
    int p1 = 0, p2 = 0;
    for (int r = 0; r < 20; ++r)
    {
        { auto b = std::make_unique<LinkProcessor>(); b->prepareToPlay (48000.0, 512); b->setStateInformation (chunkA.getData(), (int) chunkA.getSize()); nameFromHost (*b, "Track B"); pump (150);
          const bool ok = b->diag.slotIdx >= 0 && b->getHostTrackName() == "Track B" && slotName (reg, b->diag.slotIdx) == "Track B";
          if (r == 0) std::printf ("  P1 B: host \"%s\" published \"%s\" -> %s\n", b->getHostTrackName().toRawUTF8(), slotName (reg, b->diag.slotIdx).toRawUTF8(), ok ? "PASS" : "FAIL");
          p1 += ok; drain(); b.reset(); drain(); }
        { auto c = std::make_unique<LinkProcessor>(); c->prepareToPlay (48000.0, 512); c->setStateInformation (chunkA.getData(), (int) chunkA.getSize()); pump (150); nameFromHost (*c, "Track C"); pump (40);
          const bool ok = c->diag.slotIdx >= 0 && c->getHostTrackName() == "Track C" && slotName (reg, c->diag.slotIdx) == "Track C";
          if (r == 0) std::printf ("  P2 C: host \"%s\" published \"%s\" -> %s\n", c->getHostTrackName().toRawUTF8(), slotName (reg, c->diag.slotIdx).toRawUTF8(), ok ? "PASS" : "FAIL");
          p2 += ok; drain(); c.reset(); drain(); }
    }
    std::printf ("NEGATIVE CONTROL P20 (pre-provenance build): BEFORE kept %d/20   AFTER kept %d/20\n", p1, p2);
    drain(); a.reset(); drain(); return 0;
}
