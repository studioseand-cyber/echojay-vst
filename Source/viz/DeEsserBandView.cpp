/*
    DeEsserBandView.cpp  —  see DeEsserBandView.h.
*/

#include "DeEsserBandView.h"

#include <complex>

namespace echojay::viz
{

namespace
{
    constexpr float kGridStepDb = 6.0f;

    // Points across the plot. Both curves are a handful of complex multiplies
    // each, and they are evaluated on a REBUILD, not on a repaint.
    constexpr int kResolution = 180;

    const float kFreqGrid[]      = { 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    const char* const kFreqLbl[] = { "500",  "1k",    "2k",    "5k",    "10k"    };

    // The complex response of one biquad at a normalised frequency.
    std::complex<double> response (const echojay::Biquad& f, double w) noexcept
    {
        const std::complex<double> z1 = std::polar (1.0, -w);
        const std::complex<double> z2 = z1 * z1;

        const std::complex<double> num = (double) f.b0 + (double) f.b1 * z1 + (double) f.b2 * z2;
        const std::complex<double> den = 1.0        + (double) f.a1 * z1 + (double) f.a2 * z2;

        return den == std::complex<double> (0.0, 0.0) ? std::complex<double> (0.0, 0.0)
                                                      : num / den;
    }
}

DeEsserBandView::DeEsserBandView()
{
    setCaption ("BAND");
}

DeEsserBandView::~DeEsserBandView() = default;

void DeEsserBandView::setBand (float freqHz, double q)
{
    if (! moved (freqHz, freqHz_, 0.5f) && std::abs (q - q_) < 1.0e-6) return;

    freqHz_ = freqHz;
    q_      = q;
    rebuildPaths();
    repaint();
}

void DeEsserBandView::setSplitMode (bool split)
{
    if (split == split_) return;
    split_ = split;
    rebuildPaths();      // the gain-vs-frequency shape is a different shape
    repaint();
}

void DeEsserBandView::setDepthDb (float db)
{
    const float d = std::max (db, 6.0f);
    if (! moved (d, depthDb_, 0.01f)) return;
    depthDb_ = d;
    rebuildPaths();
    repaint();
}

void DeEsserBandView::setDwellSource (const echojay::dyn::DwellTap* tap, bool active)
{
    const bool wantTimer = glow_.setSource (tap, active);

    if (tap == nullptr)
    {
        stopTimer();
        repaint();
        return;
    }

    // Only started when there is something to show, so a de-esser sitting
    // bypassed in a rack costs nothing once its glow has faded. The editor
    // re-passes the tap 20 times a second, so un-bypassing picks it back up.
    if (wantTimer && ! isTimerRunning())
    {
        lastTickMs_ = 0.0;
        startTimerHz (DwellGlow::kGlowHz);
    }
}

// ---------------------------------------------------------------------------
// The glow's frame. Identical in shape to TransferCurveView's, because it is
// the same glow: measure the elapsed time, hand it to DwellGlow, stop when
// there is nothing left to fade.
// ---------------------------------------------------------------------------
void DeEsserBandView::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float  dt  = lastTickMs_ > 0.0 ? (float) ((now - lastTickMs_) * 0.001)
                                         : 1.0f / (float) DwellGlow::kGlowHz;
    lastTickMs_ = now;

    if (glow_.tick (dt)) repaint();

