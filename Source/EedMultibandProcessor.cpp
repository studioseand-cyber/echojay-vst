/*
    EedMultibandProcessor.cpp  —  see EedMultibandProcessor.h.
*/

#include "EedMultibandProcessor.h"
#include "EedMultibandEditor.h"
#include "EedDeviceRegistry.h"

namespace
{
    // Per-band defaults. NOT the same for every band on purpose: a low band and
    // an air band want different time constants, and shipping identical defaults
    // means every user's first act is to fix four bands by hand. Low frequencies
    // need slow attack/release simply because a 60 Hz cycle is 16 ms long — a
    // 5 ms release on a bass band tracks the waveform itself and distorts.
    struct BandDefaults { double attackMs, releaseMs; };

    const BandDefaults kBandDefaults[EedMultibandProcessor::kNumBands] = {
        { 30.0, 300.0 },   // band 1 - lows
        { 20.0, 200.0 },   // band 2 - low mids
        { 10.0, 150.0 },   // band 3 - high mids
        {  5.0, 100.0 },   // band 4 - highs
    };

    constexpr double kDefaultThresholdDb = -18.0;
    constexpr double kDefaultRatio       = 3.0;
    constexpr double kDefaultKneeDb      = 6.0;

    // The array-form keys accepted per entry, and the flat leaf each maps to.
    // Aliases are here rather than in ParamSchema because they are shapes of the
    // ARRAY, not of the contract: "thresh" is a spelling of one band's key, and
    // the flat id it resolves to is what gets validated.
    struct BandKey { const char* jsonKey; const char* leaf; };

    const BandKey kBandKeys[] = {
        { "threshold_db", EedMultibandProcessor::kThresholdDb },
        { "threshold",    EedMultibandProcessor::kThresholdDb },
        { "ratio",        EedMultibandProcessor::kRatio },
        { "attack_ms",    EedMultibandProcessor::kAttackMs },
        { "attack",       EedMultibandProcessor::kAttackMs },
        { "release_ms",   EedMultibandProcessor::kReleaseMs },
        { "release",      EedMultibandProcessor::kReleaseMs },
        { "knee_db",      EedMultibandProcessor::kKneeDb },
        { "knee",         EedMultibandProcessor::kKneeDb },
        { "makeup_db",    EedMultibandProcessor::kMakeupDb },
        { "makeup",       EedMultibandProcessor::kMakeupDb },
        { "gain_db",      EedMultibandProcessor::kMakeupDb },
        { "bypass",       EedMultibandProcessor::kBypass },

        // The depth pass's per-band character, reachable from the array form too:
        // "make the top band punchy" is a comp_bands move, and it would be odd for
        // the one param the plan calls the marquee addition to be flat-only.
        { "mode",         EedMultibandProcessor::kMode },
        { "character",    EedMultibandProcessor::kMode },
    };

    // Per-band character defaults. Like the time constants above, NOT the same for
    // every band: a low band wants the slow, soft, invisible one and an air band
    // wants the fast one. Shipping four `clean` bands would be a device whose
    // marquee control starts switched off.
    const echojay::CharacterMode kBandCharacter[EedMultibandProcessor::kNumBands] = {
        echojay::CharacterMode::Glue,     // band 1 - lows: slow and cohesive
        echojay::CharacterMode::Clean,    // band 2 - low mids: stay out of the way
        echojay::CharacterMode::Clean,    // band 3 - high mids: where words live
        echojay::CharacterMode::Punch,    // band 4 - highs: fast, a little drive
    };
}

// ---------------------------------------------------------------------------
EedMultibandProcessor::EedMultibandProcessor()
{
    for (int b = 0; b < kNumBands; ++b)
    {
        cores_[b].setMode (echojay::DynamicsMode::Compress);
        cores_[b].setRmsWindowMs (10.0);
    }

    // The detector and the four characters come from the schema's defaults, so a
    // fresh device is described in exactly one place.
    resetParamsToDefaults();
}

