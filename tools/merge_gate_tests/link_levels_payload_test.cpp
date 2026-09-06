// MEASURE the per-Link payload of a capture turn: buildLinkLevelsContext bytes with N live rows
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LinkShm.h"
#include <cstdio>
struct EchoJayTabStripTestAccess { static juce::String levels (EchoJayEditor& e) { return e.buildLinkLevelsContext(); } };
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI init;
    const int rows = argc > 1 ? atoi (argv[1]) : 1;
    int err = 0; const auto dir = LinkShm::resolveDir (err); int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    auto* slots = LinkShm::regSlots (reg); std::vector<int> mine;
    for (int k = 0; k < rows; ++k) for (int i = 0; i < kRegMaxSlots; ++i)
        if (LinkShm::loadAcquire (&slots[i].inUse) == 0 && std::find (mine.begin(), mine.end(), i) == mine.end())
        {   const juce::String uid = "ll" + juce::String::toHexString (0x100000 + k).paddedLeft ('0', 8).substring (0, 8);
            std::memset (&slots[i], 0, sizeof (RegistrySlot));
            std::strncpy (slots[i].displayName, ("Track " + juce::String (k + 1) + " Vocal Double").toRawUTF8(), 39);
            std::strncpy (slots[i].audioFile, ("audio_" + uid + ".bin").toRawUTF8(), 47); std::strncpy (slots[i].instanceUid, uid.toRawUTF8(), 10);
            slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2; slots[i].placement = 2; LinkShm::storeRelease (&slots[i].heartbeat, 1u); LinkShm::storeRelease (&slots[i].inUse, 1u);
            LinkMeterFrame* f = LinkShm::meterFrames (reg) + i; f->integrated = -18.3f - k * 0.1f; f->momentary = -16.1f; f->truePeakMax = -2.4f; LinkShm::storeRelease (&f->seq, 2u);
            mine.push_back (i); break; }
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    for (int t = 0; t < 6; ++t) { for (int i : mine) LinkShm::storeRelease (&slots[i].heartbeat, (uint32_t) (2 + t)); proc.refreshLinkRegistry(); }
    auto* ed = dynamic_cast<EchoJayEditor*> (proc.createEditor());
    const auto ctx = EchoJayTabStripTestAccess::levels (*ed);
    const int listed = (int) proc.getLinkSlotInfos().size();
    std::printf ("LINK LEVELS injection with %d live rows (%d listed): %d bytes  (%d bytes/Link)\n", rows, listed, (int) ctx.getNumBytesAsUTF8(), listed ? (int) ctx.getNumBytesAsUTF8() / listed : 0);
    std::printf ("  first 300 chars: %s\n", ctx.substring (0, 300).replaceCharacters ("\n", "|").toRawUTF8());
    delete ed; for (int i : mine) LinkShm::releaseSlot (reg, i); return 0;
}