    if (! glow_.isAlive() && ! glow_.isActive()) stopTimer();
}

void DeEsserBandView::setGainReductionDb (float db)
{
    // The gain curve's DEPTH is live, so this rebuilds the path rather than only
    // repainting — but only when it moved by a tenth of a dB, which at 20 Hz is
    // the difference between a shape that breathes and one that thrashes.
    if (! moved (db, grDb_, 0.1f)) return;
    grDb_ = db;
    rebuildPaths();
    repaint();
}

void DeEsserBandView::setBandLevelDb (float db)
{
    const bool wasVisible = levelDb_ > kNoLevel;
    const bool isVisible  = db       > kNoLevel;

    if (wasVisible == isVisible && ! moved (db, levelDb_, 0.25f)) return;
    levelDb_ = db;
    repaint();
}

void DeEsserBandView::setThresholdDb (float db)
{
    if (! moved (db, threshDb_, 0.05f)) return;
    threshDb_ = db;
    repaint();
}

void DeEsserBandView::setListening (bool on)
{
    if (on == listening_) return;
    listening_ = on;
    repaint();
}

float DeEsserBandView::bandMagnitude (float hz) const noexcept
{
    // The DSP's own filter, built by the DSP's own factory from the DSP's own
    // numbers — not a bell approximated in drawing code.
    const auto bp = echojay::Biquad::bandpass (kDrawSampleRate, freqHz_, q_);
    const double w = 2.0 * echojay::dyn::kPi * hz / kDrawSampleRate;
    return (float) std::abs (response (bp, w));
}

void DeEsserBandView::resized()
{
    rebuildPaths();
}

void DeEsserBandView::rebuildPaths()
{
    bandPath_.clear();
    bandFill_.clear();
    bandCrest_.clear();
    crestAttenDb_.clear();

    if (! hasPlot()) return;

    const auto plot = plotArea();

    auto xForHz = [&] (float hz)
    {
        const float t = std::log (hz / kMinHz) / std::log (kMaxHz / kMinHz);
        return plot.getX() + plot.getWidth() * juce::jlimit (0.0f, 1.0f, t);
    };
    auto yForDb = [&] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, -db / depthDb_);
        return plot.getY() + plot.getHeight() * t;
    };

    // The sidechain filters, built once for the whole sweep.
    const auto bp = echojay::Biquad::bandpass (kDrawSampleRate, freqHz_, q_);
    const auto lp = echojay::Biquad::lowpass  (kDrawSampleRate, freqHz_,
                                               echojay::LinkwitzRiley4::kQ);
    const auto hp = echojay::Biquad::highpass (kDrawSampleRate, freqHz_,
                                               echojay::LinkwitzRiley4::kQ);

    const float gLin = echojay::dyn::dbToGain (grDb_);

    for (int i = 0; i < kResolution; ++i)
    {
        const float t  = (float) i / (float) (kResolution - 1);
        const float hz = kMinHz * std::pow (kMaxHz / kMinHz, t);
        const double w = 2.0 * echojay::dyn::kPi * hz / kDrawSampleRate;

        // ---- what MOVES: the gain this device applies at this frequency ----
        float appliedDb;

        if (! split_)
        {
            // Wide: one gain across the whole spectrum.
            appliedDb = grDb_;
        }
        else
        {
            // Split: out = low + high * g, with an LR4 (Butterworth squared)
            // split. Computed as the COMPLEX sum the DSP actually produces, not
            // as a crossfade between two magnitudes — the two legs are not in
            // phase, and treating them as if they were is what makes a drawn
            // crossover disagree with the audible one around the corner.
            const auto l = response (lp, w); const auto l4 = l * l;
            const auto h = response (hp, w); const auto h4 = h * h;

            const auto sum = l4 + (double) gLin * h4;
            appliedDb = echojay::dyn::gainToDb ((float) std::abs (sum));
        }

        const juce::Point<float> p { xForHz (hz), yForDb (appliedDb) };
        if (i == 0) bandPath_.startNewSubPath (p);
        else        bandPath_.lineTo (p);

        // ---- what it LISTENS to: the detector's band, as a filled shape -----
        // Drawn on its own 0..1 sensitivity scale rather than on the dB axis,
        // because it is not a gain — it is "how much does energy here reach the
        // detector". Anchored at the bottom so it reads as a region of the
        // spectrum rather than as a second gain curve to be misread as one.
        const float mag = (float) std::abs (response (bp, w));
        const float by  = plot.getBottom() - plot.getHeight() * 0.45f
                                             * juce::jlimit (0.0f, 1.0f, mag);

        if (i == 0)
        {
            bandFill_.startNewSubPath (plot.getX(), plot.getBottom());
            bandFill_.lineTo (xForHz (hz), by);
        }
        else
        {
            bandFill_.lineTo (xForHz (hz), by);
        }

        // The crest, and how far DOWN the filter is here. The attenuation is
        // the whole basis of the glow — it is the extra level content at this
        // frequency needs before the detector notices it — so it is captured
        // from the same response evaluation the shape is drawn from rather than
        // recomputed later against a filter that might have moved.
        bandCrest_.push_back ({ xForHz (hz), by });
        crestAttenDb_.push_back (-echojay::dyn::gainToDb (juce::jlimit (0.0f, 1.0f, mag)));
    }

    bandFill_.lineTo (plot.getRight(), plot.getBottom());
    bandFill_.closeSubPath();
}

