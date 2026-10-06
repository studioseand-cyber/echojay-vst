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
#include "EjmapLoop.h"
#include "EjmapCandidateRules.h"
#include "EjmapWindowWatch.h"
#include "EjmapCategoriesMerge.h"
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
#include "EjmapSidechainCheck.h"
#include "EjmapPitch.h"
#include "EjmapTunerProfile.h"
#include "EjmapGainCal.h"
#include "EjmapTiming.h"
#include "EjmapLimiter.h"
#include "EjmapEq.h"
#include <functional>
#include "EjmapCertDriver.h"
#include "EjmapCertReview.h"
#include "EjmapSaturation.h"
#include "EjmapReverbDelay.h"
#include "EjmapDynamics.h"
#include "EjmapDeesser.h"
#include "EjmapMultiband.h"
#include "EjmapRoleEvidence.h"
#include "EjmapPhaseB.h"

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
    // PROGRESS IS WHAT IS ON DISK (2 Oct): a scan's probed bundles and its quarantined stalls count too
    auto root2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ejmap-scan-test-" + juce::Uuid().toDashedString());
    root2.createDirectory();
    check (ejmap::progressCount (root2) == -1, "supervisor P1: nothing on disk is no progress, not zero progress");
    root2.getChildFile ("scan-progress.jsonl").replaceWithText ("{\"a\":1}\n{\"b\":2}\n{\"c\":3}\n");
    const int p0 = ejmap::progressCount (root2);
    root2.getChildFile ("quarantine.json").replaceWithText ("[{\"plugin_id\": \"/x/ANA2.vst3\", \"reason\": \"hang_in_findAllTypesForFile\"}]");
    const int p1 = ejmap::progressCount (root2);
    check (p0 == 3 && p1 == 4, "supervisor P2: a scan that probed three bundles and then quarantined a stalling one made progress (3 -> 4): the restart is not charged (" + juce::String (p0) + " -> " + juce::String (p1) + ")");
    ejmap::sweepProgressMarker (root2).replaceWithText ("2");
    check (ejmap::progressCount (root2) == 6, "supervisor P3: maps, bundles probed and quarantines add up (2 + 3 + 1)");
    root2.getChildFile ("licence-stops.json").replaceWithText ("[{\"plugin_id\": \"/x/Drumstrip.vst3\", \"state\": \"needs_licence\"}]");
    check (ejmap::progressCount (root2) == 7, "supervisor P4: a licence stop is the job done too (2 + 3 + 1 + 1): the relaunch after it is not charged");
    root2.deleteRecursively();

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

    // 7. THE INSTANTIATE POINT FOLDS IN, AND range_partial NAMES THE GAP (ruled 4 Oct): CL 1B Gain "Off / 8.5 / 31.0", instantiate "0.0" at 0.33
    d = derive ("Off", "8.5", "31.0", 0.01, "0.0", 0.33);
    check (num (d.range, "min") == 0.0 && num (d.range, "max") == 31.0 && has (d.range, "at_instantiate") && num (d.range.getProperty ("at_instantiate", {}), "value") == 0.0
             && std::abs (num (d.range.getProperty ("at_instantiate", {}), "norm") - 0.33) < 1e-9 && has (d.range, "endsNotNumeric")
             && d.range.getProperty ("range_partial", "").toString() == "the instantiate point lies outside the sampled ends; an end prints a word",
           "range R7: CL 1B Gain's range includes the instantiate value 0.0 (min 0, max 31), records at_instantiate {0.33, 0.0}, and range_partial names both gaps (" + d.range.getProperty ("range_partial", "").toString() + ")");
    d = derive ("-24.0", "-12.0", "0.0", 0.01, "-18.0", 0.25);
    check (num (d.range, "min") == -24.0 && num (d.range, "max") == 0.0 && ! has (d.range, "range_partial") && has (d.range, "at_instantiate"),
           "range R7b: an instantiate point inside the sampled ends changes nothing but is recorded; no range_partial");
    d = derive ("", "-12.0", "0.0", 0.01, "-6.0", 0.75);
    check (d.range.getProperty ("range_partial", "").toString() == "an end sample is missing; an end prints a word" && num (d.range, "min") == -12.0,
           "range R7c: an empty end sample is 'an end sample is missing' (and, unparsed, also a word end); the range borrows the middle as before");
    check (! has (derive ("Off", "8.5", "31.0").range, "at_instantiate") && num (derive ("Off", "8.5", "31.0").range, "min") == 8.5,
           "range R7d: without an instantiate text the rule is exactly what it was (the 1,783-control reproduction stands)");
    // 9. STEPPED BY EVIDENCE (ruled 4 Oct, Lindell 254E's numbers): writes at 41 norms land only on k/15 -> 16 detents; on-grid writes alone prove nothing
    {
        using ejmap::profile::LandingRow; using ejmap::profile::detentsFromLanding; using ejmap::profile::positionsAreDetents;
        std::vector<LandingRow> rows; for (int k = 0; k <= 40; ++k) { LandingRow r; r.norm = k / 40.0; r.getValue = std::round (r.norm * 15.0) / 15.0; rows.push_back (r); }   // 254E: 0.534581 -> 0.533333, 0.854582 -> 0.866667
        const auto n = detentsFromLanding (rows);
        check (n && *n == 16, "range R9 (254E): writes at k/40 landing on k/15 are 16 detents by evidence (" + juce::String (n ? *n : 0) + ")");
        std::vector<LandingRow> onGrid; for (int k = 0; k <= 15; ++k) { LandingRow r; r.norm = k / 15.0; r.getValue = r.norm; onGrid.push_back (r); }
        check (! detentsFromLanding (onGrid), "range R9b: 16 writes that each land exactly where written are NOT evidence of stepping (254E's own sweep sat on its detents)");
        std::vector<LandingRow> cont; for (int k = 0; k <= 40; ++k) { LandingRow r; r.norm = k / 40.0; r.getValue = r.norm + (k % 3 == 0 ? 0.00003 : 0.0); cont.push_back (r); }
        check (! detentsFromLanding (cont), "range R9c: writes landing within 1e-4 of where written are continuous");
        std::vector<LandingRow> coarse; for (int k = 0; k <= 40; ++k) { LandingRow r; r.norm = k / 40.0; r.getValue = std::round (r.norm * 2.0) / 2.0; coarse.push_back (r); }
        check (detentsFromLanding (coarse) && *detentsFromLanding (coarse) == 3, "range R9d: landing only on 0 / 0.5 / 1 is 3 detents");
        juce::Array<juce::var> cv; for (int k = 0; k < 16; ++k) { auto* o = new juce::DynamicObject(); o->setProperty ("norm", k / 15.0); cv.add (juce::var (o)); }
        juce::Array<juce::var> cv15 = cv; cv15.remove (3);
        check (positionsAreDetents (juce::var (cv), 16) && ! positionsAreDetents (juce::var (cv15), 16), "range R9e: the swept positions must be exactly the detents for the export to mark the control stepped");
    }
    // 8. THE RE-SAMPLE FOLD (ruled 4 Oct): CL 1B's Gain read at 21 norms plus its instantiate norm - 0.33 reads "0.0", the range includes it, "Off" is a named position
    {
        const auto gain = juce::JSON::parse (R"json({"index": 0, "name": "Gain", "range": {"min": 8.5, "max": 31.0, "endsNotNumeric": ["0.000"]}, "defaultOnInstantiate": {"normalised": 0.33, "display": "0.0"}})json");
        std::vector<ejmap::profile::ResampleRow> rows;
        for (double n : ejmap::profile::resampleNorms()) { ejmap::profile::ResampleRow r; r.norm = n; r.text = n < 0.05 ? "Off" : juce::String (n < 0.33 ? -10.0 + 30.0 * n : (n - 0.33) / 0.67 * 31.0, 1); r.value = n < 0.05 ? std::nullopt : std::optional<double> (r.text.getDoubleValue()); rows.push_back (r); }
        { ejmap::profile::ResampleRow r; r.norm = 0.33; r.text = "0.0"; r.value = 0.0; rows.push_back (r); }
        const auto f = ejmap::profile::foldResample (gain, rows); const auto rr = f.getProperty ("range_resampled", {});
        bool at33 = false; for (const auto& x : *f.getProperty ("samples", {}).getArray()) if (std::abs ((double) x.getProperty ("norm", 0.0) - 0.33) < 1e-9 && x.getProperty ("text", "").toString() == "0.0") at33 = true;
        check (at33 && num (rr, "min") <= 0.0 && num (rr, "max") == 31.0 && (int) rr.getProperty ("numeric_samples", 0) == 21 && (int) rr.getProperty ("of", 0) == 22
                 && rr.getProperty ("named_positions", {}).size() == 1 && rr.getProperty ("named_positions", {})[0].getProperty ("text", "") == "Off" && ejmap::profile::resampleNorms().size() == 21,
               "range R8: the re-sample reads 0.33 as '0.0', the corrected range includes 0.0 (min " + juce::String (num (rr, "min"), 2) + ", max 31), 21 of 22 samples numeric, 'Off' kept as a named position at norm 0");
    }
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
                // DEEP POINTS (v1.7, Q7-Q10): targets 1..6 from one constant; the trust gate stays 1/2/3; a deep point whose repeat
                // differs by more than 0.5 dB is null and listed; deep_point_error_db is the worst over the SURVIVING deep points.
                check (kGrTargetMax == 12 && kGrTargets == std::vector<int> { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 } && kTrustTargets == std::vector<int> { 1, 2, 3 } && kDeepFrom == 4 && std::abs (kDeepHoldTolDb - 0.5) < 1e-9,
                       "quality Q7 (v1.10): the targets are 1..12 generated from kGrTargetMax, the trust gate 1/2/3 unchanged, deep from 4, the deep hold tolerance 0.5 dB");
                Derived dp = tx; for (auto& r : dp.inAtGr) if (r.at.count (3) && r.at.at (3).isDouble()) { const double v3 = (double) r.at.at (3); r.at[4] = v3 + 1.5; r.at[5] = v3 + 3.0; r.at[6] = v3 + 4.5; }
                Derived dq = dp; bool bumped = false; int bumpedPos = -1;
                for (size_t i = 0; i < dq.inAtGr.size() && ! bumped; ++i) if (dq.inAtGr[i].at.count (5) && dq.inAtGr[i].at.at (5).isDouble()) { dq.inAtGr[i].at[5] = (double) dq.inAtGr[i].at.at (5) + 0.8; bumped = true; bumpedPos = (int) i; }
                for (auto& r : dq.inAtGr) if (r.at.count (4) && r.at.at (4).isDouble()) r.at[4] = (double) r.at.at (4) + 0.2;      // the 4 dB points move 0.2: survive, and set the figure
                const auto dqq = repeatQuality (dp, &dq);
                const auto v5first = (double) dp.inAtGr[(size_t) juce::jmax (0, bumpedPos)].at.at (5), v5again = v5first + 0.8;
                check (bumped && dqq.deepNullSet.count ({ bumpedPos, 5 }) == 1 && dqq.deepNulled.size() == 1 && dqq.deepNulled[0].startsWith (juce::String (bumpedPos) + "@5")
                         && dqq.deepNulled[0].contains (juce::String (v5first, 2) + " / hold-doubled " + juce::String (v5again, 2) + " / delta 0.80 dB")   // BOTH values, for the notes (v1.8)
                         && dqq.deepPointErrorDb && std::abs (*dqq.deepPointErrorDb - 0.2) < 0.01 && dqq.pointErrorDb && std::abs (*dqq.pointErrorDb) < 1e-9,
                       "quality Q8: a 5 dB point whose repeat moved 0.8 dB is nulled and listed; deep_point_error_db is the worst SURVIVING deep disagreement (0.20); point_error_db (1/2/3) untouched at 0");
                // Q8a (ruled 3 Oct): the DERIVATION keeps the crossing unrounded - a derived record carries values off the 0.1 dB grid (a 0.1 rounding at the straddle is the old bug)
                {
                    int off = 0, n = 0; juce::String sample;
                    for (const auto& r : tx.inAtGr) for (const auto& [t, v] : r.at) if (v.isDouble()) { ++n; const double x = (double) v * 10.0; if (std::abs (x - std::round (x)) > 1e-6) { ++off; if (sample.isEmpty()) sample = juce::String ((double) v, 4); } }
                    check (n > 0 && off > 0, "quality Q8a: the derived in_at_gr crossings are unrounded (" + juce::String (off) + " of " + juce::String (n) + " off the 0.1 dB grid, e.g. " + sample + "); the export rounds, the hold test does not");
                }
                // THE HOLD GATE ON RAW VALUES (Q8b-Q8c, ruled 3 Oct): a raw delta of 0.46 passes and 0.54 fails; a rounding to 0.1 dB before
                // the compare would call both 0.5 (a = -20.06 -> -20.1; b = -20.52 / -20.60 -> -20.5 / -20.6) and pass the 0.54 - the mutant
                {
                    Derived h1 = dp, h2 = dp; int hp = -1;
                    for (size_t i = 0; i < h1.inAtGr.size() && hp < 0; ++i) if (h1.inAtGr[i].at.count (5) && h1.inAtGr[i].at.at (5).isDouble()) hp = (int) i;
                    h1.inAtGr[(size_t) hp].at[5] = -20.06; h2.inAtGr[(size_t) hp].at[5] = -20.52;
                    const auto qa = repeatQuality (h1, &h2);
                    h2.inAtGr[(size_t) hp].at[5] = -20.60;
                    const auto qb = repeatQuality (h1, &h2);
                    check (hp >= 0 && qa.deepNullSet.count ({ hp, 5 }) == 0 && qa.deepPointErrorDb && std::abs (*qa.deepPointErrorDb - 0.46) < 1e-6 && qb.deepNullSet.count ({ hp, 5 }) == 1,
                           "quality Q8b: the deep hold gate compares RAW crossings - a 0.46 dB delta passes (and is the figure, 0.46), a 0.54 fails; rounding to 0.1 before the compare would pass the 0.54");
                    Derived s1 = tx, s2 = tx; int sp = -1;
                    for (size_t i = 0; i < s1.inAtGr.size() && sp < 0; ++i) if (s1.inAtGr[i].at.count (2) && s1.inAtGr[i].at.at (2).isDouble()) sp = (int) i;
                    s1.inAtGr[(size_t) sp].at[2] = -20.06; s2.inAtGr[(size_t) sp].at[2] = -20.60;
                    const auto qs = repeatQuality (s1, &s2);
                    check (sp >= 0 && qs.pointErrorDb && std::abs (*qs.pointErrorDb - 0.54) < 1e-6, "quality Q8c: the shallow point_error_db is the raw worst too (0.54 here, not 0.5)");
                }
                // the record: the nulled deep point is a gap in inAtGr itself, and the quality carries the deep figures
                Plan pq; pq.thr = 0; pq.thrName = "Threshold"; pq.norms.resize (dp.inAtGr.size(), 0.5f); pq.makeProfile();
                auto sv = composeThresholdSweep (dp, displayCheck (dp, "dB"), pq, {});
                attachRepeatQuality (sv, dp, &dq);
                const auto nulled = sv.getProperty ("inAtGr", {})[bumpedPos].getProperty ("5", {});
                check (nulled.isVoid() && sv.getProperty ("inAtGr", {})[bumpedPos].getProperty ("4", {}).isDouble() && std::abs ((double) sv.getProperty ("quality", {}).getProperty ("deep_point_error_db", -1.0) - 0.2) < 0.01
                         && sv.getProperty ("quality", {}).getProperty ("deepPointsNulled", {}).size() == 1,
                       "quality Q9: the record's inAtGr carries the nulled deep point as null (its 4 dB neighbour stays), quality.deep_point_error_db and deepPointsNulled beside it");
                Derived deepBent = dp; for (auto& r : deepBent.inAtGr) if (r.at.count (6) && r.at.at (6).isDouble()) { r.at[6] = (double) r.at.at (5) - 0.5; break; }
                check (! repeatQuality (deepBent, nullptr).withinMonotonic, "quality Q10: the within-position check runs over 1..6 - a 6 dB point below its 5 dB point is a violation");
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
/** THE MEASURED CANDIDATE RULES (ruled 4 Oct, from Sean's 3 Oct run; EjmapCandidateRules.h). Facts shaped on the traces. */
void testCandidateRules()
{
    using namespace ejmap::candidaterules;
    auto cand = [] (int idx, const juce::String& name, bool cert, std::map<double, double> curve, std::optional<double> ch, int n) { CandidateFacts f; f.index = idx; f.name = name; f.certified = cert; f.curve2 = curve; f.channelWorstDb = ch; f.channelReadings = n; return f; };
    std::map<double, double> cv; for (int i = 0; i < 17; ++i) cv[i / 16.0] = -40.0 + 2.0 * i;
    auto shifted = cv; for (auto& [n, v] : shifted) v += 0.6;
    // P1: Vertigo VSC-2's shape - Threshold A / B, both certified, curves equal, channels 0.00 -> linked pair, A picked, B the twin
    const auto vsc = decide ({ cand (4, "Threshold A", true, cv, 0.0, 665), cand (11, "Threshold B", true, cv, 0.0, 665) });
    check (vsc.decided && vsc.rule == "linked_pair" && vsc.pick == 4 && vsc.others == juce::StringArray { "Threshold B" } && vsc.note.contains ("measured, not by label") && vsc.note.contains ("0.00 dB over 17 common positions"),
           "pair P1 (Vertigo VSC-2): a channel-suffixed pair, both certified, curves equal, channels together -> linked_pair, the first is the amount, the twin named (" + vsc.note + ")");
    // P2 (ruled 6 Oct - dbx-160 (s), Mixland Vac Attack, MAGNUM-K): the same names but the first's channels differ in its own sweep ->
    // the channels are independent: a DUAL-MONO PAIR, both certified - both thresholds written together, gated on the worse channel
    const auto indep = decide ({ cand (4, "Threshold A", true, cv, 3.2, 665), cand (11, "Threshold B", true, cv, 0.0, 665) });
    check (indep.decided && indep.rule == "dual_mono_pair" && indep.pick == 4 && indep.pairWith == 11 && indep.pairWithName == "Threshold B" && indep.note.contains ("differ by up to 3.20 dB") && indep.note.contains ("written WITH it"),
           "pair P2 (dual-mono): a pair whose first candidate's own sweep shows the channels apart, both certified -> dual_mono_pair, the twin written with the amount (" + indep.note + ")");
    const auto indepTwinFlat = decide ({ cand (4, "Threshold A", true, cv, 3.2, 665), cand (11, "Threshold B", false, {}, 0.0, 665) });
    check (! indepTwinFlat.decided && indepTwinFlat.whyNot.contains ("channels are independent") && indepTwinFlat.whyNot.contains ("twin did not certify"), "pair P2b: independent channels with a twin that did not certify: no rule (" + indepTwinFlat.whyNot + ")");
    // P2c: the pair RE-SWEEP - the first's sweep wrote the twin with it, the channels now agree because of that write: still dual_mono_pair
    { auto a = cand (4, "Threshold A", true, cv, 0.05, 665); a.pairWritten = 11; a.pairWrittenName = "Threshold B";
      const auto re = decide ({ a, cand (11, "Threshold B", true, cv, 0.0, 665) });
      check (re.decided && re.rule == "dual_mono_pair" && re.pairWith == 11 && re.note.contains ("re-swept with"), "pair P2c: a pair re-sweep (twin written with the first) stays dual_mono_pair, never linked_pair (" + re.note + ")"); }
    // P3: curves differ by 0.6 dB -> no rule; P3b: no per-channel readings -> no rule (never assumed)
    const auto diff = decide ({ cand (4, "Threshold A", true, cv, 0.0, 665), cand (11, "Threshold B", true, shifted, 0.0, 665) });
    check (! diff.decided && diff.whyNot.contains ("differ by up to 0.60 dB"), "pair P3: curves 0.6 dB apart are not a linked pair (" + diff.whyNot + ")");
    check (! decide ({ cand (4, "Threshold A", true, cv, std::nullopt, 0), cand (11, "Threshold B", true, cv, 0.0, 665) }).decided, "pair P3b: without per-channel readings in the first's own sweep nothing is decided - a rule never guesses");
    // P4: PuigChild 670 (s) - Left certified with channels together, Right flat -> leader_follower
    const auto puig = decide ({ cand (4, "Left Threshold", true, cv, 0.0, 1330), cand (8, "Right Threshold", false, {}, 0.0, 1330) });
    check (puig.decided && puig.rule == "leader_follower" && puig.pick == 4 && puig.others == juce::StringArray { "Right Threshold" } && puig.note.contains ("did not certify (it follows the leader)"),
           "pair P4 (PuigChild 670 (s)): the first certifies with its channels together, the second does not -> leader_follower, the first is the amount");
    const auto rev = decide ({ cand (4, "Left Threshold", false, cv, 0.0, 10), cand (8, "Right Threshold", true, cv, 0.0, 10) });
    check (! rev.decided && rev.whyNot.contains ("the first candidate did not certify"), "pair P4b: the FIRST must be the one that certifies; a certified second alone decides nothing, and the row says why (" + rev.whyNot + ")");
    // P5: the channel tokens, literal: ""/" R" (RS124), L/M vs R/S (DPR-402), prefix L/R (UnFairchild), Left/Right (VT-7); not Low/High (kHs), not Optical 1 / Discrete 1
    check (channelPairBase ("Input Control", "Input Control R") == "Input Control" && channelPairBase ("Threshold L/M", "Threshold R/S") == "Threshold" && channelPairBase ("L Threshold", "R Threshold") == "Threshold"
             && channelPairBase ("Left Threshold", "Right Threshold") == "Threshold" && channelPairBase ("Threshold 1", "Threshold 2") == "Threshold" && channelPairBase ("Low Threshold", "High Threshold").isEmpty() && channelPairBase ("Optical Threshold 1", "Discrete Threshold 1").isEmpty(),
           "pair P5: the channel tokens are literal (''/' R', L/M-R/S, L/R as prefix or suffix, Left/Right, 1/2); Low/High and Optical/Discrete are not channel pairs");
    // P6: Shadow Hills - four candidates (two stages x two channels): no pair rule, says so
    const auto sh = decide ({ cand (2, "Optical Threshold 1", true, cv, 0.0, 840), cand (5, "Discrete Threshold 1", true, cv, 0.0, 840), cand (14, "Optical Threshold 2", true, cv, 0.0, 840), cand (17, "Discrete Threshold 2", true, cv, 0.0, 840) });
    check (! sh.decided && sh.whyNot.contains ("4 candidates, 4 certified"), "pair P6 (Shadow Hills): four candidates are not a pair and no master; stays for review");
    // M1: Kiive XTComp - INPUT over Input Left / Input Right; M2: DSM V3 - Threshold over Threshold 1/2/3; M3: a name match alone never decides
    const auto xt = decide ({ cand (3, "INPUT", true, cv, 0.0, 100), cand (7, "Input Left", true, cv, 0.0, 100), cand (8, "Input Right", true, cv, 0.0, 100) });
    check (xt.decided && xt.rule == "master_over_trims" && xt.pick == 3 && xt.others == juce::StringArray { "Input Left", "Input Right" }, "master M1 (Kiive XTComp): INPUT is Input Left / Input Right minus the channel suffix and certifies -> the amount; the trims stay");
    const auto dsm = decide ({ cand (1, "Threshold", true, cv, 0.0, 100), cand (3, "Threshold 1", false, {}, 0.0, 100), cand (7, "Threshold 2", false, {}, 0.0, 100), cand (11, "Threshold 3", true, cv, 0.0, 100) });
    check (dsm.decided && dsm.rule == "master_over_trims" && dsm.pick == 1 && dsm.others.size() == 3, "master M2 (DSM V3): Threshold over Threshold 1 / 2 / 3");
    const auto nm = decide ({ cand (3, "INPUT", false, {}, 0.0, 100), cand (7, "Input Left", true, cv, 0.0, 100), cand (8, "Input Right", true, cv, 0.0, 100) });
    check (! nm.decided && nm.whyNot.contains ("a name match alone never decides"), "master M3: the master must certify - a name match alone never decides (" + nm.whyNot + ")");
    // X1: Ozone 12 Vintage Compressor - Stereo/Main over Aux, literal words; Main must certify
    const auto oz = decide ({ cand (8, "VCOMP: Stereo/Main Threshold", true, cv, 0.0, 1008), cand (21, "VCOMP: Aux Threshold", false, {}, 0.0, 1008) });
    check (oz.decided && oz.rule == "main_over_aux" && oz.pick == 8, "aux X1 (Ozone 12 Vintage): Stereo/Main certified over Aux -> Main is the amount");
    check (! decide ({ cand (8, "VCOMP: Stereo/Main Threshold", false, {}, 0.0, 10), cand (21, "VCOMP: Aux Threshold", true, cv, 0.0, 10) }).decided, "aux X1b: Main must certify");
}

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
    // JUDGED ONLY WHERE THE LADDER ACCEPTS, WHAT WAS SEEN NAMED (A6-A9, ruled 4 Oct, from Sean's 3 Oct traces)
    {
        // A6: H-Comp (s) 15.0.72, cert-traces/2026-10-04-sean-zip: the reference run's own numbers - a modelled floor at about -82 dBFS,
        // tone_frac 0.12 / 0.37 / 0.70 at -90 / -84 / -78, clean from -72 up; the -90/-84 and -84/-78 rungs fail their 6 dB pair (1.3, 3.2 dB), -78/-72 passes (4.9? no: 77.45-72.56 = 4.9 fails), -66/-60 passes (5.9); so the judgement starts at -66
        auto hc = sweeptest::fromGains ({ { 0.0, 0.0, 0.0 }, { 0.0, -1.0, -2.0 } });
        const double in[] = { -90, -84, -78, -72, -66, -60, -58, -56, -54, -52, -50, -48, -46, -44, -42, -40, -38, -36, -34, -32, -30, -28, -26, -24, -22, -20, -18, -16, -14, -12, -10, -8, -6, -4, -2, 0 };
        const double out[] = { -81.959, -80.6809, -77.4546, -72.5555, -66.8945, -60.9804, -58.9927, -56.9974, -55.0017, -53.0067, -51.0054, -49.0077, -47.0084, -45.0064, -43.0087, -41.0068, -39.0055, -37.0067, -35.0032, -33.0022, -31.0015, -28.9966, -26.9952, -24.9911, -22.9843, -20.9804, -18.9711, -16.9607, -14.9508, -12.9335, -10.916, -8.8947, -6.8651, -4.8345, -2.7953, -0.748 };
        const double tf[]  = { 0.1245, 0.3684, 0.6992, 0.9004, 0.9739, 0.9936, 0.9955, 0.9975, 0.9983, 0.9987, 0.9996, 0.9995, 0.9996, 1.0, 0.9996, 1.0, 1.0, 0.9997, 1.0, 1.0, 0.9997, 1.0, 0.9999, 0.9999, 1.0, 0.9997, 1.0, 1.0, 0.9997, 1.0, 1.0, 0.9997, 1.0, 0.9998, 0.9999, 1.0 };
        hc.refDb.clear(); hc.refToneFrac.clear(); hc.refNonFinite.clear();
        for (size_t k = 0; k < 36; ++k) { hc.refDb[levelKey (in[k])] = out[k]; hc.refToneFrac[levelKey (in[k])] = tf[k]; hc.refNonFinite[levelKey (in[k])] = 0; hc.inRmsDb[levelKey (in[k])] = in[k] - 3.0103; }
        const auto dh = derive (hc, sweeptest::kLevels, -1);
        check (! dh.unlicensedSuspect && dh.referenceSeen.isEmpty() && dh.referenceJudgedFromDb && std::abs (*dh.referenceJudgedFromDb - (-66.0)) < 1e-9
                 && dh.referenceNote.contains ("noise floor above the test level at -90, -84 dB") && dh.referenceNote.contains ("not judged"),
               "licence A6 (H-Comp's traces): a modelled noise floor at -82 dBFS is NOT a licence flag - the off-tone readings at -90/-84 lie below the first ladder rung that passes (-66) and are noted as 'noise floor above the test level', nothing judged there (" + dh.referenceNote + ")");
        // A7: MDynamics 14.16.0, candidate [8] Threshold (Compressor): clean tone everywhere except fixed-level bursts at -44.8 dBFS on inputs -56, -54, -44 (tone_frac 0) and 0.56 at -46 - every rung passes, so judged throughout; bursts named
        auto md = hc; md.refDb.clear(); md.refToneFrac.clear(); md.refNonFinite.clear(); md.inRmsDb.clear();
        const double mout[] = { -93.0113, -87.0089, -81.0111, -75.0106, -69.0091, -63.0116, -61.0098, -44.7892, -44.806, -55.0092, -53.0105, -51.0112, -48.0116, -44.7653, -45.0104, -43.0092 };
        const double mtf[]  = { 0.9998, 1.0, 0.9998, 0.9999, 1.0, 0.9997, 1.0, 0.0, 0.0, 1.0, 1.0, 0.9998, 0.5609, 0.0, 1.0, 1.0 };
        for (size_t k = 0; k < 16; ++k) { md.refDb[levelKey (in[k])] = mout[k]; md.refToneFrac[levelKey (in[k])] = mtf[k]; md.refNonFinite[levelKey (in[k])] = 0; md.inRmsDb[levelKey (in[k])] = in[k] - 3.0103; }
        const auto dm = derive (md, sweeptest::kLevels, -1);
        check (dm.unlicensedSuspect && dm.referenceSeen.startsWith ("fixed-level non-tone bursts at -44.8 dBFS (inputs -56, -54, -44 dB)") && dm.referenceJudgedFromDb && std::abs (*dm.referenceJudgedFromDb - (-90.0)) < 1e-9,
               "licence A7 (MDynamics' traces): off-tone readings whose output sits at one level (-44.8 dBFS) whatever the input are 'fixed-level non-tone bursts', named with the inputs; every rung passed so the whole run was judged (" + dm.referenceSeen + ")");
        // A8: the same off-tone readings but at scattered output levels, few among many clean ones, are 'intermittent dropouts'; many are 'not the input's tone'
        auto dr = md; dr.refDb[levelKey (-54.0)] = -70.0; dr.refDb[levelKey (-44.0)] = -60.0;
        check (derive (dr, sweeptest::kLevels, -1).referenceSeen.startsWith ("intermittent dropouts"), "licence A8: isolated off-tone readings at scattered output levels are named 'intermittent dropouts'");
        auto silent2 = md; for (auto& [k, v] : silent2.refDb) v = -130.0;
        check (derive (silent2, sweeptest::kLevels, -1).referenceSeen == "silent at every judged level", "licence A8b: silence is named 'silent at every judged level'");
        // A9: when NO rung passes (a dead unit) every level is judged - silence still flags
        auto dead = hc; for (auto& [k, v] : dead.refDb) v = -130.0;
        check (derive (dead, sweeptest::kLevels, -1).unlicensedSuspect && ! derive (dead, sweeptest::kLevels, -1).referenceJudgedFromDb, "licence A9: with no ladder rung passing, every level is judged, and a silent unit is still flagged");
    }

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

    // THE COMMITTED FIXTURE RE-DERIVES FROM ITS TRACE (decision D2: re-compute, never re-measure). The record sits
    // beside its trace (frozen 2 Oct, when the store's townhouse record became the profile sweep's).
    const auto fx = juce::JSON::parse (dir.getChildFile ("AudioUnit_417f6e76_1.8.1.json").loadFileAsString());
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
    {
        // A SWEEP THAT LANDS ENDS THE REFUSAL (4 Oct, found by the re-sweep rehearsal: V76U73 swept for 250 s and still filed refused)
        auto refused = juce::JSON::parse (R"json({"product": "v", "controls": [{"index": 3}], "thresholdRefusal": {"stage": "plan", "reason": "not swept"}})json");
        auto* sw = new juce::DynamicObject(); sw->setProperty ("result", "flat");
        const auto after = composeFixture (refused, juce::var (sw));
        check (! after.hasProperty ("thresholdRefusal") && after.getProperty ("thresholdSweep", {}).getProperty ("result", "").toString() == "flat" && refused.hasProperty ("thresholdRefusal"),
               "sweep P1b: composing a sweep onto a refused record removes the refusal (the base is untouched), so the row reads the sweep");
        check (ejmap::loop::outcomeForRecord (after).state != "refused", "sweep P1c: the re-swept record no longer files as refused");
    }

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
    check (xla.ok && xla.thr == 3 && xla.candidates.empty() && xla.wordValuedDropped == juce::StringArray { "Input Pad" },
           "pick K1: Acme Opticom XLA-3 sweeps [3] Input Gain, not [6] Input Pad - since 4 Oct the Pad (two steps, Off / Off / On) is dropped as a switch before the pick, the same answer by the switch rule");
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
    Subject u = s; u.reach = Subject::Reach::unfixtured;
    check (measurable (s, false) && measurable (s, true) && measurable (u, false), "measurable M5 (ruled 2 Oct, both reaches): PACE-wrapped is NOT unlicensed - no marker hold, no --include-pace; the scan's licence stops are carried forward and the probe's window watch decides");
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
    check (names.contains ("elysia mpressor") && names.contains ("Local Comp") && names.contains ("A Tuner"),
           "discovery F2: mapped by another machine (map state only, no local map) and mapped here (local map) are both candidates ("
             + names.joinIntoString (", ") + ")");
    // ONE STORE (ruled 1 Oct): a pitch product is a CANDIDATE, for tuner certification, in the same worklist.
    juce::String tunerCat;
    for (const auto& c : d.candidates) if (c.inst.desc.name == "A Tuner") tunerCat = c.category;
    { juce::StringArray un; for (const auto& u : d.unmapped) un.add (u.name + ":" + u.category);
      check (d.unmapped.size() == 1 && un.contains ("Unmapped Comp:compressor") && ! un.contains ("Some EQ:eq") && d.noMapYet >= 1,
             "discovery F4 (2 Oct): a compressor with no map anywhere is NAMED in the no-map-yet list (information), an EQ is not (" + un.joinIntoString (", ") + ")"); }
    // CERTIFICATION DOES NOT NEED A MAP (ruled 2 Oct, afternoon, reversing 29 Sep): installed + category + not excluded.
    { juce::String mbU, mbO; for (const auto& c : d.candidates) { if (c.inst.desc.name == "Unmapped Comp") mbU = c.mappedBy; if (c.inst.desc.name == "Other Build") mbO = c.mappedBy; }
      check (names.contains ("Unmapped Comp") && mbU == "none" && names.contains ("Other Build") && mbO == "server map at a different build",
             "discovery F5: an installed, categorised, UNMAPPED compressor is discovered, map state 'none' on its row; a map at another build is 'server map at a different build' (" + mbU + " / " + mbO + ")"); }
    check (! names.contains ("Some EQ")
             && names.contains ("A Tuner") && tunerCat == "pitch" && d.tuners.contains ("A Tuner"),
           "discovery F3: another category is not a candidate; a pitch product IS a candidate, category pitch (one store)");
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
    {
        // --retry-licence (ruled 2 Oct): only the refusals a licence window caused come back
        const auto lic = names (partitionStore (mixed, true, false, true).toSweep);
        check (lic == juce::StringArray { "aaaa0004", "aaaa0008" }, "record R12: --retry-licence re-runs ONLY the refusal a window caused (aaaa0008) beside the never-swept record (aaaa0004), not defaults, budget, plan, ratio or reference refusals (" + lic.joinIntoString (",") + ")");
    }
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
    led.getChildFile ("map-state.json").replaceWithText (R"({"identities": {"AudioUnit|cccc0001|1.0.0": {"state": 3}, "AudioUnit|cccc0002|1.0.0": {"state": 3}, "AudioUnit|cccc0003|1.0.0": {"state": 3}, "AudioUnit|cccc0004|1.0.0": {"state": 3}, "AudioUnit|cccc0005|1.0.0": {"state": 3}}})");
    led.getChildFile ("categories.json").replaceWithText (R"({"products": {
        "hanger|test":  {"category": "compressor", "disposition": "operator_excluded", "why": "hangs on load", "mark_keys": ["AudioUnit|cccc0001"]},
        "fine|test":    {"category": "compressor", "disposition": "sweep", "mark_keys": ["AudioUnit|cccc0002"]},
        "nodisp|test":  {"category": "compressor", "mark_keys": ["AudioUnit|cccc0003"]},
        "tuner|test":   {"category": "pitch", "disposition": "no_dial_set", "mark_keys": ["AudioUnit|cccc0004"]},
        "reviewed|test": {"category": "compressor", "disposition": "review", "why": "arms disagree", "mark_keys": ["AudioUnit|cccc0005"]}}})");
    auto inst = [] (const char* name, int uid, const char* code) {
        InstalledRecord r; r.desc.name = name; r.desc.uniqueId = uid; r.desc.version = "1.0.0"; r.desc.pluginFormatName = "AudioUnit";
        r.desc.fileOrIdentifier = juce::String ("AudioUnit:Effects/") + code; r.identityKey = echojay::identityKeyForDescription (r.desc);
        r.uidKey = "AudioUnit|" + juce::String::toHexString (uid).toLowerCase(); return r; };
    const std::vector<InstalledRecord> installed { inst ("Hanger", 0xcccc0001, "aufx,hang,Test"), inst ("Fine", 0xcccc0002, "aufx,fine,Test"), inst ("NoDisp", 0xcccc0003, "aufx,nodi,Test"),
                                                   inst ("Tuner", 0xcccc0004, "aufx,tune,Test"), inst ("Reviewed", 0xcccc0005, "aufx,revw,Test") };
    const auto in = loadDiscoveryInputs (led);
    const auto disc = discoverCandidates (in, installed, {});
    juce::StringArray cand; for (const auto& c : disc.candidates) cand.add (c.inst.desc.name);
    check (cand.contains ("Fine") && cand.contains ("NoDisp") && ! cand.contains ("Hanger") && disc.excludedByDisposition.size() == 1 && disc.excludedByDisposition[0].contains ("hangs on load")
             && in.notes.joinIntoString (" ").contains ("exclusion disposition"),
           "record R10 (kept by the 2 Oct ruling): operator_excluded - the mapper's own manual exclusion, not a verdict - keeps a compressor out of the cert worklist even with a category and a clean scan, with its why; "
           "disposition sweep and no disposition are both candidates (" + cand.joinIntoString (",") + ")");
    // RE-RULED 2 Oct: a mapping verdict is not an exclusion - the category decides.
    check (cand.contains ("Tuner") && disc.tuners.contains ("Tuner") && cand.contains ("Reviewed"),
           "record R10b: a tuner with category pitch and disposition no_dial_set IS discovered (for tuner certification), and a compressor with disposition review IS a candidate (" + cand.joinIntoString (",") + ")");
    check (exclusionDisposition ("operator_excluded") && ! exclusionDisposition ("hang_on_load") && ! exclusionDisposition ("crash_on_load") && ! exclusionDisposition ("no_dial_set")
             && ! exclusionDisposition ("review") && ! exclusionDisposition ("not_a_processor") && ! exclusionDisposition ("sweep") && ! exclusionDisposition (""),
           "record R10c (THE PRINCIPLE, 2 Oct): the only exclusion disposition is the operator's; every other disposition word is a mapping verdict - load evidence lives in the ledger, not in categories.json");
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
    {
        // E11 (2 Oct, NEOLD U2A): the engage signature is a control with NO effect - pass-through, or a flat sweep whose
        // positions read identically - even when the device saturates a little at the top (not input-plus-a-constant).
        auto dev = [] (double perPositionDb) {
            Measured m; m.ok = true; m.movingDb = 0.1;
            for (int p = 0; p < 4; ++p)
            {
                PositionReading r; r.k = p; r.norm = (float) p / 3.0f; r.text = juce::String (p * 25) + " %";
                for (double L : { -54.0, -48.0, -24.0, -12.0, -6.0 })
                {
                    HoldReading h; h.present = true; h.inRmsDb = L - 3.0103;
                    h.levelDb = h.inRmsDb + 0.18 - (L > -10.0 ? (L + 10.0) * 0.02 : 0.0) - (L > -24.0 ? perPositionDb * p : 0.0);   // +0.18 dB, soft saturation above -10; a small level-dependent per-position effect when asked
                    r.holds[levelKey (L)] = h;
                }
                m.positions.push_back (r);
            }
            return m; };
        const auto same = derive (dev (0.0), sweeptest::kLevels, -1, true);
        check (same.result == "flat" && ! same.passThroughAtDefaults && same.flatSpanDb && *same.flatSpanDb < 1e-9 && engageSignature (same),
               "engage E11: a flat sweep whose positions read identically is the engage signature even with saturation at the top (not pass-through: " + juce::String (same.passThroughAtDefaults ? "yes" : "no") + ", flatSpan " + juce::String (same.flatSpanDb.value_or (-1), 3) + ")");
        const auto drift = derive (dev (0.2), sweeptest::kLevels, -1, true);
        check (drift.result == "flat" && ! engageSignature (drift),
               "engage E11b: positions that differ by 0.6 dB are flat by the 1 dB rule but NOT the signature (the control did something) (flatSpan " + juce::String (drift.flatSpanDb.value_or (-1), 2) + ")");
    }
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
    // THE WINDOW STRADDLING THE NEXT FLIP (5 Oct R8d, Auto-Tune Artist 36 / 17 / 6): the last window of each half period already
    // holds the next step (+9 cents on a plateau at 0); it is not this half period's business and must not read as unsettled
    {
        auto straddle = vibPos (30.0, 20.0, 1.0, 0.2);   // a fast retune (tau 20 ms): settled within ~60 ms, flat for the rest
        for (size_t i = 1; i < straddle.windows.size(); ++i) if (straddle.windows[i].target != straddle.windows[i - 1].target) straddle.windows[i - 1].outC += 9.0;
        const auto v7 = deriveSpeed (straddle, 30.0, 0.5);
        check (v7.result == "measured" && v7.durationMs < 120.0, "tuner V7: a +9 cent reading in the window straddling the next flip does not make a settled fast retune 'not settled' (" + v7.result + ": " + v7.reason + ", " + juce::String (v7.durationMs, 0) + " ms)");
    }
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
    Subject sm = s; sm.mapState = "none";
    check (composeFixture (sm, list, text, 0, 0, "probe", "2026-10-02").getProperty ("mapState", "").toString() == "none" && ! fx.hasProperty ("mapState"),
           "mapfp M0c (ruled 2 Oct): the record carries mapState as information when the subject has it (the tuner path sets it from the batch's option), absent otherwise");

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
        { auto* sc = new juce::DynamicObject(); sc->setProperty ("policy", ejmap::sidechaincheck::kPolicyNow); juce::Array<juce::var> eb; auto* b = new juce::DynamicObject(); b->setProperty ("index", 1); b->setProperty ("name", "Side-Chain Input Bus"); b->setProperty ("channels", 2); eb.add (juce::var (b)); sc->setProperty ("extraInputBuses", eb); s->setProperty ("sidechain", juce::var (sc)); }
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
        check (P.getProperty ("measured", {}).getProperty ("sidechain", "").toString() == "self-keyed (as EchoJay 04e)" && P.getProperty ("measured", {}).getProperty ("extra_input_buses", {}).size() == 1,
               "export X-SC (ruled 6 Oct): the profile's measured block says the sidechain policy the sweep ran under, by the record's label, with the extra buses");
        // X-PAIR (Kathy's ruling 3, 6 Oct): a sweep that wrote a twin with the amount exports amount.pair_with (control, index, rule) and keeps
        // the twin OUT of neutral; the tone check's fixed writes then never write it as a neutral (it rides with the amount at every pick)
        { auto pr = juce::JSON::parse (juce::JSON::toString (rec)); auto* pw = new juce::DynamicObject(); pw->setProperty ("index", 5); pw->setProperty ("name", "Attack"); pr.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("pairWrite", juce::var (pw));
          const auto ep = exportCompProfile (pr);
          bool attackNeutral = false; if (ep.ok) for (const auto& n : *ep.profile.getProperty ("neutral", {}).getArray()) if (n.getProperty ("control", "") == "Attack") attackNeutral = true;
          check (ep.ok && (int) ep.profile.getProperty ("amount", {}).getProperty ("pair_with", {}).getProperty ("index", -1) == 5 && ep.profile.getProperty ("amount", {}).getProperty ("pair_with", {}).getProperty ("rule", "") == "dual_mono_pair" && ! attackNeutral,
                 "export X-PAIR: pairWrite on the sweep -> amount.pair_with {Attack, 5, dual_mono_pair} and Attack is not a neutral (" + ep.refused + ")");
          if (ep.ok) { juce::StringArray sp; juce::Array<juce::var> wp; juce::String np; toneWrites (ep.profile, pr, sp, wp, np); bool five = false; for (const auto& x : sp) if (x.startsWith ("5:")) five = true;
                       check (! five, "export X-PAIR: the tone check's fixed writes do not write the twin (it is written with the amount at the pick's norm)"); }
          check (! exportCompProfile (rec).profile.getProperty ("amount", {}).hasProperty ("pair_with"), "export X-PAIR: no pairWrite, no pair_with"); }
        const auto curve = P.getProperty ("amount", {}).getProperty ("curve", {});
        const double effPeak = (double) rec.getProperty ("thresholdSweep", {}).getProperty ("thresholdEffective1dB", {})[0];
        const auto steps = P.getProperty ("measured", {}).getProperty ("steps_dbfs", {});
        check (curve.size() == 16 && std::abs ((double) curve[0].getProperty ("eff_threshold_dbfs", 0.0) - (std::round (effPeak * 10.0) / 10.0 - 3.0103)) < 0.006   // the raw crossing rounded to 0.1 dB at export (3 Oct), then -3.0103
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
                 && std::abs ((double) g0.getProperty ("2", 0.0) - ((double) g0.getProperty ("1", 0.0) + 1.0 / 0.75)) < 0.11,   // each point rounded to 0.1 dB at export: the pair can differ from 1.333 by up to 0.1
               "export X14 (v1.2): in_at_gr_dbfs {1,2,3} on every curve point and eff_threshold_dbfs == in_at_gr_dbfs[1], written identically");
        check (lastThreeNull, "export X15: not_reached in the record is null in the export, as his spec says");
        check (P.getProperty ("amount", {}).getProperty ("stepped", true).isBool() && ! (bool) P.getProperty ("amount", {}).getProperty ("stepped", true)
                 && std::abs ((double) P.getProperty ("detector_f", 9.0) - 0.2) < 1e-6,
               "export X16 (v1.4): stepped is a boolean (false for a continuous control); detector_f is the measured number");
        check (P.getProperty ("detector_f_source", "").toString() == "measured" && P.getProperty ("detector_f", {}).isDouble(), "export X16b (Sean, 6 Oct evening): a measured detector's profile carries EXACTLY detector_f_source \"measured\" beside its number (a missing key goes red)");
        // X16c (Sean's ruling + amendment, 6 Oct): a detector not measured is exported EXACTLY as "detector_f": null, "detector_f_source": "unknown",
        // the reason in notes - never assumed, never a default value; a detector never run exports the same way (no refusal)
        { auto un = juce::JSON::parse (juce::JSON::toString (rec)); auto* dd = new juce::DynamicObject(); dd->setProperty ("fraction", juce::var()); dd->setProperty ("unmeasurable", "the two-tone produced no output at any level while the sine reached 2 dB");
          un.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("detector", juce::var (dd));
          const auto eu = exportCompProfile (un);
          const auto notes = eu.ok ? juce::JSON::toString (eu.profile.getProperty ("notes", {})) : juce::String();
          check (eu.ok && eu.profile.hasProperty ("detector_f") && eu.profile.getProperty ("detector_f", 1.0).isVoid() && eu.profile.getProperty ("detector_f_source", "").toString() == "unknown" && ! eu.profile.hasProperty ("detector_f_raw")
                   && notes.contains ("detector: unknown - the two-tone produced no output") && notes.contains ("both L_ref"),
                 "export X16c: an unmeasured detector is exactly detector_f null + detector_f_source \"unknown\", the reason in notes (" + eu.refused + ")");
          check (eu.ok && ! eu.profile.getProperty ("detector_f", 1.0).isDouble() && ! eu.profile.getProperty ("detector_f", 1.0).isInt(), "export X16c: detector_f is null, never a number (no default value)");
          auto none = juce::JSON::parse (juce::JSON::toString (rec)); none.getProperty ("thresholdSweep", {}).getDynamicObject()->removeProperty ("detector");
          const auto en = exportCompProfile (none);
          check (! en.ok && en.refused.contains ("detector_f not measured"), "export X16c: a detector never ATTEMPTED still refuses (the follow-up measures first; only an attempted, unmeasured one is unknown)");
          auto att = juce::JSON::parse (juce::JSON::toString (rec)); auto* d0 = new juce::DynamicObject(); d0->setProperty ("fraction", juce::var()); att.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("detector", juce::var (d0));
          const auto ea = exportCompProfile (att);
          check (ea.ok && ea.profile.getProperty ("detector_f", 1.0).isVoid() && ea.profile.getProperty ("detector_f_source", "") == "unknown" && juce::JSON::toString (ea.profile.getProperty ("notes", {})).contains ("recorded no fraction"), "export X16c: an attempted detector with no fraction and no reason is unknown too, saying so");
          // the tone check's rule for a null detector_f: both L_ref values, rms and peak
          check (detectorUnknown (eu.profile) && ! detectorUnknown (P) && detectorFractionsToTest (eu.profile) == std::vector<double> { 0.0, 1.0 } && detectorFractionsToTest (P).size() == 1
                   && std::abs (toneLevelFor (eu.profile, 2.0, 0.0).Lref - toneLevelRef (0.0)) < 1e-9 && std::abs (toneLevelFor (eu.profile, 2.0, 1.0).Lref - toneLevelRef (1.0)) < 1e-9 && ! toneLevelFor (eu.profile, 2.0).ok,
                 "export X16d: a null detector_f tests at both L_ref values (f = 0 and f = 1); a measured one at its own; with no f at all the level cannot be anchored"); }
        const auto Q = P.getProperty ("quality", {});
        check (Q.isObject() && std::abs ((double) Q.getProperty ("point_error_db", 9.0) - 0.12) < 1e-6 && Q.getProperty ("method", "") == "hold 2.5 s vs 5 s"
                 && (bool) Q.getProperty ("monotonic_within_positions", false) && (bool) Q.getProperty ("monotonic_across_positions", false),
               "export X19 (v1.4): quality carries point_error_db from the hold-doubled repeat, its method, and the monotonic flags");
        check (notesText (P.getProperty ("notes", {})).contains ("guards passed: tone_frac") && notesText (P.getProperty ("notes", {})).contains ("quiet-reference 6 dB")
                 && notesText (P.getProperty ("notes", {})).contains ("ascending-only"),
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
        check (sx.ok && sx.fitMaxErrorDb > 1.5 && notesText (sx.profile.getProperty ("notes", {})).contains ("NOT a gate"),
               "export X18 (v1.2): a record his v1 model cannot fit (max error " + juce::String (sx.fitMaxErrorDb, 2) + " dB) still exports - fit is reported, never a gate");
    }
    check (! exportCompProfile (record (6, true, false, "", "certified")).ok, "export X9: fewer than 9 curve points refuses");
    // X9b (Kathy's ruling 3, 6 Oct - UnFairchild, Shadow Hills Class A): positions whose write did not land are DROPPED from the export,
    // and a continuous-declared control whose writes landed only on k/(n-1) is stepped with those detents, judged by Sean's stepped rule
    {
        auto withLanding = [&] (int positions, std::function<bool (int)> landed) {
            auto r = record (positions, true, false, "", "certified"); auto sv = r.getProperty ("thresholdSweep", {});
            juce::Array<juce::var> lb; for (int i = 0; i < positions; ++i) lb.add (landed (i) ? "instack" : "unlanded");
            sv.getDynamicObject()->setProperty ("positionLandedBy", lb);
            auto q = sv.getProperty ("quality", {}); if (! q.isObject()) { q = juce::var (new juce::DynamicObject()); sv.getDynamicObject()->setProperty ("quality", q); }
            juce::Array<juce::var> dn; dn.add ("4@4:-20.1/-21.9"); dn.add ("5@4:-20.1/-21.9"); q.getDynamicObject()->setProperty ("deepPointsNulled", dn);
            return r; };
        const auto un = withLanding (16, [] (int i) { return i % 3 == 0; });                       // landed at 0, 3, 6, 9, 12, 15 of 16 = k/5
        const auto dr = ejmap::sweep::dropUnlandedPositions (un.getProperty ("thresholdSweep", {}));
        check (dr.dropped == 10 && dr.sweep.getProperty ("positionNorms", {}).size() == 6 && dr.sweep.getProperty ("inAtGr", {}).size() == 6 && (int) dr.sweep.getProperty ("positions", 0) == 6
                 && dr.sweep.getProperty ("reduction_db", {}).getDynamicObject()->getProperties().begin()->value.size() == 6,
               "export X9b: dropping the unlanded positions filters every per-position array, the lists inside dicts, and recounts positions (16 -> 6)");
        { const auto dn = dr.sweep.getProperty ("quality", {}).getProperty ("deepPointsNulled", {});
          check (dn.size() == 0 || (dn.size() == 1 && dn[0].toString().startsWith ("1@")), "export X9b: hold-test nulls on dropped positions go, the rest are renumbered (4 unlanded -> gone; 5 unlanded -> gone; got " + juce::JSON::toString (dn) + ")"); }
        check (ejmap::sweep::landedDetents (un.getProperty ("thresholdSweep", {})) == 6, "export X9b: the landed norms 0, .2, .4, .6, .8, 1 say 6 detents");
        check (ejmap::sweep::landedDetents (record (16, true, false, "", "certified").getProperty ("thresholdSweep", {})) == 0, "export X9b: nothing unlanded = no detents by landing");
        check (ejmap::sweep::landedDetents (withLanding (16, [] (int i) { return i != 2; }).getProperty ("thresholdSweep", {})) == 0, "export X9b: landed norms that are not a k/(n-1) grid say nothing");
        const auto ex = exportCompProfile (un);
        check (ex.ok && ex.profile.getProperty ("amount", {}).getProperty ("curve", {}).size() == 6 && (bool) ex.profile.getProperty ("amount", {}).getProperty ("stepped", false)
                 && ex.profile.getProperty ("amount", {}).getProperty ("stepped_by_evidence", "").toString().contains ("6 values") && ex.profile.getProperty ("amount", {}).getProperty ("positions_dropped_unlanded", "").toString().contains ("10 position"),
               "export X9b (UnFairchild): 6 landed detents export stepped under Sean's rule where 16 continuous points with 6 reaching 1 dB would refuse (" + ex.refused + ")");
    }
    check (! exportCompProfile (record (16, true, true, "", "certified")).ok && exportCompProfile (record (16, true, true, "", "certified")).refused.contains ("other"),
           "export X10: several candidates is topology other and no profile");
    check (! exportCompProfile (record (16, true, false, "", "flat")).ok, "export X11: a non-certified sweep refuses");
    {
        // DEEP POINTS IN THE EXPORT (v1.7, X25-X27): keys 1..6 on every point; a position with no shallow point exports its deep
        // points null too and is never dropped; deep_point_error_db is written (null when none) and never refuses.
        auto deepRec = juce::JSON::parse (juce::JSON::toString (rec));
        auto ia = deepRec.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {});
        for (int i = 0; i < ia.size(); ++i) if (auto* o = ia[i].getDynamicObject()) { if (ia[i].getProperty ("3", {}).isDouble()) { const double v = (double) ia[i].getProperty ("3", {}); o->setProperty ("4", v + 1.4); o->setProperty ("5", v + 2.8); o->setProperty ("6", juce::var()); } }
        // position 0: deep only (no shallow point) - must export with 4/5/6 null, still present
        if (auto* o = ia[0].getDynamicObject()) { o->setProperty ("1", juce::var()); o->setProperty ("2", juce::var()); o->setProperty ("3", juce::var()); o->setProperty ("4", -40.0); o->setProperty ("5", -38.0); }
        deepRec.getProperty ("thresholdSweep", {}).getProperty ("quality", {}).getDynamicObject()->setProperty ("deep_point_error_db", 0.4);
        const auto ed = exportCompProfile (deepRec);
        const auto dc = ed.profile.getProperty ("amount", {}).getProperty ("curve", {});
        check (ed.ok && dc.size() == 16 && dc[1].getProperty ("in_at_gr_dbfs", {}).hasProperty ("6") && dc[1].getProperty ("in_at_gr_dbfs", {}).getProperty ("6", 0.0).isVoid()
                 && std::abs ((double) dc[1].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0) - ((double) dc[1].getProperty ("in_at_gr_dbfs", {}).getProperty ("3", 0.0) + 1.4)) < 0.01,
               "export X25: every point carries keys 1..6, numbers converted like the shallow ones, null where not reached (" + ed.refused + ")");
        check (dc[0].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0).isVoid() && dc[0].getProperty ("in_at_gr_dbfs", {}).getProperty ("5", 0.0).isVoid() && dc[0].getProperty ("in_at_gr_dbfs", {}).getProperty ("1", 0.0).isVoid()
                 && notesText (ed.profile.getProperty ("notes", {})).contains ("deep points nulled on positions with no shallow point: position 0"),
               "export X26: a position with no 1/2/3 value exports its deep points as null too - present, never dropped, and said in notes");
        check (std::abs ((double) ed.profile.getProperty ("quality", {}).getProperty ("deep_point_error_db", -1.0) - 0.4) < 1e-9 && (bool) ed.profile.getProperty ("quality", {}).getProperty ("monotonic_within_positions", false),
               "export X27: deep_point_error_db is written from the record and the monotonic check runs over 1..6 (a rising 4/5 keeps it true)");
        auto deepBig = juce::JSON::parse (juce::JSON::toString (deepRec)); deepBig.getProperty ("thresholdSweep", {}).getProperty ("quality", {}).getDynamicObject()->setProperty ("deep_point_error_db", 3.0);
        check (exportCompProfile (deepBig).ok, "export X27b: a large deep_point_error_db never refuses the profile (informational)");
        // EVERY DEEP NULL ACCOUNTED FOR (v1.8 notes, ruled 3 Oct, X28-X30): one "deep null <g> dB - <reason>: positions <norms>" line per
        // (level, reason); the set of (level, norm) on those lines equals the set of deep nulls in the curve, each exactly once.
        auto nullAccount = [] (const juce::var& prof, juce::String& why) -> bool
        {
            std::map<std::pair<int, juce::String>, int> inCurve, onLines;
            const auto cv = prof.getProperty ("amount", {}).getProperty ("curve", {});
            for (int i = 0; i < cv.size(); ++i) for (int t : ejmap::sweep::kGrTargets) if (t >= ejmap::sweep::kDeepFrom && cv[i].getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (t), 0.0).isVoid()) ++inCurve[{ t, juce::String ((double) cv[i].getProperty ("norm", 0.0), 4) }];
            for (const auto& line : juce::StringArray::fromTokens (notesText (prof.getProperty ("notes", {})), ";", ""))
            {
                const auto l = line.trim(); if (! l.startsWith ("deep null ")) continue;
                const int t = l.fromFirstOccurrenceOf ("deep null ", false, false).getIntValue();
                auto list = l.contains (": positions ") ? l.fromLastOccurrenceOf (": positions ", false, false) : l.fromLastOccurrenceOf (" across positions at ", false, false);
                while (list.contains ("(")) list = list.upToFirstOccurrenceOf ("(", false, false) + list.fromFirstOccurrenceOf (")", false, false);   // details ride in parentheses, never commas
                for (auto pos : juce::StringArray::fromTokens (list, ",", "")) { pos = pos.trim(); if (pos.isNotEmpty()) ++onLines[{ t, pos }]; }
            }
            if (inCurve == onLines) return true;
            juce::StringArray a, b; for (const auto& [k, n] : inCurve) a.add (juce::String (k.first) + "@" + k.second + "x" + juce::String (n)); for (const auto& [k, n] : onLines) b.add (juce::String (k.first) + "@" + k.second + "x" + juce::String (n));
            why = "curve nulls [" + a.joinIntoString (" ") + "] vs lines [" + b.joinIntoString (" ") + "]"; return false;
        };
        juce::String why;
        check (nullAccount (ed.profile, why), "export X28: every deep null in the exported curve is on exactly one 'deep null' note line (" + why + ")");
        {
            // X29: one of each reason - not_reached (6 everywhere here), the all-null position (0), a hold-test failure (position 3 @4, both values), a gap null (position 5 @5), past at the quietest (position 2 @4 below_range)
            auto rr = juce::JSON::parse (juce::JSON::toString (deepRec));
            auto ia2 = rr.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {});
            ia2[3].getDynamicObject()->setProperty ("4", juce::var()); ia2[5].getDynamicObject()->setProperty ("5", juce::var()); ia2[2].getDynamicObject()->setProperty ("4", "below_range"); ia2[7].getDynamicObject()->setProperty ("6", "not_reached");
            rr.getProperty ("thresholdSweep", {}).getProperty ("quality", {}).getDynamicObject()->setProperty ("deepPointsNulled", juce::Array<juce::var> { "3@4: -20.10 / hold-doubled -19.40 / delta 0.70 dB" });
            const auto er = exportCompProfile (rr); const auto nt = notesText (er.profile.getProperty ("notes", {}));
            check (er.ok && nullAccount (er.profile, why), "export X29: with every reason present each deep null is on exactly one line (" + why + ")");
            const auto n3 = juce::String ((double) ia2.size() > 3 ? (double) rr.getProperty ("thresholdSweep", {}).getProperty ("positionNorms", {})[3] : 0.0, 4);
            check (nt.contains ("deep null 6 dB - not reached by -3.01 dBFS: positions 0.4667") && nt.contains ("deep null 4 dB - hold test failed: positions " + n3 + " (-20.10 / hold-doubled -19.40 / delta 0.70 dB)")
                     && nt.contains ("deep null 5 dB - no rising straddle (gap or fall): positions ") && nt.contains ("deep null 4 dB - past at the quietest level: positions ")
                     && nt.contains ("deep null 4 dB - no shallow point (all-null position): positions 0.0000 (measured -43.01 - withheld: no shallow point)"),
                   "export X29b: the reasons are named - not reached by -3.01, hold test failed with BOTH values, no rising straddle, past at the quietest level, and the all-null position with the withheld measurement (" + nt.fromFirstOccurrenceOf ("deep null", false, false) + ")");
            // X30: a null that no reason explains cannot exist - the account is a set equality, so a line dropped (simulated by editing notes) is caught
            auto broken = juce::JSON::parse (juce::JSON::toString (er.profile)); broken.getDynamicObject()->setProperty ("notes", notesText (er.profile.getProperty ("notes", {})).replace ("deep null 5 dB", "deep nul 5 dB"));
            check (! nullAccount (broken, why), "export X30: the account is a set equality - a missing line is caught (control for X28/X29)");
            // X31: the all-null position's line carries the record's own word at 1 dB - not_reached (the knob does nothing there) or below_range (past at the quietest) - never an asserted cause
            auto words = juce::JSON::parse (juce::JSON::toString (rr)); auto ia3 = words.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {});
            for (auto k : { "1", "2", "3" }) ia3[0].getDynamicObject()->setProperty (k, "not_reached");
            for (auto k : { "1", "2", "3" }) ia3[1].getDynamicObject()->setProperty (k, "below_range"); for (auto k : { "4", "5", "6" }) ia3[1].getDynamicObject()->setProperty (k, -20.0);
            const auto ew = exportCompProfile (words); const auto wt = notesText (ew.profile.getProperty ("notes", {}));
            check (ew.ok && nullAccount (ew.profile, why) && wt.contains ("deep null 4 dB - not reached by -3.01 dBFS (all-null position): positions 0.0000") && wt.contains ("deep null 4 dB - past at the quietest level (all-null position): positions 0.0667 (measured -23.01 - withheld: no shallow point)"),
                   "export X31: an all-null position is filed under the record's word at 1 dB - not reached (position 0) or past at the quietest (position 1, its measured deep points withheld) (" + why + " | " + wt.fromFirstOccurrenceOf ("deep null", false, false).upToFirstOccurrenceOf ("deep null 5", false, false) + ")");
            // THE NOTES SHAPE (v1.9 section 3, X32): a list of plain strings, one line each, and the driver's append keeps the shape
            {
                const auto nv = er.profile.getProperty ("notes", {});
                const auto appended = notesAppend (nv, "deep null 6 dB - tone check failed (GR 5.2 against 6.0 expected at L -14.0 dBFS RMS): positions 0.5000");
                check (kNotesAsList && nv.isArray() && nv.size() > 5 && nv[0].isString() && nv[0].toString().startsWith ("levels converted") && ! nv[0].toString().contains (";")
                         && appended.isArray() && appended.size() == nv.size() + 1 && appended[appended.size() - 1].toString().startsWith ("deep null 6 dB - tone check failed"),
                       "export X32: notes is a list of plain strings (kNotesAsList), one line each with no '; ' inside, and notesAppend adds a line in the same shape (" + juce::String (nv.size()) + " lines)");
            }
            // A DEEP POINT OUT OF ORDER IS NULLED BEFORE EXPORT (v1.9; ruled 3 Oct, X33-X36): within its position, or across positions at its level;
            // the sixth account line names it; the account stays exact; a 1/2/3 break is untouched (the quality flag, as before)
            {
                auto mono = juce::JSON::parse (juce::JSON::toString (deepRec)); auto im = mono.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {});
                const double v3 = (double) im[6].getProperty ("3", {});
                im[6].getDynamicObject()->setProperty ("5", v3 + 1.0);                                   // position 6: its 5 dB point below its 4 dB point (v3 + 1.4): within-position break
                im[9].getDynamicObject()->setProperty ("5", (double) im[9].getProperty ("3", {}) + 5.0); // position 9's 5 dB point jumps above its 4 (within order kept) and above position 10's 5: walking the 1 dB direction, position 10's 5 is the point that fails to continue - an ACROSS-only break at 5 dB
                const auto em = exportCompProfile (mono); const auto mc = em.profile.getProperty ("amount", {}).getProperty ("curve", {}); const auto mt = notesText (em.profile.getProperty ("notes", {}));
                check (em.ok && mc[6].getProperty ("in_at_gr_dbfs", {}).getProperty ("5", 0.0).isVoid() && ! mc[6].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0).isVoid() && ! mc[6].getProperty ("in_at_gr_dbfs", {}).getProperty ("3", 0.0).isVoid()
                         && mt.contains ("deep null 5 dB - breaks monotonic order: positions " + juce::String ((double) mono.getProperty ("thresholdSweep", {}).getProperty ("positionNorms", {})[6], 4)),
                       "export X33: a 5 dB point that does not rise above its position's 4 dB point is null before export (the 4 and 3 stay) and is on the 'breaks monotonic order' line");
                // ACROSS POSITIONS THE WHOLE LEVEL GOES (ruled 3 Oct): position 9's jump breaks the 5 dB level across positions, so EVERY 5 dB point is
                // null - position 10 (which a forward walk would blame) and positions 8/11 (which it would keep) alike - on one line naming every norm
                juce::StringArray all5; { const auto pn = mono.getProperty ("thresholdSweep", {}).getProperty ("positionNorms", {}); for (int i = 0; i < pn.size(); ++i) if (! mc[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0).isVoid() && i != 6) all5.add (juce::String ((double) pn[i], 4)); }
                bool none5 = true; for (int i = 0; i < mc.size(); ++i) if (! mc[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("5", 0.0).isVoid()) none5 = false;
                check (none5 && ! mc[10].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0).isVoid() && ! mc[8].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0).isVoid()
                         && mt.contains ("deep null 5 dB - breaks monotonic order across positions at " + all5.joinIntoString (", ")) && mt.contains ("deep null 5 dB - breaks monotonic order: positions 0.4000 (measured"),   // position 6's WITHIN break stays on the point line
                       "export X34: an across-position break at 5 dB nulls the WHOLE 5 dB level (every position, the good points after the outlier included; the 4 dB points stay) on one line naming every norm; position 6's within break keeps its own point line (" + mt.fromFirstOccurrenceOf ("deep null 5 dB - breaks monotonic order across", false, false) + ")");
                check (nullAccount (em.profile, why) && (bool) em.profile.getProperty ("quality", {}).getProperty ("monotonic_within_positions", false) && (bool) em.profile.getProperty ("quality", {}).getProperty ("monotonic_across_positions", false),
                       "export X35: the account stays exact with the sixth line, and the exported quality flags are true again (the server would have nulled the same points) (" + why + ")");
                auto sh = juce::JSON::parse (juce::JSON::toString (deepRec)); auto ish = sh.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {});
                ish[6].getDynamicObject()->setProperty ("2", (double) ish[6].getProperty ("1", {}) - 0.5);   // a SHALLOW break: 2 dB below 1 dB
                const auto es = exportCompProfile (sh);
                check (! es.ok && es.refused.contains ("shallow in_at_gr order break") && es.refused.contains ("point 6: 2 dB not strictly above 1 dB"),
                       "export X36: a 1/2/3 order break that survived the derivation REFUSES the export (needs_review, the violation named) - never exported with the flag false, which the server would reject (" + es.refused + ")");
                // TARGETS TO 12 (v1.10, X37-X39): keys 1..12 on every exported point; a position carrying 7..12 exports them, the carried levels follow;
                // THE SATURATION NOTE: a deep point read above -9.01 dBFS RMS (the top 6 dB) is listed per level, information only, never nulled
                auto twelve = juce::JSON::parse (juce::JSON::toString (deepRec)); auto i12 = twelve.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {});
                for (int i = 2; i <= 13; ++i) { auto* o = i12[i].getDynamicObject(); const double v6 = (double) i12[i].getProperty ("5", {}) + 1.4; o->setProperty ("6", v6); for (int t = 7; t <= 12; ++t) o->setProperty (juce::String (t), (i >= 8 || t <= 9) ? juce::var (v6 + 1.0 * (t - 6)) : juce::var()); }
                const auto e12 = exportCompProfile (twelve); const auto c12 = e12.profile.getProperty ("amount", {}).getProperty ("curve", {}); const auto n12 = notesText (e12.profile.getProperty ("notes", {}));
                bool keys12 = e12.ok; for (int i = 0; i < c12.size() && keys12; ++i) for (int t = 1; t <= 12; ++t) if (! c12[i].getProperty ("in_at_gr_dbfs", {}).hasProperty (juce::String (t))) keys12 = false;
                check (keys12 && ! c12[13].getProperty ("in_at_gr_dbfs", {}).getProperty ("12", 0.0).isVoid() && c12[4].getProperty ("in_at_gr_dbfs", {}).getProperty ("10", 0.0).isVoid() && deepLevelsCarried (e12.profile) == std::vector<int> { 4, 5, 6, 7, 8, 9, 10, 11, 12 },
                       "export X37 (v1.10): every point carries keys 1..12, a 12 dB point exports where measured, null where not, and the carried deep levels run 4..12 (" + e12.refused + ")");
                check (nullAccount (e12.profile, why), "export X38 (v1.10): the account stays exact over 4..12 (" + why + ")");
                // THE SATURATION NOTE, judged from the export itself: at each deep level the positions whose exported point is above -9.01 dBFS RMS
                // must be on that level's line and no other position may be; a level with nothing up there has no line; the points stay numeric
                {
                    bool noteRight = true; juce::String detail;
                    for (int t = 4; t <= 12; ++t)
                    {
                        juce::StringArray up; int numeric = 0;
                        for (int i = 0; i < c12.size(); ++i) { const auto v = c12[i].getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (t), {}); if (v.isDouble()) { ++numeric; if ((double) v > -9.0103) up.add (juce::String ((double) c12[i].getProperty ("norm", 0.0), 4)); } }
                        const auto line = "deep " + juce::String (t) + " dB read in the top 6 dB of the sweep, where saturation also lowers level: positions ";
                        const bool has = n12.contains (line);
                        if (up.isEmpty() ? has : ! n12.contains (line + up.joinIntoString (", "))) { noteRight = false; detail << t << " dB: up [" << up.joinIntoString (", ") << "] has " << (int) has << "; "; }
                        if (numeric == 0 && has) noteRight = false;
                    }
                    const bool some = n12.contains ("read in the top 6 dB"), none6 = ! notesText (e.profile.getProperty ("notes", {})).contains ("read in the top 6 dB");   // the shallow-only export: no deep point, so no line
                    check (noteRight && some && none6, "export X39 (v1.10): per deep level the positions read above -9.01 dBFS RMS (and only those) are on the saturation line, which exists only when some are; the shallow-only profile (no deep point) has none; the points stay numeric (" + detail + ")");
                }
            }
        }
    }
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
        // T4 (Kathy's ruling 3, 6 Oct - AMEK): neutral entries carry their index and are written BY it, so two controls sharing a name
        // each get their own write (24 readouts named "M": by name every one went to the first, and a 0.5125 write to it never landed)
        { bool allIndexed = true; for (const auto& e : *ef.profile.getProperty ("neutral", {}).getArray()) if (! e.hasProperty ("index")) allIndexed = false;
          check (allIndexed, "tone T4: every exported neutral entry carries its index");
          auto twin = juce::JSON::parse (juce::JSON::toString (full)); auto twinP = juce::JSON::parse (juce::JSON::toString (ef.profile));
          // the record's Attack (index 5) and Mix (index 8) both named "M" in record and profile; the profile's entries keep their indices
          for (const auto& c : *twin.getProperty ("controls", {}).getArray()) if ((int) c.getProperty ("index", -1) == 5 || (int) c.getProperty ("index", -1) == 8) c.getDynamicObject()->setProperty ("name", "M");
          for (const auto& e : *twinP.getProperty ("neutral", {}).getArray()) if ((int) e.getProperty ("index", -1) == 5 || (int) e.getProperty ("index", -1) == 8) e.getDynamicObject()->setProperty ("control", "M");
          juce::StringArray s4; juce::Array<juce::var> w4; juce::String n4;
          const auto why4 = toneWrites (twinP, twin, s4, w4, n4);
          check (why4.isEmpty() && s4.contains ("5:0.500000") && s4.contains ("8:1.000000"), "tone T4: two neutrals named 'M' are written at their own indices 5 and 8, each at its own norm (" + s4.joinIntoString (" ") + ")");
          // MUTANT CHECK by hand: with the index ignored both 'M' entries resolve by name to the first (5), and 8 is never written
          auto noIdx = juce::JSON::parse (juce::JSON::toString (twinP)); for (const auto& e : *noIdx.getProperty ("neutral", {}).getArray()) e.getDynamicObject()->removeProperty ("index");
          juce::StringArray s6; juce::Array<juce::var> w6; juce::String n6; toneWrites (noIdx, twin, s6, w6, n6);
          check (! s6.contains ("8:1.000000") && s6.contains ("5:1.000000"), "tone T4: without indices the second 'M' lands on the first's index at the wrong norm - the AMEK shape (" + s6.joinIntoString (" ") + ")");
          // an index that does not bear the name falls back to the name (a profile from another record)
          auto moved = juce::JSON::parse (juce::JSON::toString (ef.profile)); for (const auto& e : *moved.getProperty ("neutral", {}).getArray()) if (e.getProperty ("control", "") == "Attack") e.getDynamicObject()->setProperty ("index", 40);
          juce::StringArray s5; juce::Array<juce::var> w5; juce::String n5;
          check (toneWrites (moved, full, s5, w5, n5).isEmpty() && s5.contains ("5:0.500000"), "tone T4: a profile index that does not bear the name is ignored and the name resolves (Attack -> 5)"); }
        // SECTION 11 (T3b, 3 Oct): the installed version must be the record's; an unknown version on either side refuses too - a guard never guesses
        check (versionMismatch ("2.5.62", "2.5.62").isEmpty() && versionMismatch (" 2.5.62", "2.5.62 ").isEmpty()
               && versionMismatch ("2.5.62", "2.5.70").contains ("installed version 2.5.70 differs from the record's 2.5.62") && versionMismatch ("2.5.62", "2.5.70").contains ("section 11")
               && versionMismatch ("2.5.62", "").contains ("installed version is unknown") && versionMismatch ("", "2.5.62").contains ("record's version is unknown"),
               "tone T3b: the section 11 guard passes only an equal version, names both when they differ, and refuses an unknown version on either side");
    }
    const auto drive = exportCompProfile (record (16, true, false, "input_as_threshold", "certified"));
    check (drive.ok && drive.profile.getProperty ("topology", "") == "input_drive" && drive.profile.getProperty ("level_coupling", {}).getProperty ("gain_db_per_point", {}).size() == 16,
           "export X12: input-as-threshold is input_drive with level_coupling from the per-position quiet gain");
    {
        // X12c (ruled 4 Oct): the record's write-landing evidence makes a continuous-declared amount control stepped in the export when its 16 positions are the 16 detents
        auto ev = record (16, true, false, "", "certified"); auto* evo = new juce::DynamicObject(); evo->setProperty ("control", 7); evo->setProperty ("detents", 16); ev.getDynamicObject()->setProperty ("amountLanding", juce::var (evo));
        auto plan0 = ejmap::sweep::planFromFixture (record (16, true, false, "", "certified")); evo->setProperty ("control", plan0.thr);
        const auto ex = exportCompProfile (ev); const auto am = ex.profile.getProperty ("amount", {});
        check (ex.ok && (bool) am.getProperty ("stepped", false) && am.getProperty ("stepped_by_evidence", "").toString().contains ("16 values (k/15)"),
               "export X12c: amountLanding {detents 16} on a continuous-declared amount control with 16 positions at k/15 exports stepped: true, saying why (" + ex.refused + ")");
        evo->setProperty ("detents", 15);
        const auto ex2 = exportCompProfile (ev);
        check (ex2.ok && ! (bool) ex2.profile.getProperty ("amount", {}).getProperty ("stepped", true) && ex2.profile.getProperty ("amount", {}).hasProperty ("stepped_by_evidence_unresolved"), "export X12d: when the swept positions are not the detents the export stays continuous and says a re-sweep on the detents is needed");
    }
    const auto amt = exportCompProfile (record (16, true, false, "amount_as_threshold", "certified"));
    check (amt.ok && amt.profile.getProperty ("topology", "") == "input_drive" && amt.profile.getProperty ("level_coupling", {}).getProperty ("gain_db_per_point", {}).size() == 16,
           "export X12b (ruled 4 Oct): an amount_as_threshold sweep (RVox's Compression, a OneKnob) exports as input_drive with level_coupling, exactly like input_as_threshold");
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
        check (pkc.ok && ! pkd.ok, "pick P4 (v1.7: 12 dB, on the pick): L = +10 puts every position's 1 dB point more than 12 dB below L - refused; L = -50 is not (" + pkd.refused + ")");
        {
            // v1.7 (P5-P9): deep points on a copy of the profile - 4/5/6 at T + 4.0 / 5.3 / 6.7 (peak) on positions 2..13 only,
            // positions 0, 1, 14, 15 null at the deep levels; the hard-knee 4:1 device's own geometry (in_at_gr[g] = T + g/0.75).
            auto deep = juce::JSON::parse (juce::JSON::toString (prof));
            auto cv = deep.getProperty ("amount", {}).getProperty ("curve", {});
            for (int i = 0; i < cv.size(); ++i)
            {
                auto* g = cv[i].getProperty ("in_at_gr_dbfs", {}).getDynamicObject();
                const double T = -40.0 + 2.0 * i - 3.0103;
                for (int t : { 4, 5, 6 }) g->setProperty (juce::String (t), (i >= 2 && i <= 13) ? juce::var (T + t / 0.75) : juce::var());
            }
            // P5: 3.5 interpolates between the 3 and 4 dB points of a position
            const auto v35 = inAtGr (cv[5], 3.5), v3 = inAtGr (cv[5], 3.0), v4 = inAtGr (cv[5], 4.0);
            check (v35 && v3 && v4 && std::abs (*v35 - (*v3 + *v4) / 2.0) < 1e-9, "pick P5: a fractional g (3.5) interpolates between its adjacent points 3 and 4, not between 1..3");
            // P6: the clamp applies to the PICK's own interpolated 1 dB point, 12 dB; a pick between two positions whose 1 dB points are 11 and 13 dB below L is refused only if the interpolated value is
            const auto pk4 = pickPosition (deep, -18.0, 4.0);
            check (pk4.ok && pk4.i1 >= 0 && std::abs (pk4.expectedGrDb - 4.0) < 1e-9 && ! pk4.fellBackToMeasured && std::abs (pk4.pickOneDb - (-18.0 - 4.0 / 0.75 + 1.0 / 0.75)) < 0.05,
                   "pick P6: at g = 4 the pick interpolates between the bracketing positions and the clamp reads the PICK's own 1 dB point (" + juce::String (pk4.pickOneDb, 2) + " vs L -18: 4.0 dB below, inside 12)");
            // P7: the deep-null fill - a position with no 5 dB point between two that carry it is filled across the norm axis; the pick says so
            auto gap = juce::JSON::parse (juce::JSON::toString (deep));
            gap.getProperty ("amount", {}).getProperty ("curve", {})[8].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()->setProperty ("5", juce::var());
            const double L5 = -40.0 + 2.0 * 8 - 3.0103 + 5.0 / 0.75;            // exactly position 8's (now missing) 5 dB point
            const auto pk5 = pickPosition (gap, L5, 5.0);
            check (pk5.ok && (pk5.i0 == 8 || pk5.i1 == 8) && pk5.filledAcrossNorm && std::abs (pk5.expectedGrDb - 5.0) < 1e-9,
                   "pick P7: a deep level null on one position is filled across the norm axis from its neighbours and the pick is marked filledAcrossNorm (points " + juce::String (pk5.i0) + "/" + juce::String (pk5.i1) + ")");
            // P7b-P7d (v1.9 section 6.3 step 1, "v1.8, clarified"): a FRACTIONAL ask borrows the missing POINT, not the level. Position 8's 4 dB
            // point is nulled and its own 3 dB point moved 1.5 dB quieter than the parallel curve: a 3.5 ask must read between ITS OWN 3 dB value
            // and the borrowed 4 dB value, so its answer differs from a wholesale re-read of 3.5 from the neighbours by 0.75 dB.
            {
                auto bp = juce::JSON::parse (juce::JSON::toString (deep)); auto c8 = bp.getProperty ("amount", {}).getProperty ("curve", {})[8].getProperty ("in_at_gr_dbfs", {}).getDynamicObject();
                const double own3 = (double) c8->getProperty ("3") - 1.5; c8->setProperty ("3", own3); c8->setProperty ("4", juce::var());
                const double n7 = (double) deep.getProperty ("amount", {}).getProperty ("curve", {})[7].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0), n9 = (double) deep.getProperty ("amount", {}).getProperty ("curve", {})[9].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0);
                const double borrowed4 = (n7 + n9) / 2.0, expectAt8 = own3 + (borrowed4 - own3) * 0.5;      // between its OWN 3 and the borrowed 4
                const double wholesale = ((double) deep.getProperty ("amount", {}).getProperty ("curve", {})[7].getProperty ("in_at_gr_dbfs", {}).getProperty ("3", 0.0) + n7) / 2.0 * 0.5
                                       + ((double) deep.getProperty ("amount", {}).getProperty ("curve", {})[9].getProperty ("in_at_gr_dbfs", {}).getProperty ("3", 0.0) + n9) / 2.0 * 0.5;   // 3.5 re-read from the neighbours
                const auto pb = pickPosition (bp, expectAt8, 3.5);
                check (pb.ok && (pb.i0 == 8 || pb.i1 == 8) && std::abs (wholesale - expectAt8 - 0.75) < 1e-6 && std::abs (pb.norm - 8.0 / 15.0) < 1e-6 && pb.filledAcrossNorm,
                       "pick P7b: a 3.5 dB ask on a position with a null 4 dB point keeps the position's OWN 3 dB value and borrows only the 4 dB point - L = that value lands exactly on position 8 (norm " + juce::String (pb.norm, 4) + "); a wholesale re-read of 3.5 from the neighbours would sit 0.75 dB away");
                const auto pw = pickPosition (bp, wholesale, 3.5);
                check (pw.ok && ! (pw.i1 < 0 && pw.i0 == 8) && std::abs (pw.norm - 8.0 / 15.0) > 1e-3, "pick P7c: the wholesale value does NOT land on position 8 (the mutant's answer is a different setting)");
                // P7d: a SHALLOW null bound is never borrowed - position 8 with its 3 dB point null cannot serve a 3.5 ask; the fill does not invent it
                auto sb = juce::JSON::parse (juce::JSON::toString (deep)); sb.getProperty ("amount", {}).getProperty ("curve", {})[8].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()->setProperty ("3", juce::var());
                const auto ps = pickPosition (sb, expectAt8, 3.5);
                check (ps.ok && ps.i0 != 8 && ps.i1 != 8, "pick P7d: a position whose SHALLOW bound (3 dB) is null is skipped for a 3.5 ask - a shallow null is never borrowed");
            }
            // P8: no position carries 6 dB -> the deepest carried level (5) answers, and the REPORTED figure is 5
            auto no6 = juce::JSON::parse (juce::JSON::toString (deep));
            { auto c6 = no6.getProperty ("amount", {}).getProperty ("curve", {}); for (int i = 0; i < c6.size(); ++i) c6[i].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()->setProperty ("6", juce::var()); }
            const auto pk6 = pickPosition (no6, -18.0, 6.0);
            check (pk6.ok && pk6.fellBackToMeasured && std::abs (pk6.expectedGrDb - 5.0) < 1e-9 && pk6.note.contains ("deepest measured level answers: 5.0 dB"),
                   "pick P8: with no 6 dB point anywhere the deepest carried level (5 dB) answers and the figure reported is 5, said in the note");
            // P8b: a shallow null is never filled - a position null at 2 dB cannot serve g = 2 (the v1.2 rule stands)
            auto sh = juce::JSON::parse (juce::JSON::toString (deep));
            sh.getProperty ("amount", {}).getProperty ("curve", {})[11].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()->setProperty ("2", juce::var());
            const auto pk2 = pickPosition (sh, -18.0, 2.0);
            check (pk2.ok && pk2.i0 != 11 && pk2.i1 != 11 && ! pk2.filledAcrossNorm, "pick P8b: at 2 dB a null position is skipped, never filled");
            // P9: stepped - only listed detents, even at a deep level; the clamp on that detent's own 1 dB point
            auto st = juce::JSON::parse (juce::JSON::toString (deep)); st.getProperty ("amount", {}).getDynamicObject()->setProperty ("stepped", true);
            const auto pks = pickPosition (st, -18.0, 4.5);
            // the nearest detent at 4.5 dB is position 10 (its 4.5 point -17.01, 0.99 from L; position 9's is 1.01 away); at L = -18 that detent gives (-18 - T10) * 0.75 = 3.7575 dB (v1.8 reverse read), the expectation
            check (pks.ok && pks.i1 < 0 && pks.i0 == 10 && std::abs (pks.expectedGrDb - 3.7575) < 1e-3 && ! pks.expectedExtrapolated && pks.note.contains ("reverse read between its 3 and 4 dB points"),
                   "pick P9: a stepped control gets the nearest listed detent at a fractional deep g, never an interpolated norm, and expects what THAT detent gives at L (3.76, read between its 3 and 4 dB points), not the 4.5 asked for (" + juce::String (pks.expectedGrDb, 4) + ")");
            // THE REVERSE READ (v1.8 section 6.4 step 2, R1-R4): the GR a position gives AT a level, across all six points
            {
                const auto pt = juce::JSON::parse (R"json({"in_at_gr_dbfs": {"1": -30.0, "2": -26.0, "3": -23.0, "4": -21.0, "5": -19.5, "6": -18.5}})json");
                const auto r5 = grAtLevel (pt, -19.5), r45 = grAtLevel (pt, -20.25), rPast = grAtLevel (pt, -10.0), rBelow = grAtLevel (pt, -40.0), rNone = grAtLevel (juce::JSON::parse (R"json({"in_at_gr_dbfs": {"1": null}})json"), -20.0);
                check (r5.ok && std::abs (r5.gr - 5.0) < 1e-9 && ! r5.extrapolated && r45.ok && std::abs (r45.gr - 4.5) < 1e-9 && r45.lo == 4 && r45.hi == 5
                       && rPast.ok && std::abs (rPast.gr - 6.0) < 1e-9 && rPast.extrapolated && rBelow.ok && std::abs (rBelow.gr - 1.0) < 1e-9 && rBelow.extrapolated && ! rNone.ok,
                       "pick R1: the reverse read at a position's 5 dB point reports 5, halfway between its 4 and 5 points 4.5, past its deepest point the deepest level FLAGGED extrapolated, below its shallowest the same, and a position with no numeric point has none");
                auto pt3 = juce::JSON::parse (juce::JSON::toString (pt)); for (int k : { 4, 5, 6 }) pt3.getProperty ("in_at_gr_dbfs", {}).getDynamicObject()->setProperty (juce::String (k), juce::var());
                const auto r3 = grAtLevel (pt3, -20.25);
                check (r3.ok && std::abs (r3.gr - 3.0) < 1e-9 && r3.extrapolated, "pick R1b: with only 1-3 measured, a level past the 3 dB point reads 3 and says extrapolated - the read runs over whatever points the position carries");
                // R1c (v1.10): the read runs to 12 - a level at a position's 9 dB point reports 9, between 8 and 9 reports 8.5, past its 12 dB point 12 extrapolated; and the forward read interpolates 8.5 between 8 and 9
                const auto pt12 = juce::JSON::parse (R"json({"in_at_gr_dbfs": {"1": -30.0, "2": -26.0, "3": -23.0, "4": -21.0, "5": -19.5, "6": -18.5, "7": -17.5, "8": -16.5, "9": -15.5, "10": -14.5, "11": -13.5, "12": -12.5}})json");
                const auto r9 = grAtLevel (pt12, -15.5), r85 = grAtLevel (pt12, -16.0), r13 = grAtLevel (pt12, -10.0);
                check (r9.ok && std::abs (r9.gr - 9.0) < 1e-9 && ! r9.extrapolated && r85.ok && std::abs (r85.gr - 8.5) < 1e-9 && r13.ok && std::abs (r13.gr - 12.0) < 1e-9 && r13.extrapolated
                         && inAtGr (pt12, 8.5) && std::abs (*inAtGr (pt12, 8.5) - (-16.0)) < 1e-9 && inAtGr (pt12, 12.0) && std::abs (*inAtGr (pt12, 12.0) - (-12.5)) < 1e-9 && inAtGr (pt12, 12.5) && std::abs (*inAtGr (pt12, 12.5) - (-12.5)) < 1e-9,
                       "pick R1c (v1.10): the reverse read reports 9 at the 9 dB point, 8.5 halfway to 8, 12 extrapolated past the 12 dB point; the forward read gives -16.0 at 8.5 and the 12 dB point at and beyond 12 - no second list stops at 6");
                // R2: a stepped unit whose nearest detent gives 2.6 at L expects 2.6 - a measured 2.6 passes, and it is NOT failed against 2.0 (0.6 out)
                const auto stepU = juce::JSON::parse (R"json({"detector_f": 0.0, "measured": {"steps_dbfs": [-63.01, -3.01, 2]}, "amount": {"stepped": true, "curve": [
                    {"norm": 0.0, "display": "a", "in_at_gr_dbfs": {"1": -30.0, "2": -26.0, "3": -23.0, "4": -21.0, "5": -19.5, "6": -18.5}},
                    {"norm": 1.0, "display": "b", "in_at_gr_dbfs": {"1": -24.0, "2": -20.0, "3": -17.0, "4": -15.0, "5": -13.5, "6": -12.5}}]}})json");
                const auto pb = pickPosition (stepU, -18.2, 2.0);
                check (pb.ok && pb.stepped && pb.i0 == 1 && pb.i1 < 0 && std::abs (pb.expectedGrDb - 2.6) < 1e-9 && ! pb.expectedExtrapolated && std::abs (2.6 - pb.expectedGrDb) <= 0.5 && std::abs (2.6 - 2.0) > 0.5,
                       "pick R2: the nearest detent (b) gives 2.6 dB at L = -18.2 (between its 2 and 3 dB points): the expectation is 2.6, so a measured 2.6 passes the 0.5 dB check instead of failing by 0.6 against the 2.0 asked for (" + juce::String (pb.expectedGrDb, 3) + ")");
                // R3: the read uses the DEEP points - at a level between the detent's 4 and 5 dB points the expectation is 4.5, which a 1-3 read would call 3 (extrapolated)
                const auto p45 = pickPosition (stepU, -14.25, 4.0);
                check (p45.ok && p45.i0 == 1 && std::abs (p45.expectedGrDb - 4.5) < 1e-9 && ! p45.expectedExtrapolated, "pick R3: at a level between the detent's 4 and 5 dB points the expectation is 4.5 - read across all six points, not 1-3 (" + juce::String (p45.expectedGrDb, 3) + ")");
                // R4: a continuous pick sits at L by construction: the expectation stays g; a stepped pick past the detent's deepest point expects the deepest level, flagged
                auto cont = juce::JSON::parse (juce::JSON::toString (stepU)); cont.getProperty ("amount", {}).getDynamicObject()->setProperty ("stepped", false);
                const auto pc = pickPosition (cont, -18.2, 2.0), pPast = pickPosition (stepU, -12.0, 6.0);   // L = -12: past b's 6 dB point (-12.5); b's 1 dB point 12.0 below, inside every clamp
                check (pc.ok && ! pc.stepped && std::abs (pc.expectedGrDb - 2.0) < 1e-9 && ! pc.expectedExtrapolated && pPast.ok && pPast.stepped && std::abs (pPast.expectedGrDb - 6.0) < 1e-9 && pPast.expectedExtrapolated && pPast.note.contains ("extrapolated"),
                       "pick R4: a continuous pick expects g (it sits at L); a stepped pick past its detent's deepest point expects that deepest level and says extrapolated");
            }
            // P10: THE CLAMP IS ON THE PICK, 12 dB. A soft unit: position A 1 dB at -24 / 2 dB at -20, position B 1 dB at -31 / 2 dB at -16.
            // For L = -18, g = 2 the pick sits halfway (t = 0.5): its own 1 dB point is -27.5, 9.5 dB below L - inside 12 (an 8 dB
            // clamp refuses it), while B's own 1 dB point is 13 dB below L (a neighbour-checking clamp drops B and loses the bracket).
            const auto soft = juce::JSON::parse (R"json({"detector_f": 0.0, "measured": {"steps_dbfs": [-63.01, -3.01, 2]}, "amount": {"stepped": false, "curve": [
                {"norm": 0.2, "display": "a", "in_at_gr_dbfs": {"1": -24.0, "2": -20.0, "3": -17.0}},
                {"norm": 0.4, "display": "b", "in_at_gr_dbfs": {"1": -31.0, "2": -16.0, "3": -12.0}},
                {"norm": 0.9, "display": "c", "in_at_gr_dbfs": {"1": -8.0, "2": -5.0, "3": -4.0}}]}})json");
            const auto pkS = pickPosition (soft, -18.0, 2.0);
            check (pkS.ok && pkS.i0 == 0 && pkS.i1 == 1 && std::abs (pkS.norm - 0.3) < 1e-9 && std::abs (pkS.pickOneDb - (-27.5)) < 1e-9,
                   "pick P10: the 12 dB clamp reads the PICK's own interpolated 1 dB point (-27.5, 9.5 below L) and keeps the bracket even though a bracketing position's own 1 dB point is 13 dB below L (" + pkS.refused + ")");
            check (! pickPosition (soft, -14.0, 2.0).ok, "pick P10b: moving L to -14 puts the pick's own 1 dB point 12.5 dB below it: refused by the clamp, with the number");
            // THE DEEP TONE LEVELS (v1.7 section 8, T4-T6): the levels a profile carries; a failing level nulled across ALL positions; the profile still passes
            const auto carried = deepLevelsCarried (deep);
            check (carried == std::vector<int> { 4, 5, 6 } && deepLevelsCarried (prof).empty(), "tone T4: the deep levels a profile carries are those with a numeric point on any position (4, 5, 6 here; none on the shallow-only profile)");
            auto nulledP = juce::JSON::parse (juce::JSON::toString (deep)); nullLevelAcrossPositions (nulledP, 5);
            const auto nc = nulledP.getProperty ("amount", {}).getProperty ("curve", {});
            bool all5null = true, any4 = false; for (int i = 0; i < nc.size(); ++i) { if (! nc[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("5", 0.0).isVoid()) all5null = false; if (nc[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("4", 0.0).isDouble()) any4 = true; }
            check (all5null && any4 && nc.size() == 16 && deepLevelsCarried (nulledP) == std::vector<int> { 4, 6 },
                   "tone T5: a failing deep level is null across ALL positions (the whole level comes out), the other levels and every position stay");
            check (toneExitCode (true, 3) == 0 && toneExitCode (false, 0) == 1, "tone T6: the profile's pass is the g = 2 check alone - three failed deep levels do not fail it, and a failed g = 2 does");
            // THE TEST L PER LEVEL (T7-T11, ruled 3 Oct): a level the clamp refuses at OUR L is not a level that failed. L = the median of
            // in_at_gr[g] over the positions carrying g, then those values by distance from it, within the sweep's range; the first whose
            // section 6.4 pick passes the 12 dB clamp. Four positions, 1->2 spacing 10/10/10/16 dB: at the fixed -18 the pick sits between
            // C and D and its own 1 dB point is 14 dB below (refused); the median (-24) picks between B and C, 10 dB (fine).
            const auto wide = juce::JSON::parse (R"json({"detector_f": 0.0, "measured": {"steps_dbfs": [-63.01, -3.01, 2]}, "amount": {"stepped": false, "curve": [
                {"norm": 0.2, "display": "a", "in_at_gr_dbfs": {"1": -40.0, "2": -30.0}},
                {"norm": 0.4, "display": "b", "in_at_gr_dbfs": {"1": -36.0, "2": -26.0}},
                {"norm": 0.6, "display": "c", "in_at_gr_dbfs": {"1": -32.0, "2": -22.0}},
                {"norm": 0.8, "display": "d", "in_at_gr_dbfs": {"1": -32.0, "2": -16.0}}]}})json");
            const auto tl = toneLevelFor (wide, 2.0);
            // L_ref for an RMS unit (f 0) is -18.4: refused (the pick's 1 dB point 13.6 below); -16 (2.4 away) refused; -22 (3.6 away) passes -> L = -22, tried 3.
            // The median (-24) is valid too but FARTHER from L_ref: a median-first rule answers -24 and goes red here (the control).
            check (! pickPosition (wide, -18.4, 2.0).ok && tl.ok && std::abs (tl.Lref - (-18.4)) < 1e-9 && std::abs (tl.L - (-22.0)) < 1e-9 && std::abs (tl.gapDb - (-3.6)) < 1e-9 && tl.tried == 3 && tl.pick.ok && pickPosition (wide, -24.0, 2.0).ok
                   && tl.rule.contains ("L_ref = -18.4 + f x (-6.2 + 18.4 - 3.01)") && tl.rule.contains ("detector_f 0.00 = -18.40") && tl.rule.contains ("4 positions") && tl.rule.contains ("-63.01..-3.01"),
                   "tone T7: L_ref (-18.4, the spec's vocal through f = 0) is refused by the clamp, the nearest valid candidate (-22, gap -3.6) answers - NOT the median (-24), which is valid but farther; L_ref, L and the gap are recorded and the rule stated (" + tl.rule + ")");
            {
                // T8: g = 2 under the same rule on the deep profile (an exported profile, detector_f from the record): L is L_ref itself when its pick is valid
                const auto t2 = toneLevelFor (deep, 2.0);
                const double fdeep = (double) deep.getProperty ("detector_f", 0.0), lref = toneLevelRef (fdeep);
                check (t2.ok && std::abs (t2.Lref - lref) < 1e-9 && std::abs (t2.L - lref) < 1e-9 && std::abs (t2.gapDb) < 1e-9 && t2.tried == 1 && std::abs (t2.pick.expectedGrDb - 2.0) < 1e-9,
                       "tone T8: the same rule applies to g = 2 - the recorded L is L_ref (" + juce::String (lref, 2) + " through detector_f " + juce::String (fdeep, 2) + ") when its pick is valid, gap 0, first try");
                check (std::abs (toneLevelRef (0.0) - (-18.4)) < 1e-9 && std::abs (toneLevelRef (1.0) - (-9.2103)) < 1e-4 && std::abs (toneLevelRef (0.43) - (-14.4484)) < 1e-3,
                       "tone T8b: L_ref is -18.4 for an RMS unit, -9.21 for a peak unit, -14.45 for CL 1B's f = 0.43 (the spec's example track through section 6.4 step 1)");
                // T8d (found live on SBC's 9..12 dB): when every position's g point sits above L_ref, the pick at L_ref is merely the nearest position
                // and does not give g there - the L rule skips it and takes the nearest point on the curve (gap recorded); a rule that accepts the
                // unbracketed pick tests the wrong thing (SBC: expected 9, read 7.77)
                const auto high = juce::JSON::parse (R"json({"detector_f": 0.0, "measured": {"steps_dbfs": [-63.01, -3.01, 2]}, "amount": {"stepped": false, "curve": [
                    {"norm": 0.5, "display": "a", "in_at_gr_dbfs": {"1": -14.0, "2": -12.0, "3": -11.0, "9": -8.0}},
                    {"norm": 0.8, "display": "b", "in_at_gr_dbfs": {"1": -13.0, "2": -11.0, "3": -10.0, "9": -6.0}},
                    {"norm": 1.0, "display": "c", "in_at_gr_dbfs": {"1": -12.0, "2": -10.0, "3": -9.0, "9": -4.5}}]}})json");
                const auto t9 = toneLevelFor (high, 9.0);
                check (pickPosition (high, -18.4, 9.0).ok && pickPosition (high, -18.4, 9.0).i1 < 0 && t9.ok && std::abs (t9.L - (-8.0)) < 1e-9 && std::abs (t9.gapDb - 10.4) < 1e-9 && t9.tried == 2 && t9.pick.i0 == 0,
                       "tone T8d: with every 9 dB point above L_ref (-18.4) the pick there is only the nearest position (unbracketed, clamp passes) - skipped; the nearest point on the curve (-8.0, position a, gap +10.4) is the test level");
                auto nof = juce::JSON::parse (juce::JSON::toString (wide)); nof.getDynamicObject()->removeProperty ("detector_f");
                check (! toneLevelFor (nof, 2.0).ok && toneLevelFor (nof, 2.0).reason.contains ("no detector_f"), "tone T8c: without detector_f the level cannot be anchored and the rule says so - it never assumes RMS");
            }
            // T9: a candidate outside the sweep's measured range is never tried - the range here excludes the median, the next by distance answers
            auto narrow = juce::JSON::parse (juce::JSON::toString (wide)); narrow.getProperty ("measured", {}).getDynamicObject()->setProperty ("steps_dbfs", juce::Array<juce::var> { -23.0, -19.0, 2 });
            const auto tn = toneLevelFor (narrow, 2.0);
            check (tn.ok && std::abs (tn.L - (-22.0)) < 1e-9 && tn.tried == 1, "tone T9: L_ref (-18.4) and -16 lie outside the measured range (-23..-19) and are not tried; the nearest in range (-22) answers (" + juce::String (tn.L, 2) + ", tried " + juce::String (tn.tried) + ")");
            // T10: CLAMP GEOMETRY - the positions' 1->2 spacings are 14 / 15 / 16 dB (the SMALLEST is what is reported), so no L inside the range gives a pick inside the 12 dB clamp; the note says it is the unit, not a failed check, with the spacing
            const auto geom = juce::JSON::parse (R"json({"detector_f": 0.0, "measured": {"steps_dbfs": [-63.01, -3.01, 2]}, "amount": {"stepped": false, "curve": [
                {"norm": 0.2, "display": "a", "in_at_gr_dbfs": {"1": -44.0, "2": -30.0}},
                {"norm": 0.5, "display": "b", "in_at_gr_dbfs": {"1": -39.0, "2": -24.0}},
                {"norm": 0.8, "display": "c", "in_at_gr_dbfs": {"1": -34.0, "2": -18.0}}]}})json");
            const auto tg = toneLevelFor (geom, 2.0);
            // T10 (v2.1: the flat 12 at g = 2 still bites): 1->2 spacings 14 / 15 / 16 dB, so no L on the curve passes at 2 dB; the reason says no valid L and names the flat allowance, never a failed check
            check (! tg.ok && tg.tried == 4 && tg.reason.contains ("no L within the measured range gives a valid pick") && tg.reason.contains ("the flat 12 dB") && ! tg.reason.contains ("failed"),
                   "tone T10: at g = 2 the flat 12 dB still applies - with 1->2 spacing 14-16 dB no L on the curve passes (4 tried: L_ref and the three values); the reason names the flat allowance, never a failed check (" + tg.reason + ")");
            check (! toneLevelFor (prof, 6.0).ok && toneLevelFor (prof, 6.0).reason.contains ("no position carries 6.0 dB"), "tone T10b: a level no position carries says so");
            check (tl.tried == 3 && tg.tried == 4, "tone T11: the tries are recorded (3 here: L_ref, -16, then -22; every candidate when none passes)");
            // THE ALLOWANCE (v2.1 section 6.4 step 4, A1-A6): past 3 dB it is the pick's OWN 1->g spacing + 3 dB; at 3 dB or under the flat 12
            {
                // A1: a CL 1B-shaped unit, 3.06 dB of input per dB of GR (1->9 = 24.48 dB, 1->12 = 33.66 dB): interior continuous picks at 9 and 12 are allowed; the old line (24 at 9, 30 at 12) refused both
                juce::Array<juce::var> cl; for (int i = 0; i < 6; ++i) { auto* o = new juce::DynamicObject(); o->setProperty ("norm", i / 5.0); o->setProperty ("display", juce::String (i)); auto* g = new juce::DynamicObject(); const double one = -50.0 + 4.0 * i; for (int t = 1; t <= 12; ++t) g->setProperty (juce::String (t), one + 3.06 * (t - 1)); o->setProperty ("in_at_gr_dbfs", juce::var (g)); cl.add (juce::var (o)); }
                auto clp = juce::JSON::parse (R"json({"detector_f": 0.43, "measured": {"steps_dbfs": [-63.01, -3.01, 2]}, "amount": {"stepped": false}})json"); clp.getProperty ("amount", {}).getDynamicObject()->setProperty ("curve", cl);
                const auto p9 = pickPosition (clp, -16.0, 9.0), p12 = pickPosition (clp, -8.0, 12.0);
                check (p9.ok && p9.i1 >= 0 && std::abs (p9.pickSpacingDb - 24.48) < 1e-6 && std::abs (p9.clampDb - 27.48) < 1e-6 && p12.ok && p12.i1 >= 0 && std::abs (p12.pickSpacingDb - 33.66) < 1e-6 && std::abs (p12.clampDb - 36.66) < 1e-6,
                       "clamp A1 (v2.1): a 3.06 dB/dB unit's interior picks at 9 dB (spacing 24.48, allowance 27.48) and 12 dB (33.66 / 36.66) are allowed - the old 12 + 2 x (g - 3) (24 / 30) refused both (" + p9.refused + p12.refused + ")");
                // A2: an interior continuous pick at any g > 3 passes by construction (L - pickOne == its spacing < spacing + 3)
                bool allPass = true; juce::String why2;
                for (double g : { 3.5, 4.0, 5.0, 7.0, 10.5, 12.0 }) for (double L : { -40.0, -30.0, -20.0, -12.0 }) { const auto pk = pickPosition (clp, L, g); if (pk.i1 >= 0 && ! pk.ok) { allPass = false; why2 << g << "@" << L << " "; } }
                check (allPass, "clamp A2: an interior continuous pick at any g > 3 always passes - its own 1 dB point sits exactly its spacing below L (" + why2 + ")");
                // A3: a STEPPED detent more than 3 dB louder than its own g point is refused, naming the allowance; one within 3 dB passes. Detents' 9 dB points: -25.52 .. -5.52 in 4 dB steps.
                auto st = juce::JSON::parse (juce::JSON::toString (clp)); st.getProperty ("amount", {}).getDynamicObject()->setProperty ("stepped", true);
                const auto sIn = pickPosition (st, -11.0, 9.0);                  // nearest detent's 9 dB point -9.52: the level sits 1.48 BELOW it (quieter): passes
                auto far = juce::JSON::parse (juce::JSON::toString (st)); { juce::Array<juce::var> two; two.add (cl[0]); two.add (cl[5]); far.getProperty ("amount", {}).getDynamicObject()->setProperty ("curve", two); }
                const auto sOut = pickPosition (far, -3.0, 9.0), sOut2 = pickPosition (far, -2.0, 9.0);   // detent 5's 9 dB point -5.52: L 2.52 above passes; 3.52 above is refused
                check (sIn.ok && sIn.stepped && std::abs (sIn.clampDb - 27.48) < 1e-6 && sOut.ok && ! sOut2.ok && sOut2.refused.contains ("allowance at 9.0 dB is this pick's own 1->9.0 dB spacing 24.5 + 3 dB") && sOut2.refused.contains ("more than 27.5 dB below L"),
                       "clamp A3: a stepped detent within 3 dB of where the level sits passes (2.5 above, or below); one more than 3 dB louder than its own 9 dB point (3.5 above) is refused, naming the allowance (" + sOut2.refused + ")");
                // A4: g = 2 and 3 use the flat 12 regardless of spacing
                const auto f2 = pickPosition (soft, -18.0, 2.0), f3 = pickPosition (clp, -30.0, 3.0);
                check (f2.ok && std::abs (f2.clampDb - 12.0) < 1e-9 && std::abs (f2.pickSpacingDb) < 1e-9 && f3.ok && std::abs (f3.clampDb - 12.0) < 1e-9 && ! pickPosition (soft, -14.0, 2.0).ok && pickPosition (soft, -14.0, 2.0).refused.contains ("the flat 12 dB"),
                       "clamp A4: at g = 2 and 3 the allowance is the flat 12 dB whatever the spacing (a mutant applying spacing + 3 there goes red: the soft unit's refused pick at -14 would pass)");
                // A5: the fallback level (deepest carried) sets the spacing when nothing carries g
                auto no12 = juce::JSON::parse (juce::JSON::toString (clp)); { auto c = no12.getProperty ("amount", {}).getProperty ("curve", {}); for (int i = 0; i < c.size(); ++i) c[i].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()->setProperty ("12", juce::var()); }
                const auto pf = pickPosition (no12, -8.0, 12.0);
                check (pf.ok && pf.fellBackToMeasured && std::abs (pf.expectedGrDb - 11.0) < 1e-9 && std::abs (pf.pickSpacingDb - 30.6) < 1e-6 && std::abs (pf.clampDb - 33.6) < 1e-6,
                       "clamp A5: when nothing carries 12 the deepest level (11) answers and the allowance is that level's own spacing + 3");
                // A6: the tone-check L rule records the allowance of the pick it chose
                const auto t12 = toneLevelFor (clp, 12.0);
                check (t12.ok && std::abs (t12.clampDb - 36.66) < 1e-6, "clamp A6: the L rule records the allowance of the pick it chose (36.66 at 12 dB on the 3.06 dB/dB unit)");
            }
        }
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
        check (! exportCompProfile (noDet).ok && exportCompProfile (noDet).refused.contains ("detector_f"), "detector D4 (v1.4): a record whose detector was never attempted is NOT exported - the follow-up measures it first (an attempted, unmeasured one exports unknown: X16c)");
        auto noRep = record (16, true, false, "", "certified"); noRep.getProperty ("thresholdSweep", {}).getDynamicObject()->removeProperty ("quality");
        check (! exportCompProfile (noRep).ok && exportCompProfile (noRep).refused.contains ("point_error_db"), "quality X21 (v1.4): a record without the hold-doubled repeat is NOT exported - required");
        // the monotonic self-check, both ways: equal neighbours across positions pass; a dip fails; equal within a position fails (strict).
        auto flat2 = record (16, true, false, "", "certified");
        { auto arr = flat2.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); arr[5].getDynamicObject()->setProperty ("1", arr[4].getProperty ("1", {})); arr[5].getDynamicObject()->setProperty ("2", arr[4].getProperty ("2", {})); arr[5].getDynamicObject()->setProperty ("3", arr[4].getProperty ("3", {})); }
        const auto fx2 = exportCompProfile (flat2);
        check (fx2.ok && (bool) fx2.profile.getProperty ("quality", {}).getProperty ("monotonic_across_positions", false),
               "monotonic M1 (v1.4): equal neighbouring positions are allowed across positions");
        // M1b (ruled 3 Oct): the order checks run on the EXPORTED values (0.1 dB) - two neighbours 0.03 dB apart in reverse order raw but equal at
        // export are not an order break; a check on the raw record would refuse this export (the mutant)
        auto near = record (16, true, false, "", "certified");
        { auto arr = near.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); const double v4 = (double) arr[4].getProperty ("1", {}); arr[4].getDynamicObject()->setProperty ("1", v4 + 0.01); arr[5].getDynamicObject()->setProperty ("1", v4 - 0.02); }
        const auto nx = exportCompProfile (near); const auto nc = nx.profile.getProperty ("amount", {}).getProperty ("curve", {});
        check (nx.ok && std::abs ((double) nc[4].getProperty ("in_at_gr_dbfs", {}).getProperty ("1", 0.0) - (double) nc[5].getProperty ("in_at_gr_dbfs", {}).getProperty ("1", 1.0)) < 1e-9 && (bool) nx.profile.getProperty ("quality", {}).getProperty ("monotonic_across_positions", false),
               "monotonic M1b: neighbours 0.03 dB apart in reverse order raw export equal (0.1 dB) and are NOT an order break - the checks judge what the server reads (" + nx.refused + ")");
        auto dip = record (16, true, false, "", "certified");
        { auto arr = dip.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); arr[5].getDynamicObject()->setProperty ("1", (double) arr[3].getProperty ("1", {}) - 1.0); }
        const auto dx = exportCompProfile (dip);
        check (! dx.ok && dx.refused.contains ("shallow in_at_gr order break") && dx.refused.contains ("1 dB values rise"),
               "monotonic M2 (v1.4, refusal since 3 Oct): a dip across positions at 1 dB REFUSES the export, the violation named (the server rejects it; a flag alone shipped a dead profile)");
        auto eq = record (16, true, false, "", "certified");
        { auto arr = eq.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {}); arr[4].getDynamicObject()->setProperty ("2", arr[4].getProperty ("1", {})); }
        const auto ex = exportCompProfile (eq);
        check (! ex.ok && ex.refused.contains ("shallow in_at_gr order break") && ex.refused.contains ("2 dB not strictly above 1 dB"),
               "monotonic M3 (v1.4, refusal since 3 Oct): within a position 1 < 2 < 3 is STRICT - an equal 2 dB point REFUSES the export, named");
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
        // RULE 1 (ruled 2 Oct, evening): the compressor stage word, WHOLE token; EMO-D5's pick; the pass-through repeat skip.
        check (compressorWord ("Comp Thresh") && compressorWord ("Compressor -> Threshold") && compressorWord ("Threshold Comp") && compressorWord ("COMPRESSION") && compressorWord ("Band 2 Compressor -> Threshold"),
               "rule1 K1: comp / compressor / compression as whole tokens, in any position, any case");
        check (! compressorWord ("Compare Thresh") && ! compressorWord ("Component Threshold") && ! compressorWord ("Compressor1") && ! compressorWord ("Gate Thresh") && ! compressorWord ("Leveller Thresh") && ! compressorWord ("Limiter Thresh"),
               "rule1 K2: a prefix is not the word - Compare, Component, Compressor1 are not it; neither is a leveller or a limiter stage");
        auto cands = [] (std::initializer_list<const char*> names) { std::vector<Plan::Candidate> v; int i = 0; for (auto* n : names) v.push_back ({ i++, n, {}, false }); return v; };
        const auto emo = cands ({ "Gate Thresh", "Comp Thresh", "Leveller Thresh", "DeEsser Thresh", "Limiter Thresh" });
        const auto pick = ruleOnePick (emo);
        check (pick && emo[(size_t) *pick].name == "Comp Thresh", "rule1 K3 (EMO-D5): among Gate / Comp / Leveller / DeEsser / Limiter the pick is Comp Thresh - the hand run's choice");
        check (! ruleOnePick (cands ({ "Mod Comp 1 Thresh dB", "Mod Comp 2 Thresh dB", "Opt B Comp 1 Threshold dB", "Opt B Comp 2 Threshold dB" })),
               "rule1 K4: four comp-worded candidates (Auto-Tune Vocal Compressor) decide nothing - needs_review");
        check (! ruleOnePick (cands ({ "Low Level Thresh", "High Level Thresh" })) && ! ruleOnePick (cands ({ "Compare Thresh", "Gate Thresh" })),
               "rule1 K5: no comp word (MaxxVolume) decides nothing; a Compare control is not a pick");
        check (ruleOnePick (cands ({ "Compressor -> Threshold", "Gate - Threshold", "Processor 1 - Threshold", "Processor 2 - Threshold" })) == std::optional<int> (0)
                 && ruleOnePick (cands ({ "Threshold Comp", "Threshold G/E" })) == std::optional<int> (0),
               "rule1 K6: MDynamics picks Compressor -> Threshold, Solid Dynamics picks Threshold Comp");
        Derived pt; pt.passThroughAtDefaults = true; Derived fl; fl.result = "flat"; fl.passThroughAtDefaults = false;
        check (! repeatWorthwhile (pt) && repeatWorthwhile (fl), "rule1 K7: the hold-doubled repeat is skipped ONLY for a pass-through first pass, never for a merely flat one");
        // K8 (ruled 2 Oct, evening): the stages line names its source - each stage's own engage switch at instantiate, or
        // "no engage control, not verified" - never a stage reading flat.
        const auto strip = juce::JSON::parse (R"json({"controls": [
            {"index": 0,  "name": "Gate On",      "numSteps": 2, "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}},
            {"index": 1,  "name": "Gate Thresh",  "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 0.0, "display": "-Inf"}},
            {"index": 15, "name": "Comp On",      "numSteps": 2, "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}},
            {"index": 16, "name": "Comp Thresh",  "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0"}},
            {"index": 30, "name": "Leveller On",  "numSteps": 2, "defaultOnInstantiate": {"normalised": 1.0, "display": "On"}},
            {"index": 31, "name": "Leveller Thresh", "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0"}},
            {"index": 40, "name": "Processor 1 - Threshold", "numSteps": 2147483647, "defaultOnInstantiate": {"normalised": 0.5, "display": "-20.0 dB"}}]})json");
        const auto st = cands ({ "Gate Thresh", "Comp Thresh", "Leveller Thresh", "Processor 1 - Threshold" });
        std::vector<Plan::Candidate> stc; int idx[] = { 1, 16, 31, 40 }; for (size_t i = 0; i < st.size(); ++i) stc.push_back ({ idx[i], st[i].name, {}, false });
        const auto line = stagesAtDefaultsLine (strip, stc, 16);
        check (line.contains ("Gate: off at instantiate (Gate On = Off)") && line.contains ("Leveller: ON at instantiate (Leveller On = On)")
                 && line.contains ("Processor: no engage control, not verified; Processor 1 - Threshold left at '-20.0 dB'") && ! line.contains ("Comp"),
               "rule1 K8: each stage's claim comes from its own switch at instantiate (off / ON, named), a stage with no switch is 'no engage control, not verified', the pick is not listed (" + line + ")");
    }
    {
        // INPUT-DRIVE AND ONE-KNOB COMPRESSORS (ruled 4 Oct, A1-A5): no threshold role -> the single amount/input control is the amount, flagged amount_as_threshold
        const auto rvox = juce::JSON::parse (R"json({"product": "RVox (s)", "controls": [
            {"index": 0, "name": "Compression", "unit": "dB", "numSteps": 2147483647, "range": {"min": -36.0, "max": 0.0}, "displayAt": {"0.000": "-36.0", "0.500": "-18.0", "1.000": "0.0"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0"}},
            {"index": 1, "name": "Gate", "unit": "dB", "numSteps": 2147483647, "range": {"min": -40.0, "max": 0.0}, "displayAt": {"0.000": "-Inf", "0.500": "-40.0", "1.000": "0.0"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "-Inf"}},
            {"index": 2, "name": "Gain", "unit": "dB", "numSteps": 2147483647, "range": {"min": -36.0, "max": 0.0}, "displayAt": {"0.000": "-36.0", "0.500": "-18.0", "1.000": "0.0"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0"}}]})json");
        const auto rp = planFromFixture (rvox);
        check (rp.ok && rp.thr == 0 && rp.thrName == "Compression" && rp.thrFlags.contains ("amount_as_threshold") && rp.quietReference && rp.candidates.empty() && rp.pickNote.contains ("amount_as_threshold"),
               "amount A1 (RVox): no threshold role; 'Compression' (an amount word, -36..0 dB) is the amount control, flagged amount_as_threshold; 'Gain' on an amount_only product is not (" + rp.why + ")");
        const auto mike = juce::JSON::parse (R"json({"product": "Empirical Labs Mike-E Comp", "controls": [
            {"index": 0, "name": "Preamp Gain", "unit": "dB", "numSteps": 2147483647, "range": {"min": 8.0, "max": 18.0, "endsNotNumeric": ["0.000"]}, "displayAt": {"0.000": "CLEAN", "0.500": "8 dB", "1.000": "18 dB"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "CLEAN"}},
            {"index": 3, "name": "Drive", "numSteps": 2147483647, "range": {"min": 0.0, "max": 10.0}, "displayAt": {"0.000": "0.00", "0.500": "5.00", "1.000": "10.00"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "5.00"}},
            {"index": 5, "name": "Ratio", "numSteps": 2147483647, "displayAt": {"0.000": "Bypass", "0.500": "4:1", "1.000": "NUKE"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "4:1"}},
            {"index": 8, "name": "Mix", "numSteps": 2147483647, "range": {"min": 0.0, "max": 10.0}, "displayAt": {"0.000": "0.00", "0.500": "5.00", "1.000": "10.00"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "10.00"}},
            {"index": 9, "name": "Out", "numSteps": 2147483647, "range": {"min": 0.0, "max": 10.0}, "displayAt": {"0.000": "0.00", "0.500": "5.00", "1.000": "10.00"}, "defaultOnInstantiate": {"normalised": 0.65, "display": "6.50"}}]})json");
        const auto mp2 = planFromFixture (mike);
        check (mp2.ok && mp2.thr == 3 && mp2.thrName == "Drive" && mp2.thrFlags.contains ("amount_as_threshold"), "amount A2 (Mike-E): 'Drive' is the amount; Preamp Gain (not exactly 'Gain'), Mix and Out are not (" + mp2.why + ")");
        const auto pump = juce::JSON::parse (R"json({"product": "OneKnob Pumper (s)", "controls": [
            {"index": 0, "name": "Pump", "numSteps": 2147483647, "range": {"min": 0.0, "max": 10.0}, "displayAt": {"0.000": "0.0", "0.500": "5.0", "1.000": "10.0"}, "defaultOnInstantiate": {"normalised": 0.7, "display": "7.0"}},
            {"index": 2, "name": "Rate", "numSteps": 10, "displayAt": {"0.000": "1/1", "0.500": "3/16", "1.000": "1/32"}, "defaultOnInstantiate": {"normalised": 0.3, "display": "1/4"}}]})json");
        check (! planFromFixture (pump).ok && planFromFixture (pump).why.contains ("rhythmic ducker"), "amount A3 (OneKnob Pumper): a Pump / Rate unit is not a level-dependent compressor - refused by name, said");
        const auto vac = juce::JSON::parse (R"json({"product": "Mixland Vac Attack", "controls": [
            {"index": 6, "name": "Left Reduction", "numSteps": 2147483647, "range": {"min": 0.0, "max": 10.0}, "displayAt": {"0.000": "0.0", "0.500": "5.0", "1.000": "10.0"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "0.0"}},
            {"index": 13, "name": "Right Reduction", "numSteps": 2147483647, "range": {"min": 0.0, "max": 10.0}, "displayAt": {"0.000": "0.0", "0.500": "5.0", "1.000": "10.0"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "0.0"}},
            {"index": 10, "name": "Power", "numSteps": 2147483647, "displayAt": {"0.000": "Off", "0.500": "Off", "1.000": "On"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "Off"}}]})json");
        const auto vp = planFromFixture (vac);
        check (vp.ok && vp.thr < 0 && vp.candidates.size() == 2 && vp.candidates[0].name == "Left Reduction", "amount A4 (Vac Attack): two amount controls ending in a channel token become candidates for the pair rule after the sweeps");
    }
    {
        // ZIP IS A ROLES CASE (ruled 2 Oct): a threshold-named control whose values are words is a mode switch, never a candidate.
        const auto zip = juce::JSON::parse (R"json({"controls": [
            {"index": 6,  "name": "Auto Threshold", "numSteps": 2147483647, "displayAt": {"0.000": "Disabled", "0.500": "Enabled", "1.000": "Enabled"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "Disabled"}},
            {"index": 17, "name": "Threshold", "unit": "dB", "numSteps": 2147483647, "displayAt": {"0.000": "-inf dB", "0.500": "-40 dB", "1.000": "0.0 dB"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0 dB"}},
            {"index": 3,  "name": "Ratio", "numSteps": 2147483647, "displayAt": {"0.000": "1:1", "0.500": "4:1", "1.000": "20:1"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "4:1"}}]})json");
        const auto zp = planFromFixture (zip);
        check (zp.ok && zp.thr == 17 && zp.candidates.empty() && zp.wordValuedDropped == juce::StringArray { "Auto Threshold" },
               "roles Z1: Zip's 'Auto Threshold' (Disabled / Enabled) is dropped as word-valued and Threshold is the single candidate (" + zp.why + ")");
        check (wordValued (zip.getProperty ("controls", {})[0]) && ! wordValued (zip.getProperty ("controls", {})[1]) && ! wordValued (juce::JSON::parse (R"({"name": "x"})")),
               "roles Z2: word-valued means every sampled text is a word with no digit; '-inf dB' and 'Off' are a level's own words; no sample is not judged");
        // Z3 (ruled 4 Oct, Pro-C 3 from Sean's run): a 2-step control printing 0 / 0 / 1 is a switch, never a threshold candidate - Threshold is the single candidate; a 2-step control printing two LEVELS is not a switch
        const auto proc = juce::JSON::parse (R"json({"controls": [
            {"index": 1, "name": "Threshold", "unit": "dB", "numSteps": 2147483647, "discrete": false, "displayAt": {"0.000": "-60.00 dB", "0.500": "-30.00 dB", "1.000": "0.00 dB"}, "defaultOnInstantiate": {"normalised": 0.733, "display": "-16.00 dB"}},
            {"index": 2, "name": "Auto Threshold", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "0", "0.500": "0", "1.000": "1"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "0"}},
            {"index": 3, "name": "Lock Auto Threshold", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "0", "0.500": "0", "1.000": "1"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "0"}},
            {"index": 4, "name": "Ratio", "numSteps": 2147483647, "displayAt": {"0.000": "1:1", "0.500": "4:1", "1.000": "20:1"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "4:1"}}]})json");
        const auto pp = planFromFixture (proc);
        check (pp.ok && pp.thr == 1 && pp.candidates.empty() && pp.wordValuedDropped == juce::StringArray { "Auto Threshold", "Lock Auto Threshold" },
               "roles Z3: Pro-C 3's 'Auto Threshold' and 'Lock Auto Threshold' (2 steps, 0 / 0 / 1) are digit-valued switches, dropped; Threshold is the single candidate (" + pp.why + ")");
        const auto twoLevels = juce::JSON::parse (R"json({"index": 9, "name": "Threshold Hi/Lo", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "-20 dB", "0.500": "-20 dB", "1.000": "0 dB"}})json");
        // Z4 (ruled 4 Oct, MaxxVolume (s) from Sean's run): a 2-step control named "... On" or printing Off / Off / On is a switch, never a candidate; the two real thresholds remain
        const auto maxx = juce::JSON::parse (R"json({"controls": [
            {"index": 0, "name": "Low Level Thresh", "unit": "dB", "numSteps": 2147483647, "displayAt": {"0.000": "-96.0", "0.500": "-30.0", "1.000": "0.0"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "-30.0"}},
            {"index": 4, "name": "High Level Thresh", "unit": "dB", "numSteps": 2147483647, "displayAt": {"0.000": "-48.0", "0.500": "-15.0", "1.000": "0.0"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "-15.0"}},
            {"index": 8, "name": "High Level Thresh On", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "Off", "0.500": "Off", "1.000": "On"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "On"}},
            {"index": 9, "name": "Low Level Thresh On", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "Off", "0.500": "Off", "1.000": "On"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "On"}}]})json");
        const auto mp = planFromFixture (maxx);
        check (mp.wordValuedDropped == juce::StringArray { "High Level Thresh On", "Low Level Thresh On" } && mp.candidates.size() == 2,
               "roles Z4 (MaxxVolume): 'High Level Thresh On' / 'Low Level Thresh On' (2 steps, Off / Off / On) are switches, dropped; the two thresholds remain candidates (" + juce::String ((int) mp.candidates.size()) + ")");
        check (switchControl (juce::JSON::parse (R"json({"name": "Comp Enable", "numSteps": 2, "displayAt": {"0.000": "0.0 dB", "0.500": "0.0 dB", "1.000": "6.0 dB"}})json"))
                 && switchControl (juce::JSON::parse (R"json({"name": "Mode", "numSteps": 2, "displayAt": {"0.000": "off", "0.500": "off", "1.000": "ON"}})json"))
                 && ! switchControl (juce::JSON::parse (R"json({"name": "Threshold On Axis", "numSteps": 2, "displayAt": {"0.000": "-20 dB", "0.500": "-20 dB", "1.000": "0 dB"}})json"))
                 && ! switchControl (juce::JSON::parse (R"json({"name": "Comp On", "numSteps": 2147483647, "displayAt": {"0.000": "Off", "0.500": "Off", "1.000": "On"}})json")),
               "roles Z4b: '... Enable' by name and On/Off by value are switches; a name that merely contains 'On' is not; a continuous control is never a switch (two steps is part of the rule)");
        const auto contDigits = juce::JSON::parse (R"json({"index": 9, "name": "Threshold Mix", "numSteps": 2147483647, "discrete": false, "displayAt": {"0.000": "0", "0.500": "0", "1.000": "1"}})json");
        check (digitSwitch (proc.getProperty ("controls", {})[1]) && ! digitSwitch (twoLevels) && ! wordValued (twoLevels) && ! digitSwitch (proc.getProperty ("controls", {})[0]) && ! digitSwitch (contDigits),
               "roles Z3b: a two-step control printing two LEVELS (-20 dB / 0 dB) is not a digit switch and stays a candidate; a CONTINUOUS control that happens to print 0 / 0 / 1 is not one either (two steps is part of the rule)");
    }
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
/** ONE LOOP (ruled 2 Oct, docs/STRANGER_MAC_TEST.md): every product ends in exactly one of five states, and a product that
    finishes the loop has an export or a named reason for not having one. The rules in EjmapLoop.h, branch by branch. */
/** CATEGORIES MERGE (2 Oct): the server's reply carries no mark_keys; the merge stamps them from the scan rows, on a
    served entry and on an existing entry without them, and leaves an entry that has them alone. */
void testCategoriesMerge()
{
    using namespace ejmap::categoriesmerge;
    auto obj = [] (std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty (k, v); return juce::var (o); };
    std::map<juce::String, ProductKeys> keys;
    keys["elysia mpressor|elysia"] = { juce::StringArray { "AudioUnit|49696d78" }, juce::StringArray { "AudioUnit:Effects/aufx,mprs,Elys" } };
    keys["known comp|vendor"]      = { juce::StringArray { "AudioUnit|11112222" }, juce::StringArray { "AudioUnit:Effects/aufx,KNWN,Vndr" } };
    juce::Array<juce::var> oldKeys { "AudioUnit|deadbeef" };
    juce::var doc = obj ({ { "products", obj ({ { "known comp|vendor", obj ({ { "name", "Known Comp" }, { "category", "compressor" }, { "mark_keys", oldKeys } }) },
                                                 { "bare|vendor", obj ({ { "name", "Bare" }, { "category", "eq" } }) } }) } });
    keys["bare|vendor"] = { juce::StringArray { "AudioUnit|33334444" }, juce::StringArray { "AudioUnit:Effects/aufx,BARE,Vndr" } };
    const auto served = obj ({ { "elysia mpressor|elysia", obj ({ { "name", "elysia mpressor" }, { "category", "compressor" } }) },
                               { "nowhere|vendor", obj ({ { "name", "Nowhere" }, { "category", "reverb" } }) } });
    const int n = mergeServed (doc, served, keys);
    const auto P = doc.getProperty ("products", {});
    check (n == 2 && P.getProperty ("elysia mpressor|elysia", {}).getProperty ("mark_keys", {}).size() == 1
             && P.getProperty ("elysia mpressor|elysia", {}).getProperty ("mark_keys", {})[0].toString() == "AudioUnit|49696d78"
             && P.getProperty ("elysia mpressor|elysia", {}).getProperty ("members", {})[0].toString() == "AudioUnit:Effects/aufx,mprs,Elys",
           "categories C1: a served entry is stamped with the scan's mark_keys and members for its product key");
    check (P.getProperty ("known comp|vendor", {}).getProperty ("mark_keys", {})[0].toString() == "AudioUnit|deadbeef",
           "categories C2: an existing entry that has mark_keys keeps them (the mapper's categorisation is not rewritten)");
    check (P.getProperty ("bare|vendor", {}).getProperty ("mark_keys", {}).size() == 1 && P.getProperty ("bare|vendor", {}).getProperty ("mark_keys", {})[0].toString() == "AudioUnit|33334444",
           "categories C3: an existing entry WITHOUT mark_keys (a file the reply alone wrote) is stamped on the next merge");
    check (! P.getProperty ("nowhere|vendor", {}).hasProperty ("mark_keys") && P.getProperty ("nowhere|vendor", {}).getProperty ("category", "") == "reverb",
           "categories C4: a product the scan does not know gets no keys and is kept as served");
}

void testLoopOutcomes()
{
    using namespace ejmap::loop;
    auto obj = [] (std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty (k, v); return juce::var (o); };
    // refused at a stage
    const auto refusal = obj ({ { "identity", "AudioUnit|1|1" }, { "thresholdRefusal", obj ({ { "stage", "plan" }, { "reason", "0 threshold roles" } }) } });
    auto o = outcomeForRecord (refusal);
    check (o.state == "refused" && o.reason.contains ("stage plan") && o.reason.contains ("0 threshold roles") && ! o.exportPending, "loop L1: a refusal record is refused at its stage with its reason");
    // a tuner with candidates: recorded; without: needs_review
    juce::Array<juce::var> pcs; pcs.add (obj ({ { "index", 4 } }));
    check (outcomeForRecord (obj ({ { "schema", "ej_cert_tuner/1" }, { "pitchCandidates", pcs } })).state == "recorded"
             && outcomeForRecord (obj ({ { "schema", "ej_cert_tuner/1" } })).state == "needs_review", "loop L2: a tuner record with pitch candidates is recorded, without them needs_review");
    // several candidates: needs_review unless Rule 1 decided (pickedCandidate + ruleDecided, the pick certified profile-grade)
    juce::Array<juce::var> cs; cs.add (obj ({})); cs.add (obj ({}));
    o = outcomeForRecord (obj ({ { "thresholdCandidates", cs } }));
    check (o.state == "needs_review" && ! o.exportPending && o.reason.contains ("2 threshold candidates") && o.reason.contains ("nobody picks"), "loop L3: several candidates end as needs_review, never a pick, when no rule decides");
    {
        juce::Array<juce::var> grid; for (int L = -60; L <= 0; L += 2) grid.add ((double) L);
        juce::Array<juce::var> cc; cc.add (obj ({ { "index", 1 }, { "name", "Gate Thresh" }, { "thresholdSweep", obj ({ { "result", "certified" }, { "hold_s", 2.5 }, { "tone", obj ({ { "levels_dbfs", grid } }) } }) } }));
        cc.add (obj ({ { "index", 16 }, { "name", "Comp Thresh" }, { "thresholdSweep", obj ({ { "result", "certified" }, { "hold_s", 2.5 }, { "tone", obj ({ { "levels_dbfs", grid } }) } }) } }));
        const auto decided = obj ({ { "thresholdCandidates", cc }, { "pickedCandidate", obj ({ { "index", 16 }, { "name", "Comp Thresh" } }) }, { "ruleDecided", obj ({ { "rule", "R1" } }) } });
        check (outcomeForRecord (decided).exportPending, "loop L3b (Rule 1): a candidates record whose pick certified on the profile grid is export-pending through its single view");
        const auto noRule = obj ({ { "thresholdCandidates", cc }, { "pickedCandidate", obj ({ { "index", 16 }, { "name", "Comp Thresh" } }) } });
        check (! outcomeForRecord (noRule).exportPending, "loop L3c: a pick without ruleDecided (a hand --candidate) is not the batch's to export");
    }
    // a flat sweep: needs_review with the result
    o = outcomeForRecord (obj ({ { "thresholdSweep", obj ({ { "result", "flat" }, { "reason", "no two positions differ" } }) } }));
    check (o.state == "needs_review" && o.reason.contains ("flat") && o.reason.contains ("no two positions differ"), "loop L4: a non-certified sweep is needs_review with the sweep's result and reason");
    // certified on the 3-level certification sweep: needs_review (a profile is needed)
    juce::Array<juce::var> three { -24.0, -12.0, -6.0 }, grid; for (int L = -60; L <= 0; L += 2) grid.add ((double) L);
    o = outcomeForRecord (obj ({ { "thresholdSweep", obj ({ { "result", "certified" }, { "hold_s", 1.5 }, { "tone", obj ({ { "levels_dbfs", three } }) } }) } }));
    check (o.state == "needs_review" && ! o.exportPending && o.reason.contains ("certification-grade"), "loop L5: certified on three levels is not profile-grade: needs_review, no export attempted");
    // certified on the profile grid: export pending
    o = outcomeForRecord (obj ({ { "thresholdSweep", obj ({ { "result", "certified" }, { "hold_s", 2.5 }, { "tone", obj ({ { "levels_dbfs", grid } }) } }) } }));
    check (o.exportPending, "loop L6: certified on the 31-level 2.5 s grid is export-pending");
    // after the export
    check (outcomeAfterExport (false, "detector_f not measured", false, "").state == "needs_review" && outcomeAfterExport (false, "detector_f not measured", false, "").reason.contains ("detector_f"),
           "loop L7: an export the exporter refused is needs_review with the exporter's reason");
    check (outcomeAfterExport (true, "", false, "exit 4").state == "needs_review" && outcomeAfterExport (true, "", false, "exit 4").reason.contains ("tone check"),
           "loop L7b: exported but no tone check is needs_review, never exported");
    check (outcomeAfterExport (true, "", true, "").state == "exported", "loop L7c: export + tone check = exported (pass or fail of the check is a result on the profile)");
    // held
    check (outcomeHeld (true, false, "PACE").state == "held" && outcomeHeld (true, false, "PACE").reason.contains ("licence") && outcomeHeld (false, true, "APB").reason.contains ("hardware"),
           "loop L8: held names licence or hardware");
    // the row invariant
    const auto good = makeRow ("AudioUnit|1|1", "P", "compressor", outcomeAfterExport (true, "", true, ""), "r.json", "p.json", "t.json", "now");
    check (rowViolation (good).isEmpty(), "loop L9: an exported row with profile and tone check files satisfies the invariant");
    const auto noFile = makeRow ("AudioUnit|1|1", "P", "compressor", outcomeAfterExport (true, "", true, ""), "r.json", "", "t.json", "now");
    Outcome bad; bad.state = "exported"; bad.reason = "";
    Outcome odd; odd.state = "done"; odd.reason = "x";
    check (rowViolation (noFile).contains ("without a profile") && rowViolation (makeRow ("i", "P", "c", bad, "", "p", "t", "now")).contains ("no reason")
             && rowViolation (makeRow ("i", "P", "c", odd, "", "", "", "now")).contains ("not one of the"),
           "loop L10: the invariant refuses an export without its file, a state without a reason, and a state outside the named ones");
    // merge by identity, count
    juce::var rows = juce::Array<juce::var>();
    rows = mergeRow (rows, makeRow ("A", "a", "compressor", outcomeHeld (true, false, "x"), "", "", "", "t1"));
    rows = mergeRow (rows, makeRow ("B", "b", "compressor", outcomeForRecord (refusal), "r", "", "", "t1"));
    rows = mergeRow (rows, makeRow ("A", "a", "compressor", outcomeAfterExport (true, "", true, ""), "r", "p", "t", "t2"));
    const auto c = count (rows);
    check (rows.size() == 2 && c.rows == 2 && c.exported == 1 && c.refused == 1 && c.held == 0 && findRow (rows, "A").getProperty ("at", "") == "t2",
           "loop L11: a later row for the same identity replaces the earlier (a resumed batch rewrites what it finished); counts follow");
    { Outcome u; u.state = "unmapped"; u.reason = "no map at this build";
      check (rowViolation (makeRow ("AudioUnit|1|2.0", "elysia mpressor", "compressor", u, {}, {}, {}, "t")).contains ("not one of the"),
             "loop L16 (re-ruled 2 Oct): 'unmapped' is NOT a state - the row carries the map as a field; certification does not wait on a map"); }
    {
        // THE SCAN'S LICENCE EVIDENCE, CARRIED FORWARD (L17-L19, ruled 2 Oct): three paths.
        QuarantinedBundle st; st.bundle = "/Library/Audio/Plug-Ins/VST3/SSL Native Drumstrip v6.vst3"; st.licence = true; st.products.add ("SSL Native Drumstrip v6"); st.reason = "activation window at scan (PACE [pid 1])"; st.at = "t";
        QuarantinedBundle qa; qa.bundle = "/x/ANA2.vst3"; qa.products.add ("ANA2"); qa.reason = "hang_in_findAllTypesForFile";
        const std::vector<QuarantinedBundle> stops { st, qa };
        const auto c1 = carriedLicenceStop (stops, "SSL Native Drumstrip v6");
        const auto c1m = carriedLicenceStop (stops, "SSL Native Drumstrip v6 (m)");
        check (c1 && c1m && outcomeCarriedLicence (*c1).state == "needs_licence" && outcomeCarriedLicence (*c1).reason.contains ("carried forward from the scan") && outcomeCarriedLicence (*c1).reason.contains ("not loaded again"),
               "loop L17: a product whose bundle raised an activation window at the scan is needs_licence in the batch, carried forward, NOT loaded again; a (m)/(s) suffix does not break the match");
        check (! carriedLicenceStop (stops, "ANA2") && ! carriedLicenceStop (stops, "bx_opto"),
               "loop L18: a bundle quarantined for a hang is not a licence stop, and a product that loaded fine at the scan runs with no flag");
        const auto winRef = obj ({ { "identity", "AudioUnit|2|1" }, { "thresholdRefusal", obj ({ { "stage", "window" }, { "reason", "UNLICENSED ON HOST: a window appeared in the probe's tree (PACE [pid 9])" } }) } });
        const auto ow = outcomeForRecord (winRef);
        check (ow.state == "needs_licence" && ow.reason.contains ("window at the probe's load") && ow.reason.contains ("PACE [pid 9]") && ! ow.exportPending,
               "loop L19: a window at the probe's load in the batch is needs_licence (the window watch guards batch loads: a licence can vanish between scan and batch)");
        // KNOWN LICENCE, NOT LOADED (L19b-L19d, ruled 3 Oct): the tone check (and anything else that loads) asks before loading.
        const auto rowsK = juce::JSON::parse (R"json([{"product": "Tube-Tech CL 1B", "state": "needs_licence", "reason": "needs_licence: window at the probe's load (PACE [pid 3]); the export stands"}, {"product": "bx_opto", "state": "exported", "reason": "x"}])json");
        const auto k1 = knownLicenceStop (stops, rowsK, "SSL Native Drumstrip v6 (s)", false), k2 = knownLicenceStop (stops, rowsK, "Tube-Tech CL 1B", false);
        check (k1.contains ("known from the scan") && k1.contains ("PACE [pid 1]") && k1.contains ("not loaded") && k2.contains ("known from this folder's outcomes") && k2.contains ("PACE [pid 3]") && ! k2.contains ("the export stands"),
               "loop L19b: a product the scan stopped, or one with a needs_licence row in this folder, is known to need a licence and is not loaded; the reason names its source (" + k1 + " / " + k2 + ")");
        check (knownLicenceStop (stops, rowsK, "bx_opto", false).isEmpty() && knownLicenceStop (stops, rowsK, "ANA2", false).isEmpty(),
               "loop L19c: an exported product and a hang-quarantined bundle are not known licence stops: they load");
        check (knownLicenceStop (stops, rowsK, "Tube-Tech CL 1B", true).isEmpty() && knownLicenceStop (stops, rowsK, "SSL Native Drumstrip v6", true).isEmpty(),
               "loop L19d: --retry-licence is the only way past a known licence stop");
        // RE-SWEEP OR RE-DERIVE (L19q-L19u, ruled 4 Oct): the follow-up decides from the plan under this build against the plan the record was swept under
        {
            using ejmap::sweep::Plan;
            auto rvoxRec = juce::JSON::parse (R"json({"product": "RVox (s)", "controls": [{"index": 0, "name": "Compression", "unit": "dB", "numSteps": 2147483647, "range": {"min": -36.0, "max": 0.0}, "displayAt": {"0.000": "-36.0", "0.500": "-18.0", "1.000": "0.0"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "0.0"}}],
                                                      "thresholdRefusal": {"stage": "plan", "reason": "not swept: 0 controls hold the threshold role (class amount_only); deferred"}})json");
            const auto d1 = planDiffers (rvoxRec, ejmap::sweep::planFromFixture (rvoxRec));
            check (d1.resweep && d1.why.contains ("refused at plan under the batch build") && d1.why.contains ("this build plans [0] Compression"), "loop L19q: a record refused at plan that now plans is re-swept, the row saying why (" + d1.why + ")");
            auto licRef = juce::JSON::parse (juce::JSON::toString (rvoxRec)); licRef.getProperty ("thresholdRefusal", {}).getDynamicObject()->setProperty ("stage", "reference");
            check (! planDiffers (licRef, ejmap::sweep::planFromFixture (licRef)).resweep, "loop L19r: a refusal at any other stage is --retry-refused's business, not the plan's");
            // a decided record whose pick is still one of this build's candidates: no re-sweep (EMO-D5 under Rule 1 keeps only the candidate it swept)
            Plan nowP; nowP.ok = true; nowP.thr = -1; for (int i : { 1, 16, 30 }) nowP.candidates.push_back ({ i, "c" + juce::String (i), {}, false });
            auto emo = juce::JSON::parse (R"json({"product": "EMO-D5 (s)", "controls": [{"index": 16, "name": "Comp Thresh"}], "thresholdCandidates": [{"index": 16, "name": "Comp Thresh", "thresholdSweep": {"result": "certified", "sweptControl": {"index": 16, "refineRounds": 1}, "positionNorms": [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9], "inAtGr": [{"1": -30, "2": -28}, {"1": -28, "2": -26}, {"1": -26, "2": -24}, {"1": -24, "2": -22}, {"1": -22, "2": -20}, {"1": -20, "2": -18}, {"1": -18, "2": -16}, {"1": -16, "2": -14}, {"1": -14, "2": -12}]}}], "pickedCandidate": {"index": 16}, "ruleDecided": {"rule": "R1"}})json");
            check (! planDiffers (emo, nowP).resweep, "loop L19s: a Rule-1 record whose pick is still one of this build's candidates is NOT re-swept");
            // L19s2 (6 Oct, Sean's b0258a7b run): a COLLAPSED candidates record - the picked candidate's single view written over the record by the
            // 5 Oct landing read - is the pick, not a plan change: never re-swept (the re-derive rebuilds the candidates from their traces)
            { auto collapsed = juce::JSON::parse (R"json({"product": "Vertigo VSC-2", "controls": [{"index": 4, "name": "Threshold A", "unit": "dB"}, {"index": 11, "name": "Threshold B", "unit": "dB"}],
                  "pickedCandidate": {"index": 4, "name": "Threshold A"}, "ruleDecided": {"rule": "linked_pair", "pick": {"index": 4, "name": "Threshold A"}},
                  "thresholdSweep": {"result": "certified", "sweptControl": {"index": 4, "flags": "", "refineRounds": 0}, "positionNorms": [0.0, 0.5, 1.0], "inAtGr": [{"1": -30.0, "2": -28.0}, {"1": -20.0, "2": -18.0}, {"1": -10.0, "2": -8.0}]}})json");
              ejmap::sweep::Plan two; two.ok = true; two.thr = -1; two.candidates = { { 11, "Threshold B", {}, false }, { 4, "Threshold A", {}, false } };
              const auto pdc = planDiffers (collapsed, two);
              check (! pdc.resweep && pdc.why.contains ("collapsed candidates record"), "loop L19s2: a collapsed candidates record whose pick is one of the plan's candidates is not a plan change (" + pdc.why + ")");
              auto notPicked = juce::JSON::parse (juce::JSON::toString (collapsed)); notPicked.getDynamicObject()->removeProperty ("pickedCandidate"); notPicked.getDynamicObject()->removeProperty ("ruleDecided");
              check (planDiffers (notPicked, two).resweep, "loop L19s2: the same single sweep without a decision IS a plan change (swept one, the plan has two)"); }
            Plan nowQ = nowP; nowQ.candidates.clear(); nowQ.candidates.push_back ({ 1, "c1", {}, false }); nowQ.candidates.push_back ({ 30, "c30", {}, false });
            check (planDiffers (emo, nowQ).resweep && planDiffers (emo, nowQ).why.contains ("no longer one this build plans"), "loop L19s2: when the pick is no longer plannable the record is re-swept");
            // a single sweep whose amount control changed, and one whose 1 dB coverage is short with a gap over the bar
            auto single = juce::JSON::parse (R"json({"product": "x", "controls": [{"index": 7}], "thresholdSweep": {"result": "certified", "sweptControl": {"index": 7, "name": "Threshold", "flags": "", "refineRounds": 0}, "positionNorms": [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9], "inAtGr": [{"1": -30, "2": -28}, {"1": -28, "2": -26}, {"1": -26, "2": -24}, {"1": -24, "2": -22}, {"1": -22, "2": -20}, {"1": -20, "2": -18}, {"1": -18, "2": -16}, {"1": -16, "2": -14}, {"1": -14, "2": -12}]}})json");
            Plan single7; single7.ok = true; single7.thr = 7; Plan single9 = single7; single9.thr = 9;
            check (! planDiffers (single, single7).resweep, "loop L19t: the same amount control with nine 1 dB positions 2 dB apart: no re-sweep");
            check (planDiffers (single, single9).resweep && planDiffers (single, single9).why.contains ("swept [7], this build plans [9]"), "loop L19t2: a different amount control under this build is re-swept");
            auto few = juce::JSON::parse (R"json({"product": "UnFairchild", "controls": [{"index": 7}], "thresholdSweep": {"result": "certified", "sweptControl": {"index": 7, "flags": "", "refineRounds": 0}, "positionNorms": [0.2, 0.4, 0.6, 0.8, 1.0, 0.3], "inAtGr": [{"1": -10, "2": -6}, {"1": -15, "2": -11.5}, {"1": -20, "2": -16.6}, {"1": -25, "2": -21.6}, {"1": -30, "2": -26.4}, {"1": "not_reached", "2": "not_reached"}]}})json");
            check (planDiffers (few, single7).resweep && planDiffers (few, single7).why.contains ("only 5 positions reach 1 dB"), "loop L19u: five 1 dB positions with 2 dB gaps over the bar and rounds left is re-swept (UnFairchild)");
            // the same five points once the landing read has said 6 detents: no position between detents exists, no re-sweep
            auto fewStepped = juce::JSON::parse (juce::JSON::toString (few)); fewStepped.getDynamicObject()->setProperty ("amountLanding", juce::JSON::parse (R"json({"control": 7, "detents": 6})json"));
            check (! planDiffers (fewStepped, single7).resweep, "loop L19u4: a control stepped by evidence is never re-swept for refinement (a round cannot add a position between detents)");
            auto fewDeclared = juce::JSON::parse (juce::JSON::toString (few)); { auto* c0 = fewDeclared.getProperty ("controls", {})[0].getDynamicObject(); c0->setProperty ("numSteps", 6); c0->setProperty ("discrete", true); }
            check (! planDiffers (fewDeclared, single7).resweep, "loop L19u5: nor is a control declared stepped");
            // a 1 dB gap over the bar with the 2 dB points tight (DSM V3's shape: 2 dB reached at two adjacent positions only)
            auto oneGap = juce::JSON::parse (R"json({"product": "d", "controls": [{"index": 7}], "thresholdSweep": {"result": "certified", "sweptControl": {"index": 7, "flags": "", "refineRounds": 1}, "positionNorms": [0.2, 0.4, 0.6, 0.8], "inAtGr": [{"1": -10, "2": -11}, {"1": -14, "2": -12.5}, {"1": -18, "2": "not_reached"}, {"1": -22, "2": "not_reached"}]}})json");
            check (planDiffers (oneGap, single7).resweep && planDiffers (oneGap, single7).why.contains ("1 dB gap of 4.0"), "loop L19u3: a 1 dB gap over the bar re-sweeps even when every reachable 2 dB gap is within it");
            auto tuner = juce::JSON::parse (R"json({"product": "t", "schema": "ej_cert_tuner/1", "controls": [], "thresholdRefusal": {"stage": "plan", "reason": "x"}})json");
            check (! planDiffers (tuner, single7).resweep && ! planDiffers (juce::JSON::parse (R"json({"product": "n"})json"), single7).resweep, "loop L19u2: a tuner with nothing measured, or a record without controls, never re-sweeps");
        }
        // THE REVIEW PICK (L19m-L19p, ruled 4 Oct): an entry in review_picks.json naming a certified candidate decides; nothing without an entry; an uncertified name picks nothing and says why
        {
            const auto recJ = juce::JSON::parse (R"json({"product": "Shadow Hills Mastering Compressor", "thresholdCandidates": [{"index": 2, "name": "Optical Threshold 1", "thresholdSweep": {"result": "certified", "hold_s": 2.5, "tone": {"levels_dbfs": [-60, -40, -20, -10, 0]}}}, {"index": 14, "name": "Optical Threshold 2", "thresholdSweep": {"result": "certified", "hold_s": 2.5, "tone": {"levels_dbfs": [-60, -40, -20, -10, 0]}}}, {"index": 5, "name": "Discrete Threshold 1", "thresholdSweep": {"result": "flat"}}]})json");
            const auto picks = juce::JSON::parse (R"json([{"product": "Shadow Hills Mastering Compressor", "candidate": "Optical Threshold 1", "by": "KD", "date": "2026-10-04", "note": "optical stage, channel 1 drives both"}])json");
            auto r1 = juce::JSON::parse (juce::JSON::toString (recJ)); const auto w1 = applyReviewPick (r1, picks);
            check (w1.startsWith ("review pick applied") && (int) r1.getProperty ("pickedCandidate", {}).getProperty ("index", -1) == 2 && r1.getProperty ("ruleDecided", {}).getProperty ("rule", "") == "review_pick"
                     && r1.getProperty ("ruleDecided", {}).getProperty ("by", "") == "KD" && r1.getProperty ("ruleDecided", {}).getProperty ("ruleText", "").toString().contains ("picked by KD on 2026-10-04") && r1.getProperty ("ruleDecided", {}).getProperty ("trims", {}).size() == 2
                     && outcomeForRecord (r1).exportPending,
                   "loop L19m: a review pick naming a certified candidate writes pickedCandidate + ruleDecided {review_pick, by, date}, the others as trims, and the record goes on as export pending (" + w1 + ")");
            // L19m2 (ruled 4 Oct): a pick on a stereo unit goes through the SAME single view as a pair rule - the twin candidate is in neutral at instantiate, and the
            // ruleText says the tone check is the pair check (server write order, both channels within 0.5 dB); the tone check itself applies it to every pick
            {
                juce::String why; const auto view = ejmap::profile::candidateAsSingle (r1, "Optical Threshold 1", why);
                check (view.isObject() && view.getProperty ("thresholdSweep", {}).isObject() && ! view.hasProperty ("thresholdCandidates") && (int) view.getProperty ("pickedCandidate", {}).getProperty ("index", -1) == 2
                         && r1.getProperty ("ruleDecided", {}).getProperty ("ruleText", "").toString().contains ("both output channels within 0.5 dB of g, as for a pair rule"),
                       "loop L19m2: the review pick's single view is the pair rules' view (its sweep as thresholdSweep, the others in neutral), and the record says the pair tone check applies");
            }
            auto r2 = juce::JSON::parse (juce::JSON::toString (recJ)); check (applyReviewPick (r2, juce::var()).isEmpty() && ! r2.hasProperty ("pickedCandidate") && outcomeForRecord (r2).state == "needs_review", "loop L19n: without an entry nothing is ever picked");
            auto r3 = juce::JSON::parse (juce::JSON::toString (recJ)); const auto w3 = applyReviewPick (r3, juce::JSON::parse (R"json([{"product": "Shadow Hills Mastering Compressor", "candidate": "Discrete Threshold 1", "by": "KD", "date": "2026-10-04"}])json"));
            check (w3.contains ("but its sweep is flat: nothing picked") && ! r3.hasProperty ("pickedCandidate"), "loop L19o: an entry naming an uncertified candidate picks nothing and says why (" + w3 + ")");
            // L19q (Kathy's ruling 3, 6 Oct - MAGNUM-K): "A + B as a pair" picks A and names B as the twin written WITH it; the record then
            // owes a pair re-sweep until its picked sweep carries pairWrite for that twin
            { auto r5 = juce::JSON::parse (juce::JSON::toString (recJ)); if (! r5.hasProperty ("controls")) r5.getDynamicObject()->setProperty ("controls", juce::Array<juce::var>());
              const auto w5 = applyReviewPick (r5, juce::JSON::parse (R"json([{"product": "Shadow Hills Mastering Compressor", "candidate": "Optical Threshold 1 + Optical Threshold 2 as a pair", "by": "Kathy", "date": "2026-10-06", "note": "independent channels"}])json"));
              const auto rd = r5.getProperty ("ruleDecided", {});
              check (w5.startsWith ("review pick applied: 'Optical Threshold 1' + 'Optical Threshold 2' as a pair") && (int) r5.getProperty ("pickedCandidate", {}).getProperty ("index", -1) == 2
                       && rd.getProperty ("pair_with", {}).getProperty ("name", "") == "Optical Threshold 2" && rd.getProperty ("ruleText", "").toString().contains ("written WITH"),
                     "loop L19q: 'A + B as a pair' picks A with B as pair_with on ruleDecided (" + w5 + ")");
              const auto pd = planDiffers (r5, ejmap::sweep::planFromFixture (r5));
              check (pd.resweep && pd.why.contains ("the pair write") && pd.why.contains ("Optical Threshold 2"), "loop L19q: a pair-picked record whose sweep did not write the twin is re-swept for the pair write (" + pd.why + ")");
              // the picked sweep carrying pairWrite for that twin closes it
              for (const auto& c : *r5.getProperty ("thresholdCandidates", {}).getArray()) if ((int) c.getProperty ("index", -1) == 2) { auto* pw = new juce::DynamicObject(); pw->setProperty ("index", (int) rd.getProperty ("pair_with", {}).getProperty ("index", -1)); pw->setProperty ("name", "Optical Threshold 2"); c.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("pairWrite", juce::var (pw)); }
              check (! planDiffers (r5, ejmap::sweep::planFromFixture (r5)).why.contains ("the pair write"), "loop L19q: once the picked sweep wrote the twin at every position the pair write is no longer owed");
              // a changed entry (a later date) replaces an earlier review pick on the record; the same entry again changes nothing
              { auto r7 = juce::JSON::parse (juce::JSON::toString (recJ)); applyReviewPick (r7, picks);
                check (applyReviewPick (r7, picks).isEmpty() && (int) r7.getProperty ("pickedCandidate", {}).getProperty ("index", -1) == 2, "loop L19q2: the same review entry applied twice is applied once");
                const auto w7 = applyReviewPick (r7, juce::JSON::parse (R"json([{"product": "Shadow Hills Mastering Compressor", "candidate": "Optical Threshold 1 + Optical Threshold 2 as a pair", "by": "Kathy", "date": "2026-10-06"}])json"));
                check (w7.startsWith ("review pick applied") && r7.getProperty ("ruleDecided", {}).getProperty ("pair_with", {}).isObject() && r7.getProperty ("ruleDecided", {}).getProperty ("date", "") == "2026-10-06", "loop L19q2: a changed entry (later date, the pair) replaces the earlier review pick (" + w7 + ")"); }
              auto r6 = juce::JSON::parse (juce::JSON::toString (recJ));
              check (applyReviewPick (r6, juce::JSON::parse (R"json([{"product": "Shadow Hills Mastering Compressor", "candidate": "Optical Threshold 1 + Nothing as a pair", "by": "Kathy", "date": "2026-10-06"}])json")).contains ("not one of this record's candidates") && ! r6.hasProperty ("pickedCandidate"),
                     "loop L19q: a pair naming a candidate the record does not have picks nothing"); }
            auto r4 = juce::JSON::parse (juce::JSON::toString (recJ)); check (applyReviewPick (r4, juce::JSON::parse (R"json([{"product": "Shadow Hills Mastering Compressor", "candidate": "Optical Threshold 1"}])json")).contains ("incomplete") && ! r4.hasProperty ("pickedCandidate"), "loop L19p: an entry without initials and a date is incomplete and picks nothing");
        }
        // OUT-OF-SCOPE STATES, ON (L19h-L19k, ruled 4 Oct): multiband by band-numbered or Low+Mid+High names; surround by Logic's (N->N), N > 2; neither needs_review; a decided record is never out of scope
        {
            auto candsOf = [] (std::initializer_list<const char*> names) { juce::Array<juce::var> a; int i = 0; for (auto* n : names) { auto* o = new juce::DynamicObject(); o->setProperty ("index", i++); o->setProperty ("name", n); o->setProperty ("thresholdSweep", juce::var()); a.add (juce::var (o)); } return juce::var (a); };
            auto rec = [&] (const juce::String& product, const juce::var& cands) { auto* o = new juce::DynamicObject(); o->setProperty ("product", product); o->setProperty ("thresholdCandidates", cands); return juce::var (o); };
            const auto c6 = outcomeForRecord (rec ("C6 (s)", candsOf ({ "Band 1 Threshold", "Band 2 Threshold", "Band 3 Threshold" })));
            check (c6.state == "multiband" && c6.reason.startsWith ("multiband: profiling not built yet (Band 1, Band 2, Band 3; 3 threshold candidates)"), "loop L19h: band-numbered threshold candidates are 'multiband: profiling not built yet', not needs_review (" + c6.reason + ")");
            check (outcomeForRecord (rec ("Lindell 354E", candsOf ({ "Low Threshold", "Mid Threshold", "High Threshold" }))).state == "multiband" && outcomeForRecord (rec ("MDynamicsMB", candsOf ({ "Threshold (Band 1)", "Threshold (Band 2 - Gate)" }))).state == "multiband",
                   "loop L19i: Low + Mid + High by literal word, and '(Band N' in parentheses, are multiband too");
            check (outcomeForRecord (rec ("kHs Dynamics", candsOf ({ "Low Threshold", "High Threshold" }))).state == "needs_review" && outcomeForRecord (rec ("MaxxVolume (s)", candsOf ({ "Low Level Thresh", "High Level Thresh" }))).state == "needs_review",
                   "loop L19j: Low + High without Mid (kHs Dynamics, MaxxVolume) is NOT called multiband - it stays needs_review");
            // L19j2-L19j4 (widened 4 Oct): band words with channel/position prefixes (DynOne3), single-letter L/M/H all present (OTT); a genuine L/R pair and L/M + R/S are not multiband
            check (outcomeForRecord (rec ("DynOne3", candsOf ({ "C HMF Threshold", "LR MF Threshold", "S HF Threshold", "C LF Threshold" }))).reason.startsWith ("multiband: profiling not built yet (HMF, MF, HF, LF")
                     && outcomeForRecord (rec ("OTT", candsOf ({ "Thresh L", "Thresh M", "Thresh H" }))).reason.startsWith ("multiband: profiling not built yet (L, M, H"),
                   "loop L19j2: DynOne3's C/LR/S-prefixed LF/LMF/MF/HMF/HF and OTT's Thresh L/M/H are multibands");
            check (outcomeForRecord (rec ("Vertigo VSC-2", candsOf ({ "Threshold L", "Threshold R" }))).state != "multiband" && outcomeForRecord (rec ("DPR-402 (s)", candsOf ({ "Threshold L/M", "Threshold R/S" }))).state != "multiband" && outcomeForRecord (rec ("x", candsOf ({ "Thresh L", "Thresh H" }))).state != "multiband",
                   "loop L19j3: a genuine L/R pair, L/M + R/S, and L + H without M are never swallowed by the L/M/H rule");
            auto flatRec = rec ("dbx-160 (s)", candsOf ({ "Threshold L/M", "Threshold R/S" })); { auto c = flatRec.getProperty ("thresholdCandidates", {}); for (int k = 0; k < c.size(); ++k) { auto* sw = new juce::DynamicObject(); sw->setProperty ("result", "flat"); sw->setProperty ("reason", "passthrough: output equals input within 0.01 dB at every reading"); c[k].getDynamicObject()->setProperty ("thresholdSweep", juce::var (sw)); } }
            const auto of = outcomeForRecord (flatRec);
            check (of.state == "needs_review" && of.reason.startsWith ("sweep result flat on every candidate (2): passthrough") && ! of.reason.contains ("threshold candidates"),
                   "loop L19j4: every candidate flat (dbx-160 (s), kHs Dynamics) is filed with the flat-results investigation, in the flat words, not as 'N threshold candidates' (" + of.reason + ")");
            const auto sp = outcomeForRecord (rec ("Spherix Compressor (10->10)", candsOf ({ "Threshold 1", "Threshold 2" })));
            check (sp.state == "surround" && sp.reason.startsWith ("surround: not profiled (10 channels") && outcomeForRecord (rec ("C6 (s)", juce::var())).state != "surround" && surroundChannels ("Spherix Compressor (12->12)") == 12 && surroundChannels ("H-Comp (s)") == 0 && surroundChannels ("Some Comp (2->2)") == 0,
                   "loop L19k: Logic's (N->N) with N > 2 is 'surround: not profiled (N channels)'; a stereo name is not (" + sp.reason + ")");
            auto decided = rec ("C6 (s)", candsOf ({ "Band 1 Threshold", "Band 2 Threshold" })); { auto* pk = new juce::DynamicObject(); pk->setProperty ("index", 0); pk->setProperty ("name", "Band 1 Threshold"); decided.getDynamicObject()->setProperty ("pickedCandidate", juce::var (pk)); auto* rd = new juce::DynamicObject(); rd->setProperty ("rule", "review_pick"); decided.getDynamicObject()->setProperty ("ruleDecided", juce::var (rd)); }
            check (outcomeForRecord (decided).state != "multiband", "loop L19k2: a record with a pick (a rule or a review pick) is never filed as multiband by its names");
            auto mbLic = rec ("MDynamicsMB", candsOf ({ "Threshold (Band 1)", "Threshold (Band 2)" })); { juce::Array<juce::var> v; for (int k = 0; k < 2; ++k) { auto* o = new juce::DynamicObject(); o->setProperty ("index", k); o->setProperty ("result", "licence_suspect"); v.add (juce::var (o)); } auto* rv = new juce::DynamicObject(); rv->setProperty ("verdicts", v); mbLic.getDynamicObject()->setProperty ("thresholdReview", juce::var (rv)); }
            check (outcomeForRecord (mbLic).state == "needs_licence", "loop L19k3: a multiband whose every candidate is licence_suspect is a LICENCE row first (MDynamicsMB in Sean's run) - nothing was measured to call it anything else");
        }
        // A FAILED TONE CHECK IS needs_review (L19g, ruled 4 Oct): never exported; the reason carries GR vs g
        {
            const auto failed = juce::JSON::parse (R"json({"pass_within_0_5_db": false, "gr_measured_db": 1.02, "g_db": 2.0, "quiet_check_ok": true})json");
            const auto of = outcomeAfterExport (true, {}, true, {}, failed);
            check (of.state == "needs_review" && of.reason == "tone check failed: 1.02 vs 2.0; the profile is written but not exported", "loop L19g: a failed tone check (VBC FG-Grey's 1.02 vs 2) is needs_review 'tone check failed: <GR> vs <g>', never exported (" + of.reason + ")");
            const auto unread = juce::JSON::parse (R"json({"pass_within_0_5_db": false, "gr_measured_db": null, "g_db": 2.0, "quiet_check_ok": false})json");
            check (outcomeAfterExport (true, {}, true, {}, unread).reason.startsWith ("tone check failed: unreadable vs 2.0 (the quiet-reference check failed too)"), "loop L19g2: an unreadable check (Lindell 254E) says so, with the quiet check");
            check (outcomeAfterExport (true, {}, true, {}, juce::JSON::parse (R"json({"pass_within_0_5_db": true, "gr_measured_db": 1.99, "g_db": 2.0})json")).state == "exported", "loop L19g3: a passing check is exported");
        }
        // LICENCE, NOT CHANNEL STRIP (L19e-L19f, ruled 4 Oct): every candidate licence_suspect -> needs_licence with the reason; a mixed set stays a channel strip
        {
            const auto allSus = juce::JSON::parse (R"json({"product": "Pro-C 3", "thresholdReview": {"verdicts": [{"index": 1, "name": "Threshold", "result": "licence_suspect"}, {"index": 2, "name": "Auto Threshold", "result": "licence_suspect"}]},
                                                           "thresholdCandidates": [{"index": 1, "name": "Threshold", "thresholdSweep": null, "licenceSuspectReason": "the output is silent at every level"}, {"index": 2, "name": "Auto Threshold", "thresholdSweep": null}]})json");
            const auto oa = outcomeForRecord (allSus);
            check (oa.state == "needs_licence" && oa.reason.startsWith ("licence suspected: the output is silent at every level") && oa.reason.contains ("2 candidate(s)") && ! oa.reason.contains ("threshold candidates"),
                   "loop L19e: a candidates record whose every verdict is licence_suspect is needs_licence 'licence suspected: <reason>', never 'N threshold candidates' (" + oa.reason + ")");
            auto old = juce::JSON::parse (juce::JSON::toString (allSus)); old.getProperty ("thresholdCandidates", {})[0].getDynamicObject()->removeProperty ("licenceSuspectReason");
            const auto oo = outcomeForRecord (old);
            check (oo.state == "needs_licence" && oo.reason.contains ("the reason text is not on this record"), "loop L19e2: a record from before the reason was kept (Sean's 3 Oct run) is still needs_licence and says the reason is missing");
            auto mixed = juce::JSON::parse (juce::JSON::toString (allSus)); mixed.getProperty ("thresholdReview", {}).getProperty ("verdicts", {})[1].getDynamicObject()->setProperty ("result", "certified");
            const auto om = outcomeForRecord (mixed);
            check (om.state == "needs_review" && om.reason.contains ("2 threshold candidates"), "loop L19f: one certified candidate beside a licence_suspect one is a channel strip (needs_review), not a licence row");
        }
        // ONE ROW PER PRODUCT (L20-L21, ruled 2 Oct evening): a bundle's products that are subjects get no bundle row (their own row
        // carries the state); the others get one row each, keyed by product, linked to the bundle; the same product named by two
        // bundles (AU + VST3) gets one row.
        QuarantinedBundle two; two.bundle = "/x/SSL Pack.vst3"; two.licence = true; two.products = juce::StringArray { "SSL LMC+", "SSL X-Gate" }; two.reason = "activation window at scan"; two.stage = "scan"; two.category = "compressor";
        QuarantinedBundle dup; dup.bundle = "/x/SSL LMC+.component"; dup.licence = true; dup.products = juce::StringArray { "SSL LMC+" }; dup.reason = "activation window at scan"; dup.stage = "scan";
        const auto rows = bundleRows ({ st, two, dup }, juce::StringArray { "SSL Native Drumstrip v6 (s)" }, "now");
        juce::StringArray ids; for (const auto& r : rows) ids.add (r.getProperty ("identity", "").toString());
        check (rows.size() == 2 && ids.contains ("product|ssl lmc+") && ids.contains ("product|ssl x-gate") && ! ids.contains ("product|ssl native drumstrip v6")
                 && rows[0].getProperty ("scan_bundle", "").toString() == "/x/SSL Pack.vst3" && rowViolation (rows[0]).isEmpty(),
               "loop L20: a subject's bundle gives no second row; non-subject products get one row EACH, keyed by product, linked to the bundle; a product in two bundles gets one (" + ids.joinIntoString (", ") + ")");
        juce::var all = juce::Array<juce::var>(); for (const auto& r : rows) all = mergeRow (all, r);
        all = mergeRow (all, makeRow ("AudioUnit|d|1", "SSL Native Drumstrip v6 (s)", "compressor", outcomeCarriedLicence (st), {}, {}, {}, "now"));
        check (count (all).rows == 3 && count (all).needsLicence == 3, "loop L21: counts are per product: 3 products, 3 rows, 3 needs_licence");
    }
    {
        // QUARANTINED AT SCAN (L12-L14): a bundle the scan quarantined becomes a row in its own state, with the product's
        // category from categories.json by the registered AU's uid, else by name, else unknown; a VST3 says so.
        juce::Array<juce::var> qa;
        qa.add (obj ({ { "plugin_id", "/Library/Audio/Plug-Ins/Components/Acme.component" }, { "reason", "crash_on_load" }, { "stage", "scan" }, { "at", "t" } }));
        qa.add (obj ({ { "plugin_id", "/Library/Audio/Plug-Ins/VST3/ANA2.vst3" }, { "reason", "hang_in_findAllTypesForFile" }, { "stage", "scan" }, { "at", "t" } }));
        qa.add (obj ({ { "plugin_id", "/Library/Audio/Plug-Ins/VST3/Unseen.vst3" }, { "reason", "hang_in_findAllTypesForFile" }, { "stage", "scan" }, { "at", "t" } }));
        juce::Array<juce::var> mk1 { "AudioUnit|62485258" };
        auto* prods = new juce::DynamicObject();
        prods->setProperty ("acme opticom xla-3|acme", obj ({ { "name", "Opticom XLA-3 (Acme)" }, { "category", "compressor" }, { "mark_keys", mk1 } }));   // the name differs from the AU's: only the uid resolves it
        prods->setProperty ("ana2|sonic academy", obj ({ { "name", "ANA2" }, { "category", "synth" }, { "mark_keys", juce::Array<juce::var>() } }));
        const auto cats = obj ({ { "products", juce::var (prods) } });
        std::map<juce::String, juce::StringArray> byBundle { { "/Library/Audio/Plug-Ins/Components/Acme.component", juce::StringArray { "Acme Opticom XLA-3" } } };
        std::map<juce::String, juce::String> uidByName { { "Acme Opticom XLA-3", "62485258" } };
        const auto qb = quarantinedAtScan (juce::var (qa), cats, byBundle, uidByName);
        check (qb.size() == 3 && qb[0].category == "compressor" && qb[0].products.contains ("Acme Opticom XLA-3") && ! qb[0].vst3
                 && qb[1].category == "synth" && qb[1].vst3 && qb[2].category == "unknown" && qb[2].products[0] == "Unseen",
               "loop L12: a quarantined .component resolves to its AU and its category by uid; a VST3 by name; an unseen bundle is unknown, never dropped");
        const auto row = quarantineRow (qb[0], "now");
        check (rowViolation (row).isEmpty() && row.getProperty ("state", "") == "quarantined_at_scan" && row.getProperty ("category", "") == "compressor"
                 && row.getProperty ("reason", "").toString().contains ("crash_on_load") && certificationCategory ("compressor") && certificationCategory ("pitch") && ! certificationCategory ("synth"),
               "loop L13: the row is in the sixth state with the quarantine's reason and the category, and satisfies the invariant");
        juce::var r2 = juce::Array<juce::var>(); r2 = mergeRow (r2, row); r2 = mergeRow (r2, quarantineRow (qb[1], "now"));
        check (count (r2).quarantined == 2 && count (r2).rows == 2, "loop L14: the closing counts carry quarantined_at_scan");
        // CARRY-OVER ON RE-DERIVATION (L22, v1.7): the detector, Rule 1's decision, the pick, the map state and the manufacturer ride
        // over from the old record; the sweep's measured fields come from the fresh one; a candidate's detector by index.
        const auto oldR = obj ({ { "manufacturer", "Softube" }, { "mapState", "server map state 3" }, { "ruleDecided", obj ({ { "rule", "R1" } }) }, { "pickedCandidate", obj ({ { "index", 16 }, { "name", "Comp Thresh" } }) },
                                { "thresholdSweep", obj ({ { "result", "certified" }, { "detector", obj ({ { "fraction", 0.4 } }) }, { "positions", 16 } }) } });
        const auto freshR = carryOverAfterRederive (oldR, obj ({ { "thresholdSweep", obj ({ { "result", "certified" }, { "positions", 28 } }) } }));
        check (freshR.getProperty ("manufacturer", "") == "Softube" && freshR.getProperty ("mapState", "") == "server map state 3" && freshR.getProperty ("ruleDecided", {}).isObject() && freshR.getProperty ("pickedCandidate", {}).getProperty ("index", -1).equals (16)
                 && std::abs ((double) freshR.getProperty ("thresholdSweep", {}).getProperty ("detector", {}).getProperty ("fraction", 0.0) - 0.4) < 1e-9 && (int) freshR.getProperty ("thresholdSweep", {}).getProperty ("positions", 0) == 28,
               "loop L22: re-derivation carries over detector, ruleDecided, pickedCandidate, mapState, manufacturer and keeps the fresh sweep's fields");
        juce::Array<juce::var> oc; oc.add (obj ({ { "index", 16 }, { "thresholdSweep", obj ({ { "detector", obj ({ { "fraction", 0.7 } }) } }) } })); oc.add (obj ({ { "index", 1 }, { "thresholdSweep", obj ({}) } }));
        juce::Array<juce::var> ncs; ncs.add (obj ({ { "index", 16 }, { "thresholdSweep", obj ({ { "result", "certified" } }) } })); ncs.add (obj ({ { "index", 1 }, { "thresholdSweep", obj ({ { "result", "flat" } }) } }));
        const auto freshC = carryOverAfterRederive (obj ({ { "thresholdCandidates", oc } }), obj ({ { "thresholdCandidates", ncs } }));
        check (std::abs ((double) freshC.getProperty ("thresholdCandidates", {})[0].getProperty ("thresholdSweep", {}).getProperty ("detector", {}).getProperty ("fraction", 0.0) - 0.7) < 1e-9
                 && ! freshC.getProperty ("thresholdCandidates", {})[1].getProperty ("thresholdSweep", {}).hasProperty ("detector"),
               "loop L22b: a candidate's detector carries over by index, only where the old candidate had one");
        // L22c (6 Oct, Sean's b0258a7b run): the sweep's WRITES ride over when the re-derived view's are empty - the preconditions
        // (Zip's ratio raise 5 -> 0.4219 "4.14"), the engage writes, the pick, the pass-through fields; a fresh non-empty list wins
        { juce::Array<juce::var> pre; pre.add (obj ({ { "index", 5 }, { "norm", 0.421875 }, { "set", "4.14" }, { "role", "ratio_raise" } }));
          juce::Array<juce::var> none;
          const auto oldW = obj ({ { "thresholdSweep", obj ({ { "preconditions", pre }, { "engageWrites", obj ({ { "found", true } }) }, { "passThroughAtDefaults", true } }) } });
          const auto f1 = carryOverAfterRederive (oldW, obj ({ { "thresholdSweep", obj ({ { "result", "certified" }, { "preconditions", none } }) } }));
          const auto sw1 = f1.getProperty ("thresholdSweep", {});
          check (sw1.getProperty ("preconditions", {}).size() == 1 && (int) sw1.getProperty ("preconditions", {})[0].getProperty ("index", -1) == 5
                   && std::abs ((double) sw1.getProperty ("preconditions", {})[0].getProperty ("norm", 0.0) - 0.421875) < 1e-9
                   && (bool) sw1.getProperty ("engageWrites", {}).getProperty ("found", false) && (bool) sw1.getProperty ("passThroughAtDefaults", false),
                 "loop L22c: an EMPTY re-derived preconditions list takes the record's own writes (the ratio raise survives the re-derive)");
          juce::Array<juce::var> pre2; pre2.add (obj ({ { "index", 9 }, { "norm", 1.0 } }));
          const auto f2 = carryOverAfterRederive (oldW, obj ({ { "thresholdSweep", obj ({ { "preconditions", pre2 } }) } }));
          check ((int) f2.getProperty ("thresholdSweep", {}).getProperty ("preconditions", {})[0].getProperty ("index", -1) == 9, "loop L22c: a fresh non-empty list is kept as it is");
          juce::Array<juce::var> oc2; oc2.add (obj ({ { "index", 7 }, { "thresholdSweep", obj ({ { "preconditions", pre } }) } }));
          juce::Array<juce::var> nc2; nc2.add (obj ({ { "index", 7 }, { "thresholdSweep", obj ({ { "preconditions", none } }) } }));
          const auto f3 = carryOverAfterRederive (obj ({ { "thresholdCandidates", oc2 } }), obj ({ { "thresholdCandidates", nc2 } }));
          check (f3.getProperty ("thresholdCandidates", {})[0].getProperty ("thresholdSweep", {}).getProperty ("preconditions", {}).size() == 1, "loop L22c: a candidate's writes carry over by index too"); }
        // L22d (6 Oct): a collapsed candidates record (single sweep + pick) rebuilt as candidates carries its detector into the picked candidate
        { const auto coll = obj ({ { "pickedCandidate", obj ({ { "index", 4 } }) }, { "thresholdSweep", obj ({ { "detector", obj ({ { "fraction", 0.58 } }) } }) } });
          juce::Array<juce::var> nc3; nc3.add (obj ({ { "index", 11 }, { "thresholdSweep", obj ({ { "result", "certified" } }) } })); nc3.add (obj ({ { "index", 4 }, { "thresholdSweep", obj ({ { "result", "certified" } }) } }));
          const auto f4 = carryOverAfterRederive (coll, obj ({ { "thresholdCandidates", nc3 } }));
          check (std::abs ((double) f4.getProperty ("thresholdCandidates", {})[1].getProperty ("thresholdSweep", {}).getProperty ("detector", {}).getProperty ("fraction", 0.0) - 0.58) < 1e-9 && ! f4.getProperty ("thresholdCandidates", {})[0].getProperty ("thresholdSweep", {}).hasProperty ("detector"),
                 "loop L22d: a collapsed record's detector lands on the picked candidate (4) of the rebuilt record, not on the other (11)"); }
        // L23 (ruled 6 Oct): the sidechain A/B is OWED to a record swept under an earlier policy with an extra input, until a
        // verdict under the policy now is on it; no extra input, or swept under the policy now, owes nothing
        { auto rec = [&] (const char* pol, int buses, const char* abNow) { juce::Array<juce::var> eb; for (int i = 0; i < buses; ++i) eb.add (obj ({ { "index", i + 1 }, { "name", "SC" } }));
              auto r = obj ({ { "thresholdSweep", obj ({ { "sidechain", obj ({ { "policy", pol }, { "extraInputBuses", eb } }) } }) } });
              if (abNow != nullptr) r.getDynamicObject()->setProperty ("sidechainPolicyCheck", obj ({ { "policyNow", abNow }, { "verdict", "same" } })); return r; };
          check (sidechainAbOwed (rec ("enabled_silent", 1, nullptr)) && sidechainAbOwed (rec ("unconnected", 1, nullptr)), "loop L23: swept under an earlier policy with an extra input: the A/B is owed");
          check (! sidechainAbOwed (rec ("enabled_silent", 0, nullptr)), "loop L23: no extra input: nothing owed");
          check (! sidechainAbOwed (rec ("self-keyed (as EchoJay 04e)", 1, nullptr)), "loop L23: swept under the policy now: nothing owed");
          check (! sidechainAbOwed (rec ("enabled_silent", 1, "self-keyed (as EchoJay 04e)")) && sidechainAbOwed (rec ("enabled_silent", 1, "unconnected")), "loop L23: a verdict under the policy now closes it; one under an older policy does not"); }
        // NEEDS LICENCE (L15, 2 Oct): a licence-stops entry becomes a row in its own state with the windows it saw
        juce::Array<juce::var> wins { "PACE [pid 123]" };
        juce::Array<juce::var> ls; ls.add (obj ({ { "plugin_id", "/Library/Audio/Plug-Ins/VST3/SSL Native Drumstrip v6.vst3" }, { "state", "needs_licence" }, { "pace", true }, { "windows", wins }, { "stage", "scan" }, { "at", "t" } }));
        const auto lb = quarantinedAtScan (juce::var (ls), cats, {}, {});
        const auto lrow = quarantineRow (lb[0], "now");
        check (lb.size() == 1 && lb[0].licence && lrow.getProperty ("state", "") == "needs_licence" && lrow.getProperty ("reason", "").toString().contains ("activation window")
                 && lrow.getProperty ("reason", "").toString().contains ("PACE [pid 123]") && lrow.getProperty ("reason", "").toString().contains ("not retried") && rowViolation (lrow).isEmpty()
                 && count (mergeRow (juce::var (juce::Array<juce::var>()), lrow)).needsLicence == 1,
               "loop L15: a licence stop is the state needs_licence, naming the window and that it was not retried; counted on its own");
    }
    {
        // THE SCAN'S WINDOW WATCH, the pure rule (W1-W3, 2 Oct): an owner that appeared or grew is a new window; one that
        // closed is not news; the host's own windows at the baseline are not news. PACE by owner name.
        using namespace ejmap::windowwatch;
        OwnerCounts base { { "ejmap [pid 1]", 1 } };
        check (newWindows (base, OwnerCounts { { "ejmap [pid 1]", 1 } }).isEmpty() && newWindows (base, OwnerCounts {}).isEmpty(),
               "watch W1: the same windows, or fewer, are not a new window");
        check (newWindows (base, OwnerCounts { { "ejmap [pid 1]", 2 } }) == juce::StringArray { "ejmap [pid 1]" },
               "watch W2: one more window from the host's own process (a vendor's in-process serial dialog) is a new window");
        const auto nw = newWindows (base, OwnerCounts { { "ejmap [pid 1]", 1 }, { "PACEEdenExperience [pid 77]", 1 } });
        check (nw == juce::StringArray { "PACEEdenExperience [pid 77]" } && isPaceOwner (nw) && ! isPaceOwner (juce::StringArray { "ejmap [pid 1]" }),
               "watch W3: a window from a new process in the tree is a new window, and PACE's is known by its owner");
    }
}

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

/** THE SIDECHAIN POLICY CHECK (EjmapSidechainCheck.h, ruled 4 Oct): the five measurements of 4 Oct on this Mac, read from
    their traces through the same parser the follow-up uses; the pick; the arguments; and the verdict. A control proves
    each file-read pin reads the file. */
void testSidechainCheck()
{
    using namespace ejmap::sidechaincheck;
    const auto dir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/cert-traces/2026-10-04-sidechain");
    auto trace = [&] (const char* name) { return parseTrace (dir.getChildFile (name).loadFileAsString()); };
    struct Unit { const char* stem; double level; double before, after; bool resweep; };
    const Unit units[] = { { "c1comp_s",        -6.0,  -9.0102, -19.4687, true  },     // Waves C1 comp (s): keys from the connected bus
                           { "rcompressor_s",   -6.0,  -9.0111, -63.0425, true  },     // Waves RCompressor (s), 50:1
                           { "emod5_s",         -6.0, -36.8077, -36.8077, false },     // Waves EMO-D5 (s): same bus, keys internally
                           { "lindell_sbc",     -6.0, -20.8373, -20.8373, false },     // declares a Sidechain bus
                           { "elysia_mpressor", -6.0, -29.6403, -29.6403, false } };   // declares Input #2
    for (const auto& u : units)
    {
        const auto before = trace ((juce::String (u.stem) + ".enabled_silent.txt").toRawUTF8());
        const auto after  = trace ((juce::String (u.stem) + ".unconnected.txt").toRawUTF8());
        check (before.ok && before.policy == "enabled_silent" && before.extraInputBuses == 1 && before.holdDb.count (u.level) && std::abs (before.holdDb.at (u.level) - u.before) < 1e-4,
               juce::String ("sidechain S1 ") + u.stem + ": the enabled_silent trace parses (policy, one extra bus, the hold at -6)");
        check (after.ok && after.policy == "unconnected" && after.extraInputBuses == 1 && std::abs (after.holdDb.at (u.level) - u.after) < 1e-4,
               juce::String ("sidechain S2 ") + u.stem + ": the unconnected trace parses");
        const auto v = verdict (before.holdDb.at (u.level), after.holdDb.at (u.level), before.norm, u.level);
        check (v.resweep == u.resweep && v.why.startsWith (u.resweep ? "keys from a connected sidechain" : "sidechain policy: no effect"),
               juce::String ("sidechain S3 ") + u.stem + ": " + (u.resweep ? "re-swept" : "kept") + " (" + v.why + ")");
        // the one reading the follow-up would take: this trace, at -6, with the trace's own writes
        const auto pick = pickReading ({ before }, before.sets);
        check (pick.ok && pick.trace == 0 && pick.level == u.level && std::abs (pick.beforeDb - u.before) < 1e-4, juce::String ("sidechain S4 ") + u.stem + ": the pick is this trace at -6 dBFS");
        const auto args = argsFor (before, u.level);
        check (args[0] == "--sweep" && args.contains ("thr=" + juce::String (before.thr)) && args.contains ("levels=-6") && args.contains ("ref=0")
                 && args.contains ("norms=" + juce::String (before.norm, 6)) && (before.sets.empty() || args[args.size() - 1].startsWith ("set=")),
               juce::String ("sidechain S5 ") + u.stem + ": the arguments repeat the trace's process at the one level");
    }
    // CONTROLS: an unread file parses to nothing, and a trace without the policy line or the bus is not in the set
    check (! parseTrace ("").ok && ! parseTrace (dir.getChildFile ("no_such_file.txt").loadFileAsString()).ok, "sidechain S6: an absent file yields no trace (the control for S1/S2)");
    {
        auto t = trace ("c1comp_s.enabled_silent.txt");
        auto noBus = t; noBus.extraInputBuses = 0;
        check (! pickReading ({ noBus }, t.sets).ok && pickReading ({ noBus }, t.sets).why.contains ("no input bus past the main one"), "sidechain S7: a product with no second input bus is not in the set");
        auto newPolicy = t; newPolicy.policy = kPolicyNow;   // (the old pickReading: a trace under any later policy is not an "enabled_silent" trace)
        check (! pickReading ({ newPolicy }, t.sets).ok && pickReading ({ newPolicy }, t.sets).why.contains ("not swept under enabled_silent"), "sidechain S8: a record already swept unconnected is not in the set");
        check (! pickReading ({ t }, {}).ok && pickReading ({ t }, {}).why.contains ("record's writes"), "sidechain S9: a trace that ran with other writes than the record's is not compared (the writes are part of the reading)");
        check (! pickReading ({}, t.sets).ok && pickReading ({}, t.sets).why.contains ("no position trace"), "sidechain S10: no trace -> unknown, said, never guessed");
        // the loud level and the lowest reading: two traces at the same level, the lower output wins; a -12 only trace loses to -6
        auto a = t, b = t; a.holdDb = { { -6.0, -9.0 }, { -12.0, -15.0 } }; b.holdDb = { { -6.0, -19.0 } };
        const auto pk = pickReading ({ a, b }, t.sets);
        check (pk.ok && pk.trace == 1 && pk.level == -6.0 && pk.beforeDb == -19.0, "sidechain S11: the pick is the loudest level at or below -6 and the position reading lowest there");
        auto c = t; c.holdDb = { { 0.0, -3.0 } };
        check (pickReading ({ c }, t.sets).ok == false, "sidechain S12: a trace with holds only above -6 dBFS offers no reading");
    }
    check (std::abs (verdict (-20.0, -20.1, 0.5, -6).deltaDb + 0.1) < 1e-9 && ! verdict (-20.0, -20.1, 0.5, -6).resweep && verdict (-20.0, -20.11, 0.5, -6).resweep,
           "sidechain S13: the bar is 0.1 dB inclusive");
    // the record's writes: preconditions + engage writes
    const auto view = juce::JSON::parse (R"json({"preconditions": [{"index": 28, "norm": 1.0}, {"index": 41, "norm": 0.5}], "engageWrites": {"writes": [{"index": 15, "norm": 1.0}]}})json");
    const auto w = recordWrites (view);
    check (w.size() == 3 && sameWrites (w, { { 15, 1.0 }, { 28, 1.0 }, { 41, 0.5 } }) && ! sameWrites (w, { { 28, 1.0 }, { 41, 0.5 } }), "sidechain S14: the record's writes are its preconditions plus its engage writes, order-free");
    // SC-R the record's own evidence beats a cached verdict: a view swept under the policy now is out of the set even with a stale
    // "resweep" verdict on the record (the re-sweep carries it over; read first it re-swept C1 comp (s) on every follow-up run)
    { const auto viewNow = juce::JSON::parse ("{\"sidechain\":{\"policy\":\"self-keyed (as EchoJay 04e)\",\"extraInputBuses\":[{\"index\":1}]}}");
      const auto viewPrev = juce::JSON::parse ("{\"sidechain\":{\"policy\":\"unconnected\",\"extraInputBuses\":[{\"index\":1}]}}");
      const auto viewOld = juce::JSON::parse ("{\"sidechain\":{\"policy\":\"enabled_silent\",\"extraInputBuses\":[{\"index\":1}]}}");
      // THE POLICY (Kathy's ruling, 6 Oct): EchoJay's, labelled exactly so on every record and profile; the two earlier ones are "before"
      check (juce::String (kPolicyNow) == "self-keyed (as EchoJay 04e)" && juce::String (kPolicyPrev) == "unconnected" && juce::String (kPolicyOld) == "enabled_silent", "sidechain SC-P: the policy now is EchoJay's, by its label");
      check (switchFor (kPolicyOld) == "silent" && switchFor (kPolicyPrev) == "unconnected" && switchFor (kPolicyNow) == "echojay" && switchFor ("something").isEmpty(), "sidechain SC-P: each policy a record can carry maps to the probe's test switch, an unknown one to nothing");
      // THE WRITE FAULT (ruling 2): Zip's case is the mutant - the check wrote [5] at 0.0000 where the sweep wrote 0.421875
      const std::vector<std::pair<int, double>> sweepSets { { 5, 0.421875 }, { 4, 0.0 }, { 28, 1.0 } };
      const std::vector<std::pair<int, double>> zipCheck  { { 0, 0.0 }, { 5, 0.0 }, { 4, 0.0 }, { 28, 1.0 }, { 17, 0.727 } };
      const std::vector<std::pair<int, double>> goodCheck { { 0, 0.0 }, { 5, 0.421875 }, { 4, 0.0 }, { 28, 1.0 }, { 17, 0.727 } };
      const auto wf = writeFault (zipCheck, sweepSets, 17);
      check (wf == "write fault: [5] check 0.0000, sweep 0.4219", "sidechain SC-W (Zip, 5 Oct): the ratio written at 0.0000 where the sweep wrote 0.4219 is named as a write fault (got '" + wf + "')");
      check (writeFault (goodCheck, sweepSets, 17).isEmpty(), "sidechain SC-W: the same writes agree (extra writes the sweep never made - the neutral list - are not faults; the amount is never compared)");
      check (writeFault ({ { 4, 0.0 } }, sweepSets, 17).startsWith ("write fault: the sweep wrote [5]"), "sidechain SC-W: a sweep write the check omitted is a fault");
      check (nearZeroWherePredicted (0.0, 2.0) && nearZeroWherePredicted (-0.2, 2.0) && ! nearZeroWherePredicted (0.5, 2.0) && ! nearZeroWherePredicted (0.0, 0.5) && ! nearZeroWherePredicted (std::nullopt, 2.0),
             "sidechain SC-W: 'near zero where predicted' is |GR| < 0.3 dB with >= 1 dB predicted, never on an unreadable GR");
      check (sweptUnderPolicyNow (viewNow), "sidechain SC-R: a view with the EchoJay label was swept under the policy now");
      check (! sweptUnderPolicyNow (viewPrev) && ! sweptUnderPolicyNow (viewOld), "sidechain SC-R: a view under unconnected or enabled_silent was not (both are before)");
      check (! sweptUnderPolicyNow (juce::JSON::parse ("{}")), "sidechain SC-R: no sidechain field = not under the policy now (the traces decide)"); }
}

/** THE INERT CHECK (EjmapSweep.h inertCandidates / inertVerdict, ruled 4 Oct): NEOLD V76U73's control list and its
    4 Oct readings - byte-identical output under Mode, Gain, Makeup +24, Trim +18 and Power Off. */
void testInertCheck()
{
    using namespace ejmap::sweep;
    const auto v76 = juce::JSON::parse (R"json({"product": "NEOLD V76U73", "controls": [
        {"index": 0, "name": "Attack Time", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "Fast", "1.000": "Slow"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "Fast"}},
        {"index": 2, "name": "Mix", "unit": "%", "numSteps": 2147483647, "displayAt": {"0.000": "0.0", "1.000": "100.0"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "100.0"}},
        {"index": 3, "name": "Gain", "unit": "dB", "numSteps": 2147483647, "displayAt": {"0.000": "43", "1.000": "76"}, "defaultOnInstantiate": {"normalised": 0.454545, "display": "58"}},
        {"index": 9, "name": "Makeup Gain", "unit": "dB", "numSteps": 2147483647, "displayAt": {"0.000": "0.0", "1.000": "24.0"}, "defaultOnInstantiate": {"normalised": 0.0, "display": "0.0"}},
        {"index": 10, "name": "Mode", "numSteps": 2147483647, "displayAt": {"0.000": "Compress", "0.500": "Bypass", "1.000": "Limit"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "Bypass"}},
        {"index": 13, "name": "Power", "numSteps": 2, "discrete": true, "displayAt": {"0.000": "Off", "1.000": "On"}, "defaultOnInstantiate": {"normalised": 1.0, "display": "On"}},
        {"index": 17, "name": "Trim", "unit": "dB", "numSteps": 2147483647, "displayAt": {"0.000": "-18.0", "1.000": "18.0"}, "defaultOnInstantiate": {"normalised": 0.5, "display": "0.0"}}]})json");
    auto tried = inertCandidates (v76, 3);
    juce::StringArray names; for (const auto& t : tried) names.add (t.name + "@" + juce::String (t.norm, 1));
    check (names.size() == 3 && names[0] == "Power@0.0" && names.contains ("Makeup Gain@1.0") && names.contains ("Trim@0.0"),
           "inert I1: V76U73's candidates are Power (to Off, first), Makeup Gain (to its far end) and Trim - not the threshold Gain, not Mode (word-valued), not Mix (" + names.joinIntoString (",") + ")");
    // the 4 Oct readings: control -9.0877, every write -9.0877
    for (auto& t : tried) { t.ran = true; t.afterDb = -9.0877; }
    juce::String why;
    check (inertVerdict (-9.0877, tried, why) && why.startsWith ("processing never runs: output unchanged by every control including Power"), "inert I2: unchanged by Power, Makeup and Trim -> inert, the ruling's words (" + why + ")");
    auto moved = tried; moved[1].afterDb = 14.9;   // Makeup +24 would do this on a product that processes
    check (! inertVerdict (-9.0877, moved, why) && why.startsWith ("the product processes: 1 of 3"), "inert I3: one control moving the output -> not inert, the row names it");
    auto edge = tried; edge[2].afterDb = -9.0877 - 0.1;
    check (inertVerdict (-9.0877, edge, why), "inert I4: a 0.1 dB difference is within the bar");
    auto none = tried; for (auto& t : none) t.ran = false;
    check (! inertVerdict (-9.0877, none, why) && why.contains ("no control could be tried"), "inert I5: nothing ran -> not inert (a guard refuses, never guesses)");
    auto noPower = juce::JSON::parse (juce::JSON::toString (v76)); { auto* cs = noPower.getProperty ("controls", {}).getArray(); for (int i = cs->size(); --i >= 0;) if ((int) (*cs)[i].getProperty ("index", -1) == 13) cs->remove (i); }
    auto t2 = inertCandidates (noPower, 3); for (auto& t : t2) { t.ran = true; t.afterDb = -9.0877; }
    check (inertVerdict (-9.0877, t2, why) && why.contains ("(no power switch declared)") && ! why.contains ("including Power"), "inert I6: without a power switch the reason says so instead of claiming Power");
    check (inertCandidates (juce::JSON::parse (R"json({"controls": [{"index": 3, "name": "Gain"}]})json"), 3).empty(), "inert I7: the threshold itself is never a candidate");
    // the record: result inert overrides flat and carries the check
    Plan p; p.thr = 3; p.thrName = "Gain"; p.inert = true; p.inertReason = why; p.inertTried = t2; p.inertControlDb = -9.0877;
    Derived d; d.result = "flat"; d.reason = "no two positions differ"; d.passThroughAtDefaults = true;
    const auto rec = composeThresholdSweep (d, {}, p, {});
    check (rec.getProperty ("result", "").toString() == "inert" && rec.getProperty ("reason", "").toString() == why && rec.getProperty ("inertCheck", {}).getProperty ("tried", {}).size() == 2,
           "inert I8: the record says inert with the check's reason, never flat");
    check (ejmap::loop::outcomeForRecord (juce::JSON::parse (R"json({"product": "v", "controls": [], "thresholdSweep": {"result": "inert", "reason": "processing never runs: x"}})json")).reason.startsWith ("licence suspected: processing never runs"),
           "inert I9: the row carries the reason under its own result word");
    // INERT = LICENCE (ruled 5 Oct): the state, the remedy in the reason, every-candidate inert, and --retry-licence's reach
    {
        const auto single = juce::JSON::parse (R"json({"product": "v", "controls": [], "thresholdSweep": {"result": "inert", "reason": "processing never runs: output unchanged by every control including Power"}})json");
        const auto o = ejmap::loop::outcomeForRecord (single);
        check (o.state == "needs_licence" && o.reason.contains ("activation and a re-run") && o.reason.contains ("--retry-licence") && o.reason.contains ("including Power"), "inert L1: an inert sweep is needs_licence, the reason names the remedy (activation, re-run) and what was measured");
        const auto cands = juce::JSON::parse (R"json({"product": "c", "controls": [], "thresholdCandidates": [{"index": 1, "name": "A", "thresholdSweep": {"result": "inert", "reason": "processing never runs: p"}}, {"index": 2, "name": "B", "thresholdSweep": {"result": "inert", "reason": "processing never runs: p"}}]})json");
        const auto oc = ejmap::loop::outcomeForRecord (cands);
        check (oc.state == "needs_licence" && oc.reason.contains ("on every candidate (2)"), "inert L2: inert on every candidate is a licence row too");
        const auto mixed = juce::JSON::parse (R"json({"product": "m", "controls": [], "thresholdCandidates": [{"index": 1, "name": "A", "thresholdSweep": {"result": "inert", "reason": "p"}}, {"index": 2, "name": "B", "thresholdSweep": {"result": "flat", "reason": "q"}}]})json");
        check (ejmap::loop::outcomeForRecord (mixed).state != "needs_licence", "inert L3: one inert candidate beside a flat one is not a licence row (something ran)");
        // the worklist: --retry-licence (licenceOnly) brings the inert record back; a plain run and --retry-refused leave it recorded
        using namespace ejmap::cert;
        Subject s; s.product = "v"; s.pushed = single; std::vector<Subject> store { s };
        check (partitionStore (store, false).toSweep.empty() && partitionStore (store, false).recorded == 1, "inert L4: a plain batch leaves an inert record recorded (not re-swept)");
        check (partitionStore (store, true, false, false).toSweep.empty(), "inert L5: --retry-refused does not touch it (it is not a refusal)");
        check (partitionStore (store, true, false, true).toSweep.size() == 1, "inert L6: --retry-licence puts it back on the worklist");
        Subject f; f.product = "f"; f.pushed = juce::JSON::parse (R"json({"product": "f", "controls": [], "thresholdSweep": {"result": "flat", "reason": "x"}})json");
        check (partitionStore ({ f }, true, false, true).toSweep.empty(), "inert L7: --retry-licence leaves a flat record alone");
        check (inertRecorded (cands) && ! inertRecorded (mixed) && ! inertRecorded (f.pushed), "inert L8: inertRecorded = the sweep, or every candidate");
    }
}

/** THE TUNER PLAN v2 (EjmapPitch.h, 4 Oct A4): detents by evidence from the text grid, the adaptive speed half period. */
void testTunerPlanV2()
{
    using namespace ejmap::pitch;
    // Auto-Tune Access's Retune Speed: declared continuous, reads Slow / Medium / Fast, lands on three values only
    const juce::String grid = "at\t0.000000\tlanded\tgetValue\t0.000000\tconfirm_ms\t3.0\tlanded_by\tinstack\ttext\tSlow\n"
                              "at\t0.031250\tunlanded\tgetValue\t0.000000\tconfirm_ms\t-1.0\tlanded_by\tunlanded\ttext\tSlow\n"
                              "at\t0.500000\tlanded\tgetValue\t0.500000\tconfirm_ms\t3.1\tlanded_by\tinstack\ttext\tMedium\n"
                              "at\t0.531250\tunlanded\tgetValue\t0.500000\tconfirm_ms\t-1.0\tlanded_by\tunlanded\ttext\tMedium\n"
                              "at\t1.000000\tlanded\tgetValue\t1.000000\tconfirm_ms\t2.9\tlanded_by\tinstack\ttext\tFast\n";
    const auto rows = parseTextGrid (grid);
    const auto det = detentsFromTextGrid (rows);
    check (rows.size() == 5 && det.size() == 3 && det[0].second == "Slow" && det[1].first == 0.5f && det[2].second == "Fast", "tuner T1: three detents from the read-back values; a between write that reads back the detent it snapped to adds nothing");
    check (detentsFromTextGrid (parseTextGrid ("at\t0.000000\tlanded\tgetValue\t0.000000\tconfirm_ms\t3.0\tlanded_by\tinstack\ttext\tSlow\n")).empty(), "tuner T2: one detent is nothing to sweep (empty, the caller says so)");
    // Auto-Tune Access's Key (4 Oct, live): 33 writes, only 4 "landed" at the asked norm, but every read-back is one of 12 detents
    {
        juce::String keyGrid; const char* keys[] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        for (int k = 0; k <= 32; ++k) { const double asked = k / 32.0; const int d = (int) std::round (asked * 11.0); const double got = d / 11.0;
                                        keyGrid << "at\t" << juce::String (asked, 6) << "\t" << (std::abs (got - asked) < 0.005 ? "landed" : "unlanded") << "\tgetValue\t" << juce::String (got, 6) << "\tconfirm_ms\t1.0\tlanded_by\tpump\ttext\t" << keys[d] << "\n"; }
        const auto kd = detentsFromTextGrid (parseTextGrid (keyGrid));
        check (kd.size() == 12 && kd[1].second == "Db" && std::abs (kd[1].first - 1.0f / 11.0f) < 1e-4f && kd[11].second == "B", "tuner T2b: a 12-key control snapping writes to k/11 yields its 12 detents from the read-backs (the landed flag would give 4)");
    }
    check (textsAreWords (juce::JSON::parse (R"json({"displayAt": {"0.000": "Slow", "0.500": "Medium", "1.000": "Fast"}})json"))
             && ! textsAreWords (juce::JSON::parse (R"json({"displayAt": {"0.000": "400", "0.500": "50", "1.000": "0"}})json")), "tuner T3: Slow/Medium/Fast are words, 400/50/0 are numbers (eight positions stay)");
    // the adaptive half period: a position unsettled at 1 s and settled at 2 s reads from the 2 s run and says so
    auto win = [] (double tMs, double target, double outC) { Window w; w.tMs = tMs; w.inC = target; w.inConf = 1.0; w.outC = outC; w.outConf = 1.0; w.outDb = -20.0; w.target = target; return w; };
    auto squareRun = [&] (double halfPeriodS, double tauMs, int halfPeriods, bool present)
    {
        PitchMeasured m; m.ok = true; m.cents = 30.0; m.rateHz = 0.5 / halfPeriodS;
        PitchPosition p; p.k = present ? 0 : -1; p.norm = 0.0f; p.landed = present;
        const double hop = 50.0, half = halfPeriodS * 1000.0;
        for (int e = 0; e < halfPeriods && present; ++e)
        {
            const double target = (e % 2) ? 30.0 : -30.0;
            for (double t = 0.0; t < half; t += hop) p.windows.push_back (win (e * half + t, target, target * juce::jmax (0.0, 1.0 - t / tauMs)));   // the output walks toward 0 (corrected) from the step, reaching it at tauMs
        }
        m.positions.push_back (p);
        return SpeedRun { halfPeriodS, m };
    };
    const auto slow1 = squareRun (1.0, 1200.0, 6, true);    // corrected only at 1.2 s: not settled inside a 1 s half period
    const auto slow2 = squareRun (2.0, 1200.0, 6, true);    // settled inside 2 s
    const auto r1 = deriveSpeed (slow1.vib.positions[0], 30.0, slow1.vib.rateHz);
    check (r1.result == "refused" && r1.reason.contains ("had not settled before the next flip"), "tuner T4: a correction that completes at 1.2 s does not settle inside a 1 s half period (" + r1.reason + ")");
    const auto pk = pickSpeed ({ slow1, slow2 }, 0);
    check (pk.fromRun && pk.halfPeriodS == 2.0 && pk.result.result == "measured", "tuner T5: the speed is read from the 2 s run, and the pick says 2 s (" + pk.result.result + ": " + pk.result.reason + ")");
    check (unsettledPositions ({ slow1 }, 1).size() == 1 && unsettledPositions ({ slow1, slow2 }, 1).empty(), "tuner T6: the unsettled set drives the next run and empties once settled");
    const auto fast1 = squareRun (1.0, 300.0, 6, true);
    check (pickSpeed ({ fast1, slow2 }, 0).halfPeriodS == 1.0, "tuner T7: a position settled at 1 s is never re-read from a longer run");
    // a position absent from a partial run is skipped, never read as "write did not land"
    const auto absent2 = squareRun (2.0, 1200.0, 6, false);
    const auto pk2 = pickSpeed ({ slow1, absent2 }, 0);
    check (pk2.halfPeriodS == 1.0 && pk2.result.reason.contains ("had not settled"), "tuner T8: a run that lacks the position is skipped (the 1 s refusal stands)");
    // the record: half_period_s per position and the half periods tried on the generator
    const auto rec = composePitchSweep (PitchMeasured{}, std::vector<SpeedRun> { slow1, slow2 }, -1, {});
    check ((double) rec.getProperty ("positions", {})[0].getProperty ("speed", {}).getProperty ("half_period_s", 0.0) == 2.0 && rec.getProperty ("generator", {}).getProperty ("speed_half_periods_s", {}).size() == 2,
           "tuner T9: the record says which half period each position's speed came from, and which were tried");
    // planDiffers: a tuner measured under plan v1 is re-measured; under v2 it is not
    const auto v1 = juce::JSON::parse (R"json({"product": "t", "schema": "ej_cert_tuner/1", "controls": [], "pitchCandidates": [{"index": 1}]})json");
    const auto v2 = juce::JSON::parse (R"json({"product": "t", "schema": "ej_cert_tuner/1", "controls": [], "pitchCandidates": [{"index": 1}], "pitchPlan": {"version": 2}})json");
    ejmap::sweep::Plan none;
    check (ejmap::loop::planDiffers (v1, none).resweep && ejmap::loop::planDiffers (v1, none).why.contains ("tuner procedure changed") && ! ejmap::loop::planDiffers (v2, none).resweep,
           "tuner T10: planDiffers re-measures a tuner recorded under plan v1 and leaves a v2 record alone");
}

/** THE v0.1 TUNER MEASUREMENTS AND THE DRAFT EXPORTER (EjmapPitch.h deriveShortNotes / toleranceFrom, EjmapTunerProfile.h; 4 Oct A5, a PROPOSAL). */
void testTunerV01()
{
    using namespace ejmap::pitch;
    auto win = [] (double tMs, double outC, bool loud) { Window w; w.tMs = tMs; w.inC = 30.0; w.inConf = loud ? 1.0 : 0.0; w.outC = outC; w.outConf = loud ? 1.0 : 0.3; w.outDb = loud ? -20.0 : -90.0; w.target = 30.0; return w; };
    // short notes: 200 ms notes (4 windows of 50 ms) and 100 ms gaps (2 silent windows); the output on each note walks 30 -> 12 (second half ~12-15)
    PitchPosition notes; notes.k = 0; notes.landed = true;
    for (int n = 0; n < 6; ++n) { const double t0 = n * 300.0; notes.windows.push_back (win (t0, 30.0, true)); notes.windows.push_back (win (t0 + 50, 22.0, true)); notes.windows.push_back (win (t0 + 100, 15.0, true)); notes.windows.push_back (win (t0 + 150, 12.0, true));
                                  notes.windows.push_back (win (t0 + 200, -9999.0, false)); notes.windows.push_back (win (t0 + 250, -9999.0, false)); }
    const auto sn = deriveShortNotes (notes, 30.0);
    check (sn.result == "measured" && sn.windowsUsed == 6 && std::abs (sn.residualCents - 13.5) < 1e-6 && std::abs (sn.strength - 0.55) < 1e-6, "tuner V1: six short notes, residual = the median of each note's second half (13.5), strength 0.55 (" + sn.reason + ")");
    PitchPosition two = notes; two.windows.resize (12);
    check (deriveShortNotes (two, 30.0).result == "refused" && deriveShortNotes (two, 30.0).reason.contains ("fewer than 3 readable notes"), "tuner V2: two notes are not enough (a guard, not a number)");
    // tolerance: strengths at 5/10/20/30/50 cents
    // Auto-Tune Pro at Flex-Tune 86 (4 Oct): 5 -> 1.00, 10 -> 0.63, 20 -> 0.24, 30 -> 0.10, 40 -> 0.04, 45 -> 0.02: the window is 10 cents
    const auto t1 = toleranceFrom ({ { 5.0, 1.002 }, { 10.0, 0.631 }, { 20.0, 0.236 }, { 30.0, 0.104 }, { 40.0, 0.037 }, { 45.0, 0.015 } });
    check (t1.ok && ! t1.over && ! t1.none && t1.cents == 10.0 && t1.reason.contains ("up to 10 cents are corrected"), "tuner V3: the flex window is the LARGEST detune still corrected to at least half (10 cents at Flex-Tune 86)");
    const auto t2 = toleranceFrom ({ { 5.0, 0.0 }, { 45.0, 0.0 } });
    check (t2.ok && t2.none && t2.reason.contains ("nothing corrected, not even 5"), "tuner V4: Flex-Tune 100 corrects nothing, said as such (50 is never a rung: it is the midpoint between two notes)");
    const auto t3 = toleranceFrom ({ { 5.0, 1.0 }, { 45.0, 1.0 } });
    check (t3.ok && t3.over && t3.cents == 45.0, "tuner V4b: Flex-Tune 0 corrects everything up to the ladder's top: over_45");
    check (! toleranceFrom ({}).ok, "tuner V5: no detune measured -> no tolerance");
    check (std::find (kFlexDetunesCents.begin(), kFlexDetunesCents.end(), 50.0) == kFlexDetunesCents.end() && kFlexDetunesCents.back() == 45.0, "tuner V5b: the detune ladder never asks 50 cents (the midpoint between two chromatic notes, corrected UP on Pro and Artist)");
    // the exporter on an Auto-Tune-shaped record: speed from the moving transition, direction in display terms, strength null, notes for every null
    const auto rec = juce::JSON::parse (R"json({"schema": "ej_cert_tuner/1", "product": "AT", "manufacturer": "Antares", "identity": "AudioUnit|1|1.0", "version": "1.0", "pitchPlan": {"version": 2},
        "controls": [{"index": 4, "name": "Retune Speed"}, {"index": 7, "name": "Bypass", "defaultOnInstantiate": {"display": "Off", "normalised": 0.0}}, {"index": 9, "name": "Formant", "defaultOnInstantiate": {"display": "Off", "normalised": 0.0}}],
        "pitchCandidates": [{"index": 4, "name": "Retune Speed", "measuredAt": "20261004T120000", "detentsBy": "even8", "generator": {"latency_samples": 2670}, "positions": [
            {"norm": 0.0, "display": "400", "strength": {"result": "measured", "strength": 0.994}, "speed": {"result": "measured", "duration_ms": 1099, "half_period_s": 2.0, "edge_durations_ms": [1090, 1108]}},
            {"norm": 0.5, "display": "50", "strength": {"result": "measured", "strength": 1.0}, "speed": {"result": "refused", "reason": "edges disagree"}},
            {"norm": 1.0, "display": "0", "strength": {"result": "measured", "strength": 1.0}, "speed": {"result": "bound", "faster_than_ms": 21.4}}]}],
        "pitchExtras": {"flex": [{"index": 5, "name": "Flex-Tune", "positions": [{"norm": 0.0, "display": "0", "window_cents": "over_45", "strength_by_detune": {"5": 0.9}}, {"norm": 1.0, "display": "100", "window_cents": "none", "strength_by_detune": {"50": 0.1}}]}],
                        "humanize": [], "readbacks": {"1": {"index": 1, "name": "Key", "kind": "key", "values": {"C": 0.0, "C#": 0.0909}}, "2": {"index": 2, "name": "Scale", "kind": "scale", "values": {"Major": 0.0, "Chromatic": 1.0}}}}})json");
    const auto e = ejmap::tunerprofile::exportTunerProfileDraft (rec);
    const auto sp = e.profile.getProperty ("speed", {});
    check (e.ok && e.profile.getProperty ("schema", "").toString() == "ej_tuner_profile/1" && e.profile.getProperty ("status", "").toString().startsWith ("PROPOSAL v0.1 - not for publication"), "tuner V6: the draft says what it is");
    check (sp.getProperty ("control", "").toString() == "Retune Speed" && sp.getProperty ("direction", "").toString() == "lower_is_harder" && sp.getProperty ("curve", {}).size() == 3
             && sp.getProperty ("curve", {})[1].getProperty ("transition_ms", 1).isVoid() && (double) sp.getProperty ("curve", {})[2].getProperty ("faster_than_ms", 0.0) == 21.4,
           "tuner V7: speed = the moving candidate; lower_is_harder read from the display (400 slow, 0 fast); a refused position is null, a bound carries faster_than_ms");
    check (e.profile.getProperty ("strength", 1).isVoid() && e.notes.joinIntoString ("|").contains ("strength null") && e.notes.joinIntoString ("|").contains ("speed null at norm 0.5"), "tuner V8: strength null (1.0 throughout) with a note; the null speed point has its note");
    check (e.profile.getProperty ("flex", {}).getProperty ("curve", {}).size() == 2 && e.profile.getProperty ("humanize", 1).isVoid() && (double) e.profile.getProperty ("key", {}).getProperty ("values", {}).getProperty ("C#", 0.0) == 0.0909
             && e.profile.getProperty ("scale", {}).getProperty ("values", {}).hasProperty ("Chromatic"), "tuner V9: flex, key and scale carried from the extras; humanize null (no such control)");
    check (e.profile.getProperty ("never_touch", {}).size() == 1 && e.profile.getProperty ("never_touch", {})[0].toString() == "Bypass" && e.profile.getProperty ("neutral", {}).size() == 1, "tuner V10: Bypass is never_touch, Formant is neutral as instantiated, the swept control is neither");
    check (! ejmap::tunerprofile::exportTunerProfileDraft (juce::JSON::parse (R"json({"schema": "ej_cert_compressor/1"})json")).ok, "tuner V11: a compressor record is refused");
    // crispytuner's shape: Amount moves both speed and strength -> the strength field is null with the note that the speed control carries it
    const auto crispy = juce::JSON::parse (R"json({"schema": "ej_cert_tuner/1", "product": "bx", "pitchCandidates": [{"index": 12, "name": "Amount", "positions": [
        {"norm": 0.0, "display": "0", "strength": {"result": "measured", "strength": 0.0}, "speed": {"result": "measured", "duration_ms": 170}},
        {"norm": 1.0, "display": "100", "strength": {"result": "measured", "strength": 1.07}, "speed": {"result": "measured", "duration_ms": 45}}]}]})json");
    const auto ec = ejmap::tunerprofile::exportTunerProfileDraft (crispy);
    check (ec.ok && ec.profile.getProperty ("speed", {}).getProperty ("control", "").toString() == "Amount" && ec.profile.getProperty ("speed", {}).getProperty ("direction", "").toString() == "higher_is_harder"
             && ec.profile.getProperty ("strength", 1).isVoid() && ec.notes.joinIntoString ("|").contains ("is also the strength control"), "tuner V12: crispytuner's Amount is speed (higher_is_harder) and strength at once; said in a note");
}

/** SEAN'S STEPPED RULE (agreed 4 Oct; A6b): (1) stepped: true with the detent norms as read back, the curve lists only detents;
    (2) every measured detent gets the 0.5 dB hold test; (3) no estimation between detents, past the last detent is at_control_limit;
    (4) fewer than 3 detents reaching 1 dB is not publishable. Lindell 254E (16 detents, its own record) and a 6-detent UnFairchild shape. */
void testSteppedExport()
{
    using namespace ejmap::profile;
    const auto rec254 = juce::JSON::parse (juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/cert-traces/2026-10-04-stepped/lindell_254e_record.json").loadFileAsString());
    check (rec254.isObject() && (int) rec254.getProperty ("amountLanding", {}).getProperty ("detents", 0) == 16, "stepped Z0: the 254E record is read (16 detents by the landing read of 4 Oct)");
    const auto e = exportCompProfile (rec254);
    const auto amount = e.profile.getProperty ("amount", {});
    const auto curve = amount.getProperty ("curve", {});
    check (e.ok && (bool) amount.getProperty ("stepped", false) && amount.hasProperty ("stepped_by_evidence") && curve.size() == 16, "stepped Z1 (cond. 1): 254E exports stepped: true with its 16 detents (" + e.refused + ")");
    {
        bool onReadBack = true; const auto rb = detentNormsReadBack (rec254.getProperty ("amountLanding", {}));
        for (int i = 0; i < curve.size(); ++i) { const double n = (double) curve[i].getProperty ("norm", -1.0); bool hit = false; for (double v : rb) if (std::abs (v - n) < 1e-6) hit = true; onReadBack = onReadBack && hit; }
        check (rb.size() == 16 && onReadBack, "stepped Z2 (cond. 1): every curve norm is a value the plugin READ BACK, not an asked norm");
    }
    {
        // (2) the hold test: the record's own deep_point_error / quality carries the 0.5 dB hold test per point; a point that fails it is null with the reason
        const auto q = e.profile.getProperty ("quality", {});
        check (q.hasProperty ("point_error_db") && (double) q.getProperty ("point_error_db", 9.0) <= 0.5, "stepped Z3 (cond. 2): the hold test ran on the detents (point_error_db " + q.getProperty ("point_error_db", {}).toString() + " within 0.5)");
        auto bad = juce::JSON::parse (juce::JSON::toString (rec254));
        // the hold test's verdict lives on the record (quality.deepPointsNulled, written when the sweep compared its 2.5 s and 5 s
        // holds): a detent whose 4 dB point failed it by 0.8 dB must be null in the export, with the reason, the profile still exported
        // (the sweep nulls the point in the record's inAtGr when the two holds disagree by more than 0.5 dB, and accounts for it in deepPointsNulled)
        { juce::Array<juce::var> nulled; nulled.add ("3@4: -24.00 / hold-doubled -23.20 / delta 0.80 dB"); bad.getProperty ("thresholdSweep", {}).getProperty ("quality", {}).getDynamicObject()->setProperty ("deepPointsNulled", nulled);
          bad.getProperty ("thresholdSweep", {}).getProperty ("inAtGr", {})[3].getDynamicObject()->setProperty ("4", juce::var()); }
        const auto eb = exportCompProfile (bad);
        const auto p3 = eb.profile.getProperty ("amount", {}).getProperty ("curve", {})[3].getProperty ("in_at_gr_dbfs", {});
        check (eb.ok && p3.getProperty ("4", 1).isVoid() && juce::JSON::toString (eb.profile.getProperty ("notes", {})).contains ("hold test failed"), "stepped Z4 (cond. 2): a detent whose 4 dB point fails the 0.5 dB hold test is null with the reason, the profile stands (ok " + juce::String ((int) eb.ok) + ", p3.4 void " + juce::String ((int) p3.getProperty ("4", 1).isVoid()) + ", notes " + juce::JSON::toString (eb.profile.getProperty ("notes", {})).substring (0, 300) + ")");
    }
    {
        // (3) the pick on a stepped profile: nearest detent, never an estimate; past the last detent -> at_control_limit
        const auto pk = pickPosition (e.profile, -40.0, 2.0);
        check (pk.ok && pk.stepped && pk.i1 < 0 && ! pk.atControlLimit, "stepped Z5 (cond. 3): a pick inside the detents is one detent, never interpolated");
        double lo = 0.0, hi = -200.0;
        for (int i = 0; i < curve.size(); ++i) if (const auto v = inAtGr (curve[i], 2.0)) { lo = juce::jmin (lo, *v); hi = juce::jmax (hi, *v); }
        const auto past = pickPosition (e.profile, hi + 6.0, 2.0);
        check (past.ok && past.atControlLimit && past.note.contains ("at_control_limit") && std::abs (past.inAtG0 - hi) < 1e-9, "stepped Z6 (cond. 3): an ask past the last detent is at_control_limit and the end detent answers");
        const auto before = pickPosition (e.profile, lo - 6.0, 2.0);
        check (before.ok && before.atControlLimit && std::abs (before.inAtG0 - lo) < 1e-9, "stepped Z7 (cond. 3): and past the first detent likewise");
    }
    // (4) a 6-detent UnFairchild shape: 5 detents reach 1 dB -> publishable with those 5; 2 -> not publishable, reason named
    auto six = juce::JSON::parse (juce::JSON::toString (rec254));
    {
        auto* o = six.getDynamicObject(); o->setProperty ("product", "UnFairchild-shaped");
        auto sw = six.getProperty ("thresholdSweep", {}); auto* so = sw.getDynamicObject();
        juce::Array<juce::var> norms, inAt, texts, rep; juce::Array<juce::var> samples;
        for (int k = 0; k < 6; ++k)
        {
            const double n = k / 5.0; norms.add (n); texts.add (juce::String (k * 2));
            auto* p = new juce::DynamicObject(); auto* r = new juce::DynamicObject();
            if (k == 0) { for (const char* g : { "1", "2", "3" }) { p->setProperty (g, "not_reached"); r->setProperty (g, "not_reached"); } }
            else { const double base = -4.0 - 5.0 * k; p->setProperty ("1", base); p->setProperty ("2", base + 3.1); p->setProperty ("3", base + 5.0); r->setProperty ("1", base + 0.02); r->setProperty ("2", base + 3.12); r->setProperty ("3", base + 5.01); }
            inAt.add (juce::var (p)); rep.add (juce::var (r));
        }
        for (int k = 0; k <= 40; ++k) { auto* sm = new juce::DynamicObject(); const double asked = k / 40.0; sm->setProperty ("norm", asked); sm->setProperty ("getValue", std::round (asked * 5.0) / 5.0); samples.add (juce::var (sm)); }
        so->setProperty ("positionNorms", norms); so->setProperty ("positionTexts", texts); so->setProperty ("inAtGr", inAt); so->setProperty ("inAtGrRepeat", rep);
        so->setProperty ("thresholdDbEquivalent", juce::var()); so->removeProperty ("reduction_db"); so->removeProperty ("inAtGrQuality");
        auto* ev = new juce::DynamicObject(); ev->setProperty ("control", (int) six.getProperty ("amountLanding", {}).getProperty ("control", -1)); ev->setProperty ("detents", 6); ev->setProperty ("samples", samples);
        o->setProperty ("amountLanding", juce::var (ev)); o->setProperty ("thresholdSweep", sw);
    }
    const auto e6 = exportCompProfile (six);
    check (e6.ok && (bool) e6.profile.getProperty ("amount", {}).getProperty ("stepped", false) && e6.points == 5,
           "stepped Z8 (cond. 4): six detents of which five reach 1 dB export stepped with five points (the nine-point rule does not apply) - " + e6.refused);
    {
        auto two = juce::JSON::parse (juce::JSON::toString (six));
        auto sw = two.getProperty ("thresholdSweep", {}); auto inAt = sw.getProperty ("inAtGr", {});
        for (int k = 1; k <= 3; ++k) if (auto* p = inAt[k].getDynamicObject()) for (const char* g : { "1", "2", "3" }) p->setProperty (g, "not_reached");
        const auto e2 = exportCompProfile (two);
        check (! e2.ok && e2.refused.contains ("only 2 detent(s) reach 1 dB") && e2.refused.contains ("at least 3"), "stepped Z9 (cond. 4): two detents reaching 1 dB is not publishable, the reason names the rule (" + e2.refused + ")");
    }
    // the plan sweeps a control with landing evidence at its read-back detents; a record swept elsewhere is re-swept
    {
        const auto plan = ejmap::sweep::planFromFixture (six);
        check (plan.ok && plan.stepped && plan.norms.size() == 6 && std::abs (plan.norms[1] - 0.2f) < 1e-5f, "stepped Z10: the plan for a control with landing evidence sweeps its 6 read-back detents and nothing between");
        auto elsewhere = juce::JSON::parse (juce::JSON::toString (six));
        { juce::Array<juce::var> n16; for (int k = 0; k < 16; ++k) n16.add (k / 15.0); elsewhere.getProperty ("thresholdSweep", {}).getDynamicObject()->setProperty ("positionNorms", n16); }
        const auto d = ejmap::loop::planDiffers (elsewhere, plan);
        check (d.resweep && d.why.contains ("holds 6 detents"), "stepped Z11: a record swept at 16 positions over 6 detents is re-swept at the detents (" + d.why + ")");
        check (! ejmap::loop::planDiffers (six, plan).resweep, "stepped Z12: a record swept at the detents is not re-swept for that");
    }
}

/** GAIN / OUTPUT CALIBRATION (EjmapGainCal.h, roadmap 2.1 PROTOTYPE, 5 Oct B1): the derivation on probe-shaped text and the 4/5 Oct cases. */
void testGainCal()
{
    using namespace ejmap::gaincal;
    auto run = [] (double level, const std::vector<std::tuple<double, const char*, double>>& pts) {   // norm, display, out-in
        juce::String out = "sweep\tproto\t1\tthr\t3\n";
        int k = 0; for (const auto& [n, d, g] : pts) { out << "pos\t" << k << "\tnorm\t" << juce::String (n, 6) << "\tconfirm_ms\t2.0\tslices\t1\tinstack_match\t1\tgetValue\t" << juce::String (n, 6) << "\tlanded_by\tinstack\trender_blocks\t0\ttext_ms\t1\treads\t2\ttext\t" << d << "\n";
                                                       out << "hold\t" << k << "\t" << juce::String (level, 2) << "\tlevel_db\t" << juce::String (level - 3.0103 + g, 4) << "\tin_rms_db\t" << juce::String (level - 3.0103, 4) << "\ttone_frac\t1.0\n"; ++k; }
        return out; };
    // bx_opto's Output Gain: whole-dB labels, 1.2 dB steps: 4.79 at "5 dB" is the label's rounding, not an error
    std::vector<std::tuple<double, const char*, double>> opto { { 0.0, "0 dB", 0.0 }, { 0.2, "5 dB", 4.79 }, { 0.4, "10 dB", 9.59 }, { 0.6, "14 dB", 14.39 }, { 0.8, "19 dB", 19.19 }, { 1.0, "24 dB", 23.99 } };
    const auto rows = mergeLevels ({ parseLevelRun (run (-20.0, opto), -20.0), parseLevelRun (run (-40.0, opto), -40.0) });
    check (rows.size() == 6 && rows[1].measuredDb.size() == 2 && std::abs (rows[1].measuredDb.at (-20.0) - 4.79) < 1e-3, "gaincal G1: the two levels merge by norm, out - in per level");
    const auto c = judge (rows, "dB");
    check (c.verdict == "display_matches" && std::abs (c.worstOffDb - 0.41) < 0.02 && std::abs (c.barDb - 0.5) < 1e-9 && ! c.levelDependent, "gaincal G2: whole-dB labels judged at half a dB (bx_opto: 0.41 off, matches) - " + c.note);
    check (displayResolution ("5 dB") == 1.0 && displayResolution ("6.00") == 0.01 && std::abs (displayResolution ("4.8") - 0.1) < 1e-9, "gaincal G3: the label's resolution from its decimals");
    // AN INPUT GAIN INSIDE THE COMPRESSION PATH (R8c): honest at -60, compressed by 4 dB at the top at -40 and by 8 at -20 - judged at -60 it matches; at -40 it is "off"
    std::vector<std::tuple<double, const char*, double>> in60 { { 0.0, "0 dB", 0.0 }, { 0.5, "10 dB", 10.0 }, { 1.0, "20 dB", 20.0 } }, in40 { { 0.0, "0 dB", 0.0 }, { 0.5, "10 dB", 9.0 }, { 1.0, "20 dB", 16.0 } }, in20 { { 0.0, "0 dB", 0.0 }, { 0.5, "10 dB", 7.0 }, { 1.0, "20 dB", 12.0 } };
    const auto inRows = mergeLevels ({ parseLevelRun (run (-20.0, in20), -20.0), parseLevelRun (run (-40.0, in40), -40.0), parseLevelRun (run (-60.0, in60), -60.0) });
    check (kInputLevelsDbfs.size() == 3 && kInputRefDbfs == -60.0, "gaincal G7: inputs get a third level, -60 dBFS, and are judged there");
    const auto j60 = judge (inRows, "dB", kInputRefDbfs), j40 = judge (inRows, "dB", -40.0);
    // KATHY'S GAIN SPEC v0.1 section 5 (6 Oct): level_dependent is a VERDICT - a stage inside the processing, never written; inputs judged -60 against -40
    check (j60.verdict == "level_dependent" && j60.levelDependent && j60.worstLevelDepDb > 3.9 && j60.note.contains ("judged -60 against -40"), "gaincal G8 (spec v0.1): an input honest at -60 but 4 dB lower at -40 is level_dependent, judged -60 against -40 (" + j60.verdict + ": " + j60.note + ")");
    check (j40.verdict == "level_dependent" && j40.note.contains ("-40 against -20"), "gaincal G9 (spec v0.1): judged at -40 the same control is level_dependent against -20 (" + j40.verdict + ")");
    check (! writable ("level_dependent") && ! writable ("no_effect") && writable ("display_off") && writable ("not_db_scale") && writable ("display_matches"), "gaincal G9b: the server may write display_matches / display_off / not_db_scale by curve, never level_dependent or no_effect");
    check (! displayDb ("-144.0 dB") && ! displayDb ("-inf dB") && displayDb ("-60.0 dB") && *displayDb ("-60.0 dB") == -60.0, "gaincal G4b (spec v0.1): a label at or below -120 dB is a floor word, not a dB number; -60 is a number");
    { auto rowsOf = [] (std::vector<double> v) { std::vector<Reading> rs; for (size_t i = 0; i < v.size(); ++i) { Reading r; r.norm = (double) i / (double) (v.size() - 1); r.measuredDb[-40.0] = v[i]; rs.push_back (r); } return rs; };
      check (isMonotonic (rowsOf ({ 0.0, 3.0, 7.0, 12.0 }), -40.0) && isMonotonic (rowsOf ({ 12.0, 7.0, 3.0, 0.0 }), -40.0) && ! isMonotonic (rowsOf ({ 0.0, 5.0, 2.0, 8.0 }), -40.0) && isMonotonic (rowsOf ({ 0.0, 3.0, 2.95, 6.0 }), -40.0), "gaincal G10: monotonic up or down within 0.1 dB; a 3 dB reversal is not");
      const auto rs = rowsOf ({ 0.0, 4.0, 8.0, 12.0 });
      const auto i6 = normForDb (rs, 6.0, -40.0, false); check (i6.ok && std::abs (i6.norm - 0.5) < 1e-9 && std::abs (i6.givesDb - 6.0) < 1e-9 && ! i6.clamped, "gaincal G11: +6 dB on a 0..12 curve inverts to norm 0.5 by interpolation");
      const auto i20 = normForDb (rs, 20.0, -40.0, false); check (i20.ok && std::abs (i20.norm - 1.0) < 1e-9 && std::abs (i20.givesDb - 12.0) < 1e-9 && i20.clamped, "gaincal G11: a target past the span writes the end and says it is clamped (gives 12)");
      const auto is = normForDb (rs, 6.5, -40.0, true); check (is.ok && (std::abs (is.norm - 0.333333) < 1e-3 || std::abs (is.norm - 0.666667) < 1e-3) && std::abs (is.givesDb - (is.norm < 0.5 ? 4.0 : 8.0)) < 1e-9, "gaincal G11: a stepped control takes the nearest detent and reports the dB it gives");
      const auto ok = acceptanceOf (6.0, i6, 6.15), bad = acceptanceOf (6.0, i6, 6.5), none = acceptanceOf (6.0, i6, std::nullopt);
      check (ok.ran && ok.pass && std::abs (ok.missDb - 0.15) < 1e-9 && bad.ran && ! bad.pass && std::abs (bad.missDb - 0.5) < 1e-9 && ! none.ran, "gaincal G12 (section 8): a re-measured write passes within 0.2 dB of what the curve promised; 0.5 dB fails; no reading = not run");
      const auto det = acceptanceOf (6.5, is, is.givesDb + 0.1); check (det.ran && det.pass && std::abs (det.expectedDb - is.givesDb) < 1e-9, "gaincal G12: a stepped control is judged against the detent's own measured value, not the target"); }
    check (displayDb ("+6.0 dB") && *displayDb ("+6.0 dB") == 6.0 && displayDb ("-10.00") && ! displayDb ("Off") && ! displayDb ("Max"), "gaincal G4: numeric labels parse, words do not");
    // SBC's Output Gain: unit-less "6.00" that the output tracks IS a dB label
    std::vector<std::tuple<double, const char*, double>> sbc { { 0.0, "-10.00", -10.01 }, { 0.5, "0.00", -0.01 }, { 1.0, "10.00", 9.99 } };
    const auto cs = judge (mergeLevels ({ parseLevelRun (run (-40.0, sbc), -40.0) }), "");
    check (cs.verdict == "display_matches" && cs.hasZeroPoint && std::abs (cs.barDb - 0.1) < 1e-9, "gaincal G5: a unit-less label the output tracks matches (relative to its own 0.00 point)");
    // Solid Bus Comp's Output: the unit sits 2.35 dB above unity at "0.00" and the labels track RELATIVE to that point
    std::vector<std::tuple<double, const char*, double>> sbcOut { { 0.0, "-6.00", -3.65 }, { 0.5, "0.00", 2.35 }, { 1.0, "6.00", 8.35 } };
    const auto co = judge (mergeLevels ({ parseLevelRun (run (-40.0, sbcOut), -40.0) }), "dB");
    check (co.verdict == "display_matches" && std::abs (co.zeroRefDb - 2.35) < 1e-6 && co.worstOffDb < 1e-6, "gaincal G5b: a label is judged relative to the control's own 0.00 point (Solid Bus Comp's Output sits 2.35 dB above unity there)");
    // U2A's Gain: 0..100 %, output 0..35 dB: a scale, not a dB label
    std::vector<std::tuple<double, const char*, double>> u2a { { 0.0, "0.0", 0.18 }, { 0.5, "50.0", 12.0 }, { 1.0, "100.0", 34.59 } };
    check (judge (mergeLevels ({ parseLevelRun (run (-40.0, u2a), -40.0) }), "%").verdict == "not_db_scale", "gaincal G6: a percentage scale the output does not track is listed, not judged");
    // a dB label the output does not track: display_off (Solid Bus Comp's Makeup 1.00 dB off)
    std::vector<std::tuple<double, const char*, double>> sbc2 { { 0.0, "0.00", 0.0 }, { 0.5, "6.00", 5.0 }, { 1.0, "12.00", 11.0 } };
    check (judge (mergeLevels ({ parseLevelRun (run (-40.0, sbc2), -40.0) }), "dB").verdict == "display_off", "gaincal G7: a dB label the output misses by 1 dB is display_off");
    // level dependence: SBC's "Gain" drives the compressor at -20
    const auto dep = judge (mergeLevels ({ parseLevelRun (run (-20.0, { { 0.0, "0.00", 0.0 }, { 0.5, "12.00", 6.3 }, { 1.0, "24.00", 12.4 } }), -20.0), parseLevelRun (run (-40.0, { { 0.0, "0.00", 0.0 }, { 0.5, "12.00", 10.2 }, { 1.0, "24.00", 19.6 } }), -40.0) }), "dB");
    check (dep.levelDependent && dep.worstLevelDepDb > 7.0 && dep.verdict == "level_dependent", "gaincal G8: two levels disagreeing by 7 dB is the level_dependent VERDICT (an input gain inside the compression path; spec v0.1)");
    // no effect, silence, words
    check (judge (mergeLevels ({ parseLevelRun (run (-40.0, { { 0.0, "0", 0.0 }, { 0.5, "50", 0.01 }, { 1.0, "100", 0.0 } }), -40.0) })).verdict == "no_effect", "gaincal G9: a control that moves nothing is no_effect (Virtual Gain, EQ Gain)");
    check (judge (mergeLevels ({ parseLevelRun (run (-40.0, { { 0.0, "0", -806.0 }, { 1.0, "10", -800.0 } }), -40.0) })).verdict == "unreadable", "gaincal G10: silence is not a reading (7X-500's Output with its Input at minimum)");
    check (judge (mergeLevels ({ parseLevelRun (run (-40.0, { { 0.0, "Off", 0.0 }, { 0.5, "Mid", 3.0 }, { 1.0, "Max", 6.0 } }), -40.0) })).verdict == "words", "gaincal G11: word labels are listed, not judged");
    check (judge (mergeLevels ({ parseLevelRun (run (-40.0, { { 0.0, "0.0", 0.0 }, { 0.5, "Mid", 3.0 }, { 1.0, "Max", 6.0 } }), -40.0) })).verdict == "few_numeric_points", "gaincal G12: one numeric label among words is too few to judge");
}

/** COMPRESSOR TIMING (EjmapTiming.h, roadmap 2.3 PROTOTYPE, 5 Oct B2): synthetic exponential bursts with the rule's own numbers, and two of
    Lindell SBC's 5 Oct traces read through the follow-up's own parser (with an unread-file control). */
void testTiming()
{
    // KATHY'S TIMING SPEC v0.1 (6 Oct): the hold scales, a bound attack gets the 4 kHz / 1 ms second pass, gr_shift_db against the
    // instantiate position, the section 7 position shape - each a pure rule, each with a mutant
    {
        using namespace ejmap::timing;
        check (std::abs (scaledHoldS (std::nullopt, 2.0) - 2.0) < 1e-9 && std::abs (scaledHoldS (48.0, 2.0) - 2.0) < 1e-9 && std::abs (scaledHoldS (500.0, 2.0) - 5.0) < 1e-9 && std::abs (scaledHoldS (5000.0, 2.0) - 30.0) < 1e-9 && std::abs (scaledHoldS (100.0, 3.0) - 3.0) < 1e-9,
               "timing TS1: the hold is the longer of 10 x the first-pass attack and 2 s (48 ms -> 2 s, 500 ms -> 5 s), capped at 30 s, never below the segment the label gave");
        Timing bound; bound.result = "bound"; bound.attackBoundMs = 6.4; Timing meas; meas.result = "measured"; meas.attackMs = 8.8; Timing ref; ref.result = "refused";
        check (secondPassNeeded (bound) && ! secondPassNeeded (meas) && ! secondPassNeeded (ref), "timing TS2: only a measured-or-bound first pass whose attack is a bound gets the second pass");
        Timing fast; fast.result = "measured"; fast.attackMs = 0.9; Timing fastBound; fastBound.result = "bound"; fastBound.attackBoundMs = 1.0;
        const auto a1 = attackAfterPasses (bound, fast), a2 = attackAfterPasses (bound, fastBound), a3 = attackAfterPasses (meas, std::nullopt), a4 = attackAfterPasses (bound, std::nullopt);
        check (a1.attackMs && std::abs (*a1.attackMs - 0.9) < 1e-9 && a1.pass.startsWith ("4 kHz"), "timing TS3: the second pass's time stands when the first was a bound");
        check (! a2.attackMs && a2.fasterThanMs && std::abs (*a2.fasterThanMs - 1.0) < 1e-9, "timing TS3: a bound in the second pass too is 'faster than 1 ms'");
        check (a3.attackMs && std::abs (*a3.attackMs - 8.8) < 1e-9 && a3.pass.startsWith ("997"), "timing TS3: a measured first pass needs no second");
        check (! a4.attackMs && a4.fasterThanMs && std::abs (*a4.fasterThanMs - 6.4) < 1e-9, "timing TS3: no second pass = the first pass's bound");
        check (std::abs (grShiftDb (8.1, 8.0) - 0.1) < 1e-9 && ! shiftsAmount (0.4) && shiftsAmount (0.6) && shiftsAmount (-0.6), "timing TS4: gr_shift_db is the position's steady GR step minus the instantiate position's; over 0.5 dB shifts the amount");
        const auto pa = timePosition (0.0, "0.03 ms", "attack", bound, fastBound, 0.0);
        check (pa.getProperty ("attack_ms", 1.0).isVoid() && std::abs ((double) pa.getProperty ("faster_than_ms", 0.0) - 1.0) < 1e-9 && ! pa.hasProperty ("shifts_amount") && std::abs ((double) pa.getProperty ("gr_shift_db", 9.0)) < 1e-9,
               "timing TS5: a section 7 attack position: attack_ms null with faster_than_ms when only a bound was measurable, gr_shift_db on it");
        Timing rel; rel.result = "measured"; rel.releaseMs = 2066.3;
        const auto pr = timePosition (1.0, "3.000", "release", rel, std::nullopt, 0.7);
        check (std::abs ((double) pr.getProperty ("release_ms", 0.0) - 2066.3) < 1e-9 && (bool) pr.getProperty ("shifts_amount", false), "timing TS5: a release position carries release_ms and shifts_amount when the shift is over 0.5 dB");
        check (juce::String (kDefinition).startsWith ("63%"), "timing TS6: the definition string is the 63 % (one time constant) one, required by the spec");
    }
    using namespace ejmap::timing;
    // a synthetic burst: 1 s pre at gain 0, 2 s loud with GR 6 dB reached as 1 - exp(-t/tauA), 3 s post recovering as exp(-t/tauR); 5 ms windows; latency none
    auto synth = [] (double tauAMs, double tauRMs, double stepDb, double holdS = 2.0, double postS = 3.0) {
        juce::String out = "burst\tproto\t1\tquiet\t-36.00\tloud\t-20.00\tpre_s\t1.000\thold_s\t" + juce::String (holdS, 3) + "\tpost_s\t" + juce::String (postS, 3) + "\thz\t997.000\twin_ms\t5.00\nconfig\tmain_in\t2\tmain_out\t2\tlatency\t0\n";
        for (double t = 2.5; t < (1.0 + holdS + postS) * 1000.0; t += 5.0)
        {
            const char* seg = t < 1000.0 ? "pre" : t < (1.0 + holdS) * 1000.0 ? "loud" : "post";
            const double in = t < 1000.0 || t >= (1.0 + holdS) * 1000.0 ? -39.01 : -23.01;
            double gain = 0.0;
            if (t >= 1000.0 && t < (1.0 + holdS) * 1000.0) gain = -stepDb * (1.0 - std::exp (-(t - 1000.0) / tauAMs));
            else if (t >= (1.0 + holdS) * 1000.0) gain = -stepDb * std::exp (-(t - (1.0 + holdS) * 1000.0) / tauRMs);
            out << "bwin\tt_ms\t" << juce::String (t, 2) << "\tseg\t" << seg << "\tin_db\t" << juce::String (in, 3) << "\tout_db\t" << juce::String (in + gain, 3) << "\n";
        }
        return out; };
    const auto a = derive (parseBurst (synth (50.0, 300.0, 6.0)));
    check (a.result == "measured" && std::abs (a.stepDb - 6.0) < 0.05 && a.attackMs && std::abs (*a.attackMs - 50.0) < 6.0 && a.releaseMs && std::abs (*a.releaseMs - 300.0) < 8.0,
           "timing K1: an exponential with tau 50 / 300 ms reads attack " + juce::String (a.attackMs.value_or (0), 1) + " and release " + juce::String (a.releaseMs.value_or (0), 1) + " (63 % = one time constant)");
    const auto fast = derive (parseBurst (synth (0.5, 300.0, 6.0)));
    check (fast.result == "measured" && ! fast.attackMs && fast.attackBoundMs && *fast.attackBoundMs <= 10.0, "timing K2: an attack inside the first window is a bound, never a number");
    const auto slow = derive (parseBurst (synth (50.0, 5000.0, 6.0)));
    check (slow.result == "bound" && ! slow.releaseMs && slow.releaseBoundMs && *slow.releaseBoundMs > 2900.0, "timing K3: a release not recovered inside the post segment is a bound (longer than)");
    const auto small = derive (parseBurst (synth (50.0, 300.0, 1.0)));
    check (small.result == "refused" && small.reason.contains ("only 1.00 dB"), "timing K4: a 1 dB step times nothing (refused, named)");
    const auto unsettled = derive (parseBurst (synth (1500.0, 300.0, 6.0, 2.0)));
    check (unsettled.result == "refused" && unsettled.reason.contains ("had not settled before the step down"), "timing K5: a gain still moving at the end of the hold refuses (hold too short), never a number");
    // the 5 Oct traces: SBC Release 3.000 -> 2066 ms, Attack 0.03 ms -> inside the first window
    const auto dir = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/cert-traces/2026-10-05-timing");
    const auto r3 = derive (parseBurst (dir.getChildFile ("sbc_release_3s.txt").loadFileAsString()));
    check (r3.result == "measured" && r3.releaseMs && std::abs (*r3.releaseMs - 2066.3) < 1.0 && std::abs (r3.stepDb - 4.28) < 0.02, "timing K6: Lindell SBC Release 3.000 reads 2066 ms from its trace (" + juce::String (r3.releaseMs.value_or (0), 1) + ")");
    const auto a0 = derive (parseBurst (dir.getChildFile ("sbc_attack_0p03ms.txt").loadFileAsString()));
    check (a0.result == "measured" && ! a0.attackMs && a0.attackBoundMs, "timing K7: SBC Attack 0.03 ms is a bound (inside the first window)");
    check (! parseBurst (dir.getChildFile ("no_such_trace.txt").loadFileAsString()).ok, "timing K8: an absent trace parses to nothing (the control for K6/K7)");
    // a window straddling a step is skipped: a +16 dB spike at the step-down window must not read as recovery
    {
        auto lines = juce::StringArray::fromLines (synth (50.0, 300.0, 6.0)); int spiked = 0;
        for (auto& l : lines) if (l.startsWith ("bwin\tt_ms\t3002.50\t")) { l = "bwin\tt_ms\t3002.50\tseg\tpost\tin_db\t-39.010\tout_db\t-23.010"; ++spiked; }
        const auto sp = derive (parseBurst (lines.joinIntoString ("\n")));
        check (spiked == 1, "timing K9a: the spike was injected into the step-down window");
        check (sp.result == "measured" && sp.releaseMs && std::abs (*sp.releaseMs - 300.0) < 8.0, "timing K9: a spike in the window straddling the step is ignored (the latency artefact)");
    }
}

/** THE HOLD SCALED TO THE LABEL (EjmapTiming.h, 5 Oct R8b): segments from the control's own time label, never shorter than the defaults, capped. */
void testTimingSegments()
{
    using namespace ejmap::timing;
    check (labelMs ("1.2 s") && std::abs (*labelMs ("1.2 s") - 1200.0) < 1e-9 && labelMs ("300 ms") && *labelMs ("300 ms") == 300.0 && labelMs ("50mS") && *labelMs ("50mS") == 50.0 && ! labelMs ("Auto") && ! labelMs ("4:1") && ! labelMs ("7"), "timing T1: time labels in s / ms parse; words, ratios and bare numbers are not times");
    check (std::abs (segmentFor ("1.2 s", kDefaultPostS) - 6.0) < 1e-9, "timing T2: a 1.2 s release gets a 6 s post (5x the label)");
    check (std::abs (segmentFor ("100 ms", kDefaultPostS) - kDefaultPostS) < 1e-9 && std::abs (segmentFor ("Auto", kDefaultHoldS) - kDefaultHoldS) < 1e-9, "timing T3: a short or wordy label keeps the default segment");
    check (std::abs (segmentFor ("20 s", kDefaultPostS) - kMaxSegmentS) < 1e-9, "timing T4: the segment is capped at 30 s");
}

/** LIMITER CEILINGS (EjmapLimiter.h, roadmap 2.4 PROTOTYPE, 5 Oct B3): the hold line's peaks, the ceiling positions from labels, the verdict with the 5 Oct cases. */
void testLimiter()
{
    using namespace ejmap::limiter;
    const auto pk = parsePeaks ("hold\t0\t-1.00\tlevel_db\t-4.51\tin_rms_db\t-4.01\ttone_frac\t1.0\tch\t-4.51\tdoubled\t0\tlast_move_db\t0\tfinal_move_db\t0\twin\t-4.5\tnonfinite\t0\tout_peak_db\t-1.5000\tout_true_peak_db\t-0.9200\n");
    check (pk.ok && std::abs (pk.peakDb + 1.5) < 1e-9 && std::abs (pk.truePeakDb + 0.92) < 1e-9, "limiter L1: the hold line's sample and true peaks parse");
    check (! parsePeaks ("hold\t0\t-1.00\tlevel_db\t-4.51\tin_rms_db\t-4.01\n").ok, "limiter L2: a hold line without peaks (an older probe) is no reading");
    std::vector<std::pair<float, juce::String>> grid; for (int k = 0; k <= 32; ++k) grid.push_back ({ k / 32.0f, juce::String (-12.0 + 12.0 * k / 32.0, 2) + " dB" });
    const auto pos = ceilingPositions (grid);
    check (pos.size() == 5 && std::abs (pos[0].labelDb + 0.0) < 0.2 && std::abs (pos[4].labelDb + 6.0) < 0.2, "limiter L3: five ceiling positions, the labels nearest -0.1/-0.3/-1/-3/-6 (" + juce::String ((int) pos.size()) + ")");
    check (labelDb ("-1.05 dB") && *labelDb ("-1.05 dB") == -1.05 && labelDb ("-0.3 dBTP") && ! labelDb ("Off"), "limiter L4: ceiling labels parse with their units");
    auto res = [] (double label, double peak, double tpeak) { CeilingResult r; r.pos.labelDb = label; r.reading.ok = true; r.reading.peakDb = peak; r.reading.truePeakDb = tpeak; r.sampleErrDb = peak - label; r.trueErrDb = tpeak - label; return r; };
    // MLimiterX: sample exact, true peak +0.58 at every ceiling
    const auto mx = judge ({ res (0.0, 0.0, 0.58), res (-0.75, -0.75, -0.17), res (-1.5, -1.5, -0.92), res (-3.0, -3.0, -2.42), res (-6.0, -6.0, -5.42) });
    check (mx.holdsSample && ! mx.holdsTrue && std::abs (mx.worstTrueDb - 0.58) < 1e-9 && mx.positions == 5, "limiter L5: MLimiterX holds the sample peak and overshoots true peak by 0.58 dB");
    // L2: both hold
    const auto l2 = judge ({ res (0.0, -0.01, -0.01), res (-0.9, -0.9, -0.9), res (-5.6, -5.6, -5.6) });
    check (l2.holdsSample && l2.holdsTrue, "limiter L6: L2 holds both");
    // bx_limiter True Peak: the -0.12 and -0.26 ceilings were never reached by the -1 dBFS output: not driven, excluded, said
    const auto bx = judge ({ res (-0.12, -1.02, -1.02), res (-0.26, -1.02, -1.02), res (-1.05, -1.07, -1.07), res (-2.93, -2.94, -2.94), res (-5.74, -5.75, -5.75) });
    check (bx.positions == 3 && bx.notDriven == 2 && bx.holdsSample && bx.holdsTrue && bx.note.contains ("2 position(s) not driven"), "limiter L7: ceilings the drive never reached are not counted as holding (bx TP at -0.12 / -0.26)");
    check (! judge ({ res (-0.1, -3.0, -3.0) }).holdsSample && judge ({ res (-0.1, -3.0, -3.0) }).note.contains ("no ceiling position was driven"), "limiter L8: nothing driven -> no verdict, said");
    check (! judge ({ res (-1.0, -0.85, -0.85) }).holdsSample && judge ({ res (-1.0, -0.9, -0.9) }).holdsSample, "limiter L9: the bar is 0.1 dB above the label (0.15 over fails, 0.10 holds)");
}

/** EQ RESPONSE (EjmapEq.h, roadmap 2.2 PROTOTYPE, 5 Oct B4): a synthetic peaking band and a shelf on the 61-tone grid; the band grouping on real names. */
void testEq()
{
    using namespace ejmap::eq;
    auto grid = [] { std::vector<double> hz; for (int k = 0; k < 121; ++k) hz.push_back (20.0 * std::pow (1000.0, k / 120.0)); return hz; }();   // the mode's grid: 1/12 octave
    auto pos = [&] (std::function<double (double)> gainAt) { Position p; p.k = 0; p.landed = true; for (double f : grid) { Tone t; t.hz = f; t.inDb = -30.0; t.outDb = -30.0 + gainAt (f); p.tones.push_back (t); } return p; };
    const auto base = pos ([] (double) { return 0.0; });
    // a peaking band: +6 dB at 1 kHz, bandwidth 1 octave (a Gaussian in log f whose half-power... -3 dB points at +-0.5 oct)
    auto peakAt = [] (double f0, double g, double bwOct) { return [=] (double f) { const double x = std::log2 (f / f0) / (bwOct / 2.0); return g * std::pow (2.0, -x * x) ; }; };   // 2^-x^2: at x = +-1 the gain is g/2 (-3 dB of a 6 dB peak... in dB terms g*0.5)
    {
        // in dB terms the -3 dB points of a +6 dB peak are at +3 dB: our shape gives g/2 = 3 dB at x = +-1, i.e. +-bw/2 -> bandwidth = bwOct
        const auto b = deriveBand (deviation (pos (peakAt (1000.0, 6.0, 1.0)), base));
        check (b.result == "measured" && b.shape == "peak" && std::abs (b.centreHz - 1000.0) < 25.0 && std::abs (b.gainDb - 6.0) < 0.1 && std::abs (b.bandwidthOct - 1.0) < 0.1,
               "eq E1: a +6 dB peak at 1 kHz, 1 octave wide: centre " + juce::String (b.centreHz, 0) + ", gain " + juce::String (b.gainDb, 2) + ", bw " + juce::String (b.bandwidthOct, 2));
        // a centre half a grid step off a tone (1030 Hz; the tones sit at 1002 and 1061): the parabolic refinement puts it within 1.5 %
        const auto m = deriveBand (deviation (pos (peakAt (1030.0, 6.0, 1.0)), base));
        check (m.result == "measured" && std::abs (m.centreHz - 1030.0) < 15.0, "eq E1b: a centre between two tones is refined to within 1.5 % (" + juce::String (m.centreHz, 1) + ")");
        const auto c = deriveBand (deviation (pos (peakAt (3000.0, -6.0, 0.5)), base));   // a 6 dB peak: its -3 dB points ARE its half-gain points, so the shape's width is the bandwidth
        check (c.result == "measured" && std::abs (c.centreHz - 3000.0) < 100.0 && std::abs (c.gainDb + 6.0) < 0.3 && std::abs (c.bandwidthOct - 0.5) < 0.1, "eq E2: a -6 dB cut at 3 kHz, half an octave, read to the 1/12-octave grid (gain within 0.3, bw within 0.1 oct): " + juce::String (c.centreHz, 0) + " / " + juce::String (c.gainDb, 2) + " / " + juce::String (c.bandwidthOct, 2));
    }
    {
        // a low shelf: +4 dB below 100 Hz, half the plateau at the corner (first-order shelf shape)
        auto shelf = [] (double f) { return 4.0 / (1.0 + std::pow (f / 100.0, 2.0)); };
        const auto b = deriveBand (deviation (pos (shelf), base));
        check (b.result == "shelf" && b.shape == "low_shelf" && std::abs (b.gainDb - 4.0) < 0.2 && std::abs (b.cornerHz - 100.0) < 10.0, "eq E3: a +4 dB low shelf (this first-order shape reads 3.85 at 20 Hz): plateau " + juce::String (b.gainDb, 2) + ", corner " + juce::String (b.cornerHz, 0) + " Hz (half the plateau)");
        auto hshelf = [] (double f) { return -6.0 / (1.0 + std::pow (4000.0 / f, 2.0)); };   // a 4 kHz corner: the plateau is reached inside the grid (an 8 kHz one is not by 20 kHz, and its half-plateau corner reads 13 % low - a grid fact, said in the proposal)
        const auto h = deriveBand (deviation (pos (hshelf), base));
        check (h.result == "shelf" && h.shape == "high_shelf" && std::abs (h.gainDb + 6.0) < 0.3 && std::abs (h.cornerHz - 4000.0) < 400.0, "eq E4: a -6 dB high shelf at 4 kHz, corner within 10 %: " + juce::String (h.cornerHz, 0));
    }
    check (deriveBand (deviation (pos ([] (double) { return 0.2; }), base)).result == "flat", "eq E5: a 0.2 dB deviation everywhere is flat (the control did nothing here)");
    check (labelNumber ("1.52k Hz") && std::abs (*labelNumber ("1.52k Hz") - 1520.0) < 1e-6 && labelNumber ("+4.0 dB") && *labelNumber ("+4.0 dB") == 4.0 && labelNumber ("370.0 Hz") && *labelNumber ("370.0 Hz") == 370.0 && ! labelNumber ("Off"), "eq E6: labels parse (k = x1000 for frequencies, not for dB)");
    // the bands from real names
    const auto bands = bandsFrom ({ { 52, "EQ Band HF 1 Gain" }, { 53, "EQ Band HF 1 Q" }, { 54, "EQ Band HF 1 Frequency" }, { 5, "Low boost" }, { 6, "Low frequency" }, { 1, "Hi attenuation" }, { 3, "Hi frequency" }, { 4, "Hi bandwidth" }, { 99, "Output Gain" }, { 7, "High Shelf Level 1" }, { 8, "High Shelf Frequency 1" } });
    juce::StringArray keys; for (const auto& b : bands) keys.add (b.key);
    check (bands.size() == 4 && keys.contains ("EQ Band HF 1") && keys.contains ("Low") && keys.contains ("Hi") && keys.contains ("High Shelf 1") && ! keys.contains ("Output"), "eq E7: bands by shared key with a gain and a frequency; a lone Output Gain is no band (" + keys.joinIntoString (",") + ")");
    for (const auto& b : bands) if (b.key == "EQ Band HF 1") check (b.gains == std::vector<int> { 52 } && b.freqs == std::vector<int> { 54 } && b.qs == std::vector<int> { 53 }, "eq E8: gain / freq / q by token (bx_digital's HF 1)");
    for (const auto& b : bands) if (b.key == "Hi") check (b.gains == std::vector<int> { 1 } && b.qs == std::vector<int> { 4 }, "eq E9: Pultec 'attenuation' is a gain, 'bandwidth' a Q");
    // THE ENGAGE SEARCH (R8a): the switches that share the band key's tokens, closest first; the ON norm by text; bypass inverted; band controls excluded
    {
        std::vector<std::tuple<int, juce::String, bool, std::map<juce::String, float>>> sw {
            { 10, "EQ Band HF 2 On", true, { { "Off", 0.0f }, { "On", 1.0f } } },
            { 11, "EQ Band HF 2 Bypass", true, { { "Off", 0.0f }, { "On", 1.0f } } },
            { 12, "Channel 2 In", true, { { "Out", 0.0f }, { "In", 1.0f } } },
            { 13, "EQ Band LF 2 On", true, { { "Off", 0.0f }, { "On", 1.0f } } },
            { 14, "EQ Band HF 2 Gain", false, {} },
            { 15, "Master Bypass", true, { { "Off", 0.0f }, { "On", 1.0f } } } };
        const auto cands = ejmap::eq::engageCandidates ("EQ Band HF 2", sw, { 14 });
        check (cands.size() == 2 && cands[0].index == 10 && cands[0].onNorm == 1.0f && cands[0].onText == "On" && cands[1].index == 11 && cands[1].onNorm == 0.0f && cands[1].onText == "Off",
               "eq E10: the band's own On switch first (On -> norm 1), its Bypass second with the ON position inverted (Off -> norm 0); another band's switch, a plain gain and a master bypass are out (" + juce::String ((int) cands.size()) + ")");
        const auto ch = ejmap::eq::engageCandidates ("Channel 2 HF", sw, {});
        check (ch.size() == 1 && ch[0].index == 12 && ch[0].onText == "In", "eq E11: a channel-level 'In' switch sharing the key's channel token is a candidate for a band that has no switch of its own");
    }
}

/** SATURATION HARMONICS (EjmapSaturation.h, roadmap 2.5 PROTOTYPE, 5 Oct R3): the rharm trace, THD / even-odd / onset / inert. */
void testSaturation()
{
    using namespace ejmap::saturation;
    // a trace: three positions; h2..h5 relative to a -12 dB fundamental
    auto pos = [] (int k, double norm, const char* text, double fundOut, double h2, double h3, double h4, double h5)
    {
        juce::String s;
        s << "rpos\t" << k << "\tnorm\t" << juce::String (norm, 6) << "\tconfirm_ms\t40.0\tlanded_by\twall\ttext\t" << text << "\n";
        s << "rtone\t" << k << "\thz\t997.000\tin_db\t-12.000\tout_db\t" << juce::String (fundOut, 3) << "\n";
        for (auto [o, d] : std::vector<std::pair<int, double>> { { 2, h2 }, { 3, h3 }, { 4, h4 }, { 5, h5 } })
            s << "rharm\t" << k << "\torder\t" << o << "\thz\t" << juce::String (997.0 * o, 3) << "\tin_db\t-150.000\tout_db\t" << juce::String (fundOut + d, 3) << "\n";
        s << "rdone\t" << k << "\ttones\t1\tnonfinite\t0\n";
        return s;
    };
    const juce::String trace = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t3\ttones\t1\tlo\t997.00\thi\t997.00\tdb\t-12.00\thold_s\t1.000\tdiscard_s\t0.500\n"
                               "config\tmain_in\t2\tmain_out\t2\tlatency\t0\n"
                             + pos (0, 0.0, "0", -12.0, -110.0, -112.0, -115.0, -118.0)      // clean
                             + pos (1, 0.5, "5", -11.0, -50.0, -70.0, -80.0, -90.0)          // warm: 2nd dominant, ~0.3 %
                             + pos (2, 1.0, "10", -8.0, -30.0, -26.0, -40.0, -36.0);         // driven: odd dominant, > 1 %
    const auto resp = parseHarmonics (trace);
    check (resp.ok && resp.ctl == 4 && resp.ctlName == "Drive" && resp.positions.size() == 3 && resp.positions[1].harmonics.size() == 4 && resp.positions[1].harmonics[0].order == 2 && std::abs (resp.positions[1].harmonics[0].outDb + 61.0) < 1e-9,
           "sat S1: the trace parses: the fundamental per position and four harmonic orders");
    const auto L = deriveLevel (resp, -12.0);
    check (L.readings.size() == 3 && L.readings[0].valid && std::abs (L.readings[0].gainDb - 0.0) < 1e-9 && std::abs (L.readings[2].gainDb - 4.0) < 1e-9 && std::abs (L.gainSpanDb - 4.0) < 1e-9, "sat S2: gain = fundamental out - in per position; the span over the positions");
    {
        const auto& w = L.readings[1];
        const double expectThd = 10.0 * std::log10 (std::pow (10.0, -5.0) + std::pow (10.0, -7.0) + std::pow (10.0, -8.0) + std::pow (10.0, -9.0));
        check (std::abs (w.thdDb - expectThd) < 1e-6 && std::abs (w.thdPct - 100.0 * std::sqrt (std::pow (10.0, expectThd / 10.0))) < 1e-6, "sat S3: THD is the harmonic power sum against the fundamental, in dB and % (" + juce::String (w.thdDb, 2) + " dB, " + juce::String (w.thdPct, 3) + " %)");
        check (w.harmonicDb.at (2) == -50.0 && w.harmonicDb.at (5) == -90.0, "sat S4: each harmonic is read relative to the fundamental's own output bin");
        check (w.evenOddKnown && w.evenOddDb > 15.0 && w.character == "mixed" && L.readings[2].evenOddDb < -3.0, "sat S5: even/odd balance: the warm position is even-dominant (+), the driven one odd-dominant (-) (" + juce::String (w.evenOddDb, 1) + " / " + juce::String (L.readings[2].evenOddDb, 1) + ")");
        check (! L.readings[0].evenOddKnown && L.readings[0].character.isEmpty(), "sat S5b: at the floor on both sides the balance is unknown and the character empty");
    }
    check (L.onset01pctNorm && std::abs (*L.onset01pctNorm - 0.5f) < 1e-6 && L.onset1pctNorm && std::abs (*L.onset1pctNorm - 1.0f) < 1e-6 && L.onset1pctText == "10", "sat S6: onset: 0.1 % at the warm position, 1 % at the driven one, with the display");
    check (std::abs (L.maxThdDb - L.readings[2].thdDb) < 1e-9, "sat S7: max THD over the positions");
    // one-sided: odd harmonics only (CamelCrusher's shape) is "odd", never a -280 dB ratio
    const juce::String oddOnly = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t1\ttones\t1\n" + pos (0, 1.0, "10", -8.0, -300.0, -26.0, -300.0, -36.0);
    { const auto o = readingFor (parseHarmonics (oddOnly).positions[0], -12.0); check (o.character == "odd" && ! o.evenOddKnown, "sat S5c: harmonics on one side only give a character word, not a ratio"); }
    const auto c = judge (4, "Drive", { L });
    check (! c.inert && c.note.contains ("-12 dBFS: max THD") && c.note.contains ("1 % onset '10'") && c.thdAtTopByLevel.count (-12.0), "sat S8: a control that distorts is judged with its onset and its THD at the top position per level (" + c.note + ")");
    // inert: three positions, nothing moves
    const juce::String flat = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t3\ttones\t1\n" + pos (0, 0.0, "0", -12.0, -120.0, -120.0, -120.0, -120.0) + pos (1, 0.5, "5", -12.01, -120.0, -120.0, -120.0, -120.0) + pos (2, 1.0, "10", -12.0, -120.0, -120.0, -120.0, -120.0);
    const auto ci = judge (4, "Drive", { deriveLevel (parseHarmonics (flat), -20.0), deriveLevel (parseHarmonics (flat), -12.0) });
    check (ci.inert && ci.note.startsWith ("inert:"), "sat S9: fundamental within 0.05 dB and no harmonic above -90 dB at every position and level = inert, said as the licence shape");
    const juce::String gainOnly = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t2\ttones\t1\n" + pos (0, 0.0, "0", -12.0, -120.0, -120.0, -120.0, -120.0) + pos (1, 1.0, "10", -9.0, -120.0, -120.0, -120.0, -120.0);
    check (! judge (4, "Drive", { deriveLevel (parseHarmonics (gainOnly), -12.0) }).inert, "sat S10: a control that only changes level is not inert (it did something)");
    const juce::String harmOnly = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t2\ttones\t1\n" + pos (0, 0.0, "0", -12.0, -120.0, -120.0, -120.0, -120.0) + pos (1, 1.0, "10", -12.0, -35.0, -40.0, -120.0, -120.0);
    check (! judge (4, "Drive", { deriveLevel (parseHarmonics (harmOnly), -12.0) }).inert, "sat S10b: a control that adds harmonics at unity gain (MSaturator's shape) is not inert");
    // no effect: the product distorts (THD -37 at every position) but this control moves neither level nor THD (MSaturator's Harmonics - Gain)
    const juce::String noEff = "response\tproto\t1\tctl\t8\tname\tHarmonics - Gain\tpositions\t3\ttones\t1\n" + pos (0, 0.0, "-24", -12.0, -40.0, -50.0, -60.0, -70.0) + pos (1, 0.5, "0", -12.0, -40.0, -50.1, -60.0, -70.0) + pos (2, 1.0, "+24", -12.02, -40.0, -50.0, -60.1, -70.0);
    { const auto n = judge (8, "Harmonics - Gain", { deriveLevel (parseHarmonics (noEff), -12.0) }); check (n.noEffect && ! n.inert && n.note.startsWith ("no effect:"), "sat S15: a control that changes neither level nor THD on a product that distorts is 'no effect', not inert and not a drive law"); }
    // silent: landed, the input carries the tone, the output holds nothing (bx_yellowdrive on this Mac: -600 dB)
    const juce::String silent = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t2\ttones\t1\n" + pos (0, 0.0, "0", -600.0, 0.0, 0.0, 0.0, 0.0) + pos (1, 1.0, "10", -600.0, 0.0, 0.0, 0.0, 0.0);
    const auto cs = judge (4, "Drive", { deriveLevel (parseHarmonics (silent), -12.0) });
    check (cs.silent && ! cs.inert && cs.note.startsWith ("silent:") && ! deriveLevel (parseHarmonics (silent), -12.0).readings[0].valid, "sat S14: no output at any landed position is silent, a shape of its own, never a reading");
    // an unlanded position is not a reading; a refused trace is not ok
    const juce::String unl = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t1\ttones\t1\nrpos\t0\tnorm\t0.500000\tconfirm_ms\t-1.0\tlanded_by\tnone\ttext\t5\nrdone\t0\ttones\t0\tunlanded\n";
    check (! deriveLevel (parseHarmonics (unl), -12.0).readings[0].valid && judge (4, "Drive", { deriveLevel (parseHarmonics (unl), -12.0) }).note.startsWith ("no position read"), "sat S11: an unlanded position is invalid; nothing read is said");
    {   // the sideband figure from an rtotal line (ruling 1): a -12 dB fundamental (RMS -15.01) with harmonics at -40 rel and a total of -14.54 dB RMS
        const juce::String withTotal = "response\tproto\t1\tctl\t4\tname\tDrive\tpositions\t1\ttones\t1\n" + pos (0, 1.0, "10", -12.0, -40.0, -46.0, -52.0, -58.0) + "rtotal\t0\tin_rms_db\t-15.010\tout_rms_db\t-14.540\n";
        const auto rd = readingFor (parseHarmonics (withTotal).positions[0], -12.0);
        // total power 10^-1.454 = 0.03516; fundamental 10^-1.501 = 0.03155; harmonics ~0.000011; sideband = 0.00360 -> -9.4 dB relative to the fundamental
        check (rd.sidebandDb && std::abs (*rd.sidebandDb + 9.4) < 0.3, "sat S16: the energy beside the fundamental and its harmonics, relative to the fundamental, from the total (" + juce::String (rd.sidebandDb ? *rd.sidebandDb : -999.0, 2) + ")");
        check (! readingFor (parseHarmonics (pos (0, 1.0, "10", -12.0, -40.0, -46.0, -52.0, -58.0)).positions[0], -12.0).sidebandDb, "sat S17: no rtotal line (an older probe) -> no sideband figure, never a guess");
    }
    check (! parseHarmonics ("refused harmonics= needs tones=1 and 2..20\n").ok && parseHarmonics ("refused harmonics= needs tones=1 and 2..20\n").refused.startsWith ("harmonics="), "sat S12: a refusal line is carried");
    const auto v = toVar (c);
    check (v.getProperty ("control", "").toString() == "Drive" && v.getProperty ("levels", {}).size() == 1 && v.getProperty ("levels", {})[0].getProperty ("curve", {}).size() == 3 && (double) v.getProperty ("levels", {})[0].getProperty ("curve", {})[2].getProperty ("harmonics_db", {}).getProperty ("h3", 0.0) == -26.0 && v.getProperty ("levels", {})[0].getProperty ("onset_1pct", {}).getProperty ("display", "").toString() == "10",
           "sat S13: the JSON carries the curve per level with the harmonics and the onset");
}

/** REVERB AND DELAY (EjmapReverbDelay.h, roadmap 2.7 PROTOTYPE, 5 Oct R4): synthetic tails - a delay with repeats, a reverb with RT60 2 s - and the mix law. */
void testReverbDelay()
{
    using namespace ejmap::reverbdelay;
    // a synthetic --tail trace: burst 200 ms at -12 dBFS (RMS -15.01), 1 ms windows, tail 3 s; dryDb = the dry's level in the burst, wet = f(t) in the tail
    auto trace = [] (double dryDb, std::function<double (double)> wetDbAt, double burstMs = 200.0, double tailMs = 3000.0, double tempo = 0.0)
    {
        juce::String s; s << "tail\tproto\t1\tdb\t-12.00\tburst_ms\t" << juce::String (burstMs, 2) << "\ttail_s\t" << juce::String (tailMs / 1000.0, 3) << "\thz\t997.000\twin_ms\t1.00\ttempo\t" << juce::String (tempo, 2) << "\nconfig\tmain_in\t2\tmain_out\t2\tlatency\t0\n";
        auto db2p = [] (double db) { return db > -500.0 ? std::pow (10.0, db / 10.0) : 0.0; };
        for (double t = 0.5; t < burstMs + tailMs; t += 1.0)
        {
            const bool burst = t < burstMs;
            const double wet = wetDbAt (t);
            const double out = 10.0 * std::log10 (db2p (burst ? dryDb : -999.0) + db2p (wet) + 1e-14);   // -140 dB floor
            s << "twin\tt_ms\t" << juce::String (t, 3) << "\tseg\t" << (burst ? "burst" : "tail") << "\tin_db\t" << (burst ? "-15.010" : "-999.000") << "\tout_db\t" << juce::String (out, 3) << "\tout_peak_db\t" << juce::String (out + 3.01, 3) << "\n";
        }
        s << "tdone\twindows\t" << (int) (burstMs + tailMs) << "\tnonfinite\t0\n";
        return s;
    };
    // A DELAY: 300 ms, feedback 50 % (-6.02 dB per repeat), wet level -21 dB at 100 % (the first repeat carries the burst's RMS -15 - 6)
    auto delayWet = [] (double delayMs, double fallDb, double firstDb) { return [=] (double t) { if (t < delayMs) return -999.0; const int k = (int) std::floor (t / delayMs); const double within = t - k * delayMs; if (within >= 200.0) return -999.0; return firstDb + (k - 1) * fallDb; }; };
    const auto dl = parseTail (trace (-999.0, delayWet (300.0, -6.02, -21.01)));
    check (dl.ok && dl.windows.size() == 3200 && std::abs (dl.burstMs - 200.0) < 1e-9, "rd D1: the tail trace parses (3200 one-ms windows)");
    const auto on = onsetsOf (dl);
    check (on.onsetMs && std::abs (*on.onsetMs - 300.5) < 1.01, "rd D2: a wet-only delay's onset is the delay time from the burst's start (" + juce::String (on.onsetMs ? *on.onsetMs : -1.0, 1) + ")");
    check (on.repeats.size() >= 5 && on.spacingMs && std::abs (*on.spacingMs - 300.0) < 1.5 && on.fallPerRepeatDb && std::abs (*on.fallPerRepeatDb + 6.02) < 0.1, "rd D3: repeats 300 ms apart, falling 6.0 dB each (" + juce::String (on.spacingMs ? *on.spacingMs : -1.0, 1) + " ms, " + juce::String (on.fallPerRepeatDb ? *on.fallPerRepeatDb : 0.0, 2) + " dB, " + juce::String ((int) on.repeats.size()) + " repeats)");
    // FLAT-TOPPED REPEATS WITH RIPPLE (H-Delay, 5 Oct): a 50 ms burst echoed every 375 ms, +-0.4 dB ripple on each block - one repeat per block, not one per ripple
    {
        auto rippled = [] (double t) { if (t < 375.0) return -999.0; const int k = (int) std::floor (t / 375.0); const double within = t - k * 375.0; if (within >= 50.0) return -999.0; return -17.5 - 5.0 * (k - 1) + 0.4 * std::sin (within * 1.3); };
        const auto rp = onsetsOf (parseTail (trace (-999.0, rippled, 50.0, 3000.0)));
        check (rp.repeats.size() == 8 && rp.spacingMs && std::abs (*rp.spacingMs - 375.0) < 1.5 && rp.fallPerRepeatDb && std::abs (*rp.fallPerRepeatDb + 5.0) < 0.3, "rd D3b: flat-topped rippled echoes count once each: 8 repeats 375 ms apart falling 5 dB (" + juce::String ((int) rp.repeats.size()) + ", " + juce::String (rp.spacingMs ? *rp.spacingMs : -1.0, 1) + ", " + juce::String (rp.fallPerRepeatDb ? *rp.fallPerRepeatDb : 0.0, 2) + ")");
    }
    // A REVERB: wet starts at -30 dB at the burst's end and falls 30 dB/s (RT60 = 2.0 s); dry -15 dB
    auto reverbWet = [] (double startDb, double dbPerS, double burstMs) { return [=] (double t) { if (t < 5.0) return -999.0; if (t < burstMs) return startDb; return startDb - dbPerS * (t - burstMs) / 1000.0; }; };
    const auto rv = parseTail (trace (-15.01, reverbWet (-30.0, 30.0, 200.0)));
    const auto dc = decayOf (rv);
    check (dc.ok && std::abs (dc.t20RT60s - 2.0) < 0.05 && dc.t30 && std::abs (dc.t30RT60s - 2.0) < 0.05, "rd D4: RT60 from the T20 and T30 fits of a 30 dB/s tail = 2.0 s (" + juce::String (dc.t20RT60s, 3) + " / " + juce::String (dc.t30RT60s, 3) + ")");
    const auto lv = levelsOf (rv);
    check (lv.ok && std::abs (lv.dryDb + 15.01) < 0.05 && std::abs (lv.wetDb + 30.0) < 0.1, "rd D5: dry read in the burst's first 2 ms (before any wet), wet read just after the burst ends (" + juce::String (lv.dryDb, 2) + " / " + juce::String (lv.wetDb, 2) + ")");
    check (! decayOf (parseTail (trace (-15.01, [] (double) { return -999.0; }))).ok, "rd D6: a tail at the floor has no decay to read");
    // a tail longer than the window (RT60 12 s = 5 dB/s over a 3 s tail): fitted over what fell, said as such
    { const auto lt = decayOf (parseTail (trace (-15.01, reverbWet (-30.0, 5.0, 200.0)))); check (lt.ok && lt.t10Only && std::abs (lt.t20RT60s - 12.0) < 0.6 && lt.why.contains ("longer than the window"), "rd D7: a tail that falls only 15 dB in the window is fitted over that fall and said to be longer than the window (" + juce::String (lt.t20RT60s, 2) + ")"); }
    // the mix law: eleven positions, linear and equal power
    {
        std::vector<MixPoint> lin, eqp, odd;
        for (int k = 0; k <= 10; ++k)
        {
            const double x = k / 10.0; MixPoint p; p.norm = (float) x; p.text = juce::String (k * 10) + " %";
            p.dryDb = x < 1.0 ? -15.0 + 20.0 * std::log10 (1.0 - x) : -999.0; p.wetDb = x > 0.0 ? -30.0 + 20.0 * std::log10 (x) : -999.0; lin.push_back (p);
            MixPoint q = p; q.dryDb = x < 1.0 ? -15.0 + 20.0 * std::log10 (std::cos (x * juce::MathConstants<double>::halfPi)) : -999.0; q.wetDb = x > 0.0 ? -30.0 + 20.0 * std::log10 (std::sin (x * juce::MathConstants<double>::halfPi)) : -999.0; eqp.push_back (q);
            MixPoint o = p; o.dryDb = -15.0; o.wetDb = x > 0.0 ? -30.0 + 20.0 * std::log10 (x) : -999.0; odd.push_back (o);   // dry never falls: a "wet send" law
        }
        check (mixLaw (lin).law == "linear" && mixLaw (lin).worstLinearDb < 0.01, "rd M1: a linear mix is read as linear");
        check (mixLaw (eqp).law == "equal_power" && mixLaw (eqp).worstEqualPowerDb < 0.01 && mixLaw (eqp).worstLinearDb > 1.0, "rd M2: an equal-power mix is read as equal power (linear misses by " + juce::String (mixLaw (eqp).worstLinearDb, 2) + " dB)");
        check (mixLaw (odd).law == "other", "rd M3: a dry that never falls is neither law: other");
        check (mixLaw ({ lin[0], lin[10] }).law == "unknown", "rd M4: fewer than three positions decide nothing");
    }
    // labels
    check (labelMs ("120 ms") && *labelMs ("120 ms") == 120.0 && labelMs ("1.5 s") && *labelMs ("1.5 s") == 1500.0 && labelSeconds ("2.30s") && std::abs (*labelSeconds ("2.30s") - 2.3) < 1e-9 && labelSeconds ("450 ms") && std::abs (*labelSeconds ("450 ms") - 0.45) < 1e-9 && ! labelMs ("Off"),
           "rd L1: ms and s labels parse to ms and to s");
    check (noteBeats ("1/4") && *noteBeats ("1/4") == 1.0 && noteBeats ("1/8") && *noteBeats ("1/8") == 0.5 && noteBeats ("1/8 D") && std::abs (*noteBeats ("1/8 D") - 0.75) < 1e-9 && noteBeats ("1/8 T") && std::abs (*noteBeats ("1/8 T") - 1.0 / 3.0) < 1e-9 && ! noteBeats ("120 ms"),
           "rd L2: note values to beats (dotted x1.5, triplet x2/3)");
    check (tailForLabel (std::nullopt) == 6.0 && tailForLabel (2.0) == 6.0 && std::abs (tailForLabel (8.0) - 12.0) < 1e-9 && tailForLabel (40.0) == 30.0, "rd L4: the tail is 1.5 x the decay label, 6 s at least, 30 s at most (Valhalla at 8 s gets 12 s)");
    check (std::abs (expectedSyncMs (1.0, 120.0) - 500.0) < 1e-9 && std::abs (expectedSyncMs (1.0, 90.0) - 666.667) < 0.01 && std::abs (expectedSyncMs (0.5, 140.0) - 214.286) < 0.01, "rd L3: a quarter at 120 = 500 ms, at 90 = 666.7 ms; an eighth at 140 = 214.3 ms");
}

/** TRANSIENT SHAPERS AND GATES (EjmapDynamics.h, roadmap 2.8 PROTOTYPE, 5 Oct R5): synthetic hits, a ramp through a gate, a burst through a gate. */
void testDynamics()
{
    using namespace ejmap::dynamics;
    // HITS: 4 hits every 600 ms, input peak -6 dB decaying 150 ms; the unit boosts the transient by +4 dB (first 10 ms) and cuts the sustain by -3 dB
    auto hitsTrace = [] (double trBoost, double suGain, int hits = 4)
    {
        juce::String s; s << "hits\tproto\t1\tdb\t-6.00\thz\t997.000\tdecay_ms\t150.00\tperiod_ms\t600.00\thits\t" << hits << "\twin_ms\t1.00\n";
        for (double t = 0.5; t < 600.0 * hits; t += 1.0)
        {
            const int k = (int) (t / 600.0); const double rel = t - k * 600.0;
            const double env = -6.0 + 20.0 * std::log10 (std::exp (-rel / 150.0));   // the peak envelope in dB
            const double inPk = env, inRms = env - 3.01;
            const double g = rel < 10.0 ? trBoost : suGain;
            s << "hwin\tt_ms\t" << juce::String (t, 3) << "\thit\t" << k << "\tin_db\t" << juce::String (inRms, 3) << "\tout_db\t" << juce::String (inRms + g, 3) << "\tin_peak_db\t" << juce::String (inPk, 3) << "\tout_peak_db\t" << juce::String (inPk + g, 3) << "\n";
        }
        s << "hdone\twindows\t" << 600 * hits << "\tnonfinite\t0\n"; return s;
    };
    const auto neutral = hitFigures (parseHits (hitsTrace (0.0, 0.0)));
    const auto boosted = hitFigures (parseHits (hitsTrace (4.0, -3.0)));
    check (neutral.ok && std::abs (neutral.transientDb) < 1e-6 && std::abs (neutral.sustainDb) < 1e-6 && neutral.hitsUsed == 3, "dyn H1: a transparent unit reads 0 / 0; the first hit is left out (3 of 4 used)");
    check (boosted.ok && std::abs (boosted.transientDb - 4.0) < 1e-6 && std::abs (boosted.sustainDb + 3.0) < 1e-6, "dyn H2: transient from the first 10 ms peaks, sustain from the 80-250 ms body (" + juce::String (boosted.transientDb, 2) + " / " + juce::String (boosted.sustainDb, 2) + ")");
    const auto pt = transientPoint (1.0f, "+6.0 dB", boosted, neutral);
    check (pt.ok && std::abs (pt.dTransientDb - 4.0) < 1e-6 && std::abs (pt.dSustainDb + 3.0) < 1e-6 && pt.labelDb && *pt.labelDb == 6.0, "dyn H3: a position's effect is its figures minus the neutral run's; a dB label is read");
    check (! transientPoint (1.0f, "50 %", boosted, neutral).labelDb, "dyn H4: a % label is not a dB expectation");
    check (! hitFigures (parseHits ("refused no main input or output bus\n")).ok, "dyn H5: a refusal is carried");
    // RAMP through a gate: threshold at -30 dBFS RMS opening, closes at -36 (6 dB hysteresis), range -40 dB closed; up 3 s / down 3 s from -60 to -6
    auto rampTrace = [] (double openAt, double closeAt, double rangeDb)
    {
        juce::String s; s << "ramp\tproto\t1\tfrom\t-60.00\tto\t-6.00\tup_s\t3.000\tdown_s\t3.000\thz\t997.000\twin_ms\t5.00\n";
        bool open = false;
        for (double t = 2.5; t < 6000.0; t += 5.0)
        {
            const bool up = t < 3000.0; const double in = up ? -60.0 + 54.0 * t / 3000.0 : -6.0 - 54.0 * (t - 3000.0) / 3000.0;
            if (up && in >= openAt) open = true; if (! up && in <= closeAt) open = false;
            s << "rwin\tt_ms\t" << juce::String (t, 3) << "\tseg\t" << (up ? "up" : "down") << "\tin_db\t" << juce::String (in - 3.01, 3) << "\tout_db\t" << juce::String (in - 3.01 + (open ? 0.0 : rangeDb), 3) << "\n";
        }
        s << "rdone\twindows\t1200\tnonfinite\t0\n"; return s;
    };
    const auto g = gateLevels (parseRamp (rampTrace (-30.0, -36.0, -40.0)));
    check (g.ok && g.gating && std::abs (g.rangeDb - 40.0) < 0.01 && g.openAtInDb && std::abs (*g.openAtInDb - (-30.0 - 3.01)) < 0.3 && g.closeAtInDb && std::abs (*g.closeAtInDb - (-36.0 - 3.01)) < 0.3 && g.hysteresisDb && std::abs (*g.hysteresisDb - 6.0) < 0.5,
           "dyn G1: range 40 dB, opens at -33 dBFS RMS (-30 peak), closes at -39, hysteresis 6 dB (" + g.why + ")");
    const auto ng = gateLevels (parseRamp (rampTrace (-30.0, -36.0, -1.0)));
    check (ng.ok && ! ng.gating && ng.why.contains ("not gating"), "dyn G2: a unit that attenuates 1 dB when closed is not gating, said");
    // BURST through a gate: closed -40 dB; opens over 5 ms (90 % at 4.5 ms), holds 100 ms, releases over 200 ms (90 % at 180 ms)
    {
        juce::String s; s << "burst\tproto\t1\tquiet\t-50.00\tloud\t-10.00\tpre_s\t1.000\thold_s\t1.000\tpost_s\t2.000\thz\t997.000\twin_ms\t1.00\nconfig\tmain_in\t2\tmain_out\t2\tlatency\t0\n";
        for (double t = 0.5; t < 4000.0; t += 1.0)
        {
            const char* seg = t < 1000.0 ? "pre" : t < 2000.0 ? "loud" : "post"; const double in = t < 1000.0 || t >= 2000.0 ? -53.01 : -13.01;
            double gain;
            if (t < 1000.0) gain = -40.0;
            else if (t < 2000.0) gain = -40.0 + 40.0 * juce::jmin (1.0, (t - 1000.0) / 5.0);
            else if (t < 2100.0) gain = 0.0;
            else gain = -40.0 * juce::jmin (1.0, (t - 2100.0) / 200.0);
            s << "bwin\tt_ms\t" << juce::String (t, 2) << "\tseg\t" << seg << "\tin_db\t" << juce::String (in, 3) << "\tout_db\t" << juce::String (in + gain, 3) << "\n";
        }
        const auto gt = gateTiming (ejmap::timing::parseBurst (s));
        // attack to within 1 dB of open = 39/40 of the 5 ms rise = 4.875; hold ends at the first 1 dB of fall (5 ms into the 200 ms release) = 105;
        // release = from there to 20 dB below open (100 ms into the release) = 95: the definitions, said
        check (gt.ok && gt.attackMs && std::abs (*gt.attackMs - 4.875) < 0.5 && gt.holdMs && std::abs (*gt.holdMs - 105.0) < 0.5 && gt.releaseMs && std::abs (*gt.releaseMs - 95.0) < 0.5,
               "dyn G3: attack 4.9 ms (within 1 dB of open), hold 105 ms (to the first 1 dB of fall), release 95 ms (from there 20 dB down), interpolated (" + gt.why + ")");
        // a closed gate is silent (-600 dB windows): read as -150 relative, never skipped - the ramp still finds its crossings
        auto silentRamp = [] { juce::String s; s << "ramp\tproto\t1\tfrom\t-60.00\tto\t-6.00\tup_s\t3.000\tdown_s\t3.000\thz\t997.000\twin_ms\t5.00\n"; bool open = false;
            for (double t = 2.5; t < 6000.0; t += 5.0) { const bool up = t < 3000.0; const double in = up ? -60.0 + 54.0 * t / 3000.0 : -6.0 - 54.0 * (t - 3000.0) / 3000.0; if (up && in >= -30.0) open = true; if (! up && in <= -36.0) open = false;
                s << "rwin\tt_ms\t" << juce::String (t, 3) << "\tseg\t" << (up ? "up" : "down") << "\tin_db\t" << juce::String (in - 3.01, 3) << "\tout_db\t" << (open ? juce::String (in - 3.01, 3) : juce::String ("-600.000")) << "\n"; } return s; };
        const auto sg = gateLevels (parseRamp (silentRamp()));
        check (sg.ok && sg.gating && sg.rangeDb > 80.0 && sg.openAtInDb && std::abs (*sg.openAtInDb + 33.01) < 0.3 && sg.closeAtInDb && std::abs (*sg.closeAtInDb + 39.01) < 0.3, "dyn G4: a gate that closes to digital silence reads a floor range (-150 dBFS against the quiet input) and still gives its open and close levels (" + sg.why + ")");
    }
    check (labelMs ("12.5 ms") && *labelMs ("12.5 ms") == 12.5 && labelMs ("1.20 s") && std::abs (*labelMs ("1.20 s") - 1200.0) < 1e-9 && labelNumber ("-30.0 dB") && *labelNumber ("-30.0 dB") == -30.0 && ! labelNumber ("Off"), "dyn L1: labels");
}

/** DE-ESSERS (EjmapDeesser.h, roadmap 2.9 PROTOTYPE, 5 Oct R6): the ladder against the open end, the centre and the mode from a deviation. */
void testDeesser()
{
    using namespace ejmap::deesser;
    // a ladder: three threshold positions x two levels; the open end (norm 0) at unity gain; GR grows with norm and with level
    ejmap::sweep::Measured m;
    for (int k = 0; k < 3; ++k)
    {
        ejmap::sweep::PositionReading p; p.k = k; p.norm = k / 2.0f; p.text = juce::String (-10 * k) + " dB";
        for (double L : { -18.0, -6.0 }) { ejmap::sweep::HoldReading h; h.present = true; h.inRmsDb = L - 3.01; h.levelDb = h.inRmsDb - k * (L == -6.0 ? 4.0 : 2.0); p.holds[ejmap::sweep::levelKey (L)] = h; }
        m.positions.push_back (p);
    }
    const auto L = ladderOf (m, 6500.0);
    check (L.ok && L.openIndex >= 0 && L.cells[(size_t) L.openIndex].norm == 0.0f && std::abs (L.maxGrDb - 8.0) < 1e-9, "ds L1: the open end is the position with the most gain at the loudest level; max GR 8 dB");
    { double gr = -1.0; for (const auto& c : L.cells) if (c.norm == 1.0f && std::abs (c.levelDbfs + 18.0) < 0.01) gr = c.grDb; check (std::abs (gr - 4.0) < 1e-9, "ds L2: GR at a position and level = open gain minus its gain (4 dB at norm 1, -18)"); }
    check (hardestNorm (L, -6.0) && *hardestNorm (L, -6.0) == 1.0f && ! hardestNorm (L, -30.0), "ds L3: the hardest norm at a level; none at a level not swept");
    // a deviation: a 6 kHz notch of -8 dB on the 1/12-octave grid, flat at 997 Hz -> split band, centre near 6 kHz
    std::vector<std::pair<double, double>> split, wide;
    for (int k = 0; k < 121; ++k) { const double f = 20.0 * std::pow (1000.0, k / 120.0); const double x = std::log2 (f / 6000.0) / 0.5; const double notch = -8.0 * std::pow (2.0, -x * x); split.push_back ({ f, notch }); wide.push_back ({ f, -8.0 + notch * 0.1 }); }
    const auto cs = centreOf (split);
    check (cs.ok && cs.shape == "notch" && std::abs (cs.hz - 6000.0) < 150.0 && std::abs (cs.figureHz() - 6000.0) < 150.0 && std::abs (cs.depthDb + 8.0) < 0.05 && std::abs (cs.at997Db) < 0.3 && modeWord (cs) == "split_band", "ds C1: a notch: the centre is the deepest deviation refined to within 2.5 % (" + juce::String (cs.hz, 0) + "), 997 Hz untouched -> split band");
    // a shelf-shaped reduction (a high-pass detector: the cut holds to the top): the figure is the half-depth corner, not the deepest point
    std::vector<std::pair<double, double>> shelf; for (int k = 0; k < 121; ++k) { const double f = 20.0 * std::pow (1000.0, k / 120.0); shelf.push_back ({ f, -8.0 / (1.0 + std::pow (5000.0 / f, 2.0)) }); }
    const auto sh = centreOf (shelf);
    check (sh.ok && sh.shape == "shelf" && sh.hz > 15000.0 && std::abs (sh.cornerHz - 5000.0) < 300.0 && std::abs (sh.figureHz() - 5000.0) < 300.0, "ds C1b: a shelf: the deepest point sits at the top of the grid, the figure is the half-depth corner (" + juce::String (sh.cornerHz, 0) + " for 5 kHz)");
    const auto cw = centreOf (wide);
    check (cw.ok && modeWord (cw) == "wideband", "ds C2: the whole band cut within 1.5 dB of the deepest point -> wideband (997 at " + juce::String (cw.at997Db, 2) + ")");
    std::vector<std::pair<double, double>> partial = split; for (auto& d : partial) if (d.first < 2000.0) d.second = -4.0;
    check (modeWord (centreOf (partial)) == "partial", "ds C3: 997 Hz cut 4 dB under an 8 dB band is partial");
    std::vector<std::pair<double, double>> flat; for (int k = 0; k < 121; ++k) flat.push_back ({ 20.0 * std::pow (1000.0, k / 120.0), -0.5 });
    check (! centreOf (flat).ok && modeWord (centreOf (flat)) == "unknown", "ds C4: nothing cut 3 dB -> no centre, mode unknown");
    check (labelHz ("6.50 kHz") && std::abs (*labelHz ("6.50 kHz") - 6500.0) < 1e-6 && labelHz ("7200 Hz") && *labelHz ("7200 Hz") == 7200.0 && labelHz ("5.2k") && *labelHz ("5.2k") == 5200.0 && ! labelHz ("Wide"), "ds H1: frequency labels");
}

/** MULTIBAND (EjmapMultiband.h, the proposal's prototype, 5 Oct R7): bands from crossovers, the dB offset to a norm, the whole-unit gain. */
void testMultiband()
{
    using namespace ejmap::multiband;
    juce::StringArray skipped;
    const auto b = bandsFromCrossovers ({ "4000", "92", "11071" }, skipped);   // C4's defaults, in any order
    check (b.size() == 4 && skipped.isEmpty() && std::abs (b[0].centreHz - std::sqrt (20.0 * 92.0)) < 0.01 && std::abs (b[1].centreHz - std::sqrt (92.0 * 4000.0)) < 0.01 && b[3].loHz == 11071.0 && b[3].hiHz == 20000.0,
           "mb B1: three crossovers give four bands 20..92..4000..11071..20000 with geometric centres (" + juce::String (b[1].centreHz, 0) + ")");
    const auto b2 = bandsFromCrossovers ({ "Off", "200 Hz", "5.0 kHz" }, skipped);
    check (b2.size() == 3 && skipped == juce::StringArray { "Off" } && std::abs (b2[1].loHz - 200.0) < 1e-9 && std::abs (b2[1].hiHz - 5000.0) < 1e-9, "mb B2: 'Off' is skipped and named; Hz / kHz labels parse");
    check (bandsFromCrossovers ({}, skipped).size() == 1, "mb B3: no crossovers = one band over the whole range (nothing assumed)");
    const auto r = dbRangeOf ("-60.0 dB", "0.0 dB");
    check (r.ok && normForDb (r, -30.0) == 0.5f && normForDb (r, -27.0) == 0.55f && normForDb (r, -70.0) == 0.0f && normForDb (r, 5.0) == 1.0f, "mb O1: a dB offset maps to a norm by the control's own ends, clamped");
    check (! dbRangeOf ("Off", "0.0 dB").ok && ! dbRangeOf ("-20", "-20").ok, "mb O2: a word end or equal ends give no range (no offset can be written)");
    ejmap::eq::Position p; for (double f : { 100.0, 1000.0, 10000.0 }) { ejmap::eq::Tone t; t.hz = f; t.inDb = -20.0; t.outDb = -26.0; p.tones.push_back (t); }
    const auto g = totalGainDb (p);
    check (g && std::abs (*g + 6.0) < 1e-9, "mb W1: the whole-unit gain is total output power over total input power across the tones (-6 dB)");
    ejmap::eq::Tone loud; loud.hz = 500.0; loud.inDb = -10.0; loud.outDb = -10.0; p.tones.push_back (loud);
    check (totalGainDb (p) && *totalGainDb (p) > -3.0, "mb W2: a loud untouched tone dominates the power sum (the figure follows where the energy is)");
    check (! totalGainDb (ejmap::eq::Position()), "mb W3: no tones, no figure");
}

/** NAMES PROPOSE, MEASUREMENT DECIDES (EjmapRoleEvidence.h, 5 Oct evening ruling): every case run 2 found where the name was wrong, as a pin. */
void testRoleEvidence()
{
    // MN (Kathy's 6 Oct ruling, the Phase B fallback): measurement nominates when the lexicon found nothing - the role's signature at the
    // two ends IS the nomination; modulation is never a drive; nothing holds, nothing nominated. The mutant that nominates nothing goes red.
    {
        using namespace ejmap::roleevidence;
        Figure ea, eb; ea.ok = eb.ok = true; ea.bandGainDb = 0.0; eb.bandGainDb = 6.0; ea.levelShiftDb = 0.0; eb.levelShiftDb = 0.1;
        const auto band = measurementNominates (24, "HP Freq 1", "eq_gain", ea, eb);
        check (band && band->verdict == "measured_unnamed" && band->role == "eq_gain" && band->index == 24 && band->reason.startsWith ("nominated by measurement"), "role MN1: a control whose two ends show a 6 dB band is nominated as an EQ band, unnamed");
        Figure fa, fb; fa.ok = fb.ok = true; fa.bandGainDb = 0.0; fb.bandGainDb = 0.3;
        check (! measurementNominates (3, "Output", "eq_gain", fa, fb), "role MN2: 0.3 dB between the ends nominates nothing");
        Figure la, lb; la.ok = lb.ok = true; la.bandGainDb = 0.0; lb.bandGainDb = 12.0; la.levelShiftDb = 0.0; lb.levelShiftDb = 12.0;
        check (! measurementNominates (2, "Input Gain", "eq_gain", la, lb), "role MN3: a level shift of the whole grid is not a band");
        Figure da, db; da.ok = db.ok = true; da.thdDb = -60.0; db.thdDb = -12.0; da.outputDb = db.outputDb = -20.0; da.sidebandDb = db.sidebandDb = -200.0;
        check (measurementNominates (5, "Volume", "drive", da, db) && measurementNominates (5, "Volume", "drive", da, db)->role == "drive", "role MN4: THD rising 48 dB between the ends nominates a drive (an amp sim's Volume)");
        Figure ma, mb; ma.ok = mb.ok = true; ma.thdDb = -60.0; mb.thdDb = -12.0; ma.outputDb = mb.outputDb = -20.0; ma.sidebandDb = -200.0; mb.sidebandDb = 20.0;
        check (! measurementNominates (7, "WOW Depth", "drive", ma, mb), "role MN5: energy beside the tone is modulation, never a drive nominee");
    }
    using namespace ejmap::roleevidence;
    auto fig = [] (std::function<void (Figure&)> fill) { Figure f; f.ok = true; fill (f); return f; };
    // drive: Saphira's "Warmth Band1 Gain" nominated by "warmth": THD -36.3 / -36.3 across its ends -> dropped; J37's Saturation -63.7 -> -10.6 -> confirmed
    check (! signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -36.3; }), fig ([] (Figure& f) { f.thdDb = -36.1; })).holds, "role D1: Saphira's band gain is not a drive (THD moves 0.2 dB)");
    check (signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -63.7; }), fig ([] (Figure& f) { f.thdDb = -10.6; })).holds, "role D2: J37's Saturation is a drive (THD -63.7 -> -10.6)");
    check (! signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -120.0; }), fig ([] (Figure& f) { f.thdDb = -95.0; })).holds && ! signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -63.7; }), fig ([] (Figure& f) { f.thdDb = -56.1; })).holds, "role D3: MSaturator's per-harmonic trim (THD at the floor) and J37's slap level (-63.7 -> -56.1, under 0.3 %) are not drives");
    // DRIVE NEEDS A STEADY TONE (ruling 1, 5 Oct evening, measured on J37 (s) with the probe's rtotal line): WOW Depth at full leaves the
    // fundamental's bin 31 dB down with +29.7 dB of energy beside it (relative) against -11.4 dB of "harmonics"; Saturation at full
    // keeps the sidebands at -26.6 dB under its -10.6 dB of harmonics
    {
        const auto wow0 = fig ([] (Figure& f) { f.thdDb = -63.8; f.sidebandDb = -200.0; f.outputDb = -11.53; }), wow1 = fig ([] (Figure& f) { f.thdDb = -11.4; f.sidebandDb = 29.7; f.outputDb = -42.56; });
        const auto sat0 = fig ([] (Figure& f) { f.thdDb = -63.7; f.sidebandDb = -35.2; f.outputDb = -11.53; }), sat1 = fig ([] (Figure& f) { f.thdDb = -10.6; f.sidebandDb = -26.6; f.outputDb = -12.45; });
        check (modulationOf (wow0, wow1).holds && ! signatureHolds ("drive", wow0, wow1).holds && signatureHolds ("drive", wow0, wow1).why.contains ("modulation, not drive"), "role D4: J37's WOW Depth smears energy beside the fundamental (+29.7 dB) - modulation, not drive");
        check (! modulationOf (sat0, sat1).holds && signatureHolds ("drive", sat0, sat1).holds, "role D5: J37's Saturation keeps its sidebands (-26.6) under its harmonics (-10.6): a steady tone with rising harmonics - drive");
        const auto hiss0 = fig ([] (Figure& f) { f.thdDb = -63.7; f.sidebandDb = -32.8; }), hiss1 = fig ([] (Figure& f) { f.thdDb = -62.8; f.sidebandDb = -32.8; });
        check (! modulationOf (hiss0, hiss1).holds && modulationOf (hiss0, hiss1).why.contains ("the unit's own floor"), "role D7: J37's Noise Level leaves the -32.8 dB tape hiss beside the tone where it was: the unit's floor, not a modulation control");
        check (modulationOf (fig ([] (Figure& f) { f.thdDb = -63.7; f.sidebandDb = -32.8; }), fig ([] (Figure& f) { f.thdDb = -56.0; f.sidebandDb = -6.5; })).holds, "role D8: J37's Flutter Depth raises the sidebands 26 dB over the hiss: modulation");
        check (unnamed (7, "WOW Depth", "modulation", modulationOf (wow0, wow1)).verdict == "measured_unnamed" && line (unnamed (7, "WOW Depth", "modulation", modulationOf (wow0, wow1))).startsWith ("measured role modulation, UNNAMED"), "role D6: the verdict word for it is 'modulation, unnamed'");
    }
    // mix: bx_delay2500's "Modulation Mix" - dry never present, wet moves: dropped; H-Delay's Mix dry -15.8 -> floor, wet floor -> -17.6: confirmed
    check (! signatureHolds ("mix", fig ([] (Figure& f) { f.wetDb = -40.0; }), fig ([] (Figure& f) { f.wetDb = -9.4; })).holds, "role M1: bx_delay2500's Modulation Mix: no dry reading -> not a mix");
    check (signatureHolds ("mix", fig ([] (Figure& f) { f.dryDb = -15.8; f.wetDb = -85.8; }), fig ([] (Figure& f) { f.dryDb = -85.7; f.wetDb = -17.6; })).holds, "role M2: H-Delay's Mix: dry falls while wet rises -> a mix");
    check (! signatureHolds ("mix", fig ([] (Figure& f) { f.dryDb = -15.0; f.wetDb = -30.0; }), fig ([] (Figure& f) { f.dryDb = -25.0; f.wetDb = -40.0; })).holds, "role M3: dry and wet falling together is a level, not a mix");
    check (signatureHolds ("mix", fig ([] (Figure& f) { f.dryDb = -15.0; }), fig ([] (Figure& f) { f.wetDb = -22.6; })).holds, "role M4: H-Reverb's Dry/Wet: no wet at the dry end and no dry at the wet end IS the mix shape (absent = floor)");
    check (! signatureHolds ("feedback", fig ([] (Figure& f) { f.repeats = 14; f.firstRepeatDb = -17.0; }), fig ([] (Figure& f) { f.repeats = 16; f.firstRepeatDb = -17.8; })).holds, "role F4: 14 -> 16 repeats with the first put (bx_delay2500's Wah Amount) is a floor effect, not feedback (needs 3)");
    // time: H-Reverb's "Predelay Free" 12.5 -> 12.5 ms: dropped; Abbey Road Plates' Predelay 0.5 -> 500.5: confirmed; MannyM's "Delay Left ms" 500 -> 500: dropped
    check (! signatureHolds ("time", fig ([] (Figure& f) { f.onsetMs = 12.5; }), fig ([] (Figure& f) { f.onsetMs = 12.5; })).holds, "role T1: H-Reverb's Predelay Free moved nothing -> dropped");
    check (signatureHolds ("time", fig ([] (Figure& f) { f.onsetMs = 0.5; }), fig ([] (Figure& f) { f.onsetMs = 500.5; })).holds, "role T2: Abbey Road Plates' Predelay 0.5 -> 500.5 ms -> confirmed");
    check (! signatureHolds ("time", fig ([] (Figure& f) { f.onsetMs = 500.0; }), fig ([] (Figure& f) { f.onsetMs = 500.0; })).holds, "role T3: MannyM's Delay Left ms under its sync'd note -> dropped");
    check (! signatureHolds ("time", fig ([] (Figure& f) { f.onsetMs = 2475.0; }), fig ([] (Figure& f) { f.onsetMs = 2490.0; })).holds, "role T4: an onset that moves 15 ms on 2.5 s (0.6 %) is a modulation or a filter's edge, not a time control (needs 20 %)");
    // TIME IS THE ONSET (ruling 2, 5 Oct evening), pinned on bx_delay2500's Feedback Hi Pass [30]: the first repeat's onset 191.5 ms at both ends
    // (the spacing read 192 -> 142 because a hi-pass rings inside each repeat and the edge detector counted the ringing); its first repeat
    // -4.29 -> -15.90 dB - tone in the feedback path, never time; Feedback Low Pass [29]: onset 191.5 both ends, first repeat -22.92 -> -4.29 - tone too
    {
        const auto hp0 = fig ([] (Figure& f) { f.onsetMs = 191.5; f.firstRepeatDb = -4.29; f.fallPerRepeatDb = -4.5; f.repeats = 14; }), hp1 = fig ([] (Figure& f) { f.onsetMs = 191.5; f.firstRepeatDb = -15.90; f.fallPerRepeatDb = -5.8; f.repeats = 16; });
        check (! signatureHolds ("time", hp0, hp1).holds && signatureHolds ("tone", hp0, hp1).holds && signatureHolds ("tone", hp0, hp1).why.contains ("tone in the feedback path, not time"), "role T5: Feedback Hi Pass: the onset holds at 191.5 ms while the first repeat moves 11.6 dB - tone, not time");
        const auto lp0 = fig ([] (Figure& f) { f.onsetMs = 191.5; f.firstRepeatDb = -22.92; f.fallPerRepeatDb = -17.17; }), lp1 = fig ([] (Figure& f) { f.onsetMs = 191.5; f.firstRepeatDb = -4.29; f.fallPerRepeatDb = -4.50; });
        check (! signatureHolds ("feedback", lp0, lp1).holds && signatureHolds ("feedback", lp0, lp1).why.contains ("first repeat moves") && signatureHolds ("tone", lp0, lp1).holds, "role T6: Feedback Low Pass moves the fall per repeat 12.7 dB but the first repeat 18.6 dB too: the path's tone, not feedback");
        const auto fb0 = fig ([] (Figure& f) { f.onsetMs = 191.5; f.firstRepeatDb = -4.3; f.fallPerRepeatDb = -10.45; }), fb1 = fig ([] (Figure& f) { f.onsetMs = 191.5; f.firstRepeatDb = -4.3; f.fallPerRepeatDb = 0.01; });
        check (signatureHolds ("feedback", fb0, fb1).holds, "role T7: Feedback L: the fall moves 10 dB with the first repeat put - feedback (the mode reports tone only where neither time nor feedback holds)");
        check (signatureHolds ("time", fig ([] (Figure& f) { f.onsetMs = 14.5; }), fig ([] (Figure& f) { f.onsetMs = 2475.0; })).holds, "role T8: Time L: the onset itself moves - time");
    }
    // decay: H-Reverb's "Buildup Time" moved RT60 3.67 -> 8.72: the measurement says it IS a decay-affecting control (confirmed, whatever the name); Valhalla 1.22 -> 9.02 confirmed
    check (signatureHolds ("decay", fig ([] (Figure& f) { f.rt60s = 3.67; }), fig ([] (Figure& f) { f.rt60s = 8.72; })).holds, "role C1: H-Reverb's Buildup Time moves RT60 x2.4 -> confirmed as a decay control by measurement");
    check (! signatureHolds ("decay", fig ([] (Figure& f) { f.rt60s = 2.5; }), fig ([] (Figure& f) { f.rt60s = 2.8; })).holds, "role C2: RT60 2.5 -> 2.8 (x1.12) is not a decay control");
    // feedback: bx_delay2500 -10.45 -> 0.01 dB per repeat: confirmed; CLA EchoSphere SlapFbK 0 -> 100: 1 -> 25 repeats: confirmed by count
    check (signatureHolds ("feedback", fig ([] (Figure& f) { f.fallPerRepeatDb = -10.45; f.repeats = 6; }), fig ([] (Figure& f) { f.fallPerRepeatDb = 0.01; f.repeats = 32; })).holds, "role F1: feedback by the fall per repeat");
    check (signatureHolds ("feedback", fig ([] (Figure& f) { f.repeats = 1; f.firstRepeatDb = -17.5; }), fig ([] (Figure& f) { f.repeats = 25; f.fallPerRepeatDb = 0.0; f.firstRepeatDb = -17.6; })).holds && ! signatureHolds ("feedback", fig ([] (Figure& f) { f.repeats = 3; f.firstRepeatDb = -17.0; }), fig ([] (Figure& f) { f.repeats = 5; f.firstRepeatDb = -17.0; })).holds, "role F2: feedback by the repeat count when the first repeat stays put; 3 -> 5 repeats is nothing");
    check (! signatureHolds ("feedback", fig ([] (Figure& f) { f.repeats = 0; }), fig ([] (Figure& f) { f.repeats = 16; f.firstRepeatDb = -20.0; })).holds && ! signatureHolds ("feedback", fig ([] (Figure& f) { f.repeats = 9; f.firstRepeatDb = -30.0; }), fig ([] (Figure& f) { f.repeats = 11; f.firstRepeatDb = -12.0; })).holds, "role F3: bx_delay2500's Gain In (0 -> 16 repeats from silence) and H-Delay's Output (every repeat 18 dB louder) are levels, not feedback");
    // transient / sustain: TransX's "Range" (unnamed) moving the transient by 10 dB -> measured role, unnamed; Quantum's "Attack - Vibrato - Mix" 0.00 -> dropped; Smack Attack confirmed
    check (unnamed (5, "Range", "transient", signatureHolds ("transient", fig ([] (Figure& f) { f.transientDb = 0.0; }), fig ([] (Figure& f) { f.transientDb = 10.0; }))).verdict == "measured_unnamed", "role S1: TransX's Range shows the transient signature -> measured role, unnamed");
    check (nominee (1, "Attack - Vibrato - Mix", "transient", signatureHolds ("transient", fig ([] (Figure& f) { f.transientDb = 0.0; }), fig ([] (Figure& f) { f.transientDb = 0.0; }))).verdict == "dropped", "role S2: Quantum's vibrato mix nominated as attack moves nothing -> dropped");
    check (signatureHolds ("transient", fig ([] (Figure& f) { f.transientDb = -24.0; }), fig ([] (Figure& f) { f.transientDb = 24.0; })).holds && signatureHolds ("sustain", fig ([] (Figure& f) { f.sustainDb = -6.95; }), fig ([] (Figure& f) { f.sustainDb = 7.69; })).holds, "role S3: Smack Attack's Attack and Sustain confirmed");
    // threshold: MannyM TripleD's "DeBoxy Thresh" GR 0 -> 0 at 6.5 kHz: dropped; DeEsser's Threshold 0 -> 11.7: confirmed; Melda's gate threshold on the wrong band's tone
    check (! signatureHolds ("threshold", fig ([] (Figure& f) { f.grDb = 0.0; }), fig ([] (Figure& f) { f.grDb = 0.0; })).holds, "role H1: TripleD's DeBoxy Thresh moves no GR on the sibilance tone -> dropped");
    check (signatureHolds ("threshold", fig ([] (Figure& f) { f.grDb = 0.0; }), fig ([] (Figure& f) { f.grDb = 11.7; })).holds, "role H2: DeEsser's Threshold -> confirmed");
    check (! signatureHolds ("band_threshold", fig ([] (Figure& f) { f.grDb = 0.05; }), fig ([] (Figure& f) { f.grDb = 0.0; })).holds, "role H3: C6's Band 1 Threshold on a tone that is not its band moves 0.05 dB -> dropped (the pairing was wrong, the measurement says so)");
    // frequency: DeEsser's Freq corner 2709 -> 13939 Hz -> confirmed; TB_Sibalance's "Stop freq" with no centre at either end -> dropped
    check (signatureHolds ("frequency", fig ([] (Figure& f) { f.centreHz = 2709.0; f.bandGainDb = -25.4; }), fig ([] (Figure& f) { f.centreHz = 13939.0; f.bandGainDb = -16.2; })).holds == false, "role Q1: a frequency whose band depth moves 9 dB with it is not a frequency ALONE (DeEsser's Freq also sets the depth: said)");
    check (signatureHolds ("frequency", fig ([] (Figure& f) { f.centreHz = 2709.0; }), fig ([] (Figure& f) { f.centreHz = 13939.0; })).holds, "role Q2: the corner moving 2.4 octaves is a frequency when the band's depth is not read");
    check (! signatureHolds ("frequency", Figure(), fig ([] (Figure& f) { f.centreHz = 10000.0; })).holds, "role Q3: TB's Stop freq with no centre at one end -> dropped (not measured at both positions)");
    check (signatureHolds ("q", fig ([] (Figure& f) { f.centreHz = 1000.0; f.bandwidthOct = 2.0; }), fig ([] (Figure& f) { f.centreHz = 1010.0; f.bandwidthOct = 0.5; })).holds && ! signatureHolds ("q", fig ([] (Figure& f) { f.centreHz = 1000.0; f.bandwidthOct = 1.0; }), fig ([] (Figure& f) { f.centreHz = 4000.0; f.bandwidthOct = 0.5; })).holds, "role Q4: a Q narrows the band with the centre still; a control that moves the centre too is a frequency");
    // gain: SBC's Gain (a path) vs its Input Gain (plain) read at two levels; a trim that moves nothing
    check (! signatureHolds ("gain", fig ([] (Figure& f) { f.levelDb = 0.0; f.levelDbQuiet = 0.0; }), fig ([] (Figure& f) { f.levelDb = 19.6; f.levelDbQuiet = 24.0; })).holds, "role G1: SBC's Gain moves 19.6 at one level and 24 at the quieter: a path, not a plain gain");
    check (signatureHolds ("gain", fig ([] (Figure& f) { f.levelDb = 0.0; f.levelDbQuiet = 0.0; }), fig ([] (Figure& f) { f.levelDb = 23.9; f.levelDbQuiet = 24.0; })).holds, "role G2: a gain that moves the same at both levels is confirmed");
    check (! signatureHolds ("gain", fig ([] (Figure& f) { f.levelDb = 0.0; }), fig ([] (Figure& f) { f.levelDb = 0.3; })).holds, "role G3: XLA-3's Noise Level moves 0.3 dB -> dropped");
    // gates and timing
    check (signatureHolds ("gate_threshold", fig ([] (Figure& f) { f.openLevelDb = -63.0; }), fig ([] (Figure& f) { f.openLevelDb = -23.0; })).holds && signatureHolds ("range", fig ([] (Figure& f) { f.rangeDb = 0.0; }), fig ([] (Figure& f) { f.rangeDb = -80.2; })).holds, "role K1: G8's Threshold and Reduction confirmed");
    check (signatureHolds ("hold", fig ([] (Figure& f) { f.holdMs = 129.6; }), fig ([] (Figure& f) { f.holdMs = 504.5; })).holds && ! signatureHolds ("attack", fig ([] (Figure& f) { f.attackMs = 8.8; }), fig ([] (Figure& f) { f.attackMs = 8.9; })).holds, "role K2: G8's Hold confirmed; a control that leaves the attack at 8.8 -> 8.9 is not an attack");
    check (! signatureHolds ("release", fig ([] (Figure& f) { f.releaseMs = 100.0; }), Figure()).holds, "role K3: a release not measured at both ends is dropped, never guessed");
    check (signatureHolds ("ceiling", fig ([] (Figure& f) { f.peakDb = -0.1; }), fig ([] (Figure& f) { f.peakDb = -6.0; })).holds && signatureHolds ("global", fig ([] (Figure& f) { f.grDb = 0.0; }), fig ([] (Figure& f) { f.grDb = 11.9; })).holds, "role K4: a ceiling and a global depth by their signatures");
    // the record: only the verdicts that say something are kept, the rest counted
    std::vector<RoleVerdict> all { nominee (1, "A", "drive", signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -60.0; }), fig ([] (Figure& f) { f.thdDb = -10.0; }))), unnamed (2, "B", "drive", signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -120.0; }), fig ([] (Figure& f) { f.thdDb = -119.0; }))), unnamed (3, "C", "drive", signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -60.0; }), fig ([] (Figure& f) { f.thdDb = -20.0; }))) };
    int notShown = 0; const auto kept = keep (all, notShown);
    check (kept.size() == 2 && notShown == 1 && kept[0].verdict == "confirmed" && kept[1].verdict == "measured_unnamed" && line (kept[1]).startsWith ("measured role drive, UNNAMED"), "role R1: confirmed and unnamed are kept, a probed control without the signature is counted (" + juce::String (notShown) + ")");
    check (signatureHolds ("nosuch", fig ([] (Figure&) {}), fig ([] (Figure&) {})).why.contains ("no signature defined"), "role R2: an unknown role has no signature, said");
    // the live lessons of the first role run (5 Oct evening): a silent end, a level read as a band, a post gain read as a ceiling, a bound at one end
    check (! signatureHolds ("drive", fig ([] (Figure& f) { f.thdDb = -10.2; f.outputDb = -88.0; }), fig ([] (Figure& f) { f.thdDb = -88.2; f.outputDb = -12.0; })).holds, "role L1: J37's Output Level at minimum is silent: THD on noise is not a drive signature");
    check (! signatureHolds ("eq_gain", fig ([] (Figure& f) { f.bandGainDb = 0.0; f.levelShiftDb = 0.0; }), fig ([] (Figure& f) { f.bandGainDb = 12.04; f.levelShiftDb = 12.0; })).holds, "role L2: bx_digital's Input Gain moves every tone alike: a level, not a band");
    check (signatureHolds ("eq_gain", fig ([] (Figure& f) { f.bandGainDb = 0.0; f.levelShiftDb = 0.0; }), fig ([] (Figure& f) { f.bandGainDb = 12.0; f.levelShiftDb = 0.5; })).holds, "role L3: a band that stands 12 dB out of a grid that moved 0.5 is a band");
    check (! signatureHolds ("transient", fig ([] (Figure& f) { f.transientDb = 0.0; f.sustainDb = 0.0; }), fig ([] (Figure& f) { f.transientDb = 48.0; f.sustainDb = 48.0; })).holds && signatureHolds ("transient", fig ([] (Figure& f) { f.transientDb = 0.0; f.sustainDb = 0.0; }), fig ([] (Figure& f) { f.transientDb = 17.2; f.sustainDb = 0.3; })).holds, "role L5: Smack Attack's Output moves transient and sustain alike (a level); TransX's Range moves the transient alone");
    check (! signatureHolds ("range", fig ([] (Figure& f) { f.rangeDb = 0.0; f.openGainDb = 0.0; }), fig ([] (Figure& f) { f.rangeDb = -147.0; f.openGainDb = -147.0; })).holds && signatureHolds ("range", fig ([] (Figure& f) { f.rangeDb = 0.0; f.openGainDb = 0.0; }), fig ([] (Figure& f) { f.rangeDb = -80.2; f.openGainDb = 0.1; })).holds, "role L6: G8's Output Gain moves the open level with the closed one (a level); its Reduction leaves the open level put");
    check (! signatureHolds ("ceiling", fig ([] (Figure& f) { f.peakDb = -1.0; }), fig ([] (Figure& f) { f.peakDb = -7.0; f.peakDriveDeltaDb = -5.9; })).holds && signatureHolds ("ceiling", fig ([] (Figure& f) { f.peakDb = -1.0; }), fig ([] (Figure& f) { f.peakDb = -6.0; f.peakDriveDeltaDb = 0.1; })).holds, "role L4: bx_limiter's Gain passes a 6 dB drive change through (a gain); its Ceiling holds it");
}

/** THE TEXT PASS TIMEOUT (ruling 3, 5 Oct evening): scaled to the count sampled - Saturn 2's 951 controls would need 601 s; 60 sampled need 66 s, floored at the process timeout. */
void testTextPassTimeout()
{
    using namespace ejmap::cert;
    check (textPassTimeoutMs (951, 120000) == 600600, "text T1: 951 controls x 3 samples x 200 ms + 30 s = 600.6 s (Saturn 2 sampled whole would get it, not 120 s)");
    check (textPassTimeoutMs (60, 120000) == 120000 && textPassTimeoutMs (200, 120000) == 150000, "text T2: a small count keeps the 120 s floor; 200 controls get 150 s");
    check (textPassTimeoutMs (0, 120000) == 120000, "text T3: nothing to sample still has the floor");
}

/** THE PHASE B BATCH (EjmapPhaseB.h, 5 Oct evening): the category table, the done marker, the progress and ETA arithmetic, the lines. */
void testPhaseB()
{
    using namespace ejmap::phaseb;
    check (categories().size() == 11 && categories().front().name == "gaincal" && categories()[1].name == "timing" && categories()[2].name == "limiter" && categories()[3].name == "eq" && categories()[4].name == "deesser" && categories()[5].name == "saturation" && categories().back().name == "multiband", "phaseb P1: eleven categories in the priority order (gain-cal, timing, limiter, EQ, de-esser, saturation/amp, reverb, delay, transient, gate, multiband)");
    for (const auto& c : categories()) check (c.guardS >= 600.0 && c.guardWhy.isNotEmpty(), "phaseb P2: " + c.name + " has a stated hang guard of at least 10 min (" + juce::String (c.guardS / 60.0, 0) + ")");
    check (categoryNamed ("saturation")->ledgerCategories.contains ("amp_sim") && modeWord ("--cert-reverb-delay") == "reverbdelay" && modeWord ("--cert-gain-cal") == "gaincal", "phaseb P3: amp sims ride with saturation; the mode word is the record folder");
    // the done marker: a row file, whole or absent
    juce::TemporaryFile tf; const auto dir = tf.getFile().getSiblingFile ("phaseb-pin"); dir.deleteRecursively(); dir.createDirectory();
    check (! isDone (dir, "eq", "AudioUnit_1_1.0"), "phaseb P4: nothing done before a row exists");
    rowFile (dir, "eq", "AudioUnit_1_1.0").getParentDirectory().createDirectory(); rowFile (dir, "eq", "AudioUnit_1_1.0").replaceWithText ("{}");
    check (isDone (dir, "eq", "AudioUnit_1_1.0") && ! isDone (dir, "eq", "AudioUnit_2_1.0") && ! isDone (dir, "limiter", "AudioUnit_1_1.0"), "phaseb P5: the row is the done marker, per category and stem");
    dir.deleteRecursively();
    // progress and ETA: medians per category, the overall median where a category has nothing timed yet
    Progress p; p.cats["eq"].total = 10; p.cats["eq"].done = 2; p.cats["eq"].seconds = { 400.0, 500.0 }; p.cats["limiter"].total = 4; p.cats["limiter"].done = 0; p.elapsedS = 900.0;
    check (std::abs (etaSeconds (p) - (8 * 450.0 + 4 * 450.0)) < 1e-9, "phaseb P6: ETA = products left x the category's median (450 s), the overall median (450) for a category not yet timed");
    p.cats["limiter"].seconds = { 100.0 }; p.cats["limiter"].done = 1;
    check (std::abs (etaSeconds (p) - (8 * 450.0 + 3 * 100.0)) < 1e-9, "phaseb P7: once a category is timed its own median rules");
    const auto line = progressLine (p, "limiter", "bx_limiter True Peak", "ok", 100.0);
    check (line.startsWith ("[limiter 1/4 | all 3/14] bx_limiter True Peak: ok 100 s | elapsed 0:15:00 | ETA 1:05:00"), "phaseb P8: the progress line (" + line + ")");
    const auto text = progressText (p);
    check (text.contains ("all: 3/14 done") && text.contains ("eq           2/10  ok 0") && text.contains ("median 450 s over 2") && text.contains ("next: limiter"), "phaseb P9: progress.txt says done/total per category, the medians, and what is next (limiter before eq in the priority order)");
    const auto back = progressFromVar (progressVar (p));
    check (back.cats.at ("eq").seconds.size() == 2 && back.cats.at ("limiter").total == 4 && std::abs (back.elapsedS - 900.0) < 1e-9, "phaseb P10: progress round-trips through its JSON (the resume carries the measured seconds)");
    check (hms (3725.0) == "1:02:05" && hms (0.0) == "0:00:00", "phaseb P11: h:mm:ss");
    // P12 the atomic write: the file is whole and its temp sibling is gone; a second write replaces, never appends
    { const auto d2 = tf.getFile().getSiblingFile ("phaseb-pin2"); d2.deleteRecursively(); d2.createDirectory(); const auto f = d2.getChildFile ("row.json");
      writeAtomic (f, "{\"a\":1}"); check (f.loadFileAsString() == "{\"a\":1}", "phaseb P12: writeAtomic writes the whole text");
      check (! f.getSiblingFile ("row.json.tmp").exists(), "phaseb P12: no temp sibling is left behind");
      writeAtomic (f, "{\"a\":2}"); check (f.loadFileAsString() == "{\"a\":2}", "phaseb P12: a second write replaces the first");
      // P13 the gzip of a raw trace: twice gives ONE copy of the content (the target is cleared, not appended to)
      const auto raw = d2.getChildFile ("x.raw.txt"); raw.replaceWithText ("trace line\n", false, false, "\n");
      const auto gzDir = d2.getChildFile ("gz"); gzDir.createDirectory(); gzipInto (raw, gzDir); const auto size1 = gzDir.getChildFile ("x.raw.txt.gz").getSize(); gzipInto (raw, gzDir);
      check (gzDir.getChildFile ("x.raw.txt.gz").getSize() == size1, "phaseb P13: the second gzip leaves the file the size of one (a decompressor stops at the first member, so the size is the tell)");
      juce::String back; juce::MemoryBlock head; { juce::FileInputStream fi (gzDir.getChildFile ("x.raw.txt.gz")); fi.readIntoMemoryBlock (head, 2); fi.setPosition (0); juce::GZIPDecompressorInputStream gz (&fi, false, juce::GZIPDecompressorInputStream::gzipFormat); back = gz.readEntireStreamAsString(); }
      check (back == "trace line\n", "phaseb P13: a re-gzipped trace holds one copy of the content, not two (got " + juce::String (back.length()) + " chars)");
      check (head.getSize() == 2 && (juce::uint8) head[0] == 0x1f && (juce::uint8) head[1] == 0x8b, "phaseb P15: the trace is a gzip FILE (magic 1f 8b), not a bare zlib stream - gunzip must open it");
      // P14 a half-done temp folder is not a DONE marker: only the row file is
      d2.getChildFile ("eq").getChildFile (".tmp-AudioUnit_1_1.0").createDirectory();
      check (! isDone (d2, "eq", "AudioUnit_1_1.0"), "phaseb P14: a .tmp folder without a row is not done");
      writeAtomic (rowFile (d2, "eq", "AudioUnit_1_1.0"), "{}"); check (isDone (d2, "eq", "AudioUnit_1_1.0"), "phaseb P14: the row file is the only DONE marker");
      // P16 (Kathy, 6 Oct): --redo names categories (every row) and/or nothing_nominated (ok rows with no record); nothing else is touched
      const auto okNoRec = juce::JSON::parse ("{\"outcome\":\"ok\",\"records\":[]}"), okRec = juce::JSON::parse ("{\"outcome\":\"ok\",\"records\":[\"eq/x.json\"]}"), failed = juce::JSON::parse ("{\"outcome\":\"failed\",\"records\":[]}");
      check (rowIsNothingNominated (okNoRec) && ! rowIsNothingNominated (okRec) && ! rowIsNothingNominated (failed), "phaseb P16: nothing_nominated = ok with no record (a failed row is not it)");
      check (rowToRedo (okNoRec, "eq", { "nothing_nominated" }) && ! rowToRedo (okRec, "eq", { "nothing_nominated" }) && rowToRedo (okRec, "timing", { "gaincal", "timing" }) && ! rowToRedo (okRec, "eq", { "gaincal", "timing" }) && ! rowToRedo (okNoRec, "eq", {}),
             "phaseb P16: a category in the list redoes every row of it; nothing_nominated only the empty ok rows; an empty list redoes nothing");
      { Progress pr; pr.redo = "gaincal,timing"; pr.cats["gaincal"].total = 3; check (progressText (pr).contains ("redo gaincal,timing") && progressFromVar (progressVar (pr)).redo == "gaincal,timing", "phaseb P16: the progress says what is being redone and it round-trips"); }
      d2.deleteRecursively(); }
}

/** THE ZIP REVIEW (EjmapCertReview.h, 5 Oct R1): hand-built records, outcomes and entry lists; every section's reading pinned. */
void testCertReview()
{
    using namespace ejmap::review;
    auto J = [] (const char* t) { return juce::JSON::parse (juce::String (t)); };
    // stamps: the newest measurement the record carries, whichever shape
    check (recordStamp (J (R"({"thresholdSweep":{"measuredAt":"20261003T194109.596+0100"}})")) == "20261003T194109.596+0100", "review R1: a single sweep's measuredAt is the stamp");
    check (recordStamp (J (R"({"thresholdCandidates":[{"thresholdSweep":{"measuredAt":"20261004T020000.000+0100"}},{"thresholdSweep":{"measuredAt":"20261004T030000.000+0100"}}]})")) == "20261004T030000.000+0100", "review R2: the newest candidate sweep is the stamp");
    check (recordStamp (J (R"({"thresholdRefusal":{"recordedAt":"20261002T154303.841+0100"}})")) == "20261002T154303.841+0100" && recordStamp (J (R"({"pitchCandidates":[{"measuredAt":"20261003T200700.000+0100"}]})")) == "20261003T200700.000+0100" && recordStamp (J (R"({"controls":[]})")).isEmpty(),
           "review R3: a refusal's recordedAt and a tuner candidate's measuredAt count; a record with nothing measured has no stamp");
    // hygiene from the zip's own names
    {
        const auto h = hygieneOf ({ "cert/", "cert/outcomes.json", "cert/fixtures/AudioUnit_1_1.0.json", "cert/fixtures/AudioUnit_1_1.0.defaults.json", "cert/AudioUnit_1_1.0.sweep.processes.json", "cert/raw/AudioUnit_1_1.0.sweep.pos00.1.txt" });
        check (h.ok() && h.certOnly() && h.records == 1 && h.processLists == 1 && h.rawFiles == 1 && h.recordsWithoutTraces.isEmpty(), "review H1: cert/ only, no config.json, one record with its process list and a raw capture -> OK");
        const auto bad = hygieneOf ({ "config.json", "cert/outcomes.json", "cert/fixtures/AudioUnit_2_1.0.json", "cert/raw/x.txt" });
        check (! bad.ok() && ! bad.certOnly() && bad.outsideCert == juce::StringArray { "config.json" } && bad.configJson == juce::StringArray { "config.json" } && bad.recordsWithoutTraces == juce::StringArray { "AudioUnit_2_1.0" } && bad.processLists == 0,
               "review H2: config.json outside cert/ is named twice (outside, and as config.json); a record without a process list is named; no process lists -> NOT OK");
        check (hygieneOf ({ "cert/fixtures/AudioUnit_3_2.0.json", "cert/AudioUnit_3_2.0.tuner.processes.json", "cert/raw/a.txt" }).recordsWithoutTraces.isEmpty(), "review H3: a tuner's process list counts as the record's traces");
    }
    // outcomes: counts, changes by product, added, gone, reason-only
    {
        const auto before = J (R"([{"identity":"A","product":"Alpha","state":"needs_review","reason":"r0"},{"identity":"B","product":"Beta","state":"exported","reason":"same"},{"identity":"C","product":"Gamma","state":"held","reason":"h"},{"identity":"D","product":"Delta","state":"refused","reason":"x"}])");
        const auto after  = J (R"([{"identity":"A","product":"Alpha","state":"exported","reason":"profile exported"},{"identity":"B","product":"Beta","state":"exported","reason":"same"},{"identity":"D","product":"Delta","state":"refused","reason":"y"},{"identity":"E","product":"Eps","state":"recorded","reason":"n"}])");
        const auto d = diffOutcomes (before, after);
        check (d.before.at ("needs_review") == 1 && d.after.at ("exported") == 2 && d.after.count ("needs_review") == 0, "review O1: counts per state on both sides");
        check (d.changes.size() == 1 && d.changes[0].product == "Alpha" && d.changes[0].before == "needs_review" && d.changes[0].after == "exported" && d.changes[0].reason == "profile exported", "review O2: one state change, by product, with the new reason");
        check (d.added == juce::StringArray { "Eps" } && d.gone == juce::StringArray { "Gamma" }, "review O3: a row only in the follow-up and a row only in the baseline are both named");
        check (d.reasonOnly.size() == 1 && d.reasonOnly[0].product == "Delta", "review O4: same state with a changed reason is said separately, not counted as a change");
    }
    // a projected re-sweep: happened by stamp, ended by row
    {
        const auto base = J (R"({"product":"P","thresholdRefusal":{"stage":"plan","recordedAt":"20261003T200000.000+0100"}})");
        const auto ran  = J (R"({"product":"P","thresholdSweep":{"measuredAt":"20261005T010000.000+0100","result":"certified"}})");
        const auto row  = J (R"({"identity":"P","product":"P","state":"exported","reason":"profile exported with its tone check"})");
        const auto l = resweepStatus ("P", "refused at plan; this build plans [1] Drive", false, base, ran, row);
        check (l.happened && l.ended == "exported: profile exported with its tone check", "review S1: a newer stamp than the baseline's = the re-sweep ran; the row says how it ended");
        const auto not1 = resweepStatus ("P", "w", false, base, base, J (R"({"state":"refused","reason":"stage plan: x"})"));
        check (! not1.happened && not1.ended == "refused: stage plan: x", "review S2: the same stamp = not run; the row's state stands");
        check (! resweepStatus ("P", "w", false, base, {}, {}).happened && resweepStatus ("P", "w", false, base, {}, {}).ended == "no record in the follow-up folder", "review S3: no follow-up record is said, not guessed");
        std::map<juce::String, juce::var> b { { "P", base }, { "Q", base } }, f { { "P", ran }, { "Q", base }, { "R", ran } };
        check (unprojectedResweeps (b, f, { "P" }) == juce::StringArray { "R (no baseline record)" }, "review S4: a record re-measured outside the projected list is named (Q unchanged, P projected, R new)");
        check (unprojectedResweeps (b, f, {}) == juce::StringArray { "P", "R (no baseline record)" }, "review S5: with nothing projected, P's newer stamp surfaces");
    }
    // sidechain readings
    {
        check (sidechainStatus ("X", J (R"({"sidechainPolicyCheck":{"verdict":"keep","norm":1.0,"level_dbfs":-6.0,"before_db":-20.8351,"after_db":-20.8367,"extraInputBuses":"Sidechain"}})")).word == "no effect", "review C1: keep -> no effect");
        const auto rs = sidechainStatus ("X", J (R"({"thresholdSweep":{"measuredAt":"20261005T012000.000+0100"},"sidechainPolicyCheck":{"verdict":"resweep","readAt":"20261005T011400.000+0100","norm":1.0,"level_dbfs":-6.0,"before_db":-9.01,"after_db":-9.54,"extraInputBuses":"Side-Chain Input Bus"}})"), J (R"({"state":"exported","reason":"profile exported with its tone check"})"));
        check (rs.word == "re-swept" && rs.detail.contains ("-9.01 -> -9.54") && rs.detail.contains ("re-sweep ran, ended exported"), "review C2: resweep -> re-swept, the numbers, and the re-sweep's end from the record's newer stamp and the row (" + rs.detail + ")");
        const auto rn = sidechainStatus ("X", J (R"({"thresholdSweep":{"measuredAt":"20261003T000000.000+0100"},"sidechainPolicyCheck":{"verdict":"resweep","readAt":"20261005T011400.000+0100","before_db":-9.01,"after_db":-9.54}})"));
        check (rn.detail.contains ("re-sweep NOT run"), "review C3: a resweep verdict with no measurement after the reading says the re-sweep did not run");
        check (sidechainStatus ("X", J (R"({"sidechainPolicyCheck":{"verdict":"window","why":"a window appeared"}})")).word == "window" && sidechainStatus ("X", J (R"({"product":"X"})")).word == "not read" && sidechainStatus ("X", {}).detail == "no record in the follow-up folder", "review C4: window, not read, and no record");
    }
    // review picks
    {
        const auto pick = J (R"({"product":"VBC Rack","candidate":"MU Threshold Left","by":"KL","date":"2026-10-05"})");
        const auto applied = J (R"({"product":"VBC Rack","thresholdCandidates":[{"index":3,"name":"MU Threshold Left","thresholdSweep":{"result":"certified"}}],"pickedCandidate":{"index":3,"name":"MU Threshold Left"},"ruleDecided":{"rule":"review_pick","by":"KL"}})");
        check (pickStatus (pick, applied, J (R"({"state":"exported","reason":"ok"})")).result == "applied -> exported: ok", "review P1: a record decided by the review pick of that candidate is applied");
        const auto other = J (R"({"product":"VBC Rack","thresholdCandidates":[{"index":3,"name":"MU Threshold Left","thresholdSweep":{"result":"certified"}}],"pickedCandidate":{"index":4,"name":"MU Threshold Right"},"ruleDecided":{"rule":"linked_pair"}})");
        check (pickStatus (pick, other, {}).result.startsWith ("NOT applied: the record was decided by linked_pair ('MU Threshold Right')"), "review P2: a record decided by a measured rule names that rule and its pick");
        const auto unpicked = J (R"({"product":"VBC Rack","thresholdCandidates":[{"index":3,"name":"MU Threshold Left","thresholdSweep":{"result":"flat"}}]})");
        check (pickStatus (pick, unpicked, {}).result.contains ("but its sweep is flat: nothing picked"), "review P3: an entry that names an uncertified candidate gets the follow-up's own refusal text");
        check (pickStatus (pick, {}, {}).result == "no record in the follow-up folder", "review P4: no record is said");
    }
    // tone checks
    {
        const auto tc = J (R"({"product":"T","g_db":2.0,"gr_measured_db":1.99,"pass_within_0_5_db":true,"deep_levels":[{"g_db":4.0,"ran":true,"gr_measured_db":4.0,"pass_within_0_5_db":true,"null_reason":null},{"g_db":5.0,"ran":true,"gr_measured_db":5.9,"pass_within_0_5_db":false,"null_reason":"failed_check_at_L"},{"g_db":6.0,"ran":false,"null_reason":"no_valid_L_clamp_geometry"}]})");
        const auto s = toneSummary ("T", tc);
        check (s.present && s.levels.size() == 4 && s.levels[0].result == "PASS" && s.levels[1].result == "PASS" && s.levels[2].result == "nulled" && s.levels[2].note == "failed_check_at_L" && s.levels[3].result == "not run",
               "review T1: g 2 from the top level, deep levels PASS / nulled (with its reason) / not run");
        check (toneLine (s) == "2:P 4:P 5:N 6:-  [5: failed_check_at_L; 6: no_valid_L_clamp_geometry]", "review T2: the one-line form (" + toneLine (s) + ")");
        const auto fail = toneSummary ("F", J (R"({"g_db":2.0,"gr_measured_db":1.02,"pass_within_0_5_db":false})"));
        check (toneLine (fail) == "2:F(1.02)", "review T3: a failed g 2 shows the GR read");
        const auto t = toneTotals ({ s, fail, toneSummary ("none", {}) });
        check (t.products == 2 && t.allPass == 0 && t.byLevel.at (2).at ("PASS") == 1 && t.byLevel.at (2).at ("FAIL") == 1 && t.byLevel.at (5).at ("nulled") == 1 && t.byLevel.at (6).at ("not run") == 1, "review T4: totals per level across products; a product without a tone check is not counted");
    }
    // deep points
    {
        const auto p = J (R"({"amount":{"curve":[{"in_at_gr_dbfs":{"1":-10,"2":-8,"3":-6,"4":-4,"5":null,"6":null,"7":null,"8":null,"9":null,"10":null,"11":null,"12":null}},{"in_at_gr_dbfs":{"1":-20,"2":-18,"3":-16,"4":-14,"5":-12,"6":-10,"7":null,"8":null,"9":null,"10":null,"11":null,"12":null}}]},"quality":{"deep_point_error_db":0.01}})");
        const auto d = deepPointsOf ("D", p);
        check (d.present && d.positions == 2 && d.deepSlots == 18 && d.deepPresent == 4 && std::abs ((double) d.dpe - 0.01) < 1e-9, "review D1: deep points are the numbers at levels 4..12 over the positions (4 of 18), with deep_point_error_db");
        check (! deepPointsOf ("D", J (R"({"schema":"x"})")).present, "review D2: a profile without an amount curve is not counted");
    }
    // inert
    {
        std::map<juce::String, juce::var> recs { { "V", J (R"({"thresholdSweep":{"result":"inert","reason":"processing never runs","inertCheck":{"inert":true}}})") }, { "W", J (R"({"thresholdSweep":{"result":"flat","inertCheck":{"inert":false,"reason":"Makeup moved the output 3.1 dB"}}})") }, { "Z", J (R"({"thresholdSweep":{"result":"flat"}})") } };
        const auto il = inertLines (recs);
        check (il.size() == 2 && il[0].product == "V" && il[0].word == "inert" && il[1].product == "W" && il[1].word == "checked, not inert" && il[1].reason.contains ("Makeup"), "review I1: inert and checked-not-inert, nothing for an unchecked flat");
    }
    // crashes
    {
        const juce::String run = "{\"tag\":\"pos00\",\"file\":\"AudioUnit_9_1.0.sweep.pos00.1.txt\",\"outcome\":\"exit 0\",\"clean\":true}\n"
                                 "{\"tag\":\"pos01\",\"file\":\"AudioUnit_9_1.0.sweep.pos01.1.txt\",\"outcome\":\"killed by signal 11\",\"clean\":false}\n"
                                 "{\"tag\":\"text-at\",\"file\":\"AudioUnit_9_1.0.defaults.text-at.1.txt\",\"outcome\":\"exit 3\",\"clean\":true}\n"
                                 "{\"tag\":\"pos02\",\"file\":\"AudioUnit_9_1.0.sweep.pos02.1.txt\",\"outcome\":\"SHOWED A WINDOW (PACEEdenExperience) after 2.1 s; killed by the driver\",\"clean\":false}\n";
        std::map<juce::String, juce::var> byStem { { "AudioUnit_9_1.0", J (R"({"product":"Nine","sidechainPolicyCheck":{"verdict":"crashed","why":"sidechain policy: the probe killed by signal 11 under the new policy"}})") } };
        const auto c = crashesIn (run, 0, J (R"([{"product":"Ten","reason":"the probe crashed (signal 11) on position 3"},{"product":"Nine","reason":"profile exported"}])"), byStem);
        check (c.size() == 4 && c[0].where == "Nine / pos01" && c[0].what == "killed by signal 11" && c[1].where == "Nine / pos02" && c[2].where == "Ten (row)" && c[3].where == "Nine (sidechain reading)", "review X1: a signal, a window, a row that says crashed, and a crashed sidechain reading; exit 0 and the answer code 3 are not crashes (" + juce::String ((int) c.size()) + ")");
        check (crashesIn (run, 2, {}, byStem).size() == 2 && crashesIn (run, 2, {}, byStem)[0].where == "Nine / pos02" && crashesIn (run, 2, {}, byStem)[1].where == "Nine (sidechain reading)", "review X2: the baseline's own run.jsonl lines are skipped (the follow-up's additions only); the record's own crashed reading still counts");
    }
    // the report names its sections and counts even when empty
    {
        ReportInput in; in.subjectName = "s.zip"; in.hygieneKnown = false;
        const auto r = render (in);
        check (r.contains ("1. HYGIENE") && r.contains ("2. OUTCOMES") && r.contains ("3. PROJECTED RE-SWEEPS") && r.contains ("no baseline: nothing projected") && r.contains ("4. SIDECHAIN READINGS\n") && r.contains ("0 record(s) in the set") && r.contains ("no review_picks.json") && r.contains ("0 product(s) with a tone check") && r.contains ("0 exported profile(s)") && r.contains ("inert: 0") && r.contains ("9. CRASHES") && r.contains ("\n0 (over 0 run.jsonl"),
               "review W1: every section prints with its count, zero said as zero");
        in.baselineKnown = true; in.states.before["exported"] = 43; in.states.after["exported"] = 45; in.states.changes.push_back ({ "Alpha", "needs_review", "exported", "ok" });
        in.resweeps.push_back ({ "P", "why", true, false, "exported: ok" }); in.resweeps.push_back ({ "Q", "why2", false, true, "refused: r" }); in.projectionKnown = true;
        const auto r2 = render (in);
        check (r2.contains ("exported              43 ->   45  (+2)") && r2.contains ("state changes by product: 1") && r2.contains ("  Alpha: needs_review -> exported | ok") && r2.contains ("2 projected, 1 happened, 1 not run") && r2.contains ("RAN     P | why | ended exported: ok") && r2.contains ("NOT RUN Q (after the re-derive) | why2 | ended refused: r"),
               "review W2: counts with their delta, the change line, the re-sweep lines with RAN / NOT RUN and how each ended");
    }
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
    testCandidateRules();
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
    testSidechainCheck();
    testInertCheck();
    testTunerPlanV2();
    testTunerV01();
    testSteppedExport();
    testGainCal();
    testTiming();
    testTimingSegments();
    testLimiter();
    testEq();
    testCertReview();
    testSaturation();
    testReverbDelay();
    testDynamics();
    testDeesser();
    testMultiband();
    testRoleEvidence();
    testTextPassTimeout();
    testPhaseB();
    testLoopOutcomes();
    testCategoriesMerge();

    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
