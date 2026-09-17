// dial_wait_guard - HURDLE 1 ITEM 1 (17 Sep 2026): apply waits for maps, bounded.
// On the REAL ChainHost (Primary) with a real AudioPluginInstance probe (one
// named parameter, NOT a builtin) and the real fetch callbacks wired to fakes
// that answer on the message thread after a delay - the shape of Sean's Pure
// Plate turn (settings attached 15:06:12.347, verdict at .425 "applied=0
// status=pending", map stored at .866, applied 3/4 by the 6 s watchdog).
//   (0) the build-complete VERDICT sees the slot APPLIED when its fetch answers
//       inside the bound. TODAY the SESSION path takes it synchronously
//       (PluginEditor.cpp:28536) -> applied=0 pending. -DEJ_GUARD_TODAY takes
//       the verdict the way today's code does; the default takes it through
//       ChainHost::whenDialSettled(kMapFetchBoundMs).
//   (1) a fallback MISS answering BEFORE the exact fetch must not settle the
//       slot noMap - the two fetches had ONE in-flight ledger (RED today).
//   (2) a fetch that NEVER answers settles noMap at the 4 s bound, not pending
//       forever (RED today: dialStateSettled never true).
//   (3) two slots fetch IN PARALLEL: answers at 300 and 900 ms, both applied
//       inside one wait well under 2 x 4 s.
//   (4) control: a cached map dials synchronously at attach.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include <cstdio>
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
struct GainParam final : public juce::HostedAudioProcessorParameter {
    float v = 0.5f; float getValue() const override { return v; } void setValue (float nv) override { v = juce::jlimit (0.0f, 1.0f, nv); }
    float getDefaultValue() const override { return 0.5f; } juce::String getName (int) const override { return "Gain"; } juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); } juce::String getText (float value, int) const override { return juce::String (value, 3); }
    juce::String getParameterID() const override { return "gain"; } };
juce::PluginDescription probeDesc (int uid) { juce::PluginDescription d; d.name = "Wait Probe " + juce::String (uid); d.pluginFormatName = "AudioUnit"; d.manufacturerName = "Guard"; d.fileOrIdentifier = "guard:wait:" + juce::String (uid); d.uniqueId = 0x2E6D000 + uid; d.version = "1.0"; return d; }
struct GainProbe final : public juce::AudioPluginInstance {
    int uid; explicit GainProbe (int u) : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)), uid (u) { addHostedParameter (std::make_unique<GainParam>()); }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = probeDesc (uid); }
    const juce::String getName() const override { return "Wait Probe " + juce::String (uid); }
    void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {} };
juce::var gainMap (const juce::String& fp) {   // gain_db anchored [-24,0],[-6,0.375],[24,1]: -6 dB -> norm 0.375 exactly
    auto* entry = new juce::DynamicObject(); entry->setProperty ("index", 0); entry->setProperty ("kind", "anchored"); entry->setProperty ("trust", "setread"); entry->setProperty ("name", "Gain");
    juce::var anchors; { const double pts[3][2] = {{-24,0.0},{-6,0.375},{24,1.0}}; for (auto& pr : pts) { juce::var a; a.append (pr[0]); a.append (pr[1]); anchors.append (a); } }
    entry->setProperty ("anchors", anchors);
    auto* params = new juce::DynamicObject(); params->setProperty ("gain_db", juce::var (entry));
    auto* map = new juce::DynamicObject(); map->setProperty ("fp", fp); map->setProperty ("dialable", true); map->setProperty ("category", "dynamics"); map->setProperty ("params", juce::var (params)); return juce::var (map); }
juce::var settings() { auto* o = new juce::DynamicObject(); o->setProperty ("gain_db", -6.0); return juce::var (o); }
juce::var missRowFor (const juce::String& body) {   // the lookup's MISS row for the ik the body asked about
    auto ik = juce::JSON::parse (body).getProperty ("plugins", juce::var())[0].getProperty ("ik", juce::var()).toString();
    auto* r = new juce::DynamicObject(); r->setProperty ("i", 0); r->setProperty ("ik", ik); r->setProperty ("tier", "miss"); r->setProperty ("map", juce::var()); r->setProperty ("reason", "no_map_any_version");
    juce::Array<juce::var> arr; arr.add (juce::var (r)); return juce::var (arr); }
