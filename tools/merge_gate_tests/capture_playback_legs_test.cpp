/*  CAPTURE / PLAYBACK / BUILD-B LEGS (8 Sep 2026 ruling). Each mode runs against the archive it is compiled with:
    the worktree at a21db44 (Build A2 = the installed binary) is the NEGATIVE CONTROL, the working tree the fix.
    legs_new_api.h exists only in the working tree, so new-API assertions compile only there.
      capture   45 s capture at 48 k / 512 with the transport rolling, then a STOPPED block; every processBlock is timed.
                Assert: max block time (whole capture INCLUDING the stop block) < 1,000 us; the last-10 s median is not
                above 1.5 x the first-10 s median (no upward trend); the final block < 1,000 us. New API: 0 overruns.
      playback  a 60 s WAV; press (loadABFile as the chat does) -> first audible output block; pause; resume the way the
                CHAT does (new: resumeAB; old: loadABFile(path, pausedOffset)) -> first audible; assert both <= 100 ms and
                that resume continues from the paused position (phase check against the file).
      buildb    engage a borrow with the budget OFF (refused on budget), then make WANTED ON and commit at a STOPPED block:
                the lease's muteOut must turn Y (in-context restored) within two renews. Old build: stays N.             */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "PluginProcessor.h"
#include "LinkShm.h"
#include "ChainHost.h"
#if __has_include("legs_new_api.h")
#include "legs_new_api.h"
#endif
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <unistd.h>

