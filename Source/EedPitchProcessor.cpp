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
          0.0, (double) (EedPitchProcessor::kNumModes - 1), (double) EedPitchProcessor::kCustom,
          "PROVISIONAL DEFAULT = custom at retune 44 / flex 0 / humanize 0 / depth 100 (5 Sep 2026, UI_SIMPLIFICATION round 36): the shipped natural default (120/55/60) DETUNED the reference take - improve-rate 44.6%, off-grid 7.95c vs the source 6.72c - corroborating round 27 (flex >= 25 tunes worse than dry). Re-derived when broader material lands; this take is already near-grid. "
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
          "how fast pitch is pulled to the target - the time constant of an "
          "exponential glide, in honest milliseconds. 0 is the hard-tuned "
          "snap, 100+ is transparent and keeps the singer's own movement. "
          "THE RANGE ENDS AT 150 ms (2 Sep 2026): measured, useful "
          "correction ends there - roughness reaches its minimum near 40, "
          "frames-within-3-cents cliffs past ~150, and by 400 the glide "
          "never arrives on normal notes (all scoop). Comparable correctors "
          "expose none of that region. Saved sessions above 150 clamp on "
          "load and the readout says so. "
          "THE EFFECTIVE FLOOR IS 6 ms: values below it behave as 6, because "
          "the 0-6 zone was measured strictly dominated - same tuning, same "
          "lock speed (acquisition is floored by note-change detection), "
          "worse roughness from chasing per-hop detection jitter - and "
          "because other correctors' 'retune 0' carries equivalent internal "
          "smoothing, so 6 here IS what 0 means elsewhere. The UI readout "
          "shows the mapping. NOTE: retune 0 alone is NOT the full "
          "hard-tune - natural_vibrato re-adds the singer's own wobble and "
          "defaults to 100; for the complete snap set correction_mode hard",
          false },

        { EedPitchProcessor::kSeamAttackMs, "ms",
          0.0, 150.0, 60.0,
          "SEAM ATTACK (2 Sep 2026): at every word start the correction "
          "used to arrive as an instantaneous pitch STEP the moment the wet "
          "path resumed after the bit-exact-dry consonant (9 cents at the "
          "ear-confirmed exemplar - above the pitch JND). With attack > 0 "
          "the wet path resumes AT THE SUNG PITCH and ramps to full "
          "correction over this many ms - pitch continuity at the seam, the "
          "same move as GSnap's Attack and Waves Tune RT's Note Transition. "
          "Measured at 60ms: word-start glitch events fall to ZERO "
          "(ignore-vib off) with no paired tuning regression. 0 = off "
          "(pre-fix behaviour). DEFAULT 60 (3 Sep 2026, ear gate passed on "
          "the intro region - no audible damage): the geometric midpoint of "
          "the ~30ms pitch-integration floor and Waves' field-proven 120ms, "
          "and the measured event-elimination point. Validated on one "
          "already-corrected take + one falsifier only - broader material "
          "validation still owed before this default is called shipped",
          false },   // was `true` in the boolean slot: a continuous knob advertised as an on/off switch (5 Sep 2026 verification)

        { EedPitchProcessor::kFlex, "%", 0.0, 100.0, 0.0,      // PROVISIONAL DEFAULT 0 (was 55): measured to detune on the reference take
          "how much expressive drift is left alone before correction engages; "
          "high keeps slides, scoops and deliberate blue notes, 0 corrects "
          "every deviation however small",
          false },

        { EedPitchProcessor::kHumanize, "%", 0.0, 100.0, 0.0,   // PROVISIONAL DEFAULT 0 (was 60), with the flex change
          "relaxes correction on SUSTAINED notes while keeping onsets tight, so "
          "long notes do not sound frozen. Sustain is judged from how long the "
          "pitch has been steady, not from how loud it is",
          false },
        { EedPitchProcessor::kDepth, "%", 0.0, 100.0, 100.0,
          "DEPTH (5 Sep 2026): how much of the correction is APPLIED. 100 is "
          "full correction (today's sound, bit-identical); 0 is exact identity "
          "- the dry voice. Blended AFTER the retune envelope on the applied "
          "shift, so it is the control the slow end of the retune dial only "
          "pretends to be: Antares' slow end is transparent because it never "
          "commits, not because it is slow. Measured on the reference take: "
          "retune 150 at 25 -> 1.7c of movement (Antares max retune 0.6c) "
          "while still improving the grid at 58%; retune 44 at 50 -> 2.8c at "
          "64%. Gentleness lives HERE, not in Flex (which is a threshold and "
          "tunes worse than dry above 25 on this material). Every mode writes "
          "100 - a mode is a character, depth is how much of it you take",
          false },   // was `true` in the boolean slot: a continuous knob advertised as an on/off switch (5 Sep 2026 verification)

        { EedPitchProcessor::kIgnoreVib, "", 0.0, 1.0, 1.0,
          "stops a wide vibrato flipping the target between neighbouring notes. "
          "Target selection uses a slow-smoothed pitch while the correction "
          "still follows the fast one, so the vibrato survives and the note "
          "does not chatter",
          true },

        { EedPitchProcessor::kKeySource, "", 0.0, 1.0, 0.0,
          "where the key comes from. auto FOLLOWS THE DETECTED KEY of the "
          "music - EchoJay measures it on the mix or instrumental bus and this "
          "device tracks it, including a mid-song modulation, so you do not "
          "have to know or state the key. manual uses key_root and scale as "
          "set. In auto, a reading that is not confident enough falls back to "
          "CHROMATIC rather than to a guess or to a stale key, and the UI "
          "names which source the key came from",
          false, { "auto", "manual" } },

        { EedPitchProcessor::kKeyRoot, "", 0.0, 11.0, 0.0,
          "the root the scale is built on. IGNORED while scale is chromatic, "
          "which is the default - so setting this alone changes nothing. Only "
          "meaningful once the user has told you the key and you have set a "
          "named scale to match",
          false,
          { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },

        { EedPitchProcessor::kScale, "", 0.0, 10.0, 9.0,
          "which notes correction is allowed to choose. With key_source auto "
          "this FOLLOWS THE DETECTED KEY - leave it alone and it tracks the "
          "song. Setting it by hand switches key_source to manual, and then: "
          "NEVER GUESS A KEY. Forcing a scale the song is not actually in is "
          "measurably WORSE THAN NOT CORRECTING AT ALL - a take left 13 cents "
          "from the nearest note on average was pushed to 29 cents by "
          "correcting it to a wrong key, because partial correction toward "
          "foreign notes lands between semitones. If no key is detected and "
          "the user has not named one, chromatic is the answer: it still tunes "
          "every note and cannot force a wrong one",
          false,
          { "major", "minor", "harmonic_minor", "dorian", "mixolydian",
            "major_pentatonic", "minor_pentatonic", "blues", "whole_tone",
            "chromatic", "custom" } },

        { EedPitchProcessor::kRefSource, "", 0.0, 1.0, 0.0,
          "where concert pitch comes from. auto follows the TUNING EchoJay "
          "detected in the music, so a track cut at 441.3 Hz is corrected to "
          "441.3 rather than dragged to 440 and left sitting subtly wrong "
          "against everything else - but NEVER from this channel itself: a "
          "grid derived from the signal being corrected is circular, so when "
          "the only measurable material is this channel (a solo instance), "
          "auto falls back to 440. manual uses reference_hz as set",
          false, { "auto", "manual" } },

        { EedPitchProcessor::kReferenceHz, "Hz",
          (double) PitchCorrect::kMinReferenceHz, (double) PitchCorrect::kMaxReferenceHz,
          440.0,
          "concert pitch reference. Set it to the track's actual tuning - a "
          "vocal corrected to 440 against a band at 441.3 sits subtly wrong "
          "against everything",
          false },

        { EedPitchProcessor::kRefManualByUser, "", 0.0, 1.0, 0.0,
          "internal provenance marker, not a control: 1 once a person has "
          "taken manual control of the reference (set reference_hz or chose "
          "reference_source manual live). Saved states from before this "
          "marker existed could carry a DETECTED reference laundered into "
          "the manual field; on load, a manual reference without this marker "
          "reverts to auto. Do not set this directly",
          false },

        { EedPitchProcessor::kTranspose, "st",
          (double) PitchCorrect::kMinTranspose, (double) PitchCorrect::kMaxTranspose, 0.0,
          "shifts the corrected result in semitones, after correction",
          false },

        { EedPitchProcessor::kNaturalVib, "%", 0.0, 200.0, 100.0,
          "AS SHIPPED THIS IS A SWITCH, NOT A GAIN (measured 5 Sep 2026, "
          "UI_SIMPLIFICATION.md ruling 3): 100 keeps the singer's own vibrato "
          "exactly as sung; EVERY OTHER VALUE removes it entirely - 0, 40, 150 "
          "and 200 render bit-identically. The intended gain (0 dead-still, "
          "200 exaggerated) reaches the audio only on the shift path, which is "
          "selected only at 100; making it a true gain at every value is DSP "
          "work with its own bar (the ring-aligned fast term). Presets: "
          "natural and balanced 100 (keep); tuned 40 and hard 0 (REMOVE). "
          "Applies at every retune speed",
          false },   // was `true` in the boolean slot: a continuous knob advertised as an on/off switch (5 Sep 2026 verification)

        { EedPitchProcessor::kVibDepth, "c", 0.0, 100.0, 0.0,
          "depth of ADDED vibrato, in cents. 0 is off and is the default - "
          "this generates movement that was never sung, so reach for it only "
          "when asked for vibrato that is not there, not to fix a flat take",
          false },

        { EedPitchProcessor::kVibRate, "Hz", 0.1, 10.0, 5.5,
          "rate of the ADDED vibrato. 5-7 Hz is a natural singing rate; slower "
          "reads as a swell, much faster as an effect",
          false },

        { EedPitchProcessor::kVibShape, "", 0.0, 2.0, 0.0,
          "waveform of the added vibrato: sine is the natural one, triangle is "
          "more even and slightly mechanical, ramp is a repeated scoop",
          false, { "sine", "triangle", "ramp" } },

        { EedPitchProcessor::kVibOnset, "ms", 0.0, 3000.0, 300.0,
          "how long a note is held before the ADDED vibrato fades in, so it "
          "starts the way a singer would rather than arriving fully formed on "
          "the attack. Measured from the start of each note",
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
          "what happens to the vocal character when pitch moves. preserve is "
          "the default and the right answer for correction. off is a "
          "TRANSPOSE-TIME control, not a correction mode: at tuning-sized "
          "corrections it is measured IDENTICAL to preserve (and the build "
          "asserts that equivalence), and it only matters when transpose "
          "moves pitch by semitones - preserved formants keep the same "
          "singer, moving formants make a smaller or larger one. Setting off "
          "with transpose at 0 changes nothing. shift keeps formants "
          "independent of pitch AND moves them by formant_shift, on a "
          "heavier synthesis path with a MEASURED quality cost even at "
          "formant_shift 0 - about 1.6 dB of harmonicity and several times "
          "the roughness of preserve - so reach for it only when the "
          "character change IS the point",
          false,
          // Mirrors PsolaEngine::FormantMode, APPEND-ONLY.
          { "off", "preserve", "shift" } },

        { EedPitchProcessor::kFormantShift, "st",
          (double) -PsolaEngine::kMaxFormantShiftSt,
          (double) PsolaEngine::kMaxFormantShiftSt, 0.0,
          "moves the vocal character independently of pitch - the throat-"
          "length control. Negative reads bigger and deeper, positive smaller "
          "and brighter; a couple of semitones is a subtle character change, "
          "the extremes are an effect. ONLY ACTIVE when formant_mode is "
          "shift, which carries a measured fidelity cost regardless of this "
          "value (see formant_mode) - the moved character rides on a rougher "
          "voice. An EFFECT control: never part of making correction sound "
          "transparent or natural",
          false },

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
        forEachShifter ([&] (auto& e) { e.setTargetHz ((float) value); });
        return true;
    }
    if (id == kFormantMode)
    {
        forEachShifter ([&] (auto& e) { e.setFormantMode ((int) std::lround (value)); });
        return true;
    }
    if (id == kFormantShift)
    {
        forEachShifter ([&] (auto& e) { e.setFormantShift ((float) value); });
        return true;
    }
    if (id == kLowLatency)
    {
        forEachShifter ([&] (auto& e) { e.setLookaheadPeriods (value >= 0.5 ? kLookaheadTracking : kLookaheadMixing); });
        latencyVoiceType_ = -1;          // force the host to be told
        refreshLatency();
        return true;
    }
    if (id == kKeySource)
    {
        // manual = 1, auto = 0 (choices order). Switching back to auto must
        // re-apply on the next block, so the memo is cleared.
        keyAuto_.store (value < 0.5);
        lastAutoRoot_ = -1; lastAutoFellBack_ = false;
        return true;
    }
    if (id == kRefSource)
    {
        const bool manual = value >= 0.5;
        refAuto_.store (! manual); lastAutoTuning_ = 0.0f;
        // Manual mode applies the MANUAL field - never whatever detection
        // last left in the corrector (the laundering path, 29 Aug 2026).
        if (manual) correct_.setReferenceHz (manualRefHz_.load());
        if (manual && ! writingDefaults_ && ! applyingState())
            refManualByUser_.store (true);
        return true;
    }
    if (id == kRefManualByUser) { refManualByUser_.store (value >= 0.5); return true; }
    if (id == kDepth)       { correct_.setDepth ((float) value * 0.01f); return true; }
    if (id == kMode)        { applyMode ((int) std::lround (value)); return true; }
    if (id == kNaturalVib)  { correct_.setNaturalVibrato ((float) value); toCustomMode(); return true; }
    if (id == kVibDepth)    { correct_.setVibDepthCents ((float) value);  return true; }
    if (id == kVibRate)     { correct_.setVibRateHz ((float) value);      return true; }
    if (id == kVibShape)    { correct_.setVibShape ((int) std::lround (value)); return true; }
    if (id == kVibOnset)    { correct_.setVibOnsetMs ((float) value);     return true; }
    if (id == kMix)         { forEachShifter ([&] (auto& e) { e.setMixPercent ((float) value); });  return true; }
    if (id == kOutputDb)    { forEachShifter ([&] (auto& e) { e.setOutputDb ((float) value); });    return true; }
    if (id == kCorrect)     { correctOn_.store (value >= 0.5); return true; }
    // These four are what a mode writes, so moving one by hand means the
    // display no longer honestly names the state.
    if (id == kRetuneMs)
    {
        // The 150ms cap (2 Sep 2026): clamp ON LOAD with memory for the
        // readout; live writes cannot exceed the schema max anyway.
        if (value > (double) PitchCorrect::kMaxRetuneMs && applyingState())
            retuneWasMs_ = (float) value;
        else if (! applyingState())
            retuneWasMs_ = 0.0f;
        correct_.setRetuneMs ((float) value);
        toCustomMode(); return true;
    }
    if (id == kFlex)        { correct_.setFlex ((float) value);     toCustomMode(); return true; }
    if (id == kSeamAttackMs){ forEachShifter ([&] (auto& e) { e.setSeamRampMs ((float) value); }); return true; }
    if (id == kHumanize)    { correct_.setHumanize ((float) value); toCustomMode(); return true; }
    if (id == kKeyRoot)
    {
        // Setting the key by hand IS choosing manual - otherwise the next
        // block silently overwrites it and the control looks broken.
        correct_.setKeyRoot ((int) std::lround (value));
        if (! writingDefaults_) keyAuto_.store (false);
        return true;
    }
    if (id == kScale)       { applyScale ((int) std::lround (value));
                              if (! writingDefaults_) keyAuto_.store (false); return true; }
    if (id == kReferenceHz)
    {
        // The value lands in the MANUAL FIELD. Only a LIVE write - a person
        // at the knob, or their agent in chat - takes manual control; a
        // state load merely replays the field and decides the mode via
        // reference_source (plus the onStateApplied migration).
        manualRefHz_.store (juce::jlimit (PitchCorrect::kMinReferenceHz,
                                          PitchCorrect::kMaxReferenceHz, (float) value));
        if (! writingDefaults_ && ! applyingState())
        { refAuto_.store (false); refManualByUser_.store (true); }
        if (! refAuto_.load()) correct_.setReferenceHz (manualRefHz_.load());
        return true;
    }
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
    if (id == kTargetHz)   return (double) shifter().getTargetHz();
    if (id == kFormantMode) return (double) shifter().getFormantMode();
    if (id == kFormantShift) return (double) shifter().getFormantShift();
    if (id == kLowLatency)  return shifter().getLookaheadPeriods() <= kLookaheadTracking + 0.01f
                                 ? 1.0 : 0.0;
    if (id == kKeySource)   return keyAuto_.load() ? 0.0 : 1.0;
    if (id == kDepth)       return (double) correct_.getDepth() * 100.0;
    if (id == kRefSource)   return refAuto_.load() ? 0.0 : 1.0;
    if (id == kMode)        return (double) modeIndex_.load();
    if (id == kNaturalVib)  return (double) correct_.getNaturalVibrato();
    if (id == kVibDepth)    return (double) correct_.getVibDepthCents();
    if (id == kVibRate)     return (double) correct_.getVibRateHz();
    if (id == kVibShape)    return (double) correct_.getVibShape();
    if (id == kVibOnset)    return (double) correct_.getVibOnsetMs();
    if (id == kMix)         return (double) shifter().getMixPercent();
    if (id == kOutputDb)    return (double) shifter().getOutputDb();
    if (id == kCorrect)     return correctOn_.load() ? 1.0 : 0.0;
    if (id == kRetuneMs)    return (double) correct_.getRetuneMs();
    if (id == kSeamAttackMs) return (double) psola_[0].getSeamRampMs();
    if (id == kFlex)        return (double) correct_.getFlex();
    if (id == kHumanize)    return (double) correct_.getHumanize();
    if (id == kKeyRoot)     return (double) correct_.getKeyRoot();
    if (id == kScale)       return (double) scaleIndex_.load();
    if (id == kReferenceHz) return (double) manualRefHz_.load();   // the manual FIELD, never the live/detected grid
    if (id == kRefManualByUser) return refManualByUser_.load() ? 1.0 : 0.0;
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

    // Spec §4's table, in full. natural_vibrato is part of it and MUST be
    // written: since the corrector separates the note from the wobble and adds
    // the wobble back at natural_vibrato, a mode that snaps the note but leaves
    // the wobble at 100% is not hard-tuned at all. Measured when this was
    // missing: hard tune came out at 15.7 cents mean deviation against 13.0 for
    // the untouched signal - i.e. WORSE than not correcting.
    struct Preset { float retune, flex, humanize, naturalVib; bool ignoreVib; };
    // seam_attack_ms is WRITTEN BY EVERY MODE (3 Sep 2026 ruling), at the
    // schema's default - one source of truth, consulted here rather than
    // copied: it is a fix constant (the word-start pitch-continuity ramp),
    // not a mode choice, but a character-bearing param that a mode does not
    // write is exactly how natural_vibrato shipped WORSE than dry (§12).
    const float seamAttack = [&]
    { const auto* sp = schema().find (kSeamAttackMs); return sp != nullptr ? (float) sp->def : 0.0f; }();
    // targeting_ignores_vibrato is ON in ALL FOUR modes. The spec's first §4
    // table set it off for tuned/hard, and that was a spec error (its author's
    // words), corrected in both places 2026-08-14: target selection without
    // vibrato smoothing flips between adjacent scale degrees whenever a wide
    // vibrato sits on a semitone boundary, and that is true no matter how
    // hard the retune - Antares keeps its equivalent independent of retune
    // speed, and with it on the device matches Auto-Tune audibly.
    static const Preset kPresets[4] = {
        { 120.0f, 55.0f, 60.0f, 100.0f, true },    // natural
        {  40.0f, 25.0f, 30.0f, 100.0f, true },    // balanced
        {   8.0f,  0.0f,  0.0f,  40.0f, true },    // tuned
        {   0.0f,  0.0f,  0.0f,   0.0f, true },    // hard
    };
    const Preset& p = kPresets[m];

    const juce::ScopedValueSetter<bool> guard (applyingMode_, true);
    correct_.setRetuneMs (p.retune);
    correct_.setFlex (p.flex);
    correct_.setHumanize (p.humanize);
    correct_.setIgnoreVibrato (p.ignoreVib);
    correct_.setNaturalVibrato (p.naturalVib);
    forEachShifter ([&] (auto& e) { e.setSeamRampMs (seamAttack); });
    // DEPTH is written by every mode at the schema default (100): a mode is a
    // character; depth is how much of it is applied. Same one-default rule as
    // seam_attack_ms (round 22).
    { const auto* dp = schema().find (kDepth); correct_.setDepth (dp != nullptr ? (float) dp->def * 0.01f : 1.0f); }

    // Every mode preserves formants: the character is retune speed and how much
    // deviation survives, never whether it still sounds like the singer.
    forEachShifter ([&] (auto& e) { e.setFormantMode (echojay::PsolaEngine::kFormantPreserve); });

    const auto* spec = schema().find (kMode);
    juce::String name = spec != nullptr ? juce::String (spec->choiceLabel (m)) : juce::String (m);

    pendingModeSummary_ = "which set retune_speed_ms " + juce::String (p.retune, 0)
         + ", flex " + juce::String (p.flex, 0)
         + ", humanize " + juce::String (p.humanize, 0)
         + ", natural_vibrato " + juce::String (p.naturalVib, 0)
         + ", targeting_ignores_vibrato " + juce::String (p.ignoreVib ? "on" : "off")
         + ", seam_attack_ms " + juce::String (seamAttack, 0)
         + ", depth 100"
         + ", formant_mode preserve";

    return "correction_mode " + name
         + " (retune_speed_ms " + juce::String (p.retune, 0)
         + ", flex " + juce::String (p.flex, 0)
         + ", humanize " + juce::String (p.humanize, 0)
         + ", natural_vibrato " + juce::String (p.naturalVib, 0)
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
// the key auto-map (spec §6) — why this device belongs in EchoJay
// ---------------------------------------------------------------------------
// The precedence walk that decides WHICH source wins lives once, in
// EchoJayProcessor::collectKeySources(), and is shared with the [DETECTED KEY] feed
// block and the Meters panel. This reads its published result. Re-implementing
// the walk here would give the suite two rankings that can disagree, which is
// the exact bug the shared collector exists to prevent.
void EedPitchProcessor::refreshAutoKey()
{
    const bool keyAuto = keyAuto_.load();
    const bool refAuto = refAuto_.load();
    if (! keyAuto && ! refAuto)
    {
        const juce::ScopedLock sl (autoLock_);
        autoState_.active         = false;
        autoState_.refAuto        = false;
        autoState_.refApplied     = correct_.getReferenceHz();
        autoState_.refSelfIgnored = false;
        return;
    }

    const echojay::DetectedKeyFact f = echojay::KeyFeed::instance().get();

    // THE CONFIDENCE GATE. Below it, fall back to CHROMATIC - never to the
    // last known key. A stale key is applied with total confidence and can
    // force a note that is wrong for the song; chromatic still tunes every
    // note and cannot. Measured: correcting to a wrong key pushed a take from
    // 13 cents off the nearest note to 29, i.e. worse than not correcting.
    const bool usable = f.usable();

    // A fact whose primary came from THIS instance's own channel: the
    // reference guard below has always treated it as unmeasured for tuning.
    // Behind keySelfGuard_ it is unmeasured for KEY ROOT/MODE as well (see
    // debugKeySelfGuard): the corrector must not take its scale from the
    // melody it is correcting. Chromatic, actively applied - never the last
    // key - exactly as the confidence gate does.
    const bool selfFact    = f.selfDerived && f.publisherId != 0
                          && f.publisherId == keyFeedSelfId_.load (std::memory_order_relaxed);
    const bool keyCircular = keySelfGuard_.load() && selfFact;
    const bool keyUsable   = usable && ! keyCircular;

    if (keyAuto)
    {
        const int  root  = keyUsable ? f.root  : 0;
        const bool minor = keyUsable ? f.minor : false;

        if (! keyUsable)
        {
            if (! lastAutoFellBack_)
            {
                correct_.beginScaleCrossfade();
                applyScale (kScaleChromatic);
                lastAutoFellBack_ = true;
                lastAutoRoot_ = -1;
            }
        }
        else if (root != lastAutoRoot_ || minor != lastAutoMinor_ || lastAutoFellBack_)
        {
            // A live key change - a modulation, or a new song under a running
            // instance - cross-fades rather than switching on a sample.
            correct_.beginScaleCrossfade();
            correct_.setKeyRoot (root);
            applyScale (minor ? kScaleMinor : kScaleMajor);
            lastAutoRoot_ = root; lastAutoMinor_ = minor; lastAutoFellBack_ = false;
        }
    }

    // THE CIRCULARITY GUARD (29 Aug 2026 ruling): auto must never derive
    // the grid from the signal being corrected. A fact whose primary came
    // from THIS instance's own channel (selfDerived, same publisher) is a
    // loop closing on its own output - a flat singer defines a flat grid
    // and correction becomes a no-op in the mean. Measured on the standing
    // reference: the installed default corrected Sean's take to its own
    // 439.1 Hz, -3.4c flat of the Antares grid ("never arrives at the
    // note"). Such a fact is treated as UNMEASURED: fall back to 440,
    // never to the channel itself. The fallback is applied actively, not
    // by omission - on a solo instance auto now IS manual-440.
    bool refCircular = false;
    if (refAuto)
    {
        refCircular = selfFact;
        const float wantRef = (usable && ! refCircular && f.tuningHz > 0.0f)
                                ? f.tuningHz : 440.0f;
        if (std::abs (wantRef - lastAutoTuning_) > 0.05f)
        {
            correct_.setReferenceHz (wantRef);
            lastAutoTuning_ = wantRef;
        }
    }

    const juce::ScopedLock sl (autoLock_);
    autoState_.active     = keyAuto;
    autoState_.applied    = keyAuto && keyUsable;
    autoState_.fellBack   = keyAuto && ! keyUsable;
    autoState_.root       = keyUsable ? f.root : 0;
    autoState_.minor      = keyUsable && f.minor;
    autoState_.keySelfIgnored = keyAuto && usable && keyCircular;
    autoState_.conf       = f.confidence;
    autoState_.tuningHz   = f.tuningHz;
    autoState_.sourceName = juce::String (juce::CharPointer_ASCII (f.sourceName));
    autoState_.refAuto        = refAuto;
    autoState_.refApplied     = correct_.getReferenceHz();
    autoState_.refSelfIgnored = refAuto && refCircular;
}

