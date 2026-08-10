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
        { EedPitchProcessor::kMode, "",
          0.0, (double) (EedPitchProcessor::kNumModes - 1), (double) EedPitchProcessor::kNatural,
          "the CHARACTER of the correction, and the fastest way to get one. "
          "natural is transparent - the tuning is tidied but the performance "
          "survives, and on a good take it is hard to hear; use it when the "
          "brief is 'tune it but keep it sounding natural'. balanced is "
          "audibly corrected but still human. tuned is the modern pop sound. "
          "hard is the unmistakable effect where every note snaps instantly - "
          "reach for hard when the brief is 'make it obviously tuned', "
          "'autotuned', 'robotic' or 'T-Pain'. Selecting a mode WRITES the "
          "individual params below (retune_speed_ms, flex, humanize, "
          "targeting_ignores_vibrato) so you can see what it did and adjust "
          "from there; moving any of them by hand shows custom",
          false,
          { "natural", "balanced", "tuned", "hard", "custom" } },

        // ---- P2: the musical layer ------------------------------------
        { EedPitchProcessor::kCorrect, "", 0.0, 1.0, 1.0,
          "correction on/off. ON by default - adding this device to a chain "
          "corrects. Turn it OFF to A/B against the untouched signal without "
          "removing the device; bypass and mix 0 do the same thing with the "
          "same reported latency",
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

        { EedPitchProcessor::kIgnoreVib, "", 0.0, 1.0, 1.0,
          "stops a wide vibrato flipping the target between neighbouring notes. "
          "Target selection uses a slow-smoothed pitch while the correction "
          "still follows the fast one, so the vibrato survives and the note "
          "does not chatter",
          true },

        { EedPitchProcessor::kKeyRoot, "", 0.0, 11.0, 0.0,
          "the root the scale is built on. IGNORED while scale is chromatic, "
          "which is the default - so setting this alone changes nothing. Only "
          "meaningful once the user has told you the key and you have set a "
          "named scale to match",
          false,
          { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },

        { EedPitchProcessor::kScale, "", 0.0, 10.0, 9.0,
          "which notes correction is allowed to choose. DEFAULT TO CHROMATIC "
          "UNLESS THE USER STATES A KEY: this device does not detect the key "
          "yet, and forcing a scale the song is not actually in is measurably "
          "WORSE THAN NOT CORRECTING AT ALL - a take left 13 cents from the "
          "nearest note on average was pushed to 29 cents by correcting it to "
          "a wrong key, because partial correction toward foreign notes lands "
          "between semitones. Chromatic still tunes every note and cannot "
          "force a wrong one. Set a named scale only when the user names the "
          "key, or when they ask for the tighter, more obviously-tuned sound "
          "and have told you what key the song is in",
          false,
          { "major", "minor", "harmonic_minor", "dorian", "mixolydian",
            "major_pentatonic", "minor_pentatonic", "blues", "whole_tone",
            "chromatic", "custom" } },

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

        // ---- the headline control -------------------------------------
        { EedPitchProcessor::kMix, "%", 0.0, 100.0, 100.0,
          "blend against the untouched signal. 100 is fully corrected; below "
          "that the original sits underneath, which thickens a lead and hides "
          "correction artefacts. 40-70 is the parallel-tuning trick for a take "
          "that only needs help in places",
          false },

        { EedPitchProcessor::kOutputDb, "dB", -24.0, 24.0, 0.0,
          "final output trim, after everything else", false },

        { EedPitchProcessor::kTargetHz, "Hz",
          0.0, (double) PsolaEngine::kMaxTargetHz, 0.0,
          "DIAGNOSTIC, not a musical control: forces every voiced frame to one "
          "fixed pitch, ignoring key, scale and retune speed. Useful for "
          "checking the shifter in isolation or for a deliberate monotone "
          "effect. Leave at 0 for normal correction - to tune a vocal, use "
          "correction_mode with correct on, never this",
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
    if (id == kMode)        { applyMode ((int) std::lround (value)); return true; }
    if (id == kMix)         { psola_.setMixPercent ((float) value);  return true; }
    if (id == kOutputDb)    { psola_.setOutputDb ((float) value);    return true; }
    if (id == kCorrect)     { correctOn_.store (value >= 0.5); return true; }
    // These four are what a mode writes, so moving one by hand means the
    // display no longer honestly names the state.
    if (id == kRetuneMs)    { correct_.setRetuneMs ((float) value); toCustomMode(); return true; }
    if (id == kFlex)        { correct_.setFlex ((float) value);     toCustomMode(); return true; }
    if (id == kHumanize)    { correct_.setHumanize ((float) value); toCustomMode(); return true; }
    if (id == kKeyRoot)     { correct_.setKeyRoot ((int) std::lround (value)); return true; }
    if (id == kScale)       { applyScale ((int) std::lround (value)); return true; }
    if (id == kReferenceHz) { correct_.setReferenceHz ((float) value); return true; }
    if (id == kTranspose)   { correct_.setTranspose ((float) value);   return true; }
    if (id == kIgnoreVib)   { correct_.setIgnoreVibrato (value >= 0.5); toCustomMode(); return true; }
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
    if (id == kMode)        return (double) modeIndex_.load();
    if (id == kMix)         return (double) psola_.getMixPercent();
    if (id == kOutputDb)    return (double) psola_.getOutputDb();
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
// correction_mode — a named point in the parameter space, not a hidden branch
// ---------------------------------------------------------------------------
// Spec §4: selecting a mode WRITES the visible params. It does not switch to a
// different algorithm and it does not hide anything. That is what makes the
// mode dialable by a model AND adjustable by a human from the same state - and
// it is why the summary below names every value it wrote rather than saying
// "mode = natural" over six knobs that silently moved.
juce::String EedPitchProcessor::applyMode (int mode)
{
    const int m = juce::jlimit (0, (int) kNumModes - 1, mode);
    modeIndex_.store (m);
    if (m == kCustom) { pendingModeSummary_ = {}; return "correction_mode custom"; }

    struct Preset { float retune, flex, humanize; bool ignoreVib; };
    static const Preset kPresets[4] = {
        { 120.0f, 55.0f, 60.0f, true  },   // natural
        {  40.0f, 25.0f, 30.0f, true  },   // balanced
        {   8.0f,  0.0f,  0.0f, false },   // tuned
        {   0.0f,  0.0f,  0.0f, false },   // hard
    };
    const Preset& p = kPresets[m];

    const juce::ScopedValueSetter<bool> guard (applyingMode_, true);
    correct_.setRetuneMs (p.retune);
    correct_.setFlex (p.flex);
    correct_.setHumanize (p.humanize);
    correct_.setIgnoreVibrato (p.ignoreVib);

    // Every mode preserves formants: the character is retune speed and how much
    // deviation survives, never whether it still sounds like the singer.
    psola_.setFormantMode (echojay::PsolaEngine::kFormantPreserve);

    const auto* spec = schema().find (kMode);
    juce::String name = spec != nullptr ? juce::String (spec->choiceLabel (m)) : juce::String (m);

    pendingModeSummary_ = "which set retune_speed_ms " + juce::String (p.retune, 0)
         + ", flex " + juce::String (p.flex, 0)
         + ", humanize " + juce::String (p.humanize, 0)
         + ", targeting_ignores_vibrato " + juce::String (p.ignoreVib ? "on" : "off")
         + ", formant_mode preserve";

    return "correction_mode " + name
         + " (retune_speed_ms " + juce::String (p.retune, 0)
         + ", flex " + juce::String (p.flex, 0)
         + ", humanize " + juce::String (p.humanize, 0)
         + ", targeting_ignores_vibrato " + juce::String (p.ignoreVib ? "on" : "off")
         + ", formant_mode preserve)";
}

// ---------------------------------------------------------------------------
// pitch_scale — the array move, MERGE semantics (spec §5.1)
// ---------------------------------------------------------------------------
juce::String EedPitchProcessor::applyPitchScale (const juce::var& arr,
                                                 int* applied, int* skipped)
{
    if (! arr.isArray()) return {};

    juce::StringArray touched;
    for (const auto& entry : *arr.getArray())
    {
        if (! entry.isObject()) { if (skipped) ++*skipped; continue; }

        // `semitone` is the KEY. Without it there is nothing to merge against,
        // so the entry is skipped rather than guessed at.
        if (! entry.hasProperty ("semitone")) { if (skipped) ++*skipped; continue; }
        const int st = (int) entry.getProperty ("semitone", -1);
        if (st < 0 || st > 11) { if (skipped) ++*skipped; continue; }

        // MERGE: a field the caller omitted keeps its current value. This is
        // the opposite of eq_bands, which replaces.
        bool  en   = correct_.degreeEnabled (st);
        float bias = correct_.degreeBias (st);
        if (entry.hasProperty ("enabled"))    en   = (bool) entry.getProperty ("enabled", en);
        if (entry.hasProperty ("bias_cents")) bias = (float) (double) entry.getProperty ("bias_cents", bias);

        correct_.setDegree (st, en, bias);
        if (applied) ++*applied;

        static const char* kNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        juce::String one (kNames[st]);
        one << (en ? " on" : " off");
        if (std::abs (bias) > 0.01f) one << " " << juce::String (bias, 0) << "c";
        touched.add (one);
    }

    if (touched.isEmpty()) return {};

    // Editing a degree moves the scale display to custom - same visible-state
    // rule as correction_mode.
    scaleIndex_.store (kScaleCustom);
    return "pitch_scale " + touched.joinIntoString (", ") + " (scale custom)";
}

juce::String EedPitchProcessor::applyStructured (const juce::var& structured,
                                                 int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;
    if (! structured.isObject()) return EedDeviceProcessor::applyStructured (structured, appliedOut, skippedOut);

    juce::StringArray parts;
    pendingModeSummary_ = {};
    pendingScaleSummary_ = {};

    // The flat params first, so a move that sets `scale` AND edits a degree
    // ends with the degree edit winning - which is what the caller wrote.
    if (structured.hasProperty ("params"))
    {
        const auto p = applyParams (structured.getProperty ("params", juce::var()),
                                    appliedOut, skippedOut);
        if (p.isNotEmpty()) parts.add (p);

        // Put back what a mode or a named scale actually DID. Six knobs moving
        // under one line saying "correction_mode = natural" is the same failure
        // as a mode that hides its behaviour entirely.
        if (pendingModeSummary_.isNotEmpty())  parts.add (pendingModeSummary_);
        if (pendingScaleSummary_.isNotEmpty()) parts.add (pendingScaleSummary_);
    }

    if (structured.hasProperty ("pitch_scale"))
    {
        const auto p = applyPitchScale (structured.getProperty ("pitch_scale", juce::var()),
                                        appliedOut, skippedOut);
        if (p.isNotEmpty()) parts.add (p);
    }

    return parts.joinIntoString ("; ");
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
    // `custom` is a DISPLAY state reached by editing a degree, not a mask to
    // write. Selecting it explicitly leaves the degrees exactly as they are.
    if (index >= kScaleCustom) { scaleIndex_.store (kScaleCustom); return; }
    const int n = (int) (sizeof (kMasks) / sizeof (kMasks[0]));
    const int i = juce::jlimit (0, n - 1, index);
    scaleIndex_.store (i);

    juce::StringArray on;
    static const char* kNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    for (int s = 0; s < 12; ++s)
    {
        const bool en = (kMasks[i] >> s) & 1;
        correct_.setDegree (s, en, correct_.degreeBias (s));
        if (en) on.add (kNames[s]);
    }

    const auto* spec = schema().find (kScale);
    (void) spec;
    pendingScaleSummary_ = "rewriting all 12 degrees, allowing "
                         + on.joinIntoString (" ");
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
                            "or lead instrument - a vocal, or a single-note "
                            "lead. Set correct:1 and pick correction_mode: "
                            "natural when the brief is 'tune it but keep it "
                            "natural', hard when it is 'make it obviously "
                            "tuned' or 'autotuned'. A mode writes the "
                            "individual knobs so you can see and adjust what "
                            "it did. IT DOES NOT KNOW THE SONG'S KEY: leave "
                            "scale on chromatic unless the user tells you the "
                            "key, because correcting to a WRONG key measures "
                            "worse than not correcting at all. Formants are "
                            "preserved so a corrected voice still sounds like "
                            "the same singer; unvoiced frames pass through "
                            "untouched. Not for chords or polyphonic material. "
                            "To allow or forbid individual notes, send a "
                            "pitch_scale array of {semitone, enabled, "
                            "bias_cents} - it MERGES on semitone, so a degree "
                            "you omit KEEPS its current state and you only send "
                            "the ones you are changing. (Note this differs from "
                            "eq_bands, which REPLACES.) Editing any degree "
                            "moves scale to custom.";

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