static void pump (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.002, false); }
struct TestPlayHead : public juce::AudioPlayHead
{
    bool playing = true; juce::int64 samples = 0;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setIsPlaying (playing); p.setTimeInSamples (samples); p.setTimeInSeconds ((double) samples / 48000.0); return p; }
};
static double usNow() { return std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double median (std::vector<double> v) { if (v.empty()) return 0; std::sort (v.begin(), v.end()); return v[v.size() / 2]; }

struct Control
{
    juce::String mode; std::unique_ptr<EchoJayProcessor> p; int result = 1;
    void run()
    {
        p = std::make_unique<EchoJayProcessor>(); p->prepareToPlay (48000.0, 512);
        TestPlayHead ph; p->setPlayHead (&ph);
        if (mode == "capture")
        {
            p->setChannelType (ChannelType::LeadVocal);       // no key pass at stop: the leg measures the audio thread, not the save thread
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; double phase = 0;
            const int blocks = 48000 * 45 / 512; std::vector<double> t; t.reserve ((size_t) blocks + 2);
            ph.playing = true; ph.samples = 0;
            p->startCapture();
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 512; ++i) { const float v = std::sin ((float) phase) * 0.3f; phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0; buf.setSample (0, i, v); buf.setSample (1, i, v); }
                const double a = usNow(); p->processBlock (buf, midi); t.push_back (usNow() - a); ph.samples += 512;
#ifdef EJ_LEGS_NEW_API
                if (b % 94 == 0) p->serviceCaptureStop();   // what the 1 Hz timer does, at the AUDIO's 1 s cadence (45 s of audio takes ~3 s of wall time here)
#else
                if (b % 94 == 0) pump (1);
#endif
            }
            ph.playing = false;                           // the transport STOPS: the next block is the one that used to run stopCapture
            const double a = usNow(); p->processBlock (buf, midi); const double stopBlock = usNow() - a; t.push_back (stopBlock);
#ifdef EJ_LEGS_NEW_API
            for (int i = 0; i < 50; ++i) { pump (20); p->serviceCaptureStop(); }
            const int overrun = p->getWaveformRecorder().getOverrunSamples();
#else
            pump (200); const int overrun = -1;
#endif
            const double maxUs = *std::max_element (t.begin(), t.end()); const int maxAt = (int) (std::max_element (t.begin(), t.end()) - t.begin());
            std::printf ("  max block at index %d (%.1f s), first block %.0f us\n", maxAt, maxAt * 512.0 / 48000.0, t[0]);
            std::vector<double> first (t.begin(), t.begin() + 937), last (t.end() - 938, t.end() - 1);
            const double m1 = median (first), m2 = median (last);
            auto it30 = t.begin() + (int) (30.0 * 48000 / 512); const double around30 = *std::max_element (it30 - 100, std::min (t.end(), it30 + 100));
            std::printf ("  blocks %d: max %.0f us  median first10s %.1f us  last10s %.1f us (ratio %.2f)  max around 30 s %.0f us  STOP block %.0f us  overrun %d\n",
                         (int) t.size(), maxUs, m1, m2, m1 > 0 ? m2 / m1 : 0, around30, stopBlock, overrun);
            const bool ok = maxUs < 1000.0 && (m1 <= 0 || m2 <= 1.5 * m1) && stopBlock < 1000.0 && overrun <= 0;
            std::printf ("CAPTURE: max %s  trend %s  stop-block %s  overrun %s -> %s\n", maxUs < 1000 ? "PASS" : "FAIL", (m1 <= 0 || m2 <= 1.5 * m1) ? "PASS" : "FAIL", stopBlock < 1000 ? "PASS" : "FAIL", overrun <= 0 ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
            result = ok ? 0 : 1;
        }
        else if (mode == "playback")
        {
            int err = 0; const auto dir = LinkShm::resolveDir (err); juce::File wav (dir + "playback_leg.wav");
            {   // 60 s, 220 Hz left / 330 Hz right, so the phase after a resume can be checked against the file
                juce::WavAudioFormat fmt; std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (wav), 48000.0, 2, 16, {}, 0));
                juce::AudioBuffer<float> b (2, 48000); double pl = 0, pr = 0;
                for (int s = 0; s < 60; ++s) { for (int i = 0; i < 48000; ++i) { b.setSample (0, i, std::sin ((float) pl) * 0.5f); b.setSample (1, i, std::sin ((float) pr) * 0.5f); pl += 2 * juce::MathConstants<double>::pi * 220 / 48000; pr += 2 * juce::MathConstants<double>::pi * 330 / 48000; } w->writeFromAudioSampleBuffer (b, 0, 48000); }
            }
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            auto firstAudible = [&](int maxBlocks, int& blocksOut) -> double { const double t0 = juce::Time::getMillisecondCounterHiRes(); for (int b = 0; b < maxBlocks; ++b) { buf.clear(); p->processBlock (buf, midi); ph.samples += 512; if (buf.getRMSLevel (0, 0, 512) > 0.1f) { blocksOut = b; return juce::Time::getMillisecondCounterHiRes() - t0; } } blocksOut = -1; return -1; };
            ph.playing = false;
            const double c0 = juce::Time::getMillisecondCounterHiRes(); p->loadABFile (wav.getFullPathName(), 0.0); const double loadMs = juce::Time::getMillisecondCounterHiRes() - c0;
            int nb = -1; const double cold = firstAudible (2000, nb); const double coldTotal = loadMs + cold;
            for (int b = 0; b < 200; ++b) { p->processBlock (buf, midi); }     // ~2.1 s in
            p->pauseAB();
            const double pausedSec = (double) p->abPlaybackPos / 48000.0;
            pump (300);
            const double r0 = juce::Time::getMillisecondCounterHiRes();
#ifdef EJ_LEGS_NEW_API
            p->resumeAB();                                                     // what the chat now does for a paused file
#else
            p->loadABFile (wav.getFullPathName(), pausedSec);                  // what the chat did: re-read from the offset
#endif
            const double resumeLoad = juce::Time::getMillisecondCounterHiRes() - r0;
            int nb2 = -1; const double resume = firstAudible (2000, nb2); const double resumeTotal = resumeLoad + resume;
            // continuity: the first output sample after resume should be the file at pausedSec (220 Hz phase)
            const float got = buf.getSample (0, 0); const double expectPhase = 2 * juce::MathConstants<double>::pi * 220 * pausedSec; const float expect = std::sin ((float) expectPhase) * 0.5f;
            const bool continuous = std::abs (got - expect) < 0.15f;
            std::printf ("  cold: load %.0f ms + %d blocks -> first audible %.0f ms total;  paused at %.2f s;  resume: %.0f ms + %d blocks -> %.0f ms total; first sample %.3f vs file %.3f -> %s\n",
                         loadMs, nb, coldTotal, pausedSec, resumeLoad, nb2, resumeTotal, got, expect, continuous ? "continuous" : "NOT continuous");
            const bool ok = coldTotal >= 0 && coldTotal <= 100 && resumeTotal >= 0 && resumeTotal <= 100 && continuous;
            std::printf ("PLAYBACK: cold %s (%.0f ms)  resume %s (%.0f ms)  continuity %s -> %s\n", coldTotal <= 100 && coldTotal >= 0 ? "PASS" : "FAIL", coldTotal, resumeTotal <= 100 && resumeTotal >= 0 ? "PASS" : "FAIL", resumeTotal, continuous ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
            p->stopAB(); result = ok ? 0 : 1;
        }
        else if (mode == "buildb")
        {
            int err = 0; const auto dir = LinkShm::resolveDir (err); int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
            if (reg == nullptr) { std::printf ("registry not mappable\n"); result = 99; return; }
            const juce::String uid = "bbleg000001";
            p->refreshLinkRegistry(); pump (200);   // the product opens its registry on its first timer tick; do it before the engage so the lease has a directory
            // budget OFF at prepare (no capable row yet) -> the engage is refused on the budget
            p->borrowEngageBegin (uid, uid + "-rack-1", false, true);
            std::printf ("  lease path %s exists=%d\n", LinkShm::leasePath (dir, uid).toRawUTF8(), (int) juce::File (LinkShm::leasePath (dir, uid)).existsAsFile());
            pump (400);
            auto leaseMuteOut = [&]{ const auto v = juce::JSON::parse (juce::File (LinkShm::leasePath (dir, uid))); return v.isObject() && (bool) v.getProperty ("muteOut", false); };
            const bool before = leaseMuteOut();
            // now a counted row: registry row + sidecar naming THIS host identity as publisher and host
            auto* sl = LinkShm::regSlots (reg); auto& s = sl[0]; std::memset (&s, 0, sizeof (s)); std::strncpy (s.displayName, "BB", 39); std::snprintf (s.audioFile, 47, "audio_%s.bin", uid.toRawUTF8()); std::strncpy (s.instanceUid, uid.toRawUTF8(), 11); s.sampleRate = 48000.f; s.numChannels = 2; s.heartbeat = 1; LinkShm::storeRelease (&s.inUse, 1u);
            LinkShm::RackSidecar rc; rc.uid = uid; rc.inContextCapable = true; rc.muteSoloCapable = true; const auto& me = ChainHost::getHostIdentity(); rc.publisherPid = (int) getpid(); rc.hostPid = me.pid; rc.hostStartSec = me.startSec; rc.hostStartUsec = me.startUsec;   // the host identity the product resolved for THIS process LinkShm::writeRackSidecar (dir, rc);
            std::printf ("  sidecar path %s exists=%d\n", LinkShm::rackSidecarPath (dir, uid).toRawUTF8(), (int) juce::File (LinkShm::rackSidecarPath (dir, uid)).existsAsFile());
            for (int t = 0; t < 4; ++t) { LinkShm::storeRelease (&sl[0].heartbeat, sl[0].heartbeat + 1); p->refreshLinkRegistry(); pump (300); }   // seen to climb -> WANTED ON
            { const auto back = LinkShm::readRackSidecar (dir, uid); int listed = 0; for (const auto& si : p->getLinkSlotInfos()) if (si.uid == uid) ++listed;
              std::printf ("  planted row listed by the main: %d; sidecar read back uid=%s capable=%d publisherPid=%d hostPid=%d hostStart=%lld/%lld (me pid=%d start=%lld/%lld)\n",
                           listed, back.uid.toRawUTF8(), (int) back.inContextCapable, back.publisherPid, back.hostPid, (long long) back.hostStartSec, (long long) back.hostStartUsec, me.pid, (long long) me.startSec, (long long) me.startUsec); }
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; ph.playing = false; p->processBlock (buf, midi);   // a STOPPED block: the budget commits
            double tOk = -1; const double t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 300 && tOk < 0; ++i) { pump (20); if (leaseMuteOut()) tOk = juce::Time::getMillisecondCounterHiRes() - t0; }
            std::printf ("  lease muteOut before the commit: %d; after the STOPPED-block commit: %s (%.0f ms)\n", (int) before, tOk >= 0 ? "Y" : "N", tOk);
            const bool ok = ! before && tOk >= 0;
            std::printf ("BUILDB: refused-then-restored %s -> %s\n", ok ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
            p->borrowRelease (false); result = ok ? 0 : 1;
        }
    }
};

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("capture_playback_legs_test.cpp");
    juce::ScopedJuceInitialiser_GUI init;
    Control c; c.mode = argc > 1 ? argv[1] : "capture";
    bool done = false; int rc = 1;
    juce::MessageManager::callAsync ([&] { c.run(); rc = c.result; c.p.reset(); done = true; });
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 300000;
    while (! done && juce::Time::getMillisecondCounterHiRes() < deadline) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
    if (! done) { std::printf ("WATCHDOG\n"); return 3; }
    return rc;
}
