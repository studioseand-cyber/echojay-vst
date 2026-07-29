/*
    EedAutoPanEngine.h  —  LFO-steered stereo placement for "EchoJay Auto Pan".

    JUCE-free, header-only, g++-tested (test/mod_engines_test.cpp).

    The pan law is GainEngine's, included rather than re-derived: constant power,
    normalised so CENTRE IS UNITY. That is what makes depth 0 a bit-identical
    pass-through, and it is the whole reason to share the function instead of
    writing a second one that is 3 dB down in the middle.

    WHAT STEREO PHASE DOES HERE, because it is the one non-obvious choice. Each
    input channel is steered by the LFO at its own phase: the left channel by the
    left tap, the right by the right tap, keeping the gain that channel would get
    at that pan position. So

      * 0 degrees  — both taps agree and this is an ordinary auto-pan: the whole
                     image swings left and right together.
      * 90 degrees — the channels sweep out of step and the image rotates rather
                     than sliding, which is the wide "rotary" version.
      * 180 degrees— the taps mirror, so both channels dip and swell together and
                     it becomes a tremolo. Musically real, and reached by turning
                     one knob rather than by a hidden mode switch.

    The alternative — cross-feeding both inputs through a single pan position —
    sums to mono at depth 0, which fails the "a device at its defaults is
    pass-through" rule that every EchoJay utility keeps.
*/

#pragma once

#include "EedGainEngine.h"    // GainEngine::panGains — the shared pan law
#include "EedLfoCore.h"

#include <algorithm>
#include <cmath>

namespace echojay
{

class AutoPanEngine
{
public:
    AutoPanEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);
        reset();
    }

    void reset() noexcept { lfo_.reset(); }

    LfoCore&       lfo()       noexcept { return lfo_; }
    const LfoCore& lfo() const noexcept { return lfo_; }

    // ---- audio thread ------------------------------------------------------
    // In-place. `right` may be null: a mono signal has no stereo field to be
    // placed in, so it passes through untouched rather than being amplitude-
    // modulated by half a pan law — the same call GainEngine makes for its pan.
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (right == nullptr) return;

        for (int i = 0; i < numSamples; ++i)
        {
            float posL = 0.0f, posR = 0.0f;
            lfo_.nextStereo (posL, posR);        // already -depth..+depth, i.e. a
                                                 // pan position in -1..+1

            float glL = 0.0f, grL = 0.0f;
            float glR = 0.0f, grR = 0.0f;
            GainEngine::panGains (posL, glL, grL);
            GainEngine::panGains (posR, glR, grR);

            left[i]  *= glL;     // left channel's own gain at its own position
            right[i] *= grR;     // right channel's, at its own
        }
    }

private:
    LfoCore lfo_;
    double  sampleRate_ = 44100.0;
};

} // namespace echojay
