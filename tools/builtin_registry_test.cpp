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
#include "EedDelayProcessor.h"
#include "EedExciterProcessor.h"
#include "EedGainProcessor.h"
#include "EedPhaseInvertProcessor.h"
#include "EedPhaserProcessor.h"
#include "EedReverbProcessor.h"
#include "EedSaturationProcessor.h"
#include "EedStereoWidthProcessor.h"
#include "EedStereoizerProcessor.h"
#include "EedTapeProcessor.h"
#include "EedTremoloProcessor.h"
#include "EedMultibandProcessor.h"
#include "SurgicalEqProcessor.h"

#include <cmath>
#include <memory>

// EQ + Gain + Phase Invert (Wave 0) + the six Dynamics faces (Wave 1). A count
// rather than a >= so that a device silently failing to register is a FAILURE
// and not a test that quietly still passes.
static constexpr int kExpectedDevices = 20;

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
    check (registry.findByName ("EchoJay Delay")         != nullptr, "EchoJay Delay registered");
    check (registry.findByName ("EchoJay Reverb")        != nullptr, "EchoJay Reverb registered");
    check (registry.findByName ("EchoJay Tremolo")       != nullptr, "EchoJay Tremolo registered");
    check (registry.findByName ("EchoJay Auto Pan")      != nullptr, "EchoJay Auto Pan registered");
    check (registry.findByName ("EchoJay Chorus")        != nullptr, "EchoJay Chorus registered");
    check (registry.findByName ("EchoJay Phaser")        != nullptr, "EchoJay Phaser registered");
    check (registry.findByName ("EchoJay Stereo Width")  != nullptr, "EchoJay Stereo Width registered");
    check (registry.findByName ("EchoJay Stereoizer")    != nullptr, "EchoJay Stereoizer registered");
    // An EXACT count, not a lower bound: the failure this catches is a device
    // silently disappearing, and ">= 14" would not notice that. Every Wave 1
    // session bumps this by the number of devices it lands.

    // Wave 1, Dynamics cluster.
    check (registry.findByName ("EchoJay Compressor")        != nullptr, "EchoJay Compressor registered");
    check (registry.findByName ("EchoJay Gate")              != nullptr, "EchoJay Gate registered");
    check (registry.findByName ("EchoJay Expander")          != nullptr, "EchoJay Expander registered");
    check (registry.findByName ("EchoJay Limiter")           != nullptr, "EchoJay Limiter registered");
    check (registry.findByName ("EchoJay De-Esser")          != nullptr, "EchoJay De-Esser registered");
    check (registry.findByName ("EchoJay 4-Band Compressor") != nullptr, "EchoJay 4-Band Compressor registered");

    check (registry.all().size() == kExpectedDevices,
           "exactly " + juce::String (kExpectedDevices) + " devices registered (got "
           + juce::String ((int) registry.all().size()) + ")");

    // -----------------------------------------------------------------------
    std::printf ("== ordering is deterministic, not static-init order ==\n");
    {
        const auto names = registry.names();
        check (names[0] == "EchoJay EQ", "EQ sorts first (category rank)");
        // Utility comes after EQ; within it, alphabetical.
        check (names.indexOf ("EchoJay Gain") < names.indexOf ("EchoJay Phase Invert"),
               "Gain before Phase Invert (alphabetical within Utility)");
        // Harmonic ranks after Modulation, Time after Harmonic, and each sorts
        // alphabetically inside itself.
        check (names.indexOf ("EchoJay Exciter") < names.indexOf ("EchoJay Saturation")
            && names.indexOf ("EchoJay Saturation") < names.indexOf ("EchoJay Tape"),
               "Exciter, Saturation, Tape (alphabetical within Harmonic)");
        check (names.indexOf ("EchoJay Delay") < names.indexOf ("EchoJay Reverb"),
               "Delay before Reverb (alphabetical within Time)");
        check (names.indexOf ("EchoJay Phase Invert") < names.indexOf ("EchoJay Auto Pan"),
               "the whole Utility group precedes the whole Modulation group");
        check (names.indexOf ("EchoJay Tremolo") < names.indexOf ("EchoJay Exciter"),
               "the whole Modulation group precedes the whole Harmonic group");
        check (names.indexOf ("EchoJay Tape") < names.indexOf ("EchoJay Delay"),
               "the whole Harmonic group precedes the whole Time group");
        // Dynamics ranks above Utility, so every Dynamics device sorts before
        // Gain even though "Compressor" > "Gain" alphabetically. That is the
        // category rank doing its job rather than a coincidence of names.
        check (names.indexOf ("EchoJay Compressor") < names.indexOf ("EchoJay Gain"),
               "Dynamics sorts before Utility (category rank, not alphabetical)");
        check (names.indexOf ("EchoJay 4-Band Compressor") < names.indexOf ("EchoJay Compressor"),
               "within Dynamics, alphabetical: 4-Band before Compressor");

        check (registry.categories().joinIntoString (",") == "EQ,Dynamics,Utility,Stereo,Modulation,Harmonic,Time",
               "categories in canonical order: " + registry.categories().joinIntoString (","));

        // Within Modulation, alphabetical: Auto Pan, Chorus, Phaser, Tremolo.
        check (names.indexOf ("EchoJay Auto Pan") < names.indexOf ("EchoJay Chorus")
            && names.indexOf ("EchoJay Chorus")   < names.indexOf ("EchoJay Phaser")
            && names.indexOf ("EchoJay Phaser")   < names.indexOf ("EchoJay Tremolo"),
               "Modulation devices sort alphabetically among themselves");
        check (names.indexOf ("EchoJay Gain") < names.indexOf ("EchoJay Auto Pan"),
               "Utility ranks before Modulation");

        // Stereo ranks between Utility and Modulation, and sorts alphabetically
        // inside itself.
        check (names.indexOf ("EchoJay Stereo Width") < names.indexOf ("EchoJay Stereoizer"),
               "Stereo Width before Stereoizer (alphabetical within Stereo)");
        check (names.indexOf ("EchoJay Phase Invert") < names.indexOf ("EchoJay Stereo Width"),
               "the whole Utility group precedes the whole Stereo group");
        check (names.indexOf ("EchoJay Stereoizer") < names.indexOf ("EchoJay Auto Pan"),
               "the whole Stereo group precedes the whole Modulation group");
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
    // The Time cluster is where a device first has params of every SHAPE at
    // once: continuous (time_ms), boolean (sync, ping_pong), an enumerated index
    // (sync_division) and a signed range (stereo_offset). If the universal
    // contract holds for this device it holds for anything Wave 1 adds.
    std::printf ("== EchoJay Delay dials every shape of param exactly ==\n");
    {
        auto proc = makeByName ("EchoJay Delay");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "IS an EedDeviceProcessor (so ChainHost can dispatch)");

        int applied = 0, skipped = 0;
        const auto summary = device->applyStructured (
            paramsMove ({ { "time_ms", 375.0 }, { "feedback", 62.0 }, { "mix", 45.0 },
                          { "ping_pong", true }, { "stereo_offset", -25.0 },
                          { "filter_lp_hz", 3200.0 } }), &applied, &skipped);

        check (applied == 6 && skipped == 0, "6 params applied, 0 skipped");
        check (summary.isNotEmpty(), "summary: " + summary);
        check (near (device->getParamValue ("time_ms"), 375.0),       "time_ms EXACTLY 375");
        check (near (device->getParamValue ("feedback"), 62.0),       "feedback EXACTLY 62");
        check (near (device->getParamValue ("mix"), 45.0),            "mix EXACTLY 45");
        check (near (device->getParamValue ("ping_pong"), 1.0),       "ping_pong is on");
        check (near (device->getParamValue ("stereo_offset"), -25.0), "a NEGATIVE value lands");
        check (near (device->getParamValue ("filter_lp_hz"), 3200.0), "filter_lp_hz EXACTLY 3200");
    }

    std::printf ("== the tempo-sync division is an exact index, not an approximation ==\n");
    {
        auto proc = makeByName ("EchoJay Delay");
        auto* delay  = dynamic_cast<EedDelayProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (delay != nullptr, "constructed as an EedDelayProcessor");

        echojay::publishHostTempo (120.0);
        device->applyStructured (paramsMove ({ { "sync", true }, { "sync_division", 9 } }));

        check (near (device->getParamValue ("sync_division"), 9.0), "landed on index 9");
        check (juce::String (echojay::DelayEngine::divisionName (9)) == "1/4",
               "index 9 is a quarter note");
        check (near (delay->engine().effectiveTimeMs(), 500.0, 1e-6),
               "which at 120 BPM is exactly 500 ms");

        // Out of range must clamp to a real division, not wrap or land nowhere.
        device->applyStructured (paramsMove ({ { "sync_division", 99 } }));
        check (near (device->getParamValue ("sync_division"),
                     (double) (echojay::DelayEngine::kNumDivisions - 1)),
               "an out-of-range index clamps to the longest note");
    }

    std::printf ("== EchoJay Reverb dials, and reports no latency ==\n");
    {
        auto proc = makeByName ("EchoJay Reverb");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "IS an EedDeviceProcessor");

        int applied = 0, skipped = 0;
        device->applyStructured (
            paramsMove ({ { "size", 85.0 }, { "decay_s", 4.5 }, { "predelay_ms", 60.0 },
                          { "damping", 30.0 }, { "width", 80.0 }, { "mix", 22.0 } }),
            &applied, &skipped);

        check (applied == 6 && skipped == 0, "6 params applied, 0 skipped");
        check (near (device->getParamValue ("size"), 85.0),        "size EXACTLY 85");
        check (near (device->getParamValue ("decay_s"), 4.5),      "decay_s EXACTLY 4.5");
        check (near (device->getParamValue ("predelay_ms"), 60.0), "predelay_ms EXACTLY 60");
        check (near (device->getParamValue ("damping"), 30.0),     "damping EXACTLY 30");
        check (near (device->getParamValue ("width"), 80.0),       "width EXACTLY 80");
        check (near (device->getParamValue ("mix"), 22.0),         "mix EXACTLY 22");

        // The claim the summary makes to the model, checked against the device.
        proc->prepareToPlay (48000.0, 512);
        check (proc->getLatencySamples() == 0, "reports ZERO latency, as advertised");
        check (proc->getTailLengthSeconds() > 1.0, "but DOES report a tail ("
               + juce::String (proc->getTailLengthSeconds(), 2) + " s), so an offline "
               "render will not truncate it");
    }

    std::printf ("== both Time devices survive a real prepare + process ==\n");
    {
        // Not a DSP test (the engines have g++ suites) — a wiring test: the
        // buffer layout the chain actually hands a device, at a rate it did not
        // expect, with a block size that changes underneath it.
        for (const char* name : { "EchoJay Delay", "EchoJay Reverb" })
        {
            auto proc = makeByName (name);
            proc->setPlayConfigDetails (2, 2, 44100.0, 512);
            proc->prepareToPlay (44100.0, 512);

            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            bool finite = true;

            for (int blk = 0; blk < 40; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        buf.setSample (ch, i, 0.5f * std::sin ((blk * 512 + i) * 0.02f));

                proc->processBlock (buf, midi);

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        if (! std::isfinite (buf.getSample (ch, i))) finite = false;
            }
            check (finite, juce::String (name) + " renders finite audio through the chain's layout");

            // Mono, which a device has to survive to be auditionable standalone.
            proc->setPlayConfigDetails (1, 1, 44100.0, 256);
            proc->prepareToPlay (44100.0, 256);
            juce::AudioBuffer<float> mono (1, 256);
            mono.clear();
            mono.setSample (0, 0, 1.0f);
            proc->processBlock (mono, midi);
            check (std::isfinite (mono.getSample (0, 100)),
                   juce::String (name) + " survives mono");
        }
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
    // Wave 0 refactored every device editor onto DeviceEditorBase, and a fault in
    // that SHARED path takes out all 19 devices at once — which is exactly what
    // happened: the base constructor called setSize(), JUCE dispatched resized()
    // synchronously while the subclass was still unbuilt, and the base's
    // resized() called the pure virtual layoutContent(). Every editor aborted on
    // open, in Debug and Release alike.
    //
    // So the editors are constructed AND painted here, for every registered
    // device, the same way the mojibake check pins the advertised strings: a
    // crash in the shared shell now fails a test instead of a DAW.
    std::printf ("== every device's editor constructs, lays out and PAINTS ==\n");
    for (const auto& d : registry.all())
    {
        auto proc = d.create();
        if (proc == nullptr) { check (false, d.name + ": no processor"); continue; }

        check (proc->hasEditor(), d.name + " claims an editor");

        // createEditor() returns a raw pointer the caller owns.
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        check (ed != nullptr, d.name + " editor constructs");
        if (ed == nullptr) continue;

        // The editor declares its own default size in its constructor — that is
        // what the rack opens it at, so that is what gets painted.
        //
        // Asserted as a RANGE, deliberately, not as a number. Every category is
        // growing its default height to seat a visualisation over its dials
        // (VISUALS_PLAN.md), and five parallel sessions each editing one
        // hard-coded size in a shared test is a merge conflict per device for
        // no information gained: the exact pixel count is a design decision,
        // not a contract. What IS a contract is that the size is sane — a
        // device that opens at 40x20 or at 4000x3000 has a real bug (a
        // metric read before it was set, an accumulated layout) and the rack
        // will lay it out unusably either way.
        const int w = juce::jmax (1, ed->getWidth());
        const int h = juce::jmax (1, ed->getHeight());

        constexpr int kMinW = 240, kMaxW = 1400;
        constexpr int kMinH = 100, kMaxH =  900;

        check (w >= kMinW && w <= kMaxW && h >= kMinH && h <= kMaxH,
               d.name + " declares a sane default size (" + juce::String (w) + "x"
                      + juce::String (h) + ", allowed "
                      + juce::String (kMinW) + "-" + juce::String (kMaxW) + " x "
                      + juce::String (kMinH) + "-" + juce::String (kMaxH) + ")");

        auto paintInto = [&ed] (int pw, int ph)
        {
            ed->setSize (pw, ph);
            juce::Image img (juce::Image::ARGB, pw, ph, true);
            juce::Graphics g (img);
            ed->paintEntireComponent (g, true);
        };

        paintInto (w, h);
        check (true, d.name + " painted at its default size");

        // Inline hosting: the rack lays an editor out SMALLER than its default.
        // DeviceEditorBase's clamp is what keeps that from producing a negative
        // content rect, so it is exercised rather than assumed.
        paintInto (juce::jmax (1, w / 3), juce::jmax (1, h / 3));
        check (true, d.name + " painted at one third of its default size");

        // And degenerate, which is what a collapsed rack slot hands it.
        paintInto (1, 1);
        check (true, d.name + " painted at 1x1 without escaping its bounds");
    }

    // -----------------------------------------------------------------------
    // The block above opens an editor STANDALONE, which is not how the rack opens
    // one. ChainListPanel::showInline gives it a PARENT first, then sizes it:
    //
    //     inlineHolder.addAndMakeVisible (*inlineEditor);   // -> parentHierarchyChanged()
    //     layoutInline();                                   // -> setBounds() -> resized()
    //
    // and closeInline() detaches it from the holder before destroying it, so a
    // device gets opened, closed and reopened over a session.
    //
    // That distinction is not cosmetic, and missing it is why a crash that this
    // very test was written to catch still reached a DAW. DeviceEditorBase has to
    // defer its first layout out of the constructor — it cannot dispatch into a
    // subclass that is not built yet — and it flushes that deferred layout from
    // parentHierarchyChanged(). Nothing above ever gives an editor a parent, so
    // the one hook the fix depends on was the one hook the test could not reach.
    //
    // So the rack's sequence is run here verbatim, against a real parent, twice —
    // the second pass being the reopen that a deferred-layout flag left stale, or
    // a dangling look-and-feel, would fail on.
    std::printf ("== every device's editor survives the rack's inline open/close/reopen ==\n");
    for (const auto& d : registry.all())
    {
        auto proc = d.create();
        if (proc == nullptr) { check (false, d.name + ": no processor"); continue; }

        // Stands in for ChainListPanel::inlineHolder — an ordinary parent, which
        // is all the built-in path uses (no NativeClip, no foreign NSView).
        juce::Component holder;
        holder.setSize (900, 600);

        bool ok = true;

        for (int pass = 1; pass <= 2 && ok; ++pass)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
            if (ed == nullptr) { ok = false; break; }

            // showInline(): parent FIRST — this is the parentHierarchyChanged()
            // that flushes the layout the constructor had to skip — then fill the
            // display area, as layoutInline() does.
            holder.addAndMakeVisible (*ed);
            ed->setBounds (holder.getLocalBounds());

            juce::Image img (juce::Image::ARGB, holder.getWidth(), holder.getHeight(), true);
            juce::Graphics g (img);
            ed->paintEntireComponent (g, true);

            // closeInline(): detach, then destroy. Same order as the rack, so a
            // teardown that reaches back into a parent it no longer has fails here.
            holder.removeChildComponent (ed.get());
            ed.reset();
        }

        check (ok, d.name + " opens, closes and reopens inline under a parent");
    }

    std::printf ("== a Dynamics face dials in real units, and clamps to its schema ==\n");
    {
        auto proc = makeByName ("EchoJay Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (device != nullptr, "compressor IS an EedDeviceProcessor");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (
            paramsMove ({ { "threshold_db", -24.0 }, { "ratio", 6.0 },
                          { "attack_ms", 3.0 }, { "mix", 50.0 } }), &applied, &skipped);

        check (applied == 4 && skipped == 0, "4 params applied, 0 skipped");
        check (s.isNotEmpty(), "summary: " + s);
        check (near (device->getParamValue ("threshold_db"), -24.0), "threshold_db exact");
        check (near (device->getParamValue ("ratio"), 6.0),          "ratio exact");
        check (near (device->getParamValue ("attack_ms"), 3.0),      "attack_ms exact");
        check (near (device->getParamValue ("mix"), 50.0),           "mix exact (percent, not 0..1)");

        // Merge: release was never mentioned, so it must still be its default.
        check (near (device->getParamValue ("release_ms"), 120.0),
               "release_ms untouched by a move that did not mention it");

        // Clamp: a ratio of 100:1 is not in the contract.
        device->applyStructured (paramsMove ({ { "ratio", 100.0 } }));
        check (near (device->getParamValue ("ratio"), 20.0), "ratio clamped to the advertised 20 max");
    }

    std::printf ("== the limiter REPORTS its lookahead latency ==\n");
    {
        auto proc = makeByName ("EchoJay Limiter");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        proc->prepareToPlay (48000.0, 512);
        device->applyStructured (paramsMove ({ { "lookahead_ms", 5.0 } }));

        // 5 ms at 48 kHz. An unreported delay puts this track out of time with
        // the whole session, so the number itself is the feature.
        check (proc->getLatencySamples() == 240,
               "5 ms lookahead at 48k reports 240 samples (got "
               + juce::String (proc->getLatencySamples()) + ")");

        device->applyStructured (paramsMove ({ { "lookahead_ms", 0.0 } }));
        check (proc->getLatencySamples() == 0, "zero lookahead reports zero latency");
    }

    std::printf ("== the de-esser's switches dial as on/off, in every spelling ==\n");
    {
        auto proc = makeByName ("EchoJay De-Esser");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "mode", false } }));
        check (near (device->getParamValue ("mode"), 0.0), "JSON false -> wide");

        device->applyStructured (paramsMove ({ { "mode", "on" } }));
        check (near (device->getParamValue ("mode"), 1.0), "the string \"on\" -> split");

        device->applyStructured (paramsMove ({ { "listen", 1 } }));
        check (near (device->getParamValue ("listen"), 1.0), "1 -> listen on");
    }

    // -----------------------------------------------------------------------
    std::printf ("== 4-Band: comp_bands and flat params reach the SAME knobs ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* mb     = dynamic_cast<EedMultibandProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        check (mb != nullptr && device != nullptr, "4-Band is both itself and a device");

        // The array form, with an explicit band.
        juce::DynamicObject::Ptr b4 = new juce::DynamicObject();
        b4->setProperty ("band", 4);
        b4->setProperty ("threshold_db", -24.0);
        b4->setProperty ("ratio", 5.0);

        juce::Array<juce::var> bands;
        bands.add (juce::var (b4.get()));

        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("comp_bands", juce::var (bands));

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (juce::var (root.get()), &applied, &skipped);
        check (applied == 2, "comp_bands applied 2 params (got " + juce::String (applied) + ")");
        check (s.isNotEmpty(), "summary: " + s);
        check (near (device->getParamValue ("band4_threshold_db"), -24.0),
               "band 4 threshold landed exactly");
        check (near (device->getParamValue ("band4_ratio"), 5.0), "band 4 ratio landed exactly");

        // The flat form, on the same knob.
        device->applyStructured (paramsMove ({ { "band4_threshold_db", -30.0 } }));
        check (near (device->getParamValue ("band4_threshold_db"), -30.0),
               "the flat id reaches the identical param");
    }

    std::printf ("== 4-Band: comp_bands MERGES into a band, unlike eq_bands ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "band2_threshold_db", -22.0 },
                                               { "band2_ratio", 6.0 } }));

        // A partial entry: a compressor band is a fixed slot, so this must not
        // reset the threshold the way a partial eq_bands entry replaces a band.
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("band", 2);
        e->setProperty ("attack_ms", 4.0);
        juce::Array<juce::var> arr;
        arr.add (juce::var (e.get()));

        device->applyStructured (juce::var (arr));      // bare array IS comp_bands

        check (near (device->getParamValue ("band2_attack_ms"), 4.0), "attack_ms updated");
        check (near (device->getParamValue ("band2_threshold_db"), -22.0),
               "threshold SURVIVED the partial entry");
        check (near (device->getParamValue ("band2_ratio"), 6.0), "ratio SURVIVED it too");
    }

    std::printf ("== 4-Band: array position IS the band when none is named ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        juce::Array<juce::var> arr;
        for (int i = 0; i < 4; ++i)
        {
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty ("threshold_db", -10.0 - i);     // -10, -11, -12, -13
            arr.add (juce::var (e.get()));
        }
        device->applyStructured (juce::var (arr));

        bool ok = true;
        for (int i = 0; i < 4; ++i)
            ok = ok && near (device->getParamValue ("band" + juce::String (i + 1) + "_threshold_db"),
                             -10.0 - i);
        check (ok, "four unnumbered entries landed on bands 1..4 in order");
    }

    std::printf ("== 4-Band: an out-of-range band is reported, not guessed at ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("band", 9);
        e->setProperty ("threshold_db", -20.0);
        juce::Array<juce::var> arr;
        arr.add (juce::var (e.get()));

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (juce::var (arr), &applied, &skipped);
        check (applied == 0, "nothing applied");
        check (skipped == 1, "counted as skipped");
        check (s.contains ("9"), "and named in the summary: " + s);
    }

    std::printf ("== 4-Band: crossovers are dialable and come back sorted ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* mb     = dynamic_cast<EedMultibandProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        proc->prepareToPlay (48000.0, 512);
        device->applyStructured (paramsMove ({ { "crossover1_hz", 200.0 },
                                               { "crossover2_hz", 1200.0 },
                                               { "crossover3_hz", 7000.0 } }));

        check (near (device->getParamValue ("crossover1_hz"), 200.0),  "crossover1 exact");
        check (near (device->getParamValue ("crossover3_hz"), 7000.0), "crossover3 exact");

        // The splitter is rebuilt on the AUDIO thread, so a move is a target
        // until a block runs. Processing one here is not test scaffolding — it
        // is the same handoff a DAW performs, and asserting before it would be
        // asserting on a state the plugin never actually plays in.
        check (! near (mb->crossoverHz (1), 1200.0, 1.0),
               "before a block runs, the splitter still holds the prepared value");

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        juce::MidiBuffer midi;
        proc->processBlock (buf, midi);

        check (near (mb->crossoverHz (1), 1200.0, 1.0),
               "after one block, the splitter has realised crossover2");
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
