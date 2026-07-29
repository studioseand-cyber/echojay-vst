/*
    EedDeEsserProcessor.cpp  —  see EedDeEsserProcessor.h.
*/

#include "EedDeEsserProcessor.h"
#include "EedDeEsserEditor.h"
#include "EedDeviceRegistry.h"

namespace
{
    // The sidechain bandpass Q. Wide enough to cover a real "s" (which is not a
    // sine) and narrow enough that the vowel underneath it does not trigger the
    // detector — the failure mode of a broad sidechain is a de-esser that ducks
    // on every loud word rather than on sibilance.
    constexpr double kSidechainQ = 2.0;
}

EedDeEsserProcessor::EedDeEsserProcessor()
{
    core_.setMode (echojay::DynamicsMode::Compress);
    core_.setDetectorMode (echojay::DetectorMode::Peak);

    // A steep ratio and a soft knee: sibilance is short and loud, so what is
    // wanted is "hold it at the threshold", engaged gradually enough that the
    // reduction does not announce itself.
    core_.setRatio (8.0f);
    core_.setKneeDb (4.0f);

    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedDeEsserProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kFreqHz, "Hz", 1000.0, 16000.0, 6500.0,
          "centre of the band it listens to; 5-8 kHz for most vocals, higher for "
          "a bright or close-miked one", false },

        { kRangeDb, "dB", 0.0, 24.0, 8.0,
          "the deepest reduction allowed; this is the de-essing amount, and "
          "keeping it modest is what stops a lisp", false },

        { kThresholdDb, "dB", -60.0, 0.0, -28.0,
          "level in that band where it starts reducing; lower catches more "
          "sibilance", false },

        { kAttackMs, "ms", 0.05, 50.0, 1.0,
          "how fast it clamps a sibilant; must be fast, an 's' is only a few "
          "tens of milliseconds long", false },

        { kReleaseMs, "ms", 5.0, 500.0, 60.0,
          "how fast it recovers after one; too long and it holds the whole word "
          "down", false },

        { kMode, "", 0.0, 1.0, 1.0,
          "on = split (only the band above freq_hz is ducked, the body of the "
          "voice never moves); off = wide (the whole signal ducks)", true },

        { kListen, "", 0.0, 1.0, 0.0,
          "on = monitor the detector's band alone, to tune freq_hz onto the "
          "sibilance. A MONITORING aid - turn it off to hear the result", true },
    });
    return s;
}

bool EedDeEsserProcessor::setParamValue (const juce::String& id, double value)
{
    // Just the target. The audio thread picks it up and rebuilds the filters.
    if (id == kFreqHz)      { freqHz_.store (value);                return true; }
    if (id == kRangeDb)     { core_.setRangeDb     ((float) value); return true; }
    if (id == kThresholdDb) { core_.setThresholdDb ((float) value); return true; }
    if (id == kAttackMs)    { core_.setAttackMs    (value);         return true; }
    if (id == kReleaseMs)   { core_.setReleaseMs   (value);         return true; }
    if (id == kMode)        { splitMode_.store (value >= 0.5);      return true; }
    if (id == kListen)      { listen_.store    (value >= 0.5);      return true; }
    return false;
}

double EedDeEsserProcessor::getParamValue (const juce::String& id) const
{
    if (id == kFreqHz)      return freqHz_.load();
    if (id == kRangeDb)     return (double) core_.getRangeDb();
    if (id == kThresholdDb) return (double) core_.getThresholdDb();
    if (id == kAttackMs)    return core_.getAttackMs();
    if (id == kReleaseMs)   return core_.getReleaseMs();
    if (id == kMode)        return splitMode_.load() ? 1.0 : 0.0;
    if (id == kListen)      return listen_.load() ? 1.0 : 0.0;
    return 0.0;
}

void EedDeEsserProcessor::updateFilters()
{
    appliedFreqHz_ = freqHz_.load();

    // Coefficients are replaced, state is not: a filter reset on every dial
    // movement is a click on every dial movement.
    const auto bp = echojay::Biquad::bandpass (sampleRate_, appliedFreqHz_, kSidechainQ);

    for (int ch = 0; ch < 2; ++ch)
    {
        scFilter_[ch].setCoeffs (bp);
        splitFilter_[ch].setCutoff (sampleRate_, appliedFreqHz_);
    }
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedDeEsserProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    core_.prepare (sampleRate_);
    core_.reset();

    for (auto& f : scFilter_)    f.reset();
    for (auto& f : splitFilter_) f.reset();

    appliedFreqHz_ = -1.0;      // force a rebuild for the new sample rate
    updateFilters();
}

void EedDeEsserProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    // Pick up a moved freq_hz here, on the audio thread, once per block.
    if (freqHz_.load() != appliedFreqHz_) updateFilters();

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
    const int n = buffer.getNumSamples();

    const bool split  = splitMode_.load();
    const bool listen = listen_.load();

    for (int i = 0; i < n; ++i)
    {
        const float inL = l[i];
        const float inR = r != nullptr ? r[i] : inL;

        // The sidechain: what the detector hears, filtered but never heard in
        // the output unless LISTEN is on.
        const float scL = scFilter_[0].process (inL);
        const float scR = r != nullptr ? scFilter_[1].process (inR) : scL;

        // Stereo-linked, like every other face — a de-esser that ducks one side
        // of a doubled vocal pulls the image with it.
        const float g = core_.gainForSidechain (scL, scR);

        if (listen)
        {
            // Solo the detector's band. Deliberately NOT gain-reduced: the point
            // is to hear what is being detected, and applying the reduction to
            // the monitor would hide exactly the peaks being tuned for.
            l[i] = scL;
            if (r != nullptr) r[i] = scR;
            continue;
        }

        if (! split)
        {
            l[i] = inL * g;
            if (r != nullptr) r[i] = inR * g;
            continue;
        }

        // SPLIT: reduce only the high side, then sum. With g == 1 the two halves
        // of an LR4 split add back to the input, so an idle de-esser in split
        // mode is a pass-through and not a filter sitting in the path.
        float lowL = 0.0f, highL = 0.0f;
        splitFilter_[0].process (inL, lowL, highL);
        l[i] = lowL + highL * g;

        if (r != nullptr)
        {
            float lowR = 0.0f, highR = 0.0f;
            splitFilter_[1].process (inR, lowR, highR);
            r[i] = lowR + highR * g;
        }
    }
}

juce::AudioProcessorEditor* EedDeEsserProcessor::createEditor()
{
    return new EedDeEsserEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeDeEsserDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay De-Esser";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay de-esser (built in)";
        d.summary         = "Band-triggered compressor for sibilance, in wide or "
                            "split-band mode, with a listen solo for tuning the "
                            "frequency. Reach for it when a vocal's 's' and 't' "
                            "sounds are harsh.";
        d.identifier      = "echojay:builtin:deesser";
        d.uid             = 0x456A4453;   // 'EjDS' - frozen once shipped
        d.aliases         = { "EchoJayDeEsser", "EchoJay Deesser", "EchoJay De Esser",
                              "EchoJay Sibilance" };
        d.schema          = EedDeEsserProcessor::schema();
        d.create          = [] { return std::make_unique<EedDeEsserProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar deEsserRegistrar { makeDeEsserDevice() };
}
