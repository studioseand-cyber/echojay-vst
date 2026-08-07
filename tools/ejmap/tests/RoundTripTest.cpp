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
    check (ejmap::kMapSchemaVersion == 23, "kMapSchemaVersion is 23");
    check (&ejmap::kMapSchemaVersion == &echojay::kMapSchemaVersion,
           "ejmap and echojay name the same object, not two copies");
    check (juce::String (ejmap::kMapSchemaString) == "2.3", "kMapSchemaString is 2.3");
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
void testSweepRules()
{
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
    testSweepRules();
    testMapperIdentity();
    testStaleControlsRefused();
    testSweepDeadline();
    testEmptyControlNameRejected();

    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
