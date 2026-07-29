/*
    HarmonicBars.cpp  —  see HarmonicBars.h.
*/

#include "HarmonicBars.h"

namespace echojay::viz
{

namespace
{
    // One Goertzel bin: the magnitude of `freq` in `n` samples. Standard second-
    // order recurrence — two multiply-accumulates per sample, no buffers.
    double goertzelMag (const float* x, int n, double sampleRate, double freq) noexcept
    {
        if (n <= 0 || sampleRate <= 0.0 || freq <= 0.0 || freq >= sampleRate * 0.5)
            return 0.0;

        const double w    = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
        const double coef = 2.0 * std::cos (w);

        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double s = (double) x[i] + coef * s1 - s2;
            s2 = s1;
            s1 = s;
        }

        // |X(k)|, normalised by the frame length so the number does not depend
        // on how much of the ring the caller happened to hand over.
        const double mag2 = s1 * s1 + s2 * s2 - coef * s1 * s2;
        return std::sqrt (std::max (0.0, mag2)) * (2.0 / (double) n);
    }
}

HarmonicBars::HarmonicBars()
{
    setCaption ("HARMONICS");

    for (auto& m : magsDb_) m = floorDb_;
}

void HarmonicBars::setNumHarmonics (int n)
{
    const int v = juce::jlimit (2, kMaxHarmonics, n);
    if (v == numHarmonics_) return;
    numHarmonics_ = v;
    repaint();
}

void HarmonicBars::setFloorDb (float db)
{
    const float f = juce::jlimit (-90.0f, -12.0f, db);
    if (! moved (f, floorDb_, 0.5f)) return;
    floorDb_ = f;
    repaint();
}

void HarmonicBars::setMagnitudesDb (const float* db, int n)
{
    if (db == nullptr || n <= 0) return;

    bool changed = ! haveData_;
    const int count = juce::jmin (n, numHarmonics_);

    for (int i = 0; i < count; ++i)
    {
        const float v = juce::jlimit (floorDb_, 24.0f, db[i]);
        if (moved (v, magsDb_[i], 0.3f)) changed = true;
        magsDb_[i] = v;
    }

    haveData_ = true;
    if (changed) repaint();
}

void HarmonicBars::analyse (const float* mono, int numSamples,
                            double sampleRate, double fundamentalHz)
{
    if (mono == nullptr || numSamples <= 0 || fundamentalHz <= 0.0) return;

    float rel[kMaxHarmonics];

    const double fundamental = goertzelMag (mono, numSamples, sampleRate, fundamentalHz);

    // No fundamental means no reference, and a ratio against noise is noise.
    // Report the floor rather than a made-up shape.
    if (fundamental < 1.0e-5)
    {
        for (int i = 0; i < numHarmonics_; ++i) rel[i] = floorDb_;
        setMagnitudesDb (rel, numHarmonics_);
        return;
    }

    rel[0] = 0.0f;
    for (int h = 1; h < numHarmonics_; ++h)
    {
        const double m = goertzelMag (mono, numSamples, sampleRate,
                                      fundamentalHz * (double) (h + 1));
        rel[h] = m > 1.0e-9 ? (float) (20.0 * std::log10 (m / fundamental)) : floorDb_;
    }

    setMagnitudesDb (rel, numHarmonics_);
}

void HarmonicBars::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a    = dimAlpha();
    const float span = -floorDb_;
    if (span <= 0.0f) return;

    // ---- grid --------------------------------------------------------------
    g.setColour (Colours::border.withMultipliedAlpha (a));
    for (float db = -12.0f; db > floorDb_; db -= 12.0f)
    {
        const float y = plot.getY() + plot.getHeight() * (-db / span);
        g.fillRect (plot.getX(), y, plot.getWidth(), 0.5f);
    }

    const float slotW = plot.getWidth() / (float) numHarmonics_;
    const float barW  = juce::jmax (2.0f, slotW * 0.55f);

    for (int i = 0; i < numHarmonics_; ++i)
    {
        const float cx = plot.getX() + slotW * ((float) i + 0.5f);
        const float db = haveData_ ? magsDb_[i] : floorDb_;
        const float t  = juce::jlimit (0.0f, 1.0f, (db - floorDb_) / span);

        auto bar = juce::Rectangle<float> (cx - barW * 0.5f,
                                           plot.getBottom() - plot.getHeight() * t,
                                           barW, plot.getHeight() * t);

        // The fundamental is the reference, not a harmonic — drawn in the
        // neutral grey the EQ's spectrum uses so the eye reads the coloured
        // bars as "what the device added".
        //
        // Even harmonics cyan, odd harmonics purple: even-order is the warm
        // one, odd-order is the aggressive one, and that split is the single
        // most useful thing this view can say at a glance.
        const auto col = (i == 0) ? Colours::text2
                       : ((i % 2) == 1 ? Colours::blue2 : Colours::purple);

        if (bar.getHeight() > 0.5f)
        {
            g.setColour (col.withAlpha (0.75f * a));
            g.fillRoundedRectangle (bar, 1.0f);
        }

        if (slotW > 12.0f)
        {
            g.setColour (Colours::text3.withMultipliedAlpha (0.8f * a));
            g.setFont (uiFont (7.5f, i == 0));
            g.drawText (juce::String (i + 1),
                        (int) (cx - slotW * 0.5f), (int) plot.getBottom() - 9,
                        (int) slotW, 9, juce::Justification::centred);
        }
    }
}

} // namespace echojay::viz
