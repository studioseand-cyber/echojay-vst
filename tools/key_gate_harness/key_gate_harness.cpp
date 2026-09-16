// key_gate_harness — COMMIT 4 guard (17 Sep 2026): the key confidence gate is
// GONE. A valid detection is usable at any confidence; the pitch corrector
// locks to the nearest key from the first valid detection and HOLDS it —
// chromatic only before the first valid detection, never on confidence.
// The relative-key toggle (keyShowRelative) turns "A minor" into "C major"
// on every display and in the prompt, through ONE shared helper.
//
// Asserts on the objects that do the work: DetectedKeyFact::usable() (the
// feed's own gate) and EedPitchProcessor::autoKeyState() read back after the
// device's own per-block refresh (processBlock -> refreshAutoKey), never a
// sibling flag. RED today: valid+conf 0.3 is not usable and the corrector
// falls back to chromatic; a later invalid fact drops the held key. The
// display legs need KeyEngine::keyNameShown + KeyDisplayPrefs — absent
// today, so the RED run compiles with -DKEY_GATE_NO_DISPLAY_LEGS and states
// their absence; the GREEN run compiles them in.
// Isolation: private ECHOJAY_STATE_HOME + HOME (build_and_run.sh).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EedPitchProcessor.h"
#include "EedKeyFeed.h"
#include "EedKeyEngine.h"
#include <cstdio>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
echojay::DetectedKeyFact fact (bool valid, int root, bool minor, float conf)
{
    echojay::DetectedKeyFact f;
    f.valid = valid; f.root = root; f.minor = minor; f.confidence = conf;
    f.tuningHz = 440.0f; f.publisherId = 0xB05; f.selfDerived = false;   // a bus-grade fact, not this channel
    juce::String ("Music Bus").copyToUTF8 (f.sourceName, (int) sizeof (f.sourceName));
    return f;
}
void pump (EedPitchProcessor& p, int blocks = 2)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) { buf.clear(); p.processBlock (buf, midi); }
}
juce::String stateStr (const EedPitchProcessor::AutoKeyState& s)
{
    char nm[24]; echojay::KeyEngine::keyName (s.root, s.minor, nm, (int) sizeof (nm));
    juce::String o ("active="); o << (s.active ? "1" : "0") << " applied=" << (s.applied ? "1" : "0")
      << " fellBack=" << (s.fellBack ? "1" : "0") << " key=" << nm << " conf=" << juce::String (s.conf, 2);
    return o;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("key_gate_harness: no confidence gate; lock to nearest key and hold; relative toggle\n");

    std::printf ("== 1. the feed's gate: valid + conf 0.3 is USABLE ==\n");
    check (fact (true, 9, true, 0.30f).usable(),  "valid + conf 0.30 -> usable()");
    check (fact (true, 9, true, 0.05f).usable(),  "valid + conf 0.05 -> usable() (any confidence)");
    check (! fact (false, 9, true, 0.90f).usable(), "NOT valid -> not usable, whatever the confidence");

    std::printf ("== 2. the corrector: valid + conf 0.3 -> the KEY scale, not chromatic ==\n");
    {
        EedPitchProcessor p;
        p.prepareToPlay (48000.0, 512);
        echojay::KeyFeed::instance().publish (fact (true, 9, true, 0.30f));   // A minor, low confidence
        pump (p);
        const auto s = p.autoKeyState();
        check (s.active, "key_source is auto by default", stateStr (s));
        check (s.applied && ! s.fellBack && s.root == 9 && s.minor,
               "conf 0.30 -> A minor APPLIED (scale = minor, root A), no chromatic fallback", stateStr (s));
    }

    std::printf ("== 3. before the first valid detection: CHROMATIC ==\n");
    {
        EedPitchProcessor p;
        p.prepareToPlay (48000.0, 512);
        echojay::KeyFeed::instance().publish (fact (false, 0, false, 0.0f));
        pump (p);
        const auto s = p.autoKeyState();
        check (s.active && ! s.applied && s.fellBack, "no valid fact yet -> chromatic (fellBack), nothing applied", stateStr (s));
    }

    std::printf ("== 4. valid, then silence (the fact goes invalid): the LAST KEY IS HELD ==\n");
    {
        EedPitchProcessor p;
        p.prepareToPlay (48000.0, 512);
        echojay::KeyFeed::instance().publish (fact (true, 2, false, 0.80f));   // D major
        pump (p);
        auto s = p.autoKeyState();
        check (s.applied && s.root == 2 && ! s.minor, "D major applied", stateStr (s));
        echojay::KeyFeed::instance().publish (fact (false, 0, false, 0.0f));   // silence: no source now
        pump (p, 4);
        s = p.autoKeyState();
        check (s.applied && ! s.fellBack && s.root == 2 && ! s.minor,
               "after the fact goes INVALID the corrector still holds D major (never back to chromatic)", stateStr (s));
        echojay::KeyFeed::instance().publish (fact (true, 7, true, 0.20f));    // G minor, low confidence: a real change
        pump (p);
        s = p.autoKeyState();
        check (s.applied && s.root == 7 && s.minor, "a later valid low-confidence fact MOVES the key (G minor)", stateStr (s));
    }

#ifndef KEY_GATE_NO_DISPLAY_LEGS
    std::printf ("== 5. display strings under the relative toggle (one shared helper) ==\n");
    {
        char b[24];
        echojay::KeyEngine::keyNameShown (9, true,  false, b, (int) sizeof (b)); check (juce::String (b) == "A minor", "A minor, toggle OFF -> \"A minor\"", b);
        echojay::KeyEngine::keyNameShown (9, true,  true,  b, (int) sizeof (b)); check (juce::String (b) == "C major", "A minor, toggle ON  -> \"C major\"", b);
        echojay::KeyEngine::keyNameShown (0, false, true,  b, (int) sizeof (b)); check (juce::String (b) == "A minor", "C major, toggle ON  -> \"A minor\"", b);
        echojay::KeyEngine::keyNameShown (0, false, false, b, (int) sizeof (b)); check (juce::String (b) == "C major", "C major, toggle OFF -> \"C major\"", b);
        echojay::KeyEngine::keyNameShown (6, true,  true,  b, (int) sizeof (b)); check (juce::String (b) == "A major", "F# minor, toggle ON -> \"A major\"", b);
        echojay::KeyDisplayPrefs::showRelative().store (true);
        echojay::KeyEngine::keyNameShown (9, true, echojay::KeyDisplayPrefs::showRelative().load(), b, (int) sizeof (b));
        check (juce::String (b) == "C major", "the process-wide pref feeds the same helper", b);
        echojay::KeyDisplayPrefs::showRelative().store (false);
    }
#else
    std::printf ("== 5. display legs: ABSENT in this build (KEY_GATE_NO_DISPLAY_LEGS: keyNameShown/KeyDisplayPrefs do not exist yet) ==\n");
    check (false, "display legs compiled in (the helper exists)");
#endif

    std::printf ("\n==== key_gate_harness: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
