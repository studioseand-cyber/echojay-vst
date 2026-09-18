// au_instantiate_probe: the smallest possible answer to ONE question —
// does a specific AU instantiate AND initialise (prepareToPlay ->
// AudioUnitInitialize) headless in THIS project's HEADLESS AU host, on THIS
// machine? Ground truth for whether the Link rack-switch crash (16 Sep 2026)
// is bench-reproducible or in-host only. Runs with the REAL home so iLok/
// Softube licensing resolves; writes no EchoJay state.
//
//   au_instantiate_probe "<name>" "<AudioUnit:...identifier...>" <uidHex>
//
// Reports, per plugin: INSTANTIATE ok/fail, PREPARE (initialise) ok/fail via a
// few process blocks (alive/silent/passthrough). A hang on a licensing dialog
// is the caller's job to time out and kill — this program never dismisses one.
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes Point
#include <JuceHeader.h>
#include <cstdio>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);   // the HEADLESS module's registration (as ChainHost)

    juce::PluginDescription d;
    d.pluginFormatName = "AudioUnit";
    d.name             = (argc >= 2) ? juce::String::fromUTF8 (argv[1]) : juce::String();
    d.fileOrIdentifier = (argc >= 3) ? juce::String::fromUTF8 (argv[2]) : juce::String();
    if (argc >= 4) d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String (argv[3]).getHexValue64();

   #if defined(__x86_64__)
    const char* probeArch = "x86_64";
   #elif defined(__arm64__) || defined(__aarch64__)
    const char* probeArch = "arm64";
   #else
    const char* probeArch = "unknown";
   #endif
    std::printf ("probe: \"%s\" | %s | uid=%s | arch=%s\n",
                 d.name.toRawUTF8(), d.fileOrIdentifier.toRawUTF8(),
                 argc >= 4 ? argv[3] : "(none)", probeArch);
    // 18 Sep 2026: argv[4] = a marker file the HOST polls. Touched the moment the instance exists, so a
    // crash or stall in the render check below (AMEK Mastering Compressor segfaults there, exit 139) can
    // never read as "hangs on load".
    const juce::File marker = (argc >= 5) ? juce::File (juce::String::fromUTF8 (argv[4])) : juce::File();
    std::fflush (stdout);

    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String err;
    bool done = false;
    fm.createPluginInstanceAsync (d, 48000.0, 512,
        [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e)
        { inst = std::move (p); err = e; done = true; });
    for (int i = 0; i < 600 && ! done; ++i)   // internal ~30s cap
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    }
    if (! done)          { std::printf ("RESULT: TIMED OUT creating instance\n"); return 2; }
    if (inst == nullptr) { std::printf ("INSTANTIATE: FAIL (%s)\nRESULT: FAIL\n", err.toRawUTF8()); return 1; }
    std::printf ("INSTANTIATE: OK  name=\"%s\" latency=%d\n",
                 inst->getName().toRawUTF8(), inst->getLatencySamples());
    std::fflush (stdout);
    if (marker != juce::File()) marker.create();

    // PREPARE == the initialise that matters for the crash path: the headless
    // AU host's prepareToPlay calls AudioUnitInitialize. Then render a few
    // blocks to prove it is a live, initialised AU (and does not fault).
    inst->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    juce::Random rng (7);
    double inSum = 0, outSum = 0, diff = 0;
    for (int b = 0; b < 40; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                buf.setSample (ch, i, rng.nextFloat() * 0.5f - 0.25f);
        juce::AudioBuffer<float> in; in.makeCopyOf (buf);
        inst->processBlock (buf, midi);
        for (int i = 0; i < 512; ++i)
        {
            inSum  += std::abs (in.getSample (0, i));
            outSum += std::abs (buf.getSample (0, i));
            diff   += std::abs (buf.getSample (0, i) - in.getSample (0, i));
        }
    }
    std::printf ("PREPARE+RENDER: OK  inSum=%.1f outSum=%.1f diff=%.1f -> %s\n",
                 inSum, outSum, diff,
                 outSum < 1e-6 ? "silent" : diff < 1e-6 ? "passthrough" : "processing(alive)");
    inst->releaseResources();
    inst.reset();
    std::printf ("RESULT: OK\n");
    return 0;
}
