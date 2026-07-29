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
#include "EedGainProcessor.h"
#include "EedPhaseInvertProcessor.h"
#include "EedStereoWidthProcessor.h"
#include "EedStereoizerProcessor.h"
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
    check (registry.findByName ("EchoJay Stereo Width")  != nullptr, "EchoJay Stereo Width registered");
    check (registry.findByName ("EchoJay Stereoizer")    != nullptr, "EchoJay Stereoizer registered");

    // Deliberately NOT an exact count: every Wave 1 cluster adds devices, and a
    // hard-coded total would make each of those a false failure here. What
    // actually has to hold is that no two devices claim the same identity —
    // registry.add() refuses a duplicate, so a collision silently DROPS a device
    // rather than erroring, and only a check like this would notice.
    std::printf ("  (%d devices registered)\n", (int) registry.all().size());
    {
        juce::StringArray ids;
        juce::Array<int> uids;
        bool unique = true;
        for (const auto& d : registry.all())
        {
            if (ids.contains (d.identifier) || uids.contains (d.uid)) unique = false;
            ids.add (d.identifier);
            uids.add (d.uid);
        }
        check (unique, "every device has a unique identifier and uid");
    }

    // -----------------------------------------------------------------------
    std::printf ("== ordering is deterministic, not static-init order ==\n");
    {
        const auto names = registry.names();
        check (names[0] == "EchoJay EQ", "EQ sorts first (category rank)");
        // Utility comes after EQ; within it, alphabetical.
        check (names.indexOf ("EchoJay Gain") < names.indexOf ("EchoJay Phase Invert"),
               "Gain before Phase Invert (alphabetical within Utility)");
        // Categories come out in the registry's canonical rank order, not
        // alphabetically and not in static-init order.
        const auto cats = registry.categories();
        check (cats.indexOf ("EQ") == 0, "EQ is the first category");
        check (cats.indexOf ("Utility") < cats.indexOf ("Stereo"),
               "Utility ranks before Stereo: " + cats.joinIntoString (","));
        check (names.indexOf ("EchoJay Stereo Width") < names.indexOf ("EchoJay Stereoizer"),
               "Stereo Width before Stereoizer (alphabetical within Stereo)");
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
        check (descs.size() == (int) registry.all().size(), "one description per device");
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
    std::printf ("== the Stereo cluster dials, and its defaults land ==\n");
    {
        auto proc = makeByName ("EchoJay Stereo Width");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "Stereo Width is an EedDeviceProcessor");

        // Fresh device: the SCHEMA's defaults, not the engine's idle values.
        check (near (device->getParamValue ("width"), 100.0),         "width defaults to 100%");
        check (near (device->getParamValue ("bass_mono_hz"), 0.0),    "bass mono defaults to off");
        check (near (device->getParamValue ("output_trim_db"), 0.0),  "trim defaults to 0 dB");

        int applied = 0, skipped = 0;
        const auto summary = device->applyStructured (
            paramsMove ({ { "width", 140.0 }, { "bass_mono_hz", 120.0 },
                          { "output_trim_db", -1.5 } }), &applied, &skipped);

        check (applied == 3 && skipped == 0, "3 params applied, 0 skipped");
        check (summary.isNotEmpty(), "summary: " + summary);
        check (near (device->getParamValue ("width"), 140.0),        "width landed EXACTLY at 140");
        check (near (device->getParamValue ("bass_mono_hz"), 120.0), "bass_mono_hz landed EXACTLY at 120");
        check (near (device->getParamValue ("output_trim_db"), -1.5),"output_trim_db landed EXACTLY at -1.5");
    }
    {
        auto proc = makeByName ("EchoJay Stereoizer");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        // This device's defaults are a WORKING widener, not a bypass, so the
        // schema has to win over the engine's idle values on construction.
        check (near (device->getParamValue ("width"), 120.0),         "width defaults to 120%");
        check (near (device->getParamValue ("haas_ms"), 15.0),        "haas defaults to 15 ms");
        check (near (device->getParamValue ("mono_maker_hz"), 150.0), "mono maker defaults to 150 Hz");
        check (near (device->getParamValue ("mix"), 100.0),           "mix defaults to 100%");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "haas_ms", 22.0 }, { "mix", 60.0 } }),
                                 &applied, &skipped);
        check (applied == 2 && skipped == 0, "a partial move applies both params");
        check (near (device->getParamValue ("haas_ms"), 22.0), "haas_ms landed EXACTLY at 22");
        check (near (device->getParamValue ("width"), 120.0),  "width SURVIVED (merge semantics)");
    }

    // ParamSchema::find is deliberately tolerant of case and separators, so a
    // near-miss id lands rather than silently doing nothing. That tolerance has
    // to hold all the way to the KNOB: applyParams dispatches on the schema's
    // spelling rather than the one that arrived, or a well-formed move would be
    // matched, clamped, and then reported "not implemented".
    std::printf ("== a near-miss param id still reaches the knob ==\n");
    {
        auto proc = makeByName ("EchoJay Stereo Width");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "Width", 75.0 },
                                              { "bassMonoHz", 90.0 } }), &applied, &skipped);
        check (applied == 2 && skipped == 0, "\"Width\" and \"bassMonoHz\" both applied");
        check (near (device->getParamValue ("width"), 75.0),        "capitalised id landed");
        check (near (device->getParamValue ("bass_mono_hz"), 90.0), "camelCase id landed");
    }

    // The device's headline claim, checked through the REAL processBlock rather
    // than at the engine level: whatever it is set to, folding the output down
    // to mono gives back the input's fold-down. A widener that combs when summed
    // is worse than no widener, and this is the thing a later edit could break
    // without any other test noticing.
    std::printf ("== the Stereoizer's mono sum survives a real processBlock ==\n");
    {
        auto proc = makeByName ("EchoJay Stereoizer");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        device->applyStructured (paramsMove ({ { "width", 200.0 }, { "haas_ms", 30.0 },
                                              { "mono_maker_hz", 200.0 }, { "mix", 75.0 } }));

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buf (2, 4096);
        juce::Random rng (1234);
        std::vector<float> sumIn (4096);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const float l = rng.nextFloat() * 2.0f - 1.0f;
            const float r = rng.nextFloat() * 2.0f - 1.0f;
            buf.setSample (0, i, l);
            buf.setSample (1, i, r);
            sumIn[(std::size_t) i] = l + r;
        }

        juce::MidiBuffer midi;
        for (int i = 0; i < buf.getNumSamples(); i += 512)
        {
            juce::AudioBuffer<float> slice (buf.getArrayOfWritePointers(), 2, i, 512);
            proc->processBlock (slice, midi);
        }

        float worst = 0.0f;
        for (int i = 0; i < buf.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs ((buf.getSample (0, i) + buf.getSample (1, i))
                                                 - sumIn[(std::size_t) i]));
        check (worst <= 1.0e-5f, "L+R is unchanged end to end (worst dev "
                                 + juce::String (worst, 9) + ")");
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
    // The editors are hosted INLINE in the chain rack, which sizes them to
    // whatever space it has rather than to the default they ask for. So the
    // thing worth checking is not that they look right (they cannot be seen from
    // here) but that constructing one and squeezing it does not assert or leave
    // a control outside its parent -- the two ways an inline editor breaks the
    // rack rather than just itself.
    std::printf ("== every device's editor builds and survives being squeezed ==\n");
    for (const auto& d : registry.all())
    {
        auto proc = d.create();
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        check (ed != nullptr, d.name + ": createEditor returns an editor");
        if (ed == nullptr) continue;

        check (ed->getWidth() > 0 && ed->getHeight() > 0,
               d.name + ": opens at a real default size ("
               + juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) + ")");

        bool escaped = false;
        for (int w : { 240, 320, 640, 1200 })
        {
            ed->setSize (w, 110);
            for (int i = 0; i < ed->getNumChildComponents(); ++i)
            {
                auto* c = ed->getChildComponent (i);
                if (c != nullptr && c->isVisible()
                    && ! ed->getLocalBounds().contains (c->getBounds()))
                    escaped = true;
            }
        }
        check (! escaped, d.name + ": no control escapes the editor at any width");
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
