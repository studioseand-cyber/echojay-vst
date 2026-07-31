/*
    LfoScopeView.cpp  —  see LfoScopeView.h.
*/

#include "LfoScopeView.h"

namespace echojay::viz
{

namespace
{
    constexpr int kResolution = 128;   // points across one cycle
}

LfoScopeView::LfoScopeView()
{
    setCaption ("LFO");
}

void LfoScopeView::setShape (int shapeIndex)
{
    if (shapeIndex == shape_) return;
    shape_ = shapeIndex;
    repaint();
}

void LfoScopeView::setDepthPercent (float pct)
{
    const float d = juce::jlimit (0.0f, 1.0f, pct * 0.01f);
    if (! moved (d, depth_, 0.002f)) return;
    depth_ = d;
    repaint();
}

void LfoScopeView::setPhase (float phase01)
{
    float p = phase01 - std::floor (phase01);
    if (! (p >= 0.0f && p < 1.0f)) p = 0.0f;

    // 1/200th of a cycle is under a pixel on any plot a rack slot holds. A
    // slow LFO polled at 20 Hz would otherwise repaint for sub-pixel motion.
    if (! moved (p, phase_, 0.005f))
    {
        // ...except at the wrap, where the difference is ~1.0 and genuinely a
        // move. Guarded so the playhead does not freeze for one frame a cycle.
        if (std::abs (p - phase_) < 0.9f) return;
    }

    phase_ = p;
    repaint();
}

void LfoScopeView::setStereoPhaseDeg (float deg)
{
    const float s = juce::jlimit (0.0f, 1.0f, deg * (1.0f / 360.0f));
    if (! moved (s, stereo_, 0.002f)) return;
    stereo_ = s;
    repaint();
}

void LfoScopeView::setUnipolar (bool u)
{
    if (u == unipolar_) return;
    unipolar_ = u;
    repaint();
}

float LfoScopeView::displayShapeAt (int shapeIndex, float phase01) noexcept
{
    if (shapeIndex == echojay::LfoCore::kRandom)
    {
        // See the header note: the plot compresses the shape's first few
        // cycles into a staircase, sampled through the SAME shapeAt the DSP
        // reads, so the steps drawn are values the device really plays.
        float p = phase01 - std::floor (phase01);
        if (! (p >= 0.0f && p < 1.0f)) p = 0.0f;
        return echojay::LfoCore::shapeAt (shapeIndex, p * (float) kRandomScopeSteps);
    }

    return echojay::LfoCore::shapeAt (shapeIndex, phase01);
}

float LfoScopeView::valueAt (float phase01) const noexcept
{
    // The DSP's own shape table, so the drawn waveform IS the modulator.
    const float bipolar = displayShapeAt (shape_, phase01);

    if (! unipolar_)
        return bipolar * depth_;

    // A unipolar modulator (a tremolo's gain) rests at UNITY and only ever
    // dips: gain = 1 - depth * (1 - unipolar). Drawing it centred instead would
    // claim the signal spends half its time louder than the input, which is the
    // one thing a tremolo never does. Mapped back onto the -1..+1 plot axis.
    const float gain = 1.0f - depth_ * (1.0f - echojay::LfoCore::toUnipolar (bipolar));
    return gain * 2.0f - 1.0f;
}

void LfoScopeView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a = dimAlpha();
    const float midY = plot.getCentreY();
    const float halfH = plot.getHeight() * 0.45f;

    auto yFor = [&] (float v) { return midY - juce::jlimit (-1.0f, 1.0f, v) * halfH; };
    auto xFor = [&] (float p) { return plot.getX() + plot.getWidth() * p; };

    // ---- graticule ---------------------------------------------------------
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), midY - 0.25f, plot.getWidth(), 0.5f);

    g.setColour (Colours::border.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), yFor (1.0f), plot.getWidth(), 0.5f);
    g.fillRect (plot.getX(), yFor (-1.0f), plot.getWidth(), 0.5f);
    for (int q = 1; q < 4; ++q)
        g.fillRect (xFor (0.25f * (float) q), plot.getY(), 0.5f, plot.getHeight());

    // ---- the trace(s) ------------------------------------------------------
    auto buildTrace = [&] (float phaseOffset)
    {
        juce::Path p;
        for (int i = 0; i < kResolution; ++i)
        {
            const float t = (float) i / (float) (kResolution - 1);
            const juce::Point<float> pt { xFor (t), yFor (valueAt (t + phaseOffset)) };
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        return p;
    };

    // The R trace goes UNDER the L trace and dimmer: it is context for the
    // stereo spread, not a second reading to compare against.
    if (stereo_ > 0.002f)
    {
        g.setColour (Colours::purple.withMultipliedAlpha (0.55f * a));
        g.strokePath (buildTrace (stereo_), juce::PathStrokeType (1.0f));
    }

    const auto trace = buildTrace (0.0f);
    g.setColour (Colours::blue2.withMultipliedAlpha (a));
    g.strokePath (trace, juce::PathStrokeType (1.75f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // ---- the playhead ------------------------------------------------------
    if (isDimmed()) return;

    const float px = xFor (phase_);
    const float py = yFor (valueAt (phase_));

    g.setColour (Colours::amber.withAlpha (0.25f));
    g.fillRect (px - 0.5f, plot.getY(), 1.0f, plot.getHeight());
    g.setColour (Colours::amber.withAlpha (0.20f));
    g.fillEllipse (px - 5.0f, py - 5.0f, 10.0f, 10.0f);
    g.setColour (Colours::amber);
    g.fillEllipse (px - 2.5f, py - 2.5f, 5.0f, 5.0f);

    if (plot.getWidth() > 70.0f)
    {
        g.setColour (Colours::text3.withAlpha (0.8f));
        g.setFont (uiFont (7.5f, true));
        g.drawText (echojay::LfoCore::shapeName (shape_), plot.reduced (2.0f, 1.0f),
                    juce::Justification::topRight);
    }
}

} // namespace echojay::viz