// ---------------------------------------------------------------------------
// The glow's columns: where the band's crest is at each pixel, and how often
// content there actually triggers the device (see the header for why that is
// the honest question on a frequency axis).
// ---------------------------------------------------------------------------
void DeEsserBandView::renderGlow (juce::Graphics& g, juce::Rectangle<float> plot, float a)
{
    const int n = (int) bandCrest_.size();
    if (n < 2 || (int) crestAttenDb_.size() != n) return;

    const int w = glow_.prepareColumns (plot);
    if (w < 2) return;

    const float originX = std::floor (plot.getX());
    const float originY = std::floor (plot.getY());

    auto& colY = glow_.columnY();
    auto& colE = glow_.columnIntensity();

    for (int ix = 0; ix < w; ++ix)
    {
        // The crest is sampled on a LOG-frequency sweep and this walk is in
        // linear pixels, but both are parameterised on the same t across the
        // plot's width — so interpolating on t lands on the same frequency the
        // shape was drawn at, without redoing the log.
        const float t   = (originX + (float) ix + 0.5f - plot.getX()) / plot.getWidth();
        const float pos = juce::jlimit (0.0f, 1.0f, t) * (float) (n - 1);

        const int   i0   = juce::jlimit (0, n - 2, (int) pos);
        const float frac = pos - (float) i0;

        const auto& p0 = bandCrest_[(std::size_t) i0];
        const auto& p1 = bandCrest_[(std::size_t) i0 + 1];

        colY[(std::size_t) ix] = (p0.y + (p1.y - p0.y) * frac) - originY;

        const float atten = crestAttenDb_[(std::size_t) i0]
                          + (crestAttenDb_[(std::size_t) i0 + 1]
                           - crestAttenDb_[(std::size_t) i0]) * frac;

        if (atten > kMaxBandAttenDb) { colE[(std::size_t) ix] = 0.0f; continue; }

        // The bar this frequency has to clear: the threshold, plus whatever the
        // sidechain filter takes off here. Then ask the histogram how much of
        // the detector's time is spent at or above it.
        colE[(std::size_t) ix] = glow_.fractionAbove (threshDb_ + atten);
    }

    glow_.render (g, a);
}

