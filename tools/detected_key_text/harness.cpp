// detected_key_text — COMMIT 4 proof (17 Sep 2026): the ASSEMBLED [DETECTED KEY]
// block, through the plugin's own assembly path (EchoJayEditor::
// testAssembleChainInjections -> buildDetectedKeyContext), for the SAME
// detected key with keyShowRelative OFF and then ON. Prints both blocks
// verbatim and asserts the key line reads "A minor" then "C major" — the
// rule "the prompt states the key exactly as displayed", shown not claimed.
// The source is a stored capture with an offline key reading (the highest
// precedence source), pushed through the processor's existing friend.
// Isolation: ECHOJAY_STATE_HOME set here (own_diff's shape) + HOME by the runner.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EedKeyFeed.h"
#include <cstdio>

struct EchoJayAlignTestAccess   // the friend PluginProcessor.h already declares
{
    static void pushCapture (EchoJayProcessor& p, const CaptureSnapshot& s)
    { std::lock_guard<std::mutex> l (p.snapshotMutex); p.snapshots.push_back (s); }
};

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
juce::String keyBlockOf (const juce::String& all)
{
    const int a = all.indexOf ("[DETECTED KEY");
    if (a < 0) return {};
    const int b = all.indexOf (a, "analyse:1 while audio plays.]");
    return b < 0 ? all.substring (a) : all.substring (a, b + 29);
}
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_keytext_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("detected_key_text: the assembled [DETECTED KEY] block, keyShowRelative OFF then ON\n");

    EchoJayProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    CaptureSnapshot s;
    s.id = "cap-1"; s.name = "Mix capture"; s.timestamp = juce::Time::currentTimeMillis() - 4000;
    s.durationSeconds = 12.0f; s.channelType = ChannelType::Other;
    s.keyValid = true; s.keyRoot = 9; s.keyMinor = true; s.keyConfidence = 0.31f;   // A minor, low confidence
    s.keyTuningHz = 440.0f; s.keyTuningCents = 0.0f; s.keySourceName = "Music Bus"; s.keySourcePlacement = 1;
    s.keyAltRoot = 0; s.keyAltMinor = false; s.keyAltScore = 0.29f;
    EchoJayAlignTestAccess::pushCapture (proc, s);
    check (proc.collectKeySources().primary() != nullptr, "the capture is the primary key source");

    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
    check (ed != nullptr, "the real editor constructed");
    if (ed == nullptr) return 2;

    juce::StringArray mf;
    echojay::KeyDisplayPrefs::showRelative().store (false);
    const auto offBlock = keyBlockOf (ed->testAssembleChainInjections ("what key is this in", {}, &mf));
    std::printf ("\n---- keyShowRelative = OFF ----\n%s\n", offBlock.toRawUTF8());
    proc.setKeyShowRelative (true);   // the V2 REL chip's write
    const auto onBlock = keyBlockOf (ed->testAssembleChainInjections ("what key is this in", {}, &mf));
    std::printf ("\n---- keyShowRelative = ON ----\n%s\n\n", onBlock.toRawUTF8());
    proc.setKeyShowRelative (false);

    check (offBlock.contains ("key: A minor"), "OFF: the prompt states \"key: A minor\"");
    check (onBlock.contains ("key: C major"),  "ON:  the prompt states \"key: C major\" (exactly as displayed)");
    check (onBlock.contains ("alternate: A minor"), "ON:  the alternate follows the same helper (C major -> A minor)");
    check (! offBlock.containsIgnoreCase ("unreliable") && ! offBlock.contains ("0.5") && ! offBlock.contains ("treat the key as unknown"),
           "no confidence / 0.5 / unreliable clause remains");
    check (offBlock.contains ("exactly what EchoJay displays"), "the rule line is present");

    // persisted in both plugins: the storage key is "keyShowRelative"
    juce::MemoryBlock mb; proc.setKeyShowRelative (true); proc.getStateInformation (mb); proc.setKeyShowRelative (false);
    const juce::String st = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize());
    check (st.contains ("keyShowRelative"), "V2 state carries the storage key \"keyShowRelative\"");

    std::printf ("\n==== detected_key_text: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
