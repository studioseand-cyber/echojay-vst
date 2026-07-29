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
#include "EedAutoPanProcessor.h"
#include "EedChorusProcessor.h"
#include "EedGainProcessor.h"
#include "EedPhaseInvertProcessor.h"
#include "EedPhaserProcessor.h"
#include "EedTremoloProcessor.h"
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
    check (registry.findByName ("EchoJay Tremolo")       != nullptr, "EchoJay Tremolo registered");
    check (registry.findByName ("EchoJay Auto Pan")      != nullptr, "EchoJay Auto Pan registered");
    check (registry.findByName ("EchoJay Chorus")        != nullptr, "EchoJay Chorus registered");
    check (registry.findByName ("EchoJay Phaser")        != nullptr, "EchoJay Phaser registered");
    check (registry.all().size() == 7, "exactly 7 devices registered (got "
                                       + juce::String ((int) registry.all().size()) + ")");

    // -----------------------------------------------------------------------
    std::printf ("== ordering is deterministic, not static-init order ==\n");
    {
        const auto names = registry.names();
        check (names[0] == "EchoJay EQ", "EQ sorts first (category rank)");
        // Utility comes after EQ; within it, alphabetical.
        check (names.indexOf ("EchoJay Gain") < names.indexOf ("EchoJay Phase Invert"),
               "Gain before Phase Invert (alphabetical within Utility)");
        check (registry.categories().joinIntoString (",") == "EQ,Utility,Modulation",
               "categories in canonical order: " + registry.categories().joinIntoString (","));

        // Within Modulation, alphabetical: Auto Pan, Chorus, Phaser, Tremolo.
        check (names.indexOf ("EchoJay Auto Pan") < names.indexOf ("EchoJay Chorus")
            && names.indexOf ("EchoJay Chorus")   < names.indexOf ("EchoJay Phaser")
            && names.indexOf ("EchoJay Phaser")   < names.indexOf ("EchoJay Tremolo"),
               "Modulation devices sort alphabetically among themselves");
        check (names.indexOf ("EchoJay Gain") < names.indexOf ("EchoJay Auto Pan"),
               "Utility ranks before Modulation");
    }

    // -----------------------------------------------------------------------
    std::printf ("== name resolution is tolerant, and honours aliases ==\n");
    check (registry.findByName ("echojay gain")   != nullptr, "case-insensitive");
    check (registry.findByName ("EchoJayGain")    != nullptr, "alias without the space");
    check (registry.findByName ("EchoJay Trim")   != nullptr, "declared alias");
    check (registry.findByName ("EchoJay Polarity") != nullptr, "phase-invert alias");
    check (registry.findByName ("Pro-Q 3")        == nullptr, "a third-party name is NOT ours");
    check (registry.findByName ("AutoPan")        != nullptr, "auto pan without the space");
    check (registry.findByName ("Tremolo")        != nullptr, "bare \"Tremolo\" resolves");
    check (registry.findByName ("echojay phaser") != nullptr, "phaser, case-insensitive");

    // -----------------------------------------------------------------------
    std::printf ("== synthetic descriptions (what saved chain XML carries) ==\n");
    {
        const auto descs = registry.descriptions();
        check (descs.size() == registry.all().size(), "one description per device");
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
    std::printf ("== identities are unique (a clash would make restore ambiguous) ==\n");
    {
        bool clash = false;
        const auto& all = registry.all();
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j)
                if (all[i].uid == all[j].uid || all[i].identifier == all[j].identifier)
                    clash = true;
        check (! clash, "no two devices share a uid or an identifier");
    }

    // -----------------------------------------------------------------------
    std::printf ("== MODULATION: each device dials exactly from params ==\n");
    {
        auto proc = makeByName ("EchoJay Tremolo");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "Tremolo constructs as a device");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "rate_hz", 6.5 },
                                               { "depth", 80.0 },
                                               { "stereo_phase", 180.0 },
                                               { "mix", 75.0 } }), &applied, &skipped);
        check (applied == 4 && skipped == 0, "Tremolo: 4 params applied, 0 skipped");
        check (near (device->getParamValue ("rate_hz"), 6.5),        "rate_hz landed at 6.5");
        check (near (device->getParamValue ("depth"), 80.0),         "depth landed at 80");
        check (near (device->getParamValue ("stereo_phase"), 180.0), "stereo_phase landed at 180");
        check (near (device->getParamValue ("mix"), 75.0),           "mix landed at 75");

        // A discrete param is ROUNDED, not truncated: shape 1.6 is a square, and
        // an off-by-one here would silently hand the user the wrong waveform.
        device->applyStructured (paramsMove ({ { "shape", 1.6 } }));
        check (near (device->getParamValue ("shape"), 2.0), "shape 1.6 rounds to 2 (square)");

        // The tempo-sync interlock, as the model would send it.
        device->applyStructured (paramsMove ({ { "sync", true }, { "sync_division", 8.0 } }));
        check (near (device->getParamValue ("sync"), 1.0),          "sync on");
        check (near (device->getParamValue ("sync_division"), 8.0), "division landed at 1/8");
    }

    {
        auto proc = makeByName ("EchoJay Auto Pan");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        int applied = 0, skipped = 0;
        const auto summary = device->applyStructured (
            paramsMove ({ { "depth", 100.0 }, { "mix", 50.0 } }), &applied, &skipped);

        check (near (device->getParamValue ("depth"), 100.0), "Auto Pan: depth landed at 100");
        // Auto Pan publishes no MIX on purpose (panning is a placement, not a
        // parallel effect), so the id must be REFUSED rather than absorbed.
        check (skipped == 1 && summary.contains ("mix"),
               "Auto Pan: an unpublished \"mix\" is refused, not swallowed");
    }

    {
        auto proc = makeByName ("EchoJay Chorus");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "delay_ms", 25.0 },
                                               { "voices", "3" },        // string from JSON
                                               { "feedback", -50.0 },
                                               { "mix", 100.0 } }));
        check (near (device->getParamValue ("delay_ms"), 25.0),  "Chorus: delay_ms landed at 25");
        check (near (device->getParamValue ("voices"), 3.0),     "Chorus: the string \"3\" -> 3 voices");
        check (near (device->getParamValue ("feedback"), -50.0), "Chorus: negative feedback landed");

        // Out of range in both directions, clamped to the advertised limits.
        device->applyStructured (paramsMove ({ { "voices", 99.0 }, { "delay_ms", 0.0 } }));
        check (near (device->getParamValue ("voices"), 4.0),    "Chorus: 99 voices clamped to 4");
        check (near (device->getParamValue ("delay_ms"), 1.0),  "Chorus: 0 ms clamped to the 1 ms floor");
    }

    {
        auto proc = makeByName ("EchoJay Phaser");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "center_freq", 2000.0 },
                                               { "stages", 12.0 },
                                               { "feedback", 80.0 } }));
        check (near (device->getParamValue ("center_freq"), 2000.0), "Phaser: center_freq landed at 2 kHz");
        check (near (device->getParamValue ("stages"), 12.0),        "Phaser: 12 stages landed");
        check (near (device->getParamValue ("feedback"), 80.0),      "Phaser: feedback landed at 80");

        // "centerFreq" / "center-freq" are the same id: a near-miss spelling has
        // to land, or the move is a silent no-op with nothing to see.
        device->applyStructured (paramsMove ({ { "centerFreq", 400.0 } }));
        check (near (device->getParamValue ("center_freq"), 400.0),
               "Phaser: \"centerFreq\" resolves to the same param");
    }

    // -----------------------------------------------------------------------
    std::printf ("== MODULATION: a device at its defaults is PASS-THROUGH ==\n");
    {
        // Belt and braces on the DSP side of the contract: adding one of these
        // to a chain and touching nothing must not change the audio. The engines'
        // own g++ test covers the maths; this covers the wiring in between.
        for (const char* name : { "EchoJay Tremolo", "EchoJay Auto Pan",
                                  "EchoJay Chorus",  "EchoJay Phaser" })
        {
            auto proc = makeByName (name);
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            device->resetParamsToDefaults();

            // Chorus and Phaser are wet by default, so mute the wet path: what is
            // under test here is that the dry path arrives intact.
            device->applyStructured (paramsMove ({ { "mix", 0.0 } }));
            device->applyStructured (paramsMove ({ { "depth", 0.0 } }));

            proc->prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buf (2, 512);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, std::sin ((float) i * 0.05f));

            juce::AudioBuffer<float> before (buf);
            juce::MidiBuffer midi;
            proc->processBlock (buf, midi);

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    worst = juce::jmax (worst, std::abs (buf.getSample (ch, i)
                                                       - before.getSample (ch, i)));
            check (worst < 1.0e-4f,
                   juce::String (name) + ": dry path is intact (worst delta "
                   + juce::String (worst, 7) + ")");
        }
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
