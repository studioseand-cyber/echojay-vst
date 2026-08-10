/*
    EedPitchProcessor.cpp  —  see EedPitchProcessor.h.
*/

#include "EedPitchProcessor.h"
#include "EedPitchEditor.h"
#include "EedDeviceRegistry.h"

using echojay::PitchEngine;
using echojay::PsolaEngine;
using echojay::PitchCorrect;

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

        { EedPitchProcessor::kFormantMode, "",
          0.0, (double) (PsolaEngine::kNumFormantModes - 1),
          (double) PsolaEngine::kFormantPreserve,
          "what happens to the vocal character when pitch moves: preserve "
          "keeps the formants where they are so a shifted voice still sounds "
          "like the same singer, off lets them move with the pitch for the "
          "chipmunk/resampler effect",
          false,
          // Mirrors PsolaEngine::FormantMode, and APPEND-ONLY: `shift` (LPC
          // envelope warping) is a later phase and becomes index 2.
          { "off", "preserve" } },

        { EedPitchProcessor::kLowLatency, "", 0.0, 1.0, 0.0,
          "turn ON when the singer is TRACKING through this plugin and needs "
          "to monitor themselves - it shortens the shifter's lookahead and "
          "cuts the reported latency by a third. Leave OFF when this is a MIX: "
          "the extra delay is compensated by the host and costs nothing, while "
          "the shorter lookahead trades a little transient accuracy on note "
          "onsets. Latency depends on voice_type - a bass setting is long "
          "enough that monitoring is not practical either way",
          true },

        // ---- P2: the musical layer ------------------------------------
        { EedPitchProcessor::kCorrect, "", 0.0, 1.0, 0.0,
          "turn ON to actually correct pitch to the key and scale. OFF leaves "
          "the audio untouched (target_hz still works as a fixed-target lab "
          "control). This is the switch that turns the device into a corrector",
          true },

        { EedPitchProcessor::kRetuneMs, "ms",
          (double) PitchCorrect::kMinRetuneMs, (double) PitchCorrect::kMaxRetuneMs,
          (double) PitchCorrect::kDefRetuneMs,
          "how fast pitch is pulled to the target; 0 is the hard tuned effect "
          "where every note snaps instantly, 100+ is transparent and keeps the "
          "singer's own movement between notes",
          false },

        { EedPitchProcessor::kFlex, "%", 0.0, 100.0, 55.0,
          "how much expressive drift is left alone before correction engages; "
          "high keeps slides, scoops and deliberate blue notes, 0 corrects "
          "every deviation however small",
          false },

        { EedPitchProcessor::kHumanize, "%", 0.0, 100.0, 60.0,
          "relaxes correction on SUSTAINED notes while keeping onsets tight, so "
          "long notes do not sound frozen. Sustain is judged from how long the "
          "pitch has been steady, not from how loud it is",
          false },

        { EedPitchProcessor::kKeyRoot, "", 0.0, 11.0, 0.0,
          "the key the scale is built on",
          false,
          { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },

        { EedPitchProcessor::kScale, "", 0.0, 9.0, 9.0,
          "which notes correction is allowed to choose. chromatic is the safe "
          "default - it still tunes and cannot force a note that is wrong for "
          "the song; a named scale is tighter and more obviously tuned",
          false,
          { "major", "minor", "harmonic_minor", "dorian", "mixolydian",
            "major_pentatonic", "minor_pentatonic", "blues", "whole_tone",
            "chromatic" } },

        { EedPitchProcessor::kReferenceHz, "Hz",
          (double) PitchCorrect::kMinReferenceHz, (double) PitchCorrect::kMaxReferenceHz,
          440.0,
          "concert pitch reference. Set it to the track's actual tuning - a "
          "vocal corrected to 440 against a band at 441.3 sits subtly wrong "
          "against everything",
          false },

        { EedPitchProcessor::kTranspose, "st",
          (double) PitchCorrect::kMinTranspose, (double) PitchCorrect::kMaxTranspose, 0.0,
          "shifts the corrected result in semitones, after correction",
          false },

        { EedPitchProcessor::kIgnoreVib, "", 0.0, 1.0, 1.0,
          "stops a wide vibrato flipping the target between neighbouring notes. "
          "Target selection uses a slow-smoothed pitch while the correction "
          "still follows the fast one, so the vibrato survives and the note "
          "does not chatter",
          true },

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
    if (id == kFormantMode)
    {
        psola_.setFormantMode ((int) std::lround (value));
        return true;
    }
    if (id == kLowLatency)
    {
        psola_.setLookaheadPeriods (value >= 0.5 ? kLookaheadTracking : kLookaheadMixing);
        latencyVoiceType_ = -1;          // force the host to be told
        refreshLatency();
        return true;
    }
    if (id == kCorrect)     { correctOn_.store (value >= 0.5); return true; }
    if (id == kRetuneMs)    { correct_.setRetuneMs ((float) value);   return true; }
    if (id == kFlex)        { correct_.setFlex ((float) value);       return true; }
    if (id == kHumanize)    { correct_.setHumanize ((float) value);   return true; }
    if (id == kKeyRoot)     { correct_.setKeyRoot ((int) std::lround (value)); return true; }
    if (id == kScale)       { applyScale ((int) std::lround (value)); return true; }
    if (id == kReferenceHz) { correct_.setReferenceHz ((float) value); return true; }
    if (id == kTranspose)   { correct_.setTranspose ((float) value);   return true; }
    if (id == kIgnoreVib)   { correct_.setIgnoreVibrato (value >= 0.5); return true; }
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
    if (id == kFormantMode) return (double) psola_.getFormantMode();
    if (id == kLowLatency)  return psola_.getLookaheadPeriods() <= kLookaheadTracking + 0.01f
                                 ? 1.0 : 0.0;
    if (id == kCorrect)     return correctOn_.load() ? 1.0 : 0.0;
    if (id == kRetuneMs)    return (double) correct_.getRetuneMs();
    if (id == kFlex)        return (double) correct_.getFlex();
    if (id == kHumanize)    return (double) correct_.getHumanize();
    if (id == kKeyRoot)     return (double) correct_.getKeyRoot();
    if (id == kScale)       return (double) scaleIndex_.load();
    if (id == kReferenceHz) return (double) correct_.getReferenceHz();
    if (id == kTranspose)   return (double) correct_.getTranspose();
    if (id == kIgnoreVib)   return correct_.getIgnoreVibrato() ? 1.0 : 0.0;
    if (id == kResetStats) return 0.0;
    return 0.0;
}

