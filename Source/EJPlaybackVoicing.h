#pragma once

// =============================================================================
//  PLAYBACK VOICING: a device class as a set of coefficients.
// =============================================================================
//
// A phone speaker, a laptop, a car and a small radio are not four pieces of
// code. Each is one ROW: a high pass, a low pass and one resonance, run through
// filters this project already has. Adding a device is adding a row.
//
// NO NEW FILTER CODE. The two band edges are echojay::Biquad
// (EedDynamicsCore.h:153) and the resonance is echojay::harmonic::PeakBiquad
// (EedHarmonicCore.h:375). Both are RBJ cookbook sections already shipping in
// the built-in devices, so this file adds a table and the wiring between them,
// and nothing that computes a coefficient.
//
// WIRED INTO THE PLAYBACK STAGE, NOT YET TO A CARD. PlaybackSimStage
// (EJPlaybackSim.h) owns one VoicingChain per channel and runs it for the
// PlaybackSim values that map to a voicing. No card selects one yet, so outside
// a test driving the stage directly, nothing here is audible.

#include "EedDynamicsCore.h"   // echojay::Biquad: the high pass and the low pass
#include "EedHarmonicCore.h"   // echojay::harmonic::PeakBiquad: the resonance

#include <array>
#include <cstddef>

namespace echojay
{

/** The device classes a playback voicing can model.

    ITS OWN TYPE, NOT PlaybackSim, AND THAT IS DELIBERATE. Every PlaybackSim
    value needs a case in applyPlaybackSim's switch (EJPlaybackSim.h), and
    pb PIN7 sweeps every value and reddens on one that has no body. Adding these
    voicings to PlaybackSim here, before anything plays them, would redden
    pb PIN7 on purpose or tempt a stub case to quiet it. So PlaybackSim gains
    its values in the commit that wires a voicing into the stage, not this one.

    Device CLASSES, never brands: decision 11 of COMPARE_REFERENCE_PLAN, as
    recorded at the PlaybackSim enum. */
enum class PlaybackVoicing
{
    None = 0,       ///< no voicing; the chain does not run and must stay a no-op
    PhoneSpeaker,
    Laptop,
    CarDashboard,
    KitchenRadio,
    Earbuds,
    TvSoundbar,       ///< a soundbar under a television: stereo, no mono sum
    BluetoothSpeaker, ///< a portable speaker: one box, summed to mono first
    ClubPA,           ///< a club's PA: one system, summed to mono first

    /** SENTINEL, ALWAYS LAST. NEW VALUES GO ABOVE THIS LINE, NEVER BELOW IT.
        The table below is sized by it and the pv pins sweep up to it, so a
        value added below Count would have no row and no pin. */
    Count
};

/** One voicing: the band edges and the resonance.

    CORNER FREQUENCIES IN Hz WITH A Q, because that is how every audio path
    filter in this project is specified: Biquad, PeakBiquad, the EQ's SVF, the
    one poles in EedDelayCore.h and EedHarmonicCore.h, and the meters' K
    weighting. ballisticCoeff (EJBandScheme.h:128) is derived from a time
    constant, but it smooths meter readings once per block, so it is a meter
    ballistic and not a precedent for a filter that touches audio. */
struct VoicingRow
{
    double hpHz,   hpQ;          ///< high pass corner and Q
    double lpHz,   lpQ;          ///< low pass corner and Q
    double peakHz, peakQ, peakDb;///< resonance centre, Q and gain in dB

