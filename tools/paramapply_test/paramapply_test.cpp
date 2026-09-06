/*
  typedReadbackMatch self-test, born from a live-telemetry defect: AMEK EQ 200
  logged partial, manual ["q"] on every dial while freq and gain applied.

  The Q control is a hardware-style stepped ladder and the display snaps to a
  step. The tolerance is half the bracketing anchor gap - "the map's own
  resolution" - and the common request q=0.7 sits at the EXACT midpoint of
  the 0.6/0.8 bracket, where the verdict is decided by float representation:
  |0.8 - 0.7| in float32 beats 0.5*gap by ~3e-8, so a correct best-effort
  write was reverted, every time, deterministically.

  The fixture is the REAL cached map's LF Q 1 anchor table (rev b3f287f9537d),
  not a synthetic ladder, so this test fails the same way the fleet did. The
  header under test is header-only inline code: including it IS the shipped
  implementation, no reimplementation and no lib-vs-header drift possible for
  these functions. The SharedCode link is for the JUCE symbols only.
*/

#include <JuceHeader.h>
#include "EchoJayParamApply.h"

static int passN = 0, failN = 0;
static void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok) { ++passN; std::cout << "  ok    " << name << "\n"; }
    else    { ++failN; std::cout << "  FAIL  " << name
                                 << (detail.isNotEmpty() ? ("\n        " + detail) : juce::String()) << "\n"; }
}

// LF Q 1, index 34, map rev b3f287f9537d - copied verbatim from the cache.
static juce::Array<juce::Array<float>> amekQTable()
{
    const float pairs[][2] = {
        { 0.400000006f, 0.100000001f }, { 0.600000024f, 0.150000006f },
        { 0.800000012f, 0.200000003f }, { 0.899999976f, 0.25f },
        { 1.10000002f,  0.300000012f }, { 1.29999995f,  0.350000024f },
        { 1.5f,         0.400000006f }, { 1.60000002f,  0.449999988f },
        { 1.79999995f,  0.5f },         { 2.0f,         0.550000012f },
        { 2.20000005f,  0.600000024f }, { 2.4000001f,   0.649999976f },
        { 2.70000005f,  0.699999988f }, { 2.9000001f,   0.75f },
        { 3.0999999f,   0.800000012f }, { 3.29999995f,  0.850000024f },
        { 3.5999999f,   0.899999976f }, { 3.79999995f,  0.949999988f },
        { 4.0f,         1.0f },
    };
    juce::Array<juce::Array<float>> t;
    for (auto& p : pairs) { juce::Array<float> a; a.add (p[0]); a.add (p[1]); t.add (a); }
    return t;
}

