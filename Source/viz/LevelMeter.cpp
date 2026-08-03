/*
    LevelMeter.cpp  —  see LevelMeter.h.
*/

#include "LevelMeter.h"

namespace echojay::viz
{

LevelMeter::LevelMeter()
{
    setFramed (true);
    startTimerHz (30);      // peak-hold decay only; the value itself is pushed
}

LevelMeter::~LevelMeter()
{
    stopTimer();
}

void LevelMeter::setFloorDb (float db)
{
    const float f = juce::jlimit (-120.0f, -12.0f, db);
    if (! moved (f, floorDb_, 0.5f)) return;
    floorDb_ = f;
    repaint();
}

void LevelMeter::setVertical (bool v)
{
    if (v == vertical_) return;
    vertical_ = v;
    repaint();
}

void LevelMeter::setLevelLinear (float peak, float rms)
{
    auto toDb = [] (float g)
    {
        return g > 1.0e-6f ? 20.0f * std::log10 (g) : kSilenceDb;
    };
    setLevelDb (toDb (peak), toDb (rms));
}

void LevelMeter::setLevelDb (float peakDb, float rmsDb)
{
    // The same 0.02 dB gate the GR meter uses, and for the same reason: at
    // 20-30 Hz an unconditional repaint of several meters is real CPU spent on
    // pixels that did not move.
    const bool changed = moved (peakDb, peakDb_, 0.02f) || moved (rmsDb, rmsDb_, 0.02f);

    peakDb_ = peakDb;
    rmsDb_  = rmsDb;

    if (peakDb > holdDb_)
    {
        holdDb_     = peakDb;
        holdFrames_ = kPeakHoldFrames;
    }

    if (changed) repaint();
}

void LevelMeter::timerCallback()
{
    if (holdFrames_ > 0) { --holdFrames_; return; }
    if (holdDb_ <= peakDb_) return;

    // Falls back toward the live value rather than to the floor, so the hold
    // line never drops below the bar it is holding above.
    holdDb_ = juce::jmax (peakDb_, holdDb_ - 1.5f);
    repaint();
}

void LevelMeter::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a    = dimAlpha();
    const float span = -floorDb_;
    if (span <= 0.0f) return;

    auto normOf = [&] (float db)
    {
        return juce::jlimit (0.0f, 1.0f, (db - floorDb_) / span);
    };

    g.setColour (Colours::bg2.withMultipliedAlpha (a));
    g.fillRoundedRectangle (plot, 1.5f);

    // Ticks every 12 dB, so "about -18" is readable off the bar.
    g.setColour (Colours::border.withMultipliedAlpha (a));
    for (float db = -12.0f; db > floorDb_; db -= 12.0f)
    {
        const float t = normOf (db);
        if (vertical_)
            g.fillRect (plot.getX(), plot.getBottom() - plot.getHeight() * t,
                        plot.getWidth(), 0.5f);
        else
            g.fillRect (plot.getX() + plot.getWidth() * t, plot.getY(),
                        0.5f, plot.getHeight());
    }

    if (isDimmed()) return;

    auto barFor = [&] (float t)
    {
        auto r = plot.reduced (1.0f);
        if (vertical_) return r.removeFromBottom (r.getHeight() * t);
        return r.removeFromLeft (r.getWidth() * t);
    };

    // RMS underneath in cyan (how loud it is), peak over it as a thin bright
    // overlay (how close to clipping it is), red once it is over -1 dBFS.
    const float rt = normOf (rmsDb_);
    if (rt > 0.001f)
    {
        g.setColour (Colours::blue.withAlpha (0.55f));
        g.fillRoundedRectangle (barFor (rt), 1.0f);
    }

    const float pt = normOf (peakDb_);
    if (pt > 0.001f)
    {
        g.setColour ((peakDb_ > -1.0f ? Colours::red : Colours::blue2).withAlpha (0.85f));
        auto peakBar = barFor (pt);
        if (vertical_) g.fillRect (peakBar.removeFromTop (1.5f));
        else           g.fillRect (peakBar.removeFromRight (1.5f));
    }

    // Peak hold.
    const float ht = normOf (holdDb_);
    if (ht > 0.001f)
    {
        g.setColour ((holdDb_ > -1.0f ? Colours::red : Colours::blue2).brighter (0.3f));
        if (vertical_)
            g.fillRect (plot.getX() + 1.0f,
                        plot.getBottom() - plot.getHeight() * ht - 1.0f,
                        plot.getWidth() - 2.0f, 1.5f);
        else
            g.fillRect (plot.getX() + plot.getWidth() * ht - 1.0f,
                        plot.getY() + 1.0f, 1.5f, plot.getHeight() - 2.0f);
    }

    // The number. A meter you cannot read a value off is a decoration.
    if (! vertical_ && plot.getWidth() > 56.0f && peakDb_ > floorDb_)
    {
        g.setColour (Colours::text2);
        g.setFont (uiFont (8.0f, true));
        g.drawText (juce::String (peakDb_, 1), plot.reduced (4.0f, 0.0f),
                    juce::Justification::centredRight);
    }
}

} // namespace echojay::viz
