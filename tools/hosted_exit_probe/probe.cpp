// hosted_exit_probe (27 Oct 2026): ONE question, with no EchoJay chain in the picture —
//
//   if a third-party AU instance is still ALIVE when this process leaves, does the door we leave by decide
//   whether the process dies?
//
// Why it exists. Eleven host_rig reports between 26 Oct 23:50 and 27 Oct 01:39 are one crash identity:
//   EXC_BAD_ACCESS (SIGSEGV) at 0x0, faulting thread named "JUCE Timer", frames
//     pthread_mutex_lock < ValhallaDelay+0x1e3a38 < ValhallaDelay+0x1e5ac8 < ValhallaDelay+0x1c2874
//       < _pthread_start < thread_start
// and in every one of them the MAIN thread is
//     start < dyld4::...::exit(int) < exit < __cxa_finalize_ranges < <a static destructor>
// i.e. the process had already left main and was running static destructors. The report was first written up as
// "deleting a track with ValhallaDelay hosted takes the process down". That attribution was wrong: the rig's own
// log puts the verdict at 01:39:26.392, the borrow-pool teardown line at 01:39:26.4xx and the crash capture at
// 01:39:26.602 - after every row, on the way out.
//
// What is going on is the plugin's OWN statically linked JUCE: its timer thread is still running when
// __cxa_finalize destroys the statics in its image, so it locks a mutex that is no longer there. Our teardown
// cannot be the cause, because our teardown does not destroy the instance at all - ChainHost::~ChainHost logs
// "teardown KEPT 3 hosted plugin instance(s) alive on purpose", the ruled answer to the Softube AP_Close abort
// (tools/teardown_dispose_guard). The instance, and therefore its threads, are alive BY DESIGN at that point.
//
// So the only thing we own here is the DOOR: a host tool of ours that hosts a third-party AU must leave by
// _Exit (no static destruction), the way tools/au_instantiate_probe already does. This probe is the evidence for
// that rule, in both directions, and it contains no ChainHost, no EchoJayProcessor and no editor:
//
//   probe "<name>" "<identifier>" <uidHex> --door return     leaves by returning from main  (expected: it dies)
//   probe "<name>" "<identifier>" <uidHex> --door fastexit    leaves by std::_Exit(0)       (expected: clean)
//
// Either way it instantiates, prepares, renders 64 blocks and pumps the run loop for 300 ms first, so the
// plugin's own threads are up and running when the door is used. It never creates an editor: the point is that
// the editor is not in this story.
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes Point
#include <JuceHeader.h>
#include "EJPaceCheck.h"
#include <cstdio>
#include <cstdlib>

