/*  BUILD A legs (8 Sep 2026 ruling), each with its negative control on the pre-fix archive:
      crash    - a capture > 2 s on a key-qualifying host channel (FullMix, the DEFAULT type) then stopCapture:
                 pre-fix the "EchoJay WAV Save" thread overflows its 512 KB stack (SIGBUS kills this process);
                 Build A survives and the offline key pass completes. Same `echojay::KeyEngine` construction
                 as the bus-placed-Link case (keySrcIdx >= 0 and == -1 share the one line).
      order    - registry rows claimed Z, then A, then M: getLinkDisplayList must read Z, A, M (pre-fix: A, M, Z)
      refusal  - borrowEngageBegin with inContextCapable=false must log the refusal naming the false term
    Console harness against the SHIPPING main archive; message-thread state machine (see v9_rack_restore_test). */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "PluginProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

static void pump (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }

static int plantRow (void* reg, const juce::String& dir, int slot, const char* name, const char* uid)
{
    auto* sl = LinkShm::regSlots (reg); auto& s = sl[slot];
    std::memset (&s, 0, sizeof (s));
    std::strncpy (s.displayName, name, sizeof (s.displayName) - 1);
    std::snprintf (s.audioFile, sizeof (s.audioFile), "audio_%s.bin", uid);
    std::strncpy (s.instanceUid, uid, sizeof (s.instanceUid) - 1);
    s.sampleRate = 48000.f; s.numChannels = 2; s.activeFlag = 0; s.heartbeat = 1;
    LinkShm::storeRelease (&s.inUse, 1u);
    LinkShm::RackSidecar rc; rc.uid = uid; rc.publisherPid = (int) getpid(); rc.hostPid = (int) getpid();
    LinkShm::writeRackSidecar (dir, rc);
    return slot;
}

struct Control
{
    juce::String mode; std::unique_ptr<EchoJayProcessor> p; int result = 1;
    void run()
    {
        if (mode == "crash")
        {
            p = std::make_unique<EchoJayProcessor>(); p->prepareToPlay (48000.0, 512);
            p->setChannelType (ChannelType::FullMix);                 // the DEFAULT type; it qualifies as a key source
            p->startCapture();
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; double ph = 0;
            for (int b = 0; b < 48000 * 4 / 512; ++b)             // 4 s of a 220 Hz tone: n > 2 s, so the key pass runs
            {
                for (int i = 0; i < 512; ++i) { const float v = std::sin ((float) ph) * 0.3f; ph += 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0; buf.setSample (0, i, v); buf.setSample (1, i, v); }
                p->processBlock (buf, midi);
            }
            std::printf ("  captured 4.0 s on a FullMix-typed channel; calling stopCapture() -> the WAV save thread runs the offline key pass\n");
            std::fflush (stdout);
            p->stopCapture();
            pump (6000);                                            // the pre-fix build never gets here: SIGBUS on the save thread
            std::printf ("  save thread finished; process alive 6 s after stopCapture\n");
            result = 0;
        }
        else if (mode == "order")
        {
            int err = 0; const auto dir = LinkShm::resolveDir (err); int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
            if (reg == nullptr) { std::printf ("registry not mappable\n"); result = 99; return; }
            plantRow (reg, dir, 0, "Z", "zzzzzzzzz1"); plantRow (reg, dir, 1, "A", "aaaaaaaaa1"); plantRow (reg, dir, 2, "M", "mmmmmmmmm1");
            p = std::make_unique<EchoJayProcessor>(); p->prepareToPlay (48000.0, 512);
            // the walk needs the heartbeat OBSERVED TO CLIMB once; bump between walks
            for (int t = 0; t < 4; ++t) { auto* sl = LinkShm::regSlots (reg); for (int i = 0; i < 3; ++i) LinkShm::storeRelease (&sl[i].heartbeat, sl[i].heartbeat + 1); p->refreshLinkRegistry(); pump (200); }
            juce::StringArray names; for (const auto& e : p->getLinkDisplayList()) names.add (e.displayName + "(slot " + juce::String (e.info.regIdx) + ")");
            std::printf ("  claimed Z (slot 0), A (slot 1), M (slot 2) -> display order: %s\n", names.joinIntoString (", ").toRawUTF8());
            const auto flat = [&]{ juce::StringArray f; for (const auto& e : p->getLinkDisplayList()) f.add (e.displayName); return f.joinIntoString (","); }();
            std::printf ("ORDER: %s -> %s\n", flat.toRawUTF8(), flat == "Z,A,M" ? "PASS (insertion order)" : flat == "A,M,Z" ? "FAIL (alphabetical)" : "FAIL");
            result = flat == "Z,A,M" ? 0 : 1;
        }
        else if (mode == "refusal")
        {
            p = std::make_unique<EchoJayProcessor>(); p->prepareToPlay (48000.0, 512);
            std::printf ("  calling borrowEngageBegin(uid, lease, structureCapable=false, inContextCapable=false) - the refusal must be logged with its false term\n");
            std::fflush (stdout);
            p->borrowEngageBegin ("refusetest01", "refusetest01-rack-1", false, false);
            pump (300);
            p->borrowRelease();
            result = 0;   // the verdict is the quoted line in this process's log (stderr), read by the runner
        }
    }
};

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("build_a_legs_test.cpp");
    juce::ScopedJuceInitialiser_GUI init;
    Control c; c.mode = argc > 1 ? argv[1] : "order";
    bool done = false; int rc = 1;
    juce::MessageManager::callAsync ([&] { c.run(); rc = c.result; c.p.reset(); done = true; });
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 60000;
    while (! done && juce::Time::getMillisecondCounterHiRes() < deadline) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
    if (! done) { std::printf ("WATCHDOG\n"); return 3; }
    return rc;
}