// The laundered-reference migration (29 Aug 2026): before manualRefHz_
// existed, a state save exported the corrector's LIVE reference - under
// auto, the detected value - and the load's reference_hz write flipped the
// mode to manual. Anyone who saved a session with auto reference got a
// detected grid promoted to a "manual" setting with no control to change
// it. A genuine pre-fix manual reference (chat-set) is indistinguishable
// from a laundered one in saved state - both are just {manual, value} - so
// per the ruling both revert to auto; the readout shows it and the new REF
// control restores a wanted manual value in one gesture.
void EedPitchProcessor::onStateApplied()
{
    if (! refAuto_.load() && ! refManualByUser_.load())
    {
        refAuto_.store (true);
        lastAutoTuning_ = 0.0f;   // force the next refresh to re-apply
    }
}

// ONE DEFAULT PER PARAMETER (3 Sep 2026 ruling): a bare-constructed device
// consults the schema for every default, the same call the registry and
// state restore make. seam_attack_ms constructed at the engine's 0 against
// the advertised 60 - both live paths happened to write the schema value
// first, which is luck, not design, and a third path (a preset loader, a
// harness, a refactor) would have run a fix that was not switched on and
// reported it as measured. Any construction path that does not consult
// the schema default is a bug, not an exemption.
EedPitchProcessor::EedPitchProcessor()
{
    resetParamsToDefaults();
}

