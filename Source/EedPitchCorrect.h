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

// ---------------------------------------------------------------------------
// CENTS FRAMES (3 Sep 2026, the D-minor rotation). A bare float carried an
// implicit reference frame: inCents was zeroed at reference_hz (the tuning
// A) while nearestIn subtracted a C-frame root index — two quantities nine
// semitones apart that assigned and compared without complaint, rotating
// every requested root to (r+9)%12. D minor became the D-major pitch-class
// set, F natural was pushed to E/F#, and a unit test written in the same
// frame as the bug agreed with it. The frame is now part of the NAME:
// cents enter the corrector through centsCFromHz (C-zeroed — the frame the
// root indices and scale masks always lived in), leave through
// hzFromCentsC, and the quantiser accepts only the CentsC wrapper, so the
// compiler refuses an unconverted axis. Frame-INVARIANT quantities
// (differences): osc, the note-change deltas, wanted, vibNow_, transpose.
// Frame-CARRYING (all C-frame now): inCents, slowCents_, curCents_,
// noteRefCents_, pendingCents_, targetCents_, selectCents.
// ---------------------------------------------------------------------------
struct CentsC { float v; };   // 0 == pitch class C in the reference tuning
static constexpr float kCentsAtoC = 900.0f;    // A440 axis -> C axis
inline float centsCFromHz (float hz, float refA) noexcept
{ return 1200.0f * std::log2 (hz / refA) + kCentsAtoC; }
inline float hzFromCentsC (float cC, float refA) noexcept
{ return refA * std::pow (2.0f, (cC - kCentsAtoC) / 1200.0f); }

// ---------------------------------------------------------------------------
// F0JumpGate — rejects octave-scale detector excursions BEFORE anything acts
// on them (PITCH_P0_VALIDATION.md §16.8).
//
// The audible defect this exists for is a momentary WRONG PITCH, not a click:
// the octave guard's residual failures (339 fires logged on one phrase) put a
// brief octave/sub-harmonic estimate into the chain, the corrector snaps a
// target to it and the splice-resampler's ratio inherits it, and the output
// lands smoothly - grain-accurately - on the wrong note. Perfectly clean in
// the waveform, invisible to every discontinuity gate, measured at 3.5% of
// voiced frames departing >600 cents from the dry against Antares's 0.44%,
// mostly downward (the estimate doubling drops the splice ratio an octave).
//
// The rule extends the note-change confirm window (PitchCorrect's
// kNoteConfirmMs) to LARGE jumps at the f0 level: a jump of more than
// kBigJumpCents from the last accepted estimate must PERSIST for kConfirmMs
// before it is believed; until then the last accepted f0 is substituted, and
// a jump that reverts inside the window never happened. Genuine octave leaps
// in a sung phrase are rare and can afford the ~50 ms onset delay; spurious
// ones are frequent and cost an audible glitch each.
//
// This sits UPSTREAM of both consumers on purpose: filtering only the
// corrector's target would leave the shifter's f0 ring spiking, and the
// splice ratio target/f0 would then drop the output an octave with the
// target held perfectly still.
struct F0JumpGate
{
    static constexpr float kBigJumpCents  = 600.0f;   // octave-scale, incl. the x1.5 lattice confusion (702c)
    static constexpr float kSameCandCents = 200.0f;   // hops agreeing with the pending candidate
    static constexpr float kConfirmMs     = 50.0f;    // the user-stated 40-60 ms window

    // The confirm window applies WITHIN a voiced run, not across gap resumes.
    // Measured with a 200 ms forget (PitchCorrect's gap-resume window): the
    // gate INJECTED excursions on the rap acapella - 16 against 8 without it
    // - because that delivery changes register across consonant gaps
    // constantly, and holding the old octave for 50 ms after such a resume is
    // itself a 50 ms wrong note. 30 ms sits above the 11 ms median mid-note
    // tracking dropout (§5.3) and below any real consonant gap, so mid-phrase
    // spikes are still gated and register changes across gaps are believed
    // immediately.
    static constexpr float kGapForgetMs   = 30.0f;

    // Periodicity threshold for the audio question (see filter()).
    static constexpr float kStillPeriodic = 0.60f;

    // Would this hop trigger the octave-scale confirm path? Callers use it
    // to decide whether to spend the two autocorrelations.
    bool isBigJump (float f0Hz, bool voiced) const noexcept
    {
        return voiced && f0Hz > 0.0f && lastGood_ > 0.0f
            && std::fabs (1200.0f * std::log2 (f0Hz / lastGood_)) > kBigJumpCents;
    }
    float lastGood() const noexcept { return lastGood_; }

    // rOldPeriod / rNewPeriod: the input's normalised autocorrelation at the
    // last-accepted and the candidate period (PsolaEngine::inputPeriodicity),
    // or -1 when the caller cannot ask the audio. THE AUDIO SETTLES IT when
    // it can (measured on the rap acapella, §16.8): blind persistence-holding
    // INJECTED excursions - 16 against 8 - because that take's vocal fry
    // produces true subharmonics, and holding the old octave through a real
    // period-doubling is itself a 50 ms wrong note. A spurious flip leaves
    // the waveform periodic at the OLD lag; a real drop collapses it there;
    // an upward move is believed when the NEW (shorter) lag correlates.
    // Persistence remains the backstop for the inconclusive cases.
    float filter (float f0Hz, bool voiced, float dtMs,
                  float rOldPeriod = -1.0f, float rNewPeriod = -1.0f) noexcept
    {
        if (! voiced || f0Hz <= 0.0f)
        {
            gapMs_ += dtMs;
            if (gapMs_ >= kGapForgetMs) { lastGood_ = 0.0f; havePending_ = false; }
            return f0Hz;
        }
        gapMs_ = 0.0f;

        if (lastGood_ <= 0.0f)
        {
            // SEED VETTING. A span's first estimate can be a SUB-OCTAVE read
            // of a creaky onset (measured: 79-87 Hz for 24 ms where the true
            // pitch is 175, then a +1200c "jump" when the detector rights
            // itself - and synthesis had already chased the wrong octave).
            // At the seed, rOldPeriod carries the correlation at HALF the
            // candidate period: if the audio is strongly periodic there too,
            // the candidate is plausibly the sub-octave of a higher true
            // pitch - present the hop as untracked (the output stays dry, as
            // it would under tracker warm-up) until the candidate persists
            // kConfirmMs or corrects. A clean onset has a LOW half-period
            // correlation and seeds immediately, so it costs nothing.
            if (rOldPeriod >= kStillPeriodic)
            {
                if (! havePending_
                    || std::fabs (1200.0f * std::log2 (f0Hz / pending_)) > kSameCandCents)
                { havePending_ = true; pending_ = f0Hz; pendingMs_ = 0.0f; }
                pendingMs_ += dtMs;
                if (pendingMs_ < kConfirmMs)
                { ++rejectedHops_; return 0.0f; }
            }
            lastGood_ = f0Hz; havePending_ = false; return f0Hz;
        }

        const float jump = std::fabs (1200.0f * std::log2 (f0Hz / lastGood_));
        if (jump <= kBigJumpCents)
        { lastGood_ = f0Hz; havePending_ = false; return f0Hz; }

        // Ask the audio first.
        if (rOldPeriod >= 0.0f && rNewPeriod >= 0.0f)
        {
            const bool up = f0Hz > lastGood_;
            const bool audioMoved = up ? rNewPeriod >= kStillPeriodic
                                       : rOldPeriod <  kStillPeriodic;
            if (audioMoved)
            {
                lastGood_ = f0Hz; havePending_ = false;
                ++confirmed_;
                return f0Hz;
            }
        }

        // Inconclusive or unaskable: confirm by persistence.
        if (! havePending_
            || std::fabs (1200.0f * std::log2 (f0Hz / pending_)) > kSameCandCents)
        { havePending_ = true; pending_ = f0Hz; pendingMs_ = 0.0f; }

        pendingMs_ += dtMs;
        if (pendingMs_ >= kConfirmMs)
        {
            lastGood_ = f0Hz; havePending_ = false;
            ++confirmed_;
            return f0Hz;
        }
        ++rejectedHops_;
        return lastGood_;
    }

