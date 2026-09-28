// detected_key_text — COMMIT 4 proof + COMMIT 4b guard (17 Sep 2026): the ASSEMBLED [DETECTED KEY]
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
    // 21t-j (28 Sep 2026): THE MARKER IS "[KEY", not "[DETECTED KEY". B asked for the block to be split into
    // [KEY] - the one selected source, the authority - and [KEY CANDIDATES] - other channels' readings, for
    // context only - and it was, this round. This guard was still slicing on the old marker and so was reading
    // an EMPTY string and failing every assertion about its contents. The em dash pins the [KEY] block itself,
    // because "[KEY CANDIDATES" also begins with "[KEY".
    const int a = all.indexOf (juce::String::fromUTF8 ("[KEY \xe2\x80\x94"));
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
    // COMMIT 4b (17 Sep 2026) said the model must never see the confidence number. SUPERSEDED, 27 Sep 2026
    // (the 21t-i re-cut ruling): the block names ONE source and calls it the authority, and a server told to use
    // one reading and nothing else needs to know how good that reading is. What 4b was really protecting against
    // was a MENU of readings to choose from, and that is what [KEY CANDIDATES] now carries separately.
    check (offBlock.contains ("confidence:"),
           "21t-i re-cut: the selected source's confidence IS carried (4b's rule superseded)",
           offBlock.fromFirstOccurrenceOf ("confidence:", true, false).substring (0, 18).trim());
    check (offBlock.contains ("alternate: C major\n"), "4b: the alternate line is the NAME only (no score)");
    // 21t-j: the field is "ref_hz:" now, with the cents clause B asked for; "detected_tuning:" was its name
    // before the [KEY] split.
    check (offBlock.contains ("ref_hz:") && offBlock.contains ("cents from A=440")
           && offBlock.contains ("root_hz:") && offBlock.contains ("source:") && offBlock.contains ("age:"),
           "21t-j: key, ref_hz (with cents), root_hz, source, age kept",
           offBlock.fromFirstOccurrenceOf ("ref_hz:", true, false).substring (0, 40).trim());
    check (! offBlock.containsIgnoreCase ("unreliable") && ! offBlock.containsIgnoreCase ("treat the key as unknown"),
           "no \"unreliable\" or \"treat the key as unknown\" clause on a usable reading");
    check (offBlock.contains ("THIS IS THE AUTHORITY for key, scale and reference"),
           "21t-i re-cut: ...and the block says in one line that this source is the authority");
    check (offBlock.contains ("exactly what EchoJay displays"), "the rule line is present");

    // persisted in both plugins: the storage key is "keyShowRelative"
    juce::MemoryBlock mb; proc.setKeyShowRelative (true); proc.getStateInformation (mb); proc.setKeyShowRelative (false);
    const juce::String st = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize());
    check (st.contains ("keyShowRelative"), "V2 state carries the storage key \"keyShowRelative\"");

    std::printf ("\n==== detected_key_text: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