// THE LINGER, and why the probe needs it. The first run of this probe did NOT reproduce the crash on any door:
// leaving by return was clean 3 of 3. The reason is in the crash reports - when ValhallaDelay's timer thread
// faulted, the main thread was not finishing exit(), it was STUCK inside one:
//     __cxa_finalize_ranges -> wvWavesV15_0_70_71::ServerClient::WCBIClientWithWLSThread::~WCBIClientWithWLSThread
//                           -> stop_thread() -> std::thread::join -> _pthread_join
// WaveShell 15.0 was loaded in that process and its static destructor joins a reporting thread, which takes
// time. dyld runs an image's terminators in reverse load order, so ValhallaDelay's statics - it was loaded
// later - are already gone while main is still in that join. THAT is the window the plugin's own timer thread
// dies in. A process that leaves instantly never opens it.
//
// So the known-bad door models the rig's shape exactly: a static destructor registered EARLY (before any
// plugin image is loaded, therefore run LAST) that takes its time. EJ_PROBE_EXIT_LINGER_MS sets how long;
// 1500 ms is the default and well under what the WaveShell join cost.
struct ExitLinger
{
    bool armed = false;
    ~ExitLinger()
    {
        if (! armed) return;
        const int ms = std::getenv ("EJ_PROBE_EXIT_LINGER_MS") != nullptr
                         ? atoi (std::getenv ("EJ_PROBE_EXIT_LINGER_MS")) : 1500;
        std::printf ("linger: a static destructor is taking %d ms, with the plugin's threads still running\n", ms);
        std::fflush (stdout);
        juce::Thread::sleep (ms);
    }
};
static ExitLinger g_linger;   // registered first, destroyed last: the same place WaveShell's is

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);

    juce::PluginDescription d;
    d.pluginFormatName = "AudioUnit";
    d.name             = (argc >= 2) ? juce::String::fromUTF8 (argv[1]) : juce::String();
    d.fileOrIdentifier = (argc >= 3) ? juce::String::fromUTF8 (argv[2]) : juce::String();
    if (argc >= 4) d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String (argv[3]).getHexValue64();

    juce::String door = "return";
    // --editor <n>: open and close the plugin's own editor n times before leaving, the way the rig does. The
    // first run of this probe never created one and never reproduced the crash; a plugin's timer thread only
    // exists once something in it starts a juce::Timer, and its editor is the usual thing that does.
    // --keep-editor: leave the last one open, so a live Timer is registered at the door.
    // --render <n>: n blocks of noise through the plugin. OFF BY DEFAULT, and it stays off: this probe
    // hands the plugin a plain stereo buffer, which is not a layout - FabFilter Pro-C 3 (a music effect with
    // a sidechain bus) walked off the end of it inside AudioUnitRender, in the headless host's
    // renderGetInputCallback, exactly where AMEK's Mastering Compressor took au_instantiate_probe's render
    // step down on 18 Sep. "JUCE pulls every declared bus" is the rule, and giving a plugin a correct layout
    // is ChainHost's job, not a door probe's. The door does not need audio: the EDITOR is what starts the
    // plugin's timers, and that is the thread this probe is about.
    int editorOpens = 0; bool keepEditor = false; int renderBlocks = 0;
    for (int i = 4; i < argc; ++i)
    {
        const juce::String a (juce::String::fromUTF8 (argv[i]));
        if (a == "--door" && i + 1 < argc) door = juce::String::fromUTF8 (argv[i + 1]);
        if (a == "--editor" && i + 1 < argc) editorOpens = atoi (argv[i + 1]);
        if (a == "--keep-editor") keepEditor = true;
        if (a == "--render" && i + 1 < argc) renderBlocks = atoi (argv[i + 1]);
    }
    if (door != "return" && door != "fastexit")
    { std::printf ("refused unknown door \"%s\"\n", door.toRawUTF8()); return 3; }

    std::printf ("probe: \"%s\" door=%s\n", d.name.toRawUTF8(), door.toRawUTF8());
    std::fflush (stdout);

    // The same rule as every other unsigned harness process on this Mac: never load a PACE-wrapped bundle.
    if (std::getenv ("EJ_PROBE_ALLOW_PACE") == nullptr)
        if (const auto why = echojay::refuseIfPaceWrapped (d); why.isNotEmpty())
        { std::printf ("refused %s\n", why.toRawUTF8()); std::fflush (stdout); return 3; }

    // Resolve the real description from the identifier, the way host_rig builds its catalogue - name + uid alone
    // is JUCE matching by hand and it answers "no compatible plug-in format" for anything it gets slightly wrong.
    {
        juce::OwnedArray<juce::PluginDescription> types;
        for (auto* fmt : fm.getFormats())
            if (fmt->getName() == d.pluginFormatName) fmt->findAllTypesForFile (types, d.fileOrIdentifier);
        const juce::PluginDescription* pick = nullptr;
        for (auto* c : types) if (c->name.trim().equalsIgnoreCase (d.name.trim())) { pick = c; break; }
        if (pick == nullptr && types.size() > 0) pick = types[0];
        if (pick == nullptr) { std::printf ("INSTANTIATE: FAIL (nothing at that identifier)\nRESULT: FAIL\n"); std::fflush (stdout); return 1; }
        d = *pick;
    }

    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst (fm.createPluginInstance (d, 48000.0, 512, err));
    if (inst == nullptr)
    { std::printf ("INSTANTIATE: FAIL (%s)\nRESULT: FAIL\n", err.toRawUTF8()); std::fflush (stdout); return 1; }
    std::printf ("INSTANTIATE: OK name=\"%s\"\n", inst->getName().toRawUTF8());
    std::fflush (stdout);

    inst->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buf ((int) juce::jmax (2, inst->getTotalNumOutputChannels()), 512);
    juce::MidiBuffer midi;
    juce::Random rnd (1);
    for (int b = 0; b < renderBlocks; ++b)
    {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int s = 0; s < buf.getNumSamples(); ++s)
                buf.setSample (ch, s, 0.05f * (rnd.nextFloat() * 2.0f - 1.0f));
        inst->processBlock (buf, midi);
    }
    // Let the plugin's own threads settle into their normal running state - that is the state we are leaving from.
    const auto until = juce::Time::getMillisecondCounter() + 300;
    while (juce::Time::getMillisecondCounter() < until)
    { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }

    // The editor, opened and closed in its own window the way a host does it.
    for (int e = 0; e < editorOpens; ++e)
    {
        auto* ed = inst->createEditorIfNeeded();
        if (ed == nullptr) { std::printf ("editor: this plugin has none\n"); break; }
        auto* w = new juce::DocumentWindow (inst->getName(), juce::Colours::black, juce::DocumentWindow::closeButton, true);
        w->setContentNonOwned (ed, true);
        w->setVisible (true);
        const auto t1 = juce::Time::getMillisecondCounter() + 250;
        while (juce::Time::getMillisecondCounter() < t1)
        { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
        const bool last = (e == editorOpens - 1);
        if (last && keepEditor) { std::printf ("editor: left open (%d opened)\n", e + 1); break; }
        w->clearContentComponent();
        delete w;
        inst->editorBeingDeleted (ed);
        delete ed;
        const auto t2 = juce::Time::getMillisecondCounter() + 150;
        while (juce::Time::getMillisecondCounter() < t2)
        { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
    }
    if (editorOpens > 0 && ! keepEditor) std::printf ("editor: opened and closed %d time(s)\n", editorOpens);
    std::fflush (stdout);

    // THE INSTANCE IS LEFT ALIVE ON PURPOSE. That is what ChainHost::~ChainHost does with a hosted third-party
    // plugin, so it is the state the rig is in when it leaves. Nothing below destroys it.
    (void) inst.release();

    std::printf ("RESULT: OK (leaving by %s with the instance alive)\n", door.toRawUTF8());
    std::fflush (stdout);
    if (door == "fastexit")
        std::_Exit (0);   // no static destruction: the plugin's threads never see their statics vanish
    g_linger.armed = true;
    return 0;             // exit() -> __cxa_finalize_ranges -> the plugin's statics die under its own threads
}
