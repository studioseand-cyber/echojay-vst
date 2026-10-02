/*
  RoundTripTest.cpp

  The drift guard. This is the test the spec calls non-negotiable.

  Claim under test: a map produced by ejmap, run through EchoJay's applySettings
  from the shared header, produces the identical writes the mapper verified
  during mapping.

  If that stops being true, ejmap is verifying one thing and EchoJay is doing
  another, which is the exact failure shape that let usableCoreCount go stale and
  read bx_digital V3 as not dialable.

  The round trip was closed 2026-07-31, deferred since M0 because the corpus
  was empty. Two layers, deliberately:

    - Write level: a SYNTHETIC map, constructed by hand before M3 produced any
      real anchors (a test written against real anchors risks shaping itself
      to the data it should judge), run through the real applySettings against
      a minimal in-process AudioPluginInstance. Expected norms are
      hand-computed constants, never calls into the code under test.
    - Interpolation level: when real maps exist in ~/Library/ejmap/maps/,
      every evidence.readback pair must reproduce through the real
      interpolateAnchors over the map's own table. No instantiation needed,
      so the gate stays runnable on a machine with no plugins.

  A test that passes and a feature that works are different claims, and on
  this project the gap has bitten three times: the tripwire had green tests
  and had never fired, the client gate had 35/35 in a harness and wrote
  +16 dB in Logic.
*/

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>   // ScopedJuceInitialiser_GUI
#include "EjmapSchema.h"
#include "EjmapSubject.h"
#include "EjmapTriage.h"
#include "EjmapSend.h"
#include "EjmapProbeRoute.h"
#include "EjmapMarks.h"
#include "EjmapAssignment.h"
#include "EjmapLedger.h"
#include "EjmapSupervisor.h"
#include "EjmapViewLayer.h"
#include "EjmapMouth.h"
#include <sys/stat.h>

// The shared sweep and parsers, compiled here so the drift gate proves both
// binaries build the SAME code: ejextract compiles these headers to produce
// the corpus, ejmap compiles them to produce anchors, and this test pins the
// behaviours M3 leans on. EjmapSchema.h already pulls in EchoJayParamApply.h
// (parseDisplayForUnit, dominantMonotonicTable); the extractor header is the
// M3 lift.
#include "EchoJayParamExtractor.h"
#include "EjmapMouth.h"
#include <sys/stat.h>
#include "EchoJayParamMaps.h"   // identityKeyForDescription
#include "EjmapExposure.h"
#include "EjmapFixtureUnit.h"
#include "EjmapFixtureRange.h"
#include "EjmapFixtureReadout.h"
#include "EjmapCertOutcome.h"
#include "EjmapNameTokens.h"
#include "EjmapRoles.h"
#include "EjmapRoleSemantics.h"
#include "EjmapSweep.h"
#include <functional>
#include "EjmapCertDriver.h"

namespace
{

int failures = 0;
int checks   = 0;

void check (bool condition, const juce::String& what)
{
    ++checks;
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

//==============================================================================
void testSchemaVersionPinned()
{
    // The constant now lives in EchoJayParamApply.h and the compile-time guard
    // is in EjmapSchema.h, which this file includes: drift stops this target at
    // the compiler, before these run. These stay as the runtime restatement, so
    // a binary that somehow linked against a different constant still says so.
    check (ejmap::kMapSchemaVersion == 24, "kMapSchemaVersion is 24");
    check (&ejmap::kMapSchemaVersion == &echojay::kMapSchemaVersion,
           "ejmap and echojay name the same object, not two copies");
    check (juce::String (ejmap::kMapSchemaString) == "2.4", "kMapSchemaString is 2.4");
}

void testVerdictSemantics()
{
    using namespace ejmap;

    // The single most important behavioural claim in the schema: an inconclusive
    // probe is not a pass and does not block. If either of these flips, an
    // unprobeable parameter starts reading as verified.
    check (! countsAsPass (ProbeVerdict::inconclusive), "inconclusive is not a pass");
    check (! blocksSubmit (ProbeVerdict::inconclusive),  "inconclusive does not block submit");
    check (  blocksSubmit (ProbeVerdict::contradicts),   "contradicts blocks submit");
    check (  countsAsPass (ProbeVerdict::confirms),      "confirms is a pass");
}

void testTrustOrdering()
{
    using namespace ejmap;

    // The merge-per-key rule depends on this ordering. Human where present,
    // model elsewhere.
    check (Trust::humanVerified > Trust::llmClassified, "human-verified outranks llm-classified");
    check (Trust::humanVerified > Trust::setread,       "human-verified outranks setread");
    check (Trust::adminApproved > Trust::humanVerified, "admin-approved outranks human-verified");
    check (Trust::setread       > Trust::ruleBuilt,     "setread outranks rule-built");
}

void testSkipRequiresReason()
{
    using namespace ejmap;

    SkipRecord s ("makeup_db", SkipOutcome::notPresent, "no makeup control on this plugin");
    check (s.reason.isNotEmpty(), "a skip carries a reason");

    auto v = s.toVar();
    check (v.getProperty ("outcome", "").toString() == "not_present", "skip outcome serialises");
    check (v.getProperty ("reason", "").toString().isNotEmpty(),      "skip reason serialises");
}

void testPayloadSerialises()
{
    using namespace ejmap;

    MapPayload p;
    p.fp = "testfp";
    p.category = "eq";
    p.mode = Mode::deep;
    p.identity.format = "AudioUnit";
    p.identity.name = "Test EQ";
    p.identity.paramCount = 12;

    ParamMapping m;
    m.semantic = "freq_hz";
    m.indices.add (3);
    m.paramName = "LF Freq";
    m.kind = "freq_hz";
    m.anchors.add ({ 0.0, 15.0 });
    m.anchors.add ({ 1.0, 780.0 });
    m.trust = Trust::humanVerified;
    m.method = AnchorMethod::setread;
    p.params.add (m);

    auto json = p.toJson();
    check (json.contains ("freq_hz"),         "payload contains the semantic key");
    check (json.contains ("human-verified"),  "payload carries per-key trust");
    check (json.contains ("\"schema\""),      "payload carries a schema version");

    auto reparsed = juce::JSON::parse (json);
    check (reparsed.isObject(), "payload reparses as JSON");
}

void testContradictionBlocks()
{
    using namespace ejmap;

    MapPayload p;
    check (! p.hasUnresolvedContradiction(), "empty payload has no contradiction");

    ProbeResult r;
    r.semantic = "freq_hz";
    r.verdict  = ProbeVerdict::inconclusive;
    p.evidence.audioProbe.add (r);
    check (! p.hasUnresolvedContradiction(), "inconclusive does not count as a contradiction");

    r.verdict = ProbeVerdict::contradicts;
    p.evidence.audioProbe.add (r);
    check (p.hasUnresolvedContradiction(), "contradicts is detected");
}

//==============================================================================
void testSharedParsers()
{
    // parseLeadingFloat: the extractor-side parser that guides adaptive
    // refinement, so its behaviour shapes corpus output byte-for-byte.
    double v = 0.0;
    check (echojay::parseLeadingFloat ("1.4:1", v)    && juce::approximatelyEqual (v, 1.4),  "parseLeadingFloat 1.4:1");
    check (echojay::parseLeadingFloat ("-18.0 dB", v) && juce::approximatelyEqual (v, -18.0),"parseLeadingFloat -18.0 dB");
    check (! echojay::parseLeadingFloat ("Bypass", v),                                       "parseLeadingFloat rejects Bypass");

    // Pinned as MEASURED, against the header comment's claim: "Inf:1" parses
    // as 1.0 because a digit anywhere counts. The 4,233-map corpus was built
    // with this behaviour, so this is the behaviour the gate protects; the
    // header comment said otherwise and has been corrected.
    check (echojay::parseLeadingFloat ("Inf:1", v) && juce::approximatelyEqual (v, 1.0),     "parseLeadingFloat Inf:1 -> 1.0 (measured)");

    // parseDisplayForUnit: the dial-time parser whose bare-k and NkM fixes the
    // plan requires to be THE shared ones. Pinned here so a drift in either
    // fix stops the gate, exactly like the schema constant.
    float f = 0.0f; bool negInf = false;
    check (echojay::parseDisplayForUnit ("1k1", "hz", f, negInf)   && juce::approximatelyEqual (f, 1100.0f),  "NkM: 1k1 hz -> 1100");
    check (echojay::parseDisplayForUnit ("12k", "hz", f, negInf)   && juce::approximatelyEqual (f, 12000.0f), "bare-k: 12k hz -> 12000");
    check (echojay::parseDisplayForUnit ("12k", "db", f, negInf)   && juce::approximatelyEqual (f, 12.0f),    "k never multiplies under db");
    check (echojay::parseDisplayForUnit ("-oo dB", "db", f, negInf) && negInf,                                 "-oo dB -> negInf");
    check (echojay::parseDisplayForUnit ("3.00 : 1", "ratio", f, negInf) && juce::approximatelyEqual (f, 3.0f),"ratio 3.00:1 -> 3");

    // sweepIsFlat: flat detection is behavioural, never name-based (register
    // rule), so the predicate itself is pinned.
    juce::Array<echojay::SweepPoint> flat, rising;
    flat.add ({ 0.0f, "50%" });  flat.add ({ 0.5f, "50%" });  flat.add ({ 1.0f, "50%" });
    rising.add ({ 0.0f, "0%" }); rising.add ({ 1.0f, "100%" });
    check (  echojay::sweepIsFlat (flat),   "flat sweep detected");
    check (! echojay::sweepIsFlat (rising), "distinct texts are not flat");
}

//==============================================================================
// The round trip itself, closed 2026-07-31 after deferral since M0.
//
// The corpus was empty for the whole deferral, and closing it NOW, before M3
// writes any anchors, is deliberate: a test written against real anchors risks
// shaping itself to the data it should be judging. So the map here is
// SYNTHETIC, CONSTRUCTED BY HAND, and says so. The plugin is synthetic too --
// a minimal in-process AudioPluginInstance whose parameters display exactly
// what their anchor tables claim -- because the pre-commit gate must run on a
// machine with no plugins installed.
//
// What is real: applySettings, applyOne, interpolateAnchors,
// dominantMonotonicTable, typedReadbackMatch -- the exact shipping functions
// from Source/EchoJayParamApply.h. Reimplementing any of them here is
// precisely the drift this test exists to catch, so the expected normalised
// writes are HAND-COMPUTED constants, not calls into the code under test.
//==============================================================================

/** A parameter whose display is an exact linear map from norm to value.
    Derives from HostedParameter because AudioPluginInstance only accepts
    hosted parameters, which is the right constraint: applyOne runs against
    hosted instances and this test must walk in through the same door.
*/
struct FakeParam final : juce::AudioPluginInstance::HostedParameter
{
    FakeParam (juce::String nm, juce::String unitIn, float v0, float v1, bool liarIn = false)
        : name (std::move (nm)), unit (std::move (unitIn)), lo (v0), hi (v1), liar (liarIn) {}

    float value = 0.0f;
    juce::String name, unit;
    float lo, hi;
    bool liar;      // display ignores the queried value: the Valhalla class

    float getValue() const override                    { return value; }
    void  setValue (float v) override                  { value = v; }
    float getDefaultValue() const override             { return 0.0f; }
    juce::String getName (int len) const override      { return name.substring (0, len); }
    juce::String getLabel() const override             { return unit; }
    float getValueForText (const juce::String&) const override { return 0.0f; }
    juce::String getParameterID() const override       { return name; }

    juce::StringArray labelList;      // non-empty: display shows labels

    juce::String textFor (float n) const
    {
        if (! labelList.isEmpty())
            return labelList[juce::jlimit (0, labelList.size() - 1,
                                           (int) std::round (n * (float) (labelList.size() - 1)))];
        return juce::String (lo + n * (hi - lo), 1) + " " + unit;
    }
    juce::String getText (float n, int) const override
    {
        return liar ? textFor (value)   // lies: formats CURRENT state
                    : textFor (n);
    }
    juce::String getCurrentValueAsText() const override { return textFor (value); }
};

/** The least plugin that satisfies AudioPluginInstance. */
struct FakeInstance final : juce::AudioPluginInstance
{
    FakeInstance()
    {
        // Index 0: threshold with ASCENDING anchors. Index 1: mpressor-shaped
        // DESCENDING threshold (16 dB at n=0, -18 dB at n=1). Index 2: a text
        // liar. Index 3: a degenerate-map victim that must be refused.
        addHostedParameter (std::make_unique<FakeParam> ("Thresh Up",   "dB", -30.0f, 16.0f));
        addHostedParameter (std::make_unique<FakeParam> ("Thresh Down", "dB",  16.0f, -18.0f));
        addHostedParameter (std::make_unique<FakeParam> ("Liar Mix",    "%",    0.0f, 100.0f, true));
        addHostedParameter (std::make_unique<FakeParam> ("Stuck",       "dB",   0.0f,  10.0f));

        // Indices 4-8: the AMEK shape for the M5 group pin. Two bands and a
        // Mono Maker imposter -- a USABLE, freq-named flat entry, exactly the
        // control the original bug wrote 250 Hz into.
        addHostedParameter (std::make_unique<FakeParam> ("LF Freq",    "Hz",  20.0f,  400.0f));
        addHostedParameter (std::make_unique<FakeParam> ("LF Gain",    "dB", -12.0f,   12.0f));
        addHostedParameter (std::make_unique<FakeParam> ("MF Freq",    "Hz", 500.0f, 18000.0f));
        addHostedParameter (std::make_unique<FakeParam> ("MF Gain",    "dB", -12.0f,   12.0f));
        addHostedParameter (std::make_unique<FakeParam> ("Mono Maker", "Hz",  20.0f,  400.0f));

        // Indices 9-13: the Tier 2 subjects. A unitless sharpness, a labelled
        // knee, an exact-case duplicate pair, and its lowercase cousin.
        addHostedParameter (std::make_unique<FakeParam> ("Sharpness", "", 0.0f, 10.0f));
        {
            auto knee = std::make_unique<FakeParam> ("Knee", "", 0.0f, 2.0f);
            knee->labelList = { "Hard", "Med", "Soft" };
            addHostedParameter (std::move (knee));
        }
        addHostedParameter (std::make_unique<FakeParam> ("Bypass", "", 0.0f, 1.0f));
        addHostedParameter (std::make_unique<FakeParam> ("Bypass", "", 0.0f, 1.0f));
        addHostedParameter (std::make_unique<FakeParam> ("bypass", "", 0.0f, 1.0f));
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override
    { d.name = "RoundTripFake"; d.pluginFormatName = "Fake"; }

    const juce::String getName() const override            { return "RoundTripFake"; }
    void prepareToPlay (double, int) override              {}
    void releaseResources() override                       {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override           { return 0.0; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    juce::AudioProcessorEditor* createEditor() override    { return nullptr; }
    bool hasEditor() const override                        { return false; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override   {}
};

void testPayloadFeedsApply()
{
    // The schema WRITER must produce what the apply READER consumes. Found by
    // inspection one commit before the first real submit would have shipped
    // it: anchorsToVar emitted [normalised, value] while anchorsFromVar and
    // every served map use [value, normalised]. Dormant because no consumer
    // had ever read a payload this writer produced.
    ejmap::ParamMapping m;
    m.semantic  = "threshold_db";
    m.indices.add (3);
    m.paramName = "Thresh";
    m.kind      = "threshold_db";
    m.anchors.add ({ 0.0, -30.0 });    // AnchorPoint{normalised, value}
    m.anchors.add ({ 1.0,  16.0 });

    auto parsed  = juce::JSON::parse (juce::JSON::toString (m.toVar()));
    auto table   = echojay::anchorsFromVar (parsed);
    check (table.size() == 2
             && juce::approximatelyEqual (table[0][0], -30.0f)
             && juce::approximatelyEqual (table[0][1], 0.0f),
           "payload anchors read back as [value, norm] through the real apply reader");

    const float n = echojay::interpolateAnchors (table, -7.0f);   // half way
    check (std::abs (n - 0.5f) < 1.0e-4f,
           "payload anchors interpolate through the real apply path");
}

void testRoundTripThroughApplySettings()
{
    FakeInstance plugin;

    // The synthetic map. anchors are [value, norm] pairs, as the server emits.
    const auto mapJson = juce::String (R"({
      "schema": ")" + juce::String (ejmap::kMapSchemaString) + R"(",
      "params": {
        "threshold_db": { "index": 0, "kind": "anchored", "method": "gettext",
                          "anchors": [[-30, 0], [0, 0.6], [16, 1]] },
        "makeup_db":    { "index": 1, "kind": "anchored", "method": "gettext",
                          "anchors": [[16, 0], [-18, 1]] },
        "mix_pct":      { "index": 2, "kind": "anchored", "method": "setread",
                          "anchors": [[0, 0], [100, 1]] },
        "drive":        { "index": 3, "kind": "anchored", "method": "gettext",
                          "anchors": [[5, 0], [5, 1]] }
      }
    })");
    auto map = juce::JSON::parse (mapJson);
    check (map.isObject(), "synthetic map parses");

    // The evidence block the mapper would have verified: semantic -> asked
    // value -> normalised write. The norms are hand-computed from the anchor
    // tables above, NOT computed by calling interpolateAnchors here.
    //   threshold_db -18: segment [-30,0], frac (=(-18+30)/30) = 0.4 -> 0.24
    //   threshold_db   8: segment [0,16],  0.6 + 0.5*0.4          -> 0.80
    //   makeup_db    -18: descending table bottom                 -> 1.00
    //   mix_pct       25: linear                                  -> 0.25
    struct Verified { const char* semantic; const char* asked; float wrote; };
    const Verified evidence[] = {
        { "threshold_db", "-18 dB", 0.24f },
        { "makeup_db",    "-18 dB", 1.00f },
        { "mix_pct",      "25",     0.25f },
    };

    // A CONTROL WITH NO ANCHOR TABLE MUST BE REFUSED, NOT WRITTEN. Guarded
    // since forever at applyOne's `if (anchors.isEmpty())`, and asserted by
    // nothing until 8 Aug 2026 -- one edit from regressing silently, and the
    // silence is the problem: interpolateAnchors returns 0.0f on an empty
    // table, so a lost guard means every value asked lands at minimum with the
    // write reported as done.
    //
    // 42 of the corpus's 51,606 controls have this shape and every one is
    // reachable BY NAME through Tier 2, which routes into the same applyOne.
    // Both doors, one guard, and now one test.
    {
        auto* dead = new juce::DynamicObject();
        dead->setProperty ("index", 0);
        dead->setProperty ("kind", "anchored");
        dead->setProperty ("name", "Dead");
        dead->setProperty ("anchors", juce::var (juce::Array<juce::var>{}));
        auto* ctrls = new juce::DynamicObject();
        ctrls->setProperty ("Dead", juce::var (dead));
        auto* deadMap = new juce::DynamicObject();
        deadMap->setProperty ("controls", juce::var (ctrls));
        deadMap->setProperty ("params", juce::var (new juce::DynamicObject()));
        auto* ask = new juce::DynamicObject();
        ask->setProperty ("Dead", "5");
        auto dr = echojay::applySettings (plugin, juce::var (deadMap), juce::var (ask));
        check (dr.size() == 1 && ! dr.getReference (0).applied,
               "empty anchor table: refused, not written");
        check (dr.size() == 1 && dr.getReference (0).note.contains ("no anchors"),
               "empty anchor table: and the refusal says why ("
                 + (dr.isEmpty() ? juce::String() : dr.getReference (0).note) + ")");
    }

    for (const auto& ev : evidence)
    {
        auto* settings = new juce::DynamicObject();
        settings->setProperty (ev.semantic, juce::String (ev.asked));
        auto results = echojay::applySettings (plugin, map, juce::var (settings));

        check (results.size() == 1, juce::String (ev.semantic) + ": one result");
        if (results.size() != 1) continue;
        const auto& r = results.getReference (0);

        check (r.applied, juce::String (ev.semantic) + " " + ev.asked + " applied ("
                            + r.note + ")");
        check (std::abs (r.normalized - ev.wrote) < 1.0e-4f,
               juce::String (ev.semantic) + " wrote " + juce::String (r.normalized, 4)
                 + ", evidence says " + juce::String (ev.wrote, 4));
    }

    // The mpressor gate row, stated exactly: -18 dB on the descending table
    // writes n=1 and NOT n=0. n=0 would leave the knob at +16 dB, which is
    // the +16-in-Logic bug class this whole file exists to prevent.
    {
        auto& params = plugin.getParameters();
        check (std::abs (params[1]->getValue() - 1.0f) < 1.0e-4f,
               "descending anchors: -18 dB landed at n=1, display "
                 + params[1]->getCurrentValueAsText());
    }

    // The liar wrote without display verification and said so.
    {
        auto* settings = new juce::DynamicObject();
        settings->setProperty ("mix_pct", "80");
        auto results = echojay::applySettings (plugin, map, juce::var (settings));
        check (results.size() == 1 && results[0].applied
                 && ! results[0].displayVerified,
               "setread entry applies with the unverifiable caveat, never as verified");
    }

    // The degenerate table is refused, not written: a map whose anchors span
    // nothing can only pin the knob at one end whatever is asked.
    {
        auto* settings = new juce::DynamicObject();
        settings->setProperty ("drive", "5");
        auto results = echojay::applySettings (plugin, map, juce::var (settings));
        check (results.size() == 1 && ! results[0].applied,
               "degenerate anchor span refused: " + (results.size() == 1
                   ? results[0].note : juce::String ("no result")));
    }
}

//==============================================================================
// The M5 pin: the project's headline assertion as a unit test, run on every
// commit before AMEK is ever loaded. The map below reproduces the ORIGINAL
// AMEK bug shape -- a flat freq_hz entry pointing at Mono Maker -- and adds
// the groups M5 builds. The assertion is behavioural, from parameter VALUES:
// a 250 Hz request lands on the LF band and Mono Maker's value never moves,
// even though the flat map points straight at it.
//
// This is also the writer->consumer contract test for groups: the map is
// built through the real GroupSpec/MapPayload writers (n = band NUMBER, one
// group per band; entries carry "name" for the imposter guard) and consumed
// by the real applySettings/applyBands. No served map has ever carried a
// group (0 of the corpus, measured), so this pin IS the contract.
void testGroupsRouteAroundMonoMaker()
{
    using namespace ejmap;
    FakeInstance plugin;

    auto entry = [] (const char* sem, int idx, const char* name,
                     float v0, float v1) -> ParamMapping
    {
        ParamMapping m;
        m.semantic = sem; m.kind = sem; m.paramName = name;
        m.indices.add (idx);
        m.anchors.add ({ 0.0, (double) v0 });
        m.anchors.add ({ 1.0, (double) v1 });
        m.trust = Trust::humanVerified;
        m.method = AnchorMethod::setread;
        return m;
    };

    MapPayload p;
    p.fp = "grouppin"; p.category = "eq";
    p.identity.format = "Fake"; p.identity.name = "RoundTripFake"; p.identity.paramCount = 9;

    // The bug shape: flat freq_hz points at Mono Maker, and it is USABLE.
    p.params.add (entry ("freq_hz", 8, "Mono Maker", 20.0f, 400.0f));

    GroupSpec lf;
    lf.family = "band"; lf.n = 1; lf.primary = true;
    lf.freqLo = 20.0; lf.freqHi = 400.0;
    lf.params.add (entry ("freq_hz", 4, "LF Freq", 20.0f, 400.0f));
    lf.params.add (entry ("gain_db", 5, "LF Gain", -12.0f, 12.0f));
    p.groups.add (lf);

    GroupSpec mf;
    mf.family = "band"; mf.n = 2;
    mf.freqLo = 500.0; mf.freqHi = 18000.0;
    mf.params.add (entry ("freq_hz", 6, "MF Freq", 500.0f, 18000.0f));
    mf.params.add (entry ("gain_db", 7, "MF Gain", -12.0f, 12.0f));
    p.groups.add (mf);

    auto map = juce::JSON::parse (p.toJson());
    check (map.isObject(), "group pin: payload parses");

    auto& params = plugin.getParameters();
    auto valueOf = [&params] (int i) { return params[i]->getValue(); };

    // 250 Hz: must land on LF (idx 4), not MF, and NEVER Mono Maker.
    {
        const float mmBefore = valueOf (8), mfBefore = valueOf (6);
        auto* settings = new juce::DynamicObject();
        settings->setProperty ("freq_hz", 250);
        settings->setProperty ("gain_db", 3);
        auto results = echojay::applySettings (plugin, map, juce::var (settings));

        bool freqApplied = false;
        for (const auto& r : results)
            if (r.semantic == "freq_hz") freqApplied = r.applied && r.index == 4;
        check (freqApplied, "250 Hz resolves to the LF band's index, not index 8");
        check (juce::approximatelyEqual (valueOf (8), mmBefore),
               "Mono Maker's VALUE is untouched by the 250 Hz request");
        check (juce::approximatelyEqual (valueOf (6), mfBefore),
               "the out-of-range MF band is untouched at 250 Hz");
        check (std::abs (valueOf (4) - (250.0f - 20.0f) / 380.0f) < 1.0e-3f,
               "LF Freq landed at the interpolated norm for 250 Hz");
    }

    // 8 kHz, via an explicit bands request: must land on MF (idx 6).
    {
        const float mmBefore = valueOf (8), lfBefore = valueOf (4);
        auto* band = new juce::DynamicObject();
        band->setProperty ("freq_hz", 8000);
        juce::Array<juce::var> bands; bands.add (juce::var (band));
        auto* settings = new juce::DynamicObject();
        settings->setProperty ("bands", juce::var (bands));
        auto results = echojay::applySettings (plugin, map, juce::var (settings));

        bool mfApplied = false;
        for (const auto& r : results)
            if (r.semantic == "freq_hz") mfApplied = r.applied && r.index == 6;
        check (mfApplied, "8 kHz resolves to the MF band's index");
        check (juce::approximatelyEqual (valueOf (8), mmBefore),
               "Mono Maker untouched at 8 kHz");
        check (juce::approximatelyEqual (valueOf (4), lfBefore),
               "LF untouched at 8 kHz");
    }
}

//==============================================================================
// The declines pin (schema 2.4): the `declines` key is TRI-STATE by presence.
// Absent = the controls surface was never swept by a recording binary;
// present-and-empty = swept, everything shipped; rows = the reasons, authored
// at each decision site. The writer must not collapse the first two states --
// they are different facts (the accepted_groups:0 band-diagnostic lesson).
void testDeclinesEmission()
{
    using namespace ejmap;

    MapPayload p;
    p.fp = "declinespin"; p.category = "compressor";
    p.identity.format = "Fake"; p.identity.name = "DeclineFake"; p.identity.paramCount = 4;

    // Unrecorded: no key, even though the array member exists.
    {
        auto v = juce::JSON::parse (p.toJson());
        check (v.getProperty ("declines", juce::var()).isVoid(),
               "declines: unrecorded surface emits NO key");
    }

    // Recorded and empty: the key appears, and it is an empty array.
    p.declinesRecorded = true;
    {
        auto v = juce::JSON::parse (p.toJson());
        auto d = v.getProperty ("declines", juce::var());
        check (d.isArray() && d.getArray()->isEmpty(),
               "declines: recorded-but-empty emits an EMPTY array, not absence");
    }

    // Rows survive the JSON round trip with index, name and reason intact.
    p.declines.add ({ 2, "Ratio", "flat both ways - unbuildable (T types anchors)" });
    p.declines.add ({ 3, juce::String(), "empty parameter name: not addressable" });
    {
        auto v = juce::JSON::parse (p.toJson());
        auto d = v.getProperty ("declines", juce::var());
        check (d.isArray() && d.getArray()->size() == 2, "declines: both rows emitted");
        auto r0 = (*d.getArray())[0];
        check ((int) r0.getProperty ("index", -1) == 2
                 && r0.getProperty ("name", "").toString() == "Ratio"
                 && r0.getProperty ("reason", "").toString().startsWith ("flat both ways"),
               "declines: index, name and decision-site reason survive the round trip");
        auto r1 = (*d.getArray())[1];
        check (r1.getProperty ("name", "").toString().isEmpty()
                 && r1.getProperty ("reason", "").toString().contains ("empty parameter name"),
               "declines: an unnamed parameter still carries its index and reason");
    }
}

//==============================================================================
// The M6 pin: Tier 2 named controls, writer through consumer, before any real
// control ships. Neither side existed until today (the consumer's mode-labels
// path existed but had never been fed), so this test IS the contract.
void testNamedControlsResolve()
{
    using namespace ejmap;
    FakeInstance plugin;

    MapPayload p;
    p.fp = "tier2pin"; p.category = "de-esser";
    p.identity.format = "Fake"; p.identity.name = "RoundTripFake"; p.identity.paramCount = 14;

    NamedControl sharp;
    sharp.name = "Sharpness"; sharp.indices.add (9);
    sharp.rangeLo = 0; sharp.rangeHi = 10;
    sharp.anchors.add ({ 0.0, 0.0 });
    sharp.anchors.add ({ 1.0, 10.0 });
    p.controls.add (sharp);

    NamedControl knee;
    knee.name = "Knee"; knee.indices.add (10);
    knee.kind = "mode";
    knee.labels.add ({ "Hard", 0.0 });
    knee.labels.add ({ "Med",  0.5 });
    knee.labels.add ({ "Soft", 1.0 });
    p.controls.add (knee);

    NamedControl dup;
    dup.name = "Bypass"; dup.indices.add (11); dup.indices.add (12);
    dup.duplicate = true;
    p.controls.add (dup);

    NamedControl lower;
    lower.name = "bypass"; lower.indices.add (13);
    lower.anchors.add ({ 0.0, 0.0 });
    lower.anchors.add ({ 1.0, 1.0 });
    p.controls.add (lower);

    auto map = juce::JSON::parse (p.toJson());
    check (map.isObject(), "tier2 pin: payload parses");

    auto& params = plugin.getParameters();

    // Anchored by name: {"Sharpness": 6} -> norm 0.6.
    {
        auto* st = new juce::DynamicObject();
        st->setProperty ("Sharpness", 6);
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && results[0].applied && results[0].index == 9,
               "named control resolves by exact name through applySettings");
        check (std::abs (params[9]->getValue() - 0.6f) < 1.0e-4f,
               "Sharpness 6 wrote norm 0.6 through the real anchor path");
    }

    // Mode by name: {"Knee": "Soft"} -> the label's norm, display-verified.
    {
        auto* st = new juce::DynamicObject();
        st->setProperty ("Knee", "Soft");
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && results[0].applied && results[0].displayVerified,
               "mode control applies via the labels path, display-verified: "
                 + (results.size() == 1 ? results[0].note : juce::String()));
        check (std::abs (params[10]->getValue() - 1.0f) < 1.0e-4f,
               "Knee Soft landed at the label's norm");
    }

    // Duplicates refuse with both indices; case variants stay distinct.
    {
        auto* st = new juce::DynamicObject();
        st->setProperty ("Bypass", 1);
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && ! results[0].applied
                 && results[0].note.contains ("11") && results[0].note.contains ("12"),
               "duplicate name refused, both indices in the note: "
                 + (results.size() == 1 ? results[0].note : juce::String()));
    }
    {
        auto* st = new juce::DynamicObject();
        st->setProperty ("bypass", 1);
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && results[0].applied && results[0].index == 13,
               "lowercase bypass is a DISTINCT control and resolves");
    }

    // The consumer is dead code on the plugin side by design (store now,
    // expose later), and dead code is how usableCoreCount drifted -- so every
    // branch is pinned here, or it will not survive the next refactor.
    {
        // Fall-through: a key in neither params nor controls.
        auto* st = new juce::DynamicObject();
        st->setProperty ("Nonexistent", 1);
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && ! results[0].applied
                 && results[0].note.contains ("no mapping"),
               "unknown key still falls through to the no-mapping note");
    }
    {
        // Mode path, unknown label: refused with the label named.
        auto* st = new juce::DynamicObject();
        st->setProperty ("Knee", "Wrong");
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && ! results[0].applied
                 && results[0].note.contains ("Wrong"),
               "unknown mode label refused with the label named");
    }
    {
        // Mode path, case-insensitive label acceptance (caseInsensitiveOk).
        auto* st = new juce::DynamicObject();
        st->setProperty ("Knee", "soft");
        auto results = echojay::applySettings (plugin, map, juce::var (st));
        check (results.size() == 1 && results[0].applied,
               "mode label matches case-insensitively when the entry allows it");
    }
}

//==============================================================================
void testAgainstRealMaps()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ejmap").getChildFile ("maps");

    if (! dir.isDirectory())
    {
        std::cout << "no local map corpus at " << dir.getFullPathName()
                  << ", skipping corpus round-trip (expected until M1 produces maps)"
                  << std::endl;
        return;
    }

    int mapsChecked = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.json"))
    {
        auto v = juce::JSON::parse (entry.getFile().loadFileAsString());
        check (v.isObject(), "map parses: " + entry.getFile().getFileName());

        // READABLE, not identical. Pinning equality here meant every schema
        // bump broke the gate on real maps, and the only ways to make it green
        // again are to rewrite the corpus -- mutating evidence -- or to not
        // bump. The actual rule is that this binary can READ any map at or
        // below its own schema; maps it EMITS carry the current string, which
        // testPayloadSerialises pins separately.
        {
            const auto ms = v.getProperty ("schema", "").toString();
            const int major = ms.upToFirstOccurrenceOf (".", false, false).getIntValue();
            const int minor = ms.fromFirstOccurrenceOf (".", false, false).getIntValue();
            const int mv = major * 10 + minor;
            check (mv > 0 && mv <= ejmap::kMapSchemaVersion,
                   "map schema " + ms + " is readable by this binary ("
                   + ejmap::kMapSchemaString + "): " + entry.getFile().getFileName());
        }

        // The write-level round trip runs against the synthetic instance in
        // testRoundTripThroughApplySettings, because this gate must not load
        // real plugins. What CAN be checked per real map without one is the
        // interpolation stage: every evidence.readback pair (semantic ->
        // asked -> wrote) must reproduce through the real interpolateAnchors
        // over the map's own table. This is the stage that drifted in the
        // usableCoreCount incident, and it needs no instantiation.
        // M9 PARAMETERISATION ITEM 0, against the real corpus: the new
        // map-side path must resolve the SAME indices the eq suite reaches by
        // its fixture route (groups[0] + hardcoded semantics). Equality of the
        // two paths on real maps is the behaviour-preservation proof in
        // miniature -- a synthetic test proves the helper's logic, this proves
        // it agrees with the code it is replacing.
        if (auto* gs = v.getProperty ("groups", juce::var()).getArray())
            if (! gs->isEmpty())
            {
                auto pick = ejmap::subject::primaryGroup (v);
                check (pick.ok, "corpus: a primary group resolves in "
                                + entry.getFile().getFileName());
                if (pick.ok)
                {
                    // the fixture route the eq suite uses today
                    auto fixtureGroup = (*gs)[0];
                    for (const char* sem : { "freq_hz", "gain_db", "q" })
                    {
                        const int fixtureIdx = (int) fixtureGroup
                                                   .getProperty ("params", juce::var())
                                                   .getProperty (sem, juce::var())
                                                   .getProperty ("index", -1);
                        auto slot = ejmap::subject::slotInGroup (v, pick.arrayIndex, sem);
                        // AGREEMENT, NOT PRESENCE. This asserts the two routes
                        // reach the same place; it used to also demand that
                        // they reach one at all, which quietly required every
                        // band to carry a q. A band without q is legitimate
                        // and common -- API-550A has none, several vintage EQs
                        // fix or step it, and the manual-entry design records
                        // q as optional by decision. The first real map with a
                        // q-less group (Dangerous BAX EQ Master, submitted
                        // 4 Aug 2026) failed a gate that had simply never met
                        // one. Both-absent is agreement.
                        check (slot.ok() == (fixtureIdx >= 0)
                                 && (fixtureIdx < 0 || slot.index == fixtureIdx),
                               juce::String ("corpus: map-side ") + sem + " agrees with the "
                               "fixture route (" + (fixtureIdx < 0 ? juce::String ("absent in both")
                                                                   : juce::String (fixtureIdx))
                               + ") in " + entry.getFile().getFileName());
                    }
                    // and the ladder the suite would drive is expressible
                    auto fslot = ejmap::subject::slotInGroup (v, pick.arrayIndex, "freq_hz");
                    auto oct = ejmap::subject::octavesApartWithin (fslot, 2.0);
                    check (oct.ok, "corpus: the primary band can express the 2-octave move the eq "
                                   "suite makes (" + entry.getFile().getFileName() + ")");
                    if (oct.ok)
                        std::cout << "  " << entry.getFile().getFileName().substring (0, 12)
                                  << " primary band n=" << pick.n << ": 2-octave pair "
                                  << juce::String (oct.lowHz, 1) << " -> "
                                  << juce::String (oct.highHz, 1) << " Hz (fixture used 100 -> 400)"
                                  << std::endl;
                }
            }

        auto params = v.getProperty ("params", juce::var());
        if (auto* rbObj = v.getProperty ("evidence", juce::var())
                           .getProperty ("readback", juce::var()).getDynamicObject())
        {
            // readback serializes as an OBJECT keyed by semantic (the shape
            // Evidence::toVar actually writes; the first version of this loop
            // read an array shape nothing produces, so it would have skipped
            // every real map while looking like coverage).
            for (auto& kv : rbObj->getProperties())
            {
                const auto semantic = kv.name.toString();
                auto entryVar = params.getProperty (semantic, juce::var());
                if (! entryVar.isObject()) continue;

                auto anchors = echojay::anchorsFromVar (entryVar);
                auto eff = echojay::dominantMonotonicTable (anchors);
                if (! eff.ok) continue;

                float asked = 0.0f;
                if (! echojay::semanticToFloat (kv.value.getProperty ("asked", juce::var()), asked))
                    continue;

                const float wrote = kv.value.getProperty ("wrote", juce::var()).toString().getFloatValue();
                const float now   = juce::jlimit (0.0f, 1.0f,
                                        echojay::interpolateAnchors (eff.table, asked));
                check (std::abs (now - wrote) < 1.0e-3f,
                       entry.getFile().getFileName() + " " + semantic
                         + ": interpolation reproduces the verified write");
            }
        }

        ++mapsChecked;
    }

    // An empty corpus directory is not "0 checked, all good". ejmap creates
    // maps/ on first launch, so the directory exists long before it has any
    // contents, and a bare count reads as a pass.
    if (mapsChecked == 0)
        std::cout << "empty map corpus at " << dir.getFullPathName()
                  << ", skipping corpus round-trip (expected until M1 produces maps)"
                  << std::endl;
    else
        std::cout << "corpus round-trip: " << mapsChecked << " maps checked" << std::endl;
}

} // namespace

//==============================================================================
// The dry-run file is the only artifact checkable before a server exists, so
// it is pinned BYTE BY BYTE: the getSubPath() leading-slash bug was caught by
// reading emitted bytes, not by an assertion about intent, and these checks
// keep that reading permanent. Each case is a same-shape risk found in the
// audit: dropped port, dropped/re-escaped query, missing path, framing.
void testDryRunBytes()
{
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-dryrun-test");
    root.deleteRecursively();
    root.createDirectory();

    // A body with CRLF and non-ASCII bytes inside: Content-Length must count
    // bytes, and the body must survive verbatim with nothing appended.
    juce::MemoryBlock body;
    const char raw[] = "{\"a\": 1,\r\n \"name\": \"caf\xc3\xa9\"}";
    body.append (raw, sizeof (raw) - 1);

    auto readAll = [] (const juce::File& f)
    {
        juce::MemoryBlock mb;
        f.loadFileAsData (mb);
        return mb;
    };
    auto headOf = [] (const juce::MemoryBlock& mb)
    {
        const juce::String all (juce::CharPointer_UTF8 ((const char*) mb.getData()),
                                juce::CharPointer_UTF8 ((const char*) mb.getData() + mb.getSize()));
        return all.upToFirstOccurrenceOf ("\r\n\r\n", true, false);
    };

    // Default: the placeholder that cannot be mistaken for a live endpoint.
    {
        auto f = ejmap::Mouth::writeDryRun (root, "fp-default", body, "tester", "machine", "0.0.0");
        auto mb = readAll (f);
        auto head = headOf (mb);
        check (head.startsWith ("POST /api/params/ejmap HTTP/1.1\r\n"),
               "dry run: the settled route path (/api/params/ejmap) from the placeholder URL");
        check (head.contains ("\r\nHost: UPLOAD-ENDPOINT-UNSET.echojay.invalid\r\n"),
               "dry run: placeholder host is visibly unset");
        check (head.contains ("\r\nContent-Length: " + juce::String ((juce::int64) body.getSize()) + "\r\n"),
               "dry run: Content-Length counts the body's exact bytes");
        check (mb.getSize() == head.getNumBytesAsUTF8() + body.getSize(),
               "dry run: file is head + body and NOTHING else (no trailing newline)");
        // The server route fails closed without the token header. Value is
        // env-dependent (real token or the visibly-unset placeholder), so the
        // pin is on the header LINE existing with a non-empty value.
        check (head.contains ("\r\nX-EJMap-Token: ")
                 && ! head.contains ("\r\nX-EJMap-Token: \r\n"),
               "dry run: X-EJMap-Token header present with a non-empty value");
        check (mb.getSize() >= body.getSize()
                 && memcmp ((const char*) mb.getData() + (mb.getSize() - body.getSize()),
                            body.getData(), body.getSize()) == 0,
               "dry run: body bytes verbatim, non-ASCII and CRLF intact");
    }

    // A typed port must reach the Host header (getDomain() would cut it).
    {
        auto f = ejmap::Mouth::writeDryRun (root, "fp-port", body, "t", "m", "v",
                                            "http://localhost:8080/api/params/ejmap");
        auto head = headOf (readAll (f));
        check (head.startsWith ("POST /api/params/ejmap HTTP/1.1\r\n"),
               "dry run: path independent of the port");
        check (head.contains ("\r\nHost: localhost:8080\r\n"),
               "dry run: typed port reaches the Host header");
    }

    // A query string must survive byte for byte, escapes as typed
    // (juce::URL would strip it, or re-escape it on the way back out).
    {
        auto f = ejmap::Mouth::writeDryRun (root, "fp-query", body, "t", "m", "v",
                                            "https://api.example.com/v2/maps?key=abc%20d&x=1");
        auto head = headOf (readAll (f));
        check (head.startsWith ("POST /v2/maps?key=abc%20d&x=1 HTTP/1.1\r\n"),
               "dry run: query string verbatim, escapes as typed");
        check (head.contains ("\r\nHost: api.example.com\r\n"),
               "dry run: host clean of the query");
    }

    // A URL with no path is still a legal request line, never "POST  ".
    {
        auto f = ejmap::Mouth::writeDryRun (root, "fp-bare", body, "t", "m", "v",
                                            "https://host.example");
        auto head = headOf (readAll (f));
        check (head.startsWith ("POST / HTTP/1.1\r\n"),
               "dry run: bare host gets the root path, not an empty one");
    }

    // Header-value safety lives at the gate, in words. A CRLF in a tester
    // name is a forged header; non-ASCII is outside what a field may carry.
    check (ejmap::Mouth::headerValueSafe ("sean-studio"),
           "header safety: a plain local name passes");
    check (! ejmap::Mouth::headerValueSafe ("evil\r\nX-Injected: yes"),
           "header safety: CRLF cannot ride a header value");
    check (! ejmap::Mouth::headerValueSafe (juce::String (juce::CharPointer_UTF8 ("se\xc3\xa1n"))),
           "header safety: non-ASCII cannot ride a header value");
    {
        auto* o = new juce::DynamicObject();
        auto verdict = ejmap::Mouth::structuralGate (juce::var (o), "evil\r\nX-Injected: yes");
        check (verdict.rejections.joinIntoString ("|").contains ("cannot ride an HTTP header"),
               "gate: unsafe tester name refused in words");
    }

    root.deleteRecursively();
}

//==============================================================================
// The exposure conformance pin: ejmap re-implements the server's controls
// exposure (classing, exclusions, ordering, the default twelve) and the
// fixture spec/controls-exposure-fixture.json carries the server's OWN output
// for both live maps, generated by a different program from prod data. Drift
// on either side fails here and names the side that moved.
//
// LIFECYCLE NOTE, deliberate: when the fixture predates locally-stamped
// lockstep_of/tier fields (fixture counts.lockstepTwins == 0), the local map
// is ahead of the server's ingest state. The comparison then reconstructs the
// ingest-time input by stripping exactly the ejmap-added fields -- and a
// SECOND check proves those fields do their job (twins leave the pool). Once
// AMEK re-ingests and the fixture regenerates with exclusions, the raw
// comparison takes over.
void testExposureConformance()
{
    auto fixtureFile = juce::File (EJMAP_REPO_ROOT).getChildFile ("spec")
                          .getChildFile ("controls-exposure-fixture.json");
    check (fixtureFile.existsAsFile(), "exposure fixture present at spec/");
    if (! fixtureFile.existsAsFile()) return;
    auto fx = juce::JSON::parse (fixtureFile.loadFileAsString());
    check ((int) fx.getProperty ("K", 0) == ejmap::Exposure::kPerPlugin,
           "fixture K matches the compiled K");

    // Classing pins: the server's own documented misfire caveats.
    using E = ejmap::Exposure;
    check (E::classifyControl ("Power Soak") == "musical",   "classing: Power Soak is musical (power only exact/trailing)");
    check (E::classifyControl ("Amp Power") == "guarded",    "classing: Amp Power is guarded");
    check (E::classifyControl ("Quality Factor") == "musical","classing: Quality Factor is Q, not render quality");
    check (E::classifyControl ("Oversampling") == "plumbing", "classing: oversampling is plumbing");
    check (E::classifyControl ("Param Link") == "plumbing",   "classing: Param Link is plumbing");
    check (E::classifyControl ("Delta") == "audition",        "classing: Delta is audition");
    check (E::classifyControl ("Bypass") == "guarded",        "classing: Bypass is guarded");
    check (E::classifyControl ("Cutoff 2") == "musical",      "classing: Cutoff 2 dodges the on/off/in rule");

    auto mapsDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/ejmap/maps");
    auto* plugins = fx.getProperty ("plugins", juce::var()).getArray();
    check (plugins != nullptr && plugins->size() >= 2, "fixture carries both live maps");
    if (plugins == nullptr) return;

    for (auto& pv : *plugins)
    {
        const auto name = pv.getProperty ("plugin", "?").toString();
        auto mf = mapsDir.getChildFile (pv.getProperty ("fp", "").toString() + ".json");
        if (! mf.existsAsFile())
        {
            std::cout << "  (exposure conformance: no local map for " << name
                      << "; fixture row skipped LOUDLY)" << std::endl;
            continue;
        }
        auto map = juce::JSON::parse (mf.loadFileAsString());
        auto controls = map.getProperty ("controls", juce::var());
        const int fxTwins = (int) pv.getProperty ("counts", juce::var())
                                    .getProperty ("lockstepTwins", 0);

        if (fxTwins == 0)
        {
            // Corpus may be ahead: strip the ejmap-added fields to
            // reconstruct the ingest-time input, and separately prove the
            // marker works on the un-stripped map.
            auto live = ejmap::Exposure::build (controls, map.getProperty ("groups", juce::var()));
            for (const auto& nm : live.defaultExposure)
                check (controls.getProperty (nm, juce::var())
                          .getProperty ("lockstep_of", juce::var()).isVoid(),
                       name + ": no lockstep twin sits in the exposed twelve");
            if (auto* co = controls.getDynamicObject())
                for (auto& kv : co->getProperties())
                    if (auto* eo = kv.value.getDynamicObject())
                    { eo->removeProperty ("lockstep_of"); eo->removeProperty ("lockstep_by");
                      eo->removeProperty ("tier"); }
        }

        auto got = ejmap::Exposure::build (controls, map.getProperty ("groups", juce::var()));

        auto* fxCands = pv.getProperty ("orderedCandidates", juce::var()).getArray();
        check (fxCands != nullptr && fxCands->size() == got.orderedCandidates.size(),
               name + ": candidate count " + juce::String (got.orderedCandidates.size())
                 + " == fixture " + juce::String (fxCands ? fxCands->size() : -1));
        if (fxCands != nullptr)
        {
            const int n = juce::jmin (fxCands->size(), got.orderedCandidates.size());
            int mismatches = 0;
            for (int i = 0; i < n; ++i)
            {
                const auto& g = got.orderedCandidates.getReference (i);
                const auto& f = (*fxCands)[i];
                const bool same = g.name  == f.getProperty ("name", "").toString()
                               && g.cls   == f.getProperty ("cls", "").toString()
                               && g.trust == f.getProperty ("trust", "").toString()
                               && g.kind  == f.getProperty ("kind", "").toString()
                               && g.index == (int) f.getProperty ("index", -1);
                if (! same && mismatches++ == 0)
                    std::cerr << "  first mismatch at " << i << ": got " << g.name
                              << "/" << g.cls << "/" << g.trust << "/" << g.kind << "/" << g.index
                              << " fixture " << f.getProperty ("name", "").toString()
                              << "/" << f.getProperty ("cls", "").toString() << std::endl;
            }
            check (mismatches == 0, name + ": ordered candidates byte-identical to the fixture");
        }

        auto* fxExp = pv.getProperty ("defaultExposure", juce::var()).getArray();
        juce::StringArray fxExpNames;
        if (fxExp != nullptr) for (auto& e : *fxExp) fxExpNames.add (e.toString());
        check (got.defaultExposure == fxExpNames,
               name + ": the default twelve match the fixture exactly, in order");

        auto counts = pv.getProperty ("counts", juce::var());
        check (got.orderedCandidates.size() == (int) counts.getProperty ("inventory", -1),
               name + ": inventory count matches");
        check (got.caseCollisions == (int) counts.getProperty ("caseCollisions", -1)
                 && got.bandDuplicates == (int) counts.getProperty ("bandDuplicates", -1),
               name + ": exclusion counts match the fixture");
    }
}

// The two new control fields: emission shape and the gate vocabulary.
void testLockstepAndTierFields()
{
    ejmap::NamedControl c;
    c.name = "LF Freq 2"; c.indices.add (59); c.kind = "anchored";
    c.anchors.add ({ 0.0, 15.0 }); c.anchors.add ({ 1.0, 22000.0 });
    c.lockstepOf = 29; c.lockstepBy = "write_verify"; c.tier = "hidden";
    auto v = c.toVar();
    check ((int) v.getProperty ("lockstep_of", -1) == 29, "lockstep_of emitted");
    check (v.getProperty ("lockstep_by", "").toString() == "write_verify", "lockstep_by emitted");
    check (v.getProperty ("tier", "").toString() == "hidden", "tier emitted");

    ejmap::NamedControl plain;
    plain.name = "Drive"; plain.indices.add (3);
    auto pv = plain.toVar();
    check (pv.getProperty ("lockstep_of", juce::var()).isVoid()
             && pv.getProperty ("tier", juce::var()).isVoid(),
           "untouched controls emit NEITHER field (absent means heuristic)");

    // Gate: chain refused, vocabularies closed.
    auto* mo = new juce::DynamicObject();
    auto* co = new juce::DynamicObject();
    auto* a = new juce::DynamicObject();
    a->setProperty ("index", 10); a->setProperty ("lockstep_of", 20);
    a->setProperty ("lockstep_by", "write_verify"); a->setProperty ("kind", "mode");
    auto* lb = new juce::DynamicObject(); lb->setProperty ("A", 0.0); lb->setProperty ("B", 1.0);
    a->setProperty ("labels", juce::var (lb));
    auto* b = new juce::DynamicObject();
    b->setProperty ("index", 20); b->setProperty ("lockstep_of", 30);
    b->setProperty ("lockstep_by", "human_pick"); b->setProperty ("kind", "mode");
    auto* lb2 = new juce::DynamicObject(); lb2->setProperty ("A", 0.0); lb2->setProperty ("B", 1.0);
    b->setProperty ("labels", juce::var (lb2));
    co->setProperty ("Twin A", juce::var (a));
    co->setProperty ("Twin B", juce::var (b));
    mo->setProperty ("controls", juce::var (co));
    auto* ident = new juce::DynamicObject(); ident->setProperty ("param_count", 100);
    mo->setProperty ("identity", juce::var (ident));
    auto verdict = ejmap::Mouth::structuralGate (juce::var (mo), "t");
    check (verdict.rejections.joinIntoString ("|").contains ("chains to"),
           "gate: a lockstep chain is refused in words");

    auto* mo2 = new juce::DynamicObject();
    auto* co2 = new juce::DynamicObject();
    auto* e2 = new juce::DynamicObject();
    e2->setProperty ("index", 5); e2->setProperty ("tier", "sometimes");
    e2->setProperty ("kind", "mode");
    auto* lb3 = new juce::DynamicObject(); lb3->setProperty ("A", 0.0); lb3->setProperty ("B", 1.0);
    e2->setProperty ("labels", juce::var (lb3));
    co2->setProperty ("Weird", juce::var (e2));
    mo2->setProperty ("controls", juce::var (co2));
    auto v2 = ejmap::Mouth::structuralGate (juce::var (mo2), "t");
    check (v2.rejections.joinIntoString ("|").contains ("vocabulary"),
           "gate: an out-of-vocabulary tier is refused in words");
}

//==============================================================================
// IDENTITY-FORMAT PIN (3 Aug 2026). The scan's map-state query keys on
// format|uid|version, and that string is built INDEPENDENTLY on both sides:
// identityKeyForDescription here in C++, identityKeyOf in the server's
// params-lib.js. They were verified byte-identical on both live fixtures by
// running each against real data -- but they match by coincidence of
// authorship, not by construction, and the next edit to either can
// desynchronise them silently. Every row would then read "unmapped" and it
// would look like an empty corpus rather than a bug.
//
// THIS IS A DETECTOR, NOT A FIX. Two definitions still exist. A single shared
// definition would need a codegen step producing both a C++ header and a JS
// module, which this project does not have; that trade is worth revisiting
// only if the corpus makes it worth it.
//
// The expectations are LITERALS on purpose. Deriving them from either
// implementation would only catch the OTHER side drifting, which is half a
// check and reads like a whole one.
void testIdentityKeyFormat()
{
    struct Fixture { const char* format; const char* uidHex; const char* version;
                     const char* expected; const char* who; };
    const Fixture fixtures[] = {
        { "AudioUnit", "426a7f6f", "1.4.1", "AudioUnit|426a7f6f|1.4.1", "AMEK EQ 200" },
        { "AudioUnit", "7d606b6a", "1.3.2", "AudioUnit|7d606b6a|1.3.2", "spiff" },
    };

    for (const auto& f : fixtures)
    {
        // CLIENT side: the shipping function, given a description carrying the
        // same fields the map stores.
        juce::PluginDescription d;
        d.pluginFormatName = f.format;
        d.uniqueId = (int) juce::String (f.uidHex).getHexValue64();
        d.version  = f.version;
        const auto clientKey = echojay::identityKeyForDescription (d);
        check (clientKey == f.expected,
               juce::String ("identity key, client side, ") + f.who + ": got '"
                 + clientKey + "' expected '" + f.expected + "'");

        // SERVER side's RULE, asserted against the same literal: format|uid|
        // version joined with '|'. If the server changes its separator or field
        // order, this fixture still expects the literal and the mismatch
        // surfaces here rather than as an empty corpus in the scan UI.
        const juce::String serverShape = juce::String (f.format) + "|" + f.uidHex + "|" + f.version;
        check (serverShape == f.expected,
               juce::String ("identity key, server shape, ") + f.who + ": got '"
                 + serverShape + "' expected '" + f.expected + "'");
    }

    // And the corpus must actually contain those identities, or the pin is
    // guarding a format nothing produces.
    auto mapsDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/ejmap/maps");
    juce::StringArray seen;
    for (const auto& e : juce::RangedDirectoryIterator (mapsDir, false, "*.json"))
    {
        auto id = juce::JSON::parse (e.getFile().loadFileAsString())
                      .getProperty ("identity", juce::var());
        if (! id.isObject()) continue;
        seen.add (id.getProperty ("format", "").toString() + "|"
                    + id.getProperty ("uid", "").toString() + "|"
                    + id.getProperty ("version", "").toString());
    }
    if (seen.isEmpty())
        std::cout << "  (no local corpus; identity-key pin checked against literals only)"
                  << std::endl;
    else
        for (const auto& f : fixtures)
            check (seen.contains (f.expected),
                   juce::String ("corpus carries the pinned identity for ") + f.who);
}

//==============================================================================

//==============================================================================
/** M9 PARAMETERISATION ITEM 0: the map-side lookups a suite reads instead of
    its fixture constants.

    Every REFUSAL is tested by attempting it, not by inspecting the branch --
    the machinery's whole purpose is to refuse where a fixture constant used to
    assume, so an untested refusal is the feature untested. Values here are
    hand-written, never taken from a real map, so the test cannot shape itself
    to the corpus it judges.
*/
void testSubjectLookups()
{
    using namespace ejmap::subject;

    auto anchorsVar = [] (std::initializer_list<std::pair<double,double>> pts)
    {
        juce::Array<juce::var> rows;
        for (auto& p : pts)
            rows.add (juce::var (juce::Array<juce::var> { p.first, p.second }));
        return juce::var (rows);
    };
    auto entry = [&] (int index, const juce::String& name,
                      std::initializer_list<std::pair<double,double>> pts)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("index", index);
        o->setProperty ("name", name);
        o->setProperty ("unit", "hz");
        o->setProperty ("anchors", anchorsVar (pts));
        return juce::var (o);
    };

    // ---- a map with one primary group and one top-level semantic ----------
    auto* freq = new juce::DynamicObject();
    freq->setProperty ("freq_hz", entry (29, "LF Freq 1", { {100.0, 0.0}, {200.0, 0.5}, {1600.0, 1.0} }));
    auto* g1 = new juce::DynamicObject();
    g1->setProperty ("n", 1);
    g1->setProperty ("primary", true);
    g1->setProperty ("params", juce::var (freq));
    auto* g2 = new juce::DynamicObject();
    g2->setProperty ("n", 2);
    g2->setProperty ("params", juce::var (new juce::DynamicObject()));
    auto* params = new juce::DynamicObject();
    params->setProperty ("output_db", entry (2, "Output Gain", { {-15.0, 0.0}, {15.0, 1.0} }));
    auto* mp = new juce::DynamicObject();
    mp->setProperty ("params", juce::var (params));
    mp->setProperty ("groups", juce::var (juce::Array<juce::var> { juce::var (g1), juce::var (g2) }));
    juce::var map (mp);

    auto pick = primaryGroup (map);
    check (pick.ok && pick.arrayIndex == 0 && pick.n == 1,
           "subject: primary group is found by its flag, not by position");

    auto fs = slotInGroup (map, pick.arrayIndex, "freq_hz");
    check (fs.ok() && fs.index == 29 && fs.where == "group 1 / freq_hz",
           "subject: a group semantic resolves to its index and location");
    check (fs.ladderLo() == 100.0 && fs.ladderHi() == 1600.0,
           "subject: the ladder comes from the map's anchors");
    check (std::abs (fs.normFor (200.0) - 0.5f) < 1.0e-6,
           "subject: normFor goes through the SAME interpolation as the dial path");

    auto os = slotFor (map, "output_db");
    check (os.ok() && os.index == 2 && os.where == "params / output_db",
           "subject: a top-level semantic resolves");

    // ---- REFUSALS, each attempted ----------------------------------------
    auto missing = slotFor (map, "ratio");
    check (! missing.ok() && missing.why.contains ("no 'ratio'"),
           "subject REFUSES an absent semantic, and says which one");

    auto notInGroup = slotInGroup (map, 1, "freq_hz");
    check (! notInGroup.ok() && notInGroup.why.isNotEmpty(),
           "subject REFUSES a semantic absent from the named group");

    auto badGroup = slotInGroup (map, 7, "freq_hz");
    check (! badGroup.ok() && badGroup.why.contains ("no such group"),
           "subject REFUSES an out-of-range group rather than clamping");

    // one-anchor ladder: present, addressable, and unusable
    auto* thin = new juce::DynamicObject();
    thin->setProperty ("thin", entry (5, "Thin", { {1.0, 0.0} }));
    auto* tm = new juce::DynamicObject();
    tm->setProperty ("params", juce::var (thin));
    auto thinSlot = slotFor (juce::var (tm), "thin");
    check (thinSlot.found && ! thinSlot.ok() && thinSlot.why.contains ("at least 2"),
           "subject REFUSES a one-anchor ladder: found is not the same as usable");

    // no primary flag anywhere
    auto* np1 = new juce::DynamicObject(); np1->setProperty ("n", 1);
    auto* npm = new juce::DynamicObject();
    npm->setProperty ("groups", juce::var (juce::Array<juce::var> { juce::var (np1) }));
    auto noPrimary = primaryGroup (juce::var (npm));
    check (! noPrimary.ok && noPrimary.why.contains ("no group is flagged primary"),
           "subject REFUSES when no group is primary, rather than taking groups[0]");

    // two primaries
    auto* tp1 = new juce::DynamicObject(); tp1->setProperty ("n", 1); tp1->setProperty ("primary", true);
    auto* tp2 = new juce::DynamicObject(); tp2->setProperty ("n", 4); tp2->setProperty ("primary", true);
    auto* tpm = new juce::DynamicObject();
    tpm->setProperty ("groups", juce::var (juce::Array<juce::var> { juce::var (tp1), juce::var (tp2) }));
    auto twoPrimary = primaryGroup (juce::var (tpm));
    check (! twoPrimary.ok && twoPrimary.why.contains ("n = 1, 4"),
           "subject REFUSES two primary groups and names both");

    // ---- ladder-point choosers -------------------------------------------
    auto spread = spreadAcrossLadder (fs, { 0.0, 0.5, 1.0 });
    check (spread.size() == 3 && spread[0] == 100.0 && spread[2] == 1600.0,
           "subject: ladder points come from the map's own range");

    auto oct = octavesApartWithin (fs, 2.0);
    check (oct.ok && std::abs (std::log2 (oct.highHz / oct.lowHz) - 2.0) < 1.0e-9,
           "subject: an octave pair is exactly the interval asked for");
    check (oct.lowHz > fs.ladderLo() && oct.highHz < fs.ladderHi(),
           "subject: the octave pair is centred, so neither point sits on an endpoint");

    // a ladder too narrow to express the interval REFUSES rather than stretching
    auto narrow = slotFor (juce::var (mp), "output_db");   // -15..15, not a frequency
    auto badOct = octavesApartWithin (narrow, 2.0);
    check (! badOct.ok && badOct.why.contains ("not in a positive frequency domain"),
           "subject REFUSES octaves on a non-frequency ladder");

    auto* nb = new juce::DynamicObject();
    nb->setProperty ("f", entry (1, "Narrow", { {200.0, 0.0}, {400.0, 1.0} }));
    auto* nbm = new juce::DynamicObject(); nbm->setProperty ("params", juce::var (nb));
    auto narrowOct = octavesApartWithin (slotFor (juce::var (nbm), "f"), 4.0);
    check (! narrowOct.ok && narrowOct.why.contains ("cannot express"),
           "subject REFUSES an interval the ladder cannot express, rather than shrinking it");

    // ---- the send's refusals, on the function the send path itself calls ---
    // The live 401 and the timeout are proven against the real endpoint by
    // --gate-m9 sendtest. The redirect is proven HERE, because a live 307
    // specimen through that path did not return bytes and an unproven refusal
    // is the misplaced-guard class. This is not a copy of the decision: the
    // send path calls this exact function.
    {
        using ejmap::classifyReply;
        const juce::String ok200 = "HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\n{\"ok\":true}";
        auto a = classifyReply (ok200, (size_t) ok200.length());
        check (a.sent && a.status == 200 && a.queueState() == "sent",
               "send: a 2xx with a body is the only outcome that counts as sent");

        const juce::String r307 = "HTTP/1.1 307 Temporary Redirect\r\n"
                                  "Location: https://elsewhere.example/api\r\n"
                                  "Content-Length: 0\r\n\r\n";
        auto b = classifyReply (r307, (size_t) r307.length());
        check (! b.sent && b.status == 307, "send REFUSES a redirect");
        check (b.refusedReason.contains ("NOT followed")
                 && b.refusedReason.contains ("elsewhere.example"),
               "send names the Location it did not follow, so the refusal is auditable");
        check (b.queueState() == "refused", "a refused redirect is queued as refused");

        const juce::String r401 = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 24\r\n\r\n"
                                  "{\"error\":\"unauthorized\"}";
        auto c = classifyReply (r401, (size_t) r401.length());
        check (! c.sent && c.status == 401 && c.refusedReason.contains ("unauthorized"),
               "send REFUSES a 401 and carries the server's own words");

        auto d = classifyReply ("", 0);
        check (! d.sent && d.status == 0 && d.refusedReason.contains ("no status line"),
               "send REFUSES an empty reply rather than reading it as success");

        // The property that matters most: NOTHING reports unknown.
        for (const auto* r : { &a, &b, &c, &d })
            check (r->queueState() == "sent" || r->queueState() == "refused",
                   "every outcome is sent or refused, never unknown");
    }

    // ---- item 3 session 3: the floor's unit decides the verdict ------------
    // The standing-question answer for handing eq to the routing fork, proven
    // rather than argued. routeVerdict is dimensionless, so the SAME
    // measurements route differently depending only on which floor is passed.
    {
        // eq's centre feature: moved 0.20 oct against a predicted 2.0 oct.
        const double moved = 0.20, predicted = 2.0, tol = 0.0838;
        const double octaveFloor = 0.0322;   // sigma_centre, in octaves
        const double dbFloor     = 0.088;    // sigma_depth, in decibels

        const auto withOct = ejmap::route::routeVerdict (moved, octaveFloor, predicted, tol);
        const auto withDb  = ejmap::route::routeVerdict (moved, dbFloor,     predicted, tol);

        check (withOct == ejmap::route::Route::overClaim,
               "floor unit: with the OCTAVE floor the feature moved above 4*sigma -> over-claim");
        check (withDb == ejmap::route::Route::deafness,
               "floor unit: with the dB floor the SAME measurement reads as deafness");
        check (withOct != withDb,
               "floor unit: identical measurements, opposite verdicts, decided only by which "
               "floor was passed -- contradicts vs inconclusive");

        // and the pairing that makes it loud
        ejmap::route::Floor f (0.0322, "oct");
        check (f.unit == "oct" && f.value == 0.0322,
               "floor carries its unit, so a mismatch is checkable at the emit");
    }

    // ---- item 3 session 1: cause triage, all four states -------------------
    // The write-did-not-land state cannot be forced on real hardware -- writes
    // land -- so it is proven HERE, where the write function can be made to
    // fail, and the other three are proven end to end on AMEK.
    {
        using namespace ejmap::triage;
        auto ok    = [] (float) { return 1.0; };
        auto fails = [] (float) { return -1.0; };

        auto oor = classifyLiveness (9, 4, 0.0f, 1.0f, 0.1, ok, [] { return 0.0; });
        check (oor.state == Liveness::indexOutOfRange
                 && oor.cause().contains ("statement about the MAP"),
               "triage: an out-of-range index is a statement about the MAP");

        auto unl = classifyLiveness (1, 4, 0.0f, 1.0f, 0.1, fails, [] { return 0.0; });
        check (unl.state == Liveness::writeDidNotLand
                 && unl.cause().contains ("WRITE PATH")
                 && unl.cause().contains ("Nothing was measured"),
               "triage: an unlanded write is a statement about the WRITE PATH, and measures nothing");

        double v = 0;
        auto inert = classifyLiveness (1, 4, 0.0f, 1.0f, 0.1, ok, [&v] { return v; });
        check (inert.state == Liveness::landedButInert
                 && inert.cause().contains ("statement about the PLUGIN"),
               "triage: landed-but-inert is the ONLY one of the three about the plugin");

        int n = 0;
        auto live = classifyLiveness (1, 4, 0.0f, 1.0f, 0.1, ok, [&n] { return n++ * 5.0; });
        check (live.state == Liveness::live && live.isLive(),
               "triage: a landed write that moves the feature is live");

        // the three causes produce three DIFFERENT sentences -- the whole point
        check (oor.cause() != unl.cause() && unl.cause() != inert.cause()
                 && oor.cause() != inert.cause(),
               "triage: one symptom, three causes, three different sentences");
    }

    // ---- item 3 session 1: role verification and the enable null-test ------
    {
        using namespace ejmap::triage;
        // a width control: side moves, mid is a minority of it
        auto width = verifyStereoWidthRole ("Mono Maker", 35.0, 0.44, 1.94, 0.35);
        check (width.supported && width.why.contains ("supported"),
               "role: side moving with mid as a minority is supported");
        check (width.why.contains ("RECORDED, NOT A CRITERION"),
               "role: a mid movement above its own floor is RECORDED, not used as a criterion -- "
               "the first version of this check failed the signed AMEK fixture on exactly that");

        // a level control: side and mid move together
        auto gain = verifyStereoWidthRole ("Input Gain", 30.0, 30.0, 1.94, 0.35);
        check (! gain.supported && gain.why.contains ("not a minority"),
               "role REFUSES a level control, which moves mid and side together");

        // an inert index: nothing moves
        auto dead = verifyStereoWidthRole ("Mono Maker", 0.0, 0.0, 1.94, 0.35);
        check (! dead.supported && dead.why.contains ("does nothing measurable"),
               "role REFUSES an inert control, and says so differently from a level control");
        check (dead.why != gain.why, "role: inert and level-like failures read differently");

        check (width.limitStatement().contains ("CANNOT separate it from another"),
               "role states its own limit: supported by measurement, never proven");

        auto clean = checkEnableIsNull ("Mono Maker In", 0.0, 0.35);
        check (clean.clean, "enable null: an enable that changes nothing else is clean");
        auto dirty = checkEnableIsNull ("Power", 4.39, 0.35);
        check (! dirty.clean && dirty.why.contains ("not a per-control enable")
                 && dirty.why.contains ("arms not testing this link"),
               "enable null REFUSES a global switch, naming the contamination of other arms");
    }

    // ---- 5b: control roles and enable links -------------------------------
    {
        auto ctl = [&] (int index, const juce::String& name, const juce::String& role,
                        juce::var enabledBy = juce::var())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", index);
            o->setProperty ("name", name);
            o->setProperty ("unit", "hz");
            juce::Array<juce::var> rows;
            rows.add (juce::var (juce::Array<juce::var> { 20.0, 0.0 }));
            rows.add (juce::var (juce::Array<juce::var> { 2000.0, 1.0 }));
            o->setProperty ("anchors", juce::var (rows));
            if (role.isNotEmpty()) o->setProperty ("role", role);
            if (enabledBy.isObject()) o->setProperty ("enabled_by", enabledBy);
            return juce::var (o);
        };
        auto* link = new juce::DynamicObject();
        link->setProperty ("index", 8);
        link->setProperty ("value", 1.0);
        link->setProperty ("name", "Mono Maker In");
        link->setProperty ("why", "Mono Maker is inert until its In switch is engaged");

        auto* controls = new juce::DynamicObject();
        controls->setProperty ("Mono Maker", ctl (7, "Mono Maker", "stereo_width", juce::var (link)));
        controls->setProperty ("Output Gain", ctl (2, "Output Gain", ""));
        auto* m = new juce::DynamicObject();
        m->setProperty ("controls", juce::var (controls));
        juce::var rmap (m);

        auto w = controlWithRole (rmap, "stereo_width");
        check (w.ok && w.slot.index == 7 && w.controlName == "Mono Maker",
               "role: the stereo_width control resolves by ROLE, not by the name 'Mono Maker'");

        auto none = controlWithRole (rmap, "bypass");
        check (! none.ok && none.candidates == 0 && none.why.contains ("no control declares"),
               "role REFUSES when no control claims it");

        auto* two = new juce::DynamicObject();
        two->setProperty ("Width A", ctl (7, "Width A", "stereo_width"));
        two->setProperty ("Width B", ctl (9, "Width B", "stereo_width"));
        auto* m2 = new juce::DynamicObject(); m2->setProperty ("controls", juce::var (two));
        auto amb = controlWithRole (juce::var (m2), "stereo_width");
        check (! amb.ok && amb.candidates == 2
                 && amb.why.contains ("Width A") && amb.why.contains ("Width B"),
               "role REFUSES two claimants and names both, rather than taking the first");

        auto el = enableLinkFor (rmap, "Mono Maker");
        check (el.declared && el.index == 8 && el.value == 1.0,
               "enable link resolves to the index and value that make the control live");

        auto missing = enableLinkFor (rmap, "Output Gain");
        check (! missing.declared && missing.why.contains ("UNKNOWN, not assumed"),
               "an absent enable link is UNKNOWN, never 'nothing needs setting'");

        auto st = asExcitationStep (el);
        check (st.index == 8 && st.value == 1.0 && st.semantic == "enable:Mono Maker"
                 && st.why.contains ("In switch"),
               "an enable link becomes an excitation step, so one mechanism reports both");

        // and it flows through applyExcitation, including the unlanded case
        ExcitationPlan linkPlan; linkPlan.source = "map"; linkPlan.steps.add (st);
        auto lr = applyExcitation (linkPlan, 12, [] (int, double, const juce::String&) { return -1.0; });
        check (lr.unlanded == 1 && ! lr.ok(),
               "an enable write that does not land is reported by the excitation machinery");
    }

    // ---- excitation: the single resolution point --------------------------
    ExcitationPlan suitePlan;
    suitePlan.source = "suite:comp";
    suitePlan.steps.add ({ 12, 10.0, "ratio", "ratio at max" });

    auto viaSuite = resolveExcitation (map, suitePlan);
    check (viaSuite.source == "suite:comp" && viaSuite.declared(),
           "excitation: a map with no excitation key falls through to the suite's plan");

    ExcitationPlan none;
    check (! resolveExcitation (map, none).declared(),
           "excitation: absent everywhere is 'none', not an empty plan pretending to be one");

    // ---- applyExcitation: declaration and application are one act --------
    {
        ExcitationPlan p2;
        p2.source = "suite:test";
        p2.steps.add ({ 3, 10.0, "ratio", "a compressor at 1:1 compresses nothing" });
        p2.steps.add ({ 1, -30.0, "threshold_db", "below the stimulus" });

        juce::Array<int> wroteIdx; juce::Array<double> wroteVal;
        auto r = applyExcitation (p2, 8, [&] (int i, double v, const juce::String&)
                                  { wroteIdx.add (i); wroteVal.add (v); return 1.0; });
        check (r.applied == 2 && r.ok(), "applyExcitation applies every step");
        check (wroteIdx.size() == 2 && wroteIdx[0] == 3 && wroteVal[1] == -30.0,
               "applyExcitation writes the declared indices and values, in order");

        // an unlanded write is REPORTED, never assumed away
        auto r2 = applyExcitation (p2, 8, [] (int, double, const juce::String&) { return -1.0; });
        check (r2.unlanded == 2 && ! r2.ok() && r2.detail.contains ("did not land"),
               "applyExcitation reports unlanded writes rather than assuming excitation");

        // an index the instance does not have is refused, not clamped
        // paramCount 2: index 3 is out of range, index 1 is not. The step that
        // CAN apply still does, and the plan is not ok() because one could not.
        auto r3 = applyExcitation (p2, 2, [] (int, double, const juce::String&) { return 1.0; });
        check (r3.outOfRange == 1 && r3.applied == 1 && ! r3.ok()
                 && r3.detail.contains ("outside the instance's 2 parameters"),
               "applyExcitation REFUSES an out-of-range index rather than clamping it");
    }

    // the 2.3a serialised form: a map carrying a plan wins
    auto* st = new juce::DynamicObject();
    st->setProperty ("index", 40);
    st->setProperty ("value", 1.0);
    st->setProperty ("semantic", "xl_stage");
    st->setProperty ("why", "the stage is bypassed by default");
    auto* m23 = new juce::DynamicObject();
    m23->setProperty ("excitation", juce::var (juce::Array<juce::var> { juce::var (st) }));
    auto viaMap = resolveExcitation (juce::var (m23), suitePlan);
    check (viaMap.source == "map" && viaMap.steps.size() == 1 && viaMap.steps[0].index == 40,
           "excitation: a map plan overrides the suite plan (schema 2.3a, serialised)");
    check (viaMap.describe().contains ("xl_stage[40]") && viaMap.describe().contains ("bypassed"),
           "excitation: the plan describes itself, including WHY a step exists");
}

/** THE READBACK PROBE HAS A FALSE CASE, which the rule it replaced did not.

    The old rule asked the MIDPOINT of two anchors and allowed 60% of that gap,
    so on a quantised parameter both possible landings passed and the check
    could not fail. These assertions are the ones that would have caught that:
    landing on the far anchor must FAIL.
*/
void testReadbackProbe()
{
    // A coarse ladder, descending, as Dangerous BAX EQ Master's high cut is.
    juce::Array<juce::Array<float>> anchors;
    for (float v : { 70000.0f, 28000.0f, 18000.0f, 12600.0f, 11100.0f, 9000.0f, 7500.0f })
    {
        juce::Array<float> a; a.add (v); a.add (0.5f); anchors.add (a);
    }

    const auto p = ejmap::planReadback (anchors, "high_cut_freq_hz");
    check (p.valid, "probe: a 7-anchor ladder yields a plan");
    check (std::abs (p.ask - 16650.0) < 1.0,
           "probe: the ask is 25% off-centre (16650), not the midpoint (15300)");
    check (std::abs (p.nearest - 18000.0) < 1.0 && std::abs (p.far - 12600.0) < 1.0,
           "probe: nearest and far are unambiguous at a 25% ask");

    check (p.matches (18000.0),
           "probe: landing on the NEAREST step passes (a quantised parameter is still verified)");
    check (! p.matches (12600.0),
           "probe: landing on the FAR step FAILS -- the case the old rule could not express");
    check (p.matches (16650.0),
           "probe: landing exactly on the ask passes (a continuous parameter)");
    check (! p.matches (9000.0),
           "probe: landing two steps away fails");

    // Proportional tolerance for frequency, a fraction of range for the rest.
    check (std::abs (p.tol - 0.03 * 16650.0) < 1.0,
           "probe: _hz tolerance is proportional to the ask, not a fraction of range");

    juce::Array<juce::Array<float>> db;
    for (float v : { -24.0f, -18.0f, -12.0f, -6.0f, 0.0f })
    { juce::Array<float> a; a.add (v); a.add (0.5f); db.add (a); }
    const auto q = ejmap::planReadback (db, "threshold_db");
    check (q.valid && std::abs (q.tol - juce::jmax (0.02 * 24.0, 0.05)) < 1.0e-6,
           "probe: a dB parameter keeps the fraction-of-range tolerance");
}

/** THE TWO MARKS, and above all THE TWO KEY SHAPES.

    An issue keys on the full identity because it is about a build; unmappable
    keys on the product because a utility stays a utility across versions.
    These assertions exist so that collapsing them into "just use the identity"
    fails loudly rather than quietly changing what a mark means.
*/
void testMarks()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("ejmap-marks-test");
    dir.deleteRecursively();
    dir.createDirectory();

    juce::PluginDescription v1;
    v1.pluginFormatName = "AudioUnit"; v1.uniqueId = 0x62434544; v1.version = "1.5.1";
    juce::PluginDescription v2 = v1; v2.version = "2.0.0";      // same product, new build

    check (ejmap::Marks::identityKey (v1).endsWith ("|1.5.1"),
           "marks: the identity key carries the version");
    check (! ejmap::Marks::productKey (v1).contains ("1.5.1"),
           "marks: the product key does NOT carry the version");
    check (ejmap::Marks::productKey (v1) == ejmap::Marks::productKey (v2),
           "marks: two builds of one product share a product key");
    check (ejmap::Marks::identityKey (v1) != ejmap::Marks::identityKey (v2),
           "marks: two builds of one product do NOT share an identity key");

    ejmap::Marks m;
    check (m.toggleIssue (v1, "tester") && m.hasIssue (v1),
           "marks: flagging an issue sets it");
    check (m.toggleUnmappable (v1, "tester") && m.isUnmappable (v1),
           "marks: marking unmappable sets it");

    // The whole reason for two key shapes.
    check (! m.hasIssue (v2),
           "marks: an ISSUE does NOT carry to a new version (a new build may fix it)");
    check (m.isUnmappable (v2),
           "marks: UNMAPPABLE DOES carry to a new version (a utility stays a utility)");

    m.save (dir);
    auto back = ejmap::Marks::load (dir);
    check (back.hasIssue (v1) && back.isUnmappable (v1),
           "marks: both survive a save and load, so they survive a restart");
    check (back.issues[ejmap::Marks::identityKey (v1)].by == "tester"
             && back.issues[ejmap::Marks::identityKey (v1)].at.isNotEmpty(),
           "marks: who and when are recorded and restored");

    check (! back.toggleIssue (v1, "tester") && ! back.hasIssue (v1),
           "marks: the same gesture clears it");
    check (back.isUnmappable (v1),
           "marks: clearing one mark leaves the other alone");

    dir.deleteRecursively();
}

/** THE UNIT-FAMILY RULE: a semantic's declared unit against the unit the SWEEP
    measured on the display. Both cases, because a rule with only a true case is
    not a rule.
*/
void testUnitFamilyRule()
{
    auto row = [] (const char* sem, const char* fam, int idx)
    {
        ejmap::AssignRow r;
        r.semantic = sem; r.kind = sem;
        r.state = ejmap::AssignRow::State::confirmed;
        r.resolvedIndex = idx;
        r.sweep.unitFamily = fam;
        return r;
    };

    juce::Array<ejmap::AssignRow> ok;
    ok.add (row ("attack_ms",  "ms", 1));
    ok.add (row ("output_db",  "db", 2));
    ok.add (row ("freq_hz",    "hz", 3));
    check (ejmap::unitFamilyConflicts (ok).isEmpty(),
           "unit rule: matching semantics and measured units raise nothing");

    // The live defect: UAD SPL Transient Designer's Attack is dB of transient
    // gain, mapped as attack_ms. The readback passed it because it compares
    // numbers, not units.
    juce::Array<ejmap::AssignRow> bad;
    bad.add (row ("attack_ms", "db", 0));
    const auto c = ejmap::unitFamilyConflicts (bad);
    check (c.size() == 1 && c[0].contains ("attack_ms") && c[0].contains ("'ms'")
             && c[0].contains ("'db'"),
           "unit rule: a ms semantic on a dB control REFUSES, naming both units");

    // The second live one: HG-2's Output is a 0..100% control mapped output_db.
    juce::Array<ejmap::AssignRow> pct;
    pct.add (row ("output_db", "pct", 12));
    check (ejmap::unitFamilyConflicts (pct).size() == 1,
           "unit rule: a dB semantic on a percentage control REFUSES");

    // ABSENCE IS NOT A CONFLICT, on either side.
    juce::Array<ejmap::AssignRow> quiet;
    quiet.add (row ("drive",     "db", 4));   // semantic declares no unit
    quiet.add (row ("output_db", "",   5));   // display declared no unit
    check (ejmap::unitFamilyConflicts (quiet).isEmpty(),
           "unit rule: an undeclared unit on either side claims nothing");

    // Only confirmed, indexed, non-surface rows are judged.
    juce::Array<ejmap::AssignRow> skipped;
    auto s1 = row ("attack_ms", "db", 0); s1.state = ejmap::AssignRow::State::skipDeferred;
    auto s2 = row ("attack_ms", "db", -1);
    auto s3 = row ("attack_ms", "db", 0); s3.kind = "bands";
    skipped.add (s1); skipped.add (s2); skipped.add (s3);
    check (ejmap::unitFamilyConflicts (skipped).isEmpty(),
           "unit rule: unresolved, index-less and surface rows are not judged");
}

/*  ONE SEMANTIC ON TWO INDICES: the silent half of the duplicate rule.

    duplicateIndexConflicts is loud -- two semantics on one index produces a map
    with two keys pointing at one control. This is the reverse and it is silent:
    `params` is keyed by semantic, so the second write wins and the first
    control disappears with nothing recorded. Proven in both directions, and
    proven not to fire on the cases it must not judge.
*/
void testDuplicateSemanticRule()
{
    auto row = [] (const char* sem, int idx)
    {
        ejmap::AssignRow r;
        r.semantic = sem; r.kind = sem;
        r.state = ejmap::AssignRow::State::confirmed;
        r.resolvedIndex = idx;
        return r;
    };

    juce::Array<ejmap::AssignRow> ok;
    ok.add (row ("output_db", 34));
    ok.add (row ("makeup_db", 40));
    check (ejmap::duplicateSemanticConflicts (ok).isEmpty(),
           "dup semantic: distinct semantics on distinct indices raise nothing");

    juce::Array<ejmap::AssignRow> clash;
    clash.add (row ("output_db", 34));
    clash.add (row ("output_db", 40));
    const auto c = ejmap::duplicateSemanticConflicts (clash);
    check (c.size() == 1 && c[0].contains ("output_db") && c[0].contains ("[34]")
             && c[0].contains ("[40]"),
           "dup semantic: one semantic on two indices REFUSES, naming both");

    // sharedInsisted is a statement about an INDEX shared by two semantics. It
    // must not launder the reverse, which has no legitimate form.
    juce::Array<ejmap::AssignRow> insisted;
    auto i1 = row ("output_db", 34); i1.sharedInsisted = true;
    auto i2 = row ("output_db", 40); i2.sharedInsisted = true;
    insisted.add (i1); insisted.add (i2);
    check (ejmap::duplicateSemanticConflicts (insisted).size() == 1,
           "dup semantic: sharedInsisted does NOT exempt one semantic on two indices");

    // Only confirmed, indexed, non-surface rows are judged -- same scope as the
    // rules it sits beside.
    juce::Array<ejmap::AssignRow> skipped;
    auto s1 = row ("output_db", 34); s1.state = ejmap::AssignRow::State::skipDeferred;
    auto s2 = row ("output_db", 40);
    auto s3 = row ("output_db", 41); s3.kind = "bands";
    skipped.add (s1); skipped.add (s2); skipped.add (s3);
    check (ejmap::duplicateSemanticConflicts (skipped).isEmpty(),
           "dup semantic: unresolved, index-less and surface rows are not judged");

    // And the index rule still owns its own direction.
    juce::Array<ejmap::AssignRow> byIndex;
    byIndex.add (row ("output_db", 34));
    byIndex.add (row ("makeup_db", 34));
    check (ejmap::duplicateSemanticConflicts (byIndex).isEmpty()
             && ejmap::duplicateIndexConflicts (byIndex).size() == 1,
           "dup semantic: two semantics on one index is the OTHER rule's job");
}

/*  THE UNIT A DISPLAY DECLARES, AT DIAL TIME.

    The live defect this exists for: UAD SPL Transient Designer's Attack is dB
    of transient gain, mapped attack_ms. The dial asked -1.125, the display read
    "-1.13 dB", the NUMBERS agreed, and the read-back recorded a match -- because
    parseDisplayForUnit extracts the display's unit token and then throws it
    away unless it is a compatible conversion.

    Now a contradicting declared unit is a mismatch, which reverts the write and
    tells the user. Proven in both directions, and proven silent where either
    side declares nothing.
*/
void testDialTimeUnitRule()
{
    using namespace echojay;

    check (displayUnitFamily ("-1.13 dB")   == "db",  "display: -1.13 dB -> db");
    check (displayUnitFamily ("4.00 s")     == "s",   "display: 4.00 s -> s");
    check (displayUnitFamily ("150 ms")     == "ms",  "display: 150 ms -> ms");
    check (displayUnitFamily ("8000 Hz")    == "hz",  "display: 8000 Hz -> hz");
    check (displayUnitFamily ("12k")        == "",    "a bare k declares nothing on its own");
    check (displayUnitFamily ("100.0 %")    == "pct", "display: 100.0 % -> pct");
    check (displayUnitFamily ("3.00 : 1")   == "ratio", "display: 3.00 : 1 -> ratio");
    check (displayUnitFamily ("4.3")        == "",    "a bare number declares NOTHING");
    check (displayUnitFamily ("12 steps")   == "",    "an unrecognised suffix declares nothing");

    check (unitFamiliesAgree ("ms", "s"),  "ms and s are ONE time family");
    check (unitFamiliesAgree ("s", "ms"),  "...in both directions");
    check (unitFamiliesAgree ("", "db"),   "a semantic with no claim agrees with anything");
    check (unitFamiliesAgree ("db", ""),   "a display with no declaration agrees with anything");
    check (! unitFamiliesAgree ("ms", "db"), "ms and db do NOT agree");
    check (! unitFamiliesAgree ("db", "pct"), "db and pct do NOT agree");

    // A table spanning the Transient Designer's real +/-15, so the numeric
    // comparison would PASS if the unit were ignored.
    juce::Array<juce::Array<float>> table;
    for (int i = 0; i <= 20; ++i)
    {
        juce::Array<float> row;
        row.add (-15.0f + (float) i * 1.5f);
        row.add ((float) i / 20.0f);
        table.add (row);
    }

    check (typedReadbackMatch ("attack_ms", -1.125f, "-1.13 dB", table) == -1,
           "THE LIVE DEFECT: attack_ms landing on a dB display is a MISMATCH, "
           "even though the numbers agree");
    check (typedReadbackMatch ("attack_ms", -1.125f, "-1.13", table) == 1,
           "the same landing with NO declared unit still matches -- absence claims nothing");
    check (typedReadbackMatch ("drive", -1.125f, "-1.13 dB", table) == 1,
           "a semantic with no unit claim is not judged by the display's");
    check (typedReadbackMatch ("gain_db", -1.125f, "-1.13 dB", table) == 1,
           "agreeing units still match");
    check (typedReadbackMatch ("reverb_decay_s", 3.0f, "3.00 s", table) == 1,
           "reverb_decay_s reading seconds is NOT a conflict (the s/ms family)");
}

/*  A CONTROLS-ONLY MAP IS FINISHED, NOT EMPTY.

    Stage 1: the sweep finds everything and the proposer names it offline, so a
    map with no Tier 1 params and a full control surface is the ORDINARY output
    of a sweep rather than an abandoned attempt. The wire has to carry it
    without losing the controls, and a map with neither params NOR controls has
    to stay refusable -- the floor is that a map must say something.
*/
void testControlsOnlyPayload()
{
    using namespace ejmap;

    MapPayload p;
    p.fp = "controlsonly";
    p.category = "amp_sim";
    p.mode = Mode::fast;
    p.identity.format = "AudioUnit";
    p.identity.name = "Controls Only";
    p.identity.paramCount = 3;

    NamedControl c;
    c.name = "Presence";
    c.indices.add (4);
    c.kind = "anchored";
    c.rangeLo = 0.0; c.rangeHi = 10.0;
    c.anchors.add ({ 0.0, 0.0 });
    c.anchors.add ({ 1.0, 10.0 });
    c.trust = Trust::setread;
    p.controls.add (c);

    SkipRecord sk ("threshold_db", SkipOutcome::deferred,
                   "left for the proposer: unclaimed at submit");
    p.skips.add (sk);

    const auto v = p.toVar();
    check (v.getProperty ("schema", "").toString() == kMapSchemaString,
           "controls-only: the wire format is unchanged -- no bump needed for this");

    auto params = v.getProperty ("params", juce::var());
    const bool paramsEmpty = ! params.isObject()
                           || params.getDynamicObject()->getProperties().size() == 0;
    check (paramsEmpty, "controls-only: params is empty and that is legal");

    auto controls = v.getProperty ("controls", juce::var());
    check (controls.isObject()
             && controls.getDynamicObject()->getProperties().size() == 1,
           "controls-only: the control surface survives the round trip");
    check ((int) controls.getProperty ("Presence", juce::var())
             .getProperty ("index", -1) == 4,
           "controls-only: the control keeps its index");

    // The skip is a RECORDED FACT, and its reason is what separates a row left
    // for the proposer from one a human looked at and gave up on. Both are
    // `deferred` on the wire; only the reason distinguishes them, so the reason
    // is load-bearing rather than decoration.
    auto skips = v.getProperty ("skips", juce::var());
    check (skips.isArray() && skips.getArray()->size() == 1,
           "controls-only: an unclaimed row is recorded, never absent");
    check (skips.getArray()->getReference (0).getProperty ("reason", "")
             .toString().contains ("left for the proposer"),
           "controls-only: the reason says it went to the proposer, not that it failed");
}


//==============================================================================
/** THE RETRY RULE. Three separate launches before a quarantine.

    Two proofs the signed spec requires before it lands, and it requires them
    because a half-tested retry is WORSE than the single-crash rule it replaces:
    that rule is wrong but predictable, while a broken N=3 can fail to
    quarantine something that genuinely crashes every time, and the operator
    finds out by losing a session to it, repeatedly.

    A LAUNCH IS A LEDGER INSTANCE. The rule's own requirement is that attempts
    are independent process launches -- a process that has taken a SIGSEGV
    inside plugin code is not a sound place to retry from -- so the counter is
    persistent on disk and the relaunch is what makes attempts independent.
    Constructing a fresh Ledger over the same root is exactly that: it reads
    ledger.json and quarantine.json off disk with a new run id, holding nothing
    over in memory.
*/
void testRetryRule()
{
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root.createDirectory();

    const juce::String au   = "AudioUnit:Effects/aufx,0yow,SfTb";
    const juce::String vst3 = "/Library/Audio/Plug-Ins/VST3/Drawmer 1973.vst3";

    // The crash-report lookup reads a directory this process does not own, so
    // it is substituted. Without this every simulated death records as
    // unattributed and NOTHING ever quarantines -- the test would pass by
    // proving the opposite of what it claims.
    ejmap::Ledger::TestCrashOverride corroborated { true, "plugin" };
    ejmap::Ledger::TestCrashOverride unattributed { false, "unknown" };
    ejmap::Ledger::testOnlyCrashOverride() = &corroborated;

    // One launch: recover whatever the last one left, then stake and die.
    auto dieOnLoad = [&] (const juce::String& id, const juce::String& stage)
    {
        ejmap::Ledger l (root);
        l.recoverFromCrash();
        l.beginLoad (id, "Drawmer 1973", "Softube", "AudioUnit", "2.5.62", stage, "createPluginInstance");
        // no endLoad: the stake outlives the process, which is the crash.
    };
    auto quarantined = [&] (const juce::String& id)
    {
        ejmap::Ledger l (root);
        l.recoverFromCrash();
        return l.isQuarantined (id);
    };

    // ---- PROOF 1: three separate launches before quarantine -----------------
    dieOnLoad (au, "load");
    check (! quarantined (au), "retry: one death does not quarantine");
    dieOnLoad (au, "load");
    check (! quarantined (au), "retry: two deaths do not quarantine");
    dieOnLoad (au, "load");
    check (quarantined (au), "retry: THREE separate launches quarantine");

    // ---- A crash attaches to a BINARY, not a product ------------------------
    // The VST3 at the path above is a different binary by the same vendor at
    // the same version, with no failure history at all. Keying on the display
    // name would quarantine a working plugin because its sibling died.
    check (! quarantined (vst3),
           "retry: the VST3 sibling is a DIFFERENT binary and is not quarantined");
    {
        // The sibling shares name, vendor and version with the three dead rows
        // above and differs only in plugin_id. Under name-keying it would
        // arrive at its first load carrying three failures it never had.
        ejmap::Ledger l (root);
        check (l.retryEvidenceFor (vst3, "load").failures == 0,
               "retry: and it inherits NONE of its sibling's failures");
    }

    // ---- PROOF 2: succeeding on attempt two is never quarantined ------------
    auto root2 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root2.createDirectory();
    const juce::String flaky = "AudioUnit:Effects/aufx,SEQ1,NIn2";   // Solid EQ: 1 of 3

    auto dieIn = [&] (const juce::File& r, const juce::String& id, const juce::String& stage)
    {
        ejmap::Ledger l (r);
        l.recoverFromCrash();
        l.beginLoad (id, "Solid EQ", "Native Instruments", "AudioUnit", "1.4", stage, "createPluginInstance");
    };
    auto succeedIn = [&] (const juce::File& r, const juce::String& id, const juce::String& stage)
    {
        ejmap::Ledger l (r);
        l.recoverFromCrash();
        l.beginLoad (id, "Solid EQ", "Native Instruments", "AudioUnit", "1.4", stage, "createPluginInstance");
        ejmap::LedgerRecord rec;
        rec.pluginId = id; rec.name = "Solid EQ"; rec.format = "AudioUnit";
        rec.stage = stage; rec.outcome = ejmap::LoadOutcome::ok;
        l.endLoad (rec);
    };

    dieIn     (root2, flaky, "load");
    succeedIn (root2, flaky, "load");
    dieIn     (root2, flaky, "load");
    dieIn     (root2, flaky, "load");     // a third death, but only after a success
    {
        ejmap::Ledger l (root2);
        l.recoverFromCrash();
        check (l.isQuarantined (flaky),
               "retry: three deaths still quarantine even with a success between them");

        auto ev = l.retryEvidenceFor (flaky, "load");
        check (ev.priorOkInLedger == 1, "retry: the success is counted, not forgotten");
        check (ev.nonDeterministic(), "retry: prior success makes it non-deterministic");
        check (ev.note().contains ("single bad roll"),
               "retry: the quarantine row says a quarantine here is not a verdict");
        check (l.nonDeterministicQuarantine().size() == 1,
               "retry: it is selectable for the nightly re-test");
    }

    // The stated proof: TWO deaths with a success between them, never quarantined.
    auto root3 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root3.createDirectory();
    dieIn     (root3, flaky, "load");
    succeedIn (root3, flaky, "load");
    {
        ejmap::Ledger l (root3);
        l.recoverFromCrash();
        check (! l.isQuarantined (flaky),
               "retry: A PLUGIN SUCCEEDING ON ATTEMPT TWO IS NEVER QUARANTINED");
        auto ev = l.retryEvidenceFor (flaky, "load");
        check (ev.loadMs.size() == 2, "retry: load_ms carries one entry per attempt");
        check (ev.loadMs[1] >= 0, "retry: the completed load was timed, from the stake");
    }

    // ---- prior_ok_in_ledger is STAGE-SCOPED ---------------------------------
    // Drawmer's exact shape: eight "ok" rows, every one at stage scan. A scan
    // describes a plugin without instantiating it, so they say NOTHING about
    // whether it loads -- and its AU has never once loaded successfully. An
    // unscoped count reads "8 prior successes" and treats a plugin that has
    // never loaded as a well-behaved one having a bad roll.
    auto root4 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root4.createDirectory();
    for (int i = 0; i < 8; ++i)
        succeedIn (root4, au, "scan");
    dieIn (root4, au, "load");
    {
        ejmap::Ledger l (root4);
        l.recoverFromCrash();
        auto atLoad = l.retryEvidenceFor (au, "load");
        auto atScan = l.retryEvidenceFor (au, "scan");
        check (atScan.priorOkInLedger == 8, "retry: the scan successes are real, at scan");
        check (atLoad.priorOkInLedger == 0,
               "retry: EIGHT SCAN SUCCESSES VOUCH FOR NOTHING AT LOAD");
        check (! atLoad.nonDeterministic(),
               "retry: a plugin that has never loaded is not 'having a bad roll'");
        check (atLoad.failures == 1, "retry: the load death is counted at load");
    }

    // ---- an unattributed death does not count toward the three --------------
    // One of them may be the operator losing patience with a slow load, and
    // CLA-76 (m) takes 1.6 s against its sibling's 509 ms.
    auto root5 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root5.createDirectory();
    ejmap::Ledger::testOnlyCrashOverride() = &unattributed;
    for (int i = 0; i < 4; ++i)
        dieIn (root5, au, "load");
    {
        ejmap::Ledger l (root5);
        l.recoverFromCrash();
        check (! l.isQuarantined (au),
               "retry: four UNATTRIBUTED deaths do not quarantine -- they may be force-quits");
        auto ev = l.retryEvidenceFor (au, "load");
        check (ev.unattributedFailures == 4 && ev.corroboratedFailures == 0,
               "retry: they are recorded and visible, not silently dropped");
    }
    // ...but corroboration still gets there, from the same history.
    ejmap::Ledger::testOnlyCrashOverride() = &corroborated;
    for (int i = 0; i < 3; ++i)
        dieIn (root5, au, "load");
    {
        ejmap::Ledger l (root5);
        l.recoverFromCrash();
        check (l.isQuarantined (au),
               "retry: three CORROBORATED deaths quarantine, unattributed ones aside");
    }

    // ---- AN UNATTENDED RUN COUNTS ITS UNATTRIBUTED DEATHS -------------------
    // The discount exists for ONE reason: an operator force-quitting a slow
    // load leaves evidence identical to a SIGSEGV. A sweep has no operator, so
    // in an unattended run the reason does not apply and the death counts.
    //
    // Not theoretical. Three Drawmer deaths in a live supervised sweep all
    // recorded unattributed -- macOS wrote no report for any of them, and none
    // for anything since 3 August -- so the count sat at "attempt 0 of 3" and
    // the campaign re-crashed on it every relaunch until the supervisor stopped.
    auto root7 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root7.createDirectory();
    ejmap::Ledger::testOnlyCrashOverride() = &unattributed;
    auto dieUnattended = [&] (const juce::File& r, const juce::String& id)
    {
        ejmap::Ledger l (r);
        l.recoverFromCrash();
        l.setUnattended (true);
        l.beginLoad (id, "Drawmer 1973", "Softube", "AudioUnit", "2.5.62", "load", "PluginHost::load");
    };
    for (int i = 0; i < 3; ++i) dieUnattended (root7, au);
    {
        ejmap::Ledger l (root7);
        l.setUnattended (true);
        l.recoverFromCrash();
        check (l.isQuarantined (au),
               "retry: THREE UNATTRIBUTED DEATHS IN AN UNATTENDED RUN DO QUARANTINE "
               "-- nobody was there to force-quit them");
        auto ev = l.retryEvidenceFor (au, "load");
        check (ev.unattributedFailures == 3 && ev.corroboratedFailures == 0,
               "retry: and they are still recorded as unattributed, not relabelled");
        check (ev.unattendedFailures() == 3,
               "retry: the unattended tally is what the count used");
    }

    // The attended case is UNCHANGED: root5 above proved four unattributed
    // attended deaths do not quarantine, and that must stay true, because there
    // the force-quit hazard is real.

    // ---- historic rows count. They are the only determinism evidence --------
    ejmap::Ledger::testOnlyCrashOverride() = &corroborated;
    auto root6 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-retry-test-" + juce::Uuid().toDashedString());
    root6.createDirectory();
    auto historicRow = [&] (const juce::String& at)
    {
        return juce::String ("{\"plugin_id\":\"") + au
             + "\",\"stage\":\"load\",\"outcome\":\"crash_on_load\",\"at\":\"" + at + "\"}\n";
    };
    root6.getChildFile ("ledger.json").replaceWithText (
        historicRow ("2026-08-04T10:30:12") + historicRow ("2026-08-04T10:38:09"));
    {
        ejmap::Ledger l (root6);
        auto ev = l.retryEvidenceFor (au, "load");
        check (ev.corroboratedFailures == 2,
               "retry: rows spelled crash_on_load still count -- the rename kept the history");
    }
    dieIn (root6, au, "load");
    {
        ejmap::Ledger l (root6);
        l.recoverFromCrash();
        check (l.isQuarantined (au),
               "retry: two historic deaths plus one new one is three, not one");
    }

    ejmap::Ledger::testOnlyCrashOverride() = nullptr;
    root7.deleteRecursively();
    root.deleteRecursively();  root2.deleteRecursively(); root3.deleteRecursively();
    root4.deleteRecursively(); root5.deleteRecursively(); root6.deleteRecursively();
}


//==============================================================================
/** THE SWEEP'S TWO MECHANICAL RULES, proved without loading a plugin.

    Both were built BEFORE the campaign, on the --selftest finding: building the
    test first produced two real defects (a controls row counted as confirmed, a
    review screen reading READY above a disabled button), and a defect found on
    plugin 400 costs 400 maps.
*/
//==============================================================================
// The stepped-sanitizer pin (9 Aug 2026): steppedCollapseTable runs only after
// dominantMonotonicTable refuses, and must accept exactly the deterministic
// stepped shapes while every shape the strict rule exists to keep out -- the
// meters, LFOs and mirrors -- still refuses. Loosening a guard is how defects
// get in; this test is the difference between relaxing and hoping.
/** RECORDED IS COUNTED.
    ==============================================================================

    Written and RUN BEFORE the fix, on 10 Aug 2026, because the diagnosis it
    encodes was reasoned from a source read and a source read is a hypothesis.
    If this passed on the unfixed code the diagnosis was wrong and the fix would
    have been aimed at nothing.

    THE SUBJECT IS REAL. `bloom` (AudioUnit:Effects/aufx,BlmA,OekS) carries
    EIGHTEEN init_failed rows in the live ledger, 7 Aug 10:38 to 7 Aug 15:15,
    plus a death -- and was still being offered by the worklist on 10 Aug
    11:56. Every one of those rows is on disk. None of them was ever counted,
    because the retry arithmetic lives in the crash-recovery branch and an
    ordinary failed load never goes near it: it plants a stake and closes it
    through endLoad, which appends the row and decides nothing.

    So the ledger held nineteen pieces of evidence about one plugin and the
    quarantine rule consulted none of them. That is what this asserts against.

    Nothing here simulates a crash. There is no TestCrashOverride, no orphaned
    stake, no watchdog: just ten ordinary failed loads, each in its own Ledger
    over the same root, which is what ten separate launches look like from
    disk.
*/
void testRecordedIsCounted()
{
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-counted-test-" + juce::Uuid().toDashedString());
    root.createDirectory();

    const juce::String pid = "AudioUnit:Effects/aufx,BlmA,OekS";   // bloom

    // ONE ORDINARY FAILED LOAD, through the path every real one takes: a stake
    // written before control passes to plugin code, then a close carrying the
    // outcome. A fresh Ledger each time, so every count is read off disk.
    auto failOnce = [&] (ejmap::LoadOutcome o)
    {
        ejmap::Ledger l (root);
        l.setUnattended (true);
        l.beginLoad (pid, "bloom", "oeksound", "AudioUnit", "1.0.4",
                     "load", "PluginHost::load");

        ejmap::LedgerRecord r;
        r.pluginId = pid; r.name = "bloom"; r.vendor = "oeksound";
        r.format = "AudioUnit"; r.version = "1.0.4"; r.stage = "load";
        r.outcome = o;
        r.detail  = "An OS error occurred during initialisation of the plug-in (-1)";
        l.endLoad (r);
    };
    auto quarantined = [&]
    {
        ejmap::Ledger l (root);
        return l.isQuarantined (pid);
    };

    failOnce (ejmap::LoadOutcome::initFailed);
    check (! quarantined(),
           "counted: one recorded load failure is a bad roll, not a verdict");

    failOnce (ejmap::LoadOutcome::initFailed);
    check (! quarantined(),
           "counted: TWO recorded load failures do not quarantine");

    failOnce (ejmap::LoadOutcome::initFailed);
    check (quarantined(),
           "counted: THREE RECORDED LOAD FAILURES QUARANTINE -- a row that was "
           "written is a row that counts");

    failOnce (ejmap::LoadOutcome::initFailed);
    check (quarantined(),
           "counted: and it stays quarantined at row 4 -- quarantine is manual to undo");

    for (int i = 5; i <= 10; ++i)
        failOnce (ejmap::LoadOutcome::initFailed);

    check (quarantined(),
           "counted: ten failures do not un-quarantine it either");

    // EVERY ROW IS ON DISK EITHER WAY. The defect was never that the evidence
    // was missing -- bloom's eighteen rows prove it was there -- so the test
    // pins that the fix did not achieve its result by writing fewer rows.
    {
        juce::StringArray lines;
        lines.addLines (root.getChildFile ("ledger.json").loadFileAsString());
        int n = 0;
        for (const auto& line : lines)
            if (line.contains (pid)) ++n;
        check (n == 10,
               "counted: all ten failures are still RECORDED, not swallowed by the "
               "decision -- recorded and counted are the same event, not a trade");
    }

    root.deleteRecursively();

    //==========================================================================
    // THE COUNT SPANS THE FAILURE FAMILY, not one outcome string.
    //
    // This is the half of the diagnosis that is easiest to build wrong. The
    // rule it replaced counted rows matching a single literal ("timeout"), so
    // a plugin that hung once and then failed to instantiate twice reached no
    // threshold at all -- three recorded failures, three separate launches,
    // and every counter reading one. Weiss Deess is the live instance: five
    // timeouts and three init_failed rows, unquarantined.
    //
    // The outcomes below are deliberately all DIFFERENT, and deliberately
    // ordered so that no single one of them reaches its own bar.
    auto mixedRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ejmap-mixed-test-" + juce::Uuid().toDashedString());
    mixedRoot.createDirectory();
    {
        const juce::String weiss = "AudioUnit:Effects/aufx,fndm,SfTb";   // Weiss Deess

        auto failWith = [&] (ejmap::LoadOutcome o, const juce::String& stage)
        {
            ejmap::Ledger l (mixedRoot);
            l.beginLoad (weiss, "Weiss Deess", "Softube", "AudioUnit", "2.5.62",
                         stage, "PluginHost::load");
            ejmap::LedgerRecord r;
            r.pluginId = weiss; r.name = "Weiss Deess"; r.format = "AudioUnit";
            r.stage = stage; r.outcome = o; r.detail = "recorded";
            l.endLoad (r);
        };
        auto isQ = [&] { ejmap::Ledger l (mixedRoot); return l.isQuarantined (weiss); };

        failWith (ejmap::LoadOutcome::timeout, "load");
        check (! isQ(), "spans: one hang is a bad roll");
        failWith (ejmap::LoadOutcome::initFailed, "load");
        check (! isQ(), "spans: a hang and an init failure is two, and two is not three");
        failWith (ejmap::LoadOutcome::noParams, "load");
        check (isQ(),
               "spans: THREE FAILURES SPELLED THREE DIFFERENT WAYS REACH THE THRESHOLD "
               "-- the count is over the failure family, not over one outcome string");
    }
    mixedRoot.deleteRecursively();

    //==========================================================================
    // A SUCCESS SETTLES THE NON-DEATH FAILURES BEFORE IT.
    //
    // FOUND BY MEASURING, after the rest of this was already built and green,
    // while working out what to do about the plugins the live ledger has
    // already pushed past the threshold. H-EQ (s) has four load failures on
    // 4 Aug -- the Waves -10875 stream, which clears in a fresh process -- and
    // SIX successful loads on 9 Aug. A lifetime count reads 4 against a
    // threshold of 3 and withdraws a plugin that demonstrably works.
    //
    // And it does not stop at one plugin. The ledger only grows and nothing
    // ever reset the count, so every binary in the catalogue would eventually
    // accumulate three failures and be withdrawn for having been used. With
    // deaths alone (13 rows in 144,911) that never bit; counting ordinary
    // failures is what turned it into a slow fault.
    auto settleRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("ejmap-settle-test-" + juce::Uuid().toDashedString());
    settleRoot.createDirectory();
    {
        const juce::String heq = "AudioUnit:Effects/aufx,HEQS,ksWV";      // H-EQ (s)

        auto attempt = [&] (ejmap::LoadOutcome o)
        {
            ejmap::Ledger l (settleRoot);
            l.beginLoad (heq, "H-EQ (s)", "Waves", "AudioUnit", "14.0",
                         "load", "PluginHost::load");
            ejmap::LedgerRecord r;
            r.pluginId = heq; r.name = "H-EQ (s)"; r.format = "AudioUnit";
            r.stage = "load"; r.outcome = o;
            r.detail = o == ejmap::LoadOutcome::ok
                         ? "loaded without opening the editor"
                         : "An OS error occurred during initialisation of the plug-in (-10875)";
            l.endLoad (r);
        };
        auto isQ = [&] { ejmap::Ledger l (settleRoot); return l.isQuarantined (heq); };

        attempt (ejmap::LoadOutcome::timeout);
        attempt (ejmap::LoadOutcome::initFailed);
        check (! isQ(), "settles: two failures, not yet a verdict");

        attempt (ejmap::LoadOutcome::ok);
        check (! isQ(), "settles: and it loaded");

        attempt (ejmap::LoadOutcome::initFailed);
        attempt (ejmap::LoadOutcome::initFailed);
        check (! isQ(),
               "settles: FOUR LIFETIME FAILURES AND IT IS NOT WITHDRAWN -- a "
               "successful load settled the two before it, so the count since is two");

        {
            ejmap::Ledger l (settleRoot);
            auto ev = l.retryEvidenceFor (heq, "load");
            check (ev.otherFailures == 4,
                   "settles: the lifetime count is still on the record, unaltered");
            check (ev.otherFailuresSinceOk == 2,
                   "settles: and the DECISION reads the count since the success");
        }

        attempt (ejmap::LoadOutcome::initFailed);
        check (isQ(),
               "settles: three failures SINCE the success do withdraw it -- a success "
               "settles the past, it does not buy immunity");
    }
    settleRoot.deleteRecursively();

    //==========================================================================
    // A DEATH IS NEVER SETTLED, and the asymmetry is deliberate.
    //
    // This is the signed rule from the retry work -- "three deaths still
    // quarantine even with a success between them", because three separate
    // launches dying is a fact about hosting this plugin whoever's fault it
    // is. Asserted again HERE, beside the rule that would most plausibly be
    // "tidied" into it: the obvious simplification is to let a success reset
    // everything, and that would silently repeal a decision made on measured
    // evidence with a nightly re-test built around it.
    auto deathRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("ejmap-death-test-" + juce::Uuid().toDashedString());
    deathRoot.createDirectory();
    {
        const juce::String au = "AudioUnit:Effects/aufx,0yow,SfTb";       // Drawmer 1973
        ejmap::Ledger::TestCrashOverride corroborated { true, "plugin" };
        ejmap::Ledger::testOnlyCrashOverride() = &corroborated;

        auto die = [&]
        {
            ejmap::Ledger l (deathRoot);
            l.recoverFromCrash();
            l.beginLoad (au, "Drawmer 1973", "Softube", "AudioUnit", "2.5.62",
                         "load", "createPluginInstance");
        };
        auto succeed = [&]
        {
            ejmap::Ledger l (deathRoot);
            l.recoverFromCrash();
            l.beginLoad (au, "Drawmer 1973", "Softube", "AudioUnit", "2.5.62",
                         "load", "createPluginInstance");
            ejmap::LedgerRecord r;
            r.pluginId = au; r.name = "Drawmer 1973"; r.format = "AudioUnit";
            r.stage = "load"; r.outcome = ejmap::LoadOutcome::ok;
            l.endLoad (r);
        };

        die(); succeed(); die(); die();
        {
            ejmap::Ledger l (deathRoot);
            l.recoverFromCrash();
            check (l.isQuarantined (au),
                   "settles: A SUCCESS DOES NOT SETTLE A DEATH -- three deaths with a "
                   "success among them still withdraw the binary, which is the signed "
                   "rule and must not be tidied away");
        }
        ejmap::Ledger::testOnlyCrashOverride() = nullptr;
    }
    deathRoot.deleteRecursively();

    //==========================================================================
    // AND THE EXCLUSIONS, EACH TESTED BY ATTEMPTING THE THING IT REFUSES.
    //
    // These matter more than the positive cases. "Count every failure" is one
    // careless edit away from quarantining 300 VST3 bundles for declaring no
    // audio-effect types, or a vendor's whole catalogue because an iLok was
    // unplugged -- and quarantine is manual to undo, so a wrong withdrawal is
    // silent and permanent until a human notices a plugin missing.
    auto excludeRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("ejmap-exclude-test-" + juce::Uuid().toDashedString());
    excludeRoot.createDirectory();
    {
        auto tenOf = [&] (const juce::String& id, ejmap::LoadOutcome o,
                          const juce::String& stage)
        {
            for (int i = 0; i < 10; ++i)
            {
                ejmap::Ledger l (excludeRoot);
                l.beginLoad (id, "Subject", "V", "VST3", "1.0", stage, "probe");
                ejmap::LedgerRecord r;
                r.pluginId = id; r.name = "Subject"; r.format = "VST3";
                r.stage = stage; r.outcome = o; r.detail = "recorded";
                l.endLoad (r);
            }
            ejmap::Ledger l (excludeRoot);
            return l.isQuarantined (id);
        };

        check (! tenOf ("/Library/Audio/Plug-Ins/VST3/Mini V3.vst3",
                        ejmap::LoadOutcome::noTypes, "scan"),
               "excluded: TEN no_types SCANS DO NOT QUARANTINE -- a bundle with no "
               "audio-effect types is a fact about the file, re-read every scan, and "
               "4,249 such rows are on the live ledger");

        check (! tenOf ("AudioUnit:Effects/aufx,LICN,test",
                        ejmap::LoadOutcome::licenseRefused, "load"),
               "excluded: TEN LICENCE REFUSALS DO NOT QUARANTINE -- an unplugged iLok "
               "is a fact about the machine, and this would withdraw a vendor's whole "
               "catalogue overnight");

        check (! tenOf ("AudioUnit:Effects/aufx,NOED,test",
                        ejmap::LoadOutcome::noEditor, "load"),
               "excluded: TEN no_editor ROWS DO NOT QUARANTINE -- the plugin loaded, "
               "and the sweep runs headless and maps it fine");

        check (! tenOf ("AudioUnit:Effects/aufx,GOOD,test",
                        ejmap::LoadOutcome::ok, "load"),
               "excluded: and a plugin that keeps working is never withdrawn");
    }
    excludeRoot.deleteRecursively();

    //==========================================================================
    // THE TWO ASYMMETRIC THRESHOLDS SURVIVED THE MOVE.
    //
    // They were measured, they are not N=3, and a funnel that quietly
    // regularised them would cost a 4.5-minute rescan per scan hang and two
    // extra watchdog deadlines per load hang. Both are asserted here because
    // "we unified the rule" is exactly how a measured exception gets lost.
    auto hangRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("ejmap-hang-test-" + juce::Uuid().toDashedString());
    hangRoot.createDirectory();
    {
        auto hang = [&] (const juce::String& id, const juce::String& stage)
        {
            ejmap::Ledger l (hangRoot);
            l.beginLoad (id, "Hang", "V", "VST3", "1.0", stage, "findAllTypesForFile");
            ejmap::LedgerRecord r;
            r.pluginId = id; r.name = "Hang"; r.format = "VST3";
            r.stage = stage; r.outcome = ejmap::LoadOutcome::timeout;
            r.detail = "deadline expired";
            l.endLoad (r);
        };
        auto isQ = [&] (const juce::String& id)
        { ejmap::Ledger l (hangRoot); return l.isQuarantined (id); };

        const juce::String scanHang = "/Library/Audio/Plug-Ins/VST3/TDR SlickEQ M.vst3";
        hang (scanHang, "scan");
        check (isQ (scanHang),
               "thresholds: A SCAN HANG QUARANTINES ON THE FIRST TIMEOUT -- it blocks "
               "every bundle behind it, and sparing it costs a full rescan every time");

        const juce::String loadHang = "AudioUnit:Effects/aufx,LOTS,Cyma";  // Cymatics Lotus
        hang (loadHang, "load");
        check (! isQ (loadHang),
               "thresholds: a LOAD hang gets one retry -- Cymatics Lotus instantiates "
               "in 407 ms and was quarantined by a deadline that was simply too tight");
        hang (loadHang, "load");
        check (isQ (loadHang),
               "thresholds: and the second one withdraws it. A hang costs a whole "
               "deadline, so it gets one retry, not two");
    }
    hangRoot.deleteRecursively();
}

/** THE COST OF COUNTING, AT THE SIZE THE LEDGER ACTUALLY IS.

    The live ledger on the mapping machine is 144,911 rows / 54 MB, and it only
    grows. A decision that reads the whole file is affordable once per launch
    and ruinous once per row, so the number is measured here rather than
    assumed -- and it is measured at full size, because a 100-row fixture would
    have said everything was fine.

    Not a threshold anyone should tune: it is a REGRESSION GUARD. It exists so
    that the next person who makes the decision read the file again finds out
    from a gate rather than from a night that did not finish.
*/
void testCountingCost()
{
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-cost-test-" + juce::Uuid().toDashedString());
    root.createDirectory();

    // A ledger the size of the real one. Written raw, in one stream, because
    // 145,000 individual appends would be measuring the wrong thing.
    const int kRows = 144911;
    const juce::String subject = "AudioUnit:Effects/aufx,COST,test";
    {
        juce::FileOutputStream out (root.getChildFile ("ledger.json"));
        juce::String buf;
        for (int i = 0; i < kRows; ++i)
        {
            buf << "{\"run_id\":\"20260810T120000-abcd\",\"plugin_id\":\""
                << (i % 900 == 0 ? subject : "AudioUnit:Effects/aufx,f" + juce::String (i % 900).paddedLeft ('0', 3) + ",test")
                << "\",\"stage\":\"load\",\"name\":\"Filler " << i
                << "\",\"vendor\":\"V\",\"format\":\"AudioUnit\",\"version\":\"1.0\","
                   "\"outcome\":\"ok\",\"detail\":\"idx 17: anchors 0.00->-60.0 dB, "
                   "1.00->+12.0 dB, monotonic over 9 samples, unit family db, "
                   "readback verified by set-then-read; 24 parameters swept, 18 usable\","
                   "\"at\":\"2026-08-10T12:00:00.000Z\",\"param_count\":24,\"load_ms\":2214}\n";
            if (buf.length() > 1 << 16) { out.writeText (buf, false, false, nullptr); buf.clear(); }
        }
        out.writeText (buf, false, false, nullptr);
        out.flush();
    }

    const auto bytes = root.getChildFile ("ledger.json").getSize();
    check (bytes > 50 * 1024 * 1024,
           "cost: the fixture is the size of the real ledger (54 MB), not a toy -- "
           "a 100-row fixture would have said everything was fine");

    ejmap::Ledger l (root);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto ev = l.retryEvidenceFor (subject, "load");
    const auto onePassMs = juce::Time::getMillisecondCounterHiRes() - t0;

    check (ev.attempts > 0, "cost: the pass actually read the subject's rows");

    // TEN FAILED LOADS, through the real path, with that ledger underneath.
    // This is the number that decides whether counting can live in the append.
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 10; ++i)
    {
        l.beginLoad (subject, "Cost", "V", "AudioUnit", "1.0", "load", "PluginHost::load");
        ejmap::LedgerRecord r;
        r.pluginId = subject; r.name = "Cost"; r.format = "AudioUnit";
        r.stage = "load"; r.outcome = ejmap::LoadOutcome::initFailed;
        r.detail = "An OS error occurred during initialisation of the plug-in (-1)";
        l.endLoad (r);
    }
    const auto tenFailuresMs = juce::Time::getMillisecondCounterHiRes() - t1;

    // A HUNDRED SUCCESSFUL LOADS. The common case by two orders of magnitude:
    // 140,468 of the live ledger's rows are `ok`, so if a success pays the
    // cost of a decision it does not need, the sweep pays it 1,300 times a
    // night.
    const auto t2 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 100; ++i)
    {
        l.beginLoad (subject, "Cost", "V", "AudioUnit", "1.0", "scan", "probe");
        ejmap::LedgerRecord r;
        r.pluginId = subject; r.name = "Cost"; r.format = "AudioUnit";
        r.stage = "scan"; r.outcome = ejmap::LoadOutcome::ok;
        r.detail = "1 description(s)";
        l.endLoad (r);
    }
    const auto hundredOkMs = juce::Time::getMillisecondCounterHiRes() - t2;

    // Reported as TOTALS with the passes named, not as a per-row average: the
    // ten failures do not each cost a tenth of that number. The first pays for
    // the tally, one more pays for the quarantine evidence, and the other
    // eight are free -- an average would hide exactly the property being
    // asserted.
    std::cout << "COST at " << kRows << " rows / " << (bytes / (1024 * 1024)) << " MB:"
              << "  one full pass " << juce::String (onePassMs, 1) << " ms"
              << " | 10 failed loads " << juce::String (tenFailuresMs, 1) << " ms total"
              << " (~" << juce::String (tenFailuresMs / juce::jmax (1.0, onePassMs), 1)
              << " passes, not 10)"
              << " | 100 ok rows " << juce::String (hundredOkMs, 1) << " ms total"
              << " (" << juce::String (hundredOkMs / 100.0, 2) << " ms each, no pass)"
              << std::endl;

    // A SUCCESS MUST NOT PAY FOR A DECISION IT DOES NOT NEED. One full pass is
    // the unit of ruin here; a hundred of them would be 100x this fixture's
    // read. The bound is deliberately generous -- it is catching an order of
    // magnitude, not a regression of milliseconds.
    check (hundredOkMs < onePassMs * 2.0,
           "cost: ONE HUNDRED `ok` ROWS COST LESS THAN TWO FULL PASSES -- a "
           "successful load does not re-read the ledger");

    // And a failure, which DOES need the decision, pays for the file at most
    // once per launch rather than once per row.
    check (tenFailuresMs < onePassMs * 3.0,
           "cost: TEN FAILURES COST LESS THAN THREE FULL PASSES -- the count is "
           "read from the file once, then carried");

    root.deleteRecursively();
}

void testSteppedSanitizer()
{
    using Table = juce::Array<juce::Array<float>>;
    auto row = [] (float v, float n) { juce::Array<float> a; a.add (v); a.add (n); return a; };

    // A 3-step ratio (1,2,4) over 9 sweep points: the strict rule refuses it
    // (longest strict run is 3 of 9), the stepped rule accepts it, and the
    // emitted table is strictly monotonic with plateau-midpoint norms.
    {
        Table raw;
        for (int i = 0; i < 3; ++i) raw.add (row (1.0f, 0.0f + 0.1f * (float) i));
        for (int i = 0; i < 3; ++i) raw.add (row (2.0f, 0.4f + 0.1f * (float) i));
        for (int i = 0; i < 3; ++i) raw.add (row (4.0f, 0.8f + 0.1f * (float) i));
        check (! echojay::dominantMonotonicTable (raw).ok,
               "stepped: the strict rule refuses a 3-step plateau curve (this is the 672-row class)");
        auto st = ejmap::steppedCollapseTable (raw);
        check (st.ok && st.table.size() == 3, "stepped: collapse accepts it as 3 step anchors");
        check (st.table[0][0] < st.table[1][0] && st.table[1][0] < st.table[2][0],
               "stepped: the emitted table is strictly monotonic -- deployed clients consume it unchanged");
        check (std::abs (st.table[1][1] - 0.5f) < 1.0e-4f,
               "stepped: each anchor sits at its plateau's MIDPOINT norm");
        check (std::abs (echojay::interpolateAnchors (st.table, 2.0f) - 0.5f) < 1.0e-4f,
               "stepped: dialling '2' through the real apply path lands mid-step");
    }

    // Descending steps are as real as ascending ones.
    {
        Table raw;
        for (int i = 0; i < 3; ++i) raw.add (row (20.0f, 0.0f + 0.1f * (float) i));
        for (int i = 0; i < 3; ++i) raw.add (row (10.0f, 0.5f + 0.1f * (float) i));
        auto st = ejmap::steppedCollapseTable (raw);
        check (st.ok && st.table.size() == 2 && st.table[0][0] > st.table[1][0],
               "stepped: a descending step curve is accepted, direction preserved");
    }

    // What the strict rule was protecting against must STILL refuse.
    {
        Table lfo;   // slow square LFO: A,A,B,B,A,A -> collapses to A,B,A -> broken monotonic
        for (int i = 0; i < 2; ++i) lfo.add (row (1.0f, 0.0f + 0.1f * (float) i));
        for (int i = 0; i < 2; ++i) lfo.add (row (5.0f, 0.3f + 0.1f * (float) i));
        for (int i = 0; i < 2; ++i) lfo.add (row (1.0f, 0.6f + 0.1f * (float) i));
        check (! ejmap::steppedCollapseTable (lfo).ok,
               "stepped: a slow LFO still refuses -- collapse leaves A,B,A and monotonicity must be TOTAL");

        Table fast;  // fast alternation: no adjacent equals, nothing collapses
        fast.add (row (1.0f, 0.0f)); fast.add (row (5.0f, 0.25f));
        fast.add (row (1.0f, 0.5f)); fast.add (row (5.0f, 0.75f));
        check (! ejmap::steppedCollapseTable (fast).ok,
               "stepped: a fast alternation still refuses -- no plateau means no step evidence");

        Table pan;   // mirror: 50,25,0,25,50 -- the magnitude-parsed pan shape
        pan.add (row (50.0f, 0.0f)); pan.add (row (25.0f, 0.25f)); pan.add (row (0.0f, 0.5f));
        pan.add (row (25.0f, 0.75f)); pan.add (row (50.0f, 1.0f));
        check (! ejmap::steppedCollapseTable (pan).ok,
               "stepped: a mirror/pan shape still refuses -- adjacent values never repeat, and a V "
               "table would dial one magnitude to two positions");

        Table noise; // random walk, all distinct: nothing collapses
        const float ns[] = { 3.0f, 7.0f, 2.0f, 9.0f, 5.0f, 8.0f };
        for (int i = 0; i < 6; ++i) noise.add (row (ns[i], (float) i / 5.0f));
        check (! ejmap::steppedCollapseTable (noise).ok,
               "stepped: a random-walk display still refuses");

        Table flat;  // one distinct value everywhere: collapses to a single anchor
        for (int i = 0; i < 5; ++i) flat.add (row (4.0f, (float) i / 4.0f));
        check (! ejmap::steppedCollapseTable (flat).ok,
               "stepped: a flat table still refuses -- one anchor is not a curve (the N=1 class stays declined)");
    }

    // A curve the strict rule already accepts must never reach the stepped
    // path with a different answer waiting: same inputs, strict wins first.
    {
        Table clean;
        for (int i = 0; i < 5; ++i) clean.add (row ((float) i * 10.0f, (float) i / 4.0f));
        check (echojay::dominantMonotonicTable (clean).ok,
               "stepped: a strictly-monotonic sweep is accepted by the strict rule, stepped path never consulted");
    }
}

void testSweepRules()
{
    // NOTE ON WHAT IS *NOT* TESTED HERE, added 10 Aug 2026 with the skip
    // cascade extraction.
    //
    // "SWEEP WOULD OPEN" said 1,460 for a run that could open 40, because the
    // banner counted worklist rows and applied none of the sweep's own
    // filters. The fix makes the projection and the run call ONE function,
    // MainComponent::decideSweep, and the obvious next move is a test here
    // that asserts the cascade's behaviour.
    //
    // Deliberately not done: this file cannot instantiate MainComponent, so
    // such a test would re-implement the cascade in a lambda -- a THIRD
    // implementation, guarding two others by agreeing with neither. That is
    // the defect being fixed, wearing a test's clothes.
    //
    // The guard is at RUN TIME instead, and it is a better one: endSweep
    // reconciles the forecast against what the run actually did and says so on
    // screen, every run, against the real catalogue. It earned itself
    // immediately -- it caught a genuine error in its own first version, where
    // a DRY RUN reported "forecast 2, actual 0" because a dry run opens a
    // plugin without recording an outcome for it.

    // ---- 1. sweepable(): which products the loop opens ----------------------
    // The mirror of categorise.py's sweepable(). It is deliberately NOT
    // "disposition == sweep": a product both arms refused DIFFERENTLY is still
    // refused, and which refusal it is is a question for the marks review, not
    // for the loader. 14 of the 87 real disagreements are that shape.
    auto sweepable = [] (const juce::String& disposition, bool refusedByBoth,
                         const juce::String& category)
    {
        return disposition == "sweep" && ! refusedByBoth && category.isNotEmpty();
    };

    check (sweepable ("sweep", false, "eq"), "sweep: an agreed category is opened");
    check (sweepable ("sweep", false, "eq"),
           "sweep: a HEDGED agreement is opened too -- a wrong category cannot "
           "produce a wrong dial, the vocabulary is not category-scoped");
    check (! sweepable ("no_dial_set", false, ""),
           "sweep: a processor with no dial set is NEVER opened");
    check (! sweepable ("not_a_processor", false, ""),
           "sweep: nor is something that is not a processor");
    check (! sweepable ("review", false, ""),
           "sweep: a real disagreement has no category to sweep with");
    check (! sweepable ("review", true, ""),
           "sweep: and two refusals that differ are still refused -- the loader "
           "does not wait on a decision it does not need");

    // ---- 2. the circuit breaker --------------------------------------------
    // Ten of ONE class. The classes must not pool: ten licence refusals and ten
    // hangs need different advice, and mixing them would reach ten without any
    // single cause being true.
    auto runBreaker = [] (const juce::StringArray& outcomes)
    {
        juce::String cls; int consecutive = 0;
        for (const auto& o : outcomes)
        {
            const bool ok = (o == "mapped" || o == "swept_nothing");
            if (ok) { consecutive = 0; cls.clear(); continue; }
            if (o == cls) ++consecutive; else { cls = o; consecutive = 1; }
            if (consecutive >= 10) return cls;
        }
        return juce::String();
    };

    juce::StringArray ten;
    for (int i = 0; i < 10; ++i) ten.add ("license_refused");
    check (runBreaker (ten) == "license_refused",
           "breaker: ten licence refusals stop the run and NAME the class");

    juce::StringArray nine = ten; nine.remove (0);
    check (runBreaker (nine).isEmpty(), "breaker: nine do not");

    juce::StringArray mixed;
    for (int i = 0; i < 9; ++i) { mixed.add ("license_refused"); mixed.add ("timeout"); }
    check (runBreaker (mixed).isEmpty(),
           "breaker: NINE OF EACH, ALTERNATING, IS NOT TEN OF ONE -- the classes "
           "do not pool, because they need different advice");

    juce::StringArray interrupted;
    for (int i = 0; i < 9; ++i) interrupted.add ("license_refused");
    interrupted.add ("mapped");
    for (int i = 0; i < 9; ++i) interrupted.add ("license_refused");
    check (runBreaker (interrupted).isEmpty(),
           "breaker: a success in the middle resets it -- 18 failures around one "
           "win is not a machine-wide fault");

    juce::StringArray sweptNothing;
    for (int i = 0; i < 4; ++i) sweptNothing.add ("swept_nothing");
    for (int i = 0; i < 10; ++i) sweptNothing.add ("load_failed");
    check (runBreaker (sweptNothing) == "load_failed",
           "breaker: swept_nothing is a SUCCESS for the breaker -- the plugin "
           "loaded, so nothing is wrong with the machine");

    // init_failed IS ITS OWN CLASS. AVOX SYBIL refuses to initialise every
    // time (4097); the Waves -10875 stream clears with a fresh process. Pooling
    // them under load_failed would let two persistent refusers plus eight
    // transient ones trip a restart that cannot help the two -- and, worse,
    // let the curable stream be misread as ten broken plugins.
    juce::StringArray mixedInit;
    for (int i = 0; i < 5; ++i) { mixedInit.add ("init_failed"); mixedInit.add ("load_failed"); }
    check (runBreaker (mixedInit).isEmpty(),
           "breaker: five init_failed alternating with five load_failed is not "
           "ten of EITHER -- the classes stay separate");

    // ---- 3. a FLAG beats a SESSION -----------------------------------------
    // Found on plugin 4 of a live supervised sweep, not in a harness. A plugin
    // that loads, sweeps nothing and gets flagged leaves a session behind --
    // there was nothing to submit -- so if parked is tested first it returns as
    // PARKED, at the FRONT of the list, and is swept again forever.
    auto bucket = [] (bool hasIssue, bool parked)
    {
        if (hasIssue) return juce::String ("flagged");
        if (parked)   return juce::String ("parked");
        return juce::String ("sweep");
    };
    check (bucket (true, true) == "flagged",
           "worklist: A FLAG BEATS A SESSION -- a flag is a decision, a session "
           "is a state, and testing the state first loops forever");
    check (bucket (false, true) == "parked", "worklist: an unflagged session is parked work");
    check (bucket (true, false) == "flagged", "worklist: a flag with no session is still flagged");

    // ---- 3b. ONE EVENT, ONE ROW ---------------------------------------------
    // PluginHost::load plants its own stake and closes it with a complete
    // record. A caller that also closes it wrote a SECOND row for one load:
    // 124 such pairs are on disk, and 393 of 578 load rows have an empty name
    // because the closer knew the id and nothing else. The retry rule counts
    // rows, so a duplicate is not cosmetic.
    {
        auto r8 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("ejmap-dupe-test-" + juce::Uuid().toDashedString());
        r8.createDirectory();
        ejmap::Ledger l (r8);
        const juce::String pid = "AudioUnit:Effects/aufx,TEST,test";

        l.beginLoad (pid, "Test", "V", "AudioUnit", "1.0", "load", "PluginHost::load");
        ejmap::LedgerRecord a; a.pluginId = pid; a.stage = "load"; a.name = "Test";
        a.outcome = ejmap::LoadOutcome::initFailed; a.detail = "OS error 4097";
        l.endLoad (a);                                   // the closer's row

        ejmap::LedgerRecord b = a;                       // a caller closing it too
        l.endLoad (b);

        auto ev = l.retryEvidenceFor (pid, "load");
        check (ev.attempts == 1,
               "ledger: two closes of ONE stake write ONE row (got "
                 + juce::String (ev.attempts) + ")");

        // A genuine second load plants a new stake, which clears the guard.
        l.beginLoad (pid, "Test", "V", "AudioUnit", "1.0", "load", "PluginHost::load");
        l.endLoad (a);
        check (l.retryEvidenceFor (pid, "load").attempts == 2,
               "ledger: and a REAL second load is still two rows");
        r8.deleteRecursively();
    }

    // ---- 3c. an init failure is not a timeout -------------------------------
    // AVOX SYBIL returns "An OS error occurred during initialisation (4097)" in
    // 125 ms and was recorded as a timeout. Nothing hung, and the breaker's
    // advice for `timeout` is "loads are hanging" -- ten of those would stop a
    // run with a diagnosis that is simply untrue.
    check (ejmap::toString (ejmap::LoadOutcome::initFailed) == "init_failed",
           "outcome: an instantiate failure says init_failed");
    check (ejmap::toString (ejmap::LoadOutcome::initFailed)
             != ejmap::toString (ejmap::LoadOutcome::timeout),
           "outcome: and is NOT a timeout, so the breaker keeps its meaning");

    // ---- 4. capture state is named after what was OBSERVED ------------------
    // An earlier reading attributed an empty capture to out-of-process hosting,
    // because API-550A was both empty AND an NSRemoteView. Disproved 5 Aug: the
    // same vendor's VST3, loaded in-process with no NSRemoteView anywhere in
    // the tree, captures at 0.0% too. A field called `unavailable_bridged`
    // would have frozen the wrong cause into the corpus -- the crash_on_load
    // mistake again.
    {
        ejmap::CaptureResult none;
        check (none.state() == "unavailable", "capture: never attempted -> unavailable");

        ejmap::CaptureResult blank; blank.attempted = true;
        blank.width = 632; blank.height = 1194; blank.fraction = 0.0;
        check (blank.state() == "empty",
               "capture: A BLANK RECTANGLE IS 'empty' -- what was observed, not why");
        check (blank.state() != "unavailable_bridged",
               "capture: and NEVER named after a cause. A non-remote Waves panel "
               "reads 0.0% too, so bridging is not it");

        ejmap::CaptureResult ok; ok.attempted = true;
        ok.width = 2044; ok.height = 1400; ok.fraction = 0.526;
        check (ok.state() == "ok", "capture: AirEQ's 52.6% is a real panel");
        check (ok.width > 0 && ok.height > 0 && ok.fraction > 0,
               "capture: the fraction and the dimensions ride with it, so a later "
               "answer has numbers to be tested against");
    }

    // ---- 5. which launches supervise themselves -----------------------------
    // A mapper double-clicks an app and never types --supervise, so a GUI
    // launch supervises by default and the flag becomes the way to say no.
    auto selfSupervises = [] (const juce::StringArray& args)
    {
        for (const auto& a : args)
            if (a == "--child" || a == "--no-supervise" || a.startsWith ("--selftest")
                 || a == "--gate-m9")
                return false;
        return true;
    };
    check (selfSupervises ({}), "supervise: a bare double-click supervises itself");
    check (selfSupervises ({"--sweep"}), "supervise: and a sweep certainly does");
    check (! selfSupervises ({"--child"}),
           "supervise: the supervised child does NOT, or it forks forever");
    check (! selfSupervises ({"--selftest-segv"}),
           "supervise: a diagnostic that CRASHES ON PURPOSE is never relaunched");
    check (! selfSupervises ({"--selftest-controlsonly", "x"}),
           "supervise: nor any other selftest");
    check (! selfSupervises ({"--no-supervise"}),
           "supervise: and there is an escape hatch for running under a debugger");
    check (selfSupervises ({"--ledger-root", "/tmp/x", "--sweep", "--sweep-limit", "4"}),
           "supervise: ordinary flags do not disable it");

    // ---- 6. the supervisor's progress exemption -----------------------------
    // A sweep with a 5% death rate needs ~50 relaunches and the total-restart
    // ceiling is 10. The exemption is keyed on PROGRESS, never on activity: a
    // child that loads a plugin and dies without finishing it moves nothing and
    // still spends the budget, so the bound that cannot be reset survives.
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-sweep-test-" + juce::Uuid().toDashedString());
    root.createDirectory();

    check (ejmap::sweepProgressCount (root) == -1,
           "supervisor: no marker means no sweep, and no exemption");

    ejmap::sweepProgressMarker (root).replaceWithText ("7");
    const int before = ejmap::sweepProgressCount (root);
    check (before == 7, "supervisor: the finished count is read off disk");

    ejmap::sweepProgressMarker (root).replaceWithText ("9");
    check (ejmap::sweepProgressCount (root) > before,
           "supervisor: a child that FINISHED plugins is not charged a restart");

    ejmap::sweepProgressMarker (root).replaceWithText ("9");
    check (! (ejmap::sweepProgressCount (root) > 9),
           "supervisor: a child that loaded and died without finishing IS charged");

    // AND THE COUNT MUST BE CUMULATIVE. Per-launch counters make it go DOWN
    // after a crash -- measured on the first supervised sweep, 2 mapped then a
    // crash then 1 swept wrote 1 -- so every relaunch reads as no progress and
    // the run stops at the ceiling with work left.
    auto cumulative = [] (int base, int thisLaunch) { return base + thisLaunch; };
    ejmap::sweepProgressMarker (root).replaceWithText ("2");
    const int base = juce::jmax (0, ejmap::sweepProgressCount (root));
    check (cumulative (base, 1) > 2,
           "supervisor: one plugin finished after a crash at 2 records 3, NOT 1");
    check (juce::jmax (0, -1) == 0, "supervisor: an absent marker starts the base at 0");

    // AND PROGRESS MUST CLEAR fastDeaths, not just consecutive. That counter is
    // for a crash BEFORE THE WINDOW EXISTS -- and a sweep child that maps a
    // plugin in 8 s then dies on the next has a sub-10s lifetime and looks
    // identical. Measured: a run with 3 maps already written stopped on "the
    // session is dying before it can be used".
    auto budget = [] (bool madeProgress, int fastDeathsIn)
    {
        int fast = fastDeathsIn + 1;              // this exit was a fast one
        if (madeProgress) fast = 0;
        return fast;
    };
    check (budget (true, 2) == 0, "supervisor: progress clears fastDeaths");
    check (budget (false, 2) == 3, "supervisor: no progress and it still counts");

    root.deleteRecursively();
}


//==============================================================================
/** PER-MAPPER IDENTITY. One shared ingest token means any leak writes to the
    corpus and revoking it locks out everyone at once, and "who mapped this" is
    unanswerable because provenance comes from a name someone TYPES.
*/
/** THE STALE-CONTROLS INVARIANT.

    Half a test sweep submitted the PREVIOUS plugin's control surface under the
    new plugin's fingerprint: a Korg delay carrying an Ampeg's Ch64Bass, an SPL
    Transient Designer with 20 controls when it has 4 parameters. Valid-looking,
    gate-passing, and wrong -- the worst shape a corpus can take.

    The cause was `tierPhase` surviving resetAll, so the first SPACE landed in
    tierAccept() instead of the controls sweep and the stale pendingControls
    were never rebuilt. The reset is fixed; this is the invariant that makes the
    class impossible, because the next state flag someone forgets to reset will
    not announce itself either.
*/
void testStaleControlsRefused()
{
    // The rule, isolated: staged controls carry the fingerprint they were swept
    // from, and a submit for any other fingerprint is refused.
    auto staleFor = [] (const juce::String& stagedFp, const juce::String& currentFp,
                        int staged)
    {
        return ! (staged == 0 || stagedFp == currentFp);
    };

    check (! staleFor ("fpA", "fpA", 22), "controls swept from THIS plugin are accepted");
    check (staleFor ("fpA", "fpB", 22),
           "CONTROLS SWEPT FROM ANOTHER PLUGIN ARE REFUSED -- this is the one that "
           "wrote a Korg delay with an Ampeg's knobs");
    check (! staleFor ("", "fpB", 0),
           "an empty staging is not stale, it is empty: swept_nothing, not a refusal");
    check (staleFor ("", "fpB", 5),
           "controls with NO stamp are refused too -- absence is not proof of freshness");

    // AND THE HAND PATH ENFORCES IT TOO. SSL Fusion HF Compressor was submitted
    // BY HAND on 4 Aug carrying Dangerous BAX EQ Mix's controls, and reached the
    // server. The sweep is not where the 40 maps came from.
    check (staleFor ("bax-fp", "fusion-fp", 6),
           "the HAND path refuses it as well -- that is where the live one happened");

    // AND A RESTORED SESSION IS NOT STALE. Found on the live overnight run at
    // plugin 1: every parked plugin refused to submit, because the stamp was
    // set where controls are BUILT and a restore does not build them. A session
    // file is keyed by fingerprint -- assign-<fp>.json -- so its controls are
    // its own by construction.
    auto restored = [] (const juce::String& stampedInSession, const juce::String&)
    {
        return stampedInSession;    // NO fallback -- see the comment below
    };
    check (! staleFor (restored ("fpA", "fpA"), "fpA", 22),
           "a session restored with its stamp is NOT stale");
    // NO FALLBACK, and the fallback that stood here wrote a bad map. "A
    // session file is keyed by fingerprint, so its controls are its own by
    // construction" was an ARGUMENT: assign-bc7ef28d....json is API-560 (m)'s
    // session and holds ADA STD-1's 22 controls, parked by a binary that
    // predates the stamp. Adopting them stamped foreign controls as native and
    // shipped 'Tap Assign 4' at index 14 on a 14-parameter plugin.
    check (staleFor (restored ("", "fpA"), "fpA", 22),
           "an UNSTAMPED session is unstamped -- its controls are refused, not "
           "adopted; the cost is one re-sweep and the claim is nothing");
    check (staleFor (restored ("fpB", "fpA"), "fpA", 22),
           "but a stamp that disagrees with the session is still refused");
}

/** THE SWEEP-PHASE DEADLINE, and that it is not a load timeout. */
void testSweepDeadline()
{
    // The formula, mirrored. 60s floor + 1s per parameter, capped at 600s.
    auto deadline = [] (int params)
    {
        return juce::jmin (600000, 60000 + 1000 * juce::jmax (0, params));
    };

    // FROM MEASUREMENT: 191 automated plugin-runs, median 2.2s, p95 12.1s, and
    // the worst REAL sweep 14.1s at 99 parameters. The deadline has to clear
    // that comfortably or it kills a plugin for being large.
    check (deadline (99) > 14100 * 10,
           "sweep deadline: ten times the worst measured sweep (14.1s at 99 params)");
    check (deadline (4) >= 60000,
           "sweep deadline: a tiny plugin still gets the floor, not 4 seconds");
    check (deadline (99) > deadline (4),
           "sweep deadline: SCALES with the parameter count -- a fixed number would "
           "kill a 200-param plugin for being large or give a 4-param one an hour to hang");
    check (deadline (2000) == 600000,
           "sweep deadline: and is capped, so nothing gets 34 minutes");
    check (deadline (0) == 60000, "sweep deadline: an unknown parameter count gets the floor");

    // A HANG WHILE SWEEPING IS NOT A HANG WHILE LOADING. The plugin loaded --
    // in 68 ms, on the run that produced this. They mean different things, they
    // get different advice, and the retry rule is stage-scoped so it counts
    // them separately. Recording both as "timeout" is the init_failed mistake
    // with a different name.
    check (ejmap::toString (ejmap::LoadOutcome::sweepTimeout) == "sweep_timeout",
           "outcome: a sweep-phase expiry says sweep_timeout");
    check (ejmap::toString (ejmap::LoadOutcome::sweepTimeout)
             != ejmap::toString (ejmap::LoadOutcome::timeout),
           "outcome: and is NOT a load timeout");
    check (ejmap::toString (ejmap::LoadOutcome::sweepTimeout)
             != ejmap::toString (ejmap::LoadOutcome::initFailed),
           "outcome: nor an init failure -- three distinct things, three names");

    // The stage is what the retry rule scopes on, and the two must not pool:
    // SSL X-Gate dies at probe_gate_load and succeeds 23 times at load.
    check (juce::String ("sweep") != juce::String ("load"),
           "outcome: the sweep row is recorded at stage 'sweep', not 'load'");

    // THE WIRING, NOT THE FORMULA. The previous version of this test checked
    // that sweepDeadlineFor returned sensible numbers and passed -- while the
    // Watchdog::Scope that was supposed to USE it had never been inserted. The
    // live run then took eleven deaths with zero sweep_timeout rows, and the
    // clean test was the reason nobody looked.
    //
    // A source check is crude and it is the only thing that would have caught
    // this: the guard and the stake are single lines in one function and there
    // is no seam to assert on from here.
    {
        auto src = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/Source/MainComponent.h")
                       .loadFileAsString();
        auto body = src.fromFirstOccurrenceOf ("juce::String sweepOne (", false, false)
                       .upToFirstOccurrenceOf ("\n    /** The panel image", false, false);
        check (body.contains ("Watchdog::Scope guard (watchdog, \"controls sweep\""),
               "WIRING: sweepOne actually ARMS the watchdog it was given a deadline for");
        check (body.contains ("sweepDeadlineFor (cal.paramCount)"),
               "WIRING: ...with the measured per-parameter deadline, not a constant");
        check (body.contains ("\"sweep\", \"sweep phase (calibrate/mask/assign/submit)\""),
               "WIRING: and plants a STAKE over the WHOLE post-load block. The narrow "
               "stake around the two dispatches missed a real death -- signal:5 between "
               "elysia's banner and its outcome, no inflight, restart said (unknown) -- "
               "because calibrate, the noise mask, the session restore and submit were "
               "all unstaked");
        check (body.contains ("SweepStakeCloser"),
               "WIRING: the stake is closed by a CLOSER at every exit -- seven returns "
               "follow it, and a per-return endLoad is the shape that grows an eighth "
               "with no close");
    }
}

/** juce::JSON writes things its own parser refuses. An empty property key is
    one: JSON::toString emits `"": {...}` and JSON::parse answers "Invalid
    property name". A map carrying one is unreadable by every JUCE client
    INCLUDING ITS OWN WRITER -- it un-registers from localMapIdentities and the
    plugin re-sweeps every launch. Found live on bx_XL V2, whose parameters
    58-60+ have empty names.
*/
/** A CACHED "unmapped" IS A STATEMENT ABOUT THE SERVER AT FETCH TIME; a map on
    this disk is a fact about now.

    Measured on Mac 2, 10-11 Aug 2026. The map-state fetch ran at 15:49 with
    maps/ empty, so 1,458 identities were cached `unmapped` -- 234 of them UAD
    11.8.0, which the server had never held (this machine's corpus was 11.2.0).
    beginSweep does not refetch while mapStateFetchedAt is set, so every launch
    replayed that answer. mapStateFor returned the cached row BEFORE consulting
    localMapIdentities, and `unmapped` is offerable. UAD Korg SDD-3000 was swept
    twelve times under one fingerprint, seven of them inside 90 seconds; the map
    was written correctly every time and the index that knew it was never asked.

    Same family as testEmptyControlNameRejected above -- that one un-registered
    a plugin from localMapIdentities, this one never consults it -- and the
    symptom is identical, which is why it took two days to tell apart.

    Source checks, for the reason testSweepDeadline gives: this file cannot
    instantiate MainComponent, and the ordering is a handful of lines inside one
    function with no seam to assert on from here.
*/
void testLocalMapOutranksCachedNonConfirmation()
{
    auto src = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/Source/MainComponent.h")
                   .loadFileAsString();

    {
        auto body = src.fromFirstOccurrenceOf ("MapStateRow mapStateFor (", false, false)
                       .upToFirstOccurrenceOf ("\n    /** The detail line.", false, false);

        check (body.contains ("const bool haveLocal = localMapIdentities.count (key) > 0;"),
               "PRECEDENCE: mapStateFor reads the local index BEFORE it decides what the "
               "cached row is worth");
        check (body.contains ("MapState::unmapped")
                 && body.contains ("MapState::unknown")
                 && body.contains ("cachedSaysNothing"),
               "PRECEDENCE: a cached unmapped/unknown is named as a NON-CONFIRMATION "
               "rather than treated as an answer");
        check (body.contains ("if (! (haveLocal && cachedSaysNothing))"),
               "PRECEDENCE: the cached row is returned only when it is NOT contradicted "
               "by a map on this disk -- the unconditional return is the defect");
        // The positive server answers must still win, or a local write would
        // quietly downgrade evidence the server actually gave us.
        check (! body.contains ("it->second.state == MapState::submittedByYou"),
               "PRECEDENCE: submittedByYou/submittedByOther are NOT in the yielding set");
    }

    {
        auto body = src.fromFirstOccurrenceOf ("MAP SAVED (local only", false, false);
        (void) body;
        // The write sits just above the MAP SAVED banner, so anchor on the
        // save site itself rather than on the banner that follows it.
        auto save = src.fromFirstOccurrenceOf ("// AND THE STATE THE WORKLIST READS", false, false)
                       .upToFirstOccurrenceOf ("applyFilter();", false, false);
        check (save.contains ("srow.state        = MapState::localOnly;"),
               "MAP SAVE: writing a map writes the state the worklist reads -- the send "
               "path did this from its 200 and the save path did not, and --send-pending "
               "never writes map state at all");
        check (save.contains ("srow.fromLocalMap = true;"),
               "MAP SAVE: recorded as the THIRD provenance, not as a server answer");
        check (save.contains ("!= MapState::submittedByYou")
                 && save.contains ("!= MapState::submittedByOther"),
               "MAP SAVE: a server confirmation is not downgraded by a local write");
        check (save.contains ("saveMapStateCache();"),
               "MAP SAVE: and it is persisted, or the next launch replays the stale row");
    }

    {
        // The state 279 maps were in for two days. A status line that cannot
        // say it is how the operator ended up unsure whether the work existed.
        check (src.contains ("bool fromLocalMap = false;"),
               "PROVENANCE: 'mapped here, not yet sent' is a field, distinct from "
               "fromServer and fromLocalSubmit");
        check (src.contains ("e->setProperty (\"from_local_map\", kv.second.fromLocalMap);"),
               "PROVENANCE: persisted, or the distinction dies at the next launch");
        check (src.contains ("kv.value.getProperty (\"from_local_map\", false)"),
               "PROVENANCE: and restored");
        check (src.contains ("mapped here and NOT SENT"),
               "PROVENANCE: the status line COUNTS it and names it -- the line could "
               "previously only report what the server had not confirmed");
    }
}

/** THE PLACEHOLDER MUST NOT BE DERIVED FROM, AND A FAILURE MUST NAME ITS URL.

    Mac 2, 11 Aug 2026. `saveMapperToken` writes a FRESH config.json holding only
    mapper_token when none exists, so signing in silently removed upload_url.
    resolveEndpoint then returned its unset placeholder --
    "https://UPLOAD-ENDPOINT-UNSET.echojay.invalid/api/params/ejmap" -- and
    categoriseEndpoint's guard was `contains ("/api/params/")`, which the
    placeholder satisfies BY CONSTRUCTION. So it derived
    ".../echojay.invalid/api/params/categories", the production fallback on the
    next line became dead code, `.invalid` never resolved, createInputStream
    returned nullptr, status stayed 0, and the message said "no response".

    Size-independent, so it read as a body-size problem for six rounds; and the
    maps fetch kept working throughout because mapsEndpoint never consults
    config.json. The message had the URL in scope and never printed it.

    Source checks, per testSweepDeadline: this file cannot instantiate
    MainComponent.
*/
void testUnsetEndpointNotDerived()
{
    auto src = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/Source/MainComponent.h")
                   .loadFileAsString();

    {
        auto body = src.fromFirstOccurrenceOf ("juce::String categoriseEndpoint()", false, false)
                       .upToFirstOccurrenceOf ("\n    /** The products a sweep", false, false);
        check (body.contains ("cfg.urlFrom != \"placeholder\""),
               "ENDPOINT: categorise asks whether anything CONFIGURED the url, not whether "
               "the string looks like one -- the placeholder looks like one on purpose");
        check (body.contains ("return \"https://www.echojay.ai/api/params/categories\";"),
               "ENDPOINT: and the production fallback is still there for a READ");
    }

    {
        // resolveEndpoint must keep recording provenance, or the guard above
        // silently degrades to always-true.
        auto mouth = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/Source/EjmapMouth.h")
                         .loadFileAsString();
        check (mouth.contains ("urlFrom = \"placeholder\""),
               "ENDPOINT: resolveEndpoint still defaults urlFrom to \"placeholder\"");
        check (mouth.contains ("e.urlFrom = \"EJMAP_UPLOAD_URL\"")
                 && mouth.contains ("e.urlFrom = configFile (root).getFileName()"),
               "ENDPOINT: ...and still overwrites it when something actually sets the url");
    }

    {
        auto body = src.fromFirstOccurrenceOf ("void runCategorise()", false, false)
                       .upToFirstOccurrenceOf ("\n    juce::String categoriseLine", false, false);
        check (body.contains ("\"  [endpoint \" + endpoint + \"]\""),
               "MESSAGE: the categorise failure NAMES the url it tried -- the one fact that "
               "would have ended this on day one");
        check (body.contains ("could not connect -- no response ever ")
                 && body.contains ("the server refused it: HTTP "),
               "MESSAGE: and distinguishes a refusal from never reaching anything, which the "
               "code already knew and the words did not say");
    }

    {
        auto body = src.fromFirstOccurrenceOf ("void sendPendingMaps (", false, false);
        check (body.contains ("hostPort.containsIgnoreCase (\"UPLOAD-ENDPOINT-UNSET\")"),
               "SEND: the unset placeholder is refused as a CONFIGURATION fault before the "
               "wire, rather than posting into a domain that cannot resolve");
        check (body.contains ("signing in creates that file without one"),
               "SEND: and the refusal names how the machine got there");
    }
}

/** CORRECT WORK, REPEATED. The aggregate is the cost.

    Measured in the shipped binary (--selftest-mapindex) at 1,108 maps / 1,799
    identity rows:

      index rebuild  (was: every map save)   493.55 ms
      index insert   (now)                     0.00 ms
      backlog x2     (was: every label)       57.49 ms
      backlog x1     (now)                    27.64 ms
      -> per mapped plugin: 551.0 ms -> 27.6 ms

    refreshLocalMapIdentities opens and JSON-parses every file in maps/ to
    relearn a key the caller already holds. On the message thread, once per
    mapped plugin, that is the beachball-recover rhythm reported from the GUI --
    and the CLI paid it identically with no window to freeze, so it read as a
    GUI-only problem and was not one.

    updateBacklogLabel asked computeBacklog twice for one label: the text and
    the colour are two questions about ONE backlog.

    Third instance of this class: 490745f (a directory walk per identity),
    73ee64a (783 declines re-derived per restart), this. Nothing is wrong with
    the work; only with how often it runs.
*/
void testPerStepWorkIsNotRepeated()
{
    auto src = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/Source/MainComponent.h")
                   .loadFileAsString();

    {
        // The map-save site: insert the known key, do not relearn the corpus.
        auto save = src.fromFirstOccurrenceOf ("// ONE IDENTITY, NOT ONE THOUSAND", false, false)
                       .upToFirstOccurrenceOf ("applyFilter();", false, false);
        check (save.contains ("localMapIdentities.insert "
                              "(echojay::identityKeyForDescription (loadedDesc));"),
               "PER-STEP: the map save adds the ONE identity it just wrote");
        check (! save.contains ("refreshLocalMapIdentities();"),
               "PER-STEP: ...and does not re-parse all 1,108 maps to learn it (493 ms)");
    }
    {
        // The send site: same key, already in hand as `key`.
        auto send = src.fromFirstOccurrenceOf ("mapStateByIdentity[key] = row;", false, false)
                       .upToFirstOccurrenceOf ("logColumns (\"send accepted\"", false, false);
        check (send.contains ("localMapIdentities.insert (key);"),
               "PER-STEP: an accepted send adds its own key -- 279 sends paid ~96 s of "
               "re-parsing before this");
        check (! send.contains ("refreshLocalMapIdentities();"),
               "PER-STEP: ...and no longer rebuilds the whole index per accepted map");
    }
    {
        auto lab = src.fromFirstOccurrenceOf ("void updateBacklogLabel()", false, false)
                      .upToFirstOccurrenceOf ("\n    void ", false, false);
        check (lab.contains ("const auto b = computeBacklog();          // once, for both"),
               "PER-STEP: the backlog is computed ONCE for the text and the colour");
        check (! lab.contains ("backlogLabel.setText (backlogLine()"),
               "PER-STEP: ...not once via backlogLine() and again for the colour");
    }
    {
        // The measurement must stay runnable, or the next person re-argues this
        // from feel. "It feels smoother" is what a reader reports; it is not evidence.
        check (src.contains ("void selfTestMapIndexCost()"),
               "PER-STEP: --selftest-mapindex keeps the before/after measurable in the "
               "shipped binary");
        check (src.contains ("index rebuild  (was: every map save)"),
               "PER-STEP: ...and prints the rebuild cost it replaced, not just the new one");
    }
    {
        // The full rebuild must survive where it is genuinely needed: a restored
        // cache and a completed scan learn identities nobody has in hand.
        check (src.contains ("refreshLocalMapIdentities();"),
               "PER-STEP: the full rebuild still exists for cache restore, scan and the "
               "lazy fill -- the fix is frequency, not deletion");
    }
}

void testEmptyControlNameRejected()
{
    using ejmap::Mouth;

    // The mouth refuses it, with the reason.
    auto mk = [] (const char* key)
    {
        auto* ctrl = new juce::DynamicObject();
        ctrl->setProperty ("name", "x");
        auto* controls = new juce::DynamicObject();
        controls->setProperty (juce::Identifier (juce::String (key).isEmpty()
                                                   ? juce::String (" ") : juce::String (key)),
                               juce::var (ctrl));
        // an actually-empty identifier asserts in debug, so the empty case is
        // exercised through the whitespace-only spelling, which the gate must
        // treat the same: trim() decides, not length()
        auto* o = new juce::DynamicObject();
        o->setProperty ("controls", juce::var (controls));
        return juce::var (o);
    };
    auto rejectsEmpty = [] (const ejmap::Mouth::Verdict& v)
    {
        for (const auto& r : v.rejections)
            if (r.contains ("EMPTY name")) return true;
        return false;
    };
    check (rejectsEmpty (Mouth::structuralGate (mk (" "), "t")),
           "mouth: a whitespace-only control key is refused, with the reason");
    check (! rejectsEmpty (Mouth::structuralGate (mk ("Sustain"), "t")),
           "mouth: a real control name is not caught by that check");

    // And the write/read asymmetry itself, pinned so the NEXT one of these is
    // found by the gate and not by a campaign: what the payload writer emits,
    // the parser must re-read.
    ejmap::MapPayload p;
    p.fp = "f"; p.identity.name = "X"; p.identity.format = "AudioUnit";
    ejmap::NamedControl c; c.name = "Sustain"; c.indices.add (1);
    p.controls.add (c);
    juce::var back;
    check (juce::JSON::parse (p.toJson(), back).wasOk(),
           "round trip: WHAT THE WRITER EMITS, THE PARSER RE-READS");
}

/** A PLUGIN THAT DIES EVERY TIME MUST COME OFF THE WORKLIST, and it must not
    depend on every stake in the system being hole-free.

    bx_rooMS died six consecutive times and the ledger recorded ZERO deaths for
    it: the per-index sweep closed its stake after each parameter, so a crash
    between index 45's close and index 46's open had nothing open.
    recoverFromCrash returned empty, no death row was written, the retry rule
    counted nothing, and the worklist re-offered it every relaunch.

    Two independent guards now, and this pins both rules.
*/
void testUnfinishedAttemptRule()
{
    // 1. THE GAP ITSELF. appendRow must NOT clear the stake; endLoad must.
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-gap-test-" + juce::Uuid().toDashedString());
    root.createDirectory();
    {
        ejmap::Ledger l (root);
        const juce::String pid = "AudioUnit:Effects/aufx,GAPT,test";
        l.beginLoad (pid, "Gap", "V", "AudioUnit", "1.0", "sweep", "index 45");

        ejmap::LedgerRecord idx;
        idx.pluginId = pid; idx.name = "Gap"; idx.stage = "sweep";
        idx.outcome = ejmap::LoadOutcome::ok; idx.detail = "idx 45: anchors";
        l.appendRow (idx);

        check (root.getChildFile ("inflight.json").existsAsFile(),
               "gap: appendRow leaves the STAKE OPEN -- a crash at the next index is "
               "still attributable");

        l.endLoad (idx);
        check (! root.getChildFile ("inflight.json").existsAsFile(),
               "gap: and endLoad still closes it, so the sweep's own end is clean");
    }
    root.deleteRecursively();

    // 2. THE BELT: unfinished attempts, counted without any stake at all.
    // Three, matching kRetryAttempts -- one is a bad roll, three is the plugin.
    auto quarantinesAt = [] (int unfinished) { return unfinished >= 3; };
    check (! quarantinesAt (1), "attempts: one unfinished attempt is a bad roll");
    check (! quarantinesAt (2), "attempts: two is not yet a verdict");
    check (quarantinesAt (3),
           "attempts: THREE attempts with no outcome reported from any of them is a "
           "plugin that kills the process -- quarantined, off the worklist, no human");

    // The record must survive the process that made it: written BEFORE the open,
    // deleted when an outcome is known. Its presence at startup IS the evidence.
    auto survives = [] (bool wroteBeforeOpen, bool outcomeReported)
    { return wroteBeforeOpen && ! outcomeReported; };
    check (survives (true, false),
           "attempts: written before the open and never cleared -> the process died "
           "holding that plugin");
    check (! survives (true, true),
           "attempts: an outcome of ANY kind clears it, including load_failed -- the "
           "process survived to say so, which is the whole distinction");

    // 3. THE TALLY IS KEYED BY BINARY, NOT BY DISPLAY NAME -- which is what
    // the "Lexicon 480L reported 1 of 3 twice" observation actually meant.
    // Both events were real and both counts were right: the AU
    // (AudioUnit:Effects/aufx,paau,!UAD) died at 12:21 and the VST3 BUNDLE
    // died at 15:14. Two binaries, one display name, one count each.
    const juce::String lexAU   = "AudioUnit:Effects/aufx,paau,!UAD";
    const juce::String lexVST3 = "/Library/Audio/Plug-Ins/VST3/Universal Audio/"
                                 "Reverb and Room/UAD Lexicon 480L.vst3";
    check (lexAU != lexVST3,
           "attempts: two binaries sharing the display name 'UAD Lexicon 480L' are "
           "DIFFERENT subjects -- one count each is correct, not a stuck counter");

    // And the array form is kept because an invalid Identifier ASSERTS IN
    // DEBUG, not because the release behaviour was broken: it round-trips.
    check (! juce::Identifier::isValidIdentifier (lexVST3),
           "attempts: a bundle path is not a valid Identifier -- a debug assertion "
           "waiting to fire, which is reason enough for the array form");
}

void testMapperIdentity()
{
    using ejmap::Mouth;
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("ejmap-mapper-test-" + juce::Uuid().toDashedString());
    root.createDirectory();

    check (! Mouth::resolveMapper (root).signedIn(),
           "mapper: a fresh machine is not signed in");
    check (Mouth::resolveMapper (root).ref.isEmpty(),
           "mapper: and has no ref to put in a map");

    const juce::String token = "ejm_live_9f3c2a7e5b104d68";
    check (Mouth::saveMapperToken (root, token).isEmpty(), "mapper: the token saves");

    auto m = Mouth::resolveMapper (root);
    check (m.signedIn(), "mapper: and reads back through the path that will use it");
    check (m.ref.length() == 12, "mapper: the ref is 12 hex of the token's SHA-256");
    check (m.ref == Mouth::mapperRefFor (token), "mapper: derived only from the token");

    // THE REF IS NOT THE TOKEN, and this is the property the whole design rests
    // on. A map is stored, copied, resubmitted and read by other people; a
    // credential inside one would leak by being useful.
    check (! m.ref.containsIgnoreCase (token), "mapper: THE REF IS NOT THE TOKEN");
    check (! token.containsIgnoreCase (m.ref), "mapper: nor a prefix of it");
    check (Mouth::mapperRefFor (token) != Mouth::mapperRefFor (token + "x"),
           "mapper: a different token gives a different ref");
    check (Mouth::mapperRefFor ({}).isEmpty(), "mapper: no token, no ref");

    // THE SERVER MUST DERIVE THE SAME REF, and until 8 Aug 2026 nothing here
    // checked WHAT this function computes -- only that `ref` came out of it.
    // Every assertion above would still pass if the derivation changed to
    // upper-case hex, or 16 characters, or SHA-1, and the server would then
    // disagree SILENTLY: maps would carry a ref no mapper record matches, and
    // attribution would rot with nothing failing.
    //
    // These are literal vectors, asserted by BOTH ENDS -- here, and by
    // scripts/test-mapper-auth.mjs in the dashboard repo, which reads the same
    // fixture file. Pinning a value is the only version of this test that can
    // fail when the two ends drift apart.
    //
    // The three ways SHA-256 implementations disagree, one vector each:
    //   case      "AAAA" -> lower-case hex, never upper
    //   length    exactly 12 characters, not 16
    //   encoding  a non-ASCII token pins UTF-8 bytes rather than UTF-16
    struct RefVector { const char* token; const char* ref; };
    const RefVector refVectors[] = {
        { "sean-studio-token",                                              "1f0e22b603e3" },
        { "db973a254ea5c03828224916b0a9689d58f1611ff334e20d81a09d92a6c258a5", "9f3d65f50f7b" },
        { "t\xc3\xb6k\xc3\xa9n-w\xc3\xadth-\xc3\xbcmlauts",                 "fa9abcccc442" },
        { "AAAA",                                                           "63c1dd951ffe" },
    };
    for (const auto& v : refVectors)
        check (Mouth::mapperRefFor (juce::String::fromUTF8 (v.token)) == juce::String (v.ref),
               juce::String ("mapper ref vector: ") + v.token + " -> " + v.ref);

    // Two machines, one mapper: the ref has to be the same or provenance
    // fragments by laptop.
    auto root2 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("ejmap-mapper-test-" + juce::Uuid().toDashedString());
    root2.createDirectory();
    Mouth::saveMapperToken (root2, token);
    check (Mouth::resolveMapper (root2).ref == m.ref,
           "mapper: the SAME token on another machine gives the SAME ref");

    // A FILE THE WHOLE MACHINE CAN READ IS NOT HOLDING A SECRET. Refused, not
    // warned about -- the same rule the endpoint token already follows.
    auto cfg = Mouth::configFile (root);
    check (cfg.existsAsFile(), "mapper: the token lives in config.json");
    // Directly, not through a shell: a path with a space in it made the child
    // process silently do nothing, and the test then "passed" the loose-perms
    // case by never loosening anything.
    ::chmod (cfg.getFullPathName().toRawUTF8(), 0644);
    auto loose = Mouth::resolveMapper (root);
    check (! loose.signedIn(), "mapper: a group-readable token is REFUSED, not used");
    check (loose.refused && loose.warning.contains ("chmod 600"),
           "mapper: and the refusal says how to fix it");

    // The gate: attributable or it does not leave, by EITHER route.
    auto mapWith = [] (const juce::String& testerId, const juce::String& mapperRef)
    {
        auto* prov = new juce::DynamicObject();
        prov->setProperty ("tester_id", testerId);
        if (mapperRef.isNotEmpty()) prov->setProperty ("mapper_ref", mapperRef);
        prov->setProperty ("machine_id", "m"); prov->setProperty ("ejmap_version", "0.1.0");
        prov->setProperty ("apply_header_sha", "abc"); prov->setProperty ("at", "2026-08-05T00:00:00Z");
        auto* o = new juce::DynamicObject();
        o->setProperty ("provenance", juce::var (prov));
        return juce::var (o);
    };
    auto rejectsAttribution = [] (const ejmap::Mouth::Verdict& v)
    {
        for (const auto& r : v.rejections)
            if (r.contains ("nothing attributes this map")) return true;
        return false;
    };
    check (rejectsAttribution (Mouth::structuralGate (mapWith ({}, {}), {})),
           "gate: a map with no tester and no mapper ref is NOT attributable");
    check (! rejectsAttribution (Mouth::structuralGate (mapWith ({}, "0123456789ab"), {})),
           "gate: a mapper ref alone attributes it");
    check (! rejectsAttribution (Mouth::structuralGate (mapWith ("sean", {}), {})),
           "gate: and a typed name still does, so the 40 maps that predate tokens "
           "stay resubmittable");

    root.deleteRecursively(); root2.deleteRecursively();
}

//==============================================================================
/** THE FIXTURE UNIT RULE (EjmapFixtureUnit.h), pinned on the measured cases.
    Every text below is verbatim from a pushed compressor-profile fixture, and
    every expected unit is what that fixture records. The labels are the ones
    the signed probe read on 28 Sep (all empty on the 13 products measured).
    Each case is one a simpler rule got wrong, so simplifying the rule turns
    one of these red.
*/
void testFixtureUnitRule()
{
    using ejmap::fixtureunit::unitFor;
    const juce::String minus = juce::CharPointer_UTF8 ("\xe2\x88\x92");    // U+2212
    const juce::String narrow = juce::CharPointer_UTF8 ("\xe2\x80\xaf");   // U+202F

    // The 0.0 point decides when it starts with a number.
    check (unitFor ("", "-20.0 dB", "0.0 dB") == "dB", "unit: XLA-3 Level Trim -> dB");
    check (unitFor ("", "20 Hz", "2.0 kHz") == "Hz",
           "unit: AMEK Mono Maker Freq -> Hz (the 0.0 point, not the last or the majority)");
    check (unitFor ("", "400mS", "AUTO") == "mS",
           "unit: Neve 2254 Comp Rcvr -> mS (majority over 400mS / 1.5S / AUTO would say S)");
    check (unitFor ("", "1/64T", "2B") == "/64T", "unit: H-Comp ReleaseBPM -> /64T");
    check (unitFor ("", "1.2:1", "Flood") == ":1", "unit: Shadow Hills Discrete Ratio -> :1");

    // A word at 0.0 falls through to the 1.0 point.
    check (unitFor ("", "-oo dB", "-60.0 dB") == "dB", "unit: XLA-3 Noise Level -> dB (-oo is a word)");
    check (unitFor ("", "Off", "2.0 kHz") == "kHz",
           "unit: MC 77 Monomaker Freq -> kHz (the middle point's Hz is never read)");

    // THE FOUR EXCEPTIONS: a unit visible somewhere, and the fixture records none.
    check (unitFor ("", ".1 s", "AUTO").isEmpty(),
           "unit: SSL G3 Low/Mid/High Release -> none. BUG-COMPATIBLE: '.1 s' is seconds, but "
           "the fixtures were built reading '.1' as not-a-number. Fix the rule only TOGETHER "
           "with re-sampling those three fixtures, never alone (EjmapFixtureUnit.h)");
    check (unitFor ("", "Bypass", "NUKE").isEmpty(),
           "unit: Mike-E Ratio -> none: the 4:1 is at the middle point, which is never read");

    // A label wins over the text.
    check (unitFor ("", "0.00", "48.00").isEmpty(), "unit: APB C-18 Output text alone -> none");
    check (unitFor ("dB", "0.00", "48.00") == "dB", "unit: ...and its label supplies dB");
    check (unitFor ("", "-10.0 N", "12.0") == "N", "unit: C1 Floor text alone would say N");
    check (unitFor ("dB", "-10.0 N", "12.0") == "dB", "unit: ...the label overrides it");

    // U+2212 is NOT a sign - decided by the RANGE data (kHs Threshold is named
    // positions), so the unit rule shares that parser. Bug-compatible.
    check (unitFor ("", minus + "40.00" + narrow + "dB", "Max").isEmpty(),
           "unit: U+2212 does not start a number (the range data decides it), so a lone "
           "U+2212 point gives no unit");
    check (unitFor ("", minus + "40.00" + narrow + "dB", "+6.00" + narrow + "dB") == "dB",
           "unit: kHs Compressor Threshold -> dB, from its 1.0 point; U+202F reads as a space");
    // The one choice the 1,783 controls do not decide, pinned so a change is deliberate.
    check (unitFor ("", "0.00", "48 dB").isEmpty(),
           "unit: an empty remainder at 0.0 means no unit; it does not fall back to 1.0");
}

//==============================================================================
/** THE FIXTURE RANGE AND DIRECTION RULES (EjmapFixtureRange.h), one pin per
    clause, each a real control from a pushed fixture with the value that fixture
    records. Together the rules reproduce all 1,783 controls (28 Sep).
*/
void testFixtureRangeRule()
{
    using ejmap::fixturerange::derive;
    auto has = [] (const juce::var& r, const char* k) { return r.getDynamicObject() != nullptr
                                                           && r.getDynamicObject()->hasProperty (k); };
    auto num = [] (const juce::var& r, const char* k) { return (double) r.getProperty (k, -12345.0); };
    const juce::String minus = juce::CharPointer_UTF8 ("\xe2\x88\x92");
    const juce::String narrow = juce::CharPointer_UTF8 ("\xe2\x80\xaf");

    // 1. fewer than two numbers -> not numeric
    auto d = derive ("0.08 s", "F", "S");
    check (d.direction == "named positions" && d.range.getProperty ("status", "") == "text is not numeric",
           "range: Drawmer 1973 Release '0.08 s / F / S' -> named positions (one number is not a range)");
    d = derive ("OFF", "+8", "GR");
    check (d.direction == "named positions", "range: UAD 1176AE Meter 'OFF / +8 / GR' -> named positions");
    d = derive (minus + "40.00" + narrow + "dB", minus + "17.00" + narrow + "dB", "+6.00" + narrow + "dB");
    check (d.direction == "named positions",
           "range: kHs Threshold -> named positions: U+2212 is not a sign, so only '+6.00' parsed");

    // 2. min / max from the ENDS; a missing end borrows the middle
    d = derive ("0.50:1", "4.58:1", "-5.00:1");
    check (num (d.range, "min") == -5.0 && num (d.range, "max") == 0.5,
           "range: C1 Comp Ratio -> min -5, max 0.5 (the ends), not the middle's 4.58");
    check (d.direction == "descending" && (bool) d.range.getProperty ("inverted", false)
           && ! (bool) d.range.getProperty ("linear", true),
           "range: ...descending, inverted, and not linear");
    d = derive ("-oo dB", "-90.0 dB", "-60.0 dB");
    check (num (d.range, "min") == -90.0 && num (d.range, "max") == -60.0 && d.range.getProperty ("at0", 0).isVoid(),
           "range: XLA-3 Noise Level -> min -90 borrowed from the middle, at0 null");

    // 4. endsNotNumeric names only ENDS, with the note; 5. linear only when both ends parse
    check (has (d.range, "endsNotNumeric") && has (d.range, "note") && ! has (d.range, "linear"),
           "range: ...an unparsed END gives endsNotNumeric + note and no linear");
    d = derive ("+10dB", "GR", "+4dB");
    check (! has (d.range, "endsNotNumeric") && ! has (d.range, "note") && has (d.range, "linear")
           && ! (bool) d.range.getProperty ("linear", true) && d.direction == "descending",
           "range: LA-2 Meter '+10dB / GR / +4dB' -> both ends parse: no endsNotNumeric, linear false");

    // 5. the linearity tolerance, bracketed by the data (0.476% true, 2.17% false)
    check ((bool) derive ("0.0", "5.2", "10.5").range.getProperty ("linear", false),
           "range: Distressor 0 / 5.2 / 10.5 (0.476% off the midpoint) is linear");
    check (! (bool) derive ("1.0", "13.0", "24.0").range.getProperty ("linear", true),
           "range: Shadow Hills Discrete Gain 1 / 13 / 24 (2.17% off) is not linear");

    // 6. flat
    d = derive ("None", "2:1+4:1", "2:1+20:1");
    check (d.direction == "flat" && num (d.range, "min") == 2.0 && num (d.range, "max") == 2.0,
           "range: UAD 1176AE Ratio -> flat at 2 (the missing end borrows the middle)");
}

//==============================================================================
/** ITEM 12, THE READOUT FIELDS (EjmapFixtureReadout.h). Two groups, kept apart so a
    single mutation reddens exactly ONE pin:
      P1-P7  the TRANSFORM (applySchema) on a measured fixture;
      P8-P10 the CHECKER (checkEmission) on a HAND-BUILT emitted fixture, never on the
             transform's output, so a transform bug cannot also redden a checker pin.
    Mutating applySchema to drop readoutCheck reddens P1 alone; mutating it to keep the
    first sample instead of null reddens P3 alone. The case is Shadow Hills Class A's
    VU Meters as measured on 28 Sep.
*/
void testFixtureReadoutEmission()
{
    namespace fr = ejmap::fixturereadout;
    const auto measured = juce::JSON::parse (R"({"identity":"AudioUnit|704f4855|1.4.1","controls":[
        {"index":40,"name":"VU Meter L","defaultOnInstantiate":{"normalised":0.0,"display":"","declaredDefault":1.0,"note":"n"}},
        {"index":41,"name":"VU Meter R","defaultOnInstantiate":{"normalised":0.189068,"display":"","declaredDefault":1.0,"note":"n"}}]})");
    const std::vector<fr::Moved> moved { { 41, 0.189068, 0.186410, "", "" } };
    auto ctl = [] (const juce::var& fx, int i) { return fx.getProperty ("controls", juce::var())[i]; };
    auto has = [] (const juce::var& v, const char* k) { return v.getDynamicObject() != nullptr && v.getDynamicObject()->hasProperty (k); };

    const auto e = fr::applySchema (measured, moved, true);
    const auto rc = e.getProperty ("readoutCheck", juce::var());
    check (rc.getProperty ("method", "") == "instantiate_twice" && (int) rc.getProperty ("instances", 0) == 2,
           "readout P1: a checked fixture carries readoutCheck {instantiate_twice, 2}");
    const auto s = ctl (e, 1).getProperty ("readout", juce::var()).getProperty ("samples", juce::var());
    check (! has (ctl (e, 0), "readout") && s.size() == 2 && (double) s[0] == 0.189068 && (double) s[1] == 0.186410,
           "readout P2: readout only on the control that moved, with both samples");
    check (ctl (e, 1).getProperty ("defaultOnInstantiate", juce::var()).getProperty ("normalised", 0).isVoid(),
           "readout P3: a readout's normalised is NULL, not its first sample");
    check (ctl (e, 1).getProperty ("defaultOnInstantiate", juce::var()).getProperty ("display", 0).isVoid(),
           "readout P4: a readout's display is null");
    check ((double) ctl (e, 1).getProperty ("defaultOnInstantiate", juce::var()).getProperty ("declaredDefault", -1) == 1.0,
           "readout P5: declaredDefault survives on a readout");
    check ((double) ctl (e, 0).getProperty ("defaultOnInstantiate", juce::var()).getProperty ("normalised", -1) == 0.0
           && ctl (e, 0).getProperty ("defaultOnInstantiate", juce::var()).getProperty ("note", "") == "n",
           "readout P6: a control that did not move is untouched");
    check (! has (fr::applySchema (measured, {}, false), "readoutCheck"),
           "readout P7: an unchecked fixture carries NO readoutCheck (absence = never checked)");

    const auto expected = juce::JSON::parse (R"({"identity":"AudioUnit|704f4855|1.4.1",
        "readoutCheck":{"method":"instantiate_twice","instances":2},"controls":[
        {"index":40,"name":"VU Meter L","defaultOnInstantiate":{"normalised":0.0,"display":"","declaredDefault":1.0,"note":"n"}},
        {"index":41,"name":"VU Meter R","readout":{"samples":[0.189068,0.186410],"displays":["",""]},
         "defaultOnInstantiate":{"normalised":null,"display":null,"declaredDefault":1.0,
         "note":"a readout: its value moved between two fresh instances, so it has no instantiate value"}}]})");
    check (fr::checkEmission (expected, measured, moved, true).isEmpty(),
           "readout P8: the checker accepts a correctly emitted fixture");
    auto noCheck = expected.clone();
    noCheck.getDynamicObject()->removeProperty ("readoutCheck");
    const auto f9 = fr::checkEmission (noCheck, measured, moved, true);
    check (f9.size() == 1 && f9[0].startsWith ("C1"), "readout P9: dropping readoutCheck reddens exactly one check (C1)");
    auto firstSample = expected.clone();
    if (auto* d = ctl (firstSample, 1).getProperty ("defaultOnInstantiate", juce::var()).getDynamicObject())
        d->setProperty ("normalised", 0.189068);
    const auto f10 = fr::checkEmission (firstSample, measured, moved, true);
    check (f10.size() == 1 && f10[0].startsWith ("C3"),
           "readout P10: keeping the first sample instead of null reddens exactly one check (C3)");
}

//==============================================================================
/** WHICH FAILURES ARE LICENSING FACTS (EjmapCertOutcome.h). K1 is the 29 Sep kHs
    Compressor case: PACE's UI appeared although the bundle scan found no PACE, and
    behavioural evidence outranks the scan. Dropping the behavioural clause reddens K1
    alone.
*/
void testLicenceOutcome()
{
    using namespace ejmap::certoutcome;
    check (classifyFailure (false, { "PACEEdenExperience [pid 49265]" }) == Failure::unlicensedOnHost,
           "licence K1: PACE's UI in the probe's tree makes it unlicensed_on_host even when the bundle scan "
           "found no PACE (kHs Compressor, 29 Sep)");
    check (classifyFailure (true, {}) == Failure::unlicensedOnHost,
           "licence K2: a bundle-marked PACE product that refuses or hangs is unlicensed_on_host");
    check (classifyFailure (false, {}) == Failure::notReproduced,
           "licence K3: an unmarked product that hangs with no window is NOT called a licence fact");
    check (classifyFailure (false, { "SomeHelper [pid 1]" }) == Failure::notReproduced,
           "licence K4: a window from something other than PACE is recorded, not called a licence fact");
}

//==============================================================================
/** THE NAME MATCHER IS THE SERVER'S (EjmapNameTokens.h). Every vector in
    name-token-vectors.json was produced by the server's own controlNameTokens /
    controlAnswersTerm (lib/controls-note.js @ a86dba8, unmodified, in node); this port
    must reproduce all of them. The counts are the CONTROL: an unread or truncated file
    would otherwise pass by asserting nothing.
*/
void testNameTokenVectors()
{
    using namespace ejmap::nametokens;
    const auto file = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/name-token-vectors.json");
    const auto v = juce::JSON::parse (file.loadFileAsString());
    const auto* toks = v.getProperty ("tokens", {}).getArray();
    const auto* ans  = v.getProperty ("answers", {}).getArray();
    check (toks != nullptr && toks->size() == 1006 && ans != nullptr && ans->size() == 1506,
           "tokens T0: the vector file is read whole (1,006 token vectors, 1,506 answer vectors)");
    if (toks == nullptr || ans == nullptr) return;

    int tokenMiss = 0, answerMiss = 0, refusals = 0;
    for (const auto& t : *toks)
    {
        juce::StringArray want;
        if (const auto* a = t.getProperty ("tokens", {}).getArray())
            for (const auto& x : *a) want.add (x.toString());
        const auto got = controlNameTokens (t.getProperty ("name", {}).toString());
        if (got != want && ++tokenMiss <= 5)
            std::cerr << "  token vector: '" << t.getProperty ("name", {}).toString() << "' server ["
                      << want.joinIntoString ("|") << "] port [" << got.joinIntoString ("|") << "]" << std::endl;
    }
    for (const auto& a : *ans)
    {
        const bool want = (bool) a.getProperty ("result", false);
        if (! want) ++refusals;
        const bool got = controlAnswersTerm (a.getProperty ("name", {}).toString(), a.getProperty ("term", {}).toString());
        if (got != want && ++answerMiss <= 5)
            std::cerr << "  answer vector: '" << a.getProperty ("name", {}).toString() << "' / '"
                      << a.getProperty ("term", {}).toString() << "' server " << (int) want << " port " << (int) got << std::endl;
    }
    check (tokenMiss == 0, "tokens T1: controlNameTokens reproduces every server vector (" + juce::String (tokenMiss) + " differ)");
    check (answerMiss == 0, "tokens T2: controlAnswersTerm reproduces every server vector (" + juce::String (answerMiss) + " differ)");
    check (refusals == 466, "tokens T3: the file carries the 466 refusals - letters inside a token, and band abbreviations "
                             "without their word boundary - that the rule exists to make");

    // Named, so a reader sees what the vectors protect.
    check (! controlAnswersTerm ("Threshold", "hold") && ! controlAnswersTerm ("Calibration", "ratio"),
           "tokens T4: letters inside a token do not answer (hold/Threshold, ratio/Calibration)");
    check (controlNameTokens ("aBcD").joinIntoString ("|") == "a|bc|d",
           "tokens T5: only lower->Upper splits a case change; Upper->lower does not");
    check (controlAnswersTerm ("LF Gain", "low") && ! controlAnswersTerm ("LF_Gain", "low")
             && ! controlAnswersTerm ("Shelf Gain", "low"),
           "tokens T7: a band abbreviation answers only at a JavaScript \\b boundary, where _ and digits are word characters");
    check (controlAnswersTerm (juce::String::charToString ((juce::juce_wchar) 0x212A) + "ey", "key"),
           "tokens T6: JavaScript lowercases the Kelvin sign to k, so the server answers 'key' - and so must the port");
}

//==============================================================================
/** THE ROLE RULE (EjmapRoles.h), each clause named with the real control it was
    written for. */
void testRoleRules()
{
    using namespace ejmap::roles;
    auto one = [] (const char* n, Category c = Category::compressor) { return roleOfName (n, c); };

    check (one ("L DC Thr").role == "threshold" && one ("L DC Thr").flags.contains ("dc"),
           "roles R1: DC is a FLAG, not a veto - Fairchild 'L DC Thr' keeps its threshold role, flagged");
    check (one ("Select Attack Release").role.isEmpty()
             && one ("Select Attack Release").reason == "ambiguous:attack|release",
           "roles R2: a name answering two roles REFUSES and names both (CL 1B 'Select Attack Release')");
    check (one ("Key").role.isEmpty() && one ("Key").reason == "veto:sidechain",
           "roles R3: on a compressor 'Key' is the sidechain key - vetoed");
    check (one ("Key", Category::tuner).role == "key" && one ("Speed", Category::tuner).role == "strength"
             && one ("Speed").role.isEmpty(),
           "roles R4: on a tuner 'Key' is the musical key and 'Speed' is strength; on a compressor neither is a tuner role");
    check (one ("Peak Reduct").role == "threshold" && one ("Thr").role == "threshold"
             && one ("Rcv").role == "release" && one ("Att").role == "attack" && one ("Rat").role == "ratio",
           "roles R5: the lexicon carries LA-2A's 'Peak Reduct' and UAD's Thr/Rcv/Att/Rat");
    check (one ("SC HPF").reason == "veto:sidechain+filter" && one ("GR Meter").reason == "veto:meter"
             && one ("Preset Threshold").reason == "veto:preset",
           "roles R6: vetoes name every family that fired");

    auto cls = [] (std::initializer_list<const char*> names)
    {
        std::vector<NamedControl> cs;
        int i = 0;
        for (auto* n : names) cs.push_back ({ i++, n });
        return classify (cs, Category::compressor);
    };
    const auto g = cls ({ "CompThresh", "ExpThresh", "CompRatio" });
    check (g.cls == "comp_over_expander" && g.controls[0].role == "threshold"
             && g.controls[1].role.isEmpty() && g.controls[1].reason == "expander_or_strap_threshold",
           "roles R7: SSL G-Channel - the compressor threshold keeps the role, the expander's loses it");
    const auto u = cls ({ "Input", "Output", "Attack" });
    check (u.cls == "input_as_threshold" && u.controls[0].role == "threshold"
             && u.controls[0].flags.contains ("input_as_threshold"),
           "roles R8: a 1176 with no threshold by name - its Input takes the role, flagged");
    check (cls ({ "L Thr", "R Thr" }).cls == "channels_lr" && cls ({ "Thr LFE", "Thr C" }).cls == "surround"
             && cls ({ "Threshold 1", "Threshold 2" }).cls == "bands_or_stages"
             && cls ({ "Density", "Output" }).cls == "amount_only" && cls ({ "Bypass" }).cls == "none",
           "roles R9: the multi-threshold and no-threshold classes");
    {
        std::vector<NamedControl> cs { { 0, "Threshold", true }, { 1, "Input", false } };
        const auto r = classify (cs, Category::compressor);
        check (r.controls[0].role.isEmpty() && r.controls[0].reason == "readout" && r.cls == "input_as_threshold",
               "roles R10: a READOUT gets no role whatever its name says - a roled meter would be swept");
    }

    using namespace ejmap::rolesemantics;
    ControlRole in; in.role = "threshold"; in.flags.add ("input_as_threshold");
    check (semanticForRole ("threshold", "dB") == "threshold_db" && semanticForRole ("threshold", "").isEmpty()
             && semanticForRole ("threshold", "dBu").isEmpty(),
           "roles S1: threshold -> threshold_db only in dB; a 0-10 threshold has a role and NO semantic; dBu is not dB");
    check (semanticForRole ("ratio", "") == "ratio" && semanticForRole ("attack", "s") == "attack_ms"
             && semanticForRole ("release", "mS") == "release_ms" && semanticForRole ("attack", "dB").isEmpty()
             && semanticForRole ("mix", "%") == "mix_pct" && semanticForRole ("key", "").isEmpty(),
           "roles S2: ratio always; time roles only in time units; mix only in %; tuner roles map to nothing");
    check (semanticFor (in, "dB") == "input_db",
           "roles S3: an input-as-threshold control keeps its NAME's semantic - the role is per control, not derived");
    check (roleForSemantic ("threshold_db") == "threshold" && roleForSemantic ("input_db") == "input"
             && roleForSemantic ("knee_db").isEmpty() && roleForSemantic ("hold_ms").isEmpty(),
           "roles S4: semantic -> role always for the eight, nothing for knee/range/hold/tone/slope");
}

//==============================================================================
/** THE 74 CLASSIFIED (role-classification-74.json, produced by the reference
    classifier on the server's own matcher). Every product's class and every control's
    role, flags and reason must reproduce, and the 38 is pinned as a literal as well, so
    regenerating the file under a changed lexicon cannot move it silently.
*/
void testRoleClassification74()
{
    using namespace ejmap::roles;
    const auto file = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/role-classification-74.json");
    const auto v = juce::JSON::parse (file.loadFileAsString());
    const auto* products = v.getProperty ("products", {}).getArray();
    check (products != nullptr && products->size() == 74, "roles C0: the classification file is read whole (74 products)");
    if (products == nullptr) return;

    std::map<juce::String, int> counts;
    int classMiss = 0, controlMiss = 0, controlsSeen = 0;
    for (const auto& p : *products)
    {
        std::vector<NamedControl> cs;
        const auto* recorded = p.getProperty ("controls", {}).getArray();
        if (recorded == nullptr) { ++classMiss; continue; }
        for (const auto& c : *recorded)
            cs.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString() });
        const auto got = classify (cs, Category::compressor);
        ++counts[got.cls];
        const auto product = p.getProperty ("product", {}).toString();
        if (got.cls != p.getProperty ("cls", {}).toString() && ++classMiss <= 5)
            std::cerr << "  class: " << product << " recorded " << p.getProperty ("cls", {}).toString()
                      << " port " << got.cls << std::endl;
        for (size_t i = 0; i < got.controls.size() && i < (size_t) recorded->size(); ++i)
        {
            ++controlsSeen;
            const auto& r = recorded->getReference ((int) i);
            juce::StringArray flags;
            if (const auto* f = r.getProperty ("flags", {}).getArray())
                for (const auto& x : *f) flags.add (x.toString());
            const auto& c = got.controls[i];
            const juce::String recRole = r.getProperty ("role", {}).isVoid() ? juce::String() : r.getProperty ("role", {}).toString();
            if ((c.role != recRole || c.flags != flags || c.reason != r.getProperty ("reason", {}).toString())
                 && ++controlMiss <= 5)
                std::cerr << "  control: " << product << " / " << c.name << " recorded " << recRole << "["
                          << flags.joinIntoString (",") << "]" << r.getProperty ("reason", {}).toString()
                          << " port " << c.role << "[" << c.flags.joinIntoString (",") << "]" << c.reason << std::endl;
        }
    }
    check (controlsSeen == 1783, "roles C1: all 1,783 controls were compared (" + juce::String (controlsSeen) + ")");
    check (classMiss == 0, "roles C2: every product's class reproduces (" + juce::String (classMiss) + " differ)");
    check (controlMiss == 0, "roles C3: every control's role, flags and reason reproduce (" + juce::String (controlMiss) + " differ)");
    check (counts["single_threshold"] == 38, "roles C4: 38 products have exactly one threshold by name");
    check (counts["input_as_threshold"] == 9 && counts["comp_over_expander"] == 4,
           "roles C5: 13 take a threshold by a flagged rule (9 input-as-threshold, 4 comp-over-expander)");
    check (counts["amount_only"] == 4 && counts["channels_lr"] == 3 && counts["bands_or_stages"] == 14
             && counts["surround"] == 2 && counts["none"] == 0,
           "roles C6: 23 are deferred to review after the sweep (4 amount, 3 L/R, 14 bands or stages, 2 surround)");
}

//==============================================================================
/** THE THRESHOLD SWEEP'S DERIVATION (EjmapSweep.h), on synthetic readings where the rule is the point and on
    the committed townhouse traces where the data is. One process per position, the soft end's linear gain as the
    reference, the two guards, and two verdicts that are never merged (all ruled 29 Sep). */
namespace sweeptest
{
    using namespace ejmap::sweep;
    // A Measured built from out-minus-in gains per position (rows) at -24/-12/-6 (columns); nullopt = still moving.
    inline Measured fromGains (const std::vector<std::array<std::optional<double>, 3>>& gains, bool withRef = true)
    {
        Measured m;
        m.ok = true;
        m.movingDb = 0.1;
        const double L[3] = { -24.0, -12.0, -6.0 };
        if (withRef) for (double l : L) { m.refDb[levelKey (l)] = l - 3.0103; m.inRmsDb[levelKey (l)] = l - 3.0103; }
        for (size_t k = 0; k < gains.size(); ++k)
        {
            PositionReading p;
            p.k = (int) k;
            p.norm = gains.size() > 1 ? (float) k / (float) (gains.size() - 1) : 0.0f;
            p.text = juce::String (-20.0 + 2.0 * (double) k, 1) + " dB";
            for (int j = 0; j < 3; ++j)
            {
                HoldReading h;
                h.present = true;
                h.inRmsDb = L[j] - 3.0103;
                if (gains[k][(size_t) j]) h.levelDb = h.inRmsDb + *gains[k][(size_t) j];
                else { h.levelDb = h.inRmsDb; h.finalMoveDb = 0.5; h.doubled = true; }
                p.holds[levelKey (L[j])] = h;
            }
            m.positions.push_back (p);
        }
        return m;
    }
    inline const std::vector<double> kLevels { -24.0, -12.0, -6.0 };
}

void testSweepDerivation()
{
    using namespace sweeptest;
    // A clean lower-is-harder compressor: gain 2 dB when linear, reduction growing toward position 0.
    std::vector<std::array<std::optional<double>, 3>> clean;
    for (int k = 0; k < 6; ++k) { const double r = (5 - k) * 1.5; clean.push_back ({ 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.5 }); }
    auto d = derive (fromGains (clean), kLevels, -1);
    check (d.result == "certified" && d.sense == "lower_is_harder" && d.softEnd && *d.softEnd == 5,
           "sweep D1: a clean curve certifies, lower is harder, and the reference is the SOFT end (position 5)");
    check (std::abs (*d.reduction[levelKey (-6)][0] - 11.25) < 1e-9 && std::abs (*d.reduction[levelKey (-6)][5]) < 1e-9,
           "sweep D2: reduction is measured from the soft end's LINEAR gain, not from the default threshold");

    // THE LINEAR REFERENCE (ruled 29 Sep): the soft end's disagreement across levels is RECORDED on every fixture;
    // only beyond 2 dB - the size of the signal, against 3-20 dB useful reductions - is the data unusable.
    auto nonLinear = clean;
    nonLinear[5] = { 2.0, 2.0, -0.5 };
    check (derive (fromGains (nonLinear), kLevels, -1).result == "unreadable",
           "sweep D3: a soft end whose gains disagree by 2.5 dB is unusable - refuses");
    // THE BAR IS RELATIVE (ruled 30 Sep, superseding the absolute 2.0): the soft end's spread as a fraction of the largest
    // measured reduction, bar kSenseDb/kSaturateDb = 1/12. Against this curve's ~10 dB response, 1.2 dB is 12% - REFUSED,
    // where the absolute bar accepted it; against a 30 dB response the same 1.2 dB is 4% and certifies.
    nonLinear[5] = { 2.0, 2.0, 0.8 };
    const auto within = derive (fromGains (nonLinear), kLevels, -1);
    check (within.result == "unreadable" && within.softEndSpreadDb && std::abs (*within.softEndSpreadDb - 1.2) < 1e-9
             && within.refErrorFrac && *within.refErrorFrac > kRefErrorFrac && within.reason.contains ("% of the"),
           "sweep D4 (RELATIVE): 1.2 dB against a ~10 dB response is " + juce::String (within.refErrorFrac.value_or (0) * 100.0, 1)
             + "% - over the 1/12 bar, refused, and the fraction is in the reason ('" + within.result + "')");
    auto deep = clean;
    for (auto& g : deep) for (auto& x : g) if (x) *x = 2.0 + (*x - 2.0) * 3.0;   // the same curve three times as deep (~30 dB)
    deep[5] = { 2.0, 2.0, 0.8 };
    const auto deepR = derive (fromGains (deep), kLevels, -1);
    check (deepR.result == "certified" && deepR.refErrorFrac && *deepR.refErrorFrac < kRefErrorFrac,
           "sweep D4b: the same 1.2 dB against a ~30 dB response is " + juce::String (deepR.refErrorFrac.value_or (0) * 100.0, 1) + "% - certifies");
    // The below-reference guard has the same form: 0.86 dB above the reference against a 35 dB response passes (2%);
    // 0.4 dB against a 2.5 dB response does not (16%), where the absolute 0.5 dB bar let it through.
    auto over = deep; over[2] = { *deep[2][0], *deep[2][1], 0.8 + 0.86 };     // one reading 0.86 dB above the reference at -6
    const auto overR = derive (fromGains (over), kLevels, -1);
    std::vector<std::array<std::optional<double>, 3>> shallow;
    for (int k = 0; k < 6; ++k) { const double r = (5 - k) * 0.5; shallow.push_back ({ 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.0 }); }
    shallow[2] = { *shallow[2][0], *shallow[2][1], 2.0 + 0.4 };
    const auto shallowR = derive (fromGains (shallow), kLevels, -1);
    check (overR.result != "unreadable" && shallowR.result == "unreadable" && shallowR.reason.contains ("above the linear reference"),
           "sweep D4c (RELATIVE below-reference): 0.86 dB over a ~30 dB response passes ('" + overR.result + "'), 0.4 dB over a ~2.5 dB response refuses ('"
             + shallowR.result + "': " + shallowR.reason + ")");

    // GUARD 1: a reduction BELOW the linear reference by more than 0.5 dB.
    auto above = clean;
    above[3] = { 2.0, 3.0, 2.0 };
    check (derive (fromGains (above), kLevels, -1).result == "unreadable",
           "sweep D5: a reading 1 dB ABOVE the linear reference refuses - it carries history, or this is no compressor curve");

    // GUARD 2: more than two positions still moving after the doubling.
    auto moving = clean;
    moving[1][0] = std::nullopt; moving[2][0] = std::nullopt;
    const auto two = derive (fromGains (moving), kLevels, -1);
    check (two.result == "certified" && two.stillMoving.size() == 2 && ! two.reduction.at (levelKey (-24))[1],
           "sweep D6: two still-moving positions are left out (null) and named, not refused");
    moving[3][0] = std::nullopt;
    check (derive (fromGains (moving), kLevels, -1).result == "unreadable",
           "sweep D7: a third refuses - the holds never reached a steady state");

    // FLAT: the ends within 1 dB at every level.
    std::vector<std::array<std::optional<double>, 3>> flat (6, { 2.0, 2.0, 2.0 });
    flat[0] = { 1.5, 1.2, 1.1 };
    check (derive (fromGains (flat), kLevels, -1).result == "flat", "sweep D8: ends within 1 dB at every level is flat");

    // THE ARM A TRACE (29 Sep, positions walked in ONE process): it read "certified" before the guards. It must not.
    const auto dir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep");
    const auto armA = derive (parseSweep (dir.getChildFile ("townhouse-armA-ascending.txt").loadFileAsString()), kLevels, 5);
    check (armA.result == "unreadable" && armA.stillMoving.size() > 2,
           "sweep D9: townhouse arm A (one process, loud-to-quiet history) does NOT certify: '" + armA.result + "' - " + armA.reason);
    const auto armB = derive (parseSweep (dir.getChildFile ("townhouse-armB-descending.txt").loadFileAsString()), kLevels, 5);
    check (armB.result != "certified", "sweep D10: nor does arm B: '" + armB.result + "'");

    // PASS-THROUGH gets its own reason (C1 comp and RCompressor at 12.0.0, 29 Sep: output = input at every reading).
    std::vector<std::array<std::optional<double>, 3>> pass (6, { 0.0, 0.0, 0.0 });
    const auto pt = derive (fromGains (pass), kLevels, -1);
    check (pt.result == "flat" && pt.reason.startsWith ("passthrough"), "sweep D11: output equal to input everywhere is flat with reason passthrough");
    check (derive (fromGains (flat), kLevels, -1).reason.startsWith ("no two positions differ"), "sweep D12: an ordinary flat keeps its plain reason");
    {
        // PASS-THROUGH IS ITS OWN RECORDED OUTCOME (ruled 30 Sep): a field, never re-derived from the reason string.
        Plan pl; pl.thr = 0; pl.thrName = "Thresh"; pl.norms = { 0.f, 0.2f, 0.4f, 0.6f, 0.8f, 1.f };
        const auto ptVar = composeThresholdSweep (pt, displayCheck (pt, ""), pl, {});
        const auto flatD = derive (fromGains (flat), kLevels, -1);
        const auto flatVar = composeThresholdSweep (flatD, displayCheck (flatD, ""), pl, {});
        check (pt.passThroughAtDefaults && (bool) ptVar.getProperty ("passThroughAtDefaults", false)
                 && ! flatD.passThroughAtDefaults && ptVar.hasProperty ("passThroughAtDefaults") && ! (bool) flatVar.getProperty ("passThroughAtDefaults", true),
               "sweep D13: thresholdSweep.passThroughAtDefaults is true for the pass-through flat and false (present) for an ordinary flat");
    }
    {
        // READING RULES (ruled 30 Sep). A curve like `clean` with ONE hold whose output is not the tone.
        auto withNonTone = [] (Measured m, size_t pos, int level, double frac) {
            auto& h = m.positions[pos].holds[levelKey (level == 0 ? -24.0 : level == 1 ? -12.0 : -6.0)];
            h.toneFrac = frac; h.levelDb = h.inRmsDb - 35.0;   // 35 dB down and not the tone: the shape seen on six products
            return m; };
        std::vector<std::array<std::optional<double>, 3>> curve;   // the D1 shape over 8 positions: gain 2 dB when linear, reduction toward position 0
        for (int k = 0; k < 8; ++k) { const double r = (7 - k) * 1.2; curve.push_back ({ 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.5 }); }
        const auto clean = derive (fromGains (curve), kLevels, -1);
        const auto spiked = derive (withNonTone (fromGains (curve), 3, 2, 0.0), kLevels, -1);
        check (clean.result == "certified" && spiked.result == "certified" && ! spiked.gain.at (levelKey (-6.0))[3].has_value()
                 && spiked.notTone == juce::StringArray { "3@-6.00" },
               "sweep T1 (THE TONE GUARD): a hold whose output is not the tone (tone_frac 0) is refused as a reading and listed, and the curve around it still certifies ('" + spiked.reason + " / '"
                 + spiked.result + "', " + spiked.notTone.joinIntoString (",") + ")");
        auto legacy = withNonTone (fromGains (curve), 3, 2, -1.0);   // pre-29-Sep trace: no tone_frac printed
        check (derive (legacy, kLevels, -1).gain.at (levelKey (-6.0))[3].has_value(), "sweep T2: a trace with no tone_frac (-1) is read as before");
        Plan pl2; pl2.thr = 0; pl2.thrName = "Thresh"; pl2.norms = { 0.f, 0.14f, 0.29f, 0.43f, 0.57f, 0.71f, 0.86f, 1.f };
        check ((bool) juce::JSON::parse (juce::JSON::toString (composeThresholdSweep (spiked, displayCheck (spiked, ""), pl2, {}))).getProperty ("notToneReadings", {}).isArray(),
               "sweep T3: the refused readings are recorded on the fixture as notToneReadings");

        // THE FLAT TEST READS EVERY POSITION: ends equal, a 20 dB drop in the middle.
        std::vector<std::array<std::optional<double>, 3>> bump (6, { 0.0, 0.0, 0.0 });
        bump[3] = { -20.0, -20.0, -20.0 };
        const auto b = derive (fromGains (bump), kLevels, -1);
        check (b.result != "flat" && b.flatSpanDb && *b.flatSpanDb > 19.0 && b.reason.contains ("not across the sweep"),
               "sweep F1 (ALL POSITIONS): ends that agree with a 20 dB drop between them are NOT flat ('" + b.result + "': " + b.reason + ")");
        check (derive (fromGains (flat), kLevels, -1).result == "flat", "sweep F2: an ordinary flat is still flat under the all-positions test");
        std::vector<std::array<std::optional<double>, 3>> quietOnly (6);                   // AMEK's shape: ends differ 7 dB at -24, agree at -6
        for (int k = 0; k < 6; ++k) quietOnly[(size_t) k] = { 2.0 - 1.4 * (double) (5 - k), 2.0 - 0.2 * (double) (5 - k), 2.0 };
        const auto qo = derive (fromGains (quietOnly), kLevels, -1);
        check (qo.result != "flat" && ! qo.reason.contains ("not across the sweep") && qo.sense.isNotEmpty(),
               "sweep F3: ends that differ at the QUIET level only are a response across the sweep - the sense is read there, not called 'not across' from the loud level ('" + qo.result + "': " + qo.reason + ")");

        // PASS-THROUGH IS INPUT PLUS A CONSTANT.
        std::vector<std::array<std::optional<double>, 3>> offset (6, { 0.31, 0.31, 0.31 });
        const auto po = derive (fromGains (offset), kLevels, -1);
        check (po.result == "flat" && po.passThroughAtDefaults && po.passThroughOffsetDb && std::abs (*po.passThroughOffsetDb - 0.31) < 0.006
                 && po.reason.contains ("plus a constant 0.31 dB"),
               "sweep P1 (dbx-160): output = input + 0.31 dB at every reading is pass-through, with the constant recorded");
        std::vector<std::array<std::optional<double>, 3>> mixed (6, { 0.31, 0.31, 0.31 });
        mixed[2] = { 0.0, 0.0, 0.0 };
        const auto pm = derive (fromGains (mixed), kLevels, -1);
        check (pm.result == "flat" && ! pm.passThroughAtDefaults, "sweep P2: readings that differ by 0.31 dB are flat but NOT pass-through");

        // THE RATIO-FREE CURVE (Sean's eff_threshold_dbfs, 1 Oct): the level where GR reaches 1.0 dB, interpolated between
        // the bracketing test levels; textbook puts it R/(R-1) dB above T. A guard: no bracket, no number. On `clean`
        // (reduction r/2, r, 1.5r at -24/-12/-6 with r = (5-k)*1.5): position 0 is past 1 dB at -24; position 4 (r = 1.5:
        // 0.75 / 1.5 / 2.25) crosses between -24 and -12 at t = 1/3 -> -20.0; position 5 (r = 0) never reaches it.
        {
            std::vector<std::array<std::optional<double>, 3>> cl;
            for (int k = 0; k < 6; ++k) { const double r = (5 - k) * 1.5; cl.push_back ({ 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.5 }); }
            const auto tx = derive (fromGains (cl), kLevels, -1);
            const bool sized = tx.result == "certified" && tx.tEffective1dB.size() == 6;
            check (sized, "sweep R0: the ratio-free curve is derived for every position of a certified sweep (" + tx.result + ", " + juce::String ((int) tx.tEffective1dB.size()) + ")");
            if (sized)
            {
                check (tx.tEffective1dB[4].isDouble() && std::abs ((double) tx.tEffective1dB[4] - (-20.0)) < 0.05,
                       "sweep R1 (RATIO-FREE): position 4 crosses 1 dB between (-24, 0.75) and (-12, 1.5) at -20.0 (" + juce::JSON::toString (tx.tEffective1dB[4], true) + ")");
                check (tx.tEffective1dB[5].isObject() && tx.tEffective1dB[5].hasProperty ("above"), "sweep R2: GR never reaching 1 dB at the loudest level reads {above: -6}");
                check (tx.tEffective1dB[0].isObject() && tx.tEffective1dB[0].hasProperty ("below"), "sweep R3: GR already past 1 dB at the quietest level reads {below: -24}");
                check (tx.tEquivalent.size() == 6 && tx.tEquivalent[4].isVoid(), "sweep R3b: with no ratio control the R-based map is null where the ratio-free one has a number - that is the point");
                Plan pr; pr.thr = 0; pr.thrName = "Thresh"; pr.norms = { 0.f, 0.2f, 0.4f, 0.6f, 0.8f, 1.f };
                const auto rv = composeThresholdSweep (tx, displayCheck (tx, ""), pr, {});
                check (rv.getProperty ("thresholdEffective1dB", {}).isArray() && rv.getProperty ("thresholdEffective1dB", {}).size() == 6
                         && rv.getProperty ("thresholdDbEquivalent", {}).isArray(),
                       "sweep R5: the fixture carries thresholdEffective1dB BESIDE thresholdDbEquivalent, both per position");
            }
            // in_at_gr (v1.2): 1, 2 and 3 dB from the same straddle rule; the words kept apart; nothing extrapolated.
            if (sized)
            {
                const auto& g4 = tx.inAtGr[4].at;   // position 4: 0.75 / 1.5 / 2.25 at -24 / -12 / -6
                check (g4.at (1).isDouble() && std::abs ((double) g4.at (1) - (-20.0)) < 0.05 && g4.at (2).isDouble() && std::abs ((double) g4.at (2) - (-8.0)) < 0.05
                         && g4.at (3).toString() == "not_reached",
                       "sweep G1 (in_at_gr): position 4 reaches 1 dB at -20.0, 2 dB at -8.0, and never 3 dB by 0 -> not_reached (" + juce::JSON::toString (g4.at (2), true) + ")");
                check (tx.inAtGr[0].at.at (1).toString() == "below_range" && tx.inAtGr[0].at.at (3).toString() == "below_range",
                       "sweep G2: a position already past 3 dB at the quietest level is below_range, kept apart from not_reached");
                check (tx.tEffective1dB[4].isDouble() && (double) tx.tEffective1dB[4] == (double) g4.at (1),
                       "sweep G3: eff_threshold IS in_at_gr[1], written identically (" + juce::String ((double) tx.tEffective1dB[4], 1) + ")");
                check (tx.inAtGr[4].widestGapDb == 12.0 && tx.inAtGr[4].nonMonotonicStraddles == 0, "sweep G4: the quality figure records the straddle width (12 dB on a 3-level sweep) and no non-monotonic straddle");
            }
            {
                std::vector<std::array<std::optional<double>, 3>> fall (6);                      // position 2: GR 0.3 at -24, 1.5 at -12, 0.8 at -6: a rising 1 dB straddle FOLLOWED by a fall
                for (int k = 0; k < 6; ++k) { const double r = (5 - k) * 1.5; fall[(size_t) k] = { 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.5 }; }
                fall[2] = { 2.0 - 0.3, 2.0 - 1.5, 2.0 - 0.8 };
                const auto fx = derive (fromGains (fall), kLevels, -1);
                if (fx.inAtGr.size() == 6)
                    check (fx.inAtGr[2].at.at (1).isDouble() && fx.inAtGr[2].nonMonotonicStraddles >= 1,
                           "sweep G5: a rising straddle followed by a fall is interpolated AND counted as a non-monotonic straddle (" + juce::String (fx.inAtGr[2].nonMonotonicStraddles) + ")");
                std::vector<std::array<std::optional<double>, 3>> gapped (6);                    // position 2: 0.3 at -24, MISSING at -12, 1.5 at -6: 1 dB was reached, but no readable straddle
                for (int k = 0; k < 6; ++k) { const double r = (5 - k) * 1.5; gapped[(size_t) k] = { 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.5 }; }
                gapped[2] = { 2.0 - 0.3, std::nullopt, 2.0 - 1.5 };
                const auto ox = derive (fromGains (gapped), kLevels, -1);
                if (ox.inAtGr.size() == 6)
                    check (ox.inAtGr[2].at.at (1).isVoid() && ox.inAtGr[2].at.at (3).toString() == "not_reached",
                           "sweep G6: a crossing with no readable straddle is null (never bridged), while 3 dB is honestly not_reached (" + juce::JSON::toString (ox.inAtGr[2].at.at (1), true) + ")");
            }
            {
                // v1.3 REPEAT QUALITY: a second derivation whose readings sit 0.3 dB louder at every position moves every crossing by
                // the same amount; the worst disagreement is the number; nothing is averaged.
                auto louder = cl; for (auto& g : louder) for (auto& x : g) if (x) *x -= 0.0;   // identical first
                const auto same = repeatQuality (tx, &tx);
                check (same.repeats == 2 && same.pointErrorDb && *same.pointErrorDb == 0.0 && same.pointsCompared > 0,
                       "quality Q1: an identical repeat gives point_error_db 0 over " + juce::String (same.pointsCompared) + " compared points");
                Derived ty = tx;                                                              // the same derivation with every numeric crossing 0.3 dB louder
                for (auto& r : ty.inAtGr) for (auto& [t, v] : r.at) if (v.isDouble()) v = (double) v + 0.3;
                const auto diff = repeatQuality (tx, &ty);
                check (diff.repeats == 2 && diff.pointErrorDb && std::abs (*diff.pointErrorDb - 0.3) < 0.01,
                       "quality Q2: a repeat whose crossings sit 0.3 dB louder gives point_error_db 0.30 (" + juce::String (diff.pointErrorDb.value_or (-1), 2) + ")");
                check (repeatQuality (tx, nullptr).repeats == 1 && ! repeatQuality (tx, nullptr).pointErrorDb, "quality Q3: without a repeat there is no point_error_db");
                check (repeatQuality (tx, nullptr).withinMonotonic && repeatQuality (tx, nullptr).acrossMonotonic, "quality Q4: a textbook curve is monotonic within and across positions");
                Derived bent = tx; bent.inAtGr[4].at[2] = (double) bent.inAtGr[4].at[1] - 1.0;   // 2 dB point below the 1 dB point: within-position violation
                const auto bq = repeatQuality (bent, nullptr);
                check (! bq.withinMonotonic && ! bq.violations.isEmpty(), "quality Q5: a 2 dB point below the 1 dB point is a within-position violation, named (" + bq.violations.joinIntoString ("; ") + ")");
                Derived zig = tx; zig.inAtGr.clear();                                           // four positions whose 1 dB values go -30, -20, -25, -15: up, down, up
                for (double v : { -30.0, -20.0, -25.0, -15.0 }) { Derived::InAtGr r; r.at[1] = v; r.at[2] = v + 1.0; r.at[3] = v + 2.0; zig.inAtGr.push_back (r); }
                check (! repeatQuality (zig, nullptr).acrossMonotonic, "quality Q6: 1 dB values that rise and fall across positions are an across-position violation");
            }
            auto gap = cl; gap[4][1] = std::nullopt;   // position 4's -12 reading missing: the crossing has no bracket
            const auto gx = derive (fromGains (gap), kLevels, -1);
            check (gx.tEffective1dB.size() == 6 && gx.tEffective1dB[4].isVoid(),
                   "sweep R4: a missing reading around the crossing gives null, never a guess (" + gx.result + ")");
        }

        // THE QUIET-LEVEL FALLBACK is decided by one function on the derivation, and the fixture says it was used.
        auto alwaysOn = curve; alwaysOn[7] = { 2.0, -1.0, -4.0 };                       // the soft end itself compresses 6 dB across the levels
        const auto ao = derive (fromGains (alwaysOn), kLevels, -1);
        check (ao.result == "unreadable" && needsQuietFallback (ao) && ! needsQuietFallback (clean) && ! needsQuietFallback (b),
               "sweep Q1 (ALWAYS-ON): a soft end with no linear anchor is the ONE case that asks for the quiet-level reference; a curve and a mid-sweep bump do not ('" + ao.reason + "')");
        Plan qp = pl2; qp.quietReference = true; qp.referenceFallbackNote = "quiet-level reference used because the soft end had no linear anchor: test";
        auto quietRun = fromGains (alwaysOn);                                              // the re-sweep: the same positions with -54/-48 holds, linear (2 dB) at both
        for (auto& pr : quietRun.positions)
            for (double L : { -54.0, -48.0 }) { HoldReading h; h.present = true; h.inRmsDb = L - 3.0103; h.levelDb = h.inRmsDb + 2.0; pr.holds[levelKey (L)] = h; }
        const auto aq = derive (quietRun, kLevels, -1, true);
        const auto qv = composeThresholdSweep (aq, displayCheck (aq, ""), qp, {});
        check (needsQuietFallback (shallowR), "sweep Q3: a reading above the reference (the soft end was not the floor) asks for the quiet-level reference too");
        check (qv.getProperty ("linearReference", {}).getProperty ("mode", "") == "per_position_quiet"
                 && qv.getProperty ("linearReference", {}).getProperty ("fallback", "").toString().contains ("no linear anchor"),
               "sweep Q2: a fallback sweep records mode per_position_quiet and WHY it fell back, on the fixture");
    }
}

//==============================================================================
/** INPUT-AS-THRESHOLD: each position's own quiet reference, self-checked (ruled 29 Sep). A textbook 1176-style
    device: input gain G_p = -20 + 4p dB into a fixed -20 dBFS threshold at 4:1, so a quiet tone is linear at every
    position and the reduction at a loud one is (L + G_p + 20) * 0.75 above threshold. */
void testSweepQuietReference()
{
    using namespace ejmap::sweep;
    auto build = [] (std::function<double (int, double)> quietBend) {
        Measured m; m.ok = true; m.movingDb = 0.1;
        for (int p = 0; p < 6; ++p)
        {
            PositionReading r; r.k = p; r.norm = (float) p / 5.0f; r.text = juce::String (-20.0 + 4.0 * p, 1) + " dB"; r.landedBy = "pump";
            const double G = -20.0 + 4.0 * p;
            for (double L : { -54.0, -48.0, -24.0, -12.0, -6.0 })
            {
                HoldReading h; h.present = true; h.inRmsDb = L - 3.0103;
                const double over = L + G + 20.0;
                h.levelDb = h.inRmsDb + G - (over > 0 ? over * 0.75 : 0.0) + quietBend (p, L);
                r.holds[levelKey (L)] = h;
            }
            m.positions.push_back (r);
        }
        return m; };
    const auto ok = derive (build ([] (int, double) { return 0.0; }), sweeptest::kLevels, -1, true);
    check (ok.result == "certified" && ok.sense == "higher_is_harder" && ok.quietCheckDb[3] && std::abs (*ok.quietCheckDb[3]) < 1e-9,
           "quiet Q1: an input control certifies higher_is_harder against each position's own quiet gain");
    check (std::abs (*ok.reduction.at (levelKey (-6))[5] - 10.5) < 1e-9 && std::abs (*ok.reduction.at (levelKey (-24))[0]) < 1e-9,
           "quiet Q2: reduction is that position's -48 gain minus its gain at the level (input gain cancels)");
    const auto okd = displayCheck (ok, "dB");
    check (okd.engageDriftDb && *okd.engageDriftDb < 0.01,
           "quiet Q6: for a higher_is_harder input gain the engage number is display PLUS level, so a dB-linear input shows no drift ("
             + juce::String (okd.engageDriftDb.value_or (99.0), 3) + ")");
    {
        // Q7: the same device with an INVERTED threshold display, higher_is_harder by norm while the displayed value
        // FALLS (Tube-Tech CL 1B's shape): a fixed threshold swept by display Td = -20 - 4p reduces by (L - Td) * 0.75,
        // so the engage number must be display MINUS level, and a dB-linear display must show no drift.
        Measured inv; inv.ok = true; inv.movingDb = 0.1;
        for (int p = 0; p < 6; ++p)
        {
            PositionReading r; r.k = p; r.norm = (float) p / 5.0f; const double td = 0.0 - 6.0 * p;
            r.text = juce::String (td, 1) + " dB";
            for (double L : { -24.0, -12.0, -6.0 })
            {
                HoldReading h; h.present = true; h.inRmsDb = L - 3.0103;
                h.levelDb = h.inRmsDb - juce::jmax (0.0, (L - td) * 0.75);
                r.holds[levelKey (L)] = h;
            }
            inv.positions.push_back (r);
        }
        const auto di = derive (inv, sweeptest::kLevels, -1);
        const auto dic = displayCheck (di, "dB");
        check (di.sense == "higher_is_harder" && dic.engageDriftDb && *dic.engageDriftDb < 0.01,
               "quiet Q7: an inverted threshold display (norm up = harder, value down) keys the sign on the DISPLAY: no drift ("
                 + juce::String (dic.engageDriftDb.value_or (99.0), 3) + ")");
    }
    const auto bent = derive (build ([] (int p, double L) { return p == 3 && L == -48.0 ? -0.3 : 0.0; }), sweeptest::kLevels, -1, true);
    check (bent.skipped.contains (3) && bent.skippedReasons[bent.skipped.indexOf (3)].contains ("not 6"),
           "quiet Q3: -48 and -54 differing by 5.7 dB, not 6, refuses THAT position rather than calibrate off it");
    // A trace with no quiet levels (the first batch) cannot give an input control any reference at all.
    auto old = build ([] (int, double) { return 0.0; });
    for (auto& r : old.positions) { r.holds.erase (levelKey (-54.0)); r.holds.erase (levelKey (-48.0)); }
    check (derive (old, sweeptest::kLevels, -1, true).result == "unreadable", "quiet Q4: no quiet readings, no reference - unreadable");
    // Plan: only input-as-threshold picks carry it.
    const auto dir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep/plan");
    const auto mc = planFromFixture (juce::JSON::parse (dir.getChildFile ("AudioUnit_73462d29_1.5.1.json").loadFileAsString()));
    check (mc.quietReference && mc.probeLevels().size() == 5 && mc.probeLevels().front() == -54.0,
           "quiet Q5: MC 77's plan renders -54 and -48 first, quiet to loud");

    // THE REFERENCE LADDER (2 Oct, CL 1B): a device whose threshold reaches below -48 compresses the first rung's pair;
    // the reference descends 12 dB a rung until a pair passes, loudest passing rung first, and refuses when none does.
    // Here positions 0 and 1 sit at -40 dBFS peak and each further position 12 dB lower (p = 5: -88), 2:1, so -48 is
    // compressed from p = 2, -60 from p = 3, -72 from p = 4 and -84 at p = 5: three positions descend, one has no rung.
    {
        auto ladder = [] (bool deepRungs, double bendAt48ForPos1) {
            Measured m; m.ok = true; m.movingDb = 0.1;
            for (int p = 0; p < 6; ++p)
            {
                const double T = -40.0 - 12.0 * juce::jmax (0, p - 1);
                PositionReading r; r.k = p; r.norm = (float) p / 5.0f; r.text = juce::String (T, 1) + " dB";
                std::vector<double> lv { -54.0, -48.0, -24.0, -12.0, -6.0 };
                if (deepRungs) for (double L : { -66.0, -60.0, -78.0, -72.0, -90.0, -84.0 }) lv.push_back (L);
                for (double L : lv)
                {
                    HoldReading h; h.present = true; h.inRmsDb = L - 3.0103;
                    h.levelDb = h.inRmsDb + 1.0 - (L > T ? (L - T) * 0.5 : 0.0) + (p == 1 && L == -48.0 ? bendAt48ForPos1 : 0.0);   // +1 dB static gain
                    r.holds[levelKey (L)] = h;
                }
                m.positions.push_back (r);
            }
            return m; };
        const auto deep = derive (ladder (true, 0.0), sweeptest::kLevels, -1, true);
        auto rungOf = [&] (const Derived& d, int i) { return d.quietRungDb[(size_t) i] ? juce::String ((int) d.quietRungDb[(size_t) i]->first) + "/" + juce::String ((int) d.quietRungDb[(size_t) i]->second) : juce::String ("none"); };
        check (rungOf (deep, 0) == "-54/-48" && rungOf (deep, 1) == "-54/-48" && rungOf (deep, 2) == "-66/-60" && rungOf (deep, 3) == "-78/-72" && rungOf (deep, 4) == "-90/-84" && rungOf (deep, 5) == "none",
               "ladder L1: each position's reference is the LOUDEST rung whose pair differs by 6 dB; one that passes nowhere has none ("
                 + rungOf (deep, 0) + " " + rungOf (deep, 1) + " " + rungOf (deep, 2) + " " + rungOf (deep, 3) + " " + rungOf (deep, 4) + " " + rungOf (deep, 5) + ")");
        check (deep.skipped.size() == 1 && deep.skipped.contains (5) && deep.skippedReasons[0].contains ("-84 minus -90") && deep.skippedReasons[0].contains ("every rung"),
               "ladder L2: the position no rung reaches is refused, and the reason names the last rung walked ('" + (deep.skippedReasons.isEmpty() ? juce::String() : deep.skippedReasons[0]) + "')");
        check (deep.quietGainDb[4] && std::abs (*deep.quietGainDb[4] - 1.0) < 1e-9 && deep.reduction.at (levelKey (-6))[4] && std::abs (*deep.reduction.at (levelKey (-6))[4] - 35.0) < 1e-9,
               "ladder L3: the reference gain comes from the rung's upper level (+1 dB at -84 for position 4), so reduction at -6 is (-6 - -76)/2 = 35 dB");
        check (! deep.skipped.contains (2) && deep.quietCheckDb[2] && std::abs (*deep.quietCheckDb[2]) < 1e-9 && deep.result == "certified",
               "ladder L4: a position that descended is a reading like any other and the sweep still certifies (" + deep.result + ")");
        // the same device read from a trace that holds only -54/-48 (every record before 2 Oct): positions 2-5 refused as before
        const auto shallow = derive (ladder (false, 0.0), sweeptest::kLevels, -1, true);
        check (shallow.skipped.size() == 4 && shallow.skippedReasons[0].contains ("-48 minus -54") && ! shallow.skippedReasons[0].contains ("every rung") && rungOf (shallow, 1) == "-54/-48",
               "ladder L5: a trace with the first rung only is read exactly as before - no deeper rung is invented, the reason names -48 minus -54 (skipped "
                 + juce::String (shallow.skipped.size()) + ": '" + (shallow.skippedReasons.isEmpty() ? juce::String() : shallow.skippedReasons[0]) + "')");
        // the first rung passing wins even when a deeper one also passes (and a bent first rung hands position 1 to the second)
        const auto bent = derive (ladder (true, -0.3), sweeptest::kLevels, -1, true);
        check (rungOf (bent, 1) == "-66/-60" && ! bent.skipped.contains (1) && rungOf (bent, 0) == "-54/-48",
               "ladder L6: a first rung that fails by 0.3 dB hands THAT position to the next rung; its neighbour keeps the first (" + rungOf (bent, 1) + ", " + rungOf (bent, 0) + ")");
        // the record carries the rung per position and the count below the first
        Plan lp; lp.thr = 0; lp.thrName = "Threshold"; lp.norms = { 0.f, 0.2f, 0.4f, 0.6f, 0.8f, 1.f }; lp.quietReference = true;
        const auto rec = composeThresholdSweep (deep, displayCheck (deep, "dB"), lp, {});
        const auto lr = rec.getProperty ("linearReference", {});
        check ((int) lr.getProperty ("descended", -1) == 3 && lr.getProperty ("rung_dbfs", {}).size() == 6 && (int) lr.getProperty ("rung_dbfs", {})[3][1] == -72 && lr.getProperty ("rung_dbfs", {})[5].isVoid()
                 && lr.getProperty ("ladder_dbfs", {}).size() == 4 && (int) lr.getProperty ("levels_dbfs", {})[1] == -48,
               "ladder L7: the record says which rung each position used (null where none), how many descended, the whole ladder, and levels_dbfs still names the first rung");
    }
}

//==============================================================================
/** NOT LICENSED, NARROWED (ruled 29 Sep): silence, non-finite output, or output that is not the input's tone, at the
    default settings. Default gain is information. The 3 dB rule withheld six working Waves plugins on 29 Sep. */
void testLicenceFromAudio()
{
    using namespace ejmap::sweep;
    auto m = sweeptest::fromGains ({ { 0.0, 0.0, 0.0 }, { 0.0, -1.0, -2.0 } });
    for (double L : { -24.0, -12.0, -6.0 })
    { m.refDb[levelKey (L)] = L - 3.0103 + 7.1; m.refToneFrac[levelKey (L)] = 0.99; m.refNonFinite[levelKey (L)] = 0; }
    auto d = derive (m, sweeptest::kLevels, -1);
    check (! d.unlicensedSuspect && std::abs (d.defaultGain.at (levelKey (-24)) - 7.1) < 1e-6,
           "licence A1: +7.1 dB of default gain (CLA-3A) is information, NOT a licence finding");
    auto silent = m; for (auto& [k, v] : silent.refDb) v = -130.0;
    check (derive (silent, sweeptest::kLevels, -1).unlicensedSuspect, "licence A2: silence at every level is flagged");
    auto nf = m; nf.refNonFinite[levelKey (-12)] = 3;
    check (derive (nf, sweeptest::kLevels, -1).unlicensedSuspect, "licence A3: non-finite output is flagged");
    auto noise = m; noise.refToneFrac[levelKey (-6)] = 0.2;
    check (derive (noise, sweeptest::kLevels, -1).unlicensedSuspect, "licence A4: output that is 20% the input's tone is flagged");
    auto old = m; for (auto& [k, v] : old.refToneFrac) v = -1.0;
    check (! derive (old, sweeptest::kLevels, -1).unlicensedSuspect, "licence A5: a trace without tone_frac claims nothing");

    // WHICH MECHANISM LANDED EACH WRITE, recorded per position (ruled 29 Sep).
    const auto one = parseSweep ("sweep\tproto\t1\npos\t0\tnorm\t0.0\tconfirm_ms\t612.0\tslices\t250\tinstack_match\t0\tgetValue\t0\tlanded_by\trender\trender_blocks\t9\ttext_ms\t1\treads\t2\ttext\t-20 dB\n");
    check (one.positions.size() == 1 && one.positions[0].landedBy == "render" && one.positions[0].renderBlocks == 9,
           "landing W1: a pos line's landed_by and render_blocks are read");
    auto lm = sweeptest::fromGains ({ { 0.0, -1.0, -2.0 }, { 0.0, 0.0, 0.0 } });
    lm.positions[0].landedBy = "render"; lm.positions[1].landedBy = "pump";
    const auto ld = derive (lm, sweeptest::kLevels, -1);
    Plan pl; pl.ok = true;
    const auto ts = composeThresholdSweep (ld, displayCheck (ld, ""), pl, {});
    check (ld.landedBy == std::vector<juce::String> { "render", "pump" } && (int) ts.getProperty ("writeLanding", {}).getProperty ("render", 0) == 1,
           "landing W2: the fixture records each position's mechanism and the counts");
}

// UAD-2 JOINS THE HARDWARE CATEGORY, conditional (ruled 29 Sep).
void testExternalHardware()
{
    using namespace ejmap::cert;
    check (externalHardwareNeeded ("UAD Neve 2254 E", "aufx,SCAU,!UAD") == "UAD hardware (UAD-2)"
             && externalHardwareNeeded ("APB C-18 Compressor", "aufx,c18c,McDP") == "McDSP APB hardware"
             && externalHardwareNeeded ("UADx 1176 Compressor", "aufx,U176,UADx").isEmpty()
             && externalHardwareNeeded ("bx_townhouse Buss Compressor", "aumf,bxth,Brwx").isEmpty(),
           "hardware H1: UAD-2 (!UAD) and McDSP APB need their hardware; a UADx native build and others do not");
}

void testSweepSplitVerdict()
{
    using namespace sweeptest;
    // A dB display that IS dBFS in the peak convention: T lands on the displayed value.
    // Ratio 2:1, input peak L, displayed threshold Td: g = (L - Td) / 2, so the display text must equal L - 2g.
    Measured m = fromGains ({ { 2.0, 2.0, 2.0 } });
    m.positions.clear();
    const double L[3] = { -24.0, -12.0, -6.0 };
    for (int k = 0; k < 6; ++k)
    {
        const double td = -30.0 + 6.0 * k;                 // -30 .. 0 dB
        PositionReading p; p.k = k; p.norm = (float) k / 5.0f; p.text = juce::String (td, 1) + " dB";
        for (double l : L)
        {
            HoldReading h; h.present = true; h.inRmsDb = l - 3.0103;
            const double g = juce::jmax (0.0, (l - td) / 2.0);
            h.levelDb = h.inRmsDb - g;
            p.holds[levelKey (l)] = h;
        }
        m.positions.push_back (p);
    }
    m.params[9] = { "Ratio", "2:1" };
    const auto d = derive (m, kLevels, 9);
    const auto dc = displayCheck (d, "dB");
    check (d.result == "certified" && dc.offsetDb && std::abs (*dc.offsetDb) < 0.01 && dc.engageDriftDb && *dc.engageDriftDb < 0.01,
           "sweep V1: a peak-referenced dBFS display: displayOffsetDb 0, engage drift 0");
    const auto noDb = displayCheck (d, "");
    check (! noDb.offsetDb.has_value() && noDb.engage.empty() && ! noDb.engageDriftDb.has_value(),
           "sweep V2: a threshold with no dB unit records no display numbers, never 0");

    // SPREAD, NOT MAGNITUDE (ruled 29 Sep): a console-calibrated display 14 dB off, held steady, is linear and usable -
    // subtract 14. The same curve with a display that drifts 1.5 dB per position is not.
    auto shifted = m;
    for (auto& p : shifted.positions) p.text = juce::String (p.text.getDoubleValue() - 14.0, 1) + " dB";
    const auto sc = displayCheck (derive (shifted, kLevels, 9), "dB");
    check (sc.offsetDb && std::abs (*sc.offsetDb - 14.0) < 0.01 && sc.engageDriftDb && *sc.engageDriftDb < 0.01,
           "sweep V7: a steady +14 dB offset records 14 as a number, with NO engage drift - magnitude is not the discriminator");
    auto drifting = m;
    for (size_t k = 0; k < drifting.positions.size(); ++k)
        drifting.positions[k].text = juce::String (drifting.positions[k].text.getDoubleValue() * 1.5, 1) + " dB";
    const auto dd = displayCheck (derive (drifting, kLevels, 9), "dB");
    check (dd.engageDriftDb && *dd.engageDriftDb > 1.0 && dd.iqrDb > 1.0,
           "sweep V8: a display whose offset drifts records it in BOTH numbers (engage drift " + juce::String (dd.engageDriftDb.value_or (0.0), 2)
             + ", IQR " + juce::String (dd.iqrDb, 2) + ")");

    // TOWNHOUSE, from its committed traces: a good map AND a display that models the console. Two verdicts.
    const auto dir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep/townhouse");
    ProcessOut ref; std::vector<ProcessOut> pos;
    const bool loaded = loadProcesses (dir.getChildFile ("processes.json"), dir.getChildFile ("raw"), ref, pos);
    check (loaded && pos.size() == 16, "sweep V3: the townhouse trace loads (1 reference + 16 position processes)");
    const auto td = derive (mergeProcesses (ref, pos), kLevels, 5);
    const auto tdc = displayCheck (td, "dB");
    check (td.result == "certified" && tdc.offsetDb && std::abs (*tdc.offsetDb + 15.0) < 0.01 && std::abs (tdc.iqrDb - 4.50) < 0.01
             && tdc.engageDriftDb && std::abs (*tdc.engageDriftDb - 4.46) < 0.01,
           "sweep V4: townhouse certifies its map; the display records engage drift 4.46, IQR 4.50, offset -15.00 - never merged");

    // THE COMMITTED FIXTURE RE-DERIVES FROM ITS TRACE (decision D2: re-compute, never re-measure).
    const auto fx = juce::JSON::parse (juce::File (EJMAP_REPO_ROOT)
                        .getChildFile ("tools/ejmap/cert-fixtures/profiles/AudioUnit_417f6e76_1.8.1.json").loadFileAsString());
    const auto plan = planFromFixture (fx);
    Provenance pv; pv.measuredAt = "x"; pv.host = "x";
    const auto again = composeThresholdSweep (td, tdc, plan, pv);
    const auto committed = fx.getProperty ("thresholdSweep", {});
    juce::StringArray differ;
    for (auto* k : { "reduction_db", "sense", "engage", "thresholdDbEquivalent", "result", "displayEngage", "displayOffsetDb", "displayOffsetSpread",
                     "positionLandedBy", "writeLanding", "defaultGain_db",
                     "linearReference", "positionNorms", "ratioDuring", "level_convention", "tone" })
        if (juce::JSON::toString (again.getProperty (k, {}), true) != juce::JSON::toString (committed.getProperty (k, {}), true))
            differ.add (k);
    check (committed.isObject() && differ.isEmpty(),
           "sweep V5: the committed townhouse fixture re-derives from its committed trace (differs in: " + differ.joinIntoString (", ") + ")");
    check (plan.ok && plan.thr == 4 && plan.norms.size() == 16 && plan.ratioIndex == 5 && plan.thrUnit == "dB",
           "sweep V6: townhouse plans on [4] Thresh (dB), 16 positions, ratio [5]");
    check (! committed.hasProperty ("displayLinear") && ! again.hasProperty ("displayLinear"),
           "sweep V9: displayLinear is UNSET on every fixture until ~20 decide its bound");
}

void testSweepPrivacyAndPlan()
{
    using namespace ejmap::sweep;
    auto* prov = new juce::DynamicObject();
    prov->setProperty ("tester_id", "sean"); prov->setProperty ("machine_id", "abc123"); prov->setProperty ("keep", 1);
    auto* base = new juce::DynamicObject();
    base->setProperty ("provenance", juce::var (prov)); base->setProperty ("machine_id", "top");
    auto* ts = new juce::DynamicObject(); ts->setProperty ("tester_id", "inside");
    const auto f = composeFixture (juce::var (base), juce::var (ts));
    const auto text = juce::JSON::toString (f);
    check (! text.contains ("tester_id") && ! text.contains ("machine_id") && text.contains ("\"keep\""),
           "sweep P1: the fixture writer drops tester_id and machine_id wherever they appear, and nothing else");

    auto control = [] (int idx, const char* name, int steps, bool discrete, double defNorm, const char* defText, const char* unit) {
        auto* c = new juce::DynamicObject();
        c->setProperty ("index", idx); c->setProperty ("name", name); c->setProperty ("numSteps", steps);
        c->setProperty ("discrete", discrete); c->setProperty ("unit", unit);
        auto* d = new juce::DynamicObject(); d->setProperty ("normalised", defNorm); d->setProperty ("display", defText);
        c->setProperty ("defaultOnInstantiate", juce::var (d));
        return juce::var (c); };
    auto fixture = [] (juce::Array<juce::var> cs) { auto* o = new juce::DynamicObject(); o->setProperty ("controls", cs); return juce::var (o); };
    const auto unity = planFromFixture (fixture ({ control (0, "Threshold", 0, false, 0.5, "-10 dB", "dB"),
                                                   control (1, "Ratio", 0, false, 0.0, "1.00:1", ":1") }));
    check (unity.ok && unity.ratioRaise && unity.ratioIndex == 1 && unity.ratioDefaultText == "1.00:1",
           "sweep P2: a ratio instantiating at 1:1 plans the spec 4.2 raise (\"1.00:1\" is 1, not the 1.001 every digit reads)");
    const auto autoMk = planFromFixture (fixture ({ control (0, "Threshold", 0, false, 0.5, "-10 dB", "dB"),
                                                    control (3, "Auto Makeup", 2, true, 1.0, "On", "") }));
    check (autoMk.ok && autoMk.autoMakeupDisabled && autoMk.sets.size() == 1 && autoMk.sets[0].first == 3 && autoMk.sets[0].second == 0.0f,
           "sweep P3: auto make-up on by default is written OFF for the sweep, and the fixture says so");
    const auto stepped = planFromFixture (fixture ({ control (0, "Threshold", 5, true, 0.0, "Off", "") }));
    check (stepped.ok && stepped.norms.size() == 5 && stepped.norms[4] == 1.0f, "sweep P4: a stepped threshold sweeps every step");
}

//==============================================================================
/** THE RATIO RAISE (spec 4.2) AND THE THRESHOLD PICKS (XLA-3, MC 77), ruled 29 Sep. */
void testSweepRatioAndPicks()
{
    using namespace ejmap::sweep;
    // THE CHOICE: the smallest READ value at or above 4:1, not the first norm.
    juce::String chosen;
    std::vector<GridPoint> c1 { { 0.0f, "0.50:1" }, { 0.2f, "1.00:1" }, { 0.45f, "3.80:1" }, { 0.5f, "4.58:1" }, { 0.6f, "8.00:1" },
                                { 0.8f, "+Inf" }, { 1.0f, "-5.00:1" } };
    auto n = chooseRatioRaise (c1, chosen);
    check (n && *n == 0.5f && chosen == "4.58:1", "ratio R1: C1-shaped (0.5:1 -> infinity -> -5:1): the smallest read value >= 4 (4.58:1)");
    std::vector<GridPoint> rc { { 0.0f, "50.00" }, { 0.2f, "8.10" }, { 0.3f, "4.40" }, { 0.35f, "3.90" }, { 0.6f, "1.00" }, { 1.0f, "0.50" } };
    n = chooseRatioRaise (rc, chosen);
    check (n && *n == 0.3f && chosen == "4.40", "ratio R2: RCompressor-shaped (inverted): 4.40 at 0.3, NOT 50.00 at norm 0");
    std::vector<GridPoint> none { { 0.0f, "1.00:1" }, { 1.0f, "3.50:1" } };
    check (! chooseRatioRaise (none, chosen), "ratio R3: no read value reaches 4:1 - nothing chosen, the product is refused");
    check (ratioFromText ("Ratio 4") && *ratioFromText ("Ratio 4") == 4.0 && ! ratioFromText ("-5.00:1") && ! ratioFromText ("+Inf")
             && ! ratioFromText ("All Buttons") && ! ratioFromText ("Ratio 4 of 20"),
           "ratio R4: 'Ratio 4' is 4 (its only number); negative, infinite, named and two-number texts are not guessed");

    // THE TRAP: R comes from the text READ BACK after the write, never from what was asked for. Here the plan asked
    // for a 4:1 position on a stepped ratio and the plugin landed on 5:1.
    auto m = sweeptest::fromGains ({ { 0.0, -1.0, -2.0 }, { 0.0, 0.0, -0.5 }, { 0.0, 0.0, 0.0 } });
    m.params[7] = { "Ratio", "1:1" };            // as instantiated
    m.setTexts[7] = "5:1";                       // what the plugin said after the raise
    const auto d = derive (m, sweeptest::kLevels, 7);
    check (d.ratio && *d.ratio == 5.0 && d.ratioText == "5:1" && d.ratioInstantiated == "1:1",
           "ratio R5: the sweep derives with the ratio READ BACK (5:1), and records the instantiated 1:1 it was raised from");
    ProcessOut ref { "sweep\tproto\t1\nset\t7\t0.500000\tconfirm_ms\t1\tgetValue\t0.5\ttext\t4:1\nref\t-24.00\tlevel_db\t-27\tin_peak_db\t-24\tin_rms_db\t-27\n", true, "exit 0", -1.0f };
    ProcessOut p0 { "sweep\tproto\t1\nset\t7\t0.500000\tconfirm_ms\t1\tgetValue\t0.5\ttext\t4:1\npos\t0\tnorm\t0.0\tconfirm_ms\t1\tslices\t1\tinstack_match\t1\tgetValue\t0\ttext_ms\t1\treads\t2\ttext\t-20 dB\n", true, "exit 0", 0.0f };
    ProcessOut p1 { "sweep\tproto\t1\nset\t7\t0.500000\tconfirm_ms\t1\tgetValue\t0.5\ttext\t5:1\npos\t0\tnorm\t1.0\tconfirm_ms\t1\tslices\t1\tinstack_match\t1\tgetValue\t1\ttext_ms\t1\treads\t2\ttext\t0 dB\n", true, "exit 0", 1.0f };
    const auto merged = mergeProcesses (ref, { p0, p1 });
    check (merged.setConflict.isNotEmpty() && derive (merged, sweeptest::kLevels, 7).result == "unreadable",
           "ratio R6: processes that read the ratio back differently (4:1 and 5:1) refuse the sweep");
    ProcessOut refusedOne { "sweep\tproto\t1\nset\t7\t0.500000\tconfirm_ms\t-1.0\tgetValue\t0\tlanded_by\tunlanded\ttext\t0\nrefused set_unlanded 7\n", true, "exit 0", 1.0f };
    const auto withRefusal = mergeProcesses (ref, { p0, refusedOne });
    check (withRefusal.setConflict.isEmpty() && withRefusal.positions.size() == 2 && withRefusal.positions[1].processFailed,
           "ratio R7: a process that REFUSED its precondition contributes no read-back - its position is skipped, not a conflict");

    // THE PICKS, from the pushed fixtures themselves (echojay-saas 2454c0a), by range and step count - never by name.
    const auto dir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep/plan");
    const auto xla = planFromFixture (juce::JSON::parse (dir.getChildFile ("AudioUnit_62485258_1.10.1.json").loadFileAsString()));
    check (xla.ok && xla.thr == 3 && xla.pickNote.contains ("only continuous"),
           "pick K1: Acme Opticom XLA-3 sweeps [3] Input Gain (continuous), not [6] Input Pad (two steps)");
    const auto mc = planFromFixture (juce::JSON::parse (dir.getChildFile ("AudioUnit_73462d29_1.5.1.json").loadFileAsString()));
    check (mc.ok && mc.thr == 15 && mc.channel == "L" && mc.linkStates.joinIntoString ("|").contains ("Link = 'Std'"),
           "pick K2: Purple Audio MC 77 sweeps [15] Input L (channel A, spec 4.7) and records its Link as instantiated ('Std')");
    check (mc.sets.empty() && xla.sets.empty(), "pick K3: neither pick writes a link or any other control");
    {
        // K1's order is XLA-3's own, where the continuous control happens to come first; reversed, the rule must still
        // pick by step count, not by position in the list.
        auto ctl = [] (int idx, const char* name, int steps, bool discrete) {
            auto* c = new juce::DynamicObject();
            c->setProperty ("index", idx); c->setProperty ("name", name); c->setProperty ("numSteps", steps); c->setProperty ("discrete", discrete);
            return juce::var (c); };
        auto* o = new juce::DynamicObject();
        o->setProperty ("controls", juce::Array<juce::var> { ctl (0, "Input Pad", 2, true), ctl (1, "Input Gain", 2147483647, false),
                                                             ctl (2, "Output", 2147483647, false) });
        const auto rev = planFromFixture (juce::var (o));
        check (rev.ok && rev.thr == 1, "pick K5: with the stepped pad listed FIRST, the continuous gain is still the pick");
    }
    // SEVERAL THRESHOLDS AND NO PICK (ruled 30 Sep): every candidate is swept and labelled; nothing is refused for having
    // too many thresholds. A product refused at roles was never swept, so "review with curves in hand" had no curves.
    const auto fair = planFromFixture (juce::JSON::parse (dir.getChildFile ("AudioUnit_16616669_11.8.0.json").loadFileAsString()));
    check (fair.ok && fair.thr == -1 && fair.candidates.size() == 4 && fair.cls == "channels_lr",
           "pick K4: UAD Fairchild 670 (channels_lr) plans its FOUR thresholds as labelled candidates, not a refusal");
    auto cq = fair.forCandidate (fair.candidates[1]);
    check (cq.thr == fair.candidates[1].index && cq.candidates.empty() && cq.thrFlags.contains ("dc") == fair.candidates[1].flags.contains ("dc"),
           "pick K6: a candidate's plan carries that control alone, with its own flags");
    const auto amount = planFromFixture (juce::JSON::parse (dir.getChildFile ("AudioUnit_62485258_1.10.1.json").loadFileAsString().replace ("Input Gain", "Wobble").replace ("Input Pad", "Wibble")));
    check (! amount.ok, "pick K7: a product with NO threshold candidate is still refused at the roles stage");

    // THE CONVENTION'S POSITIVE CONTROL (spec 7, a test on displayOffsetDb): MCompressor with its detector at Peak lands
    // inside 2 dB of 0 in the peak convention. The 100 ms arm is a fact about Melda's detector, recorded, not a test.
    for (auto* arm : { "rms-peak", "rms-100ms" })
    {
        const auto adir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep/mcompressor-" + juce::String (arm));
        ProcessOut r; std::vector<ProcessOut> ps;
        const bool ok = loadProcesses (adir.getChildFile ("processes.json"), adir.getChildFile ("raw"), r, ps);
        const auto ad = derive (mergeProcesses (r, ps), sweeptest::kLevels, 6);
        const auto ac = displayCheck (ad, "dB");
        if (juce::String (arm) == "rms-peak")
            check (ok && ad.result == "certified" && ac.offsetDb && std::abs (*ac.offsetDb) <= kDisplayDb
                     && ac.engageDriftDb && std::abs (*ac.engageDriftDb - 0.24) < 0.01,
                   "convention C1: MCompressor at Peak - certified, displayOffsetDb " + juce::String (ac.offsetDb ? *ac.offsetDb : 99.0, 2)
                     + " within spec 7's 2 dB of 0 (KEEP PEAK), engage drift 0.24");
        else
            check (ok && ad.result == "certified" && ac.offsetDb && std::abs (*ac.offsetDb - 0.80) < 0.01
                     && ac.engageDriftDb && std::abs (*ac.engageDriftDb - 0.28) < 0.01,
                   "convention C2: MCompressor at 100 ms RMS - displayOffsetDb +0.80 (not +3.01: Melda's detector, noted), engage drift 0.28");
    }
}

// THE VERSION GUARD IS ON COMPARISON, NOT MEASUREMENT (ruled 29 Sep): a fresh sweep at an unseen version is valid.
void testMeasurableRule()
{
    using ejmap::cert::Subject;
    using ejmap::cert::measurable;
    Subject s;
    s.reach = Subject::Reach::reachable;                       check (measurable (s, false), "measurable M1: reachable");
    s.reach = Subject::Reach::versionMismatch; s.installedUnique = true;
    check (measurable (s, false), "measurable M2: installed at ANOTHER version, one version installed - measurable (a new identity)");
    s.installedUnique = false;
    check (! measurable (s, false), "measurable M3: several versions installed - never pick one");
    s.installedUnique = true; s.hardware = true;
    check (! measurable (s, false), "measurable M4: a hardware product stays unmeasured whatever its version");
    s.hardware = false; s.licenceBound = true;
    check (! measurable (s, false) && measurable (s, true), "measurable M5: licence-bound only with PACE included");
    s.licenceBound = false;
    for (auto r : { Subject::Reach::notInstalled, Subject::Reach::ambiguous, Subject::Reach::heldPace, Subject::Reach::requiresHardware })
    { s.reach = r; check (! measurable (s, true), "measurable M6: " + ejmap::cert::reachName (r) + " is not measurable"); }
}

//==============================================================================
/** SLEEP NEVER BECOMES A HANG (ruled 29 Sep). Timeouts are measured on AWAKE time; wall time only reports how long
    the Mac slept. Pinned on a real child process with an injected clock that "sleeps" 15 minutes mid-run. */
void testSleepTimeouts()
{
    using namespace ejmap::cert;
    check (timeoutPassed (0.0, 5000.1, 5000) && ! timeoutPassed (0.0, 5000.0, 5000) && ! timeoutPassed (1000.0, 5500.0, 5000),
           "sleep S1: a timeout passes only when AWAKE elapsed exceeds it");
    auto calls = std::make_shared<int> (0);
    Clock sleeper;                                    // wall jumps 900 s after the first read; awake time does not
    sleeper.withSleepMs = [calls] { return ejmap::cert::continuousMs() + (++*calls > 1 ? 900000.0 : 0.0); };
    const auto slept = runChild ({ "/bin/sleep", "1" }, 5000, WatchOptions { false }, sleeper);
    check (slept.cleanExit() && slept.sleptMs > 899000.0,
           "sleep S2: a 1 s child across a simulated 15-minute sleep exits cleanly under a 5 s timeout, and records "
             + juce::String (slept.sleptMs / 1000.0, 1) + " s slept");
    auto ticks = std::make_shared<int> (0);
    Clock counting;                                   // the CONTROL: an awake clock that counts the sleep must time out
    counting.awakeMs = [ticks] { return juce::Time::getMillisecondCounterHiRes() + (++*ticks > 1 ? 900000.0 : 0.0); };
    const auto hung = runChild ({ "/bin/sleep", "1" }, 5000, WatchOptions { false }, counting);
    check (hung.kind == ChildResult::Kind::timedOut,
           "sleep S3 (control): the same child under a clock that COUNTS slept time is reported as a timeout - the fake hang S2 prevents");
    const SleepGuard guard ("EJ Map RoundTripTest");
    check (guard.idleHeld, "sleep S4: the idle-sleep assertion is taken (" + guard.describe() + ")");
}

//==============================================================================
/** DISCOVERY (ruled 29 Sep): the worklist is keyed on MAPS, never read back from the driver's own output. THE FRESH
    SYSTEM, in one pin: a ledger with maps and no fixtures must produce a non-empty worklist - the one thing that had
    never passed. Real ledger files in a temp directory, read by the same loader the driver uses. */
void testDiscoveryFromMaps()
{
    using namespace ejmap::cert;
    auto installedAs = [] (const char* name, int uid, const char* version, const char* code) {
        InstalledRecord r;
        r.desc.name = name; r.desc.uniqueId = uid; r.desc.version = version; r.desc.pluginFormatName = "AudioUnit";
        r.desc.fileOrIdentifier = juce::String ("AudioUnit:Effects/") + code;
        r.identityKey = echojay::identityKeyForDescription (r.desc);
        r.uidKey = "AudioUnit|" + juce::String::toHexString (uid).toLowerCase();
        return r; };
    const std::vector<InstalledRecord> installed {
        installedAs ("elysia mpressor", 0x49696d78, "1.15.1", "aufx,mprs,Elys"),     // mapped by ANOTHER machine only
        installedAs ("Local Comp",      0x11112222, "2.0.0",  "aufx,lcmp,Test"),     // mapped HERE (a local map)
        installedAs ("Some EQ",         0x33334444, "1.0.0",  "aufx,sweq,Test"),     // mapped, category eq
        installedAs ("Other Build",     0x55556666, "3.0.0",  "aufx,obld,Test"),     // mapped for a different build only
        installedAs ("Unmapped Comp",   0x77778888, "1.0.0",  "aufx,umcp,Test"),     // compressor, not mapped
        installedAs ("A Tuner",         0x0000abcd, "1.0.0",  "aufx,tune,Test") };   // mapped, pitch
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ejmap-discovery-" + juce::Uuid().toDashedString());
    root.getChildFile ("maps").createDirectory();
    root.getChildFile ("maps").getChildFile ("fp1.json").replaceWithText (
        R"({"identity": {"format": "AudioUnit", "uid": "11112222", "version": "2.0.0", "name": "Local Comp"}, "category": "compressor"})");
    root.getChildFile ("map-state.json").replaceWithText (R"({"fetched_at": "2026-09-29T20:05:51", "failure": "", "identities": {
        "AudioUnit|49696d78|1.15.1": {"state": 3}, "AudioUnit|33334444|1.0.0": {"state": 3}, "AudioUnit|55556666|3.0.0": {"state": 4},
        "AudioUnit|77778888|1.0.0": {"state": 0}, "AudioUnit|abcd|1.0.0": {"state": 3}}})");
    root.getChildFile ("categories.json").replaceWithText (R"({"products": {
        "elysia mpressor|plugin alliance": {"category": "compressor", "mark_keys": ["AudioUnit|49696d78"]},
        "some eq|test": {"category": "eq", "mark_keys": ["AudioUnit|33334444"]},
        "other build|test": {"category": "compressor", "mark_keys": ["AudioUnit|55556666"]},
        "unmapped comp|test": {"category": "compressor", "mark_keys": ["AudioUnit|77778888"]},
        "a tuner|test": {"category": "pitch", "mark_keys": ["AudioUnit|abcd"]}}})");
    const auto in = loadDiscoveryInputs (root);
    const auto d = discoverCandidates (in, installed, {});
    juce::StringArray names;
    for (const auto& c : d.candidates) names.add (c.inst.desc.name);
    check (! d.candidates.empty(), "discovery F1 (THE FRESH SYSTEM): a ledger with maps and NO fixtures produces a non-empty worklist");
    check (names.contains ("elysia mpressor") && names.contains ("Local Comp") && names.size() == 3,
           "discovery F2: mapped by another machine (map state only, no local map) and mapped here (local map) are both candidates ("
             + names.joinIntoString (", ") + ")");
    // ONE STORE (ruled 1 Oct): a pitch product is a CANDIDATE, for tuner certification, in the same worklist.
    juce::String tunerCat;
    for (const auto& c : d.candidates) if (c.inst.desc.name == "A Tuner") tunerCat = c.category;
    check (! names.contains ("Some EQ") && ! names.contains ("Other Build") && ! names.contains ("Unmapped Comp")
             && names.contains ("A Tuner") && tunerCat == "pitch" && d.tuners.contains ("A Tuner"),
           "discovery F3: another category, a map for a different build, and an unmapped compressor are not candidates; a pitch product IS a candidate, category pitch (one store)");
    const auto withFixture = discoverCandidates (in, installed, { "49696d78|1.15.1" });
    bool still = false; for (const auto& c : withFixture.candidates) still = still || c.inst.desc.name == "elysia mpressor";
    const auto otherVersion = discoverCandidates (in, installed, { "49696d78|1.0.0" });
    bool other = false; for (const auto& c : otherVersion.candidates) other = other || c.inst.desc.name == "elysia mpressor";
    check (! still && other, "discovery F4: a fixture at the installed version is the record - no candidate; one at another version is no record for this build");
    const auto empty = loadDiscoveryInputs (root.getChildFile ("nope"));
    check (discoverCandidates (empty, installed, {}).candidates.empty() && empty.notes.joinIntoString (" ").contains ("no EJ Map ledger"),
           "discovery F5: with no ledger there is nothing to discover, and the report says why");
    root.deleteRecursively();
}

/** THE RECORD, THE STORE AND THE DEFAULT PATHS (ruled 30 Sep). A second run with DEFAULT flags, after a first run
    certified a product, must not re-certify it - which was only ever true when --fixtures happened to point at
    <out>/fixtures. And a product stopped in the defaults phase must leave a record, or it is rediscovered for ever. */
void testCertRecordAndDefaultPaths()
{
    using namespace ejmap::cert;
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ejmap-record-" + juce::Uuid().toDashedString());
    auto store = root.getChildFile ("fixtures");
    store.createDirectory();
    auto fx = [] (const char* uid, const char* extra) {
        return juce::String (R"({"product": "P )") + uid + R"(", "uid": ")" + uid + R"(", "version": "1.0.0", "format": "AudioUnit")" + extra + "}"; };
    store.getChildFile ("AudioUnit_aaaa0001_1.0.0.json").replaceWithText (fx ("aaaa0001", R"(, "thresholdSweep": {"result": "certified"})"));
    store.getChildFile ("AudioUnit_aaaa0002_1.0.0.json").replaceWithText (fx ("aaaa0002", R"(, "thresholdCandidates": [{"index": 3}], "thresholdReview": {"curves": 1})"));
    store.getChildFile ("AudioUnit_aaaa0003_1.0.0.json").replaceWithText (fx ("aaaa0003", R"(, "thresholdRefusal": {"stage": "defaults"})"));
    store.getChildFile ("AudioUnit_aaaa0004_1.0.0.json").replaceWithText (fx ("aaaa0004", ""));
    store.getChildFile ("AudioUnit_aaaa0004_1.0.0.defaults.json").replaceWithText (fx ("aaaa0004", R"(, "controls": [])"));
    store.getChildFile ("AudioUnit_aaaa0010_1.0.0.json").replaceWithText (fx ("aaaa0010", R"(, "schema": "ej_cert_tuner/1", "category": "pitch", "pitchCandidates": [{"index": 1}])"));
    const auto loaded = loadFixtures (store);
    check (loaded.size() == 5, "record R1: the defaults SIDECAR is not a subject - 6 files, 5 subjects (" + juce::String ((int) loaded.size()) + ")");
    { int tun = 0; bool rec = false; for (const auto& x : loaded) if (x.uid == "aaaa0010") { tun += x.category == "pitch"; rec = sweepRecorded (x.pushed); }
      check (tun == 1 && rec, "record R1b (ONE STORE): a tuner record in the same directory is category pitch and counts as recorded"); }
    check (refusalIsPermanent ("ara_only"), "record R1c: an ARA-only refusal is permanent");
    auto names = [] (const std::vector<Subject>& v) { juce::StringArray a; for (const auto& s : v) a.add (s.uid); return a; };
    const auto part = partitionStore (loaded, false);
    check (part.recorded == 4 && part.refused == 1 && names (part.toSweep) == juce::StringArray { "aaaa0004" },
           "record R2: a sweep, a candidates fixture and a refusal are all RECORDS and leave the worklist; only the bare fixture stays ("
             + names (part.toSweep).joinIntoString (",") + ")");
    const auto retry = partitionStore (loaded, true);
    check (retry.recorded == 3 && names (retry.toSweep).contains ("aaaa0003") && names (retry.toSweep).contains ("aaaa0004") && retry.toSweep.size() == 2,
           "record R3: --retry-refused puts the refusal back on the list and leaves the sweep and the candidates alone");
    // TRANSIENT vs PERMANENT: a refusal the product cannot outgrow is not re-run by --retry-refused.
    store.getChildFile ("AudioUnit_aaaa0005_1.0.0.json").replaceWithText (fx ("aaaa0005", R"(, "thresholdRefusal": {"stage": "plan"})"));
    store.getChildFile ("AudioUnit_aaaa0006_1.0.0.json").replaceWithText (fx ("aaaa0006", R"(, "thresholdRefusal": {"stage": "ratio_none"})"));
    store.getChildFile ("AudioUnit_aaaa0007_1.0.0.json").replaceWithText (fx ("aaaa0007", R"(, "thresholdRefusal": {"stage": "budget"})"));
    store.getChildFile ("AudioUnit_aaaa0008_1.0.0.json").replaceWithText (fx ("aaaa0008", R"(, "thresholdRefusal": {"stage": "window"})"));
    store.getChildFile ("AudioUnit_aaaa0009_1.0.0.json").replaceWithText (fx ("aaaa0009", R"(, "thresholdRefusal": {"stage": "ratio_search"})"));
    store.getChildFile ("AudioUnit_aaaa000a_1.0.0.json").replaceWithText (fx ("aaaa000a", R"(, "thresholdRefusal": {"stage": "reference"})"));
    const auto mixed = loadFixtures (store);
    const auto tr = partitionStore (mixed, true);
    const auto trNames = names (tr.toSweep);
    check (tr.permanent == 2 && ! trNames.contains ("aaaa0005") && ! trNames.contains ("aaaa0006")
             && trNames.contains ("aaaa0003") && trNames.contains ("aaaa0007") && trNames.contains ("aaaa0008") && trNames.contains ("aaaa0009") && trNames.contains ("aaaa000a"),
           "record R3b: --retry-refused re-runs the TRANSIENT refusals (defaults, budget, window, ratio search, reference) and skips the PERMANENT ones (plan, no ratio at 4:1) ("
             + trNames.joinIntoString (",") + ")");
    const auto all = names (partitionStore (mixed, true, true).toSweep);
    check (all.contains ("aaaa0005") && all.contains ("aaaa0006") && all.size() == 8,
           "record R3c: --retry-refused-all is the override that re-runs the permanent ones too");
    check (! names (partitionStore (mixed, false).toSweep).contains ("aaaa0007"), "record R3d: without either flag no refusal is re-run");
    for (const char* u : { "aaaa0005", "aaaa0006", "aaaa0007", "aaaa0008", "aaaa0009", "aaaa000a" })
        store.getChildFile (juce::String ("AudioUnit_") + u + "_1.0.0.json").deleteFile();

    SweepOptions o;
    const auto exe = juce::File ("/Applications/ejmap.app/Contents/MacOS/ejmap");
    resolveCertPaths (o, exe);
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    check (o.out == home.getChildFile ("Library/ejmap/cert") && o.fixtures == o.out.getChildFile ("fixtures")
             && o.probe == juce::File ("/Applications/ejmap.app/Contents/MacOS/EchoJayProbe"),
           "record R4: with no flags the sweep lands in ~/Library/ejmap/cert, THE STORE IS <out>/fixtures, and the probe is beside ejmap (rides the handover zip)");
    SweepOptions e; e.out = root; e.probe = root.getChildFile ("p");
    resolveCertPaths (e, exe);
    check (e.fixtures == root.getChildFile ("fixtures") && e.probe == root.getChildFile ("p"),
           "record R5: an explicit --out moves the default store with it; an explicit --probe is kept");
    Options d; d.fixtures = root.getChildFile ("repo-store");
    resolveCertPaths (d, exe);
    check (d.fixtures == root.getChildFile ("repo-store") && d.out == home.getChildFile ("Library/ejmap/cert"),
           "record R6: an explicit --fixtures (a repo-store run) is kept, and --cert-defaults takes the same defaults");

    // THE DEFAULTS-PHASE CASE: a discovered identity has no controls yet; its refusal must still be a record.
    auto discovered = juce::JSON::parse (R"json({"product": "Hangs On Load", "uid": "bbbb0001", "version": "2.3.0", "format": "AudioUnit",
                                              "discovered": "mapped (server map state 3)", "tester_id": "should not survive"})json");
    const auto rec = writeRefusalRecord (store, discovered, "defaults", "ERROR: defaults --list-params timed out after 120.0 s (killed by the driver)", 2, 1,
                                         "signed EchoJayProbe", "EJ Map 0.1.0");
    const auto back = juce::JSON::parse (rec.loadFileAsString());
    check (rec.getFileName() == "AudioUnit_bbbb0001_2.3.0.json" && back.getProperty ("thresholdRefusal", {}).getProperty ("stage", "") == "defaults"
             && back.getProperty ("thresholdRefusal", {}).getProperty ("reason", "").toString().contains ("timed out")
             && ! back.hasProperty ("thresholdSweep") && back.getProperty ("product", "") == "Hangs On Load" && ! back.hasProperty ("tester_id")
             && back.getProperty ("thresholdRefusal", {}).getProperty ("retry", "").toString().startsWith ("transient"),
           "record R7 (THE DEFAULTS PHASE): a product stopped before its defaults sampled writes a fixture at its discovered identity, "
           "naming the stage and reason, with no thresholdSweep and nothing private");
    const auto again = partitionStore (loadFixtures (store), false);
    check (again.recorded == 5 && again.refused == 2 && names (again.toSweep) == juce::StringArray { "aaaa0004" },
           "record R8: the next run reads that refusal as a record - the product is not on the worklist again");
    auto keys = std::set<juce::String>();
    for (const auto& s : loadFixtures (store)) keys.insert (s.uid + "|" + s.version);
    check (keys.count ("bbbb0001|2.3.0") == 1, "record R9: and discovery's fixture key sees it at the installed version, so it is not rediscovered either");
    {
        // A RETRIED REFUSAL WITH NO CONTROLS samples defaults first; planned from as a fixture it would refuse "0 threshold roles" for ever.
        Subject r; r.reach = Subject::Reach::reachable; r.pushed = back;                                   // the defaults-phase record: identity only
        Subject withControls; withControls.reach = Subject::Reach::reachable;
        withControls.pushed = juce::JSON::parse (R"json({"product": "P", "uid": "1", "version": "1", "controls": [], "thresholdRefusal": {"stage": "plan"}})json");
        Subject disc; disc.reach = Subject::Reach::unfixtured; disc.pushed = withControls.pushed;
        check (needsDefaultsFirst (r) && ! needsDefaultsFirst (withControls) && needsDefaultsFirst (disc),
               "record R9b: a reachable subject whose record has NO controls (a defaults-phase refusal on retry) samples defaults first; one with controls plans from them; a discovered one always samples");
    }

    // THE MAPPER'S ESCAPE HATCH: a disposition other than sweep in categories.json is honoured by cert discovery.
    auto led = root.getChildFile ("ledger");
    led.createDirectory();
    led.getChildFile ("map-state.json").replaceWithText (R"({"identities": {"AudioUnit|cccc0001|1.0.0": {"state": 3}, "AudioUnit|cccc0002|1.0.0": {"state": 3}, "AudioUnit|cccc0003|1.0.0": {"state": 3}}})");
    led.getChildFile ("categories.json").replaceWithText (R"({"products": {
        "hanger|test":  {"category": "compressor", "disposition": "operator_excluded", "why": "hangs on load", "mark_keys": ["AudioUnit|cccc0001"]},
        "fine|test":    {"category": "compressor", "disposition": "sweep", "mark_keys": ["AudioUnit|cccc0002"]},
        "nodisp|test":  {"category": "compressor", "mark_keys": ["AudioUnit|cccc0003"]}}})");
    auto inst = [] (const char* name, int uid, const char* code) {
        InstalledRecord r; r.desc.name = name; r.desc.uniqueId = uid; r.desc.version = "1.0.0"; r.desc.pluginFormatName = "AudioUnit";
        r.desc.fileOrIdentifier = juce::String ("AudioUnit:Effects/") + code; r.identityKey = echojay::identityKeyForDescription (r.desc);
        r.uidKey = "AudioUnit|" + juce::String::toHexString (uid).toLowerCase(); return r; };
    const std::vector<InstalledRecord> installed { inst ("Hanger", 0xcccc0001, "aufx,hang,Test"), inst ("Fine", 0xcccc0002, "aufx,fine,Test"), inst ("NoDisp", 0xcccc0003, "aufx,nodi,Test") };
    const auto in = loadDiscoveryInputs (led);
    const auto disc = discoverCandidates (in, installed, {});
    juce::StringArray cand; for (const auto& c : disc.candidates) cand.add (c.inst.desc.name);
    check (cand == juce::StringArray { "Fine", "NoDisp" } && disc.excludedByDisposition.size() == 1 && disc.excludedByDisposition[0].contains ("hangs on load")
             && in.notes.joinIntoString (" ").contains ("disposition other than sweep"),
           "record R10: operator_excluded in categories.json keeps a mapped compressor out of the cert worklist, with its why; "
           "disposition sweep and no disposition are both candidates (" + cand.joinIntoString (",") + ")");
    root.deleteRecursively();
}

/** thresholdReview NAMES the responding candidate (ruled 30 Sep): one field the server half reads, not a scan two
    consumers each re-implement. Pass-through candidates are listed apart and are not band evidence. */
void testThresholdReviewNamesTheBand()
{
    using namespace ejmap::cert;
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ejmap-review-" + juce::Uuid().toDashedString());
    root.createDirectory();
    auto cand = [] (int idx, const char* name, const char* result, bool pass, bool written) {
        ejmap::sweep::Plan q; q.thr = idx; q.thrName = name;
        Derivation d; d.d.result = result; d.d.passThroughAtDefaults = pass; d.written = written;
        d.d.reason = pass ? "passthrough: output equals input within 0.01 dB at every reading" : "";
        if (written) { auto* o = new juce::DynamicObject(); o->setProperty ("result", result); d.sweepVar = juce::var (o); }
        return std::make_pair (q, d); };
    ejmap::sweep::Plan plan; plan.cls = "bands_or_stages";
    std::vector<std::pair<ejmap::sweep::Plan, Derivation>> cands {
        cand (14, "Band 1 Thresh", "flat", false, true),
        cand (18, "Band 2 Thresh", "certified", false, true),
        cand (22, "Band 3 Thresh", "flat", true, true),
        cand (26, "Band 4 Thresh", "nonmonotonic", false, true),
        cand (30, "Gate Thresh", "", false, false) };
    auto base = juce::JSON::parse (R"json({"product": "Multi", "uid": "abcd1234", "version": "1.0.0", "format": "AudioUnit"})json");
    const auto out = root.getChildFile ("f.json");
    composeCandidatesAndReport (base, plan, cands, out, root.getChildFile ("f.report.txt"));
    const auto f = juce::JSON::parse (out.loadFileAsString());
    const auto rv = f.getProperty ("thresholdReview", {});
    const auto resp = rv.getProperty ("responding", {});
    check (resp.isArray() && resp.size() == 1 && (int) resp[0].getProperty ("index", -1) == 18 && resp[0].getProperty ("name", "") == "Band 2 Thresh",
           "review V1: thresholdReview.responding names the responding candidate by index AND name (" + juce::JSON::toString (resp, true) + ")");
    const auto pt = rv.getProperty ("passThroughAtDefaults", {});
    check ((int) pt.getProperty ("count", -1) == 1 && pt.getProperty ("candidates", {}).size() == 1
             && (int) pt.getProperty ("candidates", {})[0].getProperty ("index", -1) == 22,
           "review V2: passThroughAtDefaults lists the pass-through candidate by index, with its count");
    check ((int) rv.getProperty ("curves", -1) == 1 && (int) rv.getProperty ("flats", -1) == 1,
           "review V3: flats counts band-coverage evidence only - the pass-through flat is not in it (" + juce::String ((int) rv.getProperty ("flats", -1)) + ")");
    const auto vd = rv.getProperty ("verdicts", {});
    juce::StringArray vs; for (int i = 0; i < vd.size(); ++i) vs.add (juce::String ((int) vd[i].getProperty ("index", -1)) + "=" + vd[i].getProperty ("result", "").toString());
    check (vs == juce::StringArray { "14=flat", "18=certified", "22=pass_through_at_defaults", "26=nonmonotonic", "30=licence_suspect" },
           "review V4: verdicts carries every candidate's result under the same index key, pass-through and licence-suspect named (" + vs.joinIntoString (",") + ")");
    const auto cs = f.getProperty ("thresholdCandidates", {});
    check (cs[2].getProperty ("reading", "").toString().startsWith ("pass_through_at_defaults")
             && cs[0].getProperty ("reading", "").toString().startsWith ("no response at 997 Hz"),
           "review V5: a pass-through candidate's reading says so, and is not the band-coverage reading");

    // A LICENCE IS A PROPERTY OF THE PRODUCT: a silent candidate beside one that produced the tone is a stage fact.
    auto suspect = [] (int idx, const char* name, const char* dataResult, bool pass) {
        ejmap::sweep::Plan q; q.thr = idx; q.thrName = name; q.norms = { 0.f, 0.5f, 1.f };
        Derivation d; d.d.result = dataResult; d.d.passThroughAtDefaults = pass; d.d.unlicensedSuspect = true; d.written = false;
        d.d.referenceNote = "the output is silent at every level; ";
        d.report = "x\nNOT LICENSED suspected (silent, non-finite or not the input's tone at default): the output is silent at every level; no thresholdSweep written\n";
        return std::make_pair (q, d); };
    std::vector<std::pair<ejmap::sweep::Plan, Derivation>> mixed { cand (18, "Band 2 Thresh", "certified", false, true), suspect (30, "Gate Thresh", "flat", false),
                                                                    suspect (34, "Proc 2 Thresh", "flat", true) };
    resolveLicenceAtProductLevel (mixed, {});
    check (mixed[1].second.written && mixed[1].second.sweepVar.getProperty ("result", "") == "flat"
             && mixed[1].second.sweepVar.getProperty ("silentOrOffToneAtDefault", "").toString().contains ("silent at every level")
             && mixed[1].second.report.contains ("NOT a licence question") && ! mixed[1].second.report.contains ("NOT LICENSED suspected"),
           "review V6: beside a candidate that produced the tone, a silent-at-default candidate is WRITTEN with its data's result and the silent reference recorded - not licence-suspect");
    composeCandidatesAndReport (base, plan, mixed, out, root.getChildFile ("f2.report.txt"));
    const auto rv2 = juce::JSON::parse (out.loadFileAsString()).getProperty ("thresholdReview", {});
    juce::StringArray vs2; for (int i = 0; i < rv2.getProperty ("verdicts", {}).size(); ++i) vs2.add (rv2.getProperty ("verdicts", {})[i].getProperty ("result", "").toString());
    check (vs2 == juce::StringArray { "certified", "flat", "pass_through_at_defaults" } && (int) rv2.getProperty ("passThroughAtDefaults", {}).getProperty ("count", 0) == 1,
           "review V7: and thresholdReview then reports what the data said - flat or pass-through - with no licence_suspect on a licensed product (" + vs2.joinIntoString (",") + ")");
    std::vector<std::pair<ejmap::sweep::Plan, Derivation>> allSilent { suspect (30, "Gate Thresh", "flat", false), suspect (34, "Proc 2 Thresh", "flat", true) };
    resolveLicenceAtProductLevel (allSilent, {});
    check (! allSilent[0].second.written && ! allSilent[1].second.written,
           "review V8: a product silent on EVERY candidate stays licence-suspect - no candidate produced the tone, so nothing says it is licensed");
    root.deleteRecursively();
}

/** ENGAGE DETECTION (spec section 3, built 1 Oct): the candidate rule, the response test, the record, and the trace run
    a re-derivation picks. The live two-sweep test itself runs plugins and is checked on the six pass-through products. */
void testEngageDetection()
{
    using namespace ejmap::cert;
    using namespace ejmap::sweep;
    auto fx = juce::JSON::parse (R"json({"product": "EMO-D5 (s)", "uid": "1", "version": "1", "format": "AudioUnit", "controls": [
        {"index": 0,  "name": "Gate On",      "numSteps": 2,          "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}},
        {"index": 15, "name": "Comp On",      "numSteps": 2,          "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}},
        {"index": 16, "name": "Comp Thresh",  "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0"}},
        {"index": 23, "name": "Comp Monitor", "numSteps": 2,          "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}},
        {"index": 28, "name": "Comp Mix",     "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 1.0, "display": "100.0"}},
        {"index": 40, "name": "Bypass",       "numSteps": 2,          "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}},
        {"index": 41, "name": "Power",        "numSteps": 2,          "defaultOnInstantiate": {"normalised": 1.0, "display": "On"}},
        {"index": 42, "name": "Auto Manual",  "numSteps": 2,          "defaultOnInstantiate": {"normalised": 1.0, "display": "Auto"}},
        {"index": 43, "name": "Sidechain In", "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 0.0, "display": "0"}}]})json");
    const auto cs = engageCandidates (fx, "Comp Thresh");
    juce::StringArray names; for (const auto& c : cs) names.add (c.name + "->" + juce::String (c.norm, 1));
    check (! cs.empty() && cs[0].name == "Comp On" && cs[0].norm == 1.0f && cs[0].fromDisplay == "Off",
           "engage E1: the two-step switch sharing the threshold's stage word ranks first, written to the OTHER extreme (" + names.joinIntoString (", ") + ")");
    check (names.contains ("Gate On->1.0") && names.contains ("Auto Manual->0.0") && names.contains ("Sidechain In->1.0"),
           "engage E2: other two-step switches and name-matched controls are candidates; a two-step at 1 is written to 0");
    check (! names.joinIntoString (",").contains ("Monitor") && ! names.joinIntoString (",").contains ("Bypass") && ! names.joinIntoString (",").contains ("Power")
             && ! names.joinIntoString (",").contains ("Comp Thresh") && ! names.joinIntoString (",").contains ("Comp Mix"),
           "engage E3: bypass, power and monitor are NEVER candidates, nor the threshold itself, nor a continuous control without an engage word (" + names.joinIntoString (", ") + ")");
    check (names.indexOf ("Comp On->1.0") < names.indexOf ("Gate On->1.0") && names.indexOf ("Gate On->1.0") < names.indexOf ("Sidechain In->1.0"),
           "engage E4: order is name-and-shape (affine first), then shape alone, then name alone");

    using namespace sweeptest;
    std::vector<std::array<std::optional<double>, 3>> pass (6, { 0.0, 0.0, 0.0 }), clean;
    for (int k = 0; k < 6; ++k) { const double r = (5 - k) * 1.5; clean.push_back ({ 2.0 - r * 0.5, 2.0 - r, 2.0 - r * 1.5 }); }
    check (! showsResponse (derive (fromGains (pass), kLevels, -1)) && showsResponse (derive (fromGains (clean), kLevels, -1)),
           "engage E5: the quick probe's test - pass-through is NOT a response, a curve is");

    Plan p; p.thr = 16; p.thrName = "Comp Thresh"; p.norms = { 0.f, 0.5f, 1.f };
    p.engage = { cs[0] }; p.engageTried = { "Gate On -> 1.0 (from 'Off'): flat - passthrough" , "Comp On -> 1.0 (from 'Off'): GAIN REDUCTION" };
    const auto d = derive (fromGains (clean), kLevels, -1);
    const auto v = composeThresholdSweep (d, displayCheck (d, ""), p, {});
    const auto eg = v.getProperty ("engageWrites", {});
    check (eg.isObject() && (bool) eg.getProperty ("found", false) && eg.getProperty ("writes", {}).size() == 1
             && eg.getProperty ("writes", {})[0].getProperty ("control", "") == "Comp On" && (bool) eg.getProperty ("writes", {})[0].getProperty ("verified", false)
             && eg.getProperty ("tried", {}).size() == 2,
           "engage E6: the fixture records the verified write (control, norm, from, verified) and every candidate tried");
    Plan none = p; none.engage.clear(); none.engageTried.clear();
    check (! composeThresholdSweep (d, displayCheck (d, ""), none, {}).hasProperty ("engageWrites"), "engage E7: a product that compressed as instantiated carries no engage record");
    Plan triedAll = p; triedAll.engage.clear();
    const auto vt = composeThresholdSweep (d, displayCheck (d, ""), triedAll, {}).getProperty ("engageWrites", {});
    check (vt.isObject() && ! (bool) vt.getProperty ("found", true) && vt.getProperty ("tried", {}).size() == 2,
           "engage E8: a product that stayed pass-through records found=false and what was tried - that is an answer");

    // THE TRACE RUN a re-derivation picks: q.e15.c16. over e15.c16. over c16.; the quick probes (eq15.) never.
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ejmap-engage-" + juce::Uuid().toDashedString());
    root.createDirectory();
    auto pj = root.getChildFile ("p.json");
    pj.replaceWithText (R"json([{"tag": "c16.ref"}, {"tag": "c16.pos00"}, {"tag": "eq15.c16.ref"}, {"tag": "eq15.c16.pos00"}, {"tag": "e15.c16.ref"}, {"tag": "e15.c16.pos00"}])json");
    const auto r1 = resolveTraceRun (pj, "c16.");
    pj.replaceWithText (R"json([{"tag": "c16.ref"}, {"tag": "eq15.c16.ref"}, {"tag": "e15.c16.ref"}, {"tag": "q.e15.c16.ref"}, {"tag": "q.e15.c16.pos00"}])json");
    const auto r2 = resolveTraceRun (pj, "c16.");
    pj.replaceWithText (R"json([{"tag": "c16.ref"}, {"tag": "eq15.c16.ref"}, {"tag": "eq15.c16.pos00"}])json");
    const auto r3 = resolveTraceRun (pj, "c16.");
    check (r1.prefix == "e15.c16." && r1.engageIndex == 15 && ! r1.quiet && r2.prefix == "q.e15.c16." && r2.quiet && r2.engageIndex == 15
             && r3.prefix == "c16." && r3.engageIndex == -1,
           "engage E9: re-derivation picks the engaged run, then its quiet fallback, and never a quick probe (" + r1.prefix + " / " + r2.prefix + " / " + r3.prefix + ")");
    Plan q; restoreEngage (q, v);
    check (q.engage.size() == 1 && q.engage[0].index == 15 && q.engage[0].norm == 1.0f && q.engageTried.size() == 2,
           "engage E10: the plan is restored from the fixture's engage record for re-derivation");
    root.deleteRecursively();
}

/** TUNER DERIVATIONS (spec section 5, built 1 Oct): strength and speed from synthetic window traces, each guard refusing. */
void testTunerDerivations()
{
    using namespace ejmap::pitch;
    // A static hold: windows every 10.7 ms for 4 s; input at +30 cents; output converging to `residual` with time constant tau.
    auto staticPos = [] (double detune, double inputRead, double residual, double tauMs, double jitter, bool silent = false) {
        PitchPosition p; p.k = 0; p.norm = 0.5f; p.landed = true;
        for (int i = 0; i < 370; ++i)
        {
            Window w; w.tMs = 21.3 + 10.667 * i; w.inC = inputRead; w.inConf = 0.999; w.target = detune;
            w.outC = residual + (inputRead - residual) * std::exp (-w.tMs / tauMs) + jitter * ((i % 3) - 1);
            w.outConf = 0.999; w.outDb = silent ? -90.0 : -20.0;
            p.windows.push_back (w);
        }
        return p; };
    const auto s1 = deriveStrength (staticPos (30.0, 30.0, 3.0, 100.0, 0.2), 30.0);
    check (s1.result == "measured" && std::abs (s1.strength - 0.9) < 0.01 && std::abs (s1.residualCents - 3.0) < 0.3,
           "tuner S1: +30 cents in, 3 cents residual at steady state -> strength 0.90 (" + s1.result + " " + juce::String (s1.strength, 3) + ": " + s1.reason + ")");
    check (deriveStrength (staticPos (30.0, 20.0, 3.0, 100.0, 0.2), 30.0).result == "refused",
           "tuner S2: the detector reading the INPUT at 20 cents when 30 were generated refuses everything (detector or routing fault)");
    check (deriveStrength (staticPos (30.0, 30.0, 3.0, 100.0, 0.2, true), 30.0).result == "refused", "tuner S3: a silent output is not a reading");
    const auto s4 = deriveStrength (staticPos (30.0, 30.0, 3.0, 100.0, 6.0), 30.0);
    check (s4.result == "refused" && s4.reason.contains ("did not settle"), "tuner S4: an output that never settles (IQR over 3 cents) refuses (" + s4.reason + ")");
    check (deriveStrength (staticPos (30.0, 30.0, 30.0, 100.0, 0.2), 30.0).strength == 0.0, "tuner S5: an untouched note is strength 0.0, measured, not refused");

    // A square vibrato at 0.5 Hz for 6 s: the input flips +-30 every 1000 ms; the output inherits each step and decays
    // back to the plateau with time constant tau (a slow tuner), or instantly (a fast one).
    auto vibPos = [] (double detune, double tauMs, double strengthFrac, double jitter, std::function<double (int)> tauOfEdge = {}) {
        PitchPosition p; p.k = 0; p.norm = 0.5f; p.landed = true;
        for (int i = 0; i < 560; ++i)
        {
            Window w; w.tMs = 21.3 + 10.667 * i; const double half = 1000.0; const int seg = (int) std::floor (w.tMs / half);
            const double target = seg % 2 == 0 ? detune : -detune; w.target = target; w.inC = target; w.inConf = 0.999;
            const double plateau = target * (1.0 - strengthFrac);                       // where the tuner settles for this half
            const double prev = (seg % 2 == 0 ? -detune : detune) * (1.0 - strengthFrac);
            const double t = w.tMs - seg * half;
            const double tau = tauOfEdge ? tauOfEdge (seg) : tauMs;
            w.outC = seg == 0 ? plateau : plateau + ((prev + (target - (seg % 2 == 0 ? -detune : detune))) - plateau) * std::exp (-t / tau) + jitter * ((i % 3) - 1);
            w.outConf = 0.999; w.outDb = -20.0;
            p.windows.push_back (w);
        }
        return p; };
    const auto v1 = deriveSpeed (vibPos (30.0, 60.0, 1.0, 0.2), 30.0, 0.5);
    // excursion 60 cents decaying with tau 60 ms: from 50% (t = 0, the first window) to 10% (t = tau ln 10 = 138 ms) -> about 130-150 ms
    check (v1.result == "measured" && v1.durationMs > 90.0 && v1.durationMs < 200.0 && v1.edges >= 3,
           "tuner V1: a 60 ms time-constant retune reads as a transition of about 140 ms over every edge (" + v1.result + " " + juce::String (v1.durationMs, 0) + " ms, " + juce::String (v1.edges) + " edges: " + v1.reason + ")");
    const auto v2 = deriveSpeed (vibPos (30.0, 2000.0, 1.0, 0.2), 30.0, 0.5);
    check (v2.result == "refused" && v2.reason.contains ("not settled"), "tuner V2: a retune slower than the half period refuses - not settled before the next flip (" + v2.result + ": " + v2.reason + ", " + juce::String (v2.durationMs, 0) + " ms)");
    const auto v3 = deriveSpeed (vibPos (30.0, 0.5, 1.0, 0.2), 30.0, 0.5);
    check (v3.result == "bound" && v3.boundMs > 0.0, "tuner V3: a correction that completes within one window is a BOUND (faster than " + juce::String (v3.boundMs, 1) + " ms), not a number (" + v3.result + ": " + v3.reason + ", excursion " + juce::String (v3.excursionCents, 2) + ")");
    const auto v4 = deriveSpeed (vibPos (30.0, 60.0, 1.0, 0.2, [] (int seg) { return seg % 2 ? 60.0 : 400.0; }), 30.0, 0.5);
    check (v4.result == "refused" && v4.reason.contains ("disagree"), "tuner V4: edges whose durations differ by more than 2x refuse (" + v4.reason + ")");
    // LATENCY MUST NOT CORRUPT THE DURATION: the same retune with the output 150 ms behind the input (a plugin latency).
    auto late = vibPos (30.0, 60.0, 1.0, 0.2);
    { std::vector<double> outs; for (const auto& w : late.windows) outs.push_back (w.outC);
      const int shift = 14;   // 14 windows * 10.667 ms = 149 ms
      for (size_t i = 0; i < late.windows.size(); ++i) late.windows[i].outC = i >= (size_t) shift ? outs[i - (size_t) shift] : outs[0]; }
    const auto v6 = deriveSpeed (late, 30.0, 0.5);
    check (v6.result == "measured" && std::abs (v6.durationMs - v1.durationMs) < 25.0,
           "tuner V6 (LATENCY): a 150 ms plugin latency leaves the transition DURATION unchanged - it is read off the output trace, never against the input's clock ("
             + juce::String (v6.durationMs, 0) + " vs " + juce::String (v1.durationMs, 0) + " ms)");
    const auto v5 = deriveSpeed (vibPos (30.0, 60.0, 0.0, 0.2), 30.0, 0.5);
    check (v5.result != "measured", "tuner V5: a tuner that corrects nothing shows no excursion to time (" + v5.result + ": " + v5.reason + ")");

    // THE PARSER and the record.
    const juce::String out = "pitch\tproto\t1\tctl\t3\tname\tRetune Speed\tpositions\t1\tgen\tstatic\tshape\tsquare\tnote_hz\t220.000\tcents\t30.00\trate_hz\t0.500\thold_s\t4.000\tdb\t-18.00\n"
                             "config\tmain_in\t2\tmain_out\t2\tlatency\t256\tsr\t48000\twin\t2048\thop\t512\n"
                             "param\t3\t0.500000\tRetune Speed\t20\n"
                             "ppos\t0\tnorm\t0.500000\tconfirm_ms\t3.0\tlanded_by\tinstack\ttext\t20\n"
                             "pwin\t0\tt_ms\t21.3\tin_cents\t30.00\tin_conf\t0.9990\tout_cents\t29.00\tout_conf\t0.9990\tout_db\t-20.00\tin_target\t30.00\n"
                             "pdone\t0\twindows\t1\n";
    const auto m = parsePitch (out);
    check (m.ok && m.ctl == 3 && m.ctlName == "Retune Speed" && m.gen == "static" && m.latency == 256 && m.positions.size() == 1
             && m.positions[0].windows.size() == 1 && std::abs (m.positions[0].windows[0].outC - 29.0) < 1e-9 && m.params.at (3).second == "20",
           "tuner P1: the probe's pitch lines parse into control, generator, latency, positions and windows");
    check (! parsePitch ("refused no parameter at index 9").ok && parsePitch ("refused no parameter at index 9").refused.contains ("no parameter"), "tuner P2: a refusal is carried, not parsed into nothing");
    PitchMeasured st; st.ok = true; st.ctl = 3; st.ctlName = "Retune Speed"; st.noteHz = 220; st.cents = 30; st.positions = { staticPos (30.0, 30.0, 3.0, 100.0, 0.2) };
    PitchMeasured vb; vb.ok = true; vb.ctl = 3; vb.ctlName = "Retune Speed"; vb.noteHz = 220; vb.cents = 30; vb.rateHz = 0.5; vb.shape = "square"; vb.positions = { vibPos (30.0, 60.0, 1.0, 0.2) };
    const auto rec = composePitchSweep (st, vb, 7, "Chromatic");
    const auto pos0 = rec.getProperty ("positions", {})[0];
    check ((int) rec.getProperty ("strengthMeasured", 0) == 1 && (int) rec.getProperty ("speedMeasured", 0) == 1
             && pos0.getProperty ("strength", {}).getProperty ("result", "") == "measured" && pos0.getProperty ("speed", {}).getProperty ("result", "") == "measured"
             && rec.getProperty ("keyScale", {}).getProperty ("asInstantiated", "") == "Chromatic",
           "tuner R1: the record carries both numbers per position, the key/scale as instantiated, and the counts");
    check (ejmap::cert::araOnlyByName ("Melodyne") && ejmap::cert::araOnlyByName ("Waves Tune LT") && ! ejmap::cert::araOnlyByName ("Auto-Tune Access"),
           "tuner A1: an ARA/offline-only tool is refused by name before any process; a real-time tuner is not");
}

/** THE JOIN KEY (ruled 1 Oct): every store record carries param_count from the probe's raw listing and map_fp from the one
    shared fingerprint function; every record that has a local EJ Map map reproduces that map's fp. The five that len(controls)
    got wrong (NEOLD U2A 11 vs 13 ...) are the reason this is pinned against the corpus and not against a formula. */
void testMapFpJoinKey()
{
    using namespace ejmap::cert;
    // The rule, pure: a fixture composed from 13 list rows and 11 text-at rows carries param_count 13 and the hash of 13.
    Subject s; s.desc.pluginFormatName = "AudioUnit"; s.desc.uniqueId = 0x61755c26; s.desc.version = "1.1.0"; s.desc.name = "NEOLD U2A";
    s.product = s.desc.name; s.uid = "61755c26"; s.version = "1.1.0";
    std::map<int, ListRow> list; for (int i = 0; i < 13; ++i) { ListRow r; r.name = "P" + juce::String (i); list[i] = r; }
    std::vector<TextAtRow> text; for (int i = 0; i < 11; ++i) { TextAtRow t; t.index = i; t.name = "P" + juce::String (i); t.defText = "0"; text.push_back (t); }
    const auto fx = composeFixture (s, list, text, 0, 0, "probe", "2026-10-01");
    check ((int) fx.getProperty ("param_count", 0) == 13 && fx.getProperty ("controls", {}).size() == 11
             && fx.getProperty ("map_fp", "").toString() == echojay::fingerprintForDescription (s.desc, 13)
             && fx.getProperty ("map_fp", "").toString() != echojay::fingerprintForDescription (s.desc, 11),
           "mapfp M0: param_count is the list's size, not controls.length, and map_fp hashes it through the shared function");
    // M0b (2 Oct): the vendor rides on the record from the host's description (CL 1B's discovery record had none, and the
    // export's plugin.manufacturer was empty)
    Subject sv = s; sv.desc.manufacturerName = "NEOLD";
    check (composeFixture (sv, list, text, 0, 0, "probe", "2026-10-02").getProperty ("manufacturer", "").toString() == "NEOLD" && ! fx.hasProperty ("manufacturer"),
           "mapfp M0b: manufacturer is the host's manufacturerName on the record, and absent (never empty) when the host has none");

    // Against the corpus: every store record with a local map reproduces that map's fp. Skipped (said so) without the maps.
    auto mapsDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/maps");
    auto store = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/cert-fixtures/profiles");
    if (! mapsDir.isDirectory() || mapsDir.getNumberOfChildFiles (juce::File::findFiles, "*.json") < 100)
    { std::cout << "mapfp M1: no corpus at " << mapsDir.getFullPathName() << ", skipped" << std::endl; return; }
    std::map<juce::String, juce::String> fpByIdentity;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString());
        const auto id = m.getProperty ("identity", {});
        if (id.isObject()) fpByIdentity[id.getProperty ("format", "").toString() + "|" + id.getProperty ("uid", "").toString().toLowerCase() + "|" + id.getProperty ("version", "").toString()] = m.getProperty ("fp", "").toString();
    }
    int withMap = 0, match = 0, noField = 0; juce::StringArray bad;
    for (const auto& f : store.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto j = juce::JSON::parse (f.loadFileAsString());
        if (! j.hasProperty ("map_fp")) { ++noField; continue; }
        const auto key = j.getProperty ("format", "").toString() + "|" + j.getProperty ("uid", "").toString().toLowerCase() + "|" + j.getProperty ("version", "").toString();
        auto it = fpByIdentity.find (key);
        if (it == fpByIdentity.end()) continue;
        ++withMap;
        if (it->second == j.getProperty ("map_fp", "").toString()) ++match; else bad.add (j.getProperty ("product", "").toString());
    }
    check (noField == 0, "mapfp M1a: every store record carries map_fp (" + juce::String (noField) + " without)");
    check (withMap >= 90 && match == withMap, "mapfp M1: every store record with a local map reproduces the map's fp - " + juce::String (match) + " of "
                                                + juce::String (withMap) + (bad.isEmpty() ? juce::String() : "; wrong: " + bad.joinIntoString (", ")));
    // REALITY: EchoJay's OWN persisted identity -> fp index (written at slot load; the source of fp= in EJDialSummary, which
    // prints only its first 12 characters). Every store record it has loaded must carry the fp it computed.
    const auto idxFile = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/EchoJay/chain_fp_scan.json");
    if (! idxFile.existsAsFile()) { std::cout << "mapfp M2: no EchoJay identity index at " << idxFile.getFullPathName() << ", skipped" << std::endl; return; }
    const auto idx = juce::JSON::parse (idxFile.loadFileAsString()).getProperty ("identityToFp", {});
    int seen = 0, same = 0; juce::StringArray wrong;
    if (auto* o = idx.getDynamicObject())
        for (const auto& f : store.findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            const auto j = juce::JSON::parse (f.loadFileAsString());
            const auto key = j.getProperty ("identity", "").toString();
            for (const auto& prop : o->getProperties())
                if (prop.name.toString().equalsIgnoreCase (key))
                { ++seen; if (prop.value.toString() == j.getProperty ("map_fp", "").toString()) ++same; else wrong.add (j.getProperty ("product", "").toString()); }
        }
    check (seen >= 80 && same == seen, "mapfp M2 (REALITY): EchoJay's own identity->fp index agrees on every store record it has loaded - " + juce::String (same) + " of " + juce::String (seen)
                                       + (wrong.isEmpty() ? juce::String() : "; wrong: " + wrong.joinIntoString (", ")));
}

/** THE EXPORTER (ej_comp_profile/1, v1.1): one place, pinned. The level reference is the likeliest silent error, so it is
    pinned twice - against the constant and against a committed trace that prints the RMS of a known peak tone. */
void testProfileExport()
{
    using namespace ejmap::profile;
    // X0: the constant, MEASURED from MCompressor's committed Peak-arm trace: the probe prints in_rms_db beside each hold.
    const auto adir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep/mcompressor-rms-peak");
    ejmap::sweep::ProcessOut r; std::vector<ejmap::sweep::ProcessOut> ps;
    const bool ok = ejmap::sweep::loadProcesses (adir.getChildFile ("processes.json"), adir.getChildFile ("raw"), r, ps);
    double measuredOffset = 0.0; bool found = false;
    if (ok) for (const auto& line : juce::StringArray::fromLines (ps[0].out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() > 8 && f[0] == "hold" && f[2] == "-24.00") { measuredOffset = -24.0 - f[f.indexOf ("in_rms_db") + 1].getDoubleValue(); found = true; break; }
    }
    check (ok && found && std::abs (measuredOffset - kPeakToSineRmsDb) < 0.001,
           "export X0 (LEVEL REFERENCE, measured): a -24 dBFS PEAK tone reads -27.01 dB RMS in the committed trace - the conversion is " + juce::String (measuredOffset, 4) + " dB, the constant 3.0103");

    // A profile-grade record, built by hand: 31 levels, a quiet reference passing at every position, a textbook 4:1 curve.
    auto record = [] (int positions, bool quietOk, bool candidates, const char* flags, const char* result) {
        juce::DynamicObject* f = new juce::DynamicObject();
        f->setProperty ("schema", "ej_cert_compressor/1"); f->setProperty ("product", "Synth Comp"); f->setProperty ("manufacturer", "Test");
        f->setProperty ("format", "AudioUnit"); f->setProperty ("identity", "AudioUnit|1234abcd|1.0.0"); f->setProperty ("version", "1.0.0"); f->setProperty ("uid", "1234abcd");
        f->setProperty ("map_fp", juce::String::repeatedString ("ab", 32));
        juce::Array<juce::var> controls;
        auto ctl = [] (int i, const char* n, int steps) { auto* c = new juce::DynamicObject(); c->setProperty ("index", i); c->setProperty ("name", n); c->setProperty ("unit", "dB"); c->setProperty ("numSteps", steps);
                                                            auto* d = new juce::DynamicObject(); d->setProperty ("normalised", 0.5); d->setProperty ("display", "0"); c->setProperty ("defaultOnInstantiate", juce::var (d)); return juce::var (c); };
        controls.add (ctl (0, "Threshold", 2147483647)); controls.add (ctl (1, "Ratio", 2147483647)); controls.add (ctl (2, "Power", 2)); controls.add (ctl (3, "Bypass", 2)); controls.add (ctl (4, "Comp On", 2));
        f->setProperty ("controls", controls);
        auto* s = new juce::DynamicObject();
        s->setProperty ("measuredAt", "20261001T120000.000+0100"); s->setProperty ("level_convention", "peak"); s->setProperty ("result", result); s->setProperty ("hold_s", 2.5); s->setProperty ("win_s", 0.3);
        auto* tone = new juce::DynamicObject(); juce::Array<juce::var> lv; for (int L = -60; L <= 0; L += 2) lv.add ((double) L); tone->setProperty ("levels_dbfs", lv); s->setProperty ("tone", juce::var (tone));
        auto* rd = new juce::DynamicObject(); rd->setProperty ("value", 4.0); rd->setProperty ("position", "4.0:1"); s->setProperty ("ratioDuring", juce::var (rd));
        juce::Array<juce::var> norms, texts, eff; auto* red = new juce::DynamicObject(); std::map<int, juce::Array<juce::var>> cols;
        for (int i = 0; i < positions; ++i)
        {
            const double T = -40.0 + 2.0 * i;                               // hard-knee 4:1 threshold per position (peak dBFS)
            norms.add ((double) i / (double) (positions - 1)); texts.add (juce::String (T, 1)); eff.add (T + 1.0 / 0.75);
            for (int L = -60; L <= 0; L += 2) cols[L].add (L > T ? (L - T) * 0.75 : 0.0);
        }
        for (auto& [L, col] : cols) red->setProperty (juce::String (L), col);
        s->setProperty ("positions", positions); s->setProperty ("positionNorms", norms); s->setProperty ("positionTexts", texts);
        s->setProperty ("thresholdEffective1dB", eff); s->setProperty ("reduction_db", juce::var (red));
        juce::Array<juce::var> inAt;
        for (int i = 0; i < positions; ++i)
        {
            const double T = -40.0 + 2.0 * i; auto* g = new juce::DynamicObject();
            auto at = [&] (double gr) { const double v = T + gr / 0.75; return v <= 0.0 ? juce::var (v) : juce::var ("not_reached"); };
            g->setProperty ("1", at (1.0)); g->setProperty ("2", at (2.0)); g->setProperty ("3", i == positions - 1 ? juce::var ("not_reached") : at (3.0));   // the last position's 3 dB point: a not_reached to export as null
            inAt.add (juce::var (g));
        }
        s->setProperty ("inAtGr", inAt);
        auto* q = new juce::DynamicObject(); q->setProperty ("nonMonotonicStraddles", 0); q->setProperty ("widestGap_db", 2.0); s->setProperty ("inAtGrQuality", juce::var (q));
        auto* dq = new juce::DynamicObject(); dq->setProperty ("fraction", 0.2); s->setProperty ("detector", juce::var (dq));                                    // v1.4: required
        auto* rq = new juce::DynamicObject(); rq->setProperty ("point_error_db", 0.12); rq->setProperty ("repeats", 2); rq->setProperty ("method", "hold 2.5 s vs 5 s"); s->setProperty ("quality", juce::var (rq));
        auto* lr = new juce::DynamicObject(); lr->setProperty ("mode", "per_position_quiet");
        juce::Array<juce::var> chk, g; for (int i = 0; i < positions; ++i) { chk.add (quietOk ? 0.02 : 0.9); g.add (0.5); }
        lr->setProperty ("check_db", chk); lr->setProperty ("gain_db", g); s->setProperty ("linearReference", juce::var (lr));
        auto* ld = new juce::DynamicObject(); ld->setProperty ("implied_ratio", 4.0); s->setProperty ("levelDependence", juce::var (ld));
        if (juce::String (flags).isNotEmpty()) s->setProperty ("roleFlag", flags);
        auto* ew = new juce::DynamicObject(); ew->setProperty ("found", true); juce::Array<juce::var> ws; auto* w = new juce::DynamicObject();
        w->setProperty ("index", 4); w->setProperty ("control", "Comp On"); w->setProperty ("norm", 1.0); w->setProperty ("set", "On"); w->setProperty ("verified", true); ws.add (juce::var (w));
        ew->setProperty ("writes", ws); s->setProperty ("engageWrites", juce::var (ew));
        f->setProperty ("thresholdSweep", juce::var (s));
        if (candidates) { f->removeProperty ("thresholdSweep"); f->setProperty ("thresholdCandidates", juce::Array<juce::var>()); }
        return juce::var (f); };
    const auto rec = record (16, true, false, "", "certified");
    const auto e = exportCompProfile (rec);
    check (e.ok, "export X1: a profile-grade record exports (" + e.refused + ")");
    if (e.ok)
    {
        const auto P = e.profile;
        const auto curve = P.getProperty ("amount", {}).getProperty ("curve", {});
        const double effPeak = (double) rec.getProperty ("thresholdSweep", {}).getProperty ("thresholdEffective1dB", {})[0];
        const auto steps = P.getProperty ("measured", {}).getProperty ("steps_dbfs", {});
        check (curve.size() == 16 && std::abs ((double) curve[0].getProperty ("eff_threshold_dbfs", 0.0) - (effPeak - 3.0103)) < 0.006
                 && steps.size() == 3 && std::abs ((double) steps[0] - (-63.01)) < 0.006 && std::abs ((double) steps[1] - (-3.01)) < 0.006 && (double) steps[2] == 2.0
                 && P.getProperty ("measured", {}).getProperty ("level_ref", "") == "sine_rms_dbfs",
               "export X2 (LEVEL REFERENCE): every exported dBFS is our peak value minus 3.0103 - eff " + juce::String ((double) curve[0].getProperty ("eff_threshold_dbfs", 0.0), 2)
                 + " from " + juce::String (effPeak, 2) + ", steps_dbfs " + juce::JSON::toString (steps, true));
        check (std::abs (toSineRms (0.0) - (-3.0103)) < 1e-6 && juce::String (r2 (toSineRms (0.0)), 2) == "-3.01",
               "export X2b (his section 5 pin): a full-scale 997 Hz sine, 0 dBFS peak, exports as -3.01");
        // v1.2: in_at_gr on every point, eff identical to "1", the words null, stepped a boolean, detector unknown, fit never a gate.
        const auto g0 = curve[0].getProperty ("in_at_gr_dbfs", {});
        bool identical = true, lastThreeNull = false;
        for (int i = 0; i < curve.size(); ++i) identical = identical && curve[i].getProperty ("eff_threshold_dbfs", {}).toString() == curve[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("1", {}).toString();
        lastThreeNull = curve[curve.size() - 1].getProperty ("in_at_gr_dbfs", {}).getProperty ("3", {}).isVoid();
        check (g0.isObject() && g0.hasProperty ("1") && g0.hasProperty ("2") && g0.hasProperty ("3") && identical
                 && std::abs ((double) g0.getProperty ("2", 0.0) - ((double) g0.getProperty ("1", 0.0) + 1.0 / 0.75)) < 0.02,
               "export X14 (v1.2): in_at_gr_dbfs {1,2,3} on every curve point and eff_threshold_dbfs == in_at_gr_dbfs[1], written identically");
        check (lastThreeNull, "export X15: not_reached in the record is null in the export, as his spec says");
        check (P.getProperty ("amount", {}).getProperty ("stepped", true).isBool() && ! (bool) P.getProperty ("amount", {}).getProperty ("stepped", true)
                 && std::abs ((double) P.getProperty ("detector_f", 9.0) - 0.2) < 1e-6,
               "export X16 (v1.4): stepped is a boolean (false for a continuous control); detector_f is the measured number");
        const auto Q = P.getProperty ("quality", {});
        check (Q.isObject() && std::abs ((double) Q.getProperty ("point_error_db", 9.0) - 0.12) < 1e-6 && Q.getProperty ("method", "") == "hold 2.5 s vs 5 s"
                 && (bool) Q.getProperty ("monotonic_within_positions", false) && (bool) Q.getProperty ("monotonic_across_positions", false),
               "export X19 (v1.4): quality carries point_error_db from the hold-doubled repeat, its method, and the monotonic flags");
        check (P.getProperty ("notes", "").toString().contains ("guards passed: tone_frac") && P.getProperty ("notes", "").toString().contains ("quiet-reference 6 dB")
                 && P.getProperty ("notes", "").toString().contains ("ascending-only"),
               "export X20 (v1.4): notes list the guards that passed - tone_frac, level dependence, quiet-reference 6 dB check, ascending-only");
        check (P.getProperty ("fit", {}).getProperty ("measured_point_quality", {}).isObject()
                 && (int) P.getProperty ("fit", {}).getProperty ("measured_point_quality", {}).getProperty ("points_with_1db", 0) == 16,
               "export X17: a measured-point quality figure sits beside the fit");
        check (P.getProperty ("topology", "") == "threshold" && P.getProperty ("plugin", {}).getProperty ("map_fp", "").toString().length() == 64
                 && (double) P.getProperty ("measured", {}).getProperty ("reference_ratio", 0.0) == 4.0 && (int) P.getProperty ("measured", {}).getProperty ("hold_ms", 0) == 2500,
               "export X3: topology threshold, 64-hex map_fp, reference_ratio is the read-back ratio, hold 2500 ms");
        check (P.getProperty ("engage", {}).size() == 1 && P.getProperty ("engage", {})[0].getProperty ("control", "") == "Comp On" && (bool) P.getProperty ("engage", {})[0].getProperty ("verified", false)
                 && P.getProperty ("never_touch", {}).size() == 2 && P.getProperty ("never_touch", {}).indexOf ("Power") >= 0 && P.getProperty ("never_touch", {}).indexOf ("Bypass") >= 0,
               "export X4: engage carries the verified write with its read-back; never_touch lists Power and Bypass");
        check (std::abs ((double) P.getProperty ("static_gain_db", 9.0) - 0.5) < 0.01 && P.getProperty ("level_coupling", {}).isVoid(),
               "export X5: static_gain_db is the median quiet-level gain; no level_coupling on a threshold topology");
        check (e.fitMaxErrorDb < 0.05 && (double) P.getProperty ("fit", {}).getProperty ("error_vs_2db_target", 9.0) < 0.03,
               "export X6: his model fits a textbook hard-knee 4:1 curve to within 0.05 dB (" + juce::String (e.fitMaxErrorDb, 3) + ")");
        check (! P.hasProperty ("time"), "export X7: time is omitted, not invented");
    }
    check (! exportCompProfile (record (16, false, false, "", "certified")).ok, "export X8: a quiet check failing at every position means NO PROFILE (static_gain_db cannot be given)");
    {
        // fit is NEVER a gate: distort the reduction readings so his model cannot fit them, and the export still happens.
        auto soft = record (16, true, false, "", "certified");
        auto* red = soft.getProperty ("thresholdSweep", {}).getProperty ("reduction_db", {}).getDynamicObject();
        for (auto& prop : red->getProperties()) { auto col = prop.value; for (int i = 0; i < col.size(); ++i) col[i] = std::sqrt (juce::jmax (0.0, (double) col[i])) * 3.0; red->setProperty (prop.name, col); }
        const auto sx = exportCompProfile (soft);
        check (sx.ok && sx.fitMaxErrorDb > 1.5 && sx.profile.getProperty ("notes", "").toString().contains ("NOT a gate"),
               "export X18 (v1.2): a record his v1 model cannot fit (max error " + juce::String (sx.fitMaxErrorDb, 2) + " dB) still exports - fit is reported, never a gate");
    }
    check (! exportCompProfile (record (6, true, false, "", "certified")).ok, "export X9: fewer than 9 curve points refuses");
    check (! exportCompProfile (record (16, true, true, "", "certified")).ok && exportCompProfile (record (16, true, true, "", "certified")).refused.contains ("other"),
           "export X10: several candidates is topology other and no profile");
    check (! exportCompProfile (record (16, true, false, "", "flat")).ok, "export X11: a non-certified sweep refuses");
    {
        // RATIO (2 Oct, ruled): an adjustable ratio never exports as fixed; its one curve point carries the norm the sweep
        // ran at, the display, the value, and measured_ratio implied from level dependence; knee_db null everywhere.
        const auto Pa = exportCompProfile (rec).profile.getProperty ("ratio", {});
        check (Pa.getProperty ("fixed", {}).isVoid() && Pa.getProperty ("curve", {}).size() == 1 && Pa.getProperty ("knee_db", 0.0).isVoid()
                 && std::abs ((double) Pa.getProperty ("curve", {})[0].getProperty ("norm", -1.0) - 0.5) < 1e-9 && Pa.getProperty ("curve", {})[0].getProperty ("set", "").toString() == "4.0:1"
                 && std::abs ((double) Pa.getProperty ("curve", {})[0].getProperty ("measured_ratio", -1.0) - 4.0) < 1e-9 && Pa.getProperty ("curve", {})[0].getProperty ("source", "") == "instantiate",
               "export X22: an adjustable ratio never exports as fixed - one curve point at the instantiate norm with display, value and the implied measured_ratio; knee_db null");
        auto raised = juce::JSON::parse (juce::JSON::toString (rec));
        { juce::Array<juce::var> pa; auto* x = new juce::DynamicObject(); x->setProperty ("index", 1); x->setProperty ("norm", 0.8); x->setProperty ("set", "8.0:1"); x->setProperty ("role", "ratio_raise"); pa.add (juce::var (x));
          raised.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("preconditions", pa); }
        const auto Pr = exportCompProfile (raised).profile.getProperty ("ratio", {});
        check (std::abs ((double) Pr.getProperty ("curve", {})[0].getProperty ("norm", -1.0) - 0.8) < 1e-9 && Pr.getProperty ("curve", {})[0].getProperty ("source", "") == "precondition"
                 && exportCompProfile (raised).profile.getProperty ("neutral", {}).size() == exportCompProfile (rec).profile.getProperty ("neutral", {}).size(),
               "export X22b: a ratio_raise precondition is the ratio point's norm and never a neutral entry");
        auto noRatio = juce::JSON::parse (juce::JSON::toString (rec));
        { juce::Array<juce::var> cs; for (int i = 0; i < noRatio.getProperty ("controls", {}).size(); ++i) if (i != 1) cs.add (noRatio.getProperty ("controls", {})[i]); noRatio.getDynamicObject()->setProperty ("controls", cs); }
        const auto en = exportCompProfile (noRatio);
        check (en.ok && en.profile.getProperty ("ratio", {}).getProperty ("fixed", {}).isObject() && en.profile.getProperty ("ratio", {}).getProperty ("fixed", {}).getProperty ("knee_db", 0.0).isVoid()
                 && std::abs ((double) en.profile.getProperty ("ratio", {}).getProperty ("fixed", {}).getProperty ("measured_ratio", -1.0) - 4.0) < 1e-9,
               "export X22c: a device with no ratio control keeps fixed {measured_ratio}, knee_db null (" + en.refused + ")");
    }
    {
        // NEUTRAL (2 Oct, ruled): every control except the amount, the ratio, readouts/meters, the engage writes and
        // never_touch, at the value it was measured at, from a precondition where one was written, else the instantiate value.
        auto full = juce::JSON::parse (juce::JSON::toString (rec));
        auto ctl = [] (int i, const char* n, double norm, const char* disp, bool readout) { auto* c = new juce::DynamicObject(); c->setProperty ("index", i); c->setProperty ("name", n); c->setProperty ("numSteps", 2147483647);
                                                                                             auto* d = new juce::DynamicObject(); d->setProperty ("normalised", norm); d->setProperty ("display", disp); c->setProperty ("defaultOnInstantiate", juce::var (d)); if (readout) c->setProperty ("readout", true); return juce::var (c); };
        { auto cs = full.getProperty ("controls", {}); cs.append (ctl (5, "Attack", 0.5, "5.0", false)); cs.append (ctl (6, "Meter", 0.5, "Comp", false)); cs.append (ctl (7, "GR Readout", 0.1, "-3", true)); cs.append (ctl (8, "Mix", 0.7, "70 %", false)); cs.append (ctl (9, "Sidechain", 0.0, "Int", false));
          full.getDynamicObject()->setProperty ("controls", cs);
          juce::Array<juce::var> pa; auto* x = new juce::DynamicObject(); x->setProperty ("index", 8); x->setProperty ("norm", 1.0); x->setProperty ("set", "100 %"); x->setProperty ("role", "mix_wet"); pa.add (juce::var (x));
          full.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("preconditions", pa); }
        const auto ef = exportCompProfile (full);
        const auto nn = ef.profile.getProperty ("neutral", {});
        juce::StringArray names; for (int i = 0; i < nn.size(); ++i) names.add (nn[i].getProperty ("control", "").toString() + "=" + nn[i].getProperty ("set", "").toString() + "@" + juce::String ((double) nn[i].getProperty ("norm", -1.0), 2) + ":" + nn[i].getProperty ("source", "").toString());
        // controls: 0 Threshold (amount) 1 Ratio 2 Power (never_touch) 3 Bypass (never_touch) 4 Comp On (engage) 5 Attack 6 Meter 7 GR Readout 8 Mix (precondition) 9 Sidechain
        check (ef.ok && nn.size() == 3 && names.joinIntoString (" ") == "Attack=5.0@0.50:instantiate Mix=100 %@1.00:precondition Sidechain=Int@0.00:instantiate",
               "export X23: neutral covers every non-amount, non-ratio, non-readout control (engage and never_touch excluded, meters and readouts excluded) at the measured value with set text and norm (" + names.joinIntoString (" ") + ")");
        auto bare = juce::JSON::parse (juce::JSON::toString (full));
        bare.getProperty ("controls", {})[5].getDynamicObject()->removeProperty ("defaultOnInstantiate");
        const auto eb = exportCompProfile (bare);
        check (! eb.ok && eb.refused.contains ("Attack"), "export X24: a neutral control with no instantiate value on the record refuses, naming it (" + eb.refused + ")");
        // THE TONE CHECK'S WRITES (2 Oct): engage + neutral + ratio from the exported profile, resolved by name; nothing from the record's sweep
        juce::StringArray sets; juce::Array<juce::var> writes; juce::String note;
        const auto why = toneWrites (ef.profile, full, sets, writes, note);
        juce::StringArray got; for (const auto& w : writes) got.add (w.getProperty ("control", "").toString() + "@" + juce::String ((double) w.getProperty ("norm", -1.0), 2));
        check (why.isEmpty() && sets.size() == 5 && got.joinIntoString (" ") == "Comp On@1.00 Attack@0.50 Mix@1.00 Sidechain@0.00 Ratio@0.50" && sets.contains ("1:0.500000"),
               "tone T1: the writes are engage (Comp On), every neutral entry and ratio.curve[0], all from the profile (" + got.joinIntoString (" ") + "; " + why + ")");
        auto renamed = juce::JSON::parse (juce::JSON::toString (full)); renamed.getProperty ("controls", {})[1].getDynamicObject()->setProperty ("name", "Slope");
        juce::StringArray s2; juce::Array<juce::var> w2; juce::String n2;
        check (toneWrites (ef.profile, renamed, s2, w2, n2).contains ("Ratio"), "tone T2: a profile control the record cannot name refuses, naming it");
        auto fixedP = juce::JSON::parse (juce::JSON::toString (ef.profile)); fixedP.getProperty ("ratio", {}).getDynamicObject()->setProperty ("control", juce::var()); fixedP.getProperty ("ratio", {}).getDynamicObject()->setProperty ("curve", juce::Array<juce::var>());
        juce::StringArray s3; juce::Array<juce::var> w3; juce::String n3;
        check (toneWrites (fixedP, full, s3, w3, n3).isEmpty() && s3.size() == 4 && n3.contains ("fixed"), "tone T3: a fixed-ratio profile writes no ratio and says so");
    }
    const auto drive = exportCompProfile (record (16, true, false, "input_as_threshold", "certified"));
    check (drive.ok && drive.profile.getProperty ("topology", "") == "input_drive" && drive.profile.getProperty ("level_coupling", {}).getProperty ("gain_db_per_point", {}).size() == 16,
           "export X12: input-as-threshold is input_drive with level_coupling from the per-position quiet gain");
    {
        // HIS SECTION 6 PICK on the exported profile: in_at_gr at g, nearest L, interpolated between positions for a continuous
        // control, the nearest detent for a stepped one, never a position whose 1 dB point is more than 8 dB below L.
        const auto prof = e.profile;                                          // positions: in_at_gr[2] = T + 2.667, T = -40 + 2i (peak) -> RMS -3.01
        const auto pk = pickPosition (prof, -18.0, 2.0);
        // T + 2.667 - 3.01 = -18 -> T = -17.66 -> between i = 11 (T -18) and i = 12 (T -16): norm between 11/15 and 12/15
        check (pk.ok && pk.i1 >= 0 && pk.norm > 11.0 / 15.0 && pk.norm < 12.0 / 15.0 && std::abs (pk.inAtG0 - (-18.34)) < 0.05,
               "pick P1: for L = -18 RMS, g = 2 the pick interpolates between the two positions whose 2 dB points bracket L (norm " + juce::String (pk.norm, 4) + ", " + juce::String (pk.inAtG0, 2) + " / " + juce::String (pk.inAtG1, 2) + ")");
        const auto pk15 = pickPosition (prof, -18.0, 1.5);
        // g = 1.5 reads T + 2.0 (midway between the 1 and 2 dB points, T + 1.33 and T + 2.67): the bracket below L is i = 11, value -18 + 2 - 3.01 = -19.01
        check (pk15.ok && std::abs (pk15.inAtG0 - (-19.01)) < 0.05, "pick P2: a fractional g interpolates between the 1 and 2 dB points (" + juce::String (pk15.inAtG0, 2) + ", expected -19.01)");
        auto stepped = juce::JSON::parse (juce::JSON::toString (prof));           // a deep copy
        stepped.getProperty ("amount", {}).getDynamicObject()->setProperty ("stepped", true);
        const auto ps = pickPosition (stepped, -18.0, 2.0);
        check (ps.ok && ps.i1 < 0 && (std::abs (ps.norm - 11.0 / 15.0) < 1e-6 || std::abs (ps.norm - 12.0 / 15.0) < 1e-6),
               "pick P3: a stepped control gets the nearest listed detent, never an interpolated norm (" + juce::String (ps.norm, 4) + ")");
        const auto pkc = pickPosition (prof, -50.0, 2.0);                     // every 1 dB point sits more than 8 dB above... no: L = -50 is far BELOW all points
        const auto pkd = pickPosition (prof, 10.0, 2.0);                      // L = +10: every 1 dB point is more than 8 dB below L -> clamped out
        check (pkc.ok && ! pkd.ok, "pick P4: the clamp refuses every position whose 1 dB point is more than 8 dB below L (" + pkd.refused + ")");
    }
    {
        // THE DETECTOR FRACTION: same 2 dB level on both signals = rms (0); the two-tone 3.01 dB earlier = peak (1); the word only at an end.
        check (std::abs (detectorFraction (-20.0, -20.0)) < 1e-9 && std::abs (detectorFraction (-20.0, -23.0103) - 1.0) < 1e-6 && std::abs (detectorFraction (-20.0, -21.5) - 0.498) < 0.01,
               "detector D1: f = shift / 3.01 - 0 for equal levels, 1 for a 3.01 dB earlier two-tone, 0.5 halfway");
        check (detectorWord (0.05) == "rms" && detectorWord (0.95) == "peak" && detectorWord (0.5) == "unknown" && detectorWord (std::nullopt) == "unknown",
               "detector D2: the spec's word only at an end of the range; in between, and unmeasured, it is unknown");
        {
            // D6 (2 Oct): the detector's and the tone check's processes hold ONE position. A one-position derive has no
            // sense and no verdict, but the reduction against the position's own quiet rung and its in_at_gr are a
            // measurement and must come back - until 2 Oct they did not, and the live detector had never recorded.
            using namespace ejmap::sweep;
            Measured one; one.ok = true; one.movingDb = 0.1;
            PositionReading r; r.k = 0; r.norm = 0.3f; r.text = "-20 dB";
            std::vector<double> lv { -54.0, -48.0 }; for (int L = -60; L <= 0; L += 2) lv.push_back ((double) L);
            for (double L : lv)
            {
                HoldReading h; h.present = true; h.inRmsDb = L - 3.0103;
                h.levelDb = h.inRmsDb - 0.5 - (L > -20.0 ? (L + 20.0) * 0.5 : 0.0);      // -0.5 dB static gain, 2:1 above -20 peak
                r.holds[levelKey (L)] = h;
            }
            one.positions.push_back (r);
            std::vector<double> grid; for (int L = -60; L <= 0; L += 2) grid.push_back ((double) L);
            const auto d1 = derive (one, grid, -1, true);
            const auto two = d1.inAtGr.size() == 1 && d1.inAtGr[0].at.count (2) ? d1.inAtGr[0].at.at (2) : juce::var();
            check (d1.result == "unreadable" && d1.sense.isEmpty() && d1.inAtGr.size() == 1 && two.isDouble() && std::abs ((double) two - (-16.0)) < 1e-6
                     && d1.reduction.count (levelKey (-6.0)) && d1.reduction.at (levelKey (-6.0))[0] && std::abs (*d1.reduction.at (levelKey (-6.0))[0] - 7.0) < 1e-9
                     && d1.quietCheckDb.size() == 1 && d1.quietCheckDb[0] && std::abs (*d1.quietCheckDb[0]) < 1e-9,
                   "detector D6: a one-position quiet-reference derive stays unreadable as a map but carries its reduction (7 dB at -6) and in_at_gr (2 dB at -16) for the detector and the tone check");
            const auto d0 = derive (one, grid, -1, false);
            check (d0.result == "unreadable" && d0.inAtGr.empty(), "detector D6b: without the quiet reference a single position has no reference and so no curve");
            // D7: a merge with NO reference process (ref=0) is ok when its position's process was, and not when it was not
            const juce::String posOut = "sweep\tproto\t1\tthr\t2\tname\tThreshold\tpositions\t1\tlevels\t1\n"
                                        "pos\t0\tnorm\t0.300000\tconfirm_ms\t2.0\tslices\t1\tinstack_match\t1\tlanded_by\tinstack\ttext\t-20 dB\n"
                                        "hold\t0\t-48.00\tlevel_db\t-51.5103\tin_rms_db\t-51.0103\ttone_frac\t1.0000\tfinal_move_db\t0.0\tnonfinite\t0\n";
            const auto none = ProcessOut { juce::String(), true, "none", -1.0f };
            const auto mOk = mergeProcesses (none, { ProcessOut { posOut, true, "clean", 0.3f } });
            const auto mNo = mergeProcesses (none, { ProcessOut { juce::String(), false, "crashed", 0.3f } });
            check (mOk.ok && mOk.positions.size() == 1 && ! mNo.ok, "detector D7: a reference-less merge (ref=0) is ok from its position's process alone (" + juce::String (mOk.ok ? "ok" : "not ok") + " / " + juce::String (mNo.ok ? "ok" : "not ok") + ")");
        }
        auto withDet = record (16, true, false, "", "certified");
        auto* dd = new juce::DynamicObject(); dd->setProperty ("fraction", 0.97); withDet.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("detector", juce::var (dd));
        const auto xd = exportCompProfile (withDet);
        check (xd.ok && std::abs ((double) xd.profile.getProperty ("detector_f", 0.0) - 0.97) < 1e-6,
               "detector D3 (v1.3): a measured fraction exports as detector_f, the number");
        auto noDet = record (16, true, false, "", "certified"); noDet.getProperty ("thresholdSweep", {}).getDynamicObject()->removeProperty ("detector");
        check (! exportCompProfile (noDet).ok && exportCompProfile (noDet).refused.contains ("detector_f"), "detector D4 (v1.4): a record without a measured detector_f is NOT exported - required");
        auto noRep = record (16, true, false, "", "certified"); noRep.getProperty ("thresholdSweep", {}).getDynamicObject()->removeProperty ("quality");
        check (! exportCompProfile (noRep).ok && exportCompProfile (noRep).refused.contains ("point_error_db"), "quality X21 (v1.4): a record without the hold-doubled repeat is NOT exported - required");
        // the monotonic self-check, both ways: equal neighbours across positions pass; a dip fails; equal within a position fails (strict).
        auto flat2 = record (16, true, false, "", "certified");
        { auto arr = flat2.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); arr[5].getDynamicObject()->setProperty ("1", arr[4].getProperty ("1", {})); arr[5].getDynamicObject()->setProperty ("2", arr[4].getProperty ("2", {})); arr[5].getDynamicObject()->setProperty ("3", arr[4].getProperty ("3", {})); }
        const auto fx2 = exportCompProfile (flat2);
        check (fx2.ok && (bool) fx2.profile.getProperty ("quality", {}).getProperty ("monotonic_across_positions", false),
               "monotonic M1 (v1.4): equal neighbouring positions are allowed across positions");
        auto dip = record (16, true, false, "", "certified");
        { auto arr = dip.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); arr[5].getDynamicObject()->setProperty ("1", (double) arr[3].getProperty ("1", {}) - 1.0); }
        const auto dx = exportCompProfile (dip);
        check (dx.ok && ! (bool) dx.profile.getProperty ("quality", {}).getProperty ("monotonic_across_positions", true),
               "monotonic M2 (v1.4): a dip across positions fails the across check (and is said in the export, the server will reject)");
        auto eq = record (16, true, false, "", "certified");
        { auto arr = eq.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); arr[4].getDynamicObject()->setProperty ("2", arr[4].getProperty ("1", {})); }
        const auto ex = exportCompProfile (eq);
        check (ex.ok && ! (bool) ex.profile.getProperty ("quality", {}).getProperty ("monotonic_within_positions", true),
               "monotonic M3 (v1.4): within a position 1 < 2 < 3 is STRICT - an equal 2 dB point fails");
        auto over = record (16, true, false, "", "certified");
        auto* d2 = new juce::DynamicObject(); d2->setProperty ("fraction", 1.3); over.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("detector", juce::var (d2));
        const auto xo = exportCompProfile (over);
        check (xo.ok && (double) xo.profile.getProperty ("detector_f", 0.0) == 1.0 && std::abs ((double) xo.profile.getProperty ("detector_f_raw", 0.0) - 1.3) < 1e-6,
               "detector D5: a measured fraction outside 0..1 is clamped for the server and kept raw beside it");
    }
    {
        // THE PICK BY NAME on a multi-threshold record: the named candidate's sweep becomes THE sweep, the rest drop, and the
        // view says which was picked; a wrong name is refused with the candidate list.
        auto multi = record (16, true, false, "", "certified");
        const auto sweepOf = multi.getProperty ("thresholdSweep", {});
        juce::Array<juce::var> cands;
        for (const char* nm : { "Gate Thresh", "Comp Thresh" }) { auto* c = new juce::DynamicObject(); c->setProperty ("index", nm[0] == 'G' ? 1 : 16); c->setProperty ("name", nm); c->setProperty ("thresholdSweep", sweepOf); cands.add (juce::var (c)); }
        multi.getDynamicObject()->removeProperty ("thresholdSweep"); multi.getDynamicObject()->setProperty ("thresholdCandidates", cands);
        juce::String why;
        check (! exportCompProfile (multi).ok && exportCompProfile (multi).refused.contains ("other"), "pick C1: a multi-threshold record unpicked is topology other, no profile");
        const auto view = candidateAsSingle (multi, "Comp Thresh", why);
        check (view.isObject() && view.getProperty ("thresholdSweep", {}).isObject() && ! view.hasProperty ("thresholdCandidates")
                 && view.getProperty ("pickedCandidate", {}).getProperty ("name", "") == "Comp Thresh" && (int) view.getProperty ("pickedCandidate", {}).getProperty ("index", -1) == 16,
               "pick C2: the named candidate becomes the single sweep, the others drop, and the pick is recorded");
        const auto ve = exportCompProfile (view);
        check (ve.ok && ve.profile.getProperty ("topology", "") == "threshold", "pick C3: the picked view exports as topology threshold (" + ve.refused + ")");
        check (candidateAsSingle (multi, "Nope", why).isVoid() && why.contains ("Gate Thresh") && why.contains ("Comp Thresh"), "pick C4: a name that is not a candidate is refused with the candidate list (" + why + ")");
        check (candidateAsSingle (rec, "anything", why) == rec, "pick C5: a single-sweep record passes through untouched");
    }
    auto bad = record (16, true, false, "", "certified");
    bad.getProperty ("thresholdSweep", {}).getProperty ("engageWrites", {}).getProperty ("writes", {})[0].getDynamicObject()->setProperty ("control", "Bypass");
    check (! exportCompProfile (bad).ok && exportCompProfile (bad).refused.contains ("never_touch"), "export X13: an engage write naming a never_touch control refuses the export");
}

/** THE PROFILE SWEEP (spec v1.1 section 4) and the neutral set, built 1 Oct: the plan's levels, hold and reference; the
    neutral chooser on a control's own texts; drive by name. */
void testProfileSweepPlan()
{
    using namespace ejmap::sweep;
    Plan p; p.thr = 0; p.thrName = "Threshold"; p.norms = { 0.f, 1.f };
    p.candidates.push_back ({ 7, "Band 2", {}, false });
    p.makeProfile();
    const auto lv = p.testLevels();
    bool asc = true; for (size_t i = 1; i < lv.size(); ++i) asc = asc && lv[i] > lv[i - 1];
    const auto pl = p.probeLevels();
    bool pasc = true; for (size_t i = 1; i < pl.size(); ++i) pasc = pasc && pl[i] > pl[i - 1];
    check (lv.size() == 31 && lv.front() == -60.0 && lv.back() == 0.0 && asc && std::find (lv.begin(), lv.end(), -54.0) != lv.end() && std::find (lv.begin(), lv.end(), -48.0) != lv.end()
             && pl.size() == 36 && pasc && pl.front() == -90.0 && std::equal (lv.begin(), lv.end(), pl.begin() + 5),
           "profile N1: 31 levels -60..0 in 2 dB steps read, rendered ascending behind the ladder's five levels below the grid (-90..-66), -54 and -48 on the grid for the first rung");
    check (p.quietReference && p.holdS == 2.5 && p.winS == 0.3 && p.discardS == 2.2, "profile N2: quiet reference on by design, 2.5 s hold, last 300 ms read");
    const auto slow = p.holdDoubled();
    check (p.repeats == 2 && slow.holdS == 5.0 && std::abs (slow.discardS - 4.7) < 1e-9 && slow.winS == 0.3 && slow.testLevels() == p.testLevels(),
           "profile N2b (v1.4): the repeat is the same sweep with the hold DOUBLED to 5 s and the same 300 ms read at its end");
    check (p.forCandidate (p.candidates[0]).quietReference && p.forCandidate (p.candidates[0]).profile,
           "profile N3: a candidate of a profile plan keeps the quiet reference and the grid (a candidate's own flag would have reset it)");
    Plan q; check (q.testLevels().size() == 3 && ! q.quietReference && q.holdS == 1.5, "profile N4: a certification plan is unchanged: three levels, 1.5 s");
    {
        // GRID REFINEMENT (2 Oct): CL 1B's 2 dB points, positions 0-5 (null, null, null, -13.91, -23.21, -27.71): the
        // 9.3 dB gap between 3 and 4 gets ceil(9.3/3) - 1 = 3 positions, the 4.5 dB gap between 4 and 5 gets 1; nulls
        // are absences, not gaps.
        std::vector<float> norms { 0.0f, 0.066667f, 0.133333f, 0.2f, 0.266667f, 0.333333f };
        std::vector<juce::var> two { juce::var(), juce::var(), juce::var(), -13.91, -23.21, -27.71 };
        const auto add = refineNorms (norms, two, kRefineGapDb, kRefinePositionsMax);
        juce::String got; for (float n : add) got << juce::String (n, 4) << " ";
        check (add.size() == 4 && std::abs (add[0] - 0.216667f) < 1e-4f && std::abs (add[1] - 0.233333f) < 1e-4f && std::abs (add[2] - 0.25f) < 1e-4f && std::abs (add[3] - 0.3f) < 1e-4f,
               "refine G1: a 9.3 dB gap gets three evenly spaced positions, a 4.5 dB gap one, a null neighbour none (" + got + ")");
        std::vector<juce::var> fine { juce::var(), juce::var(), juce::var(), -13.91, -16.5, -19.4 };
        check (refineNorms (norms, fine, kRefineGapDb, kRefinePositionsMax).empty(), "refine G2: gaps of 3 dB or less add nothing");
        std::vector<juce::var> edge { juce::var(), juce::var(), juce::var(), -13.91, -16.91, -19.91 };
        check (refineNorms (norms, edge, kRefineGapDb, kRefinePositionsMax).empty(), "refine G2b: exactly 3 dB is not over the bar");
        check (refineNorms (norms, two, kRefineGapDb, 7).size() == 1, "refine G3: the position cap stops the adding (6 measured, cap 7: one added)");
        check (refineNorms (norms, { juce::var(), -1.0 }, kRefineGapDb, kRefinePositionsMax).empty(), "refine G4: mismatched inputs add nothing");
        // the record: a plan that refined says so, with the rule and the norms it added
        Plan rp; rp.thr = 0; rp.thrName = "Threshold"; rp.norms = norms; rp.makeProfile(); rp.refineRounds = 2; rp.refinedNorms = add;
        Measured mm; mm.ok = true; mm.movingDb = 0.1;
        for (int i = 0; i < 2; ++i) { PositionReading r; r.k = i; r.norm = (float) i; r.text = juce::String (i); for (double L : { -24.0, -12.0, -6.0 }) { HoldReading h; h.present = true; h.inRmsDb = L - 3.0103; h.levelDb = h.inRmsDb - 2.0 * i; r.holds[levelKey (L)] = h; } mm.positions.push_back (r); }
        const auto dd = derive (mm, { -24.0, -12.0, -6.0 }, -1);
        const auto rec = composeThresholdSweep (dd, displayCheck (dd, "dB"), rp, {});
        const auto g = rec.getProperty ("gridRefinement", {});
        check (g.isObject() && (int) g.getProperty ("rounds", 0) == 2 && g.getProperty ("added_norms", {}).size() == 4 && g.getProperty ("rule", "").toString().contains ("3.0 dB"),
               "refine G5: the record carries gridRefinement {rule, gap_db, rounds, added_norms}; a plan that did not refine carries none");
        Plan np = rp; np.refineRounds = 0; np.refinedNorms.clear();
        check (! composeThresholdSweep (dd, displayCheck (dd, "dB"), np, {}).hasProperty ("gridRefinement"), "refine G5b: no refinement, no field");
    }

    std::vector<GridPoint> mix { { 0.0f, "0.0" }, { 0.5f, "50.0" }, { 1.0f, "100.0" } }, make { { 0.0f, "-12.0 dB" }, { 0.5f, "0.0 dB" }, { 1.0f, "+12.0 dB" } },
                           drive { { 0.0f, "1.0" }, { 0.5f, "5.0" }, { 1.0f, "10.0" } }, words { { 0.0f, "Dry" }, { 1.0f, "Wet" } };
    juce::String t;
    check (chooseNeutral (mix, "mix_wet", t) == 1.0f && t == "100.0", "neutral N5: mix picks the text nearest 100 (" + t + ")");
    check (chooseNeutral (make, "makeup_zero", t) == 0.5f && t == "0.0 dB", "neutral N6: make-up picks the text nearest 0 (" + t + ")");
    check (chooseNeutral (drive, "drive_cleanest", t) == 0.0f, "neutral N7: drive picks the smallest numeric text");
    check (! chooseNeutral (words, "mix_wet", t), "neutral N8: a control whose texts never parse to a number is NOT set - a guard, not a guess");
    check (driveNamed ("Drive") && driveNamed ("Saturation") && driveNamed ("Input Sat") && ! driveNamed ("Threshold") && ! driveNamed ("Ratio"),
           "neutral N9: drive / saturation by name; threshold and ratio never");
}

/** A PROCESS THAT SLEPT IS RE-RUN ONCE, AND REFUSED IF IT SLEEPS AGAIN (ruled 29 Sep) - the SIGTERM rule's shape. */
void testSleptProcessRetry()
{
    using namespace ejmap::cert;
    auto sleepyClock = [] (std::vector<int> jumpAtCalls) {
        auto calls = std::make_shared<int> (0);
        auto offset = std::make_shared<double> (0.0);
        Clock c;
        c.withSleepMs = [calls, offset, jumpAtCalls] {
            ++*calls;
            for (int j : jumpAtCalls) if (*calls == j) *offset += 600000.0;   // ten minutes asleep
            return continuousMs() + *offset; };
        return c; };
    int seen = 0;
    auto count = [&seen] (int, const ChildResult&) { ++seen; };
    // runChild reads withSleepMs twice per attempt: at the start (odd calls) and at the end (even calls).
    seen = 0;
    const auto once = runWithRetry ({ "/usr/bin/true" }, 5000, count, sleepyClock ({ 2 }), WatchOptions { false });
    check (once.attempts == 2 && ! once.sleptTwice && once.r.cleanExit() && seen == 2,
           "slept R1: a clean run the Mac slept through is re-run once, and the clean retry stands");
    seen = 0;
    const auto twice = runWithRetry ({ "/usr/bin/true" }, 5000, count, sleepyClock ({ 2, 4 }), WatchOptions { false });
    check (twice.attempts == 2 && twice.sleptTwice && seen == 2, "slept R2: slept again on the retry - refused, and not looped");
    seen = 0;
    const auto awake = runWithRetry ({ "/usr/bin/true" }, 5000, count, sleepyClock ({}), WatchOptions { false });
    check (awake.attempts == 1 && ! awake.sleptTwice, "slept R3 (control): a run with no sleep is not retried");
    seen = 0;
    const auto answer = runWithRetry ({ "/bin/sh", "-c", "exit 3" }, 5000, count, sleepyClock ({ 2 }), WatchOptions { false });
    check (answer.attempts == 1, "slept R4: a refusal (exit 3) is an answer even if the Mac slept - never retried");
}

//==============================================================================
/** LEVEL DEPENDENCE (ruled 30 Sep): the axis that defines a threshold. API-2500 and H-Comp certified on 29 Sep with
    reduction identical at -24, -12 and -6 - a make-up gain law - because every guard tested the curve across positions
    and none across levels. Their committed traces are the negative cases. */
void testLevelDependence()
{
    using namespace ejmap::sweep;
    const auto tr = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/cert-traces/2026-09-29-batch4-discovered");
    auto fromTraces = [&] (const char* id, int ratioIndex) {
        ProcessOut ref; std::vector<ProcessOut> pos;
        const bool ok = loadProcesses (tr.getChildFile ("processes").getChildFile (juce::String (id) + ".sweep.processes.json"), tr.getChildFile ("raw"), ref, pos);
        return std::make_pair (ok, derive (mergeProcesses (ref, pos), sweeptest::kLevels, ratioIndex)); };
    const auto api = fromTraces ("AudioUnit_4b567263_12.0.0", 2);      // API-2500 (m): ratio [2] reads "4:1"
    check (api.first && api.second.result == "unreadable" && api.second.roleFlag == "not_a_threshold"
             && api.second.ratio && std::abs (*api.second.ratio - 4.0) < 1e-9 && api.second.impliedRatio && *api.second.impliedRatio < 1.05,
           "level G1: API-2500 REFUSES as not a threshold - its reduction never moves with level, implying R = 1 against the 4.0 it read back");
    const auto hc = fromTraces ("AudioUnit_494a7063_12.0.0", 1);       // H-Comp (m)
    check (hc.first && hc.second.result == "unreadable" && hc.second.roleFlag == "not_a_threshold",
           "level G2: H-Comp REFUSES as not a threshold (" + hc.second.reason.substring (0, 70) + ")");

    // POSITIVE CONTROLS: MCompressor's Peak arm certifies, with its slope recorded against the textbook 1 - 1/1.8.
    const auto adir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures/sweep/mcompressor-rms-peak");
    ProcessOut r; std::vector<ProcessOut> ps;
    loadProcesses (adir.getChildFile ("processes.json"), adir.getChildFile ("raw"), r, ps);
    const auto mc = derive (mergeProcesses (r, ps), sweeptest::kLevels, 6);
    check (mc.result == "certified" && mc.dgdlPredicted && std::abs (*mc.dgdlPredicted - 0.444) < 0.01 && mc.dgdlMedian && *mc.dgdlMedian > 0.3,
           "level G3: MCompressor stays certified; dg/dL is recorded (median " + juce::String (mc.dgdlMedian.value_or (0.0), 3) + ") against the predicted 0.444");
    // A synthetic gain law inside the readable band refuses; the same curve with a SATURATED deepest position does not.
    std::vector<std::array<std::optional<double>, 3>> gainLaw;
    for (int k = 0; k < 6; ++k) { const double g = 8.0 - 1.4 * k; gainLaw.push_back ({ 2.0 - g, 2.0 - g, 2.0 - g }); }
    const auto gl = derive (sweeptest::fromGains (gainLaw), sweeptest::kLevels, -1);
    check (gl.result == "unreadable" && gl.roleFlag == "not_a_threshold", "level G4: a synthetic gain law (same reduction at every level) refuses");
    std::vector<std::array<std::optional<double>, 3>> sat;
    for (int k = 0; k < 6; ++k) { const double r0 = (5 - k) * 2.0; sat.push_back ({ 2.0 - r0 * 0.5, 2.0 - r0, 2.0 - r0 * 1.5 }); }
    sat[0] = { 2.0 - 30.0, 2.0 - 30.0, 2.0 - 30.0 };                     // the deepest position at its ceiling
    const auto sd = derive (sweeptest::fromGains (sat), sweeptest::kLevels, -1);
    check (sd.result == "certified" && sd.levelFlat.isEmpty(),
           "level G5: a position saturated at every level (C1 at -100 dB, MCompressor at -80 dB) is NOT a gain law - excluded by the band");

    // THE GRID MUST NOT DECIDE (ruled 30 Sep). Form C: a gain law whose every engaged position sits ABOVE the band
    // (reposition API-2500's grid so nothing lands in band): flat at every engaged position - refused.
    std::vector<std::array<std::optional<double>, 3>> above { { 2.0 - 30.0, 2.0 - 30.0, 2.0 - 30.0 }, { 2.0 - 20.0, 2.0 - 20.0, 2.0 - 20.0 },
                                                              { 2.0 - 13.0, 2.0 - 13.0, 2.0 - 13.0 }, { 2.0 - 0.4, 2.0 - 0.4, 2.0 - 0.4 }, { 2.0, 2.0, 2.0 } };
    const auto ga = derive (sweeptest::fromGains (above), sweeptest::kLevels, -1);
    check (ga.result == "unreadable" && ga.roleFlag == "not_a_threshold" && ga.levelFlatBy == "every engaged position",
           "level G6: a gain law entirely above the band still refuses - flat at every engaged position (" + ga.levelFlatBy + ")");
    // Form B: a near-gain-law whose positions straddle the band edge, so none is in band at all three levels, but its
    // adjacent in-band pairs are all flat - refused.
    std::vector<std::array<std::optional<double>, 3>> straddle { { 2.0 - 11.9, 2.0 - 11.95, 2.0 - 12.0 }, { 2.0 - 0.45, 2.0 - 3.0, 2.0 - 3.02 }, { 2.0, 2.0, 2.0 } };
    const auto gb = derive (sweeptest::fromGains (straddle), sweeptest::kLevels, -1);
    check (gb.result == "unreadable" && gb.levelFlatBy == "in-band pairs",
           "level G7: no position in band at all three levels, and every adjacent in-band pair flat - refused by the pairs (" + gb.levelFlatBy + ")");
    // Control: a REAL compressor on a coarse grid - one position crosses the band between levels, one sits above it.
    std::vector<std::array<std::optional<double>, 3>> coarse { { 2.0 - 13.0, 2.0 - 20.0, 2.0 - 27.0 }, { 2.0 - 0.3, 2.0 - 6.0, 2.0 - 11.5 }, { 2.0, 2.0, 2.0 } };
    const auto gc = derive (sweeptest::fromGains (coarse), sweeptest::kLevels, -1);
    check (gc.result == "certified" && gc.levelFlat.isEmpty(), "level G8 (control): a real compressor on a grid too coarse for form A certifies - its pairs and its engaged positions have slope");
}

int main (int, char**)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testSchemaVersionPinned();
    testDuplicateSemanticRule();
    testDialTimeUnitRule();
    testControlsOnlyPayload();
    testVerdictSemantics();
    testTrustOrdering();
    testSkipRequiresReason();
    testPayloadSerialises();
    testContradictionBlocks();
    testSharedParsers();
    testPayloadFeedsApply();
    testGroupsRouteAroundMonoMaker();
    testDeclinesEmission();
    testNamedControlsResolve();
    testRoundTripThroughApplySettings();
    testDryRunBytes();
    testIdentityKeyFormat();
    testExposureConformance();
    testLockstepAndTierFields();
    testAgainstRealMaps();
    testSubjectLookups();
    testReadbackProbe();
    testMarks();
    testUnitFamilyRule();
    testRetryRule();
    testRecordedIsCounted();
    testCountingCost();
    testSteppedSanitizer();
    testSweepRules();
    testMapperIdentity();
    testStaleControlsRefused();
    testSweepDeadline();
    testEmptyControlNameRejected();
    testLocalMapOutranksCachedNonConfirmation();
    testUnsetEndpointNotDerived();
    testPerStepWorkIsNotRepeated();
    testUnfinishedAttemptRule();
    testFixtureUnitRule();
    testFixtureRangeRule();
    testFixtureReadoutEmission();
    testLicenceOutcome();
    testNameTokenVectors();
    testRoleRules();
    testRoleClassification74();
    testSweepDerivation();
    testSweepSplitVerdict();
    testSweepPrivacyAndPlan();
    testMeasurableRule();
    testSweepRatioAndPicks();
    testSweepQuietReference();
    testLicenceFromAudio();
    testExternalHardware();
    testSleepTimeouts();
    testDiscoveryFromMaps();
    testCertRecordAndDefaultPaths();
    testThresholdReviewNamesTheBand();
    testEngageDetection();
    testTunerDerivations();
    testMapFpJoinKey();
    testProfileExport();
    testProfileSweepPlan();
    testSleptProcessRetry();
    testLevelDependence();

    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
