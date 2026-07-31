/*
    LfoScopeView.h  —  one cycle of an LFO, with a moving playhead
    (VISUALS_PLAN.md, Modulation signature visualisation).

    Analytic plus ONE float. The waveform is echojay::LfoCore::shapeAt — the
    same static the DSP evaluates, so the drawn shape is the modulator, not a
    drawing of one — and the only live value is the phase, a single float tap
    off the running LFO.

    Why the playhead earns its float: shape and depth are visible on the knobs,
    RATE is not. A 0.2 Hz auto-pan and a 6 Hz tremolo look identical on a static
    waveform, and "is it moving at the speed I asked for" is the question a user
    actually has when they dial one. The dot answers it without a meter.

    The optional second trace is the RIGHT channel's phase offset, which is what
    turns a tremolo into an auto-pan and is otherwise invisible.
*/

#pragma once

#include "VizView.h"
#include "../EedLfoCore.h"

namespace echojay::viz
{

class LfoScopeView : public VizView
{
public:
    LfoScopeView();

    // echojay::LfoCore::Shape values (kSine / kTriangle / kSquare / kSaw).
    void setShape (int shapeIndex);

    // 0..100, the schema's units.
    void setDepthPercent (float pct);

    // 0..1, wrapped. The one float tap.
    void setPhase (float phase01);

    // Degrees. 0 hides the second trace — a mono modulator has nothing to show.
    void setStereoPhaseDeg (float deg);

    // Unipolar modulators (a tremolo's gain) sit 0..1 rather than -1..+1, and
    // drawing one bipolar puts the resting position in the wrong place.
    void setUnipolar (bool u);

    // The waveform the scope draws — LfoCore::shapeAt for every deterministic
    // shape, with one carve-out. RANDOM holds ONE value per cycle and rolls a
    // fresh one each cycle, so no static picture over a single cycle can be
    // the modulator; a flat line would be literal and useless. Instead the
    // plot shows the shape's first kRandomScopeSteps cycles as a staircase —
    // the DSP's own hash values, via the same shapeAt — which is the honest
    // CHARACTER of the shape even though the live device is further along its
    // sequence. Public and static so a companion view (the auto pan's
    // position strip) can ride the identical curve rather than a second
    // opinion of it.
    static constexpr int kRandomScopeSteps = 4;
    static float displayShapeAt (int shapeIndex, float phase01) noexcept;

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    float valueAt (float phase01) const noexcept;

    int   shape_    = echojay::LfoCore::kSine;
    float depth_    = 1.0f;      // 0..1
    float phase_    = 0.0f;
    float stereo_   = 0.0f;      // 0..1 of a cycle
    bool  unipolar_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoScopeView)
};

} // namespace echojay::viz
