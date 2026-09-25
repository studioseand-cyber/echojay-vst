// smoke_body (19 Sep 2026): assemble the REAL client request bodies for the live smoke, through the editor's own
// assembler (testAssembleChainInjections), with the chain-input tally fed a -15.5 LUFS programme first so the
// [CHAIN LEVELS] block carries a measured integrated figure. Writes one file per typed turn.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
using namespace juce;
int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    File tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("ej_smoke_" + String (Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    // a COPY of the real scan/state so the assembler has the real feed, and nothing live is written
    const File realAs = File ("/Users/SeanD/Library/Application Support/EchoJay"), realEj = File ("/Users/SeanD/Library/EchoJay");
    realAs.copyDirectoryTo (tmp.getChildFile ("Library/Application Support/EchoJay"));
    realEj.copyDirectoryTo (tmp.getChildFile ("Library/EchoJay"));
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    // feed noise, then scale it to -15.5 LUFS at the chain input (two passes, like loudness_loop_guard's calibrate)
    Random rng (1234); AudioBuffer<float> buf (2, 512); float amp = 0.1f;
    auto feed = [&] (int blocks) { for (int b = 0; b < blocks; ++b) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * amp; } MidiBuffer m; proc.processBlock (buf, m); } };
    feed (600); const float have = proc.getChainHost().getChainInLevels().levelDb; amp *= std::pow (10.0f, (-15.5f - have) / 20.0f);
    proc.getChainHost().resetAllLevels(); feed (1200);
    std::fprintf (stderr, "chain input calibrated: %.2f LUFS\n", proc.getChainHost().getChainInLevels().levelDb);
    std::unique_ptr<AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    // wait for the scan to land and the recommendable feed to build (the real client has it before any build turn)
    { const double t0 = Time::getMillisecondCounterHiRes();
      while (proc.getChainHost().getRecommendableCount() == 0 && Time::getMillisecondCounterHiRes() - t0 < 120000.0)
      { Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false); }
      std::fprintf (stderr, "recommendable feed: %d names after %.0f ms\n", proc.getChainHost().getRecommendableCount(), Time::getMillisecondCounterHiRes() - t0); }
    const String outDir = argc > 1 ? String (argv[1]) : String ("/tmp");
    struct T { const char* file; const char* typed; const char* meter; };
    const T turns[] = {
        { "smoke_master.txt", "build me a modern loud hip hop master chain", "\n\n[LIVE METER: Mix Bus (Hip-Hop)] Int -15.5 LUFS | Mom -14.9 | ST -15.2 | LRA 6.1 LU | RMS -18.0/-18.0 | TP -2.8/-2.8 | Cres 12.4" },
        { "smoke_vocal.txt",  "add a little compression to the vocal",        "\n\n[LIVE METER: Lead Vocal (Hip-Hop)] Int -20.3 LUFS | Mom -19.8 | ST -20.1 | LRA 7.0 LU | RMS -23.0/-23.0 | TP -6.1/-6.1 | Cres 14.0" },
        { "smoke_kick.txt",   "build me a kick chain",                         "\n\n[LIVE METER: Kick (Hip-Hop)] Int -16.0 LUFS | Mom -15.5 | ST -15.8 | LRA 3.0 LU | RMS -20.0/-20.0 | TP -3.0/-3.0 | Cres 17.0" },
    };
    for (const auto& t : turns)
    {
        StringArray missing;
        const String body = ed->testAssembleChainInjections (t.typed, String(), &missing) + t.meter;
        File (outDir + "/" + t.file).replaceWithText (body, false, false, "\n");
        std::fprintf (stderr, "%s: %d bytes (missing: %s)\n", t.file, (int) body.getNumBytesAsUTF8(), missing.joinIntoString (",").toRawUTF8());
    }
    return 0;
}