    /** SUM TO MONO FIRST, THEN FILTER. True for a device that is one speaker
        or one system heard as one source: a portable speaker, a PA. The stage
        folds the pair to mono and THEN runs the voicing, the physical order:
        the device plays the sum, and its response acts on that.

        WHAT THE FLAG CHANGES IS THE SUM, NOT THE ORDER. Both channels run the
        same high pass, low pass and resonance, and all three are linear, so
        filtering the pair and summing afterwards gives the same samples up to
        floating-point rounding. The audible difference is the fold itself: the
        stereo image collapses and anti-phase content cancels, where a stereo
        voicing keeps both. pb PIN12 pins the fold, not the order.

        A DEFAULT, so the rows that do not name it are stereo, which is what
        they were before this flag existed. The Mono tile is not a voicing and
        does not use this: it stays a selection of its own, the fold alone. */
    bool   monoFirst = false;
};

/** THE NUMBERS ARE INFORMED ESTIMATES, NOT MEASUREMENTS.

    Nobody has measured a real device for this table. Each row is a reasoned
    guess at the class: where a small driver stops producing bass, where its
    top end falls away, and the one resonance that gives it its character. They
    are good enough to hear the difference a device class makes to a mix, and
    no more than that. If anyone ever measures a real device, that response
    should replace the row it belongs to, and this comment should say so.

    THE LAST THREE ROWS (19 Sep 2026) ARE CHOSEN THE SAME WAY, and are no more
    than that either:
      TvSoundbar        small drivers in a bar: little below about 90 Hz, a
                        top that holds to about 14 kHz, and the presence lift
                        at 2.5 kHz that soundbars use to carry dialogue.
      BluetoothSpeaker  one box, summed to mono: little below about 110 Hz, a
                        top falling from about 13 kHz, and the bass lift at
                        160 Hz a small speaker's tuning uses to sound bigger.
      ClubPA            one system, summed to mono: subs reaching about 35 Hz,
                        a top held to about 16 kHz, and a 5 dB lift at 55 Hz,
                        where a club system is run hot. It is the system, not
                        the room: no reverb, no reflections, no crowd.
    THE ONLY REAL TEST AVAILABLE IS LISTENING against the actual device or the
    actual place. Until someone has, read every row here as an impression.

    Indexed by PlaybackVoicing. The None row is never read: None does not run
    the filters at all (see VoicingChain::process), so its zeros are a
    placeholder that keeps the index and the enum in step, not a filter.

    A VALUE ADDED TO THE ENUM WITHOUT A ROW gets a row of zeros from aggregate
    initialisation, silently. pv PIN1 is what catches it: 0 Hz is outside
    Biquad's clamp, so a missing row reddens there rather than playing a filter
    nobody specified. */
inline constexpr std::array<VoicingRow, (std::size_t) PlaybackVoicing::Count> kVoicingTable {{
    //                 high pass        low pass           resonance
    //                 Hz      Q        Hz       Q         Hz      Q     dB
    /* None         */ {   0.0, 0.0,       0.0, 0.0,       0.0, 0.0,  0.0 },
    /* PhoneSpeaker */ { 500.0, 0.707,  6000.0, 0.707,  2500.0, 1.0,  4.0 },
    /* Laptop       */ { 200.0, 0.707, 12000.0, 0.707,  3000.0, 1.0,  2.0 },
    /* CarDashboard */ {  60.0, 0.707, 14000.0, 0.707,   100.0, 1.2,  3.0 },
    /* KitchenRadio */ { 250.0, 0.707,  5000.0, 0.707,  1500.0, 1.0,  3.0 },
    /* Earbuds      */ {  40.0, 0.707, 16000.0, 0.707,    60.0, 1.0,  2.0 },
    //                                                                          mono first
    /* TvSoundbar   */ {  90.0, 0.707, 14000.0, 0.707,  2500.0, 0.9,  2.5,  false },
    /* BluetoothSpk */ { 110.0, 0.707, 13000.0, 0.707,   160.0, 1.2,  4.0,  true  },
    /* ClubPA       */ {  35.0, 0.707, 16000.0, 0.707,    55.0, 1.0,  5.0,  true  },
}};

/** Whether voicing v sums the pair to mono before its filters: the row's
    monoFirst, or false for None, Count and anything outside them, so a bad
    cast can never index past the table. */
inline bool voicingSumsToMono (PlaybackVoicing v) noexcept
{
    const int i = (int) v;
    if (i <= (int) PlaybackVoicing::None || i >= (int) PlaybackVoicing::Count) return false;
    return kVoicingTable[(std::size_t) i].monoFirst;
}

/** One channel's voicing: high pass, low pass, resonance, in series.

    PER CHANNEL, because each filter carries its own state; a stereo caller
    owns two of these. REAL-TIME SAFE: no allocation, no locks, no logging. */
class VoicingChain
{
public:
    /** Zeroes the filters' memory and derives coefficients for this rate.

        The state is ZEROED here, on every prepare, because that is what this
        project's audio path filters already do: EqEngine::prepare and
        PeakBiquad::prepare both reset unconditionally. (The level tallies clear
        only on a real rate change, but they hold accumulated measurements, not
        a few samples of filter memory, so they are not the precedent here.) */
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        hp_.reset();
        lp_.reset();
        peak_.prepare (sampleRate_);   // resets its own state
        applyCoefficients();
    }