float probeNorm (ChainHost& h, int slot) { auto* p = h.getSlotProcessor (slot); return (p && p->getParameters().size() > 0) ? p->getParameters()[0]->getValue() : -1.0f; }
const char* stName (ChainHost::DialStatus s) { switch (s) { case ChainHost::DialStatus::pending: return "pending"; case ChainHost::DialStatus::applied: return "applied"; case ChainHost::DialStatus::partial: return "partial"; case ChainHost::DialStatus::noMap: return "noMap"; case ChainHost::DialStatus::none: return "none"; default: return "other"; } }
// The build-complete verdict, as the code under test takes it.
void verdictThen (ChainHost& h, std::function<void()> fn) {
#ifdef EJ_GUARD_TODAY
    h.logDialSummary ("guard: verdict taken SYNCHRONOUSLY at build completion (today's SESSION path)"); fn();
#else
    bool done = false; h.whenDialSettled (ChainHost::kMapFetchBoundMs, [&] (bool settled) { h.logDialSummary (juce::String ("guard: verdict after whenDialSettled, settled=") + (settled ? "y" : "n")); done = true; });
    const double t0 = juce::Time::getMillisecondCounterHiRes(); while (! done && juce::Time::getMillisecondCounterHiRes() - t0 < 6000) pumpMs (10); fn();
#endif
}
struct Wiring { bool exactAnswers = true; int exactDelayMs = 300; bool fallbackAnswers = true; int fallbackDelayMs = 100; };
void wire (ChainHost& h, const Wiring& w, int& exactAsks, int& fallbackAsks) {
    h.onNeedParamMaps = [&h, w, &exactAsks] (const juce::StringArray& fps) { exactAsks += fps.size(); if (! w.exactAnswers) return;
        for (const auto& fp : fps) juce::Timer::callAfterDelay (w.exactDelayMs, [&h, fp] { auto* o = new juce::DynamicObject(); o->setProperty (juce::Identifier (fp), gainMap (fp)); h.storeParamMaps (juce::var (o)); }); };
    h.onNeedFallbackMaps = [&h, w, &fallbackAsks] (const juce::String& body) { ++fallbackAsks; if (! w.fallbackAnswers) return;
        juce::Timer::callAfterDelay (w.fallbackDelayMs, [&h, body] { h.storeFallbackMaps (missRowFor (body)); }); }; }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_dialwait_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("dial_wait_guard: the build verdict waits for the maps (bounded 4 s per slot, in parallel); noMap only after the fetch answers\n");
#ifdef EJ_GUARD_TODAY
    std::printf ("  [mode: TODAY - the verdict is taken synchronously at build completion, as PluginEditor.cpp:28536 does]\n");