void DeEsserBandView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a = dimAlpha();

    auto xForHz = [&] (float hz)
    {
        const float t = std::log (hz / kMinHz) / std::log (kMaxHz / kMinHz);
        return plot.getX() + plot.getWidth() * juce::jlimit (0.0f, 1.0f, t);
    };
    auto yForDb = [&] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, -db / depthDb_);
        return plot.getY() + plot.getHeight() * t;
    };

    // ---- grid --------------------------------------------------------------
    g.setFont (uiFont (7.5f));

    for (float db = 0.0f; db >= -depthDb_; db -= kGridStepDb)
    {
        const float y = yForDb (db);
        g.setColour (Colours::border.withMultipliedAlpha (a));
        g.fillRect (plot.getX(), y, plot.getWidth(), 0.5f);

        if (db < -0.5f && db > -depthDb_ + 0.5f && plot.getWidth() > 70.0f)
        {
            g.setColour (Colours::text3.withMultipliedAlpha (0.75f * a));
            g.drawText (juce::String ((int) db),
                        (int) plot.getX() + 2, (int) y - 6, 26, 12,
                        juce::Justification::centredLeft);
        }
    }

    for (int i = 0; i < (int) std::size (kFreqGrid); ++i)
    {
        const float x = xForHz (kFreqGrid[i]);
        g.setColour (Colours::border.withMultipliedAlpha (a));
        g.fillRect (x, plot.getY(), 0.5f, plot.getHeight());

        if (plot.getHeight() > 40.0f)
        {
            g.setColour (Colours::text3.withMultipliedAlpha (0.7f * a));
            g.drawText (kFreqLbl[i], (int) x - 14, (int) plot.getBottom() - 11, 28, 10,
                        juce::Justification::centred);
        }
    }

    // resized() normally builds these; this covers a view painted before it was
    // ever laid out, which is exactly what the editor-paint harness does.
    if (bandPath_.isEmpty()) rebuildPaths();

    // ---- the detector's band, cold ----------------------------------------
    // The region and its crest, drawn whatever the histogram says, so a silent
    // or bypassed de-esser still shows WHERE it is listening. The glow goes
    // over the top of this, exactly as it does over the transfer curve.
    if (! bandFill_.isEmpty())
    {
        g.setColour (Colours::purple.withAlpha (0.22f * a));
        g.fillPath (bandFill_);
        g.setColour (Colours::purple.withMultipliedAlpha (DwellGlow::kColdAlpha * a));
        g.strokePath (bandFill_, juce::PathStrokeType (1.0f));
    }

    // ---- ...and how hard it is actually being triggered, as heat -----------
    renderGlow (g, plot, a);

    // ---- the centre frequency ----------------------------------------------
    // Amber when the band is over the threshold — i.e. when this frequency is
    // the reason the device is working right now. That colour change is the
    // fastest available answer to "am I pointed at the sibilance?".
    {
        const bool triggering = levelDb_ > kNoLevel && levelDb_ >= threshDb_;
        const float fx = xForHz (freqHz_);

        g.setColour ((triggering ? Colours::amber : Colours::text3)
                         .withMultipliedAlpha ((triggering ? 0.8f : 0.45f) * a));
        g.fillRect (fx, plot.getY(), 1.0f, plot.getHeight());

        if (plot.getWidth() > 90.0f)
        {
            g.setColour ((triggering ? Colours::amber : Colours::text3)
                             .withMultipliedAlpha (a));
            g.setFont (uiFont (8.0f, true));

            const juce::String lbl = freqHz_ >= 1000.0f
                ? juce::String (freqHz_ / 1000.0f, 1) + "k"
                : juce::String ((int) freqHz_);

            g.drawText (lbl, (int) fx - 20, (int) plot.getY() + 1, 40, 10,
                        juce::Justification::centred);
        }
    }

    // ---- what moves --------------------------------------------------------
    // Drawn last so it sits over the band fill: this is the line that answers
    // "what is this device doing to my signal", and it is the one that has to be
    // readable when the two overlap.
    if (! bandPath_.isEmpty())
    {
        g.setColour (Colours::blue2.withMultipliedAlpha (a));
        g.strokePath (bandPath_, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }

    // ---- readouts ----------------------------------------------------------
    if (plot.getWidth() > 110.0f)
    {
        g.setFont (uiFont (8.5f, true));

        if (listening_)
        {
            // The device is not outputting this picture. Saying so is the whole
            // point: a de-esser left in listen sounds broken for no visible
            // reason, and this view would otherwise happily draw the processing
            // it is not doing.
            g.setColour (Colours::amber);
            g.drawText ("LISTEN - monitoring the band",
                        plot.reduced (3.0f, 1.0f), juce::Justification::bottomRight);
        }
        else if (! isDimmed() && grDb_ < -0.05f)
        {
            g.setColour (Colours::blue2.brighter (0.2f));
            g.drawText (juce::String (grDb_, 1) + " dB "
                            + juce::String (split_ ? "above " : "wide"),
                        plot.reduced (3.0f, 1.0f), juce::Justification::bottomRight);
        }
    }
}

} // namespace echojay::viz
