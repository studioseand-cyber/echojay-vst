/*  C3 (6 Sep 2026 ruling): after the seeded-name fix a fresh insert publishes an
    EMPTY name, so the replacement names must come from the MAIN plugin and be
    UNIQUE. This is the main plugin's own code: three live registry rows with
    empty names and distinct uids (exactly what three fixed Links publish),
    walked by EchoJayProcessor::refreshLinkRegistry, listed by
    getLinkDisplayList - the canonical list every Link-listing surface uses.
    N is derived from the row set: untitled rows sorted by uid, numbered 1..N
    (PluginProcessor.cpp, "Untitled N numbering ... stable by uid"). Rows are
    planted by hand (borrowhost_test's pattern) and released on exit.        */
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <set>
#include <vector>
#include <algorithm>

int main (int argc, char** argv)
{
    const int ROWS = argc > 1 ? atoi (argv[1]) : 3;
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d)\n", rerr); return 99; }
    auto* slots = LinkShm::regSlots (reg);
    std::vector<juce::String> uids; for (int k = 0; k < ROWS; ++k) uids.push_back ("c3" + juce::String::toHexString (0x100000 + k).paddedLeft ('0', 8).substring (0, 8));
    std::vector<int> planted (ROWS, -1);
    for (int k = 0; k < ROWS; ++k)
        for (int i = kRegMaxSlots - 1; i >= 0; --i)
            if (LinkShm::loadAcquire (&slots[i].inUse) == 0 && std::find (planted.begin(), planted.end(), i) == planted.end())
            {
                std::memset (&slots[i], 0, sizeof (RegistrySlot));
                std::strncpy (slots[i].audioFile, ("audio_" + uids[k] + ".bin").toRawUTF8(), 47);   // name EMPTY, as a fixed fresh insert publishes
                std::strncpy (slots[i].instanceUid, uids[k].toRawUTF8(), 10);
                slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2;
                LinkShm::storeRelease (&slots[i].heartbeat, 1u);
                LinkShm::storeRelease (&slots[i].inUse, 1u);
                planted[k] = i; break;
            }
    std::printf ("planted %d EMPTY-named live rows\n", ROWS);
    EchoJayProcessor mainProc; mainProc.prepareToPlay (48000.0, 512);
    double walkUs = 0; int walks = 0;
    for (int t = 0; t < 6; ++t)   // the walk lists a row only once its heartbeat is OBSERVED climbing
    {
        for (int k = 0; k < ROWS; ++k) LinkShm::storeRelease (&slots[planted[k]].heartbeat, (uint32_t) (2 + t));
        const auto w0 = juce::Time::getMillisecondCounterHiRes(); mainProc.refreshLinkRegistry(); walkUs += (juce::Time::getMillisecondCounterHiRes() - w0) * 1000.0; ++walks;
    }
    std::printf ("  refreshLinkRegistry (the 1-2 Hz walk) with %d live rows: %.0f us per call\n", ROWS, walkUs / walks);
    std::set<juce::String> names; int ours = 0;
    for (const auto& e : mainProc.getLinkDisplayList())
    {
        const bool mine = e.info.uid.startsWith ("c3");
        if (mine && (ours < 5 || ours >= ROWS - 2)) std::printf ("  row uid %-12s published \"%s\" -> displayed \"%s\"\n", e.info.uid.toRawUTF8(), e.info.name.toRawUTF8(), e.displayName.toRawUTF8());
        if (mine) { ++ours; names.insert (e.displayName); }
    }
    const bool ok = ours == ROWS && (int) names.size() == ROWS;
    std::printf ("%s: %d planted rows listed, %d DISTINCT display names\n", ok ? "PASS" : "FAIL", ours, (int) names.size());
    for (int k = 0; k < ROWS; ++k) LinkShm::releaseSlot (reg, planted[k]);
    return ok ? 0 : 1;
}
