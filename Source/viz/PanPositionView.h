/*
    PanPositionView.h  —  where a channel currently sits in the stereo field
    (VISUALS_PLAN.md, Modulation: "Auto Pan: show the pan position").

    A short strip: hard left to hard right, centre marked, with a dot per
    channel riding it. The LfoScopeView above it answers "what shape, how fast";
    this answers the question that is actually about PANNING — "where is it right
    now, and how far apart are the two channels" — which a waveform plotted
    against time does not say, because its axis is amplitude by convention and
    the user has to be told to read it as position.

    ANALYTIC, no tap of its own. The positions are computed by the editor from
    the same phase tap and the same LfoCore::shapeAt the scope draws, so the dot
    and the playhead cannot disagree — they are two views of one number. Tapping
    the engine's post-smoothing pan instead would be more literal and LESS
    useful: it would differ from the drawn waveform by the output smoother's
    couple of milliseconds, which is invisible at a 20 Hz repaint and would show
    up only as the dot sitting fractionally off its own curve.
*/

#pragma once

#include "VizView.h"

namespace echojay::viz
{

class PanPositionView : public VizView
{
public:
    PanPositionView();

    // -1 hard left .. 0 centre .. +1 hard right, per channel. A device whose
    // two channels move together passes the same value twice, and the strip
    // then shows one dot rather than two on top of each other.
    void setPositions (float panL, float panR);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    float panL_ = 0.0f, panR_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanPositionView)
};

} // namespace echojay::viz
