/*  PER-BLOCK COST of the main plugin's Link scan, MEASURED (6 Sep 2026 ruling):
    processBlock walks activeLinkSlots up to the slot CEILING every block.
    N registry rows are planted (with real producer rings so the walk connects
    them), the walk is refreshed, then processBlock runs 4000 blocks of 512 at
    48 kHz on a 2-channel buffer; the mean per-block wall time is reported.
    Usage: link_block_cost_test <rows>                                        */
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <chrono>

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const int rows = argc > 1 ? atoi (argv[1]) : 0;
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable\n"); return 99; }
    auto* slots = LinkShm::regSlots (reg);
    std::vector<int> mine; std::vector<void*> rings; std::vector<int> ringFds;
    for (int k = 0; k < rows; ++k)
        for (int i = kRegMaxSlots - 1; i >= 0; --i)
            if (LinkShm::loadAcquire (&slots[i].inUse) == 0 && std::find (mine.begin(), mine.end(), i) == mine.end())
            {
                const juce::String uid = "b1" + juce::String::toHexString (0x100000 + k).paddedLeft ('0', 8);
                const juce::String file = "audio_" + uid + ".bin";
                int rfd = -1, re = 0; void* rmap = LinkShm::openRingProducer (dir, file, 48000.0f, 2u, rfd, re);
                if (rmap) { rings.push_back (rmap); ringFds.push_back (rfd); }
                std::memset (&slots[i], 0, sizeof (RegistrySlot));
                std::strncpy (slots[i].audioFile, file.toRawUTF8(), 47); std::strncpy (slots[i].instanceUid, uid.toRawUTF8(), 10);
                slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2;
                LinkShm::storeRelease (&slots[i].heartbeat, 1u); LinkShm::storeRelease (&slots[i].inUse, 1u);
                mine.push_back (i); break;
            }
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    for (int t = 0; t < 8; ++t) { for (int i : mine) LinkShm::storeRelease (&slots[i].heartbeat, (uint32_t) (2 + t)); proc.refreshLinkRegistry(); }
    int connected = 0; for (const auto& s : proc.getLinkSlotInfos()) if (s.connected) ++connected;
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < 200; ++b) { buf.clear(); proc.processBlock (buf, midi); }   // warm
    const int blocks = 4000; double best = 1e9, total = 0;
    for (int rep = 0; rep < 5; ++rep)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) { buf.clear(); proc.processBlock (buf, midi); }
        const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count() / blocks;
        best = std::min (best, us); total += us;
    }
    std::printf ("rows planted %d (listed %d, connected %d): processBlock mean %.2f us/block (best of 5), avg %.2f us/block  [512 @ 48k = 10667 us budget]\n",
                 (int) mine.size(), (int) proc.getLinkSlotInfos().size(), connected, best, total / 5);
    for (int i : mine) LinkShm::releaseSlot (reg, i);
    for (size_t k = 0; k < rings.size(); ++k) juce::File (dir + "audio_b1" + juce::String::toHexString (0x100000 + (int) k).paddedLeft ('0', 8) + ".bin").deleteFile();
    return 0;
}
