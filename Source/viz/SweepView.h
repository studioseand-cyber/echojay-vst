/*
    SweepView.h  —  the moving notches of a phaser / the comb of a chorus
    (VISUALS_PLAN.md, Modulation: "Chorus/Phaser add SweepView for the moving
    notches").

    Analytic. A phaser's notches sit at frequencies its own LFO puts them at, so
    the device computes them (it already knows its stage count and centre) and
    hands over the list; this draws the resulting magnitude response on the EQ's
    own log-frequency axis, so a phaser's picture and the EQ's picture are read
    the same way.

    Approximate ON PURPOSE. The exact response of a cascade of allpasses summed
    with its input is not a shape a user reads a number off — what they need to
    see is WHERE the notches are and that they are MOVING, and a Gaussian well
    per notch says that in a fraction of the code an exact evaluation would take.
    Anything drawn here that claims more precision than that would be a lie the
    axis labels make checkable.
*/

#pragma once

#include "VizView.h"

#include <vector>

namespace echojay::viz
{

class SweepView : public VizView
{
public:
    SweepView();

    static constexpr int   kMaxNotches = 16;
    static constexpr float kMinFreq    = 20.0f;
    static constexpr float kMaxFreq    = 20000.0f;

    // The notch frequencies as the device's LFO currently has them, and how
    // deep they cut. Pushed from the editor's timer; repaints only when the
    // frequencies have actually moved.
    void setNotches (const float* freqsHz, int n, float depthDb);

    // The convenience a phaser wants: notches at the odd multiples of a centre
    // frequency, which is where a cascade of N allpass stages puts them.
    void setSweep (float centreHz, int numStages, float depthDb);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    float magnitudeDbAt (float freqHz) const noexcept;

    float freqs_[kMaxNotches] {};
    int   numNotches_ = 0;
    float depthDb_    = -12.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SweepView)
};

} // namespace echojay::viz
