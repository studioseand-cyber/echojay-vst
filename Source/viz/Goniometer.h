/*
    Goniometer.h  —  the Lissajous vectorscope + correlation bar
    (VISUALS_PLAN.md, Stereo signature visualisation).

    THE ONE VIEW THAT GENUINELY NEEDS A SIGNAL TAP. Width is not a shape you can
    draw from a parameter: 140% on a mono source is still a vertical line, and
    140% on a wide pad is a wall. Only the samples know which, so this is the
    ring-tap path (echojay::viz::ScopeTap) — the same lock-free, deliberately
    racy contract as the EQ's analyzer, read on the editor's timer.

    THE ROTATION IS THE POINT. Plotting L against R directly puts mono on a 45
    degree diagonal, which nobody can read at a glance. The axes are rotated so
    that:

        vertical   = MID  = (L+R)/2   -> mono is a vertical line
        horizontal = SIDE = (L-R)/2   -> width is how far it spreads
        the diagonals are L and R     -> a hard-panned source lies along one

    So "narrow" and "wide" are up-down and left-right, which is the way the
    words are already used, and a phase problem shows as the picture flopping
    over onto the horizontal.

    The correlation bar underneath is the number that goes with the picture:
    +1 fully in phase (mono-safe), 0 uncorrelated (wide), -1 out of phase (the
    fold-down cancels). Stereo Width cannot produce -1 by design — that is what
    "mono-safe" means — so a reading that drifts left is telling you about the
    source, which is exactly when you want to know.
*/

#pragma once

#include "VizView.h"

#include <vector>

namespace echojay::viz
{

class Goniometer : public VizView
{
public:
    Goniometer();

    // One frame of samples straight off a ScopeTap read. Decimates internally,
    // computes the correlation, and repaints only if the picture moved — a
    // silent input does not repaint at all, which is what keeps an idle editor
    // at zero CPU.
    void setSamples (const float* left, const float* right, int numSamples);

    // For a device that already has a correlation number of its own.
    void setCorrelation (float c);

    void setShowCorrelation (bool s);

    float correlation() const noexcept { return corr_; }

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    // The display holds this many points. More is a denser blob, not more
    // information: past a few hundred the scope is solid at any size a rack
    // slot gives it, and the decimation is what keeps a 20 Hz repaint cheap.
    static constexpr int kMaxPoints = 512;

    // (side, mid) pairs, both already normalised to -1..+1.
    std::vector<juce::Point<float>> points_;

    float corr_       = 0.0f;
    float corrShown_  = 0.0f;    // smoothed, so the bar reads rather than jitters
    bool  silent_     = true;
    bool  showCorr_   = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Goniometer)
};

} // namespace echojay::viz
