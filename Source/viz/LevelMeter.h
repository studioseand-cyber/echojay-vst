/*
    LevelMeter.h  —  a peak + RMS level bar (VISUALS_PLAN.md, Utility: "Gain:
    LevelMeter I/O (float tap)", and as a strip anywhere else).

    The plainest of the float-tap views and the most reused: two of these, fed
    from an input and an output FloatTap, is the entire visualisation the Gain
    device needs, and one of them is the level strip a bigger view sits above.

    It shows BOTH numbers because they answer different questions. RMS is how
    loud it sounds and is what a gain move is judged on; peak is how close to
    clipping it is and is what stops the move. A meter with only one of them
    makes the other invisible, and which one it hides decides which mistake the
    user makes.

    Peak hold and its decay are lifted from EedGrMeter for the same reason it
    has them: transients move faster than the eye, and without a hold a meter is
    a flicker with no number attached to it.
*/

#pragma once

#include <JuceHeader.h>
#include "VizView.h"

namespace echojay::viz
{

class LevelMeter : public VizView,
                   private juce::Timer
{
public:
    LevelMeter();
    ~LevelMeter() override;

    // dBFS. Anything at or below the floor reads as silence.
    void setLevelDb (float peakDb, float rmsDb);

    // Convenience for a device holding linear FloatTaps.
    void setLevelLinear (float peak, float rms);

    void setFloorDb (float db);          // default -60
    void setVertical (bool v);

    static constexpr float kSilenceDb = -120.0f;

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    void timerCallback() override;

    // ~1 second at 30 Hz, same as the GR meter's.
    static constexpr int kPeakHoldFrames = 30;

    float floorDb_ = -60.0f;
    float peakDb_  = kSilenceDb;
    float rmsDb_   = kSilenceDb;
    float holdDb_  = kSilenceDb;
    int   holdFrames_ = 0;
    bool  vertical_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace echojay::viz
