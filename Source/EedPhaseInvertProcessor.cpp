/*
    EedPhaseInvertProcessor.cpp  —  see EedPhaseInvertProcessor.h.
*/

#include "EedPhaseInvertProcessor.h"
#include "EedPhaseInvertEditor.h"
#include "EedDeviceRegistry.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedPhaseInvertProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { kInvertL, "", 0.0, 1.0, 0.0,
          "invert the polarity of the left channel", true },
        { kInvertR, "", 0.0, 1.0, 0.0,
          "invert the polarity of the right channel", true },
    });
    return s;
}

bool EedPhaseInvertProcessor::setParamValue (const juce::String& id, double value)
{
    // Values arrive clamped to 0..1 by the schema; >= 0.5 is on.
    if (id == kInvertL) { invertL_.store (value >= 0.5); return true; }
    if (id == kInvertR) { invertR_.store (value >= 0.5); return true; }
    return false;
}

double EedPhaseInvertProcessor::getParamValue (const juce::String& id) const
{
    if (id == kInvertL) return invertL_.load() ? 1.0 : 0.0;
    if (id == kInvertR) return invertR_.load() ? 1.0 : 0.0;
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedPhaseInvertProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    constexpr double tau = 0.005;                       // ~5 ms through zero
    rampCoeff_ = (float) std::exp (-1.0 / (tau * sr));

    // Start AT the target rather than ramping in from +1 on the first block, so
    // a restored session that had the left channel flipped does not click on
    // playback.
    coeffL_ = invertL_.load() ? -1.0f : 1.0f;
    coeffR_ = invertR_.load() ? -1.0f : 1.0f;

    // No reading yet for this configuration: the first stereo block lands on its
    // value instead of sliding there from the previous session's.
    corrTap_.set (kNoCorrelation);
    corrSmoothed_ = 0.0f;
    corrPrimed_   = false;
}

void EedPhaseInvertProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    const int numCh      = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0 || numSamples <= 0) return;

    // NOT gated on bypass, unlike the metering on other devices: this device
    // still runs its ramp while bypassed (below), so the buffer is still being
    // written and the reading stays true. The editor dims the dot instead.
    //
    // Bypass still has to run the ramp state toward unity, otherwise re-enabling
    // resumes from a stale coefficient and clicks.
    const bool  bypassed = isBypassed();
    const float targetL  = bypassed ? 1.0f : (invertL_.load() ? -1.0f : 1.0f);
    const float targetR  = bypassed ? 1.0f : (invertR_.load() ? -1.0f : 1.0f);

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        coeffL_ = targetL + (coeffL_ - targetL) * rampCoeff_;
        coeffR_ = targetR + (coeffR_ - targetR) * rampCoeff_;

        l[i] *= coeffL_;
        if (r != nullptr) r[i] *= coeffR_;
    }

    // Measured on the OUTPUT, so it reports the phase relationship the device is
    // actually leaving behind rather than the one it was handed.
    publishCorrelation (buffer, numCh, numSamples);
}

// Pearson correlation of L against R over the block:
//
//     c = sum(L*R) / sqrt(sum(L*L) * sum(R*R))
//
// which is exactly +1 for identical channels, -1 for one flipped against the
// other, and 0 when they share nothing — the three readings this device's two
// switches move between.
void EedPhaseInvertProcessor::publishCorrelation (const juce::AudioBuffer<float>& buffer,
                                                  int numCh, int numSamples)
{
    if (numCh < 2)
    {
        // Nothing to correlate. Say so, rather than publishing a 0 that would
        // read as "wide" on a device that is running in mono.
        corrTap_.set (kNoCorrelation);
        corrPrimed_ = false;
        return;
    }

    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getReadPointer (1);

    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        lr += (double) l[i] * (double) r[i];
        ll += (double) l[i] * (double) l[i];
        rr += (double) r[i] * (double) r[i];
    }

    const double denom = std::sqrt (ll * rr);

    // Silence has no phase relationship. HOLD the last reading rather than
    // publishing 0: a dot that snaps to centre in every gap between phrases
    // reads as the signal decorrelating, which is the opposite of what happened.
    if (! (denom > 1.0e-12)) return;

    const float c = juce::jlimit (-1.0f, 1.0f, (float) (lr / denom));

    // First stereo block after mono or a fresh prepare: land on the value rather
    // than sliding to it from whatever was there before.
    if (! corrPrimed_) { corrSmoothed_ = c; corrPrimed_ = true; }
    else               { corrSmoothed_ += 0.15f * (c - corrSmoothed_); }

    corrTap_.set (corrSmoothed_);
}

juce::AudioProcessorEditor* EedPhaseInvertProcessor::createEditor()
{
    return new EedPhaseInvertEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makePhaseInvertDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Phase Invert";
        d.category        = "Utility";
        d.descriptiveName = "EchoJay per-channel polarity flip (built in)";
        // ASCII ONLY in registry text. juce::String's const char* constructor
        // reads its input as ASCII (it jasserts on anything else), so a UTF-8
        // em-dash here arrives double-encoded and ships mojibake into the AI
        // prompt. Elsewhere in EchoJay an em-dash survives because those strings
        // are built with operator+, which takes the UTF-8 path — the difference
        // is invisible until you look at the bytes.
        d.summary         = "Flips the polarity of either channel independently. Reach for "
                            "it when two sources cancel each other - a mic pair fighting in "
                            "the low end, or one side of a stereo pair wired backwards.";
        d.identifier      = "echojay:builtin:phaseinvert";
        d.uid             = 0x456A5049;   // 'EjPI' — frozen once shipped
        d.aliases         = { "EchoJayPhaseInvert", "EchoJay Polarity",
                              "EchoJay Phase", "EchoJay Invert" };
        d.schema          = EedPhaseInvertProcessor::schema();
        d.create          = [] { return std::make_unique<EedPhaseInvertProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar phaseInvertRegistrar { makePhaseInvertDevice() };
}
