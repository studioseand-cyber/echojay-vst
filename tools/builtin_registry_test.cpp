/*
    builtin_registry_test.cpp  —  end-to-end test for the built-in device
    framework (BUILTIN_SUITE_PLAN.md Wave 0).

    Everything from the registrar to a dialled parameter, exercised for real: the
    registry is populated by the SAME static initialisers the plugin uses, devices
    are constructed through the SAME factory ChainHost calls, and moves go through
    the SAME applyStructured funnel the chain hands settings_structured to.

    This is the piece that cannot be checked by a g++ test — ParamSchema and
    GainEngine are JUCE-free and tested in test/, but the registry, the device
    processors and the apply path all need JUCE. So it is a console app linked
    against the same sources.

    Scope note: this is a console app, so its sources are linked directly rather
    than through a static archive. It therefore proves that registration, schemas
    and the apply path are CORRECT — it does NOT reproduce the plugin build's
    static-library link, where an unreferenced registrar can be discarded. That
    failure mode is handled by the force-load block in CMakeLists.txt, and was
    verified there by relinking with it disabled and watching both Wave 0 devices
    disappear from the binary.

    Build + run:
        cmake --build build --target EchoJayBuiltinRegistryTest
        ./build/EchoJayBuiltinRegistryTest_artefacts/Debug/EchoJayBuiltinRegistryTest
*/

#include <JuceHeader.h>

#include "EedDeviceRegistry.h"
#include "EedDeviceProcessor.h"
#include "EedExciterProcessor.h"
#include "EedGainProcessor.h"
#include "EedPhaseInvertProcessor.h"
#include "EedSaturationProcessor.h"
#include "EedTapeProcessor.h"
#include "SurgicalEqProcessor.h"

#include <cmath>
#include <memory>

static int g_fail = 0;

static void check (bool cond, const juce::String& what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what.toRawUTF8());
    if (! cond) ++g_fail;
}

static bool near (double a, double b, double tol = 1.0e-4)
{
    return std::fabs (a - b) <= tol;
}

// Build the settings_structured a server would send: { "params": { ... } }
static juce::var paramsMove (std::initializer_list<std::pair<const char*, juce::var>> kv)
{
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    for (const auto& p : kv) params->setProperty (juce::Identifier (p.first), p.second);

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("params", juce::var (params.get()));
    return juce::var (root.get());
}

static std::unique_ptr<juce::AudioProcessor> makeByName (const juce::String& name)
{
    const auto* d = BuiltinDeviceRegistry::instance().findByName (name);
    return d != nullptr ? d->create() : nullptr;
}

