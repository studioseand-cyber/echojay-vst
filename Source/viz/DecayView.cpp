/*
    DecayView.cpp  —  see DecayView.h.
*/

#include "DecayView.h"

namespace echojay::viz
{

namespace
{
    constexpr int kResolution   = 140;
    constexpr int kNumEarlyTaps = 7;
}

DecayView::DecayView()
{
    setCaption ("DECAY");
}

void DecayView::setDecay (float decaySeconds, float predelayMs,
                          float dampingPct, float sizePct)
{
    const float d  = juce::jmax (0.05f, decaySeconds);
    const float pd = juce::jmax (0.0f, predelayMs) * 0.001f;
    const float dm = juce::jlimit (0.0f, 1.0f, dampingPct * 0.01f);
    const float sz = juce::jlimit (0.0f, 1.0f, sizePct * 0.01f);

    if (! moved (d, decayS_, 0.01f) && ! moved (pd, predelayS_, 0.001f)
        && ! moved (dm, damping_, 0.005f) && ! moved (sz, size_, 0.005f))
        return;

    decayS_ = d; predelayS_ = pd; damping_ = dm; size_ = sz;
    repaint();
}

void DecayView::setMixPercent (float pct)
{
    const float m = juce::jlimit (0.0f, 1.0f, pct * 0.01f);
    if (! moved (m, mix_, 0.005f)) return;
    mix_ = m;
    repaint();
}

float DecayView::windowSeconds() const noexcept
{
    // Always show the whole tail plus a little air, so RT60 is read off the
    // curve's own shape rather than off an axis that cuts it in half.
    return juce::jmax (0.4f, predelayS_ + decayS_ * 1.15f);
}

void DecayView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a   = dimAlpha();
    const float win = windowSeconds();

    auto xForT = [&] (float t)
    {
        return plot.getX() + plot.getWidth() * juce::jlimit (0.0f, 1.0f, t / win);
    };
    auto yForAmp = [&] (float amp)
    {
        return plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, amp);
    };

    // -60 dB over RT60, which is the definition of RT60.
    auto envelopeAt = [&] (float t, float rt60)
    {
        if (t <= predelayS_) return 0.0f;
        return mix_ * std::pow (10.0f, -3.0f * (t - predelayS_) / juce::jmax (0.05f, rt60));
    };

    // ---- grid --------------------------------------------------------------
    g.setColour (Colours::border.withMultipliedAlpha (a));
    const float tick = win > 3.0f ? 1.0f : 0.5f;
    for (float t = tick; t < win; t += tick)
        g.fillRect (xForT (t), plot.getY(), 0.5f, plot.getHeight());

    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), plot.getBottom() - 0.5f, plot.getWidth(), 0.5f);

    // ---- the late tail -----------------------------------------------------
    juce::Path late;
    late.startNewSubPath (xForT (predelayS_), yForAmp (0.0f));
    for (int i = 0; i < kResolution; ++i)
    {
        const float t = predelayS_ + (win - predelayS_) * (float) i / (float) (kResolution - 1);
        late.lineTo (xForT (t), yForAmp (envelopeAt (t, decayS_)));
    }
    late.lineTo (plot.getRight(), yForAmp (0.0f));
    late.closeSubPath();

    g.setColour (Colours::blue.withAlpha (0.16f * a));
    g.fillPath (late);
    g.setColour (Colours::blue2.withMultipliedAlpha (0.9f * a));
    g.strokePath (late, juce::PathStrokeType (1.5f));

    // ---- the damped (high-frequency) tail ----------------------------------
    // Damping shortens the top end's RT60 specifically. Drawn as its own faster
    // envelope inside the first, so "damped" reads as the highs dying before
    // the body does rather than as the whole reverb getting quieter.
    if (damping_ > 0.02f)
    {
        const float hfRt = decayS_ * (1.0f - 0.75f * damping_);

        juce::Path hf;
        for (int i = 0; i < kResolution; ++i)
        {
            const float t = predelayS_ + (win - predelayS_) * (float) i / (float) (kResolution - 1);
            const juce::Point<float> p { xForT (t), yForAmp (envelopeAt (t, hfRt)) };
            if (i == 0) hf.startNewSubPath (p); else hf.lineTo (p);
        }

        g.setColour (Colours::purple.withMultipliedAlpha (0.65f * a));
        g.strokePath (hf, juce::PathStrokeType (1.0f));
    }

    // ---- early reflections -------------------------------------------------
    // SIZE is what spaces these: a small room's reflections arrive close
    // together, a hall's are spread out. That is the whole reason they are
    // drawn separately from the tail.
    const float firstEarly = predelayS_ + 0.004f + 0.055f * size_;

    for (int k = 0; k < kNumEarlyTaps; ++k)
    {
        // Irrational-ish spacing so the taps do not line up as a comb, which is
        // both what a real room does and what stops the picture looking like a
        // delay's.
        const float t = firstEarly * (1.0f + 0.83f * (float) k + 0.17f * (float) (k * k));
        if (t > win) break;

        const float amp = mix_ * (1.0f - 0.11f * (float) k) * 0.85f
                          * std::pow (10.0f, -3.0f * (t - predelayS_) / juce::jmax (0.05f, decayS_));

        g.setColour (Colours::blue2.withAlpha (0.55f * a));
        g.fillRect (xForT (t) - 0.6f, yForAmp (amp), 1.2f,
                    plot.getBottom() - yForAmp (amp));
    }

    // ---- pre-delay ---------------------------------------------------------
    if (predelayS_ > 0.001f)
    {
        g.setColour (Colours::amber.withMultipliedAlpha (0.30f * a));
        g.fillRect (plot.getX(), plot.getY(), xForT (predelayS_) - plot.getX(),
                    plot.getHeight());
    }

    if (plot.getWidth() > 80.0f)
    {
        g.setColour (Colours::text3.withMultipliedAlpha (0.75f * a));
        g.setFont (uiFont (7.5f, true));
        g.drawText (juce::String (decayS_, 1) + " s", plot.reduced (2.0f, 1.0f),
                    juce::Justification::topRight);
    }
}

} // namespace echojay::viz
