// near_verify_guard - ITEM 1 (17 Sep 2026 ruling): pre-chat near-map verification on the REAL ChainHost.
// The fp is sha256(format|uidHex|version|paramCount) and the scan index caches NO parameter names, so the
// verifier instantiates ONCE (injected here: a real AudioPluginInstance probe with Key/Scale/Retune Speed/
// Detune/Humanize; count observed), asks the lookup (injected: a miss row with same-name candidates), accepts
// the candidate whose names all resolve, caches the verdict per identity in param_maps.json and reports it as
// verifiedNear {name: candidateFp}. RED today (-DEJ_GUARD_TODAY): the API does not exist.
//   (a) one plugin, candidate 5/5 -> verdict VERIFIED, candidateFp, the map cached under the live fp, 1 instantiation
//   (b) verify again -> NOT repeated (still 1 instantiation)
//   (c) buildVerifiedNearJson carries {"Guard Tune": candidateFp}
//   (d) a plugin whose only candidate is 4/5 -> verdict cached as NOT verified, absent from the json, 1 instantiation
//   (e) persistence: a NEW ChainHost (same isolated state root) loads both verdicts and instantiates nothing on re-verify
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EchoJayParamMaps.h"
#include "probe.h"
#include "EJStateRoot.h"
#include <cstdio>
namespace { int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; } }
using namespace guardprobe;
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_nearverify_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    echojay::userAppData().getChildFile ("EchoJay").createDirectory();   // the app makes this; the harness must too (param_maps.json lives here)
    std::printf ("near_verify_guard: same-name candidates verified against the live names AHEAD of the chat turn, once per identity, cached\n");
#ifdef EJ_GUARD_TODAY
    std::printf ("  (TODAY: ChainHost has no pre-chat verifier - RED by construction)\n"); ++failures;
#else
    const juce::String fpA = juce::String::repeatedString ("a", 64), fpB = juce::String::repeatedString ("b", 64);
    const auto dGood = probeDesc (1, "Guard Tune"), dBad = probeDesc (2, "Guard Flex");
    const auto ikGood = echojay::identityKeyForDescription (dGood), ikBad = echojay::identityKeyForDescription (dBad);
    int instantiations = 0;
    auto wire = [&] (ChainHost& h)
    {
        h.nearInstantiate = [&instantiations] (const juce::PluginDescription& d, std::function<void(std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> cb)
        { ++instantiations; juce::Timer::callAfterDelay (20, [d, cb] { cb (std::make_unique<Probe> (d), {}); }); };
        h.onNeedNearLookup = [] (const juce::String& body, std::function<void(const juce::var&)> done)
        {
            const bool good = body.contains ("Guard Tune");
            juce::Array<juce::var> near;
            near.add (cand (good ? juce::String::repeatedString ("a", 64) : juce::String::repeatedString ("b", 64),
                            nearMap (good ? juce::String::repeatedString ("a", 64) : juce::String::repeatedString ("b", 64),
                                     good ? juce::StringArray { "Key", "Scale", "Retune Speed", "Detune", "Humanize" }
                                          : juce::StringArray { "Key", "Scale", "Retune Speed", "Detune", "Flex-Tune" })));
            juce::Timer::callAfterDelay (20, [body, near, done] { done (missRow (body, near)); });
        };
    };
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); wire (h);
        h.verifyNearCandidatesFor ({ dGood });
        const double t0 = juce::Time::getMillisecondCounterHiRes(); while (! h.nearVerifyIdle() && juce::Time::getMillisecondCounterHiRes() - t0 < 3000) pumpMs (10);
        const auto* v = h.nearVerdictFor (ikGood);
        check (v != nullptr && v->verified && v->candidateFp == fpA, "(a) verdict VERIFIED with the candidate fp", v ? (v->verified ? "verified " : "not verified ") + v->note : juce::String ("no verdict"));
        check (v != nullptr && v->liveFp.isNotEmpty(), "(a) the live fp was computed from the instance (format|uid|version|paramCount)", v ? v->liveFp.substring (0, 12) : juce::String());
        check (instantiations == 1, "(a) instantiated exactly ONCE", juce::String (instantiations));
        h.verifyNearCandidatesFor ({ dGood }); pumpMs (400);
        check (instantiations == 1, "(b) verifying again does NOT instantiate again (verdict cached)", juce::String (instantiations));
        const auto json = h.buildVerifiedNearJson();
        check (json.contains ("\"Guard Tune\"") && json.contains (fpA), "(c) buildVerifiedNearJson carries {\"Guard Tune\": candidateFp}", json.substring (0, 90));
        h.verifyNearCandidatesFor ({ dBad });
        const double t1 = juce::Time::getMillisecondCounterHiRes(); while (! h.nearVerifyIdle() && juce::Time::getMillisecondCounterHiRes() - t1 < 3000) pumpMs (10);
        const auto* vb = h.nearVerdictFor (ikBad);
        check (vb != nullptr && ! vb->verified && vb->note.contains ("Flex-Tune"), "(d) a 4/5 candidate -> verdict cached as NOT verified, naming the unresolved name", vb ? vb->note : juce::String ("no verdict"));
        check (! h.buildVerifiedNearJson().contains ("Guard Flex"), "(d) and it is absent from the verifiedNear json");
        check (instantiations == 2, "(d) one instantiation for the second plugin", juce::String (instantiations));
    }
    std::printf ("== (e) persistence across a NEW ChainHost ==\n");
    {
        ChainHost h2 (ChainHost::Mode::Primary); h2.prepare (48000.0, 512); wire (h2);
        const int before = instantiations;
        const auto* v = h2.nearVerdictFor (ikGood); const auto* vb = h2.nearVerdictFor (ikBad);
        check (v != nullptr && v->verified && v->candidateFp == fpA, "(e) the VERIFIED verdict was loaded from param_maps.json");
        check (vb != nullptr && ! vb->verified, "(e) the NOT-verified verdict was loaded too");
        h2.verifyNearCandidatesFor ({ dGood, dBad }); pumpMs (400);
        check (instantiations == before, "(e) re-verifying instantiates NOTHING (never repeated)", juce::String (instantiations - before));
        check (h2.buildVerifiedNearJson().contains (fpA), "(e) the json still carries the verified candidate after reload");
    }
#endif
    std::printf ("\n==== near_verify_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
