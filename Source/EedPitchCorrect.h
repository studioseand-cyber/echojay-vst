/*
    EedPitchCorrect.h  —  the DECISION stage and RETUNE ENVELOPE behind
    "EchoJay Pitch" (PITCH_CORRECTION_SPEC.md §2.2 and §2.3, build phase P2).

    JUCE-free, like the detector and the shifter, so the whole musical layer
    unit-tests under plain g++ (test/pitch_correct_test.cpp).

    This is the stage that turns "the singer is at 217 Hz" into "aim at 220",
    and it is where a corrector becomes musical rather than mechanical. It
    produces a TARGET f0; it does not touch audio. The shifter does that.

    THE CHAIN, in the spec's order (§2.2):

      1. f0 -> cents relative to reference_hz,
      2. nearest ENABLED scale degree, plus that degree's bias_cents,
      3. FLEX: scale the correction down for small deviations, so expressive
         drift survives and only gross errors are pulled,
      4. HUMANIZE: relax correction on SUSTAINED notes while leaving onsets
         tight - and sustain is judged from how long f0 has been STABLE, never
         from amplitude, because a held note that is fading is still held,
      5. TRANSPOSE.

    Then §2.3's envelope: a one-pole toward the target whose time constant is
    retune_speed_ms, with three refinements that are the whole character:

      NOTE-CHANGE RESET. On a genuine note change the envelope starts AT the
      new note rather than gliding from the old one, or every interval in the
      melody turns into a portamento.

      GAP RESUME IS NOT A NOTE CHANGE. The detector stops tracking through
      consonants and breath - measured median 11 ms, worst 142 ms
      (PITCH_P0_VALIDATION.md §5.3) - and a resume after such a gap looks
      exactly like a re-onset. Treating it as one re-glides from the wrong
      place on every consonant, which reads as a scoop into the back half of
      every word. Gaps shorter than kGapIsNoteChangeMs therefore RESUME the
      note in progress, holding both target and position across the hole.

      TARGETING IGNORES VIBRATO. Target selection runs on a slow-smoothed f0 so
      a wide vibrato cannot flip the target between neighbouring degrees, while
      the correction itself still tracks the fast f0. Without it, vibrato that
      straddles a semitone boundary chatters between two notes.

    Everything is in cents internally. Cents are linear in perception and in
    the maths of every step above; converting to Hz once at the end is what
    keeps the whole stage free of log calls in the hot path.
*/

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace echojay
{

class PitchCorrect
{
public:
    PitchCorrect() = default;

    // ---- advertised ranges (single source of truth for the ParamSchema) ----
    static constexpr float kMinRetuneMs   = 0.0f,   kMaxRetuneMs   = 400.0f;
    static constexpr float kDefRetuneMs   = 120.0f;
    static constexpr float kMinReferenceHz = 380.0f, kMaxReferenceHz = 500.0f;
    static constexpr float kMinTranspose  = -12.0f, kMaxTranspose  = 12.0f;

    // A jump this big, sustained past the confirm window, is a new note rather
    // than a slide within one.
    static constexpr float kNoteChangeCents = 90.0f;

    // Gaps shorter than this RESUME the note in progress. Sized against the
    // measured gap distribution: median 11 ms, p95 59 ms, worst observed
    // 142 ms - so 200 ms clears every real consonant while still being short
    // enough that a genuine new phrase reads as new.
    static constexpr float kGapIsNoteChangeMs = 200.0f;

    // How long a new pitch must hold before it counts as a note change rather
    // than a scoop or an overshoot.
    static constexpr float kNoteConfirmMs = 25.0f;

    // Sustain for humanize: how long f0 must stay inside kStableCents.
    static constexpr float kSustainMs   = 180.0f;
    static constexpr float kStableCents = 60.0f;

    // Slow smoothing used for TARGET SELECTION only.
    static constexpr float kVibratoSmoothMs = 140.0f;

    // A live key change - a modulation, or a new song under a running
    // instance - must not switch on a sample. Spec §6: cross-fade the scale
    // over a few hundred ms, because a hard switch under a sustained note is
    // audible as a step in the correction.
    static constexpr float kScaleXfadeMs = 300.0f;

    struct Degree
    {
        bool  enabled   = true;
        float biasCents = 0.0f;
    };

    // ---- lifecycle ---------------------------------------------------------
    void prepare (double sampleRate, int hopSamples) noexcept
    {
        fs_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        hop_ = std::max (1, hopSamples);
        hopMs_ = 1000.0f * (float) hop_ / (float) fs_;
        reset();
    }

    void reset() noexcept
    {
        haveNote_ = false;
        curCents_ = 0.0f; targetCents_ = 0.0f; noteRefCents_ = 0.0f;
        slowCents_ = 0.0f; haveSlow_ = false;
        gapMs_ = 0.0f; stableMs_ = 0.0f; confirmMs_ = 0.0f;
        pendingCents_ = 0.0f; havePending_ = false;
        noteChanges_ = 0; gapResumes_ = 0;
    }

    // ---- parameters --------------------------------------------------------
    void setRetuneMs (float v) noexcept   { retuneMs_.store (std::clamp (v, kMinRetuneMs, kMaxRetuneMs)); }
    float getRetuneMs() const noexcept    { return retuneMs_.load(); }
    void setFlex (float pct) noexcept     { flex_.store (std::clamp (pct, 0.0f, 100.0f)); }
    float getFlex() const noexcept        { return flex_.load(); }
    void setHumanize (float pct) noexcept { humanize_.store (std::clamp (pct, 0.0f, 100.0f)); }
    float getHumanize() const noexcept    { return humanize_.load(); }
    void setReferenceHz (float hz) noexcept { referenceHz_.store (std::clamp (hz, kMinReferenceHz, kMaxReferenceHz)); }
    float getReferenceHz() const noexcept { return referenceHz_.load(); }
    void setTranspose (float st) noexcept { transpose_.store (std::clamp (st, kMinTranspose, kMaxTranspose)); }
    float getTranspose() const noexcept   { return transpose_.load(); }
    void setIgnoreVibrato (bool b) noexcept { ignoreVibrato_.store (b); }
    bool getIgnoreVibrato() const noexcept  { return ignoreVibrato_.load(); }

    // Scale degrees are indexed 0..11 as semitones above the KEY ROOT.
    void setKeyRoot (int pc) noexcept     { keyRoot_.store (((pc % 12) + 12) % 12); }
    int  getKeyRoot() const noexcept      { return keyRoot_.load(); }

    void setDegree (int semitone, bool enabled, float biasCents) noexcept
    {
        if (semitone < 0 || semitone > 11) return;
        degEnabled_[(size_t) semitone].store (enabled);
        degBias_[(size_t) semitone].store (std::clamp (biasCents, -50.0f, 50.0f));
    }
    bool  degreeEnabled (int s) const noexcept { return degEnabled_[(size_t) (((s % 12) + 12) % 12)].load(); }
    float degreeBias   (int s) const noexcept { return degBias_[(size_t) (((s % 12) + 12) % 12)].load(); }

    void setAllDegrees (bool enabled) noexcept
    {
        for (int i = 0; i < 12; ++i) degEnabled_[(size_t) i].store (enabled);
    }

    // Snapshot the CURRENT degree set as the outgoing one and start a
    // cross-fade. Call this immediately BEFORE writing a new scale; the
    // targets then glide from the old set to the new one instead of stepping.
    void beginScaleCrossfade() noexcept
    {
        for (int i = 0; i < 12; ++i)
        {
            prevEnabled_[(size_t) i].store (degEnabled_[(size_t) i].load());
            prevBias_[(size_t) i].store (degBias_[(size_t) i].load());
        }
        xfade_.store (0.0f);
    }
    bool  scaleCrossfading() const noexcept { return xfade_.load() < 1.0f; }
    float scaleCrossfadeProgress() const noexcept { return xfade_.load(); }

    // ---- the stage ---------------------------------------------------------
    // Call once per detector hop. Returns the f0 the shifter should aim at, or
    // 0 when the frame must be left alone.
    // dtMs is how much time this call represents. It defaults to the hop the
    // engine was prepared with, but a caller that runs at a DIFFERENT cadence -
    // the plugin, which corrects once per audio block, not once per detector
    // hop - must pass its own, or every millisecond constant in here is scaled
    // by the ratio between the two and the character controls stop meaning
    // what they say.
    float process (float f0Hz, bool voiced, float dtMs = -1.0f) noexcept
    {
        stepMs_ = dtMs > 0.0f ? dtMs : hopMs_;
        const float ref = referenceHz_.load();

        // Advance any scale cross-fade FIRST, and unconditionally. A key change
        // most often lands in a gap between phrases, and a fade that only moves
        // on voiced frames would sit frozen through exactly that gap.
        {
            const float x = xfade_.load();
            if (x < 1.0f) xfade_.store (std::min (1.0f, x + stepMs_ / kScaleXfadeMs));
        }

        if (! voiced || f0Hz <= 0.0f)
        {
            // Hold everything. The envelope must not decay, slide or reset
            // through a gap - see kGapIsNoteChangeMs.
            gapMs_ += stepMs_;
            if (gapMs_ >= kGapIsNoteChangeMs) { haveNote_ = false; haveSlow_ = false; }
            return 0.0f;
        }

        const float inCents = 1200.0f * std::log2 (f0Hz / ref);

        // A resume inside the window continues the note in progress; a longer
        // gap has already cleared haveNote_ above.
        const bool resuming = gapMs_ > 0.0f && haveNote_;
        if (resuming) ++gapResumes_;
        gapMs_ = 0.0f;

        // ---- slow-smoothed f0, for TARGET SELECTION only ------------------
        if (! haveSlow_ || ! haveNote_) { slowCents_ = inCents; haveSlow_ = true; }
        else
        {
            const float a = onePole (kVibratoSmoothMs);
            slowCents_ = inCents + (slowCents_ - inCents) * a;
        }

        // ---- note-change detection ----------------------------------------
        if (! haveNote_)
        {
            haveNote_ = true;
            curCents_ = inCents;               // start AT the note, not behind it
            noteRefCents_ = inCents;
            stableMs_ = 0.0f; confirmMs_ = 0.0f; havePending_ = false;
        }
        else
        {
            // A jump has to HOLD to count: a scoop into a note passes through
            // every cent between, and reacting to the first frame of one would
            // reset the envelope mid-slide.
            //
            // The comparison is against the NOTE ANCHOR - the pitch this note
            // was established at - and NOT against the previous frame. An
            // anchor that follows the input every hop only ever sees a jump
            // for a single frame and can never accumulate the confirm window,
            // so the reset would never fire at all.
            const float from = noteRefCents_;
            if (std::fabs (inCents - from) > kNoteChangeCents)
            {
                if (! havePending_ || std::fabs (inCents - pendingCents_) > kNoteChangeCents)
                {
                    pendingCents_ = inCents; havePending_ = true; confirmMs_ = 0.0f;
                }
                else
                {
                    confirmMs_ += stepMs_;
                    if (confirmMs_ >= kNoteConfirmMs)
                    {
                        // A genuine note change: restart AT the new note so the
                        // interval does not become a portamento.
                        curCents_ = inCents;
                        slowCents_ = inCents;
                        noteRefCents_ = inCents;
                        stableMs_ = 0.0f;
                        havePending_ = false;
                        ++noteChanges_;
                    }
                }
            }
            else { havePending_ = false; confirmMs_ = 0.0f; }
        }

        // ---- sustain, judged from STABILITY not amplitude ------------------
        // Measured against the note anchor, so holding a note accumulates and
        // a moving pitch resets. Amplitude never enters into it: a held note
        // that is fading is still held.
        if (std::fabs (inCents - noteRefCents_) <= kStableCents) stableMs_ += stepMs_;
        else                                                     stableMs_ = 0.0f;

        // ---- 1..2: nearest enabled degree, plus its bias -------------------
        const float selectCents = ignoreVibrato_.load() ? slowCents_ : inCents;
        const float degreeCents = nearestDegreeCents (selectCents);

        // ---- 3: flex -------------------------------------------------------
        // Below the flex threshold correction scales toward zero, so small
        // expressive deviation survives; beyond it, full correction. At flex 0
        // everything is corrected, at 100 only gross errors are.
        float wanted = degreeCents - inCents;
        const float flexPct = flex_.load();
        if (flexPct > 0.0f)
        {
            const float threshold = flexPct;          // 0..100 cents of tolerance
            const float mag = std::fabs (wanted);
            const float scale = mag <= threshold ? (mag / std::max (1.0f, threshold))
                                                 : 1.0f;
            wanted *= scale;
        }

        // ---- 4: humanize, on sustained notes only --------------------------
        const float humanPct = humanize_.load();
        if (humanPct > 0.0f && stableMs_ >= kSustainMs)
            wanted *= 1.0f - 0.01f * humanPct;

        // ---- the envelope --------------------------------------------------
        const float aimCents = inCents + wanted;
        const float coeff = onePole (retuneMs_.load());
        curCents_ = aimCents + (curCents_ - aimCents) * coeff;

        // ---- 5: transpose --------------------------------------------------
        targetCents_ = curCents_ + 100.0f * transpose_.load();

        return ref * std::pow (2.0f, targetCents_ / 1200.0f);
    }

    // ---- introspection (readout and tests) --------------------------------
    uint32_t noteChanges() const noexcept { return noteChanges_; }
    uint32_t gapResumes()  const noexcept { return gapResumes_; }
    float    lastTargetCents() const noexcept { return targetCents_; }
    bool     inNote() const noexcept { return haveNote_; }

    // Nearest ENABLED degree to a cents value, including that degree's bias.
    // Public so a test can pin the decision independently of the envelope.
    float nearestDegreeCents (float cents) const noexcept
    {
        const float x = xfade_.load();
        const float now = nearestIn (cents, false);
        if (x >= 1.0f) return now;

        // Blend the TARGETS, not the masks: a half-enabled degree is
        // meaningless, whereas a target that travels from where the old scale
        // put the note to where the new one does is exactly the audible
        // behaviour wanted.
        const float was = nearestIn (cents, true);
        return was + (now - was) * x;
    }

    float nearestIn (float cents, bool previous) const noexcept
    {
        const int root = keyRoot_.load();

        // Work relative to the root so degree indices are semitones above it.
        const float rel = cents - 100.0f * (float) root;
        const float octaves = std::floor (rel / 1200.0f);
        const float within = rel - octaves * 1200.0f;      // 0..1200

        float bestCents = 0.0f, bestDist = 1.0e9f;
        bool any = false;

        // Search this octave and its neighbours: the nearest enabled degree to
        // a note near the octave boundary can live in the next one.
        for (int oct = -1; oct <= 1; ++oct)
            for (int s = 0; s < 12; ++s)
            {
                const bool  en = previous ? prevEnabled_[(size_t) s].load()
                                          : degEnabled_[(size_t) s].load();
                if (! en) continue;
                const float bias = previous ? prevBias_[(size_t) s].load()
                                            : degBias_[(size_t) s].load();
                const float d = 100.0f * (float) s + bias + 1200.0f * (float) oct;
                const float dist = std::fabs (d - within);
                if (dist < bestDist) { bestDist = dist; bestCents = d; any = true; }
            }

        if (! any) return cents;      // every degree disabled: leave it alone
        return bestCents + octaves * 1200.0f + 100.0f * (float) root;
    }

private:
    float onePole (float ms) const noexcept
    {
        if (ms <= 0.0f) return 0.0f;                  // instant: the hard-tuned effect
        return std::exp (-stepMs_ / ms);
    }

    double fs_ = 48000.0;
    int    hop_ = 128;
    float  hopMs_ = 2.67f;
    mutable float stepMs_ = 2.67f;

    std::atomic<float> retuneMs_    { kDefRetuneMs };
    std::atomic<float> flex_        { 55.0f };
    std::atomic<float> humanize_    { 60.0f };
    std::atomic<float> referenceHz_ { 440.0f };
    std::atomic<float> transpose_   { 0.0f };
    std::atomic<bool>  ignoreVibrato_ { true };
    std::atomic<int>   keyRoot_     { 0 };

    std::array<std::atomic<bool>, 12>  degEnabled_ {};
    std::array<std::atomic<float>, 12> degBias_ {};
    std::array<std::atomic<bool>, 12>  prevEnabled_ {};
    std::array<std::atomic<float>, 12> prevBias_ {};
    std::atomic<float> xfade_ { 1.0f };      // 1 = settled on the current set

    bool  haveNote_ = false, haveSlow_ = false, havePending_ = false;
    float curCents_ = 0.0f, targetCents_ = 0.0f, noteRefCents_ = 0.0f;
    float slowCents_ = 0.0f, pendingCents_ = 0.0f;
    float gapMs_ = 0.0f, stableMs_ = 0.0f, confirmMs_ = 0.0f;

    uint32_t noteChanges_ = 0, gapResumes_ = 0;

public:
    // Degrees default to a full chromatic scale, which is the musically safe
    // starting point: it still tunes and it cannot force a note that is wrong
    // for the song.
    void initDegrees() noexcept
    {
        for (int i = 0; i < 12; ++i)
        {
            degEnabled_[(size_t) i].store (true);  degBias_[(size_t) i].store (0.0f);
            prevEnabled_[(size_t) i].store (true); prevBias_[(size_t) i].store (0.0f);
        }
        xfade_.store (1.0f);
    }
};

} // namespace echojay