juce::String EedMultibandProcessor::bandParamId (int bandIndex, const char* leaf)
{
    return "band" + juce::String (bandIndex + 1) + "_" + leaf;
}

double EedMultibandProcessor::crossoverHz (int i) const noexcept
{
    switch (i)
    {
        case 0:  return splitter_.crossover1();
        case 1:  return splitter_.crossover2();
        default: return splitter_.crossover3();
    }
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedMultibandProcessor::schema()
{
    static const echojay::ParamSchema s = []
    {
        std::vector<echojay::ParamSpec> p;
        p.reserve (4 + kNumBands * 8);

        // Crossovers first: they define what the bands ARE, so they are what a
        // model has to reason about before it can reason about a band.
        p.push_back ({ "crossover1_hz", "Hz", 20.0, 2000.0, 120.0,
                       "split between band 1 (lows) and band 2", false });
        p.push_back ({ "crossover2_hz", "Hz", 100.0, 8000.0, 800.0,
                       "split between band 2 and band 3 (mids)", false });
        p.push_back ({ "crossover3_hz", "Hz", 1000.0, 18000.0, 5000.0,
                       "split between band 3 and band 4 (highs)", false });

        // Global, and placed with the crossovers rather than among the bands
        // because like them it is a statement about the whole device.
        p.push_back ({ kDetector, "", 0.0, 1.0, 1.0,
                       "what every band's detector measures: rms follows loudness "
                       "(right for multiband work, and what keeps the four bands "
                       "comparable), peak catches transients and is tighter",
                       false, { "peak", "rms" } });

        // Each band's full set. Descriptions name the band so the advertisement
        // reads as four compressors rather than as 28 anonymous numbers.
        const char* const bandWhat[kNumBands] = {
            "band 1 (below crossover1_hz)",
            "band 2 (crossover1_hz to crossover2_hz)",
            "band 3 (crossover2_hz to crossover3_hz)",
            "band 4 (above crossover3_hz)"
        };

        for (int b = 0; b < kNumBands; ++b)
        {
            const std::string where = bandWhat[b];

            p.push_back ({ bandParamId (b, kThresholdDb).toStdString(), "dB",
                           -60.0, 0.0, kDefaultThresholdDb,
                           "level where " + where + " starts compressing", false });

            p.push_back ({ bandParamId (b, kRatio).toStdString(), "",
                           1.0, 20.0, kDefaultRatio,
                           "compression ratio for " + where, false });

            p.push_back ({ bandParamId (b, kAttackMs).toStdString(), "ms",
                           0.1, 300.0, kBandDefaults[b].attackMs,
                           "attack for " + where, false });

            p.push_back ({ bandParamId (b, kReleaseMs).toStdString(), "ms",
                           5.0, 2000.0, kBandDefaults[b].releaseMs,
                           "release for " + where, false });

            p.push_back ({ bandParamId (b, kKneeDb).toStdString(), "dB",
                           0.0, 24.0, kDefaultKneeDb,
                           "knee width for " + where, false });

            p.push_back ({ bandParamId (b, kMakeupDb).toStdString(), "dB",
                           -24.0, 24.0, 0.0,
                           "output gain for " + where + "; doubles as a band tilt - "
                           "raise it to lean the balance that way", false });

            p.push_back ({ bandParamId (b, kBypass).toStdString(), "",
                           0.0, 1.0, 0.0,
                           "on = " + where + " passes through uncompressed", true });

            p.push_back ({ bandParamId (b, kMode).toStdString(), "",
                           0.0, (double) (echojay::kCharacterCount - 1),
                           (double) (int) kBandCharacter[b],
                           "character for " + where + ", which reshapes its attack, "
                           "release and knee and adds gentle drive as it works: clean "
                           "is transparent, glue is slow and soft (right for a low "
                           "band), punch is fast and aggressive (right for an air "
                           "band), smooth has a programme-dependent release",
                           false, { "clean", "glue", "punch", "smooth" } });
        }

        return echojay::ParamSchema (std::move (p));
    }();

    return s;
}

// Split a flat id into its band index and leaf. Returns -1 for an id that is not
// a band param (the crossovers).
static int bandIndexFromId (const juce::String& id, juce::String& leafOut)
{
    if (! id.startsWithIgnoreCase ("band")) return -1;

    const juce::String rest = id.substring (4);
    const int sep = rest.indexOfChar ('_');
    if (sep <= 0) return -1;

    const int n = rest.substring (0, sep).getIntValue();
    if (n < 1 || n > EedMultibandProcessor::kNumBands) return -1;

    leafOut = rest.substring (sep + 1);
    return n - 1;
}

bool EedMultibandProcessor::setParamValue (const juce::String& id, double value)
{
    // Just the targets. The audio thread picks them up and rebuilds the tree.
    if (id == kCrossover1Hz) { crossoverHz_[0].store (value); return true; }
    if (id == kCrossover2Hz) { crossoverHz_[1].store (value); return true; }
    if (id == kCrossover3Hz) { crossoverHz_[2].store (value); return true; }

    if (id == kDetector)
    {
        const auto m = value >= 0.5 ? echojay::DetectorMode::Rms
                                    : echojay::DetectorMode::Peak;
        for (auto& c : cores_) c.setDetectorMode (m);
        return true;
    }

    juce::String leaf;
    const int b = bandIndexFromId (id, leaf);
    if (b < 0) return false;

    auto& c = cores_[b];
    if (leaf == kThresholdDb) { c.setThresholdDb ((float) value); return true; }
    if (leaf == kRatio)       { c.setRatio       ((float) value); return true; }
    if (leaf == kAttackMs)    { c.setAttackMs    (value);         return true; }
    if (leaf == kReleaseMs)   { c.setReleaseMs   (value);         return true; }
    if (leaf == kKneeDb)      { c.setKneeDb      ((float) value); return true; }
    if (leaf == kMakeupDb)    { c.setMakeupDb    ((float) value); return true; }
    if (leaf == kBypass)      { bandBypass_[b].store (value >= 0.5); return true; }
    if (leaf == kMode)
    {
        c.setCharacter (echojay::characterModeFromIndex ((int) std::lround (value)));
        return true;
    }
    return false;
}

double EedMultibandProcessor::getParamValue (const juce::String& id) const
{
    // The stored value, not the splitter's sorted realisation: a round-trip
    // through state has to give back what was set, or a saved session drifts a
    // little further towards sorted every time it is loaded.
    if (id == kCrossover1Hz) return crossoverHz_[0].load();
    if (id == kCrossover2Hz) return crossoverHz_[1].load();
    if (id == kCrossover3Hz) return crossoverHz_[2].load();

    // One global setting, so band 0 speaks for all four — they are written
    // together and cannot disagree.
    if (id == kDetector)
        return cores_[0].getDetectorMode() == echojay::DetectorMode::Rms ? 1.0 : 0.0;

    juce::String leaf;
    const int b = bandIndexFromId (id, leaf);
    if (b < 0) return 0.0;

    const auto& c = cores_[b];
    if (leaf == kThresholdDb) return (double) c.getThresholdDb();
    if (leaf == kRatio)       return (double) c.getRatio();
    if (leaf == kAttackMs)    return c.getAttackMs();
    if (leaf == kReleaseMs)   return c.getReleaseMs();
    if (leaf == kKneeDb)      return (double) c.getKneeDb();
    if (leaf == kMakeupDb)    return (double) c.getMakeupDb();
    if (leaf == kBypass)      return bandBypass_[b].load() ? 1.0 : 0.0;
    if (leaf == kMode)        return (double) (int) c.getCharacter();
    return 0.0;
}

void EedMultibandProcessor::pushCrossovers()
{
    for (int i = 0; i < 3; ++i) appliedCrossoverHz_[i] = crossoverHz_[i].load();

    splitter_.setCrossovers (appliedCrossoverHz_[0],
                             appliedCrossoverHz_[1],
                             appliedCrossoverHz_[2]);
}

// ---------------------------------------------------------------------------
// the array form
// ---------------------------------------------------------------------------
juce::String EedMultibandProcessor::applyCompBands (const juce::var& compBandsArray,
                                                    ParamSource src,
                                                    int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    if (! compBandsArray.isArray()) return "comp_bands: expected an array";

    auto* arr = compBandsArray.getArray();
    const int n = arr != nullptr ? arr->size() : 0;
    if (n == 0) return "comp_bands: empty";

    // Rewrite the array into flat ids and hand it to the ONE apply path. Every
    // clamp, every merge rule and every line of the report is then written once,
    // in EedDeviceProcessor, and the array form cannot drift away from it.
    juce::DynamicObject::Ptr flat = new juce::DynamicObject();
    juce::StringArray unknown;

    for (int i = 0; i < n; ++i)
    {
        const juce::var& e = (*arr)[i];
        if (! e.isObject())
        {
            unknown.add ("entry " + juce::String (i + 1) + " (not an object)");
            continue;
        }

        // An explicit "band" wins; without one the position in the array IS the
        // band, which is how a model naturally writes four bands in order.
        int band = (int) e.getProperty ("band", i + 1);
        if (band < 1 || band > kNumBands)
        {
            unknown.add ("band " + juce::String (band) + " (out of range 1.."
                         + juce::String (kNumBands) + ")");
            continue;
        }

        auto* obj = e.getDynamicObject();
        if (obj == nullptr) continue;

        for (const auto& prop : obj->getProperties())
        {
            const juce::String key = prop.name.toString();
            if (key.equalsIgnoreCase ("band")) continue;      // the selector, not a value

            const char* leaf = nullptr;
            for (const auto& bk : kBandKeys)
                if (key.equalsIgnoreCase (bk.jsonKey)) { leaf = bk.leaf; break; }

            if (leaf == nullptr)
            {
                unknown.add ("band " + juce::String (band) + " " + key);
                continue;
            }

            flat->setProperty (juce::Identifier (bandParamId (band - 1, leaf)), prop.value);
        }
    }

    int applied = 0, skipped = 0;
    juce::String summary = applyParams (juce::var (flat.get()), src, &applied, &skipped);

    if (appliedOut != nullptr) *appliedOut = applied;
    if (skippedOut != nullptr) *skippedOut = skipped + unknown.size();

    if (! unknown.isEmpty())
    {
        if (summary.isNotEmpty()) summary += "; ";
        summary += "ignored " + unknown.joinIntoString (", ");
    }

    if (summary.isEmpty()) return "comp_bands: nothing recognised";
    return summary;
}

juce::String EedMultibandProcessor::applyStructured (const juce::var& structured,
                                                     ParamSource src,
                                                     int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    // A bare array IS comp_bands, the same courtesy the EQ extends to eq_bands.
    if (structured.isArray())
        return applyCompBands (structured, src, appliedOut, skippedOut);

    if (! structured.isObject()) return {};

    juce::StringArray parts;
    int applied = 0, skipped = 0;

    // Crossovers arrive through `params` and are resolved FIRST, because they
    // change what each band covers: a move that widens band 4 and then sets its
    // threshold has to set the threshold of the band it just defined.
    if (structured.hasProperty ("params"))
    {
        int a = 0, s = 0;
        const auto p = applyParams (structured.getProperty ("params", juce::var()), src, &a, &s);
        if (p.isNotEmpty()) parts.add (p);
        applied += a; skipped += s;
    }

    if (structured.hasProperty ("comp_bands"))
    {
        int a = 0, s = 0;
        const auto b = applyCompBands (structured.getProperty ("comp_bands", juce::var()), src, &a, &s);
        if (b.isNotEmpty()) parts.add (b);
        applied += a; skipped += s;
    }

    if (appliedOut != nullptr) *appliedOut = applied;
    if (skippedOut != nullptr) *skippedOut = skipped;

    if (parts.isEmpty()) return {};
    return parts.joinIntoString ("; ");
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedMultibandProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    splitter_.prepare (sampleRate_);
    splitter_.reset();
    for (auto& a : appliedCrossoverHz_) a = -1.0;   // force a rebuild for the new rate
    pushCrossovers();

    for (auto& c : cores_) { c.prepare (sampleRate_); c.reset(); }
}

void EedMultibandProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    // Pick up moved crossovers here, on the audio thread, once per block.
    if (crossoverHz_[0].load() != appliedCrossoverHz_[0]
     || crossoverHz_[1].load() != appliedCrossoverHz_[1]
     || crossoverHz_[2].load() != appliedCrossoverHz_[2])
        pushCrossovers();

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
    const int n = buffer.getNumSamples();

    // Read the per-band bypasses once per block rather than per sample: they
    // cannot change mid-block in any way a listener could hear, and reading four
    // atomics per sample is real cost in the inner loop.
    bool bypass[kNumBands];
    for (int b = 0; b < kNumBands; ++b) bypass[b] = bandBypass_[b].load();

    for (int i = 0; i < n; ++i)
    {
        float bandsL[kNumBands], bandsR[kNumBands] = { 0.0f, 0.0f, 0.0f, 0.0f };
        splitter_.process (0, l[i], bandsL);
        if (r != nullptr) splitter_.process (1, r[i], bandsR);

        float outL = 0.0f, outR = 0.0f;

        for (int b = 0; b < kNumBands; ++b)
        {
            const float bl = bandsL[b];
            const float br = r != nullptr ? bandsR[b] : bl;

            if (bypass[b])
            {
                // Still summed, just not compressed — a bypassed band drops out
                // of the OUTPUT entirely if it is skipped, which would be a
                // notch rather than a bypass.
                outL += bl;
                outR += br;
                continue;
            }

            // Stereo-linked per band: the detector sees both channels of THIS
            // band, so the band's gain is one number and the image holds.
            const float g  = cores_[b].gainForSidechain (bl, br);
            const float mk = cores_[b].nextMakeupGain();

            // This band's character, applied where its gain element sits — after
            // the gain and before the makeup. Identity for a `clean` band and for
            // any band that is not currently reducing, so an idle multiband still
            // sums back to its input.
            outL += cores_[b].shapeCharacter (bl * g) * mk;
            outR += cores_[b].shapeCharacter (br * g) * mk;
        }

        l[i] = outL;
        if (r != nullptr) r[i] = outR;
    }
}

juce::AudioProcessorEditor* EedMultibandProcessor::createEditor()
{
    return new EedMultibandEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeMultibandDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay 4-Band Compressor";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay 4-band multiband compressor (built in)";
        d.summary         = "Four Linkwitz-Riley bands, each with its own stereo-linked "
                            "compressor, makeup and character mode (clean/glue/punch/"
                            "smooth), plus one global detector. Reach for it when one "
                            "part of the spectrum needs controlling without touching the "
                            "rest - a boomy low end, a harsh upper mid - or to give the "
                            "lows a slow glue and the air a fast punch at once. Dial it "
                            "with a comp_bands array or with flat bandN_ params.";
        d.identifier      = "echojay:builtin:multiband";
        d.uid             = 0x456A4D42;   // 'EjMB' - frozen once shipped
        d.aliases         = { "EchoJayMultiband", "EchoJay Multiband",
                              "EchoJay Multiband Compressor", "EchoJay 4 Band Compressor",
                              "EchoJay Four Band Compressor" };
        d.schema          = EedMultibandProcessor::schema();
        d.create          = [] { return std::make_unique<EedMultibandProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar multibandRegistrar { makeMultibandDevice() };
}