int main()
{
    // Devices are juce::AudioProcessors; some JUCE machinery expects an
    // initialised environment even headless.
    juce::ScopedJuceInitialiser_GUI juceInit;

    auto& registry = BuiltinDeviceRegistry::instance();

    // -----------------------------------------------------------------------
    std::printf ("== every registrar survived static init AND the linker ==\n");
    check (registry.findByName ("EchoJay EQ")            != nullptr, "EchoJay EQ registered");
    check (registry.findByName ("EchoJay Gain")          != nullptr, "EchoJay Gain registered");
    check (registry.findByName ("EchoJay Phase Invert")  != nullptr, "EchoJay Phase Invert registered");
    check (registry.findByName ("EchoJay Saturation")    != nullptr, "EchoJay Saturation registered");
    check (registry.findByName ("EchoJay Tape")          != nullptr, "EchoJay Tape registered");
    check (registry.findByName ("EchoJay Exciter")       != nullptr, "EchoJay Exciter registered");
    check (registry.all().size() == 6, "exactly 6 devices registered (got "
                                       + juce::String ((int) registry.all().size()) + ")");

    // -----------------------------------------------------------------------
    std::printf ("== ordering is deterministic, not static-init order ==\n");
    {
        const auto names = registry.names();
        check (names[0] == "EchoJay EQ", "EQ sorts first (category rank)");
        // Utility comes after EQ; within it, alphabetical.
        check (names.indexOf ("EchoJay Gain") < names.indexOf ("EchoJay Phase Invert"),
               "Gain before Phase Invert (alphabetical within Utility)");
        // Harmonic ranks after Utility, and sorts alphabetically inside itself.
        check (names.indexOf ("EchoJay Phase Invert") < names.indexOf ("EchoJay Exciter"),
               "Utility before Harmonic (category rank)");
        check (names.indexOf ("EchoJay Exciter") < names.indexOf ("EchoJay Saturation")
            && names.indexOf ("EchoJay Saturation") < names.indexOf ("EchoJay Tape"),
               "Exciter, Saturation, Tape (alphabetical within Harmonic)");
        check (registry.categories().joinIntoString (",") == "EQ,Utility,Harmonic",
               "categories in canonical order: " + registry.categories().joinIntoString (","));
    }

    // -----------------------------------------------------------------------
    std::printf ("== name resolution is tolerant, and honours aliases ==\n");
    check (registry.findByName ("echojay gain")   != nullptr, "case-insensitive");
    check (registry.findByName ("EchoJayGain")    != nullptr, "alias without the space");
    check (registry.findByName ("EchoJay Trim")   != nullptr, "declared alias");
    check (registry.findByName ("EchoJay Polarity") != nullptr, "phase-invert alias");
    check (registry.findByName ("Pro-Q 3")        == nullptr, "a third-party name is NOT ours");

    // -----------------------------------------------------------------------
    std::printf ("== synthetic descriptions (what saved chain XML carries) ==\n");
    {
        const auto descs = registry.descriptions();
        check (descs.size() == 6, "one description per device");
        for (const auto& d : descs)
        {
            check (d.pluginFormatName == kEchoJayBuiltinFormat,
                   d.name + " carries the built-in format name");
            check (BuiltinDeviceRegistry::isBuiltinDescription (d),
                   d.name + " is recognised as a built-in description");
        }
        // The EQ's identity is frozen — changing it orphans saved chains.
        const auto* eq = registry.findByName ("EchoJay EQ");
        check (eq->identifier == "echojay:builtin:eq", "EQ identifier unchanged");
        check (eq->uid == 0x456A4551, "EQ uid unchanged");
    }

    // -----------------------------------------------------------------------
    std::printf ("== restore resolves a PARTIAL description (identifier / uid) ==\n");
    {
        juce::PluginDescription partial;                 // no name at all
        partial.fileOrIdentifier = "echojay:builtin:gain";
        check (registry.findForDescription (partial) != nullptr, "resolved by identifier alone");

        juce::PluginDescription byUid;
        byUid.uniqueId = 0x456A5049;                     // phase invert
        const auto* found = registry.findForDescription (byUid);
        check (found != nullptr && found->name == "EchoJay Phase Invert", "resolved by uid alone");
    }

    // -----------------------------------------------------------------------
    std::printf ("== the factory builds a real, concrete processor ==\n");
    for (const auto& d : registry.all())
    {
        auto proc = d.create();
        check (proc != nullptr, d.name + " constructs");
        if (proc != nullptr)
            check (proc->getName() == d.name,
                   d.name + " reports its own name (\"" + proc->getName() + "\")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== EchoJay Gain dials from settings_structured.params ==\n");
    {
        auto proc = makeByName ("EchoJay Gain");
        auto* gain = dynamic_cast<EedGainProcessor*> (proc.get());
        check (gain != nullptr, "constructed as an EedGainProcessor");

        // Routed exactly as ChainHost routes it: through the device base.
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "IS an EedDeviceProcessor (so ChainHost can dispatch)");

        int applied = 0, skipped = 0;
        const auto summary = device->applyStructured (
            paramsMove ({ { "level_db", -6.0 }, { "pan", 0.5 } }), &applied, &skipped);

        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (summary.isNotEmpty(), "summary: " + summary);
        check (near (gain->getParamValue ("level_db"), -6.0), "level_db landed EXACTLY at -6");
        check (near (gain->getParamValue ("pan"), 0.5),       "pan landed EXACTLY at 0.5");
    }

    // -----------------------------------------------------------------------
    std::printf ("== merge semantics: an absent param is left alone ==\n");
    {
        auto proc = makeByName ("EchoJay Gain");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        device->applyStructured (paramsMove ({ { "level_db", -6.0 }, { "pan", 0.5 } }));
        device->applyStructured (paramsMove ({ { "level_db", -3.0 } }));   // pan not mentioned

        check (near (device->getParamValue ("level_db"), -3.0), "level_db updated");
        check (near (device->getParamValue ("pan"), 0.5),       "pan SURVIVED the second move");
    }

    // -----------------------------------------------------------------------
    std::printf ("== the schema clamps, and reports honestly ==\n");
    {
        auto proc = makeByName ("EchoJay Gain");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        int applied = 0, skipped = 0;
        const auto summary = device->applyStructured (
            paramsMove ({ { "level_db", 999.0 }, { "wobble", 3.0 } }), &applied, &skipped);

        check (near (device->getParamValue ("level_db"), 24.0), "999 dB clamped to the +24 max");
        check (applied == 1, "the in-schema param applied");
        check (skipped == 1, "the unknown id was SKIPPED, not guessed at");
        check (summary.contains ("wobble"), "and named in the summary: " + summary);
    }

    // -----------------------------------------------------------------------
    std::printf ("== value coercion: JSON shapes a model actually emits ==\n");
    {
        auto proc = makeByName ("EchoJay Phase Invert");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "invert_left", true } }));
        check (near (device->getParamValue ("invert_left"), 1.0), "JSON true -> on");

        device->applyStructured (paramsMove ({ { "invert_left", "off" } }));
        check (near (device->getParamValue ("invert_left"), 0.0), "the string \"off\" -> off");

        device->applyStructured (paramsMove ({ { "invert_right", "1" } }));
        check (near (device->getParamValue ("invert_right"), 1.0), "the string \"1\" -> on");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "invert_left", "auto" } }), &applied, &skipped);
        check (skipped == 1, "a non-numeric string is rejected, NOT silently read as 0");
    }

    // -----------------------------------------------------------------------
    std::printf ("== a flat device declines a bare array (it is not eq_bands) ==\n");
    {
        auto proc = makeByName ("EchoJay Gain");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        juce::Array<juce::var> arr;
        arr.add (juce::var (1.0));
        check (device->applyStructured (juce::var (arr)).isEmpty(),
               "bare array -> empty summary (\"this wasn't for me\")");

        juce::DynamicObject::Ptr bag = new juce::DynamicObject();
        bag->setProperty ("warmth", "more");                 // anchor-path semantics
        check (device->applyStructured (juce::var (bag.get())).isEmpty(),
               "a flat semantic bag -> empty summary");
    }

    // -----------------------------------------------------------------------
    std::printf ("== the EQ still takes eq_bands, AND now takes params ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (eq != nullptr && device != nullptr, "EQ is both itself and a device");

        // Legacy bare array — must never break.
        juce::DynamicObject::Ptr band = new juce::DynamicObject();
        band->setProperty ("type", "bell");
        band->setProperty ("freq_hz", 400.0);
        band->setProperty ("gain_db", -3.5);
        band->setProperty ("q", 3.0);
        juce::Array<juce::var> bands;
        bands.add (juce::var (band.get()));

        int applied = 0, skipped = 0;
        const auto s1 = device->applyStructured (juce::var (bands), &applied, &skipped);
        check (applied == 1, "bare eq_bands array still applies (1 band)");
        check (s1.isNotEmpty(), "summary: " + s1);
        check (near (eq->getBand (0).freqHz, 400.0, 0.5), "band landed at 400 Hz exactly");
        check (near (eq->getBand (0).gainDb, -3.5, 0.01), "band landed at -3.5 dB exactly");

        // The universal flat path, on the same device.
        const auto s2 = device->applyStructured (paramsMove ({ { "output_db", -2.0 } }));
        check (s2.isNotEmpty(), "params summary: " + s2);
        check (near (eq->getOutputDb(), -2.0, 0.01), "output_db dialled through params");
    }

    // -----------------------------------------------------------------------
    // A selector is a knob, and the house rule is that a knob a human can turn is
    // one the model can set exactly. "tube" carries no digits, so without choice
    // resolution it would be rejected as "not a number" and the move would
    // silently do nothing — which is the failure mode this whole framework
    // exists to make impossible.
    std::printf ("== choice params dial BY NAME, and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Saturation");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "Saturation IS an EedDeviceProcessor");

        int applied = 0, skipped = 0;
        const auto summary = device->applyStructured (
            paramsMove ({ { "type", "diode" } }), &applied, &skipped);
        check (applied == 1 && skipped == 0, "type = \"diode\" applied");
        check (near (device->getParamValue ("type"), 2.0), "landed on the diode curve (index 2)");
        check (summary.contains ("diode"), "and reads back BY NAME: " + summary);

        device->applyStructured (paramsMove ({ { "type", "TUBE" } }));
        check (near (device->getParamValue ("type"), 0.0), "matching is case-insensitive");

        device->applyStructured (paramsMove ({ { "type", 3 } }));
        check (near (device->getParamValue ("type"), 3.0), "a numeric index still works");

        device->applyStructured (paramsMove ({ { "type", "diode" } }));
        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "type", "germanium" } }), &a2, &s2);
        check (s2 == 1, "an unknown choice is SKIPPED, not guessed at");
        check (near (device->getParamValue ("type"), 2.0), "and leaves the curve where it was");

        // The advertisement has to carry the names, or the model has no way to
        // learn them.
        const auto* spec = EedSaturationProcessor::schema().find ("type");
        const auto line = juce::String (echojay::ParamSchema::describeLine (*spec));
        check (line.contains ("tube|tape|diode|soft"), "advertised as names: " + line);
        check (line.contains ("default tube"), "and its default is a name too");
    }

    // -----------------------------------------------------------------------
    std::printf ("== the Harmonic cluster dials, and reports its latency ==\n");
    {
        auto sat = makeByName ("EchoJay Saturation");
        auto* satDev = dynamic_cast<EedDeviceProcessor*> (sat.get());
        satDev->applyStructured (paramsMove ({ { "drive_db", 18.0 }, { "tone_db", -4.5 },
                                               { "mix", 60.0 }, { "output_db", -2.0 } }));
        check (near (satDev->getParamValue ("drive_db"), 18.0),  "drive_db exact");
        check (near (satDev->getParamValue ("tone_db"), -4.5),   "tone_db exact");
        check (near (satDev->getParamValue ("mix"), 60.0),       "mix exact");
        check (near (satDev->getParamValue ("output_db"), -2.0), "output_db exact");

        sat->prepareToPlay (48000.0, 256);
        check (sat->getLatencySamples() == 45,
               "Saturation reports 45 samples of oversampling latency (got "
               + juce::String (sat->getLatencySamples()) + ")");

        auto tape = makeByName ("EchoJay Tape");
        auto* tapeDev = dynamic_cast<EedDeviceProcessor*> (tape.get());
        tapeDev->applyStructured (paramsMove ({ { "speed_ips", 30.0 }, { "wow", 0.0 },
                                                { "flutter", 55.0 }, { "head_bump_db", 4.0 },
                                                { "bias", -40.0 } }));
        check (near (tapeDev->getParamValue ("speed_ips"), 30.0),    "speed_ips exact");
        check (near (tapeDev->getParamValue ("flutter"), 55.0),      "flutter exact");
        check (near (tapeDev->getParamValue ("head_bump_db"), 4.0),  "head_bump_db exact");
        check (near (tapeDev->getParamValue ("bias"), -40.0),        "bias exact");

        tape->prepareToPlay (48000.0, 256);
        check (tape->getLatencySamples() == 165,
               "Tape reports 165 samples (transport + oversampling) (got "
               + juce::String (tape->getLatencySamples()) + ")");

        auto ex = makeByName ("EchoJay Exciter");
        auto* exDev = dynamic_cast<EedDeviceProcessor*> (ex.get());
        exDev->applyStructured (paramsMove ({ { "freq_hz", 5500.0 }, { "amount", 35.0 },
                                              { "mode", "tape" } }));
        check (near (exDev->getParamValue ("freq_hz"), 5500.0), "freq_hz exact");
        check (near (exDev->getParamValue ("amount"), 35.0),    "amount exact");
        check (near (exDev->getParamValue ("mode"), 1.0),       "mode = \"tape\" resolved");

        ex->prepareToPlay (48000.0, 256);
        check (ex->getLatencySamples() == 45,
               "Exciter reports 45 samples (got " + juce::String (ex->getLatencySamples()) + ")");
    }

    // -----------------------------------------------------------------------
    // A device that produces a NaN or an infinity poisons every plugin after it
    // in the chain, and the symptom (silence, three slots later) points nowhere
    // near the cause. Every device gets audio pushed through it here.
    std::printf ("== every device passes real audio without producing NaN ==\n");
    for (const auto& d : registry.all())
    {
        auto proc = d.create();
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        if (device == nullptr) continue;

        // Everything at its maximum: the most extreme setting the model can ask
        // for is the one most likely to blow up.
        for (const auto& p : d.schema.params())
            device->setParamValue (juce::String (p.id), p.max);

        proc->setPlayConfigDetails (2, 2, 48000.0, 128);
        proc->prepareToPlay (48000.0, 128);

        juce::AudioBuffer<float> buf (2, 128);
        juce::MidiBuffer midi;
        bool clean = true;

        for (int b = 0; b < 32; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 128; ++i)
                    buf.setSample (ch, i, 0.9f * std::sin (0.31f * (float) (b * 128 + i)));

            proc->processBlock (buf, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 128; ++i)
                {
                    const float v = buf.getSample (ch, i);
                    if (! std::isfinite (v) || std::fabs (v) > 64.0f) clean = false;
                }
        }
        check (clean, d.name + ": 32 blocks at every param's maximum stay finite");
    }

    // -----------------------------------------------------------------------
    std::printf ("== state round-trips through the schema ==\n");
    {
        auto a = makeByName ("EchoJay Gain");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "level_db", -12.0 }, { "pan", -0.75 } }));
        da->setBypassed (true);

        juce::MemoryBlock blob;
        da->getStateInformation (blob);

        auto b = makeByName ("EchoJay Gain");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (blob.getData(), (int) blob.getSize());

        check (near (db->getParamValue ("level_db"), -12.0), "level_db restored");
        check (near (db->getParamValue ("pan"), -0.75),      "pan restored");
        check (db->isBypassed(), "bypass restored");
    }

    std::printf ("== restore is a FULL REPLACE, not a merge ==\n");
    {
        auto a = makeByName ("EchoJay Gain");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        juce::MemoryBlock blob;
        da->getStateInformation (blob);          // defaults: 0 dB, centre

        auto b = makeByName ("EchoJay Gain");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->applyStructured (paramsMove ({ { "level_db", -20.0 }, { "pan", 1.0 } }));
        db->setStateInformation (blob.getData(), (int) blob.getSize());

        check (near (db->getParamValue ("level_db"), 0.0), "level_db reset to default");
        check (near (db->getParamValue ("pan"), 0.0),      "pan reset to default");
    }

    // -----------------------------------------------------------------------
    std::printf ("== every device publishes a dialable contract ==\n");
    for (const auto& d : registry.all())
    {
        check (! d.schema.empty(), d.name + " publishes a non-empty ParamSchema");
        check (d.summary.isNotEmpty(), d.name + " tells the model when to reach for it");

        auto proc = d.create();
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, d.name + " derives from EedDeviceProcessor");

        // Every advertised id must actually be settable — the schema is a
        // promise, and an unimplemented id would make it a lie.
        for (const auto& p : d.schema.params())
            check (device->setParamValue (juce::String (p.id), p.def),
                   d.name + ": advertised id \"" + juce::String (p.id) + "\" is settable");
    }

    // -----------------------------------------------------------------------
    // juce::String's const char* constructor reads its input as ASCII, so a UTF-8
    // character in a registry literal (an em-dash is the easy mistake) arrives
    // double-encoded and ships mojibake into the AI prompt. It builds, runs, and
    // only shows up if you read the bytes — so it is pinned here instead.
    std::printf ("== advertised text is pure ASCII (no mojibake in the AI feed) ==\n");
    {
        auto isAscii = [] (const juce::String& s)
        {
            for (auto c : s) if (c > 127) return false;
            return true;
        };

        for (const auto& d : registry.all())
        {
            check (isAscii (d.name),            d.name + ": name is ASCII");
            check (isAscii (d.summary),         d.name + ": summary is ASCII");
            check (isAscii (d.descriptiveName), d.name + ": descriptiveName is ASCII");

            for (const auto& p : d.schema.params())
                check (isAscii (juce::String (echojay::ParamSchema::describeLine (p))),
                       d.name + ": \"" + juce::String (p.id) + "\" advertisement is ASCII");
        }
    }

    // -----------------------------------------------------------------------
    std::printf ("\n---- the [AVAILABLE BUILTINS] advertisement, as the model sees it ----\n");
    {
        juce::String currentCategory;
        for (const auto& d : registry.all())
        {
            if (d.category != currentCategory)
            {
                currentCategory = d.category;
                std::printf ("\n-- %s --\n", currentCategory.toRawUTF8());
            }
            std::printf ("%s\n  %s\n", d.name.toRawUTF8(), d.summary.toRawUTF8());
            for (const auto& p : d.schema.params())
                std::printf ("    %s\n", echojay::ParamSchema::describeLine (p).c_str());
        }
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
