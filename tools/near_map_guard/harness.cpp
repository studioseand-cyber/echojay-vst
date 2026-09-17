// near_map_guard - HURDLE 1 ITEMS 2 + 3 (17 Sep 2026) on the REAL ChainHost.
// A probe (real AudioPluginInstance, 5 named params: Key, Scale, Retune Speed,
// Detune, Humanize) whose fingerprint has NO map. The exact fetch answers
// "null" and the fallback lookup answers a MISS row that also lists SAME-NAME
// maps for other fingerprints ("near"). Sean's Auto-Tune Pro turn: exact miss,
// product fallback "fewer_params", two same-name registry entries never tried.
//   (a) a near map whose names ALL resolve on the live instance (indices from
//       the other build, i.e. WRONG) is accepted, re-indexed by NAME, and the
//       dial lands: applied == requested, the probe's "Retune Speed" moves.
//       RED today: storeFallbackMaps ignores "near" -> noMap, nothing moves.
//   (b) a near map with one unresolvable name ("Flex-Tune") is REJECTED:
//       status noMap, the probe unchanged, the row says which name.
//   (c) addendum: two candidates, 5/5 vs 4/5 names resolving -> the 5/5 one
//       (observable: its anchors land a different value).
//   (d) addendum: a 5/5 tie -> the one with NO essential control classed
//       plumbing/hidden (essential_plumbing []), whatever the offered order.
//   (e) item 3: dial-only + answered fetches + no map + no near map -> the
//       summary row reads "NOT DIALABLE (no map for fp, no near map)" and
//       getDialInfos().notDialable; dial-only OFF -> no such words (control).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include <cstdio>
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
const char* kNames[5] = { "Key", "Scale", "Retune Speed", "Detune", "Humanize" };
struct NamedParam final : public juce::HostedAudioProcessorParameter {
    juce::String nm; float v = 0.5f; explicit NamedParam (const char* n) : nm (n) {}
    float getValue() const override { return v; } void setValue (float nv) override { v = juce::jlimit (0.0f, 1.0f, nv); }
    float getDefaultValue() const override { return 0.5f; } juce::String getName (int) const override { return nm; } juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); } juce::String getText (float value, int) const override { return juce::String (value, 3); }
    juce::String getParameterID() const override { return nm.toLowerCase().replaceCharacter (' ', '_'); } };
juce::PluginDescription probeDesc (int uid) { juce::PluginDescription d; d.name = "Near Probe " + juce::String (uid); d.pluginFormatName = "AudioUnit"; d.manufacturerName = "Guard"; d.fileOrIdentifier = "guard:near:" + juce::String (uid); d.uniqueId = 0x3E6D000 + uid; d.version = "10.5.0"; return d; }
struct Probe final : public juce::AudioPluginInstance {
    int uid; explicit Probe (int u) : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)), uid (u) { for (auto* n : kNames) addHostedParameter (std::make_unique<NamedParam> (n)); }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = probeDesc (uid); }
    const juce::String getName() const override { return "Near Probe " + juce::String (uid); }
    void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {} };
// A same-name map from ANOTHER build: `names` at deliberately WRONG indices
// (index = 10 + i), "Retune Speed" anchored 0..fullScale -> norm 0..1 (linear through 3 anchors).
juce::var nearMap (const juce::String& fp, const juce::StringArray& names, double fullScale) {
    auto* ctl = new juce::DynamicObject();
    for (int i = 0; i < names.size(); ++i) {
        auto* e = new juce::DynamicObject(); e->setProperty ("index", 10 + i); e->setProperty ("name", names[i]); e->setProperty ("kind", "anchored"); e->setProperty ("trust", "setread");
        // THREE anchors with non-multiple gaps: a two-point table reads as a stepped {0|fullScale} control in the
        // engine (EchoJayParamApply enumerated-steps rule) and the dial is left manual. 37 -> 0.37*(100/fullScale).
        juce::var anchors; { juce::var a; a.append (0.0); a.append (0.0); anchors.append (a); }
        { juce::var m; m.append (37.0 * fullScale / 100.0); m.append (0.37); anchors.append (m); }
        { juce::var b; b.append (fullScale); b.append (1.0); anchors.append (b); }
        e->setProperty ("anchors", anchors); ctl->setProperty (juce::Identifier (names[i]), juce::var (e)); }
    auto* map = new juce::DynamicObject(); map->setProperty ("fp", fp); map->setProperty ("dialable", true); map->setProperty ("category", "pitch"); map->setProperty ("controls", juce::var (ctl)); map->setProperty ("params", juce::var (new juce::DynamicObject())); return juce::var (map); }
