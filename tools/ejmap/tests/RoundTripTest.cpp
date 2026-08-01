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

// The shared sweep and parsers, compiled here so the drift gate proves both
// binaries build the SAME code: ejextract compiles these headers to produce
// the corpus, ejmap compiles them to produce anchors, and this test pins the
// behaviours M3 leans on. EjmapSchema.h already pulls in EchoJayParamApply.h
// (parseDisplayForUnit, dominantMonotonicTable); the extractor header is the
// M3 lift.
#include "EchoJayParamExtractor.h"
#include "EjmapMouth.h"

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
    check (ejmap::kMapSchemaVersion == 22, "kMapSchemaVersion is 22");
    check (&ejmap::kMapSchemaVersion == &echojay::kMapSchemaVersion,
           "ejmap and echojay name the same object, not two copies");
    check (juce::String (ejmap::kMapSchemaString) == "2.2", "kMapSchemaString is 2.2");
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

        check (v.getProperty ("schema", "").toString() == ejmap::kMapSchemaString,
               "map schema matches binary: " + entry.getFile().getFileName());

        // The write-level round trip runs against the synthetic instance in
        // testRoundTripThroughApplySettings, because this gate must not load
        // real plugins. What CAN be checked per real map without one is the
        // interpolation stage: every evidence.readback pair (semantic ->
        // asked -> wrote) must reproduce through the real interpolateAnchors
        // over the map's own table. This is the stage that drifted in the
        // usableCoreCount incident, and it needs no instantiation.
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
int main (int, char**)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testSchemaVersionPinned();
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
    testAgainstRealMaps();

    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