void EedPitchProcessor::resetParamsToDefaults()
{
    const juce::ScopedValueSetter<bool> guard (writingDefaults_, true);
    EedDeviceProcessor::resetParamsToDefaults();
}

EedPitchProcessor::AutoKeyState EedPitchProcessor::autoKeyState() const
{
    const juce::ScopedLock sl (autoLock_);
    return autoState_;
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
    const int root = correct_.getKeyRoot();
    for (int s = 0; s < 12; ++s)
    {
        const bool en = (kMasks[i] >> s) & 1;
        correct_.setDegree (s, en, correct_.degreeBias (s));
        // The NAME is a pitch class (root + degree), not a degree index —
        // this summary used to print C-rooted names for every root
        // (3 Sep 2026, found in the frame sweep).
        if (en) on.add (kNames[(root + s) % 12]);
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

    forEachShifter ([&] (auto& e) { e.setLowestF0 (PitchEngine::voiceRange (vt).fMinHz); });

    // Correctness, not latency: align the f0 to the audio it describes. Always
    // on - a stale estimate makes note-change detection fire late, which P2's
    // envelope would then act on from the wrong place.
    forEachShifter ([&] (auto& e) { e.setPitchLagSamples (engine_.pitchLagFor (vt)); });
    // TIMING FOUNDATION (5 Sep 2026, TIMING_ALIGNMENT_RECORD): the shifter's
    // per-hop back-dating needs the detector's window geometry for this voice.
    { int W = 0, tauMax = 0, hop = 128; engine_.lagModelFor (vt, W, tauMax, hop);
      forEachShifter ([&] (auto& e) { e.setPerHopLag (true, W, tauMax, hop); }); }

    // Drift bleed ON in the shipped path (5 Sep 2026 ruling): the per-span
    // drift discharge at v/uv boundaries was measured as the field's
    // near-zero-shift period inversions; bleeding it continuously (<=3
    // cents momentary detune, tau 100 ms) cut the constant-shift breaks
    // 36->15 at +5c on source4 and beat the old contract's own ideal floor.
    forEachShifter ([&] (auto& e) { e.setDriftBleed (true); });

    setLatencySamples (shifter().latencySamples());
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

    forEachShifter ([&] (auto& e) { e.prepare (sampleRate, samplesPerBlock,
                    PitchEngine::voiceRange (engine_.getVoiceType()).fMinHz, worst); });

    f0Gate_.reset();

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
        // Target 0 passed per call, so nothing has to be saved and restored.
        for (int ch = 0; ch < numCh; ++ch)
            shifter (ch).process (buffer.getReadPointer (ch), buffer.getWritePointer (ch),
                                  buffer.getNumSamples(), 0.0f, false, 0.0f);
        return;
    }

    refreshLatency();
    refreshAutoKey();

    const int n = buffer.getNumSamples();

    // Detection first: the shifter runs `latencySamples()` behind, so the f0
    // for a span is always known before the shifter reaches it.
    engine_.process (buffer.getReadPointer (0),
                     numCh > 1 ? buffer.getReadPointer (1) : nullptr,
                     n);

    // THE BLOCK IS SLICED AT HOP BOUNDARIES.
    //
    // The musical layer has millisecond time constants and produces the target
    // the shifter aims at. Running it once per BLOCK tied both to the host's
    // buffer size: measured, fixed-512 against random 16..2048 blocks differed
    // by 0.73 peak, and the target stepped once per block - a zipper on any
    // gliding note. Slicing at the detector's own hops runs it at the cadence
    // it was designed for, and makes the output independent of buffer size.
    echojay::PitchEngine::HopEvent hops[64];
    const int numHops = engine_.drainHops (hops, 64);
    const float hopMs = engine_.hopMs();

    const uint64_t blockStart = engine_.inputPosition() - (uint64_t) n;

    float target     = lastTarget_;
    float shift      = lastShift_;
    float sliceF0    = lastHopF0_;
    bool  sliceVoiced = lastHopVoiced_;
    bool  correcting = lastCorrecting_;

    int cursor = 0;
    for (int h = 0; h <= numHops; ++h)
    {
        // The slice runs to this hop, or to the end of the block after the last.
        int sliceEnd = n;
        if (h < numHops)
        {
            const int64_t rel = (int64_t) hops[h].inputPos - (int64_t) blockStart;
            sliceEnd = (int) juce::jlimit ((int64_t) cursor, (int64_t) n, rel);
        }

        if (sliceEnd > cursor)
        {
            const int len = sliceEnd - cursor;
            for (int ch = 0; ch < numCh; ++ch)
                shifter (ch).process (buffer.getReadPointer (ch) + cursor,
                                      buffer.getWritePointer (ch) + cursor, len,
                                      sliceF0, sliceVoiced, target, shift);
            cursor = sliceEnd;
        }

        // At the hop itself, the musical layer decides the next target.
        if (h < numHops)
        {
            // OCTAVE-SCALE EXCURSION REJECTION, before the corrector and the
            // shifter's f0 track see the hop (see F0JumpGate). A brief wrong-
            // octave estimate corrected smoothly is a momentary wrong NOTE -
            // clean in the waveform, invisible to every discontinuity gate,
            // caught only by the pitch ribbon and the excursion metric. On an
            // octave-scale jump the AUDIO is asked which story is true - the
            // input ring is right there in the shifter, and two
            // autocorrelations on a rare event are free.
            float rOldT = -1.0f, rNewT = -1.0f;
            const bool seeding = hops[h].voiced && hops[h].f0Hz > 0.0f
                              && f0Gate_.lastGood() <= 0.0f;
            if (f0Gate_.isBigJump (hops[h].f0Hz, hops[h].voiced) || seeding)
            {
                // On a jump, "old" is the last accepted period; at a SEED it
                // is HALF the candidate's (the sub-octave vetting lag).
                const double refHz = seeding ? 2.0 * (double) hops[h].f0Hz
                                             : (double) f0Gate_.lastGood();
                const int tOld = (int) std::lround (sampleRate_ / refHz);
                const int tNew = (int) std::lround (sampleRate_ / (double) hops[h].f0Hz);
                rOldT = shifter().inputPeriodicity (hops[h].inputPos, tOld);
                rNewT = shifter().inputPeriodicity (hops[h].inputPos, tNew);
            }
            const float gatedF0 = f0Gate_.filter (hops[h].f0Hz, hops[h].voiced, hopMs,
                                                  rOldT, rNewT);

            // The f0 the SHIFTER uses is the hop's own, not the block's last
            // reading - otherwise every slice in a block shares one value and
            // the result depends on where the block boundaries fell.
            sliceF0     = gatedF0;
            sliceVoiced = hops[h].voiced;

            if (correctOn_.load())
            {
                correcting = true;
                const float t = correct_.process (gatedF0, hops[h].voiced, hopMs);

                // THE CLICKING LIVED HERE. The corrector returns 0 on every
                // untracked hop ("hold everything"), and passing that 0 to
                // the shifter as its target flips PsolaEngine::process into
                // the emitDry PASSTHROUGH branch - an instantaneous, fadeless
                // switch from grain-summed wet to raw delayed dry. At normal
                // tracking ~13% of voiced frames are gated, so the output
                // flipped wet->dry->wet several times a second, a hard step
                // each way. Measured (tools/pitch_click_test): clicks were
                // 13x enriched at emit-state flips, the top clicks were all
                // vertical steps AT the flip, and holding the target dropped
                // the flip coincidence to exactly the base rate and killed
                // the fixed-offset-after-hop signature outright, 3.47/s ->
                // 2.43/s overall (the residual is wet-path roughness on
                // sharp-glottal passages, bounded by the permanent density
                // gate). HOLD the last target instead: the f0
                // ring already marks the gap unvoiced, and emitMixed then
                // emits those samples as bit-exact dry THROUGH the seam fade,
                // which exists for exactly this join. Same audible content,
                // faded instead of stepped - and unvoiced stays sacred.
                if (t > 0.0f)         { target = t;
                                          shift = correct_.shiftPreferred()
                                              ? correct_.lastShiftCents()
                                              : echojay::PsolaEngine::kNoShift; }
                else if (target <= 0.0f) correcting = false;   // nothing to hold yet
                // else: hold the last target AND SHIFT through the gap —
                // holding a shift is gentler than holding an absolute
                // target across a gap the voice moved through.

                // Retune trace: one record per hop while correcting (the
                // editor drains; unread records are simply overwritten).
                if (hops[h].voiced && gatedF0 > 0.0f)
                {
                    const uint32_t w = traceW_.load (std::memory_order_relaxed);
                    auto& r = trace_[w % (uint32_t) kTraceCap];
                    r.tSec  = (double) hops[h].inputPos / sampleRate_;
                    r.f0Hz  = gatedF0;
                    r.inC   = correct_.lastInCents();
                    r.slowC = correct_.lastSlowCents();
                    r.oscC  = correct_.lastOscCents();
                    r.aimC  = correct_.lastAimCents();
                    r.envC  = t > 0.0f ? correct_.lastShiftCents()
                                       : 0.0f;   // the APPLIED shift (3 Sep)
                    traceW_.store (w + 1, std::memory_order_release);
                }
            }
            else
            {
                // Not correcting: the fixed-target diagnostic still applies
                // (legacy target/f0 semantics — no shift).
                target = shifter().getTargetHz();
                shift  = echojay::PsolaEngine::kNoShift;
                correcting = false;
            }
        }
    }

    // A block with no hop in it still has to be emitted.
    if (cursor < n)
        for (int ch = 0; ch < numCh; ++ch)
            shifter (ch).process (buffer.getReadPointer (ch) + cursor,
                                  buffer.getWritePointer (ch) + cursor, n - cursor,
                                  sliceF0, sliceVoiced, target, shift);

    lastTarget_ = target; lastShift_ = shift; lastHopF0_ = sliceF0;
    lastHopVoiced_ = sliceVoiced; lastCorrecting_ = correcting;

    // Feed the ribbon here rather than from the editor's timer, so the trace
    // is the audio thread's ACTUAL decisions rather than a resampled guess at
    // them - and so it keeps its shape whether or not an editor is open.
    //
    // TWO FEED BUGS LIVED HERE, found by validating the ribbon against the
    // audio (PITCH_P0_VALIDATION.md §16.9) after a user report of tall
    // spikes sent two rounds of debugging at a defect the audio never had:
    //
    // 1. The dim trace carried the RAW engine reading - pre-F0JumpGate - so
    //    after the excursion-rejection work it displayed precisely the
    //    estimates the audio rejects. Measured: 33 of 34 full-height ribbon
    //    verticals on the acapella moved the corrected trace by under 2 st;
    //    the picture spiked, the audio did not. The feed now carries the
    //    GATED value (lastHopF0_ - the same number the shifter and the
    //    corrector obey). Rejected estimates stay visible NUMERICALLY in the
    //    octave-guard counter; a fainter raw spiking line would still read
    //    as an artefact to anyone looking at it.
    //
    // 2. Pushed once per processBlock (~93 Hz at 512), against a view
    //    designed for ~30 Hz columns - the ribbon spanned ~1.3 s where spec
    //    §7 says about 4. Pushes are now decimated to the column cadence,
    //    independent of host block size.
    ribbonAccum_ += n;
    const int colSamples = juce::jmax (1, (int) (sampleRate_ / 30.0));
    while (ribbonAccum_ >= colSamples)
    {
        ribbonAccum_ -= colSamples;
        ribbon_.push (lastHopVoiced_,
                      lastHopVoiced_ && lastHopF0_ > 0.0f
                          ? echojay::PitchEngine::hzToMidi (lastHopF0_) : 0.0f,
                      target > 0.0f ? echojay::PitchEngine::hzToMidi (target) : 0.0f,
                      correcting && lastHopVoiced_);
    }
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
