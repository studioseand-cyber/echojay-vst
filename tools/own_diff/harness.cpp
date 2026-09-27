// Own-channel byte-diff harness (15 Sep 2026). Reproduces the captured baseline's
// exact conditions - a real EchoJayProcessor fed the SAME deterministic noise
// (seed 1234, 600 blocks of 512 @ 48k, 0.1 amplitude) - assembles the OWN-channel
// injection (targetLinkUid empty), and writes it to a file so it can be diffed
// against OWN_CHANNEL_BASELINE_bd4fd9a.txt (sha 18f257...e660c9). The own path was
// preserved byte-identical (early-return in buildChainLevelsInjectionCore); this
// proves it empirically rather than by assertion.
// RE-BASELINED 20 Sep 2026 (18g item 6, commit 75f5654): the own-channel [CHAIN LEVELS] line now carries
// ", loudest 3 s -X" (the input tally's max short-term) after p90 - the server's opening-gain proxy the
// ruling named ("the meters' short-term max when present"). The ONLY byte difference against bd4fd9a is
// those 19 bytes at line 7 (char 1186): OWN_CHANNEL_BASELINE_75f5654.txt, sha 5f4789...b25ce.
// RE-BASELINED AGAIN 27 Sep 2026, by ruling and after reading the diff: the whole change was the ruled short90
// token inserted after p90 on the [CHAIN LEVELS] header, one line, nothing else in 4,953 bytes. The baseline text
// is now OWN_CHANNEL_BASELINE_efdec55.txt, sha 07abba36...1bcc1e. Every
// other number (input -18.7, p10/p90 -18.5, peak -20.0, crest 4.8, heard 6s) is identical - the samples
// did not move, the text did. Sean's HOLD (20 Sep): explained, not re-baselined silently.
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
using namespace juce;

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    File tmp = File::getSpecialLocation(File::tempDirectory)
                   .getChildFile("ej_owndiff_" + String(Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    EchoJayProcessor proc;
    proc.prepareToPlay(48000.0, 512);
    Random rng(1234);
    AudioBuffer<float> buf(2, 512);
    for (int b = 0; b < 600; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = buf.getWritePointer(ch);
            for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f;
        }
        MidiBuffer midi;
        proc.processBlock(buf, midi);
    }

    std::unique_ptr<AudioProcessorEditor> edBase(proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*>(edBase.get());
    if (!ed) { std::fprintf(stderr, "HARNESS ERROR: no editor\n"); return 2; }

    StringArray mf;
    const String ownOut = ed->testAssembleChainInjections("build me a mastering chain", String(), &mf);

    const char* outPath = (argc > 1) ? argv[1] : "/tmp/own_out.txt";
    // Write LF, not the JUCE replaceWithText default of \r\n: the baseline is
    // LF-only, so a CRLF write makes the sha differ by 17 CR bytes even when the
    // assembled string is byte-identical (cost one manual CR-strip; not again).
    File(outPath).replaceWithText(ownOut, false, false, "\n");
    std::fprintf(stderr, "own-channel injection written to %s (%d bytes)\n", outPath, (int) ownOut.getNumBytesAsUTF8());
    return 0;
}
