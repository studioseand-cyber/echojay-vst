/*
    EedPitchProcessor.cpp  —  see EedPitchProcessor.h.
*/

#include "EedPitchProcessor.h"
#include "EedPitchEditor.h"
#include "EedDeviceRegistry.h"

using echojay::PitchEngine;
using echojay::PsolaEngine;

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

        { EedPitchProcessor::kTracking, "",
          0.0, (double) (PitchEngine::kNumTracking - 1), (double) PitchEngine::kNormal,
          "how strict the detector is before it calls a frame pitched; "
          "relaxed keeps breathy and quiet frames at the cost of occasional "
          "wrong readings, tight only trusts clearly periodic frames and "
          "leaves more of the take untracked",
          false,
          // Mirrors PitchEngine::Tracking order exactly.
          { "relaxed", "normal", "tight" } },

        { EedPitchProcessor::kTargetHz, "Hz",
          0.0, (double) PsolaEngine::kMaxTargetHz, 0.0,
          "P1 development control: when non-zero EVERY voiced frame is shifted "
          "to this one fixed pitch, formants preserved. 0 leaves the audio "
          "untouched. Musical target selection - scale, key, retune speed - "
          "arrives in a later phase; this is not yet a pitch corrector",
          false },

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
    if (id == kTracking)
    {
        engine_.setTracking ((int) std::lround (value));
        return true;
    }
    if (id == kTargetHz)
    {
        psola_.setTargetHz ((float) value);
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
    if (id == kTracking)   return (double) engine_.getTracking();
    if (id == kTargetHz)   return (double) psola_.getTargetHz();
    if (id == kResetStats) return 0.0;
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedPitchProcessor::refreshLatency()
{
    const int vt = engine_.getVoiceType();
    if (vt == latencyVoiceType_) return;
    latencyVoiceType_ = vt;

    psola_.setLowestF0 (PitchEngine::voiceRange (vt).fMinHz);
    setLatencySamples (psola_.latencySamples());
}

void EedPitchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    engine_.prepare (sampleRate, samplesPerBlock);

    // Sized for the WORST case any voice_type can select, so switching type
    // mid-playback never allocates on the audio thread.
    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = juce::jmin (worst, PitchEngine::voiceRange (t).fMinHz);

    psola_.prepare (sampleRate, samplesPerBlock,
                    PitchEngine::voiceRange (engine_.getVoiceType()).fMinHz, worst);

    latencyVoiceType_ = -1;
    refreshLatency();
}

void EedPitchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel with no input behind it — the one write this
    // device is allowed, and it never touches a channel that carries signal.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    // Bypass still runs the delay line, at target 0, so bypassing changes what
    // you hear without changing WHEN you hear it.
    if (isBypassed())
    {
        const float held = psola_.getTargetHz();
        psola_.setTargetHz (0.0f);
        for (int ch = 0; ch < numCh; ++ch)
            psola_.process (buffer.getReadPointer (ch), buffer.getWritePointer (ch),
                            buffer.getNumSamples(), 0.0f, false);
        psola_.setTargetHz (held);
        return;
    }

    refreshLatency();

    const int n = buffer.getNumSamples();

    // Detection first: the shifter runs `latencySamples()` behind, so the f0
    // for a span is always known before the shifter reaches it.
    engine_.process (buffer.getReadPointer (0),
                     numCh > 1 ? buffer.getReadPointer (1) : nullptr,
                     n);

    const echojay::PitchReading r = engine_.getReading();

    // The shifter delays unconditionally, including at target 0 and when
    // bypassed, so the reported latency is the SAME in every state and
    // bypassing never shifts the track's timing (spec §8).
    for (int ch = 0; ch < numCh; ++ch)
        psola_.process (buffer.getReadPointer (ch), buffer.getWritePointer (ch), n,
                        r.f0Hz, r.voiced);
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
        d.category        = "Analysis";      // still honest at P1: without a
                                             // scale or a retune envelope this
                                             // is a lab instrument, not an
                                             // effect anyone reaches for
                                             // musically. Moves when P2 lands.
        d.descriptiveName = "EchoJay pitch shifter (built in, phase P1)";

        // ASCII ONLY (see the template's warning about mojibake in the feed).
        // The summary must not promise correction that P0 does not do.
        d.summary         = "BUILD PHASE P1: detects the pitch of a monophonic "
                            "voice or instrument, and shifts every voiced frame "
                            "to ONE fixed target_hz with formants preserved. "
                            "Unvoiced frames pass through untouched. It has no "
                            "scale, no key and no retune speed yet, so it is "
                            "NOT a pitch corrector - do not reach for it to tune "
                            "a vocal musically. target_hz 0 is pure passthrough.";

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