    void reset() noexcept
    { lastGood_ = 0.0f; pending_ = 0.0f; pendingMs_ = 0.0f; gapMs_ = 0.0f;
      havePending_ = false; }

    uint32_t rejectedHops() const noexcept { return rejectedHops_; }
    uint32_t confirmedJumps() const noexcept { return confirmed_; }

private:
    float lastGood_ = 0.0f, pending_ = 0.0f, pendingMs_ = 0.0f, gapMs_ = 0.0f;
    bool  havePending_ = false;
    uint32_t rejectedHops_ = 0, confirmed_ = 0;
};

class PitchCorrect
{
public:
    PitchCorrect() = default;

    // ---- advertised ranges (single source of truth for the ParamSchema) ----
    // THE CAP (2 Sep 2026 ruling, PITCH_P0_VALIDATION.md §17.4/§17.7):
    // the dial ends at 150ms, where §17.4's own verdict says useful
    // correction ends (roughness minimum at 40, within-3c cliff past
    // ~150, all-scoop by 400). Derived bracket: >=120 (the natural
    // preset), <=200 (the cliff); 150 taken with the curve attached.
    // RECORDED HONESTLY: the cap retires the CHARACTER region and the
    // literal tau-400 complaint point - NOT the boundary-snap class,
    // which lives on the legacy path at any tau above the floor (146c
    // applied disc at tau-120) and is still handled by the env5
    // candidate's four-gate path.
    static constexpr float kMinRetuneMs   = 0.0f,   kMaxRetuneMs   = 150.0f;
    // THE RETUNE FLOOR (30 Aug 2026 ruling, PITCH_P0_VALIDATION.md
    // Â§17.3): the effective tau never goes below 6 ms, though the dial
    // still reads 0. The 0-6 ms zone is STRICTLY DOMINATED, measured:
    // converged accuracy identical (synthetic centres 1.92-2.05c across
    // 0-6), acquisition identical (settle ~107 ms, floored by note-change
    // detection - the pole buys no speed), and roughness WORSE at 0 (84 vs
    // 74 rough spans on sourceNEW, 29 vs 26 on the hard-match take: tau 0
    // chases hop-level detection jitter verbatim). Antares's own "retune
    // 0" carries internal smoothing equivalent to ~4-6 ms of this tau -
    // Sean's blind match and the roughness curves agree - so the floor
    // also makes 0 mean what users arriving from other correctors expect.
    // The DIAL value stays as entered (round-trip honest); the floor
    // applies at the single use site, and retuneEffectiveMs() exists so
    // the UI can SHOW the mapping instead of hiding it.
    static constexpr float kRetuneFloorMs = 6.0f;
    static constexpr float kDefRetuneMs   = 6.0f;     // PROVISIONAL DEFAULT (5 Sep 2026, UI_SIMPLIFICATION round 40): what measures best AND what Sean's ear chose (retune 6 beat 44 'on both'); was 44 (round 36, chosen for where he worked), before that 120 (natural, which detuned the reference take). Hesitation on file: dial 0 is the hardest setting and Antares ships at 20; re-derive on raw material
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
    static constexpr float kNoteConfirmMs = 15.0f;   // RE-DERIVED 5 Sep 2026 with co-timing on (TIMING_ALIGNMENT_RECORD round 30): shortest window with a clean OLD-take falsifier; was 25 on the skewed clock

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
        vibPhase_ = 0.0f; vibNow_ = 0.0f; noteMs_ = 0.0f;
        pendingCents_ = 0.0f; havePending_ = false;
        noteChanges_ = 0; gapResumes_ = 0;
        // Round 48 (DEFECT_PRESS_PLAY_PHASING): everything else that carries
        // the previous position's history - the applied-shift pole and its
        // snap latch (shiftSm_/shiftCents_/shiftSnap_), the resume and seed
        // medians and their judgement state, the measured vibrato depth and
        // the pending-note median buffer, the slow track's age, and the
        // last-hop readbacks the trace and the shifter's fast term consult.
        // prepare() calls this, so a prepared instance is unchanged. NOT here,
        // deliberately: every parameter, the key/scale/degrees and the scale
        // cross-fade (round-20's question, its own ruling), the depth, and
        // resumeReanchors_ (a statistic).
        shiftSm_ = 0.0f; shiftCents_ = 0.0f; shiftSnap_ = true;
        resumeJudge_ = 0; resumeBuf_[0] = resumeBuf_[1] = resumeBuf_[2] = 0.0f;
        lastCorridorExcess_ = 0.0f; lastResumeOffset_ = 0.0f;
        seedHops_ = 0; seedBuf_[0] = seedBuf_[1] = seedBuf_[2] = 0.0f;
        depthEnv_ = 0.0f; pendDepth_ = 0.0f; pendN_ = 0;
        pendBuf_[0] = pendBuf_[1] = pendBuf_[2] = 0.0f;
        slowAgeMs_ = 0.0f;
        lastInCents_ = 0.0f; lastSlowCents_ = 0.0f; lastOscCents_ = 0.0f; lastAimCents_ = 0.0f;
        resumeProvMs_ = 0.0f; resumeN_ = 0;   // round 59: the C2 window does not survive a reset
    }

    // ---- parameters --------------------------------------------------------
    void setRetuneMs (float v) noexcept   { retuneMs_.store (std::clamp (v, kMinRetuneMs, kMaxRetuneMs)); }
    float getRetuneMs() const noexcept    { return retuneMs_.load(); }
    float retuneEffectiveMs() const noexcept
    { return std::max (retuneMs_.load(), kRetuneFloorMs); }
    void setFlex (float pct) noexcept     { flex_.store (std::clamp (pct, 0.0f, 100.0f)); }
    float getFlex() const noexcept        { return flex_.load(); }
    void setHumanize (float pct) noexcept { humanize_.store (std::clamp (pct, 0.0f, 100.0f)); }
    float getHumanize() const noexcept    { return humanize_.load(); }
    void setReferenceHz (float hz) noexcept { referenceHz_.store (std::clamp (hz, kMinReferenceHz, kMaxReferenceHz)); }
    float getReferenceHz() const noexcept { return referenceHz_.load(); }
    void setTranspose (float st) noexcept { transpose_.store (std::clamp (st, kMinTranspose, kMaxTranspose)); }
    float getTranspose() const noexcept   { return transpose_.load(); }
    void setIgnoreVibrato (bool b) noexcept { ignoreVibrato_.store (b); }
    /** DEPTH (5 Sep 2026, SLOW_END_RECORD round 31): how much of the correction
        is APPLIED, 0..1, blended AFTER the envelope on the applied shift -
        target = in + depth * (env - in). 1 = today's full correction (bit-
        identical); 0 = exact identity (proven under co-timing). This is the
        control the slow end of the dial pretends to be: Antares' slow end is
        transparent because it never commits, not because it is slow. Depth on
        the AIM (pre-envelope) was measured and rejected - it keeps the motion
        and drops the correction (round 27). */
    void setDepth (float d) noexcept { depth_.store (std::clamp (d, 0.0f, 1.0f)); }
    float getDepth() const noexcept  { return depth_.load(); }
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
            // PENDING FORGET (1 Sep 2026; §17.6 corollary, one level
            // deeper): a note-change pending is EVIDENCE, and evidence
            // lapsed unvoiced past ~30ms is stale - two agreeing creak
            // mis-reads 53ms apart accumulated into one pending and handed
            // the release a "consistent" bad destination (avg(148.2,
            // 148.2) - the 5.2s residual after the median fix). The
            // F0JumpGate learned this identical lesson (kGapForgetMs);
            // the pending now forgets on the same clock.
            if (pendForgetOn_ && gapMs_ >= kPendForgetMs)
            { havePending_ = false; pendN_ = 0; confirmMs_ = 0.0f; }
            return 0.0f;
        }

        const float inCents = centsCFromHz (f0Hz, ref);   // C FRAME from here on

        // A resume inside the window continues the note in progress; a longer
        // gap has already cleared haveNote_ above.
        const bool resuming = gapMs_ > 0.0f && haveNote_;
        if (resuming) ++gapResumes_;
        // ROUND 59, C2 (behind noteDecExp_ bit 2): A GAP RESUME RE-DECIDES THE
        // TARGET FROM THE FRESH AUDIO for one confirm window. The 200 ms rule
        // keeps the note (haveNote_) and the envelope position; the SELECTION
        // is provisional (= the input) for kNoteConfirmMs, and at the end of
        // the window the median of the fresh samples decides: a different
        // degree from the reference is a note change (reference and slow seed
        // := that degree under C1), the same degree just re-seeds the slow
        // track from the degree (not from one phase-dependent sample).
        if ((noteDecExp_ & 2) && resuming) { resumeProvMs_ = dbgConfirmMs_; resumeN_ = 0; }
        // THE SHARED ANCHOR at gap resumes (1 Sep 2026 ruling; §17.6, the
        // re-anchor rule): the resume keeps the TARGET (the 200ms rule -
        // same note) but the envelope's POSITION is pre-gap state applied
        // to post-gap audio - the ferry that held -123c off-grid for
        // 230ms at tau 400 (an unconverged limbo resumed verbatim). Arm
        // the same median-of-first-hops anchor used at note starts: a
        // note start and a gap resume differ in whether the target is
        // re-decided, not in how the position is established.
        if (medianSeed_ >= 2 && resuming)
        {
            // THE CORRIDOR, judged with ROBUST evidence (17.6 as amended,
            // completed 1 Sep 2026): the first corridor draft tested the
            // carried position against ONE fresh sample - the least
            // trustworthy reading in the signal - and fired on 33-41% of
            // resumes (median excess 34-72c, tail to 1269c: octave
            // mis-reads re-anchoring GOOD state to BAD samples). Judge
            // with the same median the anchor would use: arm a 3-hop
            // window, keep carried state through it (it was fine 11ms
            // ago), decide at the median.
            resumeJudge_ = 1; resumeBuf_[0] = inCents;
        }
        else if (resumeJudge_ > 0 && resumeJudge_ < 3)
        {
            resumeBuf_[resumeJudge_++] = inCents;
            if (resumeJudge_ >= 3)
            {
                float a = resumeBuf_[0], b = resumeBuf_[1], c2 = resumeBuf_[2];
                const float med = std::max (std::min (a, b),
                                            std::min (std::max (a, b), c2));
                // Endpoint from the SLOW selection, not the fresh median:
                // with ignore-vib the aim's degree is deg(slowCents_) - a
                // carried but INPUT-derived quantity, unpoisoned by the
                // envelope ferry and stable across blinks by design.
                // deg(med) fired on legitimate boundary-straddling vibrato
                // (64-74% of resumes); deg(slow) is where the envelope is
                // ENTITLED to sit.
                const float deg = nearestDegreeCents (CentsC { slowCents_ });
                const float lo = std::min (med, deg) - kCorridorSlackC;
                const float hi = std::max (med, deg) + kCorridorSlackC;
                lastCorridorExcess_ = curCents_ < lo ? curCents_ - lo
                                    : curCents_ > hi ? curCents_ - hi : 0.0f;
                lastResumeOffset_ = std::fabs (curCents_ - med);
                if (curCents_ < lo || curCents_ > hi)
                {
                    ++resumeReanchors_;
                    // CUR ONLY: the ferry is an ENVELOPE-POSITION defect.
                    // Re-anchoring noteRefCents_ too was measured spawning
                    // third-note wrong votes (+82 "other" in the vibvote
                    // split) - the note reference is target-side state the
                    // 200ms same-note rule legitimately carries.
                    curCents_ = med; shiftSnap_ = true;
                }
                resumeJudge_ = 0;
            }
        }
        gapMs_ = 0.0f;

        // ---- slow-smoothed f0, for TARGET SELECTION only ------------------
        if (! haveSlow_ || ! haveNote_)
        {
            // Experiment (c): seed the slow track from the NOTE, not from
            // one mid-swing audio sample - unbiased by vibrato phase by
            // construction, inheriting only the note decision the corrector
            // targets anyway. (30 Aug 2026 three-way; nothing shipped.)
            slowCents_ = (seedExp_ == 3 || (noteDecExp_ & 1)) ? nearestDegreeCents (CentsC { inCents })
                                                             : inCents;   // C1: the seed is the degree
            haveSlow_ = true;
            slowAgeMs_ = 0.0f;
            depthEnv_ = 0.0f;
        }
        else
        {
            // Experiment (a): onset-adaptive smoothing - the pole starts at
            // 30 ms and relaxes to kVibratoSmoothMs over the first 300 ms.
            const float smoothMs = seedExp_ == 1
                ? 30.0f + (kVibratoSmoothMs - 30.0f)
                        * std::min (1.0f, slowAgeMs_ / 300.0f)
                : kVibratoSmoothMs;
            const float a = onePole (smoothMs);
            slowCents_ = inCents + (slowCents_ - inCents) * a;
            slowAgeMs_ += stepMs_;
        }

        // ---- note-change detection ----------------------------------------
        if (! haveNote_)
        {
            haveNote_ = true;
            curCents_ = inCents;               // start AT the note, not behind it
            shiftSnap_ = true;
            // ROUND 59, C1 (behind noteDecExp_ bit 1): THE REFERENCE IS THE NOTE,
            // NOT THE SAMPLE - the decided degree, so the 90c re-arm is measured
            // from a note and a phase-dependent start sample never becomes it.
            noteRefCents_ = (noteDecExp_ & 1) ? nearestDegreeCents (CentsC { inCents }) : inCents;
            stableMs_ = 0.0f; confirmMs_ = 0.0f; havePending_ = false;
            noteMs_ = 0.0f; vibPhase_ = 0.0f;
            seedHops_ = 1; seedBuf_[0] = inCents;
        }
        else if (medianSeed_ && seedHops_ > 0 && seedHops_ < 3)
        {
            // MEDIAN-OF-FIRST-HOPS SEED (1 Sep 2026 ruling, defect family
            // instance five): the single-sample "start AT the note" seed is
            // taken at the moment the estimate is worst - a creak onset
            // mis-read (148 Hz for a 170 Hz note at alto_tenor) poisons
            // curCents_ AND noteRefCents_, and long tau preserves the
            // poison for hundreds of ms (the 5.20-5.43s -123c hold; at
            // small tau the same poison heals in ~20ms, invisible). For
            // the first 3 voiced hops (~8ms - inaudible), re-seed both
            // from the RUNNING MEDIAN, the same don't-trust-one-sample
            // discipline the slow-track work established.
            seedBuf_[seedHops_++] = inCents;
            float a = seedBuf_[0], b = seedBuf_[1], c = seedHops_ > 2 ? seedBuf_[2] : b;
            const float med = seedHops_ > 2
                ? std::max (std::min (a, b), std::min (std::max (a, b), c))
                : 0.5f * (a + b);
            curCents_ = med; shiftSnap_ = true;
            noteRefCents_ = med;
            if (seedHops_ >= 3) seedHops_ = 0;
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
            if (std::fabs (inCents - from) > noteChangeC_)
            {
                if (! havePending_ || std::fabs (inCents - pendingCents_) > noteChangeC_)
                {
                    // Latch the SUSTAIN's depth at pending formation:
                    // depthEnv_ here is last hop's value - the note's wobble
                    // BEFORE the candidate excursion began, which is the
                    // quantity the depth gate must judge (measured: the
                    // envelope DURING a pending reads ~74c of transition
                    // excursion, not vibrato - gating on it never fires).
                    if (! havePending_) pendDepth_ = depthEnv_;
                    pendingCents_ = inCents; havePending_ = true; confirmMs_ = 0.0f;
                    pendN_ = 1; pendBuf_[0] = inCents;
                }
                else
                {
                    if (pendN_ > 0 && pendN_ < 3) pendBuf_[pendN_++] = inCents;
                    confirmMs_ += stepMs_;
                    if (confirmMs_ >= dbgConfirmMs_)
                    {
                        // A genuine note change: restart AT the new note so the
                        // interval does not become a portamento.
                        curCents_ = inCents;
                        shiftSnap_ = true;
                        slowCents_ = (seedExp_ == 3 || (noteDecExp_ & 1))
                            ? nearestDegreeCents (CentsC { inCents }) : inCents;   // C1
                        slowAgeMs_ = 0.0f;
                        depthEnv_ = 0.0f;
                        noteRefCents_ = (noteDecExp_ & 1) ? nearestDegreeCents (CentsC { inCents }) : inCents;   // C1
                        stableMs_ = 0.0f;
                        noteMs_ = 0.0f; vibPhase_ = 0.0f;
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
        noteMs_ += stepMs_;
        // C2: the resume window runs here; when it closes, the fresh median decides.
        if (resumeProvMs_ > 0.0f)
        {
            resumeWin_[resumeN_ % 3] = inCents; ++resumeN_;
            resumeProvMs_ -= stepMs_;
            if (resumeProvMs_ <= 0.0f)
            {
                const int n = std::min (resumeN_, 3);
                float a = resumeWin_[0], b = n > 1 ? resumeWin_[1] : a, c2 = n > 2 ? resumeWin_[2] : b;
                const float med = n > 2 ? std::max (std::min (a, b), std::min (std::max (a, b), c2)) : (n > 1 ? 0.5f * (a + b) : a);
                const float deg = nearestDegreeCents (CentsC { med });
                if (std::fabs (deg - nearestDegreeCents (CentsC { noteRefCents_ })) > 0.5f)
                {
                    // the resume landed on another note: a note change, decided from fresh audio
                    noteRefCents_ = (noteDecExp_ & 1) ? deg : med;
                    slowCents_ = (noteDecExp_ & 1) ? deg : med;
                    slowAgeMs_ = 0.0f; depthEnv_ = 0.0f;
                    curCents_ = inCents; shiftSnap_ = true;
                    stableMs_ = 0.0f; noteMs_ = 0.0f; vibPhase_ = 0.0f;
                    havePending_ = false; confirmMs_ = 0.0f;
                    ++noteChanges_;
                }
                else if (noteDecExp_ & 1)
                {
                    // same note: re-seed the slow track from the DEGREE, never from one sample
                    slowCents_ = deg; slowAgeMs_ = 0.0f;
                }
                resumeProvMs_ = 0.0f;
            }
        }

        // ---- 1..2: nearest enabled degree, plus its bias -------------------
        // (d) PROVISIONAL SELECTION ON PENDING EVIDENCE (30 Aug 2026
        // ruling, SHIPPED): while the note-change detector holds a pending
        // candidate - a sustained jump past kNoteChangeCents, the same
        // evidence that already gates envelope resets - selection leans
        // into the pending note by reading inCents instead of the lagging
        // slow track. Measured before this: with ignore-vib on, 82% of
        // wrong-semitone votes were THE PREVIOUS NOTE, concentrated in the
        // first 100ms after a transition (17.4% vs 1.5% in the next bin) -
        // the slow track kept voting the old note through detector lag +
        // confirm while vib-off (and Antares) flipped at the crossing.
        // Chatter safety: a vibrato swing that does not sustain never
        // forms a pending, so mid-note anti-chatter is untouched; both
        // sources vote the same discrete note everywhere except inside
        // the confirm window, so the hand-off is inaudible where it is
        // not doing work.
        // (Experiment (b), retained for the record: provisional for the
        // slow track's first 90ms - measured a dead end, 11.7 -> 10.7%.)
        const bool slowProvisional = (seedExp_ != 4 && havePending_)
                                  || (seedExp_ == 2 && slowAgeMs_ < 90.0f)
                                  || resumeProvMs_ > 0.0f;   // C2
        // (seedExp_ 4 = the pre-(d) shipped behaviour, for measured A/Bs.)
        const float selectCents = (ignoreVibrato_.load() && ! slowProvisional)
                                    ? slowCents_ : inCents;
        const float degreeCents = nearestDegreeCents (CentsC { selectCents });
        lastSelectCents_ = selectCents; lastDegreeCents_ = degreeCents; lastSlowProvisional_ = slowProvisional;   // round 56 instrumentation

        // The NOTE and the WOBBLE are separated here, and everything below
        // acts on the note. slowCents_ is where the note is; osc is the
        // singer's oscillation around it.
        //
        // This matters for natural_vibrato: correction applied to the LIVE
        // pitch removes the wobble along with the error, so a control that
        // then scales "whatever survived" comes out symmetric about 100 rather
        // than monotonic - measured, 0% and 200% both produced 27 cents of
        // swing where 100% produced none. Correcting the NOTE and adding the
        // wobble back at the chosen amount is what makes 0 / 100 / 200 mean
        // what they say. It also makes flex judge whether the NOTE is off
        // rather than whether the vibrato is, which is the more defensible
        // reading of it.
        const float osc = haveSlow_ ? (inCents - slowCents_) : 0.0f;
        // MEASURED vibrato depth: one-pole envelope of |osc|, tau 150ms,
        // RESET at both slow-track reset sites - measured 31 Aug 2026:
        // without the reset the envelope reads 36-39c at onsets (the
        // pre-confirm interval excursion captured as "depth" - the
        // stale-per-note-state bug, fourth instance, caught in design);
        // with it, onsets read 0.9c vs sustains 12.1c, the >10x
        // separation the depth gate stands on.
        if (! havePending_)   // never ingest a candidate excursion as "depth"
            depthEnv_ = std::fabs (osc) + (depthEnv_ - std::fabs (osc)) * onePole (150.0f);
        const float noteCents = haveSlow_ ? slowCents_ : inCents;

        // ---- 3: flex -------------------------------------------------------
        // Below the flex threshold correction scales toward zero, so small
        // expressive deviation survives; beyond it, full correction. At flex 0
        // everything is corrected, at 100 only gross errors are.
        float wanted = degreeCents - noteCents;
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
        // DEPTH-BLEND EXPERIMENT (5 Sep 2026, SLOW_END_RECORD round 23 item 1,
        // INVESTIGATION ONLY, debug-gated, 1.0 = bit-identical): scales the
        // correction's DEPTH independently of its speed, to measure whether a
        // transparent slow end is a depth question rather than a tau question.
        if (dbgDepthMode_ == 1) wanted *= dbgDepth_;   // mode 1: depth on the AIM (pre-envelope)

        // ---- the envelope --------------------------------------------------
        // The corrected NOTE, plus however much of the singer's own vibrato is
        // wanted on top of it: 100 keeps it as sung, 0 gives a dead-still note,
        // 200 exaggerates.
        // STRUCTURAL (3 Sep 2026 ruling): the fast component is EXCLUDED
        // from the error rather than subtracted and re-added across the
        // shifter's latency. The envelope glides the NOTE alone; the
        // emitted quantity is a SLOW SHIFT (curCents_ - slowCents_), and
        // the shifter applies it as a ratio against its own time-aligned
        // f0 — so at natural_vibrato 100 the audio keeps its own vibrato
        // by algebraic cancellation, not by a delay-line's belief about
        // latency. The old path re-added osc(now) onto audio emitted
        // 37-55 ms later: at ~6 Hz that is ~a third of a cycle of phase
        // error, measured as MORE wobble than the source and 12 extra
        // wrong-note events. k != 1 (dead-still 0 / exaggerate 200) still
        // carries (k-1)*osc — fast, with the old phase caveat — because
        // scaling the singer's own vibrato has no slow formulation.
        const float aimCents = noteCents + wanted;
        // ENVELOPE AMBIGUITY EXPERIMENTS (31 Aug 2026, the note-boundary
        // snap defect): through detector lag + confirm the shipped envelope
        // CHASES the new aim from the old note's position - the output is
        // pulled toward the OLD note by up to the interval at every onset,
        // then the confirmation snap cancels the remaining travel in one
        // hop (measured: 4.8c at tau 6, 173c at tau 400). Same principle
        // as (d): don't act on state that belongs to the previous note
        // while you don't yet know which note you're on.
        //   envExp_ 1 = FREEZE: hold the envelope through the window; the
        //               confirm step is the old correction's magnitude.
        //   envExp_ 2 = RELEASE: ease the applied shift toward zero
        //               (~10ms) through the window; the step is spread,
        //               at the cost of briefly unwinding a good correction.
        //   envExp_ 3 = RELEASE + SLOW-TRACK CONJUNCTION: suspend only when
        //               the pending is corroborated by the slow track having
        //               departed the note anchor (> 30c). The discriminator
        //               a false pending lacks: ignore-vib's slow track does
        //               not move with fast wobble BY DESIGN (leak "a few
        //               cents" per its own comment), while a real semitone
        //               step moves it >= ~35c by mid-confirm-window - so
        //               natural's vibrato-raised pendings fail the
        //               conjunction and real note changes pass it.
        const bool pendingCorroborated = havePending_
            && std::fabs (slowCents_ - noteRefCents_) > 20.0f;
        //   envExp_ 4 = (ii) DEPTH-GATED (31 Aug 2026 ruling): suspend only
        //               when the pending is corroborated AND measured
        //               vibrato depth is shallow (< 8c) - deep preserved
        //               vibrato is where the false alarms originate AND
        //               where suspension damage is audible; onsets carry no
        //               developed vibrato, so the tau-400 fix survives.
        //   envExp_ 5 = APPLIED-SHIFT GATE (31 Aug 2026 ruling): suspend
        //               only when the shift actually REACHING THE AUDIO is
        //               large - the quantity the confirmation snap would
        //               cancel. Routed per path: the shift-preferred slow
        //               path (natVib~100) absorbs boundary discontinuities
        //               structurally (measured applied disc 3.7c at
        //               natural-120 and 8.3c even at natural-400, under
        //               146-172c TARGET discs), while the legacy target/f0
        //               path transmits them whole (181c at hard-400). The
        //               causal quantity, no fitted crossover; stays correct
        //               if flex/humanize scaling or the dial's range ever
        //               changes, and degrades gracefully: suspension
        //               engages only where the applied step is real.
        const float appliedNow = shiftPreferred() ? shiftCents_
                                                  : (curCents_ - inCents);
        const bool suspend = (envExp_ == 1 || envExp_ == 2) ? havePending_
                           : (envExp_ == 3) ? pendingCorroborated
                           : (envExp_ == 4) ? (pendingCorroborated && pendDepth_ < 8.0f)
                           : (envExp_ == 5) ? (havePending_ && std::fabs (appliedNow) > 30.0f)
                           : false;
        if (suspend)
        {
            // MEDIAN DESTINATION (1 Sep 2026 ruling; §17.6 corollary): the
            // release's old destination was inCents - the single sample
            // that raised the pending, often the very mis-read (a 148.2Hz
            // creak onset dragged a healthy 172.2Hz envelope to 161.5 in
            // ONE hop; tau400 stranded it 300ms - the 5.2s regression).
            // The destination now comes from the pending's OWN samples via
            // the shared 3-hop discipline: hop 1 NO release (a one-sample
            // spurious pending reverts before any release occurs - the
            // damage class vanishes structurally), hop 2 average, hop 3+
            // median. A real note change releases 5ms later: negligible
            // against the 25ms confirm.
            if ((envExp_ == 2 || envExp_ == 3 || envExp_ == 5) && pendN_ >= 2)
            {
                const float dest = pendN_ == 2
                    ? 0.5f * (pendBuf_[0] + pendBuf_[1])
                    : std::max (std::min (pendBuf_[0], pendBuf_[1]),
                                std::min (std::max (pendBuf_[0], pendBuf_[1]),
                                          pendBuf_[2]));
                curCents_ = dest + (curCents_ - dest) * onePole (10.0f);
            }
            // envExp_ 1: curCents_ held as-is.
        }
        else
        {
            const float coeff = onePole (std::max (retuneMs_.load(), kRetuneFloorMs));
            curCents_ = aimCents + (curCents_ - aimCents) * coeff;
        }
        {
            // The vibrato smoother's stopband leaks a few cents of ripple
            // into (curCents_ - noteCents); a slow pole on the SLOW PART
            // kills it (smoothing a slow quantity — no latency belief
            // involved), snapped at note changes so corrections never
            // glide across notes. The DELIBERATE fast term (k-1)*osc is
            // added after the pole: dead-still/exaggerate are meant to be
            // fast, and damping them would blunt their own semantics.
            const float slowPart = curCents_ - noteCents;
            if (shiftSnap_) { shiftSm_ = slowPart; shiftSnap_ = false; }
            else            shiftSm_ = slowPart
                                     + (shiftSm_ - slowPart) * onePole (50.0f);
            // RING-ALIGNED FAST TERM experiment (2 Sep 2026 ruling): under
            // the flag the corrector emits the SLOW shift only; the fast
            // component (k-1)*osc - the hop-sampled, 37-55ms-late term
            // that measured 11 clicks and 13.1c at k=0 - is applied by the
            // ENGINE per-sample from its own ring, where it is
            // phase-correct by construction. Descends from the k=100
            // structural solution (algebraic cancellation at the ring's
            // time), generalised to k != 100.
            const float depthSh = dbgDepthMode_ == 2 ? dbgDepth_ : depth_.load();
            shiftCents_ = (depthSh < 1.0f ? depthSh * shiftSm_ : shiftSm_)
                        + (dbgFastRing_ ? 0.0f
                           : (natVib_.load() * 0.01f - 1.0f) * osc);
        }
        // Per-hop introspection for the retune trace (3 Sep 2026): plain
        // stores on the audio thread, read by the processor's trace ring.
        lastInCents_  = inCents;
        lastSlowCents_ = noteCents;
        lastOscCents_ = osc;
        lastAimCents_ = aimCents;

        // ---- ADDED vibrato, after correction (spec §3) ---------------------
        vibNow_ = 0.0f;
        const float depth = vibDepth_.load();
        if (depth > 0.0f)
        {
            vibPhase_ += 6.283185307179586f * vibRate_.load() * stepMs_ * 0.001f;
            while (vibPhase_ > 6.283185307179586f) vibPhase_ -= 6.283185307179586f;

            float w = 0.0f;
            switch (vibShape_.load())
            {
                case kVibTriangle:
                    w = 2.0f * std::abs (2.0f * (vibPhase_ / 6.283185307179586f) - 1.0f) - 1.0f;
                    break;
                case kVibRamp:
                    w = 2.0f * (vibPhase_ / 6.283185307179586f) - 1.0f;
                    break;
                default:
                    w = std::sin (vibPhase_);
                    break;
            }

            // Onset delay, measured from the START OF THE NOTE, so it fades in
            // the way a singer does rather than restarting on every hop.
            const float onset = vibOnset_.load();
            float fade = 1.0f;
            if (onset > 0.0f) fade = std::clamp (noteMs_ / onset, 0.0f, 1.0f);

            vibNow_ = depth * w * fade;
        }

        // ---- 5: transpose --------------------------------------------------
        // mode 2: depth on the APPLIED SHIFT (post-envelope) - the output pitch
        // blends between the live input pitch and the envelope's position.
        const float depthNow = dbgDepthMode_ == 2 ? dbgDepth_ : depth_.load();
        const float envC = depthNow < 1.0f ? inCents + depthNow * (curCents_ - inCents) : curCents_;
        targetCents_ = envC + vibNow_ + 100.0f * transpose_.load();
        shiftCents_ += vibNow_ + 100.0f * transpose_.load();

        return hzFromCentsC (targetCents_, ref);
    }

    // ---- P5: vibrato ------------------------------------------------------
    // ADDED vibrato is applied AFTER correction (spec §3): correcting a note
    // and then adding vibrato is coherent, whereas adding it first just gives
    // the corrector something to fight.
    //
    // NATURAL vibrato is the singer's own, and it is not a generator at all -
    // it is how much of the deviation the correction is allowed to remove. 100
    // keeps it, 0 removes it, above 100 exaggerates it, by scaling the part of
    // the pitch that oscillates around the note rather than the note itself.
    static constexpr float kMinVibRateHz = 0.1f,  kMaxVibRateHz = 10.0f;
    static constexpr float kMaxVibDepthCents = 100.0f;
    static constexpr float kMaxVibOnsetMs = 3000.0f;

    enum VibShape { kVibSine = 0, kVibTriangle, kVibRamp, kNumVibShapes };

    void setVibDepthCents (float c) noexcept { vibDepth_.store (std::clamp (c, 0.0f, kMaxVibDepthCents)); }
    float getVibDepthCents() const noexcept  { return vibDepth_.load(); }
    void setVibRateHz (float hz) noexcept    { vibRate_.store (std::clamp (hz, kMinVibRateHz, kMaxVibRateHz)); }
    float getVibRateHz() const noexcept      { return vibRate_.load(); }
    void setVibShape (int sh) noexcept       { vibShape_.store (std::clamp (sh, 0, (int) kNumVibShapes - 1)); }
    int  getVibShape() const noexcept        { return vibShape_.load(); }
    void setVibOnsetMs (float ms) noexcept   { vibOnset_.store (std::clamp (ms, 0.0f, kMaxVibOnsetMs)); }
    float getVibOnsetMs() const noexcept     { return vibOnset_.load(); }
    void setNaturalVibrato (float pct) noexcept { natVib_.store (std::clamp (pct, 0.0f, 200.0f)); }
    float getNaturalVibrato() const noexcept    { return natVib_.load(); }

    // How much the ADDED vibrato is currently displacing the note, in cents -
    // published so the viz can draw the corrected trace including it.
    float vibratoCents() const noexcept { return vibNow_; }

    // ---- introspection (readout and tests) --------------------------------
    uint32_t noteChanges() const noexcept { return noteChanges_; }
    uint32_t gapResumes()  const noexcept { return gapResumes_; }
    float    lastTargetCents() const noexcept { return targetCents_; }
    /** The SLOW applied correction in cents (see the structural note in
        process): what the shifter should apply as a ratio against its own
        aligned f0. Valid whenever process() returned a target > 0. */
    float    lastShiftCents() const noexcept { return shiftCents_; }
    /** TRUE when the structural slow-shift path applies: natural_vibrato at
        (effectively) 100, where the shift is osc-free by construction. At
        any other setting the fast term (k-1)*osc has NO slow formulation —
        measured +7 clicks on the chromatic gate when it rode the shift
        path — so those settings keep the legacy target/f0 semantics with
        their documented phase caveat. Both shipping presets that preserve
        vibrato (natural, balanced) are k=100 and take the slow path. */
    bool     shiftPreferred() const noexcept
    { return dbgForceShiftPath_ || std::fabs (natVib_.load() - 100.0f) < 0.5f; }
    // Round 56 instrumentation (DEFECT_WRONG_NOTE_AT_TRANSITIONS): every state
    // the note decision reads, as of the last process() call. Read-only.
    struct DebugSnap
    {
        float inC, noteRefC, slowC, slowAgeMs, selectC, degreeC, curC, targetC, shiftC, aimC;
        bool  haveNote, havePending, slowProvisional, ignoreVib;
        float pendingC, confirmMs, gapMs, stableMs, depthEnv;
        int   pendN; uint32_t noteChanges;
    };
    DebugSnap debugSnap() const noexcept
    {
        return { lastInCents_, noteRefCents_, slowCents_, slowAgeMs_, lastSelectCents_, lastDegreeCents_, curCents_, targetCents_, shiftCents_, lastAimCents_,
                 haveNote_, havePending_, lastSlowProvisional_, ignoreVibrato_.load(),
                 pendingCents_, confirmMs_, gapMs_, stableMs_, depthEnv_, pendN_, noteChanges_ };
    }
    // Path-unification investigation (2 Sep 2026, Q1): force the shift
    // path at any natural_vibrato, to reproduce and dissect the recorded
    // "+7 clicks at k != 100" finding. Investigation-only.
    void debugForceShiftPath (bool on) noexcept { dbgForceShiftPath_ = on; }
    void debugFastRing (bool on) noexcept { dbgFastRing_ = on; }
    float    lastInCents()   const noexcept { return lastInCents_; }
    float    lastSlowCents() const noexcept { return lastSlowCents_; }
    float    lastOscCents()  const noexcept { return lastOscCents_; }
    float    lastAimCents()  const noexcept { return lastAimCents_; }
    bool     inNote() const noexcept { return haveNote_; }
    // Seed-experiment selector + raw slow-track access (30 Aug 2026
    // three-way measurement; 0 = shipped, 1/2/3 = candidates a/b/c).
    void  debugSeedExperiment (int e) noexcept { seedExp_ = e; }
    // Pending telemetry (31 Aug 2026 false-pending measurement round).
    bool  debugPendingNow() const noexcept { return havePending_; }
    float debugNoteRef() const noexcept { return noteRefCents_; }
    float debugDepthEnv() const noexcept { return depthEnv_; }
    float debugPendDepth() const noexcept { return pendDepth_; }
    void  debugEnvExperiment (int e) noexcept { envExp_ = e; }
    // ROUND 59: the note-decision fix behind an investigation flag (bit 1 = C1
    // the reference is the decided degree; bit 2 = C2 a gap resume re-decides
    // for one confirm window) and the note-change threshold as a parameter
    // (C3, re-derived by measurement). 0 / kNoteChangeCents = bit-identical.
    void  debugNoteDecision (int bits) noexcept { noteDecExp_ = bits; }
    void  debugNoteChangeCents (float c) noexcept { noteChangeC_ = c > 0.0f ? c : kNoteChangeCents; }
    void  debugNoteConfirmMs (float ms) noexcept { dbgConfirmMs_ = ms > 0.0f ? ms : kNoteConfirmMs; }   // re-derivation sweep (round 30); default = kNoteConfirmMs, bit-identical
    void  debugDepthScale (float d, int mode = 1) noexcept { dbgDepth_ = std::clamp (d, 0.0f, 1.0f); dbgDepthMode_ = mode; }   // investigation only; depth 1 = off; mode 1 aim, 2 applied shift
    void  debugMedianSeed (int mode) noexcept { medianSeed_ = mode; }   // 0 off, 1 starts only, 2 +resume corridor
    void  debugPendForget (bool on) noexcept { pendForgetOn_ = on; }    // gated: rode ungated into the baseline for one commit - the exact pattern under repair
    uint32_t debugResumeReanchors() const noexcept { return resumeReanchors_; }
    float    debugLastCorridorExcess() const noexcept { return lastCorridorExcess_; }
    float    debugLastResumeOffset() const noexcept { return lastResumeOffset_; }
    float debugSlowTrack() const noexcept { return slowCents_; }
    // The slow track in Hz, for the ring-aligned fast term: ONE slow
    // reference serves both the slow shift and the fast factor - two
    // independent slow tracks measured mistuned by the note step for
    // ~140ms after every change (rig flatness 3.4% vs legacy 22.6%).
    float slowHzNow() const noexcept
    { return haveSlow_ ? hzFromCentsC (slowCents_, referenceHz_.load()) : 0.0f; }

    // Nearest ENABLED degree to a cents value, including that degree's bias.
    // Public so a test can pin the decision independently of the envelope.
    float nearestDegreeCents (CentsC cents) const noexcept
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

    float nearestIn (CentsC centsC, bool previous) const noexcept
    {
        const float cents = centsC.v;
        const int root = keyRoot_.load();

        // Work relative to the root so degree indices are semitones above
        // it — VALID ONLY because both are C-framed, which the CentsC
        // parameter now enforces at compile time.
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
    int   seedExp_ = 0;        // 30 Aug 2026 three-way experiments; 0 = shipped
    // DEFAULT REVERTED TO 0 (1 Sep 2026 ruling): env5's release eases the
    // envelope toward the SINGLE sample that raised the pending - at
    // legacy-path x long-tau x creaky onsets that is a live regression
    // (-123c off-grid holds; the 5.2s event Sean reported, then
    // mis-attributed for four rounds because the investigation baseline
    // contained env5 itself). Its measured benefit (tau400 spans 97->76)
    // was ear-marginal twice; the regression is not, and sits at Sean's
    // working configuration. env5 returns to default only through the
    // four-part acceptance in the ruling, after the median-destination
    // fix.
    // (The original env5 rationale, for the record:)
    // natural bit-identical, hard better on 6/8 columns, tau-400 rough
    // spans 97 -> 76, ear neutral-or-better across two blind rounds.
    // 0 = the pre-fix path, kept for A/Bs; 1-4 = measured dead ends.
    // The 30c gate is DERIVED, not picked: geometric midpoint of the
    // bracket [shielded-path ceiling 8.3c (natural's applied disc, with
    // margin), smallest musical interval 100c] = sqrt(8.3*100) ~ 29c;
    // the 20/30/50 sweep shows a monotone hard-6-vs-hard-400 trade with
    // both natural corners invariant at every value.
    int   envExp_ = 0;
    float dbgDepth_ = 1.0f;      // see debugDepthScale
    float dbgConfirmMs_ = kNoteConfirmMs;   // see debugNoteConfirmMs
    int   noteDecExp_ = 0;                  // round 59: C1 | C2 bits; 0 = shipped (bit-identical)
    float noteChangeC_ = kNoteChangeCents;  // round 59: C3, the threshold under re-derivation
    float resumeProvMs_ = 0.0f; float resumeWin_[3] = { 0, 0, 0 }; int resumeN_ = 0;   // C2 window
    int   dbgDepthMode_ = 1;
    int   medianSeed_ = 0;       // 0 off, 1 starts only, 2 +resume corridor
    uint32_t resumeReanchors_ = 0;
    int   resumeJudge_ = 0;
    float resumeBuf_[3] = { 0, 0, 0 };
    float lastCorridorExcess_ = 0.0f;   // signed 0 if inside; set every resume
    float lastResumeOffset_ = 0.0f;     // |cur - med| at every judgment
    static constexpr float kCorridorSlackC = 15.0f;   // see the resume corridor
    static constexpr float kPendForgetMs = 30.0f;     // pending evidence staleness (see gap hold)
    int   seedHops_ = 0;
    float seedBuf_[3] = { 0, 0, 0 };
    float depthEnv_ = 0.0f;    // measured vibrato depth (see osc block)
    float pendDepth_ = 0.0f;   // depthEnv_ latched at pending formation
    int   pendN_ = 0;          // median-destination sample count (see release)
    bool  dbgForceShiftPath_ = false;
    bool  dbgFastRing_ = false;
    bool  pendForgetOn_ = false;
    float pendBuf_[3] = { 0, 0, 0 };
    float slowAgeMs_ = 0.0f;   // ms since the slow track was (re)seeded
    float lastSelectCents_ = 0.0f, lastDegreeCents_ = 0.0f; bool lastSlowProvisional_ = false;   // round 56 instrumentation
    std::atomic<float> flex_     { 0.0f };
    std::atomic<float> humanize_ { 0.0f };
    std::atomic<float> referenceHz_ { 440.0f };
    std::atomic<float> transpose_   { 0.0f };
    std::atomic<bool>  ignoreVibrato_ { true };
    std::atomic<float> depth_ { 1.0f };
    std::atomic<int>   keyRoot_     { 0 };

    std::array<std::atomic<bool>, 12>  degEnabled_ {};
    std::array<std::atomic<float>, 12> degBias_ {};
    std::array<std::atomic<bool>, 12>  prevEnabled_ {};
    std::array<std::atomic<float>, 12> prevBias_ {};
    std::atomic<float> xfade_ { 1.0f };      // 1 = settled on the current set

    std::atomic<float> vibDepth_ { 0.0f };
    std::atomic<float> vibRate_  { 5.5f };
    std::atomic<int>   vibShape_ { kVibSine };
    std::atomic<float> vibOnset_ { 300.0f };
    std::atomic<float> natVib_   { 100.0f };
    float lastInCents_ = 0, lastSlowCents_ = 0, lastOscCents_ = 0,
          lastAimCents_ = 0;   // trace introspection (audio thread)
    float shiftCents_ = 0.0f;   // the slow applied correction (see process)
    float shiftSm_ = 0.0f;      // its ripple-killing slow pole
    bool  shiftSnap_ = true;    // snap the pole at note changes
    float vibPhase_ = 0.0f, vibNow_ = 0.0f, noteMs_ = 0.0f;

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
