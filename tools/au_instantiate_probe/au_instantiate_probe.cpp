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
#include <cstdlib>
#include <cstring>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);   // the HEADLESS module's registration (as ChainHost)

    juce::PluginDescription d;
    d.pluginFormatName = "AudioUnit";
    d.name             = (argc >= 2) ? juce::String::fromUTF8 (argv[1]) : juce::String();
    d.fileOrIdentifier = (argc >= 3) ? juce::String::fromUTF8 (argv[2]) : juce::String();
    // 21 Sep 2026 (owed from the item-5 scan): the FORMAT comes from the file argument - a ".vst3" bundle path (or any path that is
    // not an "AudioUnit:" identifier) is a VST3; the probe used to hard-code AudioUnit and refused every VST3 with
    // "No compatible plug-in format exists for this plug-in".
    if (d.fileOrIdentifier.endsWithIgnoreCase (".vst3") || (d.fileOrIdentifier.startsWithChar ('/') && ! d.fileOrIdentifier.startsWith ("AudioUnit:")))
        d.pluginFormatName = "VST3";
    if (argc >= 4) d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String (argv[3]).getHexValue64();

   #if defined(__x86_64__)
    const char* probeArch = "x86_64";
   #elif defined(__arm64__) || defined(__aarch64__)
    const char* probeArch = "arm64";
   #else
    const char* probeArch = "unknown";
   #endif
    std::printf ("probe: \"%s\" | %s | uid=%s | arch=%s | format=%s\n",
                 d.name.toRawUTF8(), d.fileOrIdentifier.toRawUTF8(),
                 argc >= 4 ? argv[3] : "(none)", probeArch, d.pluginFormatName.toRawUTF8());
    // 18 Sep 2026: argv[4] = a marker file the HOST polls. Touched the moment the instance exists, so a
    // crash or stall in the render check below (AMEK Mastering Compressor segfaults there, exit 139) can
    // never read as "hangs on load".
    // 21 Sep 2026 (ruling): "--list-params" as the 4th argument = LIST MODE. Prints one line per parameter
    // "index<TAB>name<TAB>label<TAB>numSteps<TAB>isDiscrete", never creates an editor, never touches the marker or any
    // EchoJay state file; wall-clock bound 30 s (EJ_PROBE_LIST_BOUND_MS overrides, for the guard); exit 0 on success, exit 3
    // on refuse/timeout with one line "refused <reason>". The signed probe is the only process allowed to list a PACE-wrapped plugin.
    // 21 Sep 2026 (round (c)): "--list-steps <index>" = STEP MODE - for one stepped parameter, walk its detents and print
    // "step<TAB>i<TAB>norm<TAB>text" (the panel text read after the AU settled on the run loop), the map builder's source for
    // `positions`. Same rules as list mode: no editor, no marker, no state file, exit 0 / exit 3 "refused <reason>".
    const bool listParams = argc >= 5 && juce::String (argv[4]) == "--list-params";
    const bool listSteps  = argc >= 6 && juce::String (argv[4]) == "--list-steps";
    const bool listMode = listParams || listSteps;
    const juce::File marker = (argc >= 5 && ! listMode) ? juce::File (juce::String::fromUTF8 (argv[4])) : juce::File();
    std::fflush (stdout);

    // 22 Sep 2026 (ruling 5): a VST3 is created from the description the FORMAT finds in the bundle (findAllTypesForFile), not
    // from the bare name + uid 0 the entries list carries - JUCE matches the class by name/uid and answered "Unable to load
    // VST-3 plug-in file" for every VST3 (0 of 66 scanned) while the leg l6 only checked the format decision.
    if (d.pluginFormatName == "VST3")
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats()) if (f->getName() == "VST3") f->findAllTypesForFile (found, d.fileOrIdentifier);
        const juce::PluginDescription* pick = nullptr;
        for (auto* c : found) if (c->name.trim().equalsIgnoreCase (d.name.trim())) { pick = c; break; }
        if (pick == nullptr && found.size() > 0) pick = found[0];
        if (pick != nullptr) { const auto keepName = d.name; d = *pick; std::printf ("vst3 class: \"%s\" (asked \"%s\", %d class(es) in the bundle)\n", d.name.toRawUTF8(), keepName.toRawUTF8(), found.size()); }
        else std::printf ("vst3 class: none found in the bundle\n");
        std::fflush (stdout);
    }
    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String err;
    bool done = false;
    fm.createPluginInstanceAsync (d, 48000.0, 512,
        [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e)
        { inst = std::move (p); err = e; done = true; });
    // the wall-clock bound: 30 s; EJ_PROBE_LIST_BOUND_MS overrides in list mode (the guard forces it; 0 = refuse at once, the
    // forced-timeout fixture). The run loop is pumped in slices no longer than the bound.
    const int boundMs = listMode && std::getenv ("EJ_PROBE_LIST_BOUND_MS") != nullptr ? juce::jmax (0, atoi (std::getenv ("EJ_PROBE_LIST_BOUND_MS"))) : 30000;
    const double tStart = juce::Time::getMillisecondCounterHiRes();
    const double slice = juce::jlimit (0.001, 0.05, boundMs / 1000.0);
    while (! done && boundMs > 0 && juce::Time::getMillisecondCounterHiRes() - tStart < (double) boundMs)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, slice, false);
    }
    if (listMode)
    {
        if (! done)          { std::printf ("refused timeout after %d ms\n", boundMs); std::fflush (stdout); std::_Exit (3); }
        if (inst == nullptr) { std::printf ("refused %s\n", err.replace ("\n", " ").toRawUTF8()); std::fflush (stdout); std::_Exit (3); }
        const auto& ps = inst->getParameters();
        const auto clean = [] (juce::String t) { return t.replace ("\t", " ").replace ("\n", " ").replace ("\r", " "); };
        std::printf ("\n");   // 21 Sep 2026: row 0 starts a line of its own - WaveShell-AU writes a banner to stdout with no trailing newline
        if (listSteps)
        {
            const int idx = atoi (argv[5]);
            auto* q = idx >= 0 && idx < ps.size() ? ps[idx] : nullptr;
            if (q == nullptr) { std::printf ("refused no parameter at index %d (%d parameters)\n", idx, ps.size()); std::fflush (stdout); std::_Exit (3); }
            const int n = q->getNumSteps();
            if (! q->isDiscrete() || n < 2 || n > 64) { std::printf ("refused not a stepped control (numSteps %d, discrete %d)\n", n, q->isDiscrete() ? 1 : 0); std::fflush (stdout); std::_Exit (3); }
            for (int i = 0; i < n; ++i)
            {
                const float norm = (float) i / (float) (n - 1);
                q->setValueNotifyingHost (norm);
                for (int k = 0; k < 8; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }   // the AU's text settles on the run loop
                std::printf ("step\t%d\t%.6f\t%s\n", i + 1, norm, clean (q->getCurrentValueAsText().trim()).toRawUTF8());
            }
            std::fflush (stdout);
            std::_Exit (0);
        }
        for (int i = 0; i < ps.size(); ++i)
        {
            auto* q = ps[i]; if (q == nullptr) continue;
            std::printf ("%d\t%s\t%s\t%d\t%d\n", i, clean (q->getName (128)).toRawUTF8(), clean (q->getLabel()).toRawUTF8(), q->getNumSteps(), q->isDiscrete() ? 1 : 0);
        }
        std::fflush (stdout);
        std::_Exit (0);   // a third-party AU's teardown is not this mode's subject
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
