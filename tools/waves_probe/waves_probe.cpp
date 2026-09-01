// waves_probe: does a WaveShell-hosted Waves AU instantiate in a JUCE host
// on THIS machine, pass audio, and create an editor? Ground truth for the
// "empty pane" investigation (2 Sep 2026) — runs with the REAL home so Waves
// licensing resolves; no EchoJay state is written.
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes Point
#include <JuceHeader.h>
#include <cstdio>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);   // the project's registration

    juce::PluginDescription d;
    d.name             = "Abbey Road Chambers (s)";
    d.pluginFormatName = "AudioUnit";
    d.fileOrIdentifier = "AudioUnit:Effects/aufx,STES,ksWV";
    d.uniqueId         = (int) 0x5952747d;
    d.manufacturerName = "Waves";
    if (argc >= 3) { d.pluginFormatName = argv[1]; d.fileOrIdentifier = argv[2]; }

    std::printf ("probe: %s | %s\n", d.pluginFormatName.toRawUTF8(),
                 d.fileOrIdentifier.toRawUTF8());

    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String err;
    bool done = false;
    fm.createPluginInstanceAsync (d, 48000.0, 512,
        [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e)
        { inst = std::move (p); err = e; done = true; });
    for (int i = 0; i < 600 && ! done; ++i)   // up to ~30s
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    }
    if (! done)          { std::printf ("RESULT: TIMED OUT creating instance\n"); return 2; }
    if (inst == nullptr) { std::printf ("RESULT: FAILED to instantiate: %s\n",
                                        err.toRawUTF8()); return 1; }
    std::printf ("instantiated: \"%s\" latency=%d\n",
                 inst->getName().toRawUTF8(), inst->getLatencySamples());

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
        juce::AudioBuffer<float> in;
        in.makeCopyOf (buf);
        inst->processBlock (buf, midi);
        for (int i = 0; i < 512; ++i)
        {
            inSum  += std::abs (in.getSample (0, i));
            outSum += std::abs (buf.getSample (0, i));
            diff   += std::abs (buf.getSample (0, i) - in.getSample (0, i));
        }
    }
    std::printf ("audio: inSum=%.1f outSum=%.1f diff=%.1f -> %s\n",
                 inSum, outSum, diff,
                 outSum < 1e-6 ? "DEAD SHELL (silent)"
                 : diff  < 1e-6 ? "BIT-EXACT PASSTHROUGH (not processing)"
                                : "PROCESSING (alive)");

    juce::AudioProcessorEditor* ed = nullptr;
    try { ed = inst->createEditorIfNeeded(); } catch (...) {}
    std::printf ("editor: %s", ed == nullptr ? "FAILED to create\n" : "created ");
    if (ed != nullptr)
    {
        std::printf ("(%dx%d)\n", ed->getWidth(), ed->getHeight());
        inst->editorBeingDeleted (ed);
        delete ed;
    }
    inst.reset();
    std::printf ("RESULT: OK\n");
    return 0;
}