int main()
{
    std::cout << "typedReadbackMatch nearest-step self-test\n";
    const auto q = amekQTable();

    // THE REPRO. Asked 0.7, ladder snapped up to "0.8". Before the fix this
    // returned -1 by ~3e-8 and applyOne reverted a correct write.
    check (echojay::typedReadbackMatch ("q", 0.7f, "0.8", q) == +1,
           "q=0.7 landing on \"0.8\" (upper bracket step) is a match");
    check (echojay::typedReadbackMatch ("q", 0.7f, "0.6", q) == +1,
           "q=0.7 landing on \"0.6\" (lower bracket step) is a match");

    // The other common midpoint, both snap directions.
    check (echojay::typedReadbackMatch ("q", 1.0f, "0.9", q) == +1,
           "q=1.0 landing on \"0.9\" is a match");
    check (echojay::typedReadbackMatch ("q", 1.0f, "1.1", q) == +1,
           "q=1.0 landing on \"1.1\" is a match");

    // Off-midpoint interior value inside the old tolerance: unchanged.
    check (echojay::typedReadbackMatch ("q", 0.7f, "0.75", q) == +1,
           "q=0.7 landing \"0.75\" (inside half-gap) still matches");

    // The wrong-write class stays caught: landing on a NON-bracket step, or
    // nowhere near, is still a mismatch. Nearest-step must not become
    // any-step.
    check (echojay::typedReadbackMatch ("q", 0.7f, "1.5", q) == -1,
           "q=0.7 landing \"1.5\" (non-bracket step) is refused");
    check (echojay::typedReadbackMatch ("q", 0.7f, "4", q) == -1,
           "q=0.7 landing \"4\" (far end) is refused");

    // Unparseable display stays 0: cannot verify either way.
    check (echojay::typedReadbackMatch ("q", 0.7f, "wide", q) == 0,
           "unparseable display returns 0 (unverifiable)");

    // Continuous-display guard on a coarse table: a landing one full step
    // past the FAR bracket stays refused; a landing ON the far bracket is
    // now accepted as within the map's resolution (the spec'd trade).
    juce::Array<juce::Array<float>> gain;
    for (auto p : { std::pair<float,float>{ -24.0f, 0.0f }, { -21.0f, 0.1f },
                    { -18.0f, 0.2f }, { -15.0f, 0.3f }, { -12.0f, 0.4f } })
    { juce::Array<float> a; a.add (p.first); a.add (p.second); gain.add (a); }
    check (echojay::typedReadbackMatch ("gain_db", -17.0f, "-14.0", gain) == -1,
           "gain -17 landing \"-14\" (beyond the bracket) is refused");
    check (echojay::typedReadbackMatch ("gain_db", -17.0f, "-15.0", gain) == +1,
           "gain -17 landing \"-15\" (far bracket step) is accepted");

    // Named controls (map.controls reader): a control name derives NO unit
    // from semanticUnit, so the entry's own unit field must drive the parse.
    // "Mono Maker" showing "1.2 kHz" against a 1200 Hz target: unitless the
    // display reads 1.2 and a correct write reverts; with the override the
    // k multiplies and the landing verifies.
    {
        juce::Array<juce::Array<float>> hz;
        for (auto p : { std::pair<float,float>{ 20.0f, 0.0f }, { 200.0f, 0.25f },
                        { 600.0f, 0.5f }, { 1400.0f, 0.75f }, { 2000.0f, 1.0f } })
        { juce::Array<float> a; a.add (p.first); a.add (p.second); hz.add (a); }
        check (echojay::typedReadbackMatch ("Mono Maker", 1200.0f, "1.2 kHz", hz, "hz") == +1,
               "control unit override: \"1.2 kHz\" verifies a 1200 Hz target");
        check (echojay::typedReadbackMatch ("Mono Maker", 1200.0f, "1.2 kHz", hz) == -1,
               "without the override the same landing is refused (the lie the override removes)");
        check (echojay::typedReadbackMatch ("Mono Maker", 1200.0f, "3.5 kHz", hz, "hz") == -1,
               "the override does not loosen the tolerance: a wrong landing still refuses");
    }

    // The stepped-anchor corpus class (10 Aug 2026, pre-beta pin). The 9 Aug
    // re-sweep put 197 stepped-anchor controls on the server; Kiive XTComp's
    // Left/Right Ratio are the class-defining tables, verbatim from the
    // served map (plateau-midpoint norms, 8 rungs 1:1..8:1). The ladder
    // filing's failure - an off-rung request whose snapped landing reverts a
    // correct write - must be impossible here, and a landing on a NON-rung,
    // or the wrong rung, must still revert.
    {
        juce::Array<juce::Array<float>> ratio;
        for (auto p : { std::pair<float,float>{ 1.0f, 0.0f },  { 2.0f, 0.15f },
                        { 3.0f, 0.30f }, { 4.0f, 0.40f }, { 5.0f, 0.55f },
                        { 6.0f, 0.70f }, { 7.0f, 0.85f }, { 8.0f, 0.95f } })
        { juce::Array<float> a; a.add (p.first); a.add (p.second); ratio.add (a); }

        // Off-rung request between 3:1 and 4:1: either bracketing rung is the
        // plugin doing its job, and neither may revert.
        check (echojay::typedReadbackMatch ("Left Ratio", 3.5f, "4.0", ratio) == +1,
               "XTComp ratio 3.5 snapping up to \"4.0\" is a match, not a revert");
        check (echojay::typedReadbackMatch ("Left Ratio", 3.5f, "3.0", ratio) == +1,
               "XTComp ratio 3.5 snapping down to \"3.0\" is a match");
        // Ratio-styled display text parses the same way.
        check (echojay::typedReadbackMatch ("Left Ratio", 3.5f, "4:1", ratio) == +1,
               "a \"4:1\" display verifies the same landing");
        // Genuinely wrong writes still revert: a non-bracket rung and a
        // value off the ladder entirely.
        check (echojay::typedReadbackMatch ("Left Ratio", 3.5f, "6.0", ratio) == -1,
               "XTComp ratio 3.5 landing \"6.0\" (non-bracket rung) still reverts");
        check (echojay::typedReadbackMatch ("Left Ratio", 2.0f, "8.0", ratio) == -1,
               "XTComp ratio 2 landing \"8.0\" (far end) still reverts");
        // Exact-rung ask, exact-rung landing: the trivial case stays trivial.
        check (echojay::typedReadbackMatch ("Left Ratio", 4.0f, "4.0", ratio) == +1,
               "exact rung ask landing on its own rung matches");
    }

    // Band reach (the AMEK 8 kHz incident): ingestGroups dropped freq_range
    // from every stored map, "no range" read as reachable, and a 15-780 Hz
    // band took an 8 kHz request as a verified clamp to 780. The reach
    // window now derives from the band's own freq_hz anchors when the
    // summary field is absent - the anchors ARE the reachability.
    {
        auto makeGroup = [] (float lo, float hi, bool withRangeField, bool withFreqEntry)
        {
            juce::DynamicObject::Ptr g = new juce::DynamicObject();
            if (withRangeField)
            {
                juce::Array<juce::var> fr; fr.add (lo); fr.add (hi);
                g->setProperty ("freq_range", juce::var (fr));
            }
            juce::DynamicObject::Ptr params = new juce::DynamicObject();
            if (withFreqEntry)
            {
                juce::DynamicObject::Ptr fe = new juce::DynamicObject();
                fe->setProperty ("index", 5);
                fe->setProperty ("kind", "anchored");
                juce::Array<juce::var> av;
                for (auto p : { std::pair<float,float>{ lo, 0.0f }, { (lo + hi) / 2, 0.5f }, { hi, 1.0f } })
                { juce::Array<juce::var> pr; pr.add (p.first); pr.add (p.second); av.add (juce::var (pr)); }
                fe->setProperty ("anchors", juce::var (av));
                params->setProperty ("freq_hz", juce::var (fe.get()));
            }
            g->setProperty ("params", juce::var (params.get()));
            return juce::var (g.get());
        };
        auto rangeField = echojay::bandReachRange (makeGroup (15.0f, 780.0f, true, false));
        check (rangeField.known && rangeField.lo == 15.0f && rangeField.hi == 780.0f,
               "freq_range field wins when present");
        auto derived = echojay::bandReachRange (makeGroup (15.0f, 780.0f, false, true));
        check (derived.known && derived.lo == 15.0f && derived.hi == 780.0f,
               "reach derives from freq_hz anchors when freq_range absent (the AMEK shape)");
        check (! (8000.0f >= derived.lo && 8000.0f <= derived.hi),
               "8 kHz is out of reach of the derived 15-780 window");
        auto unknown = echojay::bandReachRange (makeGroup (0, 0, false, false));
        check (! unknown.known, "no freq entry at all reads unknown, not unreachable");
    }

    // The map entry itself is healthy - documents that the fleet failure was
    // never about the data. Mirrors the usableParamEntry rules.
    {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("index", 34);
        e->setProperty ("kind", "anchored");
        juce::Array<juce::var> av;
        for (auto& a : q) { juce::Array<juce::var> p; p.add (a[0]); p.add (a[1]); av.add (juce::var (p)); }
        e->setProperty ("anchors", juce::var (av));
        check (echojay::usableParamEntry (juce::var (e.get())),
               "the real AMEK q entry passes usableParamEntry");
    }

    // ---- BORROWED LABELS: the mode-branch escape (29 Aug 2026) -------------
    // The class-1 label join attaches a sibling's labels to a binary that
    // reports its value as an ORDINAL. The text comparison then compares a name
    // against a number, and before the escape applyOne reverted every such
    // write. MEASURED on the Saturn 2 pilot, from this path:
    //   asked "Warm Tape", plugin shows "6", value restored
    // and "Warm Tape" IS index 6 - the write was right, the instrument wrong.
    //
    // The two halves are pinned AGAINST each other. Half A alone passes with no
    // gate at all; Half B is what makes the gate mean something.
    {
        std::cout << "borrowed-label mode escape:\n";
        const juce::ScopedJuceInitialiser_GUI juceInit;

        // Saturn 2's Band 1 Style label ladder, k/27, verbatim in shape.
        const char* kStyle[] = { "Subtle Tube", "Clean Tube", "Warm Tube", "Broken Tube",
                                 "Subtle Tape", "Clean Tape", "Warm Tape", "Old Tape" };
        const int   kN = 8;
        auto normFor = [] (int i) { return (float) i / (float) (kN - 1); };

        // How the fixture answers getText: like an AU (the ordinal), like a
        // VST3 (the label), or like a plugin that ignores writes entirely.
        enum class Face { Ordinal, Label, WrongLabel };

        // A HostedParameter, not a bare AudioProcessorParameter: an
        // AudioPluginInstance accepts only those, which is also what applyOne
        // meets in the field.
        struct FixtureParam final : public juce::HostedAudioProcessorParameter
        {
            juce::String getParameterID() const override { return "style"; }
            FixtureParam (Face f, int n, const char** labels, bool ignoreWrites)
                : face (f), steps (n), names (labels), frozen (ignoreWrites) {}
            Face face; int steps; const char** names; bool frozen;
            float v = 0.0f;
            float getValue() const override { return v; }
            void  setValue (float nv) override { if (! frozen) v = nv; }
            float getDefaultValue() const override { return 0.0f; }
            juce::String getName (int) const override { return "Band 1 Style"; }
            juce::String getLabel() const override { return {}; }
            int   getNumSteps() const override { return steps; }
            bool  isDiscrete() const override { return true; }
            juce::String getText (float nv, int) const override
            {
                const int i = juce::jlimit (0, steps - 1,
                                            (int) std::lround (nv * (float) (steps - 1)));
                if (face == Face::Ordinal)    return juce::String (i);
                if (face == Face::WrongLabel) return "Definitely Not It";
                return names[i];
            }
            float getValueForText (const juce::String&) const override { return 0.0f; }
        };

        struct FixturePlugin final : public juce::AudioPluginInstance
        {
            void fillInPluginDescription (juce::PluginDescription&) const override {}
            const juce::String getName() const override { return "Fixture"; }
            void prepareToPlay (double, int) override {}
            void releaseResources() override {}
            void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            double getTailLengthSeconds() const override { return 0.0; }
            bool acceptsMidi() const override { return false; }
            bool producesMidi() const override { return false; }
            juce::AudioProcessorEditor* createEditor() override { return nullptr; }
            bool hasEditor() const override { return false; }
            int getNumPrograms() override { return 1; }
            int getCurrentProgram() override { return 0; }
            void setCurrentProgram (int) override {}
            const juce::String getProgramName (int) override { return {}; }
            void changeProgramName (int, const juce::String&) override {}
            void getStateInformation (juce::MemoryBlock&) override {}
            void setStateInformation (const void*, int) override {}
        };

        // A mode entry; joinedFrom empty => labels measured on THIS binary.
        auto modeEntry = [&] (const juce::String& joinedFrom)
        {
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty ("index", 0);
            e->setProperty ("kind", "mode");
            e->setProperty ("caseInsensitiveOk", true);
            // BOTH fixtures carry trust "setread", because the corpus does:
            // 5,772 of 5,772 genuine mode controls have it. Without this the
            // trust-gate mutation below would redden nothing and the gate pin
            // would be worthless - the exact trap this block exists to close.
            e->setProperty ("trust", "setread");
            juce::DynamicObject::Ptr labs = new juce::DynamicObject();
            for (int i = 0; i < kN; ++i) labs->setProperty (juce::Identifier (kStyle[i]), normFor (i));
            e->setProperty ("labels", juce::var (labs.get()));
            if (joinedFrom.isNotEmpty()) e->setProperty ("joined_from", joinedFrom);
            return juce::var (e.get());
        };
        auto run = [&] (Face face, bool frozen, const juce::String& joinedFrom,
                        const juce::String& ask, echojay::ApplyResult& out, float& landedNorm)
        {
            FixturePlugin p;
            auto owned = std::make_unique<FixtureParam> (face, kN, kStyle, frozen);
            auto* fp = owned.get();
            p.addHostedParameter (std::move (owned));
            out = echojay::applyOne (p, "Band 1 Style", modeEntry (joinedFrom), ask);
            landedNorm = fp->v;
        };

        echojay::ApplyResult r; float landed = -1.0f;
        const juce::String kSrc = "3ead8437660dd53fc82e0e8843ed38dc659a739b743d510b7fca30e299758b64";

        // HALF A. Borrowed labels, plugin answers with the ordinal "6".
        run (Face::Ordinal, false, kSrc, "Warm Tape", r, landed);
        check (r.applied, "half A: a joined control APPLIES against an ordinal-returning plugin");
        check (! r.displayVerified,
               "half A: and does NOT claim display verification");
        check (! r.readbackMismatch, "half A: it is not a readback mismatch");
        check (std::abs (landed - normFor (6)) < 1.0e-6f,
               "half A: the parameter actually moved to \"Warm Tape\" (index 6)",
               "landed norm " + juce::String (landed, 6));
        check (r.note == "applied (display unverifiable on this plugin)",
               "half A: reuses the existing unverifiable-display result state", r.note);

        // HALF B. THE PIN THAT MAKES THE GATE MEAN SOMETHING. Native labels, no
        // joined_from, and the plugin reports the WRONG label: must still revert.
        // Re-gate the escape on trust=="setread" and this goes red, because every
        // genuine mode control in the corpus carries that trust (5,772 of 5,772).
        run (Face::WrongLabel, false, {}, "Warm Tape", r, landed);
        check (! r.applied, "half B: a NATIVE mode control still reverts on wrong text");
        check (r.readbackMismatch, "half B: and reports a readback mismatch");
        check (std::abs (landed - 0.0f) < 1.0e-6f,
               "half B: the parameter is restored to its pre-write value",
               "landed norm " + juce::String (landed, 6));

        // The escape is not a rubber stamp: a write that does not stick reverts.
        run (Face::Ordinal, /*frozen*/ true, kSrc, "Warm Tape", r, landed);
        check (! r.applied && r.readbackMismatch,
               "not a rubber stamp: a joined control whose write is ignored still reverts", r.note);

        // Strictness is untouched where the text DOES work: a joined control on a
        // plugin that happens to answer with the label verifies the strict way.
        run (Face::Label, false, kSrc, "Warm Tape", r, landed);
        check (r.applied && r.displayVerified,
               "text-agreeing joined control still takes the STRICT path and verifies");

        // An unknown label is still refused, gate or no gate.
        run (Face::Ordinal, false, kSrc, "Not A Style", r, landed);
        check (! r.applied && r.note.startsWith ("unknown mode label"),
               "an unknown label is still refused on a joined control", r.note);

        // CORPUS FACT, so nobody reintroduces the trust gate believing it
        // discriminates: measured across 133 maps, every genuine mode control
        // carries trust "setread". The escape must therefore key on provenance.
        {
            auto f = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                        .getChildFile ("Library/EchoJay/param_maps.json");
            int modeCtl = 0, modeSetread = 0;
            if (auto root = juce::JSON::parse (f.loadFileAsString());
                auto* maps = root.getProperty ("maps", juce::var()).getDynamicObject())
            {
                for (auto& kv : maps->getProperties())
                    if (auto* ctls = kv.value.getProperty ("controls", juce::var()).getDynamicObject())
                        for (auto& c : ctls->getProperties())
                            if (c.value.getProperty ("kind", juce::var()).toString() == "mode"
                                && c.value.getProperty ("labels", juce::var()).getDynamicObject() != nullptr)
                            {
                                ++modeCtl;
                                if (c.value.getProperty ("trust", juce::var()).toString() == "setread")
                                    ++modeSetread;
                            }
            }
            if (modeCtl == 0)
                std::cout << "  SKIP  corpus-fact pin (no cached maps on this machine)\n";
            else
                check (modeSetread == modeCtl,
                       "corpus fact: trust \"setread\" is on EVERY mode control, so it cannot gate the escape",
                       juce::String (modeSetread) + " of " + juce::String (modeCtl));
        }
    }

    std::cout << passN << " passed, " << failN << " failed\n";
    return failN == 0 ? 0 : 1;
}