juce::var cand (const juce::String& fp, const juce::var& map, const juce::StringArray& essentialPlumbing) {
    auto* c = new juce::DynamicObject(); c->setProperty ("fp", fp); c->setProperty ("map", map); juce::Array<juce::var> ep; for (auto& n : essentialPlumbing) ep.add (n); c->setProperty ("essential_plumbing", juce::var (ep)); return juce::var (c); }
juce::var missRow (const juce::String& body, const juce::Array<juce::var>& near) {
    auto ik = juce::JSON::parse (body).getProperty ("plugins", juce::var())[0].getProperty ("ik", juce::var()).toString();
    auto* r = new juce::DynamicObject(); r->setProperty ("i", 0); r->setProperty ("ik", ik); r->setProperty ("tier", "miss"); r->setProperty ("map", juce::var()); r->setProperty ("reason", "fewer_params");
    if (! near.isEmpty()) r->setProperty ("near", juce::var (near));
    juce::Array<juce::var> arr; arr.add (juce::var (r)); return juce::var (arr); }
juce::var settings() { auto* c = new juce::DynamicObject(); c->setProperty ("Retune Speed", 20.0); auto* o = new juce::DynamicObject(); o->setProperty ("controls", juce::var (c)); return juce::var (o); }
float retuneNorm (ChainHost& h, int slot) { auto* p = h.getSlotProcessor (slot); return (p && p->getParameters().size() > 2) ? p->getParameters()[2]->getValue() : -1.0f; }   // live index 2 = "Retune Speed"
const char* stName (ChainHost::DialStatus s) { switch (s) { case ChainHost::DialStatus::pending: return "pending"; case ChainHost::DialStatus::applied: return "applied"; case ChainHost::DialStatus::partial: return "partial"; case ChainHost::DialStatus::noMap: return "noMap"; case ChainHost::DialStatus::none: return "none"; default: return "other"; } }
juce::StringArray all5() { return { "Key", "Scale", "Retune Speed", "Detune", "Humanize" }; }
// One scenario: the probe with settings attached; the exact fetch answers null at 50 ms, the lookup answers `near` at 80 ms.
struct Scenario { ChainHost h { ChainHost::Mode::Primary }; juce::Array<juce::var> near; int uid;
    explicit Scenario (int u, juce::Array<juce::var> n) : near (std::move (n)), uid (u) {
        h.prepare (48000.0, 512);
        h.onNeedParamMaps = [this] (const juce::StringArray& fps) { juce::Timer::callAfterDelay (50, [this, fps] { auto* o = new juce::DynamicObject(); for (auto& fp : fps) o->setProperty (juce::Identifier (fp), juce::var()); h.storeParamMaps (juce::var (o)); }); };
        h.onNeedFallbackMaps = [this] (const juce::String& body) { juce::Timer::callAfterDelay (80, [this, body] { h.storeFallbackMaps (missRow (body, near)); }); };
        h.completeLoad (std::make_unique<Probe> (uid), probeDesc (uid), ChainHost::LoadOrigin::Restore);
        h.setSlotStructuredSettings (0, settings());
        const double t0 = juce::Time::getMillisecondCounterHiRes(); while (! h.dialStateSettled() && juce::Time::getMillisecondCounterHiRes() - t0 < 5000) pumpMs (10); }
    ChainHost::SlotDialInfo di() { return h.getDialInfos()[0]; } };
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_nearmap_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("near_map_guard: same-name maps for another fingerprint, accepted only when every name resolves on the live instance\n");
    const juce::String fpA = juce::String::repeatedString ("a", 64), fpB = juce::String::repeatedString ("b", 64), fpC = juce::String::repeatedString ("c", 64);
    std::printf ("== (a) one near map, all 5 names resolve (indices from the other build) ==\n");
    {
        Scenario s (1, { cand (fpA, nearMap (fpA, all5(), 100.0), {}) });
        auto di = s.di();
        check (di.status == ChainHost::DialStatus::applied && di.appliedCount == 1, "(a) applied == requested (1/1): the near map was accepted and re-indexed by name", juce::String (stName (di.status)) + " applied=" + juce::String (di.appliedCount));
        check (std::abs (retuneNorm (s.h, 0) - 0.20f) < 0.01f, "(a) the probe's live \"Retune Speed\" (index 2, map said 12) moved to 20/100 = norm 0.20", juce::String (retuneNorm (s.h, 0), 3));
    }
    std::printf ("== (b) the only near map carries an unresolvable name ==\n");
    {
        Scenario s (2, { cand (fpB, nearMap (fpB, { "Key", "Scale", "Retune Speed", "Detune", "Flex-Tune" }, 100.0), {}) });
        auto di = s.di();
        check (di.status == ChainHost::DialStatus::noMap && di.appliedCount == 0, "(b) REJECTED: status noMap, nothing applied", juce::String (stName (di.status)) + " applied=" + juce::String (di.appliedCount));
        check (std::abs (retuneNorm (s.h, 0) - 0.5f) < 0.001f, "(b) the probe is untouched (default 0.5)", juce::String (retuneNorm (s.h, 0), 3));
#ifndef EJ_GUARD_TODAY
        check (s.h.dialSummaryRow (0).contains ("near map rejected") && s.h.dialSummaryRow (0).contains ("Flex-Tune"), "(b) the summary row names the unresolved name", s.h.dialSummaryRow (0).fromFirstOccurrenceOf ("nearMap=", false, false));
#endif
    }
    std::printf ("== (c) addendum: 4/5 offered first, 5/5 second -> the 5/5 candidate wins ==\n");
    {
        Scenario s (3, { cand (fpB, nearMap (fpB, { "Key", "Scale", "Retune Speed", "Detune", "Flex-Tune" }, 50.0), {}),   // 4/5, would land 20/50 = 0.40
                         cand (fpA, nearMap (fpA, all5(), 100.0), {}) });                                                  // 5/5, lands 0.20
        auto di = s.di();
        check (di.status == ChainHost::DialStatus::applied && std::abs (retuneNorm (s.h, 0) - 0.20f) < 0.01f, "(c) the candidate with the MOST names resolving (5/5) was chosen: norm 0.20, not 0.40", juce::String (stName (di.status)) + " norm=" + juce::String (retuneNorm (s.h, 0), 3));
    }
    std::printf ("== (d) addendum: a 5/5 tie, the first offered has \"Scale\" classed plumbing/hidden -> the clean one wins ==\n");
    {
        Scenario s (4, { cand (fpC, nearMap (fpC, all5(), 50.0), { "Scale" }),   // 5/5 but an essential control classed plumbing/hidden; would land 0.40
                         cand (fpA, nearMap (fpA, all5(), 100.0), {}) });         // 5/5, clean; lands 0.20
        auto di = s.di();
        check (di.status == ChainHost::DialStatus::applied && std::abs (retuneNorm (s.h, 0) - 0.20f) < 0.01f, "(d) tie broken toward the candidate with NO essential control classed plumbing/hidden: norm 0.20", juce::String (stName (di.status)) + " norm=" + juce::String (retuneNorm (s.h, 0), 3));
    }
    std::printf ("== (e) item 3 + AMENDMENT (17 Sep 2026): fetches answered, no map, no near map ==\n");
    {
#ifndef EJ_GUARD_TODAY
        // AMENDMENT: under dial-only the slot is SUBSTITUTED (substitute_guard); the red NOT DIALABLE row
        // exists ONLY when dial-only is OFF.
        Scenario s (5, {}); s.h.setDialOnlyMode (false);
        auto di = s.di(); const auto row = s.h.dialSummaryRow (0);
        check (di.status == ChainHost::DialStatus::noMap && di.notDialable, "(e) dial-only OFF: getDialInfos notDialable with the reason", juce::String (stName (di.status)) + " reason=\"" + di.notDialableReason + "\"");
        check (row.contains ("NOT DIALABLE (no map for fp, no near map)"), "(e) dial-only OFF: the summary row says NOT DIALABLE (no map for fp, no near map)", row.fromFirstOccurrenceOf ("status=", false, false));
        s.h.setDialOnlyMode (true);
        check (! s.h.dialSummaryRow (0).contains ("NOT DIALABLE") && ! s.h.getDialInfos()[0].notDialable, "(e) AMENDMENT: dial-only ON -> never the red row (the slot is substituted instead)");
        // a slot whose fetch is still OUT must not be called NOT DIALABLE: attach with fetches that never answer, read at once
        ChainHost hh (ChainHost::Mode::Primary); hh.prepare (48000.0, 512); hh.setDialOnlyMode (false);
        hh.onNeedParamMaps = [] (const juce::StringArray&) {}; hh.onNeedFallbackMaps = [] (const juce::String&) {};
        hh.completeLoad (std::make_unique<Probe> (7), probeDesc (7), ChainHost::LoadOrigin::Restore); hh.setSlotStructuredSettings (0, settings());
        check (! hh.getDialInfos()[0].notDialable && ! hh.dialSummaryRow (0).contains ("NOT DIALABLE"), "(e) control: while a fetch is still out the slot is PENDING, never NOT DIALABLE", stName (hh.getDialInfos()[0].status));
#else
        std::printf ("  (skipped in TODAY mode: dialSummaryRow / setDialOnlyMode do not exist)\n"); ++failures;
#endif
    }
    std::printf ("\n==== near_map_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
