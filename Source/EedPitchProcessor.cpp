/*
    EedPitchProcessor.cpp  —  see EedPitchProcessor.h.
*/

#include "EedPitchProcessor.h"
#include "EedPitchEditor.h"
#include "EedDeviceRegistry.h"

using echojay::PitchEngine;

// ---------------------------------------------------------------------------
// the dialable contract — P0's surface, deliberately small
// ---------------------------------------------------------------------------
// The full corrector contract (retune speed, flex, scale, ...) arrives with
// P3. What ships now is exactly what the detection phase needs: the analysis
// window control the spec hangs everything on, and a way to zero the
// octave-error log before a measurement pass.
const echojay::ParamSchema& EedPitchProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { EedPitchProcessor::kVoiceType, "",
          0.0, (double) (PitchEngine::kNumVoiceTypes - 1), (double) PitchEngine::kAltoTenor,
          "pitch search range of the detector; match it to the source - a "
          "wrong choice causes octave errors",
          false,
          // Mirrors PitchEngine::VoiceType order exactly.
          { "soprano", "alto_tenor", "low_male", "instrument", "bass" } },

        { EedPitchProcessor::kResetStats, "", 0.0, 1.0, 0.0,
          "set 1 to zero the octave-guard and frame counters before a "
          "detection measurement pass; always reads 0", true },
    });
    return s;
}

bool EedPitchProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kVoiceType)
    {
        engine_.setVoiceType ((int) std::lround (value));
        return true;
    }
    if (id == kResetStats)
    {
        // Momentary action dressed as a switch (the shape the schema can
        // carry): 1 fires it, it reads back 0, so a state restore of "0" is a
        // no-op rather than a phantom trigger.
        if (value >= 0.5) engine_.resetStats();
        return true;
    }
    return false;
}

double EedPitchProcessor::getParamValue (const juce::String& id) const
{
    if (id == kVoiceType)  return (double) engine_.getVoiceType();
    if (id == kResetStats) return 0.0;
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio — a READER in P0: the buffer is never written
// ---------------------------------------------------------------------------
void EedPitchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
}

void EedPitchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel with no input behind it — the one write this
    // device is allowed, and it never touches a channel that carries signal.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    // Read-only tap: detection runs on the audio thread (the correction
    // phases need f0 synchronously per hop), but there is no code below that
    // writes the buffer, at any setting.
    engine_.process (buffer.getReadPointer (0),
                     numCh > 1 ? buffer.getReadPointer (1) : nullptr,
                     buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedPitchProcessor::createEditor()
{
    return new EedPitchEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makePitchDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Pitch";
        d.category        = "Analysis";      // honest for P0: it only reads.
                                             // Moves to its own slot when the
                                             // corrector phases land.
        d.descriptiveName = "EchoJay pitch detector (built in, phase P0)";

        // ASCII ONLY (see the template's warning about mojibake in the feed).
        // The summary must not promise correction that P0 does not do.
        d.summary         = "BUILD PHASE P0, DETECTION ONLY: reads the pitch of "
                            "a monophonic voice or instrument and shows f0, "
                            "cents, confidence, voiced state and octave-guard "
                            "count. Audio passes through untouched - this is "
                            "not yet a pitch corrector; do not add it expecting "
                            "tuning.";

        // Frozen once shipped (saved chain XML carries both).
        d.identifier      = "echojay:builtin:pitch";
        d.uid             = 0x456A5043;      // 'EjPC'

        d.aliases         = { "EchoJayPitch", "Pitch Detector" };
        d.schema          = EedPitchProcessor::schema();
        d.create          = [] { return std::make_unique<EedPitchProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar pitchRegistrar { makePitchDevice() };
}
