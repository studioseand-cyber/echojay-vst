// au_instantiate_probe: the smallest possible answer to ONE question —
// does a specific AU instantiate AND initialise (prepareToPlay ->
// AudioUnitInitialize) headless in THIS project's HEADLESS AU host, on THIS
// machine? Ground truth for whether the Link rack-switch crash (16 Sep 2026)
// is bench-reproducible or in-host only. Runs with the REAL home so iLok/
// Softube licensing resolves; writes no EchoJay state.
//
//   au_instantiate_probe "<name>" "<AudioUnit:...identifier...>" <uidHex>
//
// Reports, per plugin: INSTANTIATE ok/fail (the render step was removed 18 Sep 2026; formerly PREPARE+RENDER via a
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

    // 18 Sep 2026: the probe's job is LOAD hangs. It used to prepare and render 40 blocks after this point; AMEK
    // Mastering Compressor segfaulted there (renderGetInput inside the headless AU host's input callback, exit 139,
    // both architectures) and in-host that step stalled to the bound and read as "hangs on load". There is no stated
    // reason to render: the verdict is the instance. Stop here.
    inst.reset();
    std::printf ("RESULT: OK\n");
    return 0;
}
