/*
  RoundTripTest.cpp

  The drift guard. This is the test the spec calls non-negotiable.

  Claim under test: a map produced by ejmap, run through EchoJay's applySettings
  from the shared header, produces the identical writes the mapper verified
  during mapping.

  If that stops being true, ejmap is verifying one thing and EchoJay is doing
  another, which is the exact failure shape that let usableCoreCount go stale and
  read bx_digital V3 as not dialable.

  It runs against real maps in ~/Library/ejmap/maps/, not fixtures. A test that
  passes and a feature that works are different claims, and on this project the
  gap has bitten three times: the tripwire had green tests and had never fired,
  the client gate had 35/35 in a harness and wrote +16 dB in Logic.

  SEAM. Two things below need the real shared headers and are marked TODO(fable).
  Do not invent signatures for them. Read Source/EchoJayParamApply.h and wire the
  real calls.
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

        // TODO(fable): the actual round trip.
        //
        //   1. Build the settings block the mapper verified, from
        //      evidence.readback (semantic -> wrote).
        //   2. Run it through EchoJay's applySettings using this map.
        //   3. Assert the resulting writes (index, normalised value) are
        //      identical to what evidence recorded.
        //
        // This needs the real signature from Source/EchoJayParamApply.h. Read it
        // and wire it. Do not reimplement interpolation here: reimplementing it
        // is precisely the drift this test exists to catch.

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
    testAgainstRealMaps();

    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