    /** Selects a voicing. COEFFICIENTS ONLY: the filters' state is KEPT.

        Biquad::setCoeffs "adopt[s] another biquad's coefficients without
        disturbing this one's state", because "a state reset is audible"
        (EedDynamicsCore.h:225). A voicing change is a user switching cards
        while the mix plays, which is exactly the frequent change that comment
        is about. PeakBiquad::setPeak rewrites its coefficients and likewise
        leaves its state alone; it resets only in its own prepare.

        A value outside [None, Count) is REFUSED and the current voicing
        stands, the same rule PlaybackSimSelection applies, so a bad cast can
        never index past the table. */
    void setVoicing (PlaybackVoicing v) noexcept
    {
        const int i = (int) v;
        if (i < (int) PlaybackVoicing::None || i >= (int) PlaybackVoicing::Count)
            return;
        voicing_ = v;
        applyCoefficients();
    }

    PlaybackVoicing voicing() const noexcept { return voicing_; }

    /** Zeroes the sections' memory. The voicing, the rate and the coefficients
        are left exactly as they are. The stage calls this on ACTIVATION, when a
        voicing starts after a block with none; see PlaybackSimStage in
        EJPlaybackSim.h for why then and never between two live voicings. */
    void reset() noexcept
    {
        hp_.reset();
        lp_.reset();
        peak_.reset();
    }

    /** Filters samples in place, or does nothing at all.

        NONE DOES NOT RUN THE FILTERS, and that is the contract rather than an
        optimisation. A "transparent" biquad is not a no-op: 1 * x + 0.0 turns
        -0.0 into +0.0, a NaN poisons the state it feeds, and a non-trivial
        section's rounding moves the last bits. None must leave the buffer bit
        identical, so it never touches it. Pinned by pv PIN5 with memcmp.

        Returns true when the voicing's filters ran, false when they did not,
        with the same meaning applyPlaybackSim gives its return. */
    bool process (float* samples, int numSamples) noexcept
    {
        if (voicing_ == PlaybackVoicing::None) return false;   // THE EARLY-OUT
        if (samples == nullptr) return false;

        for (int i = 0; i < numSamples; ++i)
            samples[i] = peak_.process (lp_.process (hp_.process (samples[i])));
        return true;
    }

    /** Read-only views of the derived sections, for the pv pins. */
    const Biquad&               highPass()  const noexcept { return hp_; }
    const Biquad&               lowPass()   const noexcept { return lp_; }
    const harmonic::PeakBiquad& resonance() const noexcept { return peak_; }

private:
    void applyCoefficients() noexcept
    {
        // None keeps whatever coefficients it had: it never runs them.
        if (voicing_ == PlaybackVoicing::None) return;

        const VoicingRow& r = kVoicingTable[(std::size_t) voicing_];
        hp_.setCoeffs (Biquad::highpass (sampleRate_, r.hpHz, r.hpQ));
        lp_.setCoeffs (Biquad::lowpass  (sampleRate_, r.lpHz, r.lpQ));
        peak_.setPeak ((float) r.peakHz, (float) r.peakQ, (float) r.peakDb);
    }

    double               sampleRate_ = 44100.0;
    PlaybackVoicing      voicing_    = PlaybackVoicing::None;
    Biquad               hp_, lp_;
    harmonic::PeakBiquad peak_;
};

} // namespace echojay