// ---------------------------------------------------------------------------
// scales
// ---------------------------------------------------------------------------
// Semitone masks relative to key_root. Order MIRRORS the schema's choices list.
void EedPitchProcessor::applyScale (int index)
{
    static const uint16_t kMasks[] = {
        0b101010110101,   // major            C D E F G A B
        0b010110101101,   // minor (natural)
        0b100110101101,   // harmonic minor
        0b010101101101,   // dorian
        0b011010110101,   // mixolydian
        0b001010010101,   // major pentatonic
        0b010010101001,   // minor pentatonic
        0b010011001001,   // blues
        0b010101010101,   // whole tone
        0b111111111111,   // chromatic
    };
    const int n = (int) (sizeof (kMasks) / sizeof (kMasks[0]));
    const int i = juce::jlimit (0, n - 1, index);
    scaleIndex_.store (i);

    for (int s = 0; s < 12; ++s)
        correct_.setDegree (s, (kMasks[i] >> s) & 1, correct_.degreeBias (s));
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

    // Correctness, not latency: align the f0 to the audio it describes. Always
    // on - a stale estimate makes note-change detection fire late, which P2's
    // envelope would then act on from the wrong place.
    psola_.setPitchLagSamples (engine_.pitchLagFor (vt));

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

    // The corrector runs once per DETECTOR HOP, so it needs that cadence to
    // convert its millisecond time constants.
    correct_.prepare (sampleRate, engine_.inputHopLength (engine_.getVoiceType()));
    correct_.initDegrees();
    applyScale (scaleIndex_.load());

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

    // P2: the musical layer decides WHERE the note should be. It runs on the
    // detector's reading and produces a target the shifter aims at, so a
    // detection error and a correction error stay separable.
    float target = 0.0f;
    if (correctOn_.load())
    {
        // This runs once per BLOCK, not once per detector hop, so it must be
        // told how much time the call represents.
        target = correct_.process (r.f0Hz, r.voiced,
                                   1000.0f * (float) n / (float) getSampleRate());
        psola_.setTargetHz (target);
    }

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
        d.category        = "Pitch";         // P2: it has a scale, a key and a
                                             // retune envelope, so it is an
                                             // effect now rather than the lab
                                             // instrument P0/P1 filed under
                                             // Analysis.
        d.descriptiveName = "EchoJay pitch corrector (built in)";

        // ASCII ONLY (see the template's warning about mojibake in the feed).
        // The summary must not promise correction that P0 does not do.
        d.summary         = "Real-time pitch correction for ONE monophonic voice "
                            "or lead instrument. Set correct:1, then key_root "
                            "and scale, and dial the character with "
                            "retune_speed_ms: 0 with flex 0 is the hard tuned "
                            "effect, 120 with flex and humanize up is "
                            "transparent. Formants are preserved so a corrected "
                            "voice still sounds like the same singer, and "
                            "unvoiced frames pass through untouched. Key and "
                            "scale are set by hand - it does not follow the "
                            "track's key yet. Not for polyphonic material.";

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
