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
#include "EedCompressorProcessor.h"
#include "EedDeEsserProcessor.h"
#include "EedDelayProcessor.h"
#include "EedExciterProcessor.h"
#include "EedExpanderProcessor.h"
#include "EedGainProcessor.h"
#include "EedGateProcessor.h"
#include "EedKeyDetectorProcessor.h"
#include "EedLimiterProcessor.h"
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

// EQ + Gain + Phase Invert (Wave 0) + the six Dynamics faces (Wave 1) + the
// rest of the suite + the Key Detector (the first Analysis reader) + Pitch
// (device #22, at P0 detection-only). A count rather than a >= so that a
// device silently failing to register is a FAILURE and not a test that
// quietly still passes.
static constexpr int kExpectedDevices = 22;

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

    // The first Analysis reader (KEY_DETECTOR_SPEC.md).
    check (registry.findByName ("EchoJay Key Detector")      != nullptr, "EchoJay Key Detector registered");

    // Device #22 (PITCH_CORRECTION_SPEC.md), at build phase P0: a detection-
    // only reader until the corrector phases land.
    check (registry.findByName ("EchoJay Pitch")             != nullptr, "EchoJay Pitch registered");

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

        check (registry.categories().joinIntoString (",") == "EQ,Dynamics,Utility,Stereo,Modulation,Harmonic,Time,Pitch,Analysis",
               "categories in canonical order: " + registry.categories().joinIntoString (","));

        // Analysis is the last category, so the reader sorts after every writer.
        check (names.indexOf ("EchoJay Reverb") < names.indexOf ("EchoJay Key Detector"),
               "the whole Time group precedes the Analysis group");

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

    // =======================================================================
    // THE TIME DEPTH PASS (DEVICE_DEPTH_PLAN.md, Time). The engines' own g++
    // suites prove the DSP does what each mode claims; what is checked here is
    // the DIALABILITY — by name and by index, clamped, round-tripping through
    // state, and advertised so a model can learn the names in the first place.
    // =======================================================================
    std::printf ("== REVERB: the algorithm dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Reverb");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* rev    = dynamic_cast<EedReverbProcessor*> (proc.get());
        check (device != nullptr && rev != nullptr, "reverb constructs");

        // Hall is the default because it is the NEUTRAL algorithm — the network as
        // it shipped — so a session saved before this param existed restores the
        // reverb it was mixed with rather than a new one.
        check (near (device->getParamValue ("algorithm"),
                     (double) (int) echojay::ReverbAlgorithm::Hall),
               "a fresh reverb is on hall, the neutral algorithm");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "algorithm", "plate" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "algorithm = \"plate\" applied");
        check (near (device->getParamValue ("algorithm"), 2.0), "landed on plate (index 2)");
        check (s.contains ("plate"), "and reads back BY NAME: " + s);
        check (rev->engine().getAlgorithm() == echojay::ReverbAlgorithm::Plate,
               "the ENGINE is on plate, not just the param");

        device->applyStructured (paramsMove ({ { "algorithm", "SPRING" } }));
        check (near (device->getParamValue ("algorithm"), 3.0), "matching is case-insensitive");

        device->applyStructured (paramsMove ({ { "algorithm", 4 } }));
        check (near (device->getParamValue ("algorithm"), 4.0), "a numeric index works too");
        check (rev->engine().getAlgorithm() == echojay::ReverbAlgorithm::Ambience,
               "index 4 is ambience");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "algorithm", "cathedral" } }), &a2, &s2);
        check (s2 == 1, "an unknown algorithm is SKIPPED, not guessed at");
        check (near (device->getParamValue ("algorithm"), 4.0),
               "and leaves the algorithm where it was");

        // Every one of the five, all the way to the engine.
        bool allReach = true;
        for (int i = 0; i < echojay::kNumReverbAlgorithms; ++i)
        {
            const auto want = echojay::reverbAlgorithmFromIndex (i);
            device->applyStructured (paramsMove ({
                { "algorithm", echojay::reverbAlgorithmName (want) } }));
            allReach = allReach && rev->engine().getAlgorithm() == want
                                && near (device->getParamValue ("algorithm"), (double) i);
        }
        check (allReach, "all five algorithms resolve by name and reach the engine");
    }

    std::printf ("== REVERB: diffusion and duck land exactly, and clamp ==\n");
    {
        auto proc = makeByName ("EchoJay Reverb");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        // The diffusion default is the UNITY point, for the same reason hall is the
        // default algorithm: it is the density the network was designed with.
        check (near (device->getParamValue ("diffusion"),
                     (double) echojay::kReverbDiffusionUnityPct),
               "diffusion defaults to its unity point");
        check (near (device->getParamValue ("duck"), 0.0), "and duck defaults to off");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "diffusion", 35.0 }, { "duck", 60.0 } }),
                                 &applied, &skipped);
        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (near (device->getParamValue ("diffusion"), 35.0), "diffusion EXACTLY 35");
        check (near (device->getParamValue ("duck"), 60.0),      "duck EXACTLY 60");

        device->applyStructured (paramsMove ({ { "diffusion", 900.0 }, { "duck", -20.0 } }));
        check (near (device->getParamValue ("diffusion"), 100.0), "900 clamps to 100");
        check (near (device->getParamValue ("duck"), 0.0),        "a negative duck clamps to 0");
    }

    std::printf ("== DELAY: the mode dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Delay");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* del    = dynamic_cast<EedDelayProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0), "a fresh delay is digital");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "mode", "tape" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"tape\" applied");
        check (near (device->getParamValue ("mode"), 1.0), "landed on tape (index 1)");
        check (s.contains ("tape"), "and reads back BY NAME: " + s);
        check (del->engine().getMode() == echojay::DelayMode::Tape,
               "the ENGINE is on tape, not just the param");

        device->applyStructured (paramsMove ({ { "mode", "ANALOG" } }));
        check (near (device->getParamValue ("mode"), 2.0), "matching is case-insensitive");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "bucket" } }), &a2, &s2);
        check (s2 == 1, "an unknown mode is SKIPPED, not guessed at");
        check (near (device->getParamValue ("mode"), 2.0), "and leaves the mode where it was");

        bool allReach = true;
        for (int i = 0; i < echojay::kNumDelayModes; ++i)
        {
            const auto want = echojay::delayModeFromIndex (i);
            device->applyStructured (paramsMove ({
                { "mode", echojay::delayModeName (want) } }));
            allReach = allReach && del->engine().getMode() == want
                                && near (device->getParamValue ("mode"), (double) i);
        }
        check (allReach, "all four modes resolve by name and reach the engine");
    }

    std::printf ("== DELAY: mode and ping_pong are BOTH honoured, independently ==\n");
    {
        // The one interlock in this pass, and it is deliberately non-destructive:
        // `mode = pingpong` is the clean bounce, `ping_pong` bounces WHATEVER mode
        // is selected, and the routing is the OR of the two. Neither overwrites the
        // other, so a bouncing tape echo is expressible and both round-trip.
        auto proc = makeByName ("EchoJay Delay");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* del    = dynamic_cast<EedDelayProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "mode", "pingpong" } }));
        check (del->engine().effectivePingPong(), "mode = pingpong bounces on its own");
        check (near (device->getParamValue ("ping_pong"), 0.0),
               "WITHOUT silently flipping the ping_pong param");

        device->applyStructured (paramsMove ({ { "mode", "tape" } }));
        check (! del->engine().effectivePingPong(), "back to tape, the bounce is gone");

        device->applyStructured (paramsMove ({ { "ping_pong", true } }));
        check (del->engine().effectivePingPong() && near (device->getParamValue ("mode"), 1.0),
               "and the switch bounces a TAPE echo, leaving the mode on tape");
    }

    std::printf ("== DELAY: diffusion and duck land exactly, and clamp ==\n");
    {
        auto proc = makeByName ("EchoJay Delay");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("diffusion"), 0.0), "diffusion defaults to off");
        check (near (device->getParamValue ("duck"), 0.0),      "and so does duck");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "diffusion", 45.0 }, { "duck", 70.0 } }),
                                 &applied, &skipped);
        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (near (device->getParamValue ("diffusion"), 45.0), "diffusion EXACTLY 45");
        check (near (device->getParamValue ("duck"), 70.0),      "duck EXACTLY 70");

        device->applyStructured (paramsMove ({ { "duck", 400.0 } }));
        check (near (device->getParamValue ("duck"), 100.0), "400 clamps to 100");
    }

    std::printf ("== the Time depth params round-trip through state ==\n");
    {
        // The base's state path is written in terms of the schema, so this is
        // really a check that each param is fully wired: an id that only half
        // exists (settable but not gettable) saves as 0 and comes back wrong.
        auto a = makeByName ("EchoJay Reverb");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "algorithm", "spring" },
                                           { "diffusion", 22.0 }, { "duck", 55.0 } }));
        juce::MemoryBlock rb;
        da->getStateInformation (rb);

        auto b = makeByName ("EchoJay Reverb");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (rb.getData(), (int) rb.getSize());

        check (near (db->getParamValue ("algorithm"), 3.0), "algorithm restored (spring)");
        check (near (db->getParamValue ("diffusion"), 22.0), "diffusion restored");
        check (near (db->getParamValue ("duck"), 55.0),      "duck restored");

        auto c = makeByName ("EchoJay Delay");
        auto* dc = dynamic_cast<EedDeviceProcessor*> (c.get());
        dc->applyStructured (paramsMove ({ { "mode", "analog" }, { "diffusion", 30.0 },
                                           { "duck", 65.0 }, { "ping_pong", true } }));
        juce::MemoryBlock dblob;
        dc->getStateInformation (dblob);

        auto d2 = makeByName ("EchoJay Delay");
        auto* dd = dynamic_cast<EedDeviceProcessor*> (d2.get());
        dd->setStateInformation (dblob.getData(), (int) dblob.getSize());

        check (near (dd->getParamValue ("mode"), 2.0),      "mode restored (analog)");
        check (near (dd->getParamValue ("diffusion"), 30.0),"diffusion restored");
        check (near (dd->getParamValue ("duck"), 65.0),     "duck restored");
        check (near (dd->getParamValue ("ping_pong"), 1.0),
               "and ping_pong restored ALONGSIDE the mode, not instead of it");
    }

    std::printf ("== the Time depth choices are ADVERTISED by name ==\n");
    {
        // A choice the model cannot read is a choice it cannot set, however well
        // the apply path resolves names.
        struct Expect { const char* device; const char* id; const char* names; const char* def; };

        const Expect wanted[] = {
            { "EchoJay Reverb", "algorithm", "room|hall|plate|spring|ambience", "default hall" },
            { "EchoJay Delay",  "mode",      "digital|tape|analog|pingpong",    "default digital" },
        };

        for (const auto& w : wanted)
        {
            const auto* d = registry.findByName (w.device);
            const auto* spec = d != nullptr ? d->schema.find (w.id) : nullptr;
            if (spec == nullptr)
            {
                check (false, juce::String (w.device) + " advertises " + w.id);
                continue;
            }
            const auto line = juce::String (echojay::ParamSchema::describeLine (*spec));
            check (line.contains (w.names) && line.contains (w.def),
                   juce::String (w.device) + " " + w.id + ": " + line);
        }
    }

    std::printf ("== a Time device at its DEFAULTS is unchanged by the depth pass ==\n");
    {
        // The strongest guarantee this pass makes: the neutral algorithm and mode
        // are the devices as they shipped, so an existing session sounds the same.
        // Checked by rendering the SAME audio through a device at its defaults and
        // through one with the depth params explicitly set to their neutral values.
        for (const char* name : { "EchoJay Reverb", "EchoJay Delay" })
        {
            auto mk = [name] (bool explicitNeutral)
            {
                auto proc = makeByName (name);
                auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
                if (explicitNeutral)
                {
                    if (juce::String (name) == "EchoJay Reverb")
                        device->applyStructured (paramsMove ({
                            { "algorithm", "hall" },
                            { "diffusion", (double) echojay::kReverbDiffusionUnityPct },
                            { "duck", 0.0 } }));
                    else
                        device->applyStructured (paramsMove ({
                            { "mode", "digital" }, { "diffusion", 0.0 }, { "duck", 0.0 } }));
                }
                proc->setPlayConfigDetails (2, 2, 48000.0, 512);
                proc->prepareToPlay (48000.0, 512);
                return proc;
            };

            auto pa = mk (false), pb = mk (true);

            juce::AudioBuffer<float> ba (2, 4096), bb (2, 4096);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                {
                    const float v = 0.4f * std::sin (0.037f * (float) i)
                                  + (i % 977 == 0 ? 0.5f : 0.0f);
                    ba.setSample (ch, i, v);
                    bb.setSample (ch, i, v);
                }

            juce::MidiBuffer midi;
            for (int i = 0; i < 4096; i += 512)
            {
                juce::AudioBuffer<float> sa (ba.getArrayOfWritePointers(), 2, i, 512);
                juce::AudioBuffer<float> sb (bb.getArrayOfWritePointers(), 2, i, 512);
                pa->processBlock (sa, midi);
                pb->processBlock (sb, midi);
            }

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                    worst = juce::jmax (worst, std::abs (ba.getSample (ch, i)
                                                       - bb.getSample (ch, i)));

            check (worst == 0.0f,
                   juce::String (name) + ": its defaults ARE the neutral settings "
                   "(worst delta " + juce::String (worst, 9) + ")");
        }
    }

    // =======================================================================
    // THE MODULATION DEPTH PASS (DEVICE_DEPTH_PLAN.md, Modulation). Same
    // programme as the Time section above: the engines' g++ suites prove each
    // mode's DSP claim; what is checked here is the DIALABILITY — by name and
    // by index, clamped, round-tripping through state, advertised — and the
    // neutrality guarantee, end to end through processBlock.
    // =======================================================================
    std::printf ("== CHORUS: the mode dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Chorus");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* cho    = dynamic_cast<EedChorusProcessor*> (proc.get());
        check (device != nullptr && cho != nullptr, "chorus constructs");

        check (near (device->getParamValue ("mode"), 0.0), "a fresh chorus is classic");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "mode", "ensemble" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"ensemble\" applied");
        check (s.contains ("ensemble"), "and reads back BY NAME: " + s);
        check (cho->engine().getMode() == echojay::ChorusMode::Ensemble,
               "the ENGINE is on ensemble, not just the param");

        device->applyStructured (paramsMove ({ { "mode", "DIMENSION" } }));
        check (near (device->getParamValue ("mode"), 2.0), "matching is case-insensitive");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "flanger" } }), &a2, &s2);
        check (s2 == 1, "an unknown mode is SKIPPED, not guessed at");
        check (near (device->getParamValue ("mode"), 2.0), "and leaves the mode where it was");

        bool allReach = true;
        for (int i = 0; i < echojay::kNumChorusModes; ++i)
        {
            const auto want = echojay::chorusModeFromIndex (i);
            device->applyStructured (paramsMove ({ { "mode", echojay::chorusModeName (want) } }));
            allReach = allReach && cho->engine().getMode() == want
                                && near (device->getParamValue ("mode"), (double) i);
        }
        check (allReach, "all three modes resolve by name and reach the engine");

        device->applyStructured (paramsMove ({ { "mode", 1 } }));
        check (cho->engine().getMode() == echojay::ChorusMode::Ensemble,
               "a numeric index works too");
    }

    std::printf ("== CHORUS: spread and tone land exactly, and clamp ==\n");
    {
        auto proc = makeByName ("EchoJay Chorus");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        // Spread's default is the OLD fixed 90-degree offset, for the same
        // reason hall is the reverb's default: it is the device as it shipped.
        check (near (device->getParamValue ("spread"),
                     (double) echojay::ChorusEngine::kDefaultSpreadPct),
               "spread defaults to 50, the shipped quarter-cycle offset");
        check (near (device->getParamValue ("tone"), 0.0), "tone defaults to flat");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "spread", 80.0 }, { "tone", -3.5 } }),
                                 &applied, &skipped);
        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (near (device->getParamValue ("spread"), 80.0), "spread EXACTLY 80");
        check (near (device->getParamValue ("tone"), -3.5),   "tone EXACTLY -3.5");

        device->applyStructured (paramsMove ({ { "spread", 400.0 }, { "tone", -20.0 } }));
        check (near (device->getParamValue ("spread"), 100.0), "400 clamps to 100");
        check (near (device->getParamValue ("tone"), -6.0),    "-20 clamps to -6");
    }

    std::printf ("== PHASER: the mode dials by NAME, and stereo_spread lands ==\n");
    {
        auto proc = makeByName ("EchoJay Phaser");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* pha    = dynamic_cast<EedPhaserProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0), "a fresh phaser is modern");
        check (near (device->getParamValue ("stereo_spread"),
                     (double) echojay::PhaserEngine::kDefaultSpreadDeg),
               "stereo_spread defaults to 90, the shipped offset");

        const auto s = device->applyStructured (paramsMove ({ { "mode", "vintage" } }));
        check (s.contains ("vintage"), "mode = \"vintage\" reads back BY NAME: " + s);
        check (pha->engine().getMode() == echojay::PhaserMode::Vintage,
               "the ENGINE is on vintage, not just the param");
        check (pha->engine().effectiveStages() == 4,
               "and the default 6 dialled stages run as vintage's 4");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "univibe" } }), &a2, &s2);
        check (s2 == 1, "an unknown mode is SKIPPED, not guessed at");

        bool allReach = true;
        for (int i = 0; i < echojay::kNumPhaserModes; ++i)
        {
            const auto want = echojay::phaserModeFromIndex (i);
            device->applyStructured (paramsMove ({ { "mode", echojay::phaserModeName (want) } }));
            allReach = allReach && pha->engine().getMode() == want;
        }
        check (allReach, "all three modes resolve by name and reach the engine");

        device->applyStructured (paramsMove ({ { "stereo_spread", 135.0 } }));
        check (near (device->getParamValue ("stereo_spread"), 135.0), "stereo_spread EXACTLY 135");
        device->applyStructured (paramsMove ({ { "stereo_spread", 720.0 } }));
        check (near (device->getParamValue ("stereo_spread"), 360.0), "720 clamps to 360");
    }

    std::printf ("== TREMOLO: mode and the new shapes dial by NAME ==\n");
    {
        auto proc = makeByName ("EchoJay Tremolo");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* trm    = dynamic_cast<EedTremoloProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0), "a fresh tremolo is the sine circuit");

        const auto s = device->applyStructured (paramsMove ({ { "mode", "bias" } }));
        check (s.contains ("bias"), "mode = \"bias\" reads back BY NAME: " + s);
        check (trm->engine().getMode() == echojay::TremoloMode::Bias,
               "the ENGINE is on bias, not just the param");

        bool allReach = true;
        for (int i = 0; i < echojay::kNumTremoloModes; ++i)
        {
            const auto want = echojay::tremoloModeFromIndex (i);
            device->applyStructured (paramsMove ({ { "mode", echojay::tremoloModeName (want) } }));
            allReach = allReach && trm->engine().getMode() == want;
        }
        check (allReach, "all three circuits resolve by name and reach the engine");

        // The shape choice grew names with the depth pass: the new shapes
        // resolve by name, the old NUMERIC form keeps working (the backend
        // spec promises "shape": 2 forever), and the engine gets the index.
        device->applyStructured (paramsMove ({ { "shape", "random" } }));
        check (near (device->getParamValue ("shape"), (double) echojay::LfoCore::kRandom),
               "shape = \"random\" lands on the sample-and-hold shape");
        check (trm->engine().lfo().getShape() == echojay::LfoCore::kRandom,
               "and the LFO is running it");

        device->applyStructured (paramsMove ({ { "shape", "harmonic" } }));
        check (near (device->getParamValue ("shape"), (double) echojay::LfoCore::kHarmonic),
               "shape = \"harmonic\" lands too");

        device->applyStructured (paramsMove ({ { "shape", 2 } }));
        check (trm->engine().lfo().getShape() == echojay::LfoCore::kSquare,
               "the numeric form still works: shape 2 is square");

        device->applyStructured (paramsMove ({ { "smoothing_ms", 120.0 } }));
        check (near (device->getParamValue ("smoothing_ms"), 120.0),
               "smoothing_ms EXACTLY 120 - the random glide is dialable");
        device->applyStructured (paramsMove ({ { "smoothing_ms", 9999.0 } }));
        check (near (device->getParamValue ("smoothing_ms"),
                     (double) echojay::LfoCore::kMaxSmoothingMs),
               "and clamps to the core's ceiling");
    }

    std::printf ("== AUTO PAN: mode, width and the new shapes dial by NAME ==\n");
    {
        auto proc = makeByName ("EchoJay Auto Pan");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* pan    = dynamic_cast<EedAutoPanProcessor*> (proc.get());

        // constant_power is the neutral default and NOT index 0 — the shipped
        // law keeps its behaviour, the list order stays natural.
        check (near (device->getParamValue ("mode"),
                     (double) (int) echojay::AutoPanMode::ConstantPower),
               "a fresh auto pan is constant_power, the shipped law");

        const auto s = device->applyStructured (paramsMove ({ { "mode", "binaural" } }));
        check (s.contains ("binaural"), "mode = \"binaural\" reads back BY NAME: " + s);
        check (pan->engine().getMode() == echojay::AutoPanMode::Binaural,
               "the ENGINE is on binaural, not just the param");

        device->applyStructured (paramsMove ({ { "mode", "Constant Power" } }));
        check (pan->engine().getMode() == echojay::AutoPanMode::ConstantPower,
               "\"Constant Power\" matches constant_power (tolerant folding)");

        bool allReach = true;
        for (int i = 0; i < echojay::kNumAutoPanModes; ++i)
        {
            const auto want = echojay::autoPanModeFromIndex (i);
            device->applyStructured (paramsMove ({ { "mode", echojay::autoPanModeName (want) } }));
            allReach = allReach && pan->engine().getMode() == want;
        }
        check (allReach, "all three laws resolve by name and reach the engine");

        device->applyStructured (paramsMove ({ { "width", 35.0 } }));
        check (near (device->getParamValue ("width"), 35.0), "width EXACTLY 35");
        device->applyStructured (paramsMove ({ { "width", 250.0 } }));
        check (near (device->getParamValue ("width"), 100.0), "250 clamps to 100");

        device->applyStructured (paramsMove ({ { "shape", "random" } }));
        check (pan->engine().lfo().getShape() == echojay::LfoCore::kRandom,
               "shape = \"random\" reaches the LFO here too");
    }

    std::printf ("== the Modulation depth params round-trip through state ==\n");
    {
        auto a = makeByName ("EchoJay Chorus");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "mode", "dimension" },
                                           { "spread", 75.0 }, { "tone", 2.5 } }));
        juce::MemoryBlock cb;
        da->getStateInformation (cb);

        auto b = makeByName ("EchoJay Chorus");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (cb.getData(), (int) cb.getSize());
        check (near (db->getParamValue ("mode"), 2.0),   "chorus mode restored (dimension)");
        check (near (db->getParamValue ("spread"), 75.0),"spread restored");
        check (near (db->getParamValue ("tone"), 2.5),   "tone restored");

        auto c = makeByName ("EchoJay Phaser");
        auto* dc = dynamic_cast<EedDeviceProcessor*> (c.get());
        dc->applyStructured (paramsMove ({ { "mode", "vintage" }, { "stereo_spread", 150.0 } }));
        juce::MemoryBlock pb;
        dc->getStateInformation (pb);

        auto d2 = makeByName ("EchoJay Phaser");
        auto* dd = dynamic_cast<EedDeviceProcessor*> (d2.get());
        dd->setStateInformation (pb.getData(), (int) pb.getSize());
        check (near (dd->getParamValue ("mode"), 1.0),           "phaser mode restored (vintage)");
        check (near (dd->getParamValue ("stereo_spread"), 150.0),"stereo_spread restored");

        auto e = makeByName ("EchoJay Tremolo");
        auto* de = dynamic_cast<EedDeviceProcessor*> (e.get());
        de->applyStructured (paramsMove ({ { "mode", "optical" }, { "shape", "harmonic" },
                                           { "smoothing_ms", 80.0 } }));
        juce::MemoryBlock tb;
        de->getStateInformation (tb);

        auto e2 = makeByName ("EchoJay Tremolo");
        auto* de2 = dynamic_cast<EedDeviceProcessor*> (e2.get());
        de2->setStateInformation (tb.getData(), (int) tb.getSize());
        check (near (de2->getParamValue ("mode"), 1.0), "tremolo circuit restored (optical)");
        check (near (de2->getParamValue ("shape"), 4.0),"the harmonic shape restored");
        check (near (de2->getParamValue ("smoothing_ms"), 80.0), "smoothing restored");

        auto f = makeByName ("EchoJay Auto Pan");
        auto* df = dynamic_cast<EedDeviceProcessor*> (f.get());
        df->applyStructured (paramsMove ({ { "mode", "linear" }, { "width", 60.0 },
                                           { "shape", "random" } }));
        juce::MemoryBlock ab;
        df->getStateInformation (ab);

        auto f2 = makeByName ("EchoJay Auto Pan");
        auto* df2 = dynamic_cast<EedDeviceProcessor*> (f2.get());
        df2->setStateInformation (ab.getData(), (int) ab.getSize());
        check (near (df2->getParamValue ("mode"), 0.0),  "pan law restored (linear)");
        check (near (df2->getParamValue ("width"), 60.0),"width restored");
        check (near (df2->getParamValue ("shape"), 5.0), "the random shape restored");
    }

    std::printf ("== the Modulation depth choices are ADVERTISED by name ==\n");
    {
        struct Expect { const char* device; const char* id; const char* names; const char* def; };

        const Expect wanted[] = {
            { "EchoJay Chorus",   "mode",  "classic|ensemble|dimension", "default classic" },
            { "EchoJay Phaser",   "mode",  "modern|vintage|stereo",      "default modern" },
            { "EchoJay Tremolo",  "mode",  "sine|optical|bias",          "default sine" },
            { "EchoJay Auto Pan", "mode",  "linear|constant_power|binaural", "default constant_power" },
            // The shape list itself is now taught by name, with the two new
            // shapes on the end where the frozen indices demand they be.
            { "EchoJay Tremolo",  "shape", "sine|triangle|square|saw|harmonic|random", "default sine" },
            { "EchoJay Auto Pan", "shape", "sine|triangle|square|saw|harmonic|random", "default sine" },
        };

        for (const auto& w : wanted)
        {
            const auto* d = registry.findByName (w.device);
            const auto* spec = d != nullptr ? d->schema.find (w.id) : nullptr;
            if (spec == nullptr)
            {
                check (false, juce::String (w.device) + " advertises " + w.id);
                continue;
            }
            const auto line = juce::String (echojay::ParamSchema::describeLine (*spec));
            check (line.contains (w.names) && line.contains (w.def),
                   juce::String (w.device) + " " + w.id + ": " + line);
        }
    }

    std::printf ("== a Modulation device at its DEFAULTS is unchanged by the depth pass ==\n");
    {
        // The guarantee the whole pass rests on, checked the way the Time pass
        // checked it: the SAME audio through a device at its defaults and
        // through one with every depth param explicitly set to its neutral
        // value, bit-identical. LFO phase matters here, so both instances are
        // prepared (which resets the LFO) after their params are set.
        struct Neutral
        {
            const char* name;
            std::vector<std::pair<const char*, juce::var>> params;
        };

        const Neutral devices[] = {
            { "EchoJay Chorus",   { { "mode", "classic" }, { "spread", 50.0 }, { "tone", 0.0 } } },
            { "EchoJay Phaser",   { { "mode", "modern" }, { "stereo_spread", 90.0 } } },
            { "EchoJay Tremolo",  { { "mode", "sine" }, { "shape", "sine" },
                                    { "smoothing_ms", 1.5 } } },
            { "EchoJay Auto Pan", { { "mode", "constant_power" }, { "width", 100.0 },
                                    { "shape", "sine" }, { "smoothing_ms", 1.5 } } },
        };

        for (const auto& n : devices)
        {
            auto mk = [&n] (bool explicitNeutral)
            {
                auto proc = makeByName (n.name);
                auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
                if (explicitNeutral)
                {
                    juce::DynamicObject::Ptr params = new juce::DynamicObject();
                    for (const auto& p : n.params)
                        params->setProperty (juce::Identifier (p.first), p.second);
                    juce::DynamicObject::Ptr root = new juce::DynamicObject();
                    root->setProperty ("params", juce::var (params.get()));
                    device->applyStructured (juce::var (root.get()));
                }
                proc->setPlayConfigDetails (2, 2, 48000.0, 512);
                proc->prepareToPlay (48000.0, 512);
                return proc;
            };

            auto pa = mk (false), pb = mk (true);

            juce::AudioBuffer<float> ba (2, 4096), bb (2, 4096);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                {
                    const float v = 0.4f * std::sin (0.037f * (float) i)
                                  + (i % 977 == 0 ? 0.5f : 0.0f);
                    ba.setSample (ch, i, v);
                    bb.setSample (ch, i, v);
                }

            juce::MidiBuffer midi;
            for (int i = 0; i < 4096; i += 512)
            {
                juce::AudioBuffer<float> sa (ba.getArrayOfWritePointers(), 2, i, 512);
                juce::AudioBuffer<float> sb (bb.getArrayOfWritePointers(), 2, i, 512);
                pa->processBlock (sa, midi);
                pb->processBlock (sb, midi);
            }

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                    worst = juce::jmax (worst, std::abs (ba.getSample (ch, i)
                                                       - bb.getSample (ch, i)));

            check (worst == 0.0f,
                   juce::String (n.name) + ": its defaults ARE the neutral settings "
                   "(worst delta " + juce::String (worst, 9) + ")");
        }
    }

    std::printf ("== the new shapes render finite audio through processBlock ==\n");
    {
        // Not a DSP test (the g++ suites pin the shapes) — a wiring test: the
        // whole chain from a named move to rendered audio, on the shape most
        // likely to misbehave (random, with a long glide).
        for (const char* name : { "EchoJay Tremolo", "EchoJay Auto Pan" })
        {
            auto proc = makeByName (name);
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            device->applyStructured (paramsMove ({ { "shape", "random" },
                                                   { "smoothing_ms", 150.0 },
                                                   { "rate_hz", 8.0 },
                                                   { "depth", 100.0 } }));
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);

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
            check (finite, juce::String (name) + " on random + glide renders finite audio");
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

    // =======================================================================
    // THE EQ DEPTH PASS (SURGICAL_EQ_ENHANCEMENTS.md P2-P5). The g++ suites
    // (eq_ms_test, eq_linear_test, eq_hunt_test, eq_note_test) prove the DSP
    // claims; what is checked here is DIALABILITY through the one funnel —
    // channel and note on bands, phase/ms in settings AND params, the hunt
    // action against real audio, presets as a base, state round-trip, and the
    // neutrality of every default.
    // =======================================================================
    auto eqBandsMove = [] (std::initializer_list<
                               std::initializer_list<std::pair<const char*, juce::var>>> bandDefs)
    {
        juce::Array<juce::var> bands;
        for (const auto& def : bandDefs)
        {
            juce::DynamicObject::Ptr b = new juce::DynamicObject();
            for (const auto& kv : def) b->setProperty (juce::Identifier (kv.first), kv.second);
            bands.add (juce::var (b.get()));
        }
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("eq_bands", juce::var (bands));
        return juce::var (root.get());
    };

    std::printf ("== EQ P2: per-band channel dials tolerantly, and echoes ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        const auto s = device->applyStructured (eqBandsMove ({
            { { "type", "highshelf" }, { "freq_hz", 10000.0 }, { "gain_db", 3.0 },
              { "q", 0.707 }, { "channel", "side" } } }));
        check (eq->getBand (0).channel == echojay::BandChannel::Side,
               "channel:\"side\" landed on the band");
        check (s.contains ("[side]"), "and the summary says so: " + s);

        device->applyStructured (eqBandsMove ({
            { { "band", 2 }, { "type", "bell" }, { "freq_hz", 300.0 },
              { "gain_db", -2.0 }, { "q", 2.0 }, { "channel", "m" } } }));
        check (eq->getBand (1).channel == echojay::BandChannel::Mid,
               "single-letter \"m\" resolves to mid");

        device->applyStructured (eqBandsMove ({
            { { "band", 3 }, { "type", "bell" }, { "freq_hz", 500.0 },
              { "gain_db", 1.0 }, { "q", 1.0 }, { "channel", "surround" } } }));
        check (eq->getBand (2).channel == echojay::BandChannel::Stereo,
               "an unknown channel keeps the stereo default, never guesses");

        // currentEqBandsVar echoes routing — and only routing that departs
        // from the default, so untouched states stay byte-identical.
        const auto bands = eq->currentEqBandsVar();
        check (bands[0].getProperty ("channel", "").toString() == "side",
               "side band echoes channel:\"side\"");
        check (! bands[2].hasProperty ("channel"),
               "a stereo band omits the key entirely");
    }

    std::printf ("== EQ P5: a musical note lands a band on its pitch ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (eqBandsMove ({
            { { "type", "notch" }, { "note", "G5" }, { "q", 8.0 } } }));
        check (near (eq->getBand (0).freqHz, 783.99, 1.0),
               "{note:\"G5\"} lands on 784 Hz (got "
               + juce::String (eq->getBand (0).freqHz, 2) + ")");

        device->applyStructured (eqBandsMove ({
            { { "band", 2 }, { "type", "bell" }, { "note", "A4" },
              { "freq_hz", 1234.0 }, { "gain_db", -1.0 }, { "q", 1.0 } } }));
        check (near (eq->getBand (1).freqHz, 1234.0, 0.5),
               "explicit freq_hz WINS over note when both are sent");

        int a = 0, sk = 0;
        device->applyStructured (eqBandsMove ({
            { { "band", 3 }, { "type", "bell" }, { "note", "H9" },
              { "gain_db", 2.0 }, { "q", 1.0 } } }), &a, &sk);
        check (near (eq->getBand (2).freqHz, 1000.0, 0.5),
               "an unparseable note falls back to the default freq, never guesses");
    }

    std::printf ("== EQ P4: phase_mode dials via eq_settings AND params, latency follows ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (eq->getPhaseMode() == SurgicalEqProcessor::PhaseMode::Zero,
               "a fresh EQ is zero-latency minimum phase, the neutral mode");
        proc->prepareToPlay (48000.0, 512);
        check (proc->getLatencySamples() == 0, "…and reports 0 samples");

        // Via eq_settings, the structured shape.
        juce::DynamicObject::Ptr settings = new juce::DynamicObject();
        settings->setProperty ("phase_mode", "linear");
        juce::DynamicObject::Ptr move = new juce::DynamicObject();
        move->setProperty ("eq_settings", juce::var (settings.get()));
        const auto s = device->applyStructured (juce::var (move.get()));

        check (eq->getPhaseMode() == SurgicalEqProcessor::PhaseMode::Linear,
               "eq_settings.phase_mode:\"linear\" landed");
        check (proc->getLatencySamples() == 2560,
               "and the latency was re-reported LIVE, no re-prepare (got "
               + juce::String (proc->getLatencySamples()) + ")");
        check (s.contains ("linear") && s.contains ("ms"),
               "summary names the mode and the cost: " + s);
        check (proc->getTailLengthSeconds() > 0.05,
               "getTailLengthSeconds is wired, not the 0.0 stub");

        // Via the universal flat path: by NAME and by index.
        device->applyStructured (paramsMove ({ { "phase_mode", "zero" } }));
        check (eq->getPhaseMode() == SurgicalEqProcessor::PhaseMode::Zero
            && proc->getLatencySamples() == 0,
               "params phase_mode:\"zero\" flips back and re-reports 0");
        device->applyStructured (paramsMove ({ { "phase_mode", 1 } }));
        check (eq->getPhaseMode() == SurgicalEqProcessor::PhaseMode::Linear,
               "a numeric index works too");

        device->applyStructured (paramsMove ({ { "ms_mode", true } }));
        check (eq->getMsMode(), "ms_mode dials through params");

        // The advertisement carries the choice names.
        const auto* spec = SurgicalEqProcessor::schema().find ("phase_mode");
        const auto line = juce::String (echojay::ParamSchema::describeLine (*spec));
        check (line.contains ("zero|linear") && line.contains ("default zero"),
               "advertised as names: " + line);
    }

    std::printf ("== EQ P5: a preset is a BASE that explicit bands refine ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        juce::DynamicObject::Ptr move = new juce::DynamicObject();
        move->setProperty ("eq_preset", "Vocal Clarity");        // tolerant spelling
        const auto s = device->applyStructured (juce::var (move.get()));
        check (s.contains ("vocal-clarity"), "preset loads by tolerant name: " + s);
        check (eq->getBand (0).enabled
            && eq->getBand (0).type == echojay::BandType::HighPass
            && near (eq->getBand (0).freqHz, 80.0, 0.5),
               "band 1 is the preset's 80 Hz HPF");
        check (eq->getBand (3).enabled, "all four preset bands landed");

        // Preset + explicit band in ONE move: the §0 order means the explicit
        // band edits the freshly laid base.
        juce::DynamicObject::Ptr b = new juce::DynamicObject();
        b->setProperty ("band", 2);
        b->setProperty ("type", "bell");
        b->setProperty ("freq_hz", 300.0);
        b->setProperty ("gain_db", -4.0);
        b->setProperty ("q", 1.2);
        juce::Array<juce::var> bandArr; bandArr.add (juce::var (b.get()));
        juce::DynamicObject::Ptr move2 = new juce::DynamicObject();
        move2->setProperty ("eq_preset", "vocal-clarity");
        move2->setProperty ("eq_bands", juce::var (bandArr));
        device->applyStructured (juce::var (move2.get()));
        check (near (eq->getBand (1).gainDb, -4.0, 0.01),
               "\"preset, then cut 300 harder\" is one move: band 2 reads -4 dB");

        // An unknown preset is an honest miss that changes nothing.
        juce::DynamicObject::Ptr move3 = new juce::DynamicObject();
        move3->setProperty ("eq_preset", "smiley-face");
        const auto s3 = device->applyStructured (juce::var (move3.get()));
        check (s3.contains ("unknown") && s3.contains ("vocal-clarity"),
               "the miss lists what DOES exist: " + s3);
        check (near (eq->getBand (1).gainDb, -4.0, 0.01), "and nothing moved");
    }

    std::printf ("== EQ P3: the hunt finds a real resonance in real audio ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        auto huntMove = [] (const char* sensitivity, bool dynamic)
        {
            juce::DynamicObject::Ptr action = new juce::DynamicObject();
            action->setProperty ("type", "tame_resonances");
            action->setProperty ("sensitivity", sensitivity);
            action->setProperty ("dynamic", dynamic);
            juce::DynamicObject::Ptr root = new juce::DynamicObject();
            root->setProperty ("eq_action", juce::var (action.get()));
            return juce::var (root.get());
        };

        // BEFORE audio: the honest "nothing to analyse" answer.
        const auto sQuiet = device->applyStructured (huntMove ("medium", true));
        check (sQuiet.contains ("no signal"), "silence answers honestly: " + sQuiet);

        // ~2 s of noise + a screaming 3.7 kHz resonance through processBlock —
        // the REAL capture path, not a detector shortcut.
        {
            juce::Random rng (1234);
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            double phase = 0.0;
            const double dp = 2.0 * juce::MathConstants<double>::pi * 3700.0 / 48000.0;
            for (int blk = 0; blk < 200; ++blk)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.25f * (float) std::sin (phase)
                                  + 0.05f * (rng.nextFloat() * 2.0f - 1.0f);
                    phase += dp;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }
                proc->processBlock (buf, midi);
            }
        }

        // A hand-dialled band first, so the hunt has something it must NOT eat.
        device->applyStructured (eqBandsMove ({
            { { "band", 1 }, { "type", "bell" }, { "freq_hz", 150.0 },
              { "gain_db", 2.0 }, { "q", 1.0 } } }));

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (huntMove ("medium", true),
                                                &applied, &skipped);
        check (applied >= 1, "the hunt placed band(s): " + s);
        check (s.contains ("hunt"), "and reported as a hunt");

        check (near (eq->getBand (0).freqHz, 150.0, 0.5)
            && near (eq->getBand (0).gainDb, 2.0, 0.01),
               "the hand-dialled band 1 survived untouched");

        bool foundIt = false;
        for (int i = 1; i < SurgicalEqProcessor::kNumBands; ++i)
        {
            const auto b = eq->getBand (i);
            if (b.enabled && b.dynamic
                && std::fabs (b.freqHz - 3700.0f) < 3700.0f * 0.06f
                && b.rangeDb < 0.0f)
                foundIt = true;
        }
        check (foundIt, "a dynamic bell sits on the 3.7 kHz resonance, range negative");

        // Static flavour: notches instead of bells.
        auto proc2 = makeByName ("EchoJay EQ");
        auto* eq2 = dynamic_cast<SurgicalEqProcessor*> (proc2.get());
        auto* dev2 = dynamic_cast<EedDeviceProcessor*> (proc2.get());
        proc2->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc2->prepareToPlay (48000.0, 512);
        {
            juce::Random rng (99);
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            double phase = 0.0;
            const double dp = 2.0 * juce::MathConstants<double>::pi * 3700.0 / 48000.0;
            for (int blk = 0; blk < 200; ++blk)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.25f * (float) std::sin (phase)
                                  + 0.05f * (rng.nextFloat() * 2.0f - 1.0f);
                    phase += dp;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }
                proc2->processBlock (buf, midi);
            }
        }
        dev2->applyStructured (huntMove ("medium", false));
        bool notched = false;
        for (int i = 0; i < SurgicalEqProcessor::kNumBands; ++i)
        {
            const auto b = eq2->getBand (i);
            if (b.enabled && b.type == echojay::BandType::Notch
                && std::fabs (b.freqHz - 3700.0f) < 3700.0f * 0.06f)
                notched = true;
        }
        check (notched, "dynamic:false places a static notch instead");

        // An action this EQ does not know is an honest refusal.
        juce::DynamicObject::Ptr weird = new juce::DynamicObject();
        weird->setProperty ("type", "make_it_pop");
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("eq_action", juce::var (weird.get()));
        const auto sW = device->applyStructured (juce::var (root.get()));
        check (sW.contains ("tame_resonances"),
               "unknown action names what IS available: " + sW);
    }

    std::printf ("== EQ: state v3 round-trips routing and phase ==\n");
    {
        auto proc = makeByName ("EchoJay EQ");
        auto* eq = dynamic_cast<SurgicalEqProcessor*> (proc.get());
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (eqBandsMove ({
            { { "type", "highshelf" }, { "freq_hz", 10000.0 }, { "gain_db", 3.0 },
              { "q", 0.707 }, { "channel", "side" } } }));
        juce::DynamicObject::Ptr settings = new juce::DynamicObject();
        settings->setProperty ("phase_mode", "linear");
        settings->setProperty ("ms_mode", true);
        juce::DynamicObject::Ptr move = new juce::DynamicObject();
        move->setProperty ("eq_settings", juce::var (settings.get()));
        device->applyStructured (juce::var (move.get()));

        juce::MemoryBlock blob;
        proc->getStateInformation (blob);

        auto proc2 = makeByName ("EchoJay EQ");
        auto* eq2 = dynamic_cast<SurgicalEqProcessor*> (proc2.get());
        proc2->setStateInformation (blob.getData(), (int) blob.getSize());

        check (eq2->getBand (0).channel == echojay::BandChannel::Side,
               "the side routing restored");
        check (eq2->getPhaseMode() == SurgicalEqProcessor::PhaseMode::Linear,
               "linear phase restored");
        check (eq2->getMsMode(), "M/S view restored");

        // A pre-P2 state (no channel, no version fuss) loads as stereo — the
        // exact compatibility promise the version bump documents.
        const char* v2json = "{\"v\":2,\"bypassed\":false,"
                             "\"eq_bands\":[{\"band\":1,\"type\":\"bell\","
                             "\"freq_hz\":400.0,\"gain_db\":-3.0,\"q\":2.0,"
                             "\"slope_db_oct\":12}],\"eq_settings\":{}}";
        auto proc3 = makeByName ("EchoJay EQ");
        auto* eq3 = dynamic_cast<SurgicalEqProcessor*> (proc3.get());
        proc3->setStateInformation (v2json, (int) std::strlen (v2json));
        check (eq3->getBand (0).enabled
            && eq3->getBand (0).channel == echojay::BandChannel::Stereo
            && eq3->getPhaseMode() == SurgicalEqProcessor::PhaseMode::Zero,
               "a v2 state loads: band stereo, phase zero, nothing invented");
    }

    std::printf ("== EQ: its defaults are UNCHANGED by the whole depth pass ==\n");
    {
        // The same worst-delta-is-zero proof the Time/Modulation passes used,
        // end to end through processBlock: a fresh EQ and one with every depth
        // param explicitly at neutral must be the same device, bit for bit.
        auto mk = [&] (bool explicitNeutral)
        {
            auto proc = makeByName ("EchoJay EQ");
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            if (explicitNeutral)
            {
                device->applyStructured (paramsMove ({
                    { "output_db", 0.0 }, { "auto_gain", false },
                    { "phase_mode", "zero" }, { "ms_mode", false } }));
                // …and a band set that exercises the P2 default channel too
                juce::DynamicObject::Ptr b = new juce::DynamicObject();
                b->setProperty ("type", "bell");
                b->setProperty ("freq_hz", 1200.0);
                b->setProperty ("gain_db", -3.0);
                b->setProperty ("q", 2.0);
                b->setProperty ("channel", "stereo");
                juce::Array<juce::var> arr; arr.add (juce::var (b.get()));
                device->applyStructured (juce::var (arr));
            }
            else
            {
                juce::DynamicObject::Ptr b = new juce::DynamicObject();
                b->setProperty ("type", "bell");
                b->setProperty ("freq_hz", 1200.0);
                b->setProperty ("gain_db", -3.0);
                b->setProperty ("q", 2.0);
                juce::Array<juce::var> arr; arr.add (juce::var (b.get()));
                device->applyStructured (juce::var (arr));
            }
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);
            return proc;
        };

        auto pa = mk (false), pb = mk (true);

        juce::AudioBuffer<float> ba (2, 4096), bb (2, 4096);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4096; ++i)
            {
                const float v = 0.4f * std::sin (0.037f * (float) i)
                              + (i % 977 == 0 ? 0.5f : 0.0f);
                ba.setSample (ch, i, v);
                bb.setSample (ch, i, v);
            }

        juce::MidiBuffer midi;
        for (int i = 0; i < 4096; i += 512)
        {
            juce::AudioBuffer<float> sa (ba.getArrayOfWritePointers(), 2, i, 512);
            juce::AudioBuffer<float> sb (bb.getArrayOfWritePointers(), 2, i, 512);
            pa->processBlock (sa, midi);
            pb->processBlock (sb, midi);
        }

        float worst = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4096; ++i)
                worst = juce::jmax (worst, std::abs (ba.getSample (ch, i)
                                                   - bb.getSample (ch, i)));
        check (worst == 0.0f,
               "EchoJay EQ: its defaults ARE the neutral settings "
               "(worst delta " + juce::String (worst, 9) + ")");
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

    // =======================================================================
    // THE HARMONIC DEPTH PASS (DEVICE_DEPTH_PLAN.md, Harmonic). The engines'
    // own g++ suites prove the DSP does what each mode claims; what is checked
    // here is the DIALABILITY — by name and by index, clamped, round-tripping
    // through state, advertised — and the neutrality of every default.
    // =======================================================================
    std::printf ("== SATURATION: emphasis dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Saturation");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* sat    = dynamic_cast<EedSaturationProcessor*> (proc.get());

        // Both is the default because it is the NEUTRAL emphasis — the curve as
        // it shipped — so an old session restores the saturation it was mixed
        // with.
        check (near (device->getParamValue ("emphasis"),
                     (double) (int) echojay::harmonic::Emphasis::Both),
               "a fresh saturation is on both, the neutral emphasis");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "emphasis", "even" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "emphasis = \"even\" applied");
        check (near (device->getParamValue ("emphasis"), 0.0), "landed on even (index 0)");
        check (s.contains ("even"), "and reads back BY NAME: " + s);
        check (sat->core().getEmphasis() == echojay::harmonic::Emphasis::Even,
               "the CORE is on even, not just the param");

        device->applyStructured (paramsMove ({ { "emphasis", "ODD" } }));
        check (near (device->getParamValue ("emphasis"), 1.0), "matching is case-insensitive");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "emphasis", "third" } }), &a2, &s2);
        check (s2 == 1, "an unknown emphasis is SKIPPED, not guessed at");
        check (near (device->getParamValue ("emphasis"), 1.0),
               "and leaves the emphasis where it was");
    }

    std::printf ("== SATURATION: oversample dials, and RETARGETS the latency ==\n");
    {
        auto proc = makeByName ("EchoJay Saturation");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("oversample"), 1.0),
               "a fresh saturation is on 4x, the neutral quality");

        proc->prepareToPlay (48000.0, 256);
        check (proc->getLatencySamples() == 45, "4x reports 45 samples");

        // A LIVE move — no re-prepare — must land on the engine AND republish
        // the latency, or the host would compensate for a number that is no
        // longer true.
        device->applyStructured (paramsMove ({ { "oversample", "8x" } }));
        check (near (device->getParamValue ("oversample"), 2.0), "oversample = \"8x\" landed");
        check (proc->getLatencySamples() == 48,
               "8x republishes 48 samples WITHOUT a re-prepare (got "
               + juce::String (proc->getLatencySamples()) + ")");

        device->applyStructured (paramsMove ({ { "oversample", 0 } }));
        check (proc->getLatencySamples() == 30, "a numeric index works too: 2x is 30 samples");

        // "16x" would coerce to the number 16 and clamp onto 8x — the funnel's
        // documented digit-salvage behaviour — so the unknown-label case is
        // tested with a label that carries no digits at all.
        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "oversample", "ultra" } }), &a2, &s2);
        check (s2 == 1, "an unadvertised quality is SKIPPED, not guessed at");
        check (proc->getLatencySamples() == 30, "and leaves the setting where it was");
    }

    std::printf ("== SATURATION: bias and hpf_hz land exactly, and clamp ==\n");
    {
        auto proc = makeByName ("EchoJay Saturation");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("bias"), 0.0),   "bias defaults to 0 (off)");
        check (near (device->getParamValue ("hpf_hz"), 0.0), "hpf_hz defaults to 0 (full band)");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "bias", -35.0 }, { "hpf_hz", 120.0 } }),
                                 &applied, &skipped);
        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (near (device->getParamValue ("bias"), -35.0),   "bias EXACTLY -35");
        check (near (device->getParamValue ("hpf_hz"), 120.0), "hpf_hz EXACTLY 120");

        device->applyStructured (paramsMove ({ { "bias", 400.0 }, { "hpf_hz", 9999.0 } }));
        check (near (device->getParamValue ("bias"), 100.0),   "bias 400 clamps to 100");
        check (near (device->getParamValue ("hpf_hz"), 500.0), "hpf_hz 9999 clamps to 500");
    }

    std::printf ("== TAPE: the machine dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Tape");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* tape   = dynamic_cast<EedTapeProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0),
               "a fresh tape is the studio machine, the neutral mode");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "mode", "cassette" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"cassette\" applied");
        check (near (device->getParamValue ("mode"), 2.0), "landed on cassette (index 2)");
        check (s.contains ("cassette"), "and reads back BY NAME: " + s);
        check (tape->engine().getMachine() == echojay::TapeMachine::Cassette,
               "the ENGINE is on cassette, not just the param");

        device->applyStructured (paramsMove ({ { "mode", "VINTAGE" } }));
        check (near (device->getParamValue ("mode"), 1.0), "matching is case-insensitive");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "dictaphone" } }), &a2, &s2);
        check (s2 == 1, "an unknown machine is SKIPPED, not guessed at");
        check (near (device->getParamValue ("mode"), 1.0), "and leaves the machine where it was");

        bool allReach = true;
        for (int i = 0; i < echojay::kNumTapeMachines; ++i)
        {
            const auto want = echojay::tapeMachineFromIndex (i);
            device->applyStructured (paramsMove ({
                { "mode", echojay::tapeMachineName (want) } }));
            allReach = allReach && tape->engine().getMachine() == want
                                && near (device->getParamValue ("mode"), (double) i);
        }
        check (allReach, "all three machines resolve by name and reach the engine");

        // The machine changes character, never the timing contract.
        proc->prepareToPlay (48000.0, 256);
        check (proc->getLatencySamples() == 165,
               "cassette or not, the latency stays 165 samples");
    }

    std::printf ("== TAPE: hiss and crosstalk land exactly, and clamp ==\n");
    {
        auto proc = makeByName ("EchoJay Tape");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("hiss"), 0.0),      "hiss defaults to 0 (off)");
        check (near (device->getParamValue ("crosstalk"), 0.0), "and so does crosstalk");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "hiss", 40.0 }, { "crosstalk", 25.0 } }),
                                 &applied, &skipped);
        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (near (device->getParamValue ("hiss"), 40.0),      "hiss EXACTLY 40");
        check (near (device->getParamValue ("crosstalk"), 25.0), "crosstalk EXACTLY 25");

        device->applyStructured (paramsMove ({ { "hiss", 900.0 }, { "crosstalk", -5.0 } }));
        check (near (device->getParamValue ("hiss"), 100.0),    "900 clamps to 100");
        check (near (device->getParamValue ("crosstalk"), 0.0), "a negative clamps to 0");
    }

    std::printf ("== EXCITER: all four characters dial, focus lands ==\n");
    {
        auto proc = makeByName ("EchoJay Exciter");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* ex     = dynamic_cast<EedExciterProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0), "a fresh exciter is tube");
        check (near (device->getParamValue ("focus"), 0.0), "and focus defaults to 0, the "
                                                            "split as shipped");

        // The two NEW characters, by name, all the way to the engine.
        device->applyStructured (paramsMove ({ { "mode", "odd" } }));
        check (near (device->getParamValue ("mode"), 2.0)
               && ex->engine().getMode() == 2, "mode = \"odd\" reaches the engine (index 2)");

        device->applyStructured (paramsMove ({ { "mode", "EVEN" } }));
        check (near (device->getParamValue ("mode"), 3.0)
               && ex->engine().getMode() == 3, "mode = \"even\" too, case-insensitively");

        // And the frozen pair still resolves exactly as before the pass.
        device->applyStructured (paramsMove ({ { "mode", "tube" } }));
        check (near (device->getParamValue ("mode"), 0.0), "tube keeps index 0");
        device->applyStructured (paramsMove ({ { "mode", "tape" } }));
        check (near (device->getParamValue ("mode"), 1.0), "tape keeps index 1");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "focus", 65.0 } }), &applied, &skipped);
        check (applied == 1 && near (device->getParamValue ("focus"), 65.0),
               "focus EXACTLY 65");
        device->applyStructured (paramsMove ({ { "focus", 300.0 } }));
        check (near (device->getParamValue ("focus"), 100.0), "300 clamps to 100");
    }

    std::printf ("== the Harmonic depth params round-trip through state ==\n");
    {
        auto a = makeByName ("EchoJay Saturation");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "emphasis", "odd" }, { "bias", -30.0 },
                                           { "hpf_hz", 90.0 }, { "oversample", "8x" } }));
        juce::MemoryBlock sb;
        da->getStateInformation (sb);

        auto b = makeByName ("EchoJay Saturation");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (sb.getData(), (int) sb.getSize());

        check (near (db->getParamValue ("emphasis"), 1.0),  "emphasis restored (odd)");
        check (near (db->getParamValue ("bias"), -30.0),    "bias restored");
        check (near (db->getParamValue ("hpf_hz"), 90.0),   "hpf_hz restored");
        check (near (db->getParamValue ("oversample"), 2.0),"oversample restored (8x)");

        auto c = makeByName ("EchoJay Tape");
        auto* dc = dynamic_cast<EedDeviceProcessor*> (c.get());
        dc->applyStructured (paramsMove ({ { "mode", "cassette" }, { "hiss", 45.0 },
                                           { "crosstalk", 20.0 } }));
        juce::MemoryBlock tb;
        dc->getStateInformation (tb);

        auto d2 = makeByName ("EchoJay Tape");
        auto* dd = dynamic_cast<EedDeviceProcessor*> (d2.get());
        dd->setStateInformation (tb.getData(), (int) tb.getSize());

        check (near (dd->getParamValue ("mode"), 2.0),      "machine restored (cassette)");
        check (near (dd->getParamValue ("hiss"), 45.0),     "hiss restored");
        check (near (dd->getParamValue ("crosstalk"), 20.0),"crosstalk restored");

        auto e = makeByName ("EchoJay Exciter");
        auto* de = dynamic_cast<EedDeviceProcessor*> (e.get());
        de->applyStructured (paramsMove ({ { "mode", "even" }, { "focus", 55.0 } }));
        juce::MemoryBlock eb;
        de->getStateInformation (eb);

        auto f = makeByName ("EchoJay Exciter");
        auto* df = dynamic_cast<EedDeviceProcessor*> (f.get());
        df->setStateInformation (eb.getData(), (int) eb.getSize());

        check (near (df->getParamValue ("mode"), 3.0),   "exciter mode restored (even)");
        check (near (df->getParamValue ("focus"), 55.0), "focus restored");
    }

    std::printf ("== the Harmonic depth choices are ADVERTISED by name ==\n");
    {
        struct Expect { const char* device; const char* id; const char* names; const char* def; };

        const Expect wanted[] = {
            { "EchoJay Saturation", "emphasis",   "even|odd|both",           "default both" },
            { "EchoJay Saturation", "oversample", "2x|4x|8x",                "default 4x" },
            { "EchoJay Tape",       "mode",       "studio|vintage|cassette", "default studio" },
            { "EchoJay Exciter",    "mode",       "tube|tape|odd|even",      "default tube" },
        };

        for (const auto& w : wanted)
        {
            const auto* d = registry.findByName (w.device);
            const auto* spec = d != nullptr ? d->schema.find (w.id) : nullptr;
            if (spec == nullptr)
            {
                check (false, juce::String (w.device) + " advertises " + w.id);
                continue;
            }
            const auto line = juce::String (echojay::ParamSchema::describeLine (*spec));
            check (line.contains (w.names) && line.contains (w.def),
                   juce::String (w.device) + " " + w.id + ": " + line);
        }
    }

    std::printf ("== a Harmonic device at its DEFAULTS is unchanged by the depth pass ==\n");
    {
        // The strongest guarantee this pass makes: every new param's default is
        // the device as it shipped, so an existing session sounds identical.
        // Rendered through the REAL processBlock, defaults against explicit
        // neutral, and the delta must be EXACTLY zero — the neutral paths are
        // gated, not merely quiet.
        for (const char* name : { "EchoJay Saturation", "EchoJay Tape", "EchoJay Exciter" })
        {
            auto mk = [name] (bool explicitNeutral)
            {
                auto proc = makeByName (name);
                auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
                if (explicitNeutral)
                {
                    const juce::String n (name);
                    if (n == "EchoJay Saturation")
                        device->applyStructured (paramsMove ({
                            { "emphasis", "both" }, { "bias", 0.0 },
                            { "hpf_hz", 0.0 }, { "oversample", "4x" } }));
                    else if (n == "EchoJay Tape")
                        device->applyStructured (paramsMove ({
                            { "mode", "studio" }, { "hiss", 0.0 }, { "crosstalk", 0.0 } }));
                    else
                        device->applyStructured (paramsMove ({
                            { "mode", "tube" }, { "focus", 0.0 } }));
                }
                proc->setPlayConfigDetails (2, 2, 48000.0, 512);
                proc->prepareToPlay (48000.0, 512);
                return proc;
            };

            auto pa = mk (false), pb = mk (true);

            juce::AudioBuffer<float> ba (2, 4096), bb (2, 4096);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                {
                    const float v = 0.4f * std::sin (0.037f * (float) i)
                                  + (i % 977 == 0 ? 0.5f : 0.0f);
                    ba.setSample (ch, i, v);
                    bb.setSample (ch, i, v);
                }

            juce::MidiBuffer midi;
            for (int i = 0; i < 4096; i += 512)
            {
                juce::AudioBuffer<float> sa (ba.getArrayOfWritePointers(), 2, i, 512);
                juce::AudioBuffer<float> sb (bb.getArrayOfWritePointers(), 2, i, 512);
                pa->processBlock (sa, midi);
                pb->processBlock (sb, midi);
            }

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                    worst = juce::jmax (worst, std::abs (ba.getSample (ch, i)
                                                       - bb.getSample (ch, i)));

            check (worst == 0.0f,
                   juce::String (name) + ": its defaults ARE the neutral settings "
                   "(worst delta " + juce::String (worst, 9) + ")");
        }
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
    // The goniometer is only worth having if it shows the device's OUTPUT. A tap
    // taken before the engine would look perfectly healthy on a stereo source
    // and would be a lie on a mono one: mono in, mono out, a vertical line, no
    // matter how far the Haas dial is turned.
    //
    // So the check is the mono case specifically. Feed L == R, and the ring must
    // come back DECORRELATED — side content the device manufactured, which is
    // the widening made visible. Then turn the widening off and the same input
    // must come back perfectly correlated, which is what rules out the tap
    // simply being noisy.
    std::printf ("== the Stereoizer's scope tap carries the WIDENED output ==\n");
    {
        // Correlation of the ring's two channels, the same measure the
        // goniometer's bar shows.
        auto ringCorrelation = [] (const echojay::viz::ScopeTap& tap)
        {
            std::vector<float> l (1024), r (1024);
            const int n = tap.read (l.data(), r.data(), 1024);
            if (n <= 0) return 2.0f;                    // sentinel: nothing tapped

            double lr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = 0; i < n; ++i)
            {
                lr += (double) l[i] * r[i];
                ll += (double) l[i] * l[i];
                rr += (double) r[i] * r[i];
            }
            if (ll + rr < 1.0e-9) return 2.0f;          // sentinel: silence
            const double d = std::sqrt (ll * rr);
            return d > 1.0e-12 ? (float) (lr / d) : 1.0f;
        };

        // A MONO source: both channels identical, so every scrap of side content
        // in the tap was made by this device.
        auto runMono = [] (juce::AudioProcessor& p)
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            juce::Random rng (99);
            for (int b = 0; b < 24; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = rng.nextFloat() * 1.6f - 0.8f;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }
                p.processBlock (buf, midi);
            }
        };

        {
            auto proc = makeByName ("EchoJay Stereoizer");
            auto* sz = dynamic_cast<EedStereoizerProcessor*> (proc.get());
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            check (sz != nullptr, "constructed as an EedStereoizerProcessor");

            device->applyStructured (paramsMove ({ { "width", 150.0 }, { "haas_ms", 20.0 },
                                                  { "mono_maker_hz", 0.0 }, { "mix", 100.0 } }));
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);
            runMono (*proc);

            const float c = ringCorrelation (sz->scopeTap());
            check (c < 0.99f && c <= 1.0f,
                   "mono in, DECORRELATED in the ring: the widening is visible "
                   "(correlation " + juce::String (c, 4) + ")");
        }
        {
            // Same input, widening off. If this did not come back at +1 the tap
            // would be measuring something other than the signal.
            auto proc = makeByName ("EchoJay Stereoizer");
            auto* sz = dynamic_cast<EedStereoizerProcessor*> (proc.get());
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

            device->applyStructured (paramsMove ({ { "width", 100.0 }, { "haas_ms", 0.0 },
                                                  { "mono_maker_hz", 0.0 }, { "mix", 100.0 } }));
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);
            runMono (*proc);

            const float c = ringCorrelation (sz->scopeTap());
            check (c > 0.999f && c <= 1.0f,
                   "widening off: the same mono input reads as mono "
                   "(correlation " + juce::String (c, 4) + ")");
        }
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
    // The editor-paint harness below proves a visualisation does not CRASH. It
    // cannot prove one is telling the truth: a meter wired to the wrong tap, or
    // reading the input where it meant the output, paints perfectly and is
    // simply wrong. Most views are analytic and cannot drift (they read the same
    // schema the AI writes), but the float-tap paths are real plumbing, so the
    // numbers they publish are checked against signal that was actually pushed
    // through the device.
    std::printf ("== Gain's I/O meter taps read the real signal ==\n");
    {
        auto proc = makeByName ("EchoJay Gain");
        auto* gain = dynamic_cast<EedGainProcessor*> (proc.get());
        check (gain != nullptr, "constructed as an EedGainProcessor");

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);

        // Full-scale DC is the cleanest probe: peak and RMS are both exactly the
        // amplitude, so a wrong answer is unambiguous rather than approximately
        // right. Pan stays centred, which GainEngine defines as unity.
        auto pushBlocks = [&proc] (float amp, int blocks)
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::fill (buf.getWritePointer (ch), amp, 512);
                proc->processBlock (buf, midi);
            }
        };

        // Unity: in and out must agree, or the two taps are crossed.
        gain->setParamValue ("level_db", 0.0);
        gain->setParamValue ("pan", 0.0);
        proc->prepareToPlay (48000.0, 512);
        pushBlocks (0.5f, 40);                     // 40 blocks >> the 20 ms smoother

        check (near (gain->inputPeak(),  0.5, 1e-3), "input peak reads the input amplitude");
        check (near (gain->inputRms(),   0.5, 1e-3), "input RMS reads it too (DC)");
        check (near (gain->outputPeak(), 0.5, 1e-3), "at 0 dB the output matches the input");

        // -6 dB: the output tap must move and the input tap must NOT. This is
        // the check that actually distinguishes the two taps from each other.
        gain->setParamValue ("level_db", -6.0);
        pushBlocks (0.5f, 60);

        check (near (gain->inputPeak(), 0.5, 1e-3), "the INPUT tap is unchanged by the gain");
        check (near (gain->outputPeak(), 0.5 * 0.50119, 2e-3),
               "the OUTPUT tap halved: " + juce::String (gain->outputPeak(), 4));

        // Silence has to reach the meter, or a stopped transport leaves a bar
        // sitting at whatever was last playing.
        pushBlocks (0.0f, 60);
        check (gain->inputPeak() < 1e-6 && gain->outputPeak() < 1e-6,
               "silence reads as silence on both meters");
    }

    std::printf ("== Phase Invert's correlation dot reads the real phase ==\n");
    {
        auto proc = makeByName ("EchoJay Phase Invert");
        auto* pi = dynamic_cast<EedPhaseInvertProcessor*> (proc.get());
        check (pi != nullptr, "constructed as an EedPhaseInvertProcessor");

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        // Identical channels: correlation +1. Blocks enough to clear both the
        // 5 ms polarity ramp and the display smoother.
        auto pushCorrelated = [&proc] (int blocks)
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float x = std::sin ((float) (b * 512 + i) * 0.03f);
                    buf.setSample (0, i, x);
                    buf.setSample (1, i, x);
                }
                proc->processBlock (buf, midi);
            }
        };

        pushCorrelated (60);
        check (near (pi->correlation(), 1.0, 0.02),
               "an identical pair reads +1 (" + juce::String (pi->correlation(), 3) + ")");

        // THE reading this device exists to move: flip one side and the pair
        // goes to -1, which is the mono fold-down cancelling.
        pi->setParamValue ("invert_left", 1.0);
        pushCorrelated (60);
        check (near (pi->correlation(), -1.0, 0.02),
               "flipping ONE side swings it to -1 (" + juce::String (pi->correlation(), 3) + ")");

        // Flipping BOTH is not a phase problem — it is the same signal again.
        pi->setParamValue ("invert_right", 1.0);
        pushCorrelated (60);
        check (near (pi->correlation(), 1.0, 0.02),
               "flipping BOTH is back to +1 (" + juce::String (pi->correlation(), 3) + ")");

        // Mono: the sentinel, not a number. A 0 here would draw as "fully
        // decorrelated" on a device that simply has nothing to compare.
        auto mono = makeByName ("EchoJay Phase Invert");
        auto* pim = dynamic_cast<EedPhaseInvertProcessor*> (mono.get());
        mono->setPlayConfigDetails (1, 1, 48000.0, 256);
        mono->prepareToPlay (48000.0, 256);
        {
            juce::AudioBuffer<float> buf (1, 256);
            juce::MidiBuffer midi;
            for (int b = 0; b < 8; ++b)
            {
                for (int i = 0; i < 256; ++i) buf.setSample (0, i, 0.4f);
                mono->processBlock (buf, midi);
            }
        }
        check (pim->correlation() == EedPhaseInvertProcessor::kNoCorrelation,
               "mono publishes the no-reading sentinel, not 0");
        check (std::abs (EedPhaseInvertProcessor::kNoCorrelation) > 1.0f,
               "and that sentinel is OUTSIDE the valid range, so a drawer can spot it");
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

    // =======================================================================
    // THE DEPTH PASS (DEVICE_DEPTH_PLAN.md, Dynamics). Every mode and knob it
    // added has to be dialable BY NAME, clamp, round-trip through state and — for
    // the ones that cost latency — report it. A control that is clickable but not
    // dialable is the one failure this whole framework exists to make impossible,
    // so each new param is exercised here rather than assumed from its schema
    // entry.
    // =======================================================================
    std::printf ("== COMPRESSOR: the character mode dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* comp   = dynamic_cast<EedCompressorProcessor*> (proc.get());
        check (device != nullptr && comp != nullptr, "compressor constructs");

        // Both of these come from the SCHEMA's defaults now rather than from a
        // line in the constructor, so a fresh device is described in one place —
        // and this is the check that the one place is actually consulted.
        check (near (device->getParamValue ("mode"), 0.0), "a fresh compressor is clean");
        check (near (device->getParamValue ("detector"), 1.0), "and detects RMS");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "mode", "punch" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"punch\" applied");
        check (near (device->getParamValue ("mode"), 2.0), "landed on punch (index 2)");
        check (s.contains ("punch"), "and reads back BY NAME: " + s);

        device->applyStructured (paramsMove ({ { "mode", "GLUE" } }));
        check (near (device->getParamValue ("mode"), 1.0), "matching is case-insensitive");

        device->applyStructured (paramsMove ({ { "mode", 3 } }));
        check (near (device->getParamValue ("mode"), 3.0), "a numeric index still works");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "vari-mu" } }), &a2, &s2);
        check (s2 == 1, "an unknown character is SKIPPED, not guessed at");
        check (near (device->getParamValue ("mode"), 3.0), "and leaves the mode where it was");

        // THE MODE MUST REACH THE DSP, not just the getter. Each one reshapes the
        // dialled times, so the effective attack is the observable proof.
        device->applyStructured (paramsMove ({ { "attack_ms", 10.0 },
                                               { "release_ms", 100.0 } }));

        device->applyStructured (paramsMove ({ { "mode", "clean" } }));
        const double cleanAtk = comp->effectiveAttackMs();

        device->applyStructured (paramsMove ({ { "mode", "punch" } }));
        const double punchAtk = comp->effectiveAttackMs();

        device->applyStructured (paramsMove ({ { "mode", "smooth" } }));
        const double smoothAtk = comp->effectiveAttackMs();

        check (near (cleanAtk, 10.0, 1e-9), "clean runs the dialled 10 ms attack");
        check (punchAtk < cleanAtk, "punch runs faster (" + juce::String (punchAtk, 2) + " ms)");
        check (smoothAtk > cleanAtk, "smooth runs slower (" + juce::String (smoothAtk, 2) + " ms)");
        check (near (device->getParamValue ("attack_ms"), 10.0),
               "and the DIALLED attack still round-trips as 10, whatever the mode");
    }

    std::printf ("== COMPRESSOR: every depth param lands exactly, and clamps ==\n");
    {
        auto proc = makeByName ("EchoJay Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        proc->prepareToPlay (48000.0, 512);

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "sc_hpf_hz", 120.0 },
                                               { "lookahead_ms", 4.0 },
                                               { "auto_release", true },
                                               { "detector", "peak" },
                                               { "stereo_link", 60.0 },
                                               { "range_db", 8.0 } }), &applied, &skipped);

        check (applied == 6 && skipped == 0, "6 params applied, 0 skipped");
        check (near (device->getParamValue ("sc_hpf_hz"), 120.0),   "sc_hpf_hz EXACTLY 120");
        check (near (device->getParamValue ("lookahead_ms"), 4.0),  "lookahead_ms EXACTLY 4");
        check (near (device->getParamValue ("auto_release"), 1.0),  "auto_release is on");
        check (near (device->getParamValue ("detector"), 0.0),      "detector = \"peak\" resolved");
        check (near (device->getParamValue ("stereo_link"), 60.0),  "stereo_link EXACTLY 60 (percent)");
        check (near (device->getParamValue ("range_db"), 8.0),      "range_db EXACTLY 8");

        // rms by name too, since it is the default and the one a model will send
        // most often.
        device->applyStructured (paramsMove ({ { "detector", "rms" } }));
        check (near (device->getParamValue ("detector"), 1.0), "detector = \"rms\" resolved");

        device->applyStructured (paramsMove ({ { "sc_hpf_hz", 5000.0 },
                                               { "stereo_link", 250.0 },
                                               { "range_db", -5.0 } }));
        check (near (device->getParamValue ("sc_hpf_hz"), 500.0),   "5 kHz clamped to the 500 max");
        check (near (device->getParamValue ("stereo_link"), 100.0), "250% clamped to 100");
        check (near (device->getParamValue ("range_db"), 0.0),      "a negative range clamped to 0");
    }

    std::printf ("== COMPRESSOR / GATE / EXPANDER report their lookahead latency ==\n");
    {
        // The number the DAW needs to keep this track in time with every other
        // one. Every face that publishes lookahead now owes it, not just the
        // limiter — an unreported delay is a few milliseconds of drift that reads
        // as a mix problem rather than as a bug.
        for (const char* name : { "EchoJay Compressor", "EchoJay Gate", "EchoJay Expander" })
        {
            auto proc = makeByName (name);
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

            proc->prepareToPlay (48000.0, 512);
            check (proc->getLatencySamples() == 0,
                   juce::String (name) + " defaults to ZERO latency");

            device->applyStructured (paramsMove ({ { "lookahead_ms", 5.0 } }));
            check (proc->getLatencySamples() == 240,
                   juce::String (name) + ": 5 ms at 48k reports 240 samples (got "
                   + juce::String (proc->getLatencySamples()) + ")");

            device->applyStructured (paramsMove ({ { "lookahead_ms", 0.0 } }));
            check (proc->getLatencySamples() == 0,
                   juce::String (name) + ": back to zero lookahead, zero latency");
        }
    }

    std::printf ("== a lookahead device with NO lookahead is a true pass-through ==\n");
    {
        // The bug this pins had no visible symptom: the ring was read before it
        // was written, so a delay of 0 came out as a delay of the whole buffer
        // while the device honestly reported zero latency. One track 10 ms late,
        // only when its lookahead happened to be dialled to nothing.
        auto proc = makeByName ("EchoJay Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        // Threshold at 0 dB so nothing is compressed: what is under test is the
        // signal PATH, not the gain.
        device->applyStructured (paramsMove ({ { "lookahead_ms", 0.0 },
                                               { "threshold_db", 0.0 },
                                               { "mode", "clean" } }));
        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        buf.setSample (0, 0, 1.0f);       // an impulse in the first sample
        buf.setSample (1, 0, 1.0f);

        juce::MidiBuffer midi;
        proc->processBlock (buf, midi);

        check (buf.getSample (0, 0) > 0.9f,
               "the impulse comes straight back out (" + juce::String (buf.getSample (0, 0), 4) + ")");
    }

    std::printf ("== BYPASS still delays, so toggling it cannot shift the track ==\n");
    {
        auto proc = makeByName ("EchoJay Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        device->applyStructured (paramsMove ({ { "lookahead_ms", 2.0 } }));
        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);
        device->setBypassed (true);

        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        buf.setSample (0, 0, 1.0f);
        buf.setSample (1, 0, 1.0f);

        juce::MidiBuffer midi;
        proc->processBlock (buf, midi);

        // 2 ms at 48k is 96 samples: the host is compensating for that whether or
        // not the device is bypassed.
        check (buf.getSample (0, 0) < 0.01f, "the impulse did NOT come back early");
        check (buf.getSample (0, 96) > 0.9f,
               "it emerged at sample 96, exactly the reported latency");
    }

    std::printf ("== LIMITER: three modes, and clip is a HARD ceiling ==\n");
    {
        auto proc = makeByName ("EchoJay Limiter");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0), "defaults to transparent");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "mode", "clip" },
                                                             { "true_peak", true },
                                                             { "sc_hpf_hz", 40.0 } }),
                                                &applied, &skipped);
        check (applied == 3 && skipped == 0, "3 params applied, 0 skipped");
        check (near (device->getParamValue ("mode"), 2.0), "mode = \"clip\" landed on index 2");
        check (s.contains ("clip"), "and reads back by name: " + s);
        check (near (device->getParamValue ("true_peak"), 1.0), "true_peak is on");
        check (near (device->getParamValue ("sc_hpf_hz"), 40.0), "sc_hpf_hz EXACTLY 40");

        device->applyStructured (paramsMove ({ { "mode", "punchy" } }));
        check (near (device->getParamValue ("mode"), 1.0), "and punchy by name");

        // CLIP ignores the lookahead, and must therefore report NO latency — a
        // device that delays without a reason is a device that is wrong.
        device->applyStructured (paramsMove ({ { "lookahead_ms", 5.0 } }));
        proc->prepareToPlay (48000.0, 512);
        check (proc->getLatencySamples() == 240, "punchy honours the 5 ms lookahead");

        device->applyStructured (paramsMove ({ { "mode", "clip" } }));
        check (proc->getLatencySamples() == 0, "clip reports ZERO latency despite the 5 ms");
        check (near (device->getParamValue ("lookahead_ms"), 5.0),
               "while the dialled 5 ms is REMEMBERED, not destroyed");

        device->applyStructured (paramsMove ({ { "mode", "transparent" } }));
        check (proc->getLatencySamples() == 240, "and comes back when the mode does");

        // The release survives clip too, for the same reason: clip drives the
        // core's release to zero, so the dialled value has to live elsewhere.
        device->applyStructured (paramsMove ({ { "release_ms", 300.0 } }));
        device->applyStructured (paramsMove ({ { "mode", "clip" } }));
        check (near (device->getParamValue ("release_ms"), 300.0),
               "the dialled release survives a trip through clip");
    }

    std::printf ("== LIMITER: clip actually holds the ceiling, sample by sample ==\n");
    {
        auto proc = makeByName ("EchoJay Limiter");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        // -6 dB ceiling, a full-scale sine going in: nothing may come out above
        // the ceiling, including the very first sample. A limiter with an attack
        // lets the front of the first cycle past; a clip does not, and that is
        // the whole difference between the two.
        device->applyStructured (paramsMove ({ { "mode", "clip" },
                                               { "ceiling_db", -6.0 },
                                               { "true_peak", false } }));
        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;

        float worst = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, std::sin (0.11f * (float) (b * 512 + i)));

            proc->processBlock (buf, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    worst = juce::jmax (worst, std::abs (buf.getSample (ch, i)));
        }

        const float ceiling = std::pow (10.0f, -6.0f / 20.0f);
        check (worst <= ceiling * 1.001f,
               "nothing escaped the -6 dB ceiling (worst "
               + juce::String (juce::Decibels::gainToDecibels (worst), 3) + " dBFS)");
        check (worst > ceiling * 0.9f, "and it did reach it, so the test is not measuring silence");
    }

    std::printf ("== GATE: duck is the same device, the other way up ==\n");
    {
        auto proc = makeByName ("EchoJay Gate");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* gate   = dynamic_cast<EedGateProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"), 0.0), "defaults to gate");
        check (gate != nullptr && ! gate->isDucking(), "and the DSP agrees it is gating");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "mode", "duck" } }),
                                                &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"duck\" applied");
        check (near (device->getParamValue ("mode"), 1.0), "landed on index 1");
        check (s.contains ("duck"), "and reads back by name: " + s);
        check (gate->isDucking(), "the DSP is now in Duck mode, not merely the param");

        device->applyStructured (paramsMove ({ { "mode", 0 } }));
        check (! gate->isDucking(), "and index 0 puts it back");

        // The selective-trigger pair.
        device->applyStructured (paramsMove ({ { "sc_hpf_hz", 80.0 },
                                               { "sc_lpf_hz", 250.0 } }));
        check (near (device->getParamValue ("sc_hpf_hz"), 80.0),  "sc_hpf_hz EXACTLY 80");
        check (near (device->getParamValue ("sc_lpf_hz"), 250.0), "sc_lpf_hz EXACTLY 250");

        device->applyStructured (paramsMove ({ { "sc_lpf_hz", 10.0 } }));
        check (near (device->getParamValue ("sc_lpf_hz"), 200.0),
               "and 10 Hz clamps up to the advertised 200 floor");
    }

    std::printf ("== GATE: ducking a loud signal actually reduces it ==\n");
    {
        // End to end through processBlock, because "the param changed" and "the
        // audio changed" are two different claims and only the second one matters.
        auto grFor = [] (const char* mode)
        {
            auto proc = makeByName ("EchoJay Gate");
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            auto* gate   = dynamic_cast<EedGateProcessor*> (proc.get());

            device->applyStructured (paramsMove ({ { "mode", mode },
                                                   { "threshold_db", -30.0 },
                                                   { "range_db", 12.0 },
                                                   { "hysteresis_db", 0.0 },
                                                   { "attack_ms", 0.5 },
                                                   { "hold_ms", 0.0 },
                                                   { "release_ms", 10.0 } }));
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            for (int b = 0; b < 20; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        buf.setSample (ch, i, 0.5f);      // -6 dBFS, well over
                proc->processBlock (buf, midi);
            }
            return gate->gainReductionDb();
        };

        check (near (grFor ("gate"), 0.0, 0.1),
               "a loud signal passes the gate untouched (" + juce::String (grFor ("gate"), 2) + " dB)");
        check (near (grFor ("duck"), -12.0, 0.5),
               "and is ducked by the range (" + juce::String (grFor ("duck"), 2) + " dB)");
    }

    std::printf ("== EXPANDER: the two depth params land and clamp ==\n");
    {
        auto proc = makeByName ("EchoJay Expander");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "sc_hpf_hz", 90.0 },
                                               { "lookahead_ms", 3.0 } }), &applied, &skipped);
        check (applied == 2 && skipped == 0, "2 params applied, 0 skipped");
        check (near (device->getParamValue ("sc_hpf_hz"), 90.0),   "sc_hpf_hz EXACTLY 90");
        check (near (device->getParamValue ("lookahead_ms"), 3.0), "lookahead_ms EXACTLY 3");

        device->applyStructured (paramsMove ({ { "lookahead_ms", 99.0 } }));
        check (near (device->getParamValue ("lookahead_ms"),
                     EedExpanderProcessor::kMaxLookaheadMs),
               "99 ms clamped to the buffer's maximum - never reallocated");
    }

    std::printf ("== DE-ESSER: auto_threshold tracks, and keeps the dialled one safe ==\n");
    {
        auto proc = makeByName ("EchoJay De-Esser");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* de     = dynamic_cast<EedDeEsserProcessor*> (proc.get());
        check (de != nullptr, "de-esser constructs");

        check (near (device->getParamValue ("auto_threshold"), 0.0), "defaults to off");

        device->applyStructured (paramsMove ({ { "threshold_db", -34.0 } }));
        check (near (de->effectiveThresholdDb(), -34.0, 0.01),
               "with auto off, the DSP compares against the dialled threshold");

        device->applyStructured (paramsMove ({ { "auto_threshold", "on" } }));
        check (near (device->getParamValue ("auto_threshold"), 1.0), "the string \"on\" -> on");
        check (near (device->getParamValue ("threshold_db"), -34.0),
               "and the DIALLED threshold is untouched while it is ignored");

        // Push sibilant-ish content through and the tracked threshold has to move
        // off the dialled value — that is the whole feature.
        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < 60; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.5f * std::sin (2.0f * 3.14159265f * 6500.0f
                                                           * (float) (b * 512 + i) / 48000.0f));
            proc->processBlock (buf, midi);
        }

        const float tracked = de->effectiveThresholdDb();
        check (std::abs (tracked - (-34.0f)) > 1.0f,
               "the threshold tracked the band instead ("
               + juce::String (tracked, 2) + " dB)");
        check (near (device->getParamValue ("threshold_db"), -34.0),
               "and STILL round-trips as the -34 that was dialled");

        // Switching auto off has to hand the dial back at once.
        device->applyStructured (paramsMove ({ { "auto_threshold", false } }));
        check (near (de->effectiveThresholdDb(), -34.0, 0.01),
               "switching it off restores the dialled threshold immediately");
    }

    std::printf ("== 4-BAND: per-band character, by name, flat AND via comp_bands ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* mb     = dynamic_cast<EedMultibandProcessor*> (proc.get());

        // The per-band DEFAULTS are not all the same, on purpose: a low band and
        // an air band want different characters, and four `clean` bands would be a
        // marquee control shipped switched off.
        check (near (device->getParamValue ("band1_mode"), 1.0), "band 1 defaults to glue");
        check (near (device->getParamValue ("band4_mode"), 2.0), "band 4 defaults to punch");
        check (near (device->getParamValue ("band2_mode"), 0.0), "and the mids to clean");

        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (
            paramsMove ({ { "band3_mode", "smooth" } }), &applied, &skipped);
        check (applied == 1 && skipped == 0, "band3_mode = \"smooth\" applied");
        check (near (device->getParamValue ("band3_mode"), 3.0), "landed on index 3");
        check (s.contains ("smooth"), "and reads back by name: " + s);
        check (mb->bandCharacter (2) == echojay::CharacterMode::Smooth,
               "the band's CORE is in smooth, not just the param");

        // It has to reach the knee too, or the mode is a label on nothing.
        device->applyStructured (paramsMove ({ { "band3_knee_db", 6.0 } }));
        check (mb->bandEffectiveKneeDb (2) > 6.0f,
               "smooth widened band 3's knee to "
               + juce::String (mb->bandEffectiveKneeDb (2), 1) + " dB");

        // The array form, which is how a model thinking in bands will send it.
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("band", 1);
        e->setProperty ("mode", "punch");
        juce::Array<juce::var> arr;
        arr.add (juce::var (e.get()));

        int a2 = 0, s2 = 0;
        device->applyStructured (juce::var (arr), &a2, &s2);
        check (a2 == 1 && s2 == 0, "comp_bands can set a band's mode too");
        check (near (device->getParamValue ("band1_mode"), 2.0), "band 1 is now punch");

        // ...and the "character" spelling, since that is the other word for it.
        juce::DynamicObject::Ptr e2 = new juce::DynamicObject();
        e2->setProperty ("band", 2);
        e2->setProperty ("character", "glue");
        juce::Array<juce::var> arr2;
        arr2.add (juce::var (e2.get()));
        device->applyStructured (juce::var (arr2));
        check (near (device->getParamValue ("band2_mode"), 1.0),
               "\"character\" is accepted as a spelling of the same knob");
    }

    std::printf ("== 4-BAND: the detector is GLOBAL, and reaches every band ==\n");
    {
        auto proc = makeByName ("EchoJay 4-Band Compressor");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("detector"), 1.0), "defaults to rms");

        device->applyStructured (paramsMove ({ { "detector", "peak" } }));
        check (near (device->getParamValue ("detector"), 0.0), "peak by name");

        // A per-band spelling must be REFUSED, not silently absorbed: this is one
        // setting for the device, and a band1_detector that appeared to work would
        // be a control that does nothing.
        int applied = 0, skipped = 0;
        const auto s = device->applyStructured (paramsMove ({ { "band1_detector", "rms" } }),
                                                &applied, &skipped);
        check (applied == 0 && skipped == 1, "band1_detector is refused, not guessed at");
        check (s.contains ("band1_detector"), "and named in the summary: " + s);
    }

    std::printf ("== the depth params round-trip through state ==\n");
    {
        // Every new knob at a non-default value, saved and restored. The base's
        // state path is written in terms of the schema, so this is really a check
        // that each param is fully wired: an id that only half exists (settable
        // but not gettable) saves as 0 and comes back wrong.
        auto a = makeByName ("EchoJay Compressor");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "mode", "smooth" }, { "detector", "peak" },
                                           { "sc_hpf_hz", 90.0 }, { "lookahead_ms", 3.5 },
                                           { "auto_release", true }, { "stereo_link", 40.0 },
                                           { "range_db", 6.0 } }));

        juce::MemoryBlock blob;
        da->getStateInformation (blob);

        auto b = makeByName ("EchoJay Compressor");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (blob.getData(), (int) blob.getSize());

        check (near (db->getParamValue ("mode"), 3.0),          "mode restored (smooth)");
        check (near (db->getParamValue ("detector"), 0.0),      "detector restored (peak)");
        check (near (db->getParamValue ("sc_hpf_hz"), 90.0),    "sc_hpf_hz restored");
        check (near (db->getParamValue ("lookahead_ms"), 3.5),  "lookahead_ms restored");
        check (near (db->getParamValue ("auto_release"), 1.0),  "auto_release restored");
        check (near (db->getParamValue ("stereo_link"), 40.0),  "stereo_link restored");
        check (near (db->getParamValue ("range_db"), 6.0),      "range_db restored");

        // And the other faces' new params, on the same path.
        auto lim = makeByName ("EchoJay Limiter");
        auto* dl = dynamic_cast<EedDeviceProcessor*> (lim.get());
        dl->applyStructured (paramsMove ({ { "mode", "clip" }, { "true_peak", true },
                                           { "sc_hpf_hz", 30.0 } }));
        juce::MemoryBlock lb;
        dl->getStateInformation (lb);

        auto lim2 = makeByName ("EchoJay Limiter");
        auto* dl2 = dynamic_cast<EedDeviceProcessor*> (lim2.get());
        dl2->setStateInformation (lb.getData(), (int) lb.getSize());
        check (near (dl2->getParamValue ("mode"), 2.0),      "limiter mode restored (clip)");
        check (near (dl2->getParamValue ("true_peak"), 1.0), "true_peak restored");
        check (near (dl2->getParamValue ("sc_hpf_hz"), 30.0),"limiter sc_hpf_hz restored");

        auto gt = makeByName ("EchoJay Gate");
        auto* dg = dynamic_cast<EedDeviceProcessor*> (gt.get());
        dg->applyStructured (paramsMove ({ { "mode", "duck" }, { "sc_hpf_hz", 70.0 },
                                           { "sc_lpf_hz", 400.0 }, { "lookahead_ms", 2.0 } }));
        juce::MemoryBlock gb;
        dg->getStateInformation (gb);

        auto gt2 = makeByName ("EchoJay Gate");
        auto* dg2 = dynamic_cast<EedDeviceProcessor*> (gt2.get());
        dg2->setStateInformation (gb.getData(), (int) gb.getSize());
        check (near (dg2->getParamValue ("mode"), 1.0),         "gate mode restored (duck)");
        check (near (dg2->getParamValue ("sc_lpf_hz"), 400.0),  "sc_lpf_hz restored");
        check (near (dg2->getParamValue ("lookahead_ms"), 2.0), "gate lookahead restored");

        auto de = makeByName ("EchoJay De-Esser");
        auto* dd = dynamic_cast<EedDeviceProcessor*> (de.get());
        dd->applyStructured (paramsMove ({ { "auto_threshold", true },
                                           { "threshold_db", -22.0 } }));
        juce::MemoryBlock deb;
        dd->getStateInformation (deb);

        auto de2 = makeByName ("EchoJay De-Esser");
        auto* dd2 = dynamic_cast<EedDeviceProcessor*> (de2.get());
        dd2->setStateInformation (deb.getData(), (int) deb.getSize());
        check (near (dd2->getParamValue ("auto_threshold"), 1.0), "auto_threshold restored");
        check (near (dd2->getParamValue ("threshold_db"), -22.0),
               "and the dialled threshold under it survived the trip");

        auto mb = makeByName ("EchoJay 4-Band Compressor");
        auto* dm = dynamic_cast<EedDeviceProcessor*> (mb.get());
        dm->applyStructured (paramsMove ({ { "band2_mode", "punch" }, { "detector", "peak" } }));
        juce::MemoryBlock mbb;
        dm->getStateInformation (mbb);

        auto mb2 = makeByName ("EchoJay 4-Band Compressor");
        auto* dm2 = dynamic_cast<EedDeviceProcessor*> (mb2.get());
        dm2->setStateInformation (mbb.getData(), (int) mbb.getSize());
        check (near (dm2->getParamValue ("band2_mode"), 2.0), "band2_mode restored (punch)");
        check (near (dm2->getParamValue ("detector"), 0.0),   "the global detector restored");
    }

    std::printf ("== the depth pass's choices are ADVERTISED by name ==\n");
    {
        // The advertisement is the whole contract: a choice the model cannot read
        // is a choice it cannot set, however well the apply path resolves names.
        struct Expect { const char* device; const char* id; const char* names; const char* def; };

        const Expect wanted[] = {
            { "EchoJay Compressor",        "mode",       "clean|glue|punch|smooth",  "default clean" },
            { "EchoJay Compressor",        "detector",   "peak|rms",                 "default rms" },
            { "EchoJay Limiter",           "mode",       "transparent|punchy|clip",  "default transparent" },
            { "EchoJay Gate",              "mode",       "gate|duck",                "default gate" },
            { "EchoJay 4-Band Compressor", "band1_mode", "clean|glue|punch|smooth",  "default glue" },
            { "EchoJay 4-Band Compressor", "detector",   "peak|rms",                 "default rms" },
            { "EchoJay Stereo Width",      "mode",       "full|multiband",           "default full" },
            { "EchoJay Stereoizer",        "mode",       "haas|comb|dimension",      "default haas" },
            { "EchoJay Gain",              "mode",       "stereo|mid_side",          "default stereo" },
        };

        for (const auto& w : wanted)
        {
            const auto* d = registry.findByName (w.device);
            const auto* spec = d != nullptr ? d->schema.find (w.id) : nullptr;
            if (spec == nullptr)
            {
                check (false, juce::String (w.device) + " advertises " + w.id);
                continue;
            }

            const auto line = juce::String (echojay::ParamSchema::describeLine (*spec));
            check (line.contains (w.names) && line.contains (w.def),
                   juce::String (w.device) + " " + w.id + ": " + line);
        }

        // And the booleans read as on/off rather than as a 0..1 range.
        for (const auto& p : std::initializer_list<std::pair<const char*, const char*>> {
                 { "EchoJay Compressor", "auto_release" },
                 { "EchoJay Limiter",    "true_peak" },
                 { "EchoJay De-Esser",   "auto_threshold" },
                 { "EchoJay Gain",       "mono" },
                 { "EchoJay Gain",       "phase_left" },
                 { "EchoJay Gain",       "phase_right" } })
        {
            const auto* d = registry.findByName (p.first);
            const auto* spec = d != nullptr ? d->schema.find (p.second) : nullptr;
            const auto line = spec != nullptr
                            ? juce::String (echojay::ParamSchema::describeLine (*spec))
                            : juce::String();
            check (line.contains ("on/off"),
                   juce::String (p.first) + " " + p.second + " is advertised on/off: " + line);
        }
    }

    // =======================================================================
    // THE STEREO + UTILITY DEPTH PASS (DEVICE_DEPTH_PLAN.md, Stereo/Utility).
    // The engines' own g++ tests own the DSP claims (band isolation, flat
    // crossover sums, mono fold-down per mode, the M/S algebra); what belongs
    // here is the dialability of it all — modes by name AND index, exact
    // landings, clamps, state, and the neutrality of the defaults.
    // =======================================================================
    std::printf ("== STEREO WIDTH: the mode dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Stereo Width");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* sw     = dynamic_cast<EedStereoWidthProcessor*> (proc.get());
        check (sw != nullptr, "constructed as an EedStereoWidthProcessor");

        check (near (device->getParamValue ("mode"),
                     (double) (int) echojay::WidthMode::Full),
               "a fresh device is on full, the neutral mode");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "mode", "multiband" } }),
                                 &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"multiband\" applied");
        check (sw->engine().getWidthMode() == echojay::WidthMode::Multiband,
               "and the ENGINE is actually split three ways now");

        device->applyStructured (paramsMove ({ { "mode", 0.0 } }));
        check (sw->engine().getWidthMode() == echojay::WidthMode::Full,
               "the index form reaches the same switch");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "sideways" } }), &a2, &s2);
        check (s2 == 1, "an unknown mode is SKIPPED, not guessed at");
    }

    std::printf ("== STEREO WIDTH: band widths, crossovers and rotation land exactly ==\n");
    {
        auto proc = makeByName ("EchoJay Stereo Width");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        // Fresh device: the depth params default NEUTRAL.
        check (near (device->getParamValue ("width_low"), 100.0),    "width_low defaults to 100");
        check (near (device->getParamValue ("width_mid"), 100.0),    "width_mid defaults to 100");
        check (near (device->getParamValue ("width_high"), 100.0),   "width_high defaults to 100");
        check (near (device->getParamValue ("xover_low_hz"), 150.0), "xover_low defaults to 150");
        check (near (device->getParamValue ("xover_high_hz"), 2500.0),"xover_high defaults to 2500");
        check (near (device->getParamValue ("rotation"), 0.0),       "rotation defaults to 0");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "mode", "multiband" },
                                               { "width_low", 40.0 },
                                               { "width_mid", 110.0 },
                                               { "width_high", 160.0 },
                                               { "xover_low_hz", 120.0 },
                                               { "xover_high_hz", 4000.0 },
                                               { "rotation", -12.0 } }),
                                 &applied, &skipped);
        check (applied == 7 && skipped == 0, "7 params applied, 0 skipped");
        check (near (device->getParamValue ("width_low"), 40.0),      "width_low landed EXACTLY at 40");
        check (near (device->getParamValue ("width_mid"), 110.0),     "width_mid landed EXACTLY at 110");
        check (near (device->getParamValue ("width_high"), 160.0),    "width_high landed EXACTLY at 160");
        check (near (device->getParamValue ("xover_low_hz"), 120.0),  "xover_low landed EXACTLY at 120");
        check (near (device->getParamValue ("xover_high_hz"), 4000.0),"xover_high landed EXACTLY at 4000");
        check (near (device->getParamValue ("rotation"), -12.0),      "rotation landed EXACTLY at -12");

        // Clamps: the schema's ranges, which are the engine's ranges. The two
        // crossover ranges do not overlap, so low < high is guaranteed.
        device->applyStructured (paramsMove ({ { "width_low", 999.0 },
                                               { "xover_low_hz", 5000.0 },
                                               { "xover_high_hz", 5.0 },
                                               { "rotation", 400.0 } }));
        check (near (device->getParamValue ("width_low"), 200.0),     "width_low clamped to 200");
        check (near (device->getParamValue ("xover_low_hz"), 800.0),  "xover_low clamped to 800");
        check (near (device->getParamValue ("xover_high_hz"), 1000.0),"xover_high clamped to 1000");
        check (near (device->getParamValue ("rotation"), 45.0),       "rotation clamped to +45");
        check (device->getParamValue ("xover_low_hz")
                 < device->getParamValue ("xover_high_hz"),
               "even dialled against each other, low stays below high");
    }

    std::printf ("== STEREOIZER: the mode dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Stereoizer");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* sz     = dynamic_cast<EedStereoizerProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"),
                     (double) (int) echojay::StereoizerMode::Haas),
               "a fresh device is on haas, the shipped character");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "mode", "comb" } }), &applied, &skipped);
        check (applied == 1 && skipped == 0, "mode = \"comb\" applied");
        check (sz->engine().getStereoizerMode() == echojay::StereoizerMode::Comb,
               "and the ENGINE is on the allpass chain now");

        device->applyStructured (paramsMove ({ { "mode", 2.0 } }));
        check (sz->engine().getStereoizerMode() == echojay::StereoizerMode::Dimension,
               "the index form reaches dimension");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode", "wider" } }), &a2, &s2);
        check (s2 == 1, "an unknown mode is SKIPPED, not guessed at");
    }

    // The device's headline claim, re-pinned per MODE through the REAL
    // processBlock: whatever character makes the width, folding the output to
    // mono gives back the input's fold-down. This is the check that stops a
    // future mode edit from quietly trading the guarantee for a sound.
    std::printf ("== STEREOIZER: every mode's mono sum survives processBlock ==\n");
    for (const char* mode : { "haas", "comb", "dimension" })
    {
        auto proc = makeByName ("EchoJay Stereoizer");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        device->applyStructured (paramsMove ({ { "mode", mode }, { "width", 180.0 },
                                               { "haas_ms", 25.0 },
                                               { "mono_maker_hz", 200.0 }, { "mix", 80.0 } }));

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buf (2, 4096);
        juce::Random rng (4321);
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
        check (worst <= 1.0e-5f, juce::String (mode) + ": L+R unchanged end to end "
                                 "(worst dev " + juce::String (worst, 9) + ")");
    }

    // The goniometer across the new modes: the tap is written post-engine in
    // processBlock, so every mode must land in the ring. Checked the same way
    // the Phase V0 test did — mono in, decorrelated out is widening made
    // visible — but through the COMB path, which shares none of the Haas DSP.
    std::printf ("== the scope taps still carry the output in the new modes ==\n");
    {
        auto ringCorrelation = [] (const echojay::viz::ScopeTap& tap)
        {
            std::vector<float> l (1024), r (1024);
            const int n = tap.read (l.data(), r.data(), 1024);
            if (n <= 0) return 2.0f;                    // sentinel: nothing tapped
            double lr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = 0; i < n; ++i)
            {
                lr += (double) l[i] * r[i];
                ll += (double) l[i] * l[i];
                rr += (double) r[i] * r[i];
            }
            if (ll + rr < 1.0e-9) return 2.0f;          // sentinel: silence
            const double d = std::sqrt (ll * rr);
            return d > 1.0e-12 ? (float) (lr / d) : 1.0f;
        };

        {
            auto proc = makeByName ("EchoJay Stereoizer");
            auto* sz = dynamic_cast<EedStereoizerProcessor*> (proc.get());
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            device->applyStructured (paramsMove ({ { "mode", "comb" }, { "width", 150.0 },
                                                  { "mono_maker_hz", 0.0 }, { "mix", 100.0 } }));
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            juce::Random rng (77);
            for (int b = 0; b < 24; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = rng.nextFloat() * 1.6f - 0.8f;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                }
                proc->processBlock (buf, midi);
            }

            const float c = ringCorrelation (sz->scopeTap());
            check (c < 0.99f && c <= 1.0f,
                   "comb mode: mono in, DECORRELATED in the ring (correlation "
                   + juce::String (c, 4) + ")");
        }
        {
            // Stereo Width in multiband: a stereo source with its low band
            // narrowed still writes the tap — the picture keeps working when
            // the mode changes what it is a picture of.
            auto proc = makeByName ("EchoJay Stereo Width");
            auto* sw = dynamic_cast<EedStereoWidthProcessor*> (proc.get());
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
            device->applyStructured (paramsMove ({ { "mode", "multiband" },
                                                  { "width_low", 0.0 } }));
            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);

            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            juce::Random rng (78);
            for (int b = 0; b < 24; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    buf.setSample (0, i, rng.nextFloat() * 1.6f - 0.8f);
                    buf.setSample (1, i, rng.nextFloat() * 1.6f - 0.8f);
                }
                proc->processBlock (buf, midi);
            }

            const float c = ringCorrelation (sw->scopeTap());
            check (c >= -1.0f && c <= 1.0f,
                   "multiband mode: the ring carries a real frame (correlation "
                   + juce::String (c, 4) + ")");
        }
    }

    std::printf ("== GAIN: mid/side mode dials, and its switches land in every spelling ==\n");
    {
        auto proc = makeByName ("EchoJay Gain");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* gain   = dynamic_cast<EedGainProcessor*> (proc.get());

        check (near (device->getParamValue ("mode"),
                     (double) (int) echojay::GainMode::Stereo),
               "a fresh device is on stereo, the shipped mode");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "mode", "mid_side" },
                                               { "mid_db", -3.0 },
                                               { "side_db", 4.5 } }), &applied, &skipped);
        check (applied == 3 && skipped == 0, "3 params applied, 0 skipped");
        check (gain->engine().getMode() == echojay::GainMode::MidSide,
               "the ENGINE is in mid/side now");
        check (near (device->getParamValue ("mid_db"), -3.0),  "mid_db landed EXACTLY at -3");
        check (near (device->getParamValue ("side_db"), 4.5),  "side_db landed EXACTLY at 4.5");

        // "MS", "m/s" etc. are NOT advertised; the canonical name and the index
        // both are. And the switches take every spelling a model actually emits.
        device->applyStructured (paramsMove ({ { "mode", 0.0 } }));
        check (gain->engine().getMode() == echojay::GainMode::Stereo,
               "the index form reaches the same switch");

        device->applyStructured (paramsMove ({ { "mono", true } }));
        check (near (device->getParamValue ("mono"), 1.0), "JSON true -> mono on");

        device->applyStructured (paramsMove ({ { "mono", "off" },
                                               { "phase_left", "on" },
                                               { "phase_right", "1" } }));
        check (near (device->getParamValue ("mono"), 0.0),        "the string \"off\" -> off");
        check (near (device->getParamValue ("phase_left"), 1.0),  "the string \"on\" -> on");
        check (near (device->getParamValue ("phase_right"), 1.0), "the string \"1\" -> on");

        // Clamps: cut to the level knob's -60 floor, boost only to +6 — the
        // trims compose with level_db, and two +24 stages would be a +48 dB
        // device.
        device->applyStructured (paramsMove ({ { "mid_db", 999.0 }, { "side_db", -999.0 } }));
        check (near (device->getParamValue ("mid_db"), 6.0),    "mid_db clamped to +6");
        check (near (device->getParamValue ("side_db"), -60.0), "side_db clamped to -60");
    }

    std::printf ("== GAIN: side at the floor collapses a real block to mono ==\n");
    {
        // End to end through processBlock, because "the param changed" and "the
        // audio changed" are different facts. side_db -60 is the floor, the
        // floor is true silence, and a silent side IS mono: L == R out.
        auto proc = makeByName ("EchoJay Gain");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        device->applyStructured (paramsMove ({ { "mode", "mid_side" },
                                               { "side_db", -60.0 } }));

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        juce::Random rng (55);
        for (int b = 0; b < 20; ++b)                    // past the 20 ms smoother
        {
            for (int i = 0; i < 512; ++i)
            {
                buf.setSample (0, i, rng.nextFloat() * 1.6f - 0.8f);
                buf.setSample (1, i, rng.nextFloat() * 1.6f - 0.8f);
            }
            proc->processBlock (buf, midi);
        }

        float worst = 0.0f;
        for (int i = 0; i < 512; ++i)
            worst = juce::jmax (worst, std::abs (buf.getSample (0, i)
                                               - buf.getSample (1, i)));
        check (worst <= 1.0e-4f, "L == R after the side is floored (worst diff "
                                 + juce::String (worst, 7) + ")");
    }

    std::printf ("== the Stereo/Utility depth params round-trip through state ==\n");
    {
        auto a = makeByName ("EchoJay Stereo Width");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "mode", "multiband" },
                                           { "width_low", 55.0 }, { "width_mid", 120.0 },
                                           { "width_high", 170.0 },
                                           { "xover_low_hz", 200.0 },
                                           { "xover_high_hz", 3000.0 },
                                           { "rotation", 8.0 } }));
        juce::MemoryBlock blob;
        da->getStateInformation (blob);

        auto b = makeByName ("EchoJay Stereo Width");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (blob.getData(), (int) blob.getSize());
        check (near (db->getParamValue ("mode"), 1.0),           "width mode restored (multiband)");
        check (near (db->getParamValue ("width_low"), 55.0),     "width_low restored");
        check (near (db->getParamValue ("width_mid"), 120.0),    "width_mid restored");
        check (near (db->getParamValue ("width_high"), 170.0),   "width_high restored");
        check (near (db->getParamValue ("xover_low_hz"), 200.0), "xover_low restored");
        check (near (db->getParamValue ("xover_high_hz"), 3000.0),"xover_high restored");
        check (near (db->getParamValue ("rotation"), 8.0),       "rotation restored");

        auto sz = makeByName ("EchoJay Stereoizer");
        auto* dsz = dynamic_cast<EedDeviceProcessor*> (sz.get());
        dsz->applyStructured (paramsMove ({ { "mode", "dimension" }, { "width", 140.0 } }));
        juce::MemoryBlock szb;
        dsz->getStateInformation (szb);

        auto sz2 = makeByName ("EchoJay Stereoizer");
        auto* dsz2 = dynamic_cast<EedDeviceProcessor*> (sz2.get());
        dsz2->setStateInformation (szb.getData(), (int) szb.getSize());
        check (near (dsz2->getParamValue ("mode"), 2.0),  "stereoizer mode restored (dimension)");
        check (near (dsz2->getParamValue ("width"), 140.0),"and the width under it survived");

        auto g = makeByName ("EchoJay Gain");
        auto* dg = dynamic_cast<EedDeviceProcessor*> (g.get());
        dg->applyStructured (paramsMove ({ { "mode", "mid_side" }, { "mid_db", -2.0 },
                                           { "side_db", 3.0 }, { "mono", true },
                                           { "phase_left", true } }));
        juce::MemoryBlock gb;
        dg->getStateInformation (gb);

        auto g2 = makeByName ("EchoJay Gain");
        auto* dg2 = dynamic_cast<EedDeviceProcessor*> (g2.get());
        dg2->setStateInformation (gb.getData(), (int) gb.getSize());
        check (near (dg2->getParamValue ("mode"), 1.0),       "gain mode restored (mid_side)");
        check (near (dg2->getParamValue ("mid_db"), -2.0),    "mid_db restored");
        check (near (dg2->getParamValue ("side_db"), 3.0),    "side_db restored");
        check (near (dg2->getParamValue ("mono"), 1.0),       "mono restored");
        check (near (dg2->getParamValue ("phase_left"), 1.0), "phase_left restored");
        check (near (dg2->getParamValue ("phase_right"), 0.0),"phase_right stayed off");
    }

    std::printf ("== a Stereo/Utility device at its DEFAULTS is unchanged by the depth pass ==\n");
    {
        // The strongest guarantee this pass makes, proved the way the Time pass
        // proved it: render the SAME audio through a device at its defaults and
        // through one with every depth param explicitly set to its neutral
        // value. Bit-identical output or the defaults are not neutral.
        for (const char* name : { "EchoJay Stereo Width", "EchoJay Stereoizer",
                                  "EchoJay Gain" })
        {
            auto mk = [name] (bool explicitNeutral)
            {
                auto proc = makeByName (name);
                auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
                if (explicitNeutral)
                {
                    const juce::String n (name);
                    if (n == "EchoJay Stereo Width")
                        device->applyStructured (paramsMove ({
                            { "mode", "full" },
                            { "width_low", 100.0 }, { "width_mid", 100.0 },
                            { "width_high", 100.0 },
                            { "xover_low_hz", 150.0 }, { "xover_high_hz", 2500.0 },
                            { "rotation", 0.0 } }));
                    else if (n == "EchoJay Stereoizer")
                        device->applyStructured (paramsMove ({ { "mode", "haas" } }));
                    else
                        device->applyStructured (paramsMove ({
                            { "mode", "stereo" }, { "mid_db", 0.0 }, { "side_db", 0.0 },
                            { "mono", false }, { "phase_left", false },
                            { "phase_right", false } }));
                }
                proc->setPlayConfigDetails (2, 2, 48000.0, 512);
                proc->prepareToPlay (48000.0, 512);
                return proc;
            };

            auto pa = mk (false), pb = mk (true);

            juce::AudioBuffer<float> ba (2, 4096), bb (2, 4096);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                {
                    const float v = 0.4f * std::sin (0.037f * (float) i + (float) ch)
                                  + (i % 977 == 0 ? 0.5f : 0.0f);
                    ba.setSample (ch, i, v);
                    bb.setSample (ch, i, v);
                }

            juce::MidiBuffer midi;
            for (int i = 0; i < 4096; i += 512)
            {
                juce::AudioBuffer<float> sa (ba.getArrayOfWritePointers(), 2, i, 512);
                juce::AudioBuffer<float> sb (bb.getArrayOfWritePointers(), 2, i, 512);
                pa->processBlock (sa, midi);
                pb->processBlock (sb, midi);
            }

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                    worst = juce::jmax (worst, std::abs (ba.getSample (ch, i)
                                                       - bb.getSample (ch, i)));

            check (worst == 0.0f,
                   juce::String (name) + ": its defaults ARE the neutral settings "
                   "(worst delta " + juce::String (worst, 9) + ")");
        }
    }

    std::printf ("== a Dynamics face at its defaults is still PASS-THROUGH ==\n");
    {
        // The depth pass added a harmonic character to the core, and the one thing
        // it must never do is colour a device that is not reducing. Every face,
        // at its defaults, with a signal under its threshold: the output has to be
        // the input.
        struct Idle { const char* name; std::vector<std::pair<const char*, double>> setup; };

        const Idle idle[] = {
            // Thresholds pushed out of the way so nothing engages, and the modes
            // set to the most coloured one each device has — clean cannot fail this
            // test, so testing clean would prove nothing.
            { "EchoJay Compressor", { { "threshold_db", 0.0 }, { "makeup_db", 0.0 } } },
            { "EchoJay Gate",       { { "threshold_db", -80.0 }, { "hysteresis_db", 0.0 } } },
            { "EchoJay Expander",   { { "threshold_db", -80.0 } } },
            { "EchoJay Limiter",    { { "ceiling_db", 0.0 }, { "lookahead_ms", 0.0 } } },
        };

        for (const auto& d : idle)
        {
            auto proc = makeByName (d.name);
            auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

            for (const auto& kv : d.setup) device->setParamValue (kv.first, kv.second);

            // The most coloured mode each device publishes, where it has one.
            // The gate is the exception: its `mode` is gate-vs-duck rather than a
            // character, and duck would deliberately reduce a loud signal, so it
            // stays on `gate` where an untriggered device is meant to be open.
            const juce::String name (d.name);

            if (name == "EchoJay Limiter")      device->setParamValue ("mode", 1.0);  // punchy
            else if (name != "EchoJay Gate")    device->setParamValue ("mode", 2.0);  // punch

            proc->setPlayConfigDetails (2, 2, 48000.0, 512);
            proc->prepareToPlay (48000.0, 512);

            // A sine on an OFFSET, so the probe has real frequency content (a stray
            // filter would show) but never comes near silence. A plain sine starts
            // at zero, and a gate reading digital silence in its first sample is
            // correctly closed — which would make this test fail on a device doing
            // exactly the right thing.
            juce::AudioBuffer<float> buf (2, 512);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.5f + 0.2f * std::sin ((float) i * 0.05f));

            juce::AudioBuffer<float> before (buf);
            juce::MidiBuffer midi;
            proc->processBlock (buf, midi);

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    worst = juce::jmax (worst, std::abs (buf.getSample (ch, i)
                                                       - before.getSample (ch, i)));

            check (worst < 1.0e-4f,
                   juce::String (d.name) + ": untriggered, it is transparent (worst delta "
                   + juce::String (worst, 7) + ")");
        }
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

    // =======================================================================
    // THE KEY DETECTOR (KEY_DETECTOR_SPEC.md) — the suite's first READER. The
    // engine's accuracy claims live in test/key_engine_test.cpp; what is
    // checked here is the device: dialability (by name AND index), the
    // momentary analyse/reset actions, state round-trip, and the headline
    // guarantee that it NEVER touches audio.
    // =======================================================================
    std::printf ("== KEY DETECTOR: every param dials exactly ==\n");
    {
        auto proc = makeByName ("EchoJay Key Detector");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* kd     = dynamic_cast<EedKeyDetectorProcessor*> (proc.get());
        check (device != nullptr && kd != nullptr, "key detector constructs");

        int applied = 0, skipped = 0;
        device->applyStructured (
            paramsMove ({ { "window_s", 12.0 }, { "sensitivity", 80.0 },
                          { "tuning_hz", 452.5 }, { "auto_tuning", false },
                          { "low_hz", 60.0 }, { "high_hz", 8000.0 },
                          { "continuous", true }, { "hpss", false },
                          { "hold", true } }), &applied, &skipped);
        check (applied == 9 && skipped == 0, "9 params applied, 0 skipped");
        check (near (device->getParamValue ("window_s"), 12.0),   "window_s EXACTLY 12");
        check (near (device->getParamValue ("sensitivity"), 80.0),"sensitivity EXACTLY 80");
        check (near (device->getParamValue ("tuning_hz"), 452.5), "tuning_hz EXACTLY 452.5");
        check (near (device->getParamValue ("auto_tuning"), 0.0), "auto_tuning off");
        check (near (device->getParamValue ("low_hz"), 60.0),     "low_hz EXACTLY 60");
        check (near (device->getParamValue ("high_hz"), 8000.0),  "high_hz EXACTLY 8000");
        check (near (device->getParamValue ("continuous"), 1.0),  "continuous on");
        check (near (device->getParamValue ("hpss"), 0.0),        "hpss off");
        check (near (device->getParamValue ("hold"), 1.0),        "hold on");

        // Clamps.
        device->applyStructured (paramsMove ({ { "window_s", 900.0 }, { "tuning_hz", 300.0 } }));
        check (near (device->getParamValue ("window_s"), 30.0),  "900 s clamps to 30");
        check (near (device->getParamValue ("tuning_hz"), 415.0),"300 Hz clamps to 415");
    }

    std::printf ("== KEY DETECTOR: mode_lock dials by NAME and by index ==\n");
    {
        auto proc = makeByName ("EchoJay Key Detector");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());

        check (near (device->getParamValue ("mode_lock"), 0.0), "a fresh detector is on auto");

        const auto s = device->applyStructured (paramsMove ({ { "mode_lock", "minor" } }));
        check (near (device->getParamValue ("mode_lock"), 2.0), "mode_lock = \"minor\" lands on index 2");
        check (s.contains ("minor"), "and reads back BY NAME: " + s);

        device->applyStructured (paramsMove ({ { "mode_lock", "MAJOR" } }));
        check (near (device->getParamValue ("mode_lock"), 1.0), "matching is case-insensitive");

        device->applyStructured (paramsMove ({ { "mode_lock", 0 } }));
        check (near (device->getParamValue ("mode_lock"), 0.0), "a numeric index works too");

        int a2 = 0, s2 = 0;
        device->applyStructured (paramsMove ({ { "mode_lock", "dorian" } }), &a2, &s2);
        check (s2 == 1, "an unknown mode is SKIPPED, not guessed at");

        const auto* d = registry.findByName ("EchoJay Key Detector");
        const auto* spec = d != nullptr ? d->schema.find ("mode_lock") : nullptr;
        check (spec != nullptr
                 && juce::String (echojay::ParamSchema::describeLine (*spec))
                        .contains ("auto|major|minor"),
               "mode_lock choices are ADVERTISED by name");
    }

    std::printf ("== KEY DETECTOR: analyse is the AI's trigger, reset clears ==\n");
    {
        auto proc = makeByName ("EchoJay Key Detector");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        auto* kd     = dynamic_cast<EedKeyDetectorProcessor*> (proc.get());

        check (near (device->getParamValue ("analyse"), 0.0), "idle before the trigger");

        int applied = 0, skipped = 0;
        device->applyStructured (paramsMove ({ { "analyse", true } }), &applied, &skipped);
        check (applied == 1 && skipped == 0, "analyse:1 is an accepted move");
        check (kd->engine().isCollecting(), "and the engine is now COLLECTING");
        check (near (device->getParamValue ("analyse"), 1.0), "analyse reads 1 while listening");

        device->applyStructured (paramsMove ({ { "reset", true } }));
        check (! kd->engine().isCollecting(), "reset cancels the pass");
        check (near (device->getParamValue ("reset"), 0.0),
               "and reset always reads 0 - a restored state cannot phantom-fire it");
    }

    std::printf ("== KEY DETECTOR: params round-trip through state ==\n");
    {
        auto a = makeByName ("EchoJay Key Detector");
        auto* da = dynamic_cast<EedDeviceProcessor*> (a.get());
        da->applyStructured (paramsMove ({ { "window_s", 15.0 }, { "mode_lock", "major" },
                                           { "hpss", false }, { "high_hz", 9000.0 } }));
        juce::MemoryBlock blob;
        da->getStateInformation (blob);

        auto b = makeByName ("EchoJay Key Detector");
        auto* db = dynamic_cast<EedDeviceProcessor*> (b.get());
        db->setStateInformation (blob.getData(), (int) blob.getSize());

        check (near (db->getParamValue ("window_s"), 15.0), "window_s restored");
        check (near (db->getParamValue ("mode_lock"), 1.0), "mode_lock restored (major)");
        check (near (db->getParamValue ("hpss"), 0.0),      "hpss restored");
        check (near (db->getParamValue ("high_hz"), 9000.0),"high_hz restored");
    }

    std::printf ("== KEY DETECTOR: audio is BIT-IDENTICAL at every setting ==\n");
    {
        // The reader's headline guarantee, proven the way the depth passes
        // proved neutrality — through processBlock, zero delta — but stronger:
        // the output is compared against the INPUT, with analysis armed,
        // continuous on and every band/mode moved off its default. There is
        // no neutral setting because there is no setting that touches audio.
        auto proc = makeByName ("EchoJay Key Detector");
        auto* device = dynamic_cast<EedDeviceProcessor*> (proc.get());
        device->applyStructured (
            paramsMove ({ { "analyse", true }, { "continuous", true },
                          { "window_s", 2.0 }, { "sensitivity", 90.0 },
                          { "mode_lock", "minor" }, { "tuning_hz", 452.0 },
                          { "auto_tuning", false }, { "hpss", false },
                          { "low_hz", 40.0 }, { "high_hz", 10000.0 } }));

        proc->setPlayConfigDetails (2, 2, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);
        check (proc->getLatencySamples() == 0, "reports ZERO latency, as advertised");
        check (proc->getTailLengthSeconds() == 0.0, "and no tail - it only observes");

        juce::AudioBuffer<float> buf (2, 512), ref (2, 512);
        juce::MidiBuffer midi;
        float worst = 0.0f;
        for (int blk = 0; blk < 200; ++blk)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.4f * std::sin (0.037f * (float) (blk * 512 + i))
                                  + ((blk * 512 + i) % 977 == 0 ? 0.5f : 0.0f);
                    buf.setSample (ch, i, v);
                    ref.setSample (ch, i, v);
                }
            proc->processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    worst = juce::jmax (worst, std::abs (buf.getSample (ch, i)
                                                       - ref.getSample (ch, i)));
        }
        check (worst == 0.0f, "output == input, bit for bit, analysis armed and "
               "everything dialled (worst delta " + juce::String (worst, 9) + ")");

        // Mono survives too (auditionable standalone).
        proc->setPlayConfigDetails (1, 1, 44100.0, 256);
        proc->prepareToPlay (44100.0, 256);
        juce::AudioBuffer<float> mono (1, 256);
        mono.clear();
        mono.setSample (0, 10, 0.75f);
        proc->processBlock (mono, midi);
        check (mono.getSample (0, 10) == 0.75f && mono.getSample (0, 11) == 0.0f,
               "mono passes through untouched");
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