#endif
    std::printf ("== (0)(1) exact fetch answers at 300 ms, the fallback MISS answers first at 100 ms ==\n");
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); int ea = 0, fa = 0; wire (h, Wiring{}, ea, fa);
        h.completeLoad (std::make_unique<GainProbe> (1), probeDesc (1), ChainHost::LoadOrigin::Restore);
        h.setSlotStructuredSettings (0, settings());
        auto st = h.getDialInfos()[0].status;
        check (st == ChainHost::DialStatus::pending, "right after attach: status is PENDING (a fetch is out), never noMap", stName (st));
        check (ea == 1 && fa == 1, "both fetches left at attach (exact + fallback lookup)", "exact=" + juce::String (ea) + " fallback=" + juce::String (fa));
        pumpMs (180);   // the fallback MISS has answered; the exact fetch has not
        st = h.getDialInfos()[0].status;
        check (st == ChainHost::DialStatus::pending, "(1) after the fallback MISS (exact still out): still PENDING - one ledger per fetch", stName (st));
        int applied = -1; juce::String stv;
        verdictThen (h, [&] { auto di = h.getDialInfos()[0]; applied = di.appliedCount; stv = stName (di.status); });
        check (applied == 1 && stv == "applied", "(0) the build-complete VERDICT sees the slot APPLIED (requested 1, applied 1)", "applied=" + juce::String (applied) + " status=" + stv);
        check (std::abs (probeNorm (h, 0) - 0.375f) < 0.02f, "the probe's Gain MOVED to the dialled value (norm 0.375 = -6 dB) - the audio object", juce::String (probeNorm (h, 0), 3));
    }
    std::printf ("== (2) a fetch that NEVER answers ==\n");
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); int ea = 0, fa = 0; Wiring w; w.exactAnswers = false; w.fallbackAnswers = false; wire (h, w, ea, fa);
        h.completeLoad (std::make_unique<GainProbe> (2), probeDesc (2), ChainHost::LoadOrigin::Restore);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        h.setSlotStructuredSettings (0, settings());
        // wait until settled or 5.5 s, whichever first
        while (! h.dialStateSettled() && juce::Time::getMillisecondCounterHiRes() - t0 < 5500) pumpMs (20);
        const double el = juce::Time::getMillisecondCounterHiRes() - t0;
        auto st = h.getDialInfos()[0].status;
        check (h.dialStateSettled() && st == ChainHost::DialStatus::noMap, "(2) the slot settles noMap at the bound instead of pending forever", juce::String ("settled=") + (h.dialStateSettled() ? "y" : "n") + " status=" + stName (st) + " after " + juce::String ((int) el) + " ms");
        check (el >= 3900 && el <= 5200, "(2) the bound is ~4 s (never noMap BEFORE the fetch could answer)", juce::String ((int) el) + " ms");
    }
    std::printf ("== (3) two slots, answers at 300 and 900 ms, one wait ==\n");
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); int ea = 0, fa = 0;
        h.onNeedParamMaps = [&h, &ea] (const juce::StringArray& fps) { for (const auto& fp : fps) { ++ea; const int d = (ea == 1 ? 300 : 900); juce::Timer::callAfterDelay (d, [&h, fp] { auto* o = new juce::DynamicObject(); o->setProperty (juce::Identifier (fp), gainMap (fp)); h.storeParamMaps (juce::var (o)); }); } };
        h.onNeedFallbackMaps = [&h, &fa] (const juce::String& body) { ++fa; juce::Timer::callAfterDelay (50, [&h, body] { h.storeFallbackMaps (missRowFor (body)); }); };
        h.completeLoad (std::make_unique<GainProbe> (3), probeDesc (3), ChainHost::LoadOrigin::Restore);
        h.completeLoad (std::make_unique<GainProbe> (4), probeDesc (4), ChainHost::LoadOrigin::Restore);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        h.setSlotStructuredSettings (0, settings()); h.setSlotStructuredSettings (1, settings());
        int a0 = -1, a1 = -1; verdictThen (h, [&] { auto v = h.getDialInfos(); a0 = v[0].appliedCount; a1 = v[1].appliedCount; });
        const double el = juce::Time::getMillisecondCounterHiRes() - t0;
        check (a0 == 1 && a1 == 1, "(3) both slots APPLIED by the verdict", "applied=" + juce::String (a0) + "," + juce::String (a1));
        check (el < 2000, "(3) fetched in PARALLEL: one wait, well under 2 x 4 s", juce::String ((int) el) + " ms");
    }
    std::printf ("== (4) control: the map is already cached ==\n");
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); int ea = 0, fa = 0; wire (h, Wiring{}, ea, fa);
        h.completeLoad (std::make_unique<GainProbe> (5), probeDesc (5), ChainHost::LoadOrigin::Restore);
        const auto fp = h.getSlotIdentity (0).fp; { auto* o = new juce::DynamicObject(); o->setProperty (juce::Identifier (fp), gainMap (fp)); h.storeParamMaps (juce::var (o)); }
        h.setSlotStructuredSettings (0, settings());
        auto di = h.getDialInfos()[0];
        check (di.status == ChainHost::DialStatus::applied && di.appliedCount == 1 && ea == 0, "(4) cached map: applied synchronously at attach, no fetch asked", juce::String (stName (di.status)) + " asks=" + juce::String (ea));
    }
    std::printf ("\n==== dial_wait_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
