/*
    DelayTapsView.cpp  —  see DelayTapsView.h.
*/

#include "DelayTapsView.h"

namespace echojay::viz
{

namespace
{
    // Below this a repeat is inaudible under any real programme material, so
    // the picture stops rather than drawing a hairline forever.
    constexpr float kAudibleFloor = 0.02f;   // ~ -34 dB
}

DelayTapsView::DelayTapsView()
{
    setCaption ("REPEATS");
}

void DelayTapsView::setTaps (float timeMs, float feedbackPct, float mixPct, bool pingPong)
{
    const float t  = juce::jmax (1.0f, timeMs);
    const float fb = juce::jlimit (0.0f, 0.999f, feedbackPct * 0.01f);
    const float mx = juce::jlimit (0.0f, 1.0f, mixPct * 0.01f);

    if (! moved (t, timeMs_, 0.5f) && ! moved (fb, feedback_, 0.002f)
        && ! moved (mx, mix_, 0.002f) && pingPong == pingPong_)
        return;

    timeMs_ = t; feedback_ = fb; mix_ = mx; pingPong_ = pingPong;
    repaint();
}

void DelayTapsView::setWindowMs (float ms)
{
    const float w = ms > 0.0f ? ms : 0.0f;
    if (! moved (w, windowSet_, 0.5f)) return;
    windowSet_ = w;
    repaint();
}

float DelayTapsView::windowMs() const noexcept
{
    if (windowSet_ > 0.0f) return windowSet_;

    // Auto: show about eight repeats' worth, so the axis rescales with the
    // delay time and the FIRST tap never sits in the far corner of the plot.
    return juce::jlimit (100.0f, 8000.0f, timeMs_ * 8.0f);
}

void DelayTapsView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a   = dimAlpha();
    const float win = windowMs();
    const auto  midY = plot.getCentreY();

    auto xForMs = [&] (float ms)
    {
        return plot.getX() + plot.getWidth() * juce::jlimit (0.0f, 1.0f, ms / win);
    };

    // ---- axis --------------------------------------------------------------
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), midY - 0.25f, plot.getWidth(), 0.5f);

    // Second markers, or 100 ms markers for a short window: the axis has to
    // carry a unit or "how long do the repeats last" has no answer on it.
    const float tick = win > 1500.0f ? 500.0f : 100.0f;
    g.setFont (uiFont (7.0f));
    for (float ms = tick; ms < win; ms += tick)
    {
        const float x = xForMs (ms);
        g.setColour (Colours::border.withMultipliedAlpha (a));
        g.fillRect (x, plot.getY(), 0.5f, plot.getHeight());
    }

    if (plot.getWidth() > 80.0f)
    {
        g.setColour (Colours::text3.withMultipliedAlpha (0.7f * a));
        g.drawText (win >= 1000.0f ? juce::String (win / 1000.0f, 1) + " s"
                                   : juce::String ((int) win) + " ms",
                    plot.reduced (2.0f, 1.0f), juce::Justification::bottomRight);
    }

    // ---- the dry hit -------------------------------------------------------
    // Drawn full height at t=0 so every repeat is read as a fraction of it.
    const float half = plot.getHeight() * 0.45f;
    g.setColour (Colours::text2.withMultipliedAlpha (0.55f * a));
    g.fillRect (plot.getX(), midY - half, 1.5f, half * 2.0f);

    // ---- the repeats -------------------------------------------------------
    float amp = mix_;

    for (int k = 1; k <= kMaxTaps; ++k)
    {
        const float ms = timeMs_ * (float) k;
        if (ms > win || amp < kAudibleFloor) break;

        const float x = xForMs (ms);
        const float h = half * juce::jlimit (0.0f, 1.0f, amp);

        // Ping-pong alternates sides; a normal delay draws both, which reads as
        // one centred tap and correctly says "this repeat is not moving".
        const bool leftSide  = ! pingPong_ || (k % 2) == 1;
        const bool rightSide = ! pingPong_ || (k % 2) == 0;

        g.setColour (Colours::blue2.withAlpha (0.85f * a));
        if (leftSide)  g.fillRect (x - 0.75f, midY - h, 1.5f, h);
        g.setColour (Colours::purple.withAlpha (0.85f * a));
        if (rightSide) g.fillRect (x - 0.75f, midY, 1.5f, h);

        amp *= feedback_;
    }

    if (pingPong_ && plot.getWidth() > 80.0f)
    {
        g.setColour (Colours::text3.withMultipliedAlpha (0.75f * a));
        g.setFont (uiFont (7.0f, true));
        g.drawText ("L", plot.reduced (2.0f, 1.0f), juce::Justification::topLeft);
        g.drawText ("R", plot.withTrimmedTop (plot.getHeight() * 0.5f).reduced (2.0f, 1.0f),
                    juce::Justification::bottomLeft);
    }
}

} // namespace echojay::viz
