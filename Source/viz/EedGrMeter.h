/*
    EedGrMeter.h  —  the gain-reduction meter shared by the Dynamics cluster
    (BUILTIN_SUITE_PLAN.md §2: "where a device wants a spectrum/meter, reuse the
    analyzer component").

    The EQ's dynamic-band metering (SurgicalEqEditor::updateDynamicMeters plus the
    amber action bar it draws) is the same idea drawn into the curve: read a
    single float off the engine on a timer, repaint only when it moved, draw the
    live reduction in amber against the EchoJay palette. That drawing is welded to
    the EQ's frequency/gain axes, so what is reused here is the PART that is
    reusable — the read-on-a-timer contract, the change threshold that keeps the
    repaint rate down, the peak hold, and the palette — as a standalone component
    the other five faces can simply place.

    WHY A METER IS NOT OPTIONAL. A compressor with no gain-reduction readout is a
    device you dial by faith. It is also the only way to see that an AI move
    landed: a threshold set 10 dB too low looks identical on the knobs and is
    obvious on the meter.

    HOW IT IS FED. The owning editor calls setGainReductionDb() from its existing
    timer with DynamicsCore::gainReductionDb(). That value is a benign racy read
    of one float on the audio thread — same contract as EqEngine's
    getBandDynamicGainDb — so there is no lock anywhere on this path.
*/

#pragma once

#include <JuceHeader.h>
#include "../EchoJayDeviceLookAndFeel.h"

namespace echojay::device
{

class GrMeter : public juce::Component,
                private juce::Timer
{
public:
    // `maxDb` is how far the scale runs, as a POSITIVE number of dB of
    // reduction. A limiter wants 12, a gate wants its full range.
    explicit GrMeter (float maxDb = 24.0f) : maxDb_ (juce::jmax (1.0f, maxDb))
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);      // peak-hold decay only; the value itself is pushed
    }

    ~GrMeter() override { stopTimer(); }

    void setMaxDb (float maxDb)
    {
        maxDb_ = juce::jmax (1.0f, maxDb);
        repaint();
    }

    // Horizontal (wide, under a row of dials) or vertical (a column beside them).
    void setVertical (bool v) { vertical_ = v; repaint(); }

    void setCaption (const juce::String& c) { caption_ = c; repaint(); }

    // Greys the meter out — used for BYPASS, so a bypassed device does not sit
    // there displaying stale reduction it is not applying.
    void setDimmed (bool d)
    {
        if (d == dimmed_) return;
        dimmed_ = d;
        if (d) { value_ = 0.0f; peak_ = 0.0f; }
        repaint();
    }

    // `db` is the reduction as the core reports it: NEGATIVE, or 0 for none.
    void setGainReductionDb (float db)
    {
        const float v = juce::jlimit (0.0f, maxDb_, -db);

        // Repaint only on a visible change. The EQ uses the same 0.02 dB gate on
        // its dynamic meters; at 15-30 Hz an unconditional repaint of six devices
        // is real CPU spent on pixels that did not move.
        if (std::abs (v - value_) < 0.02f) return;

        value_ = v;
        if (v > peak_) { peak_ = v; peakHoldFrames_ = kPeakHoldFrames; }
        repaint();
    }

    float currentDb() const noexcept { return value_; }

    void paint (juce::Graphics& g) override
    {
        using C = echojay::device::Colours;

        auto r = getLocalBounds().toFloat();
        if (r.isEmpty()) return;

        auto track = r;
        juce::Rectangle<float> capBox;

        if (caption_.isNotEmpty())
        {
            capBox = vertical_ ? track.removeFromBottom (metrics::kCapH)
                               : track.removeFromLeft (34.0f);
        }

        g.setColour (C::bg3);
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (C::border2);
        g.drawRoundedRectangle (track, 2.0f, 1.0f);

        // Scale ticks every 6 dB, so the eye reads "about 9 dB" off the meter
        // rather than "some".
        g.setColour (C::border2);
        for (float d = 6.0f; d < maxDb_; d += 6.0f)
        {
            const float t = d / maxDb_;
            if (vertical_)
            {
                const float y = track.getY() + track.getHeight() * t;
                g.fillRect (track.getX(), y, track.getWidth(), 1.0f);
            }
            else
            {
                const float x = track.getX() + track.getWidth() * t;
                g.fillRect (x, track.getY(), 1.0f, track.getHeight());
            }
        }

        if (! dimmed_)
        {
            // Amber, matching the EQ's dynamic-action bar, so "this is dynamics
            // moving" reads the same everywhere in EchoJay. It saturates toward
            // red as the reduction approaches the end of the scale, which is the
            // cue that the device is out of headroom rather than working.
            const float t   = juce::jlimit (0.0f, 1.0f, value_ / maxDb_);
            const auto  col = C::amber.interpolatedWith (C::red, juce::jlimit (0.0f, 1.0f, t * 1.2f));

            if (t > 0.001f)
            {
                auto fill = track.reduced (1.0f);
                fill = vertical_ ? fill.withHeight (fill.getHeight() * t)
                                 : fill.withWidth  (fill.getWidth()  * t);
                g.setColour (col.withAlpha (0.85f));
                g.fillRoundedRectangle (fill, 1.5f);
            }

            // Peak hold: the deepest reduction of the last second, as a line.
            // Dynamics move faster than the eye, and without a hold the meter
            // reads as a flicker with no number attached to it.
            const float pt = juce::jlimit (0.0f, 1.0f, peak_ / maxDb_);
            if (pt > 0.001f)
            {
                g.setColour (C::amber.brighter (0.4f));
                if (vertical_)
                    g.fillRect (track.getX() + 1.0f,
                                track.getY() + track.getHeight() * pt - 1.0f,
                                track.getWidth() - 2.0f, 2.0f);
                else
                    g.fillRect (track.getX() + track.getWidth() * pt - 1.0f,
                                track.getY() + 1.0f, 2.0f, track.getHeight() - 2.0f);
            }
        }

        if (caption_.isNotEmpty())
        {
            g.setColour (dimmed_ ? C::text3 : C::text2);
            g.setFont (uiFont (8.5f, true));
            g.drawText (caption_, capBox, juce::Justification::centred);
        }

        // The number, in the track. A meter you cannot read a value off is a
        // decoration; a compressor's whole job is expressed in this one figure.
        if (! dimmed_ && value_ > 0.05f && track.getWidth() > 40.0f)
        {
            g.setColour (C::text);
            g.setFont (uiFont (9.0f, true));
            g.drawText ("-" + juce::String (value_, 1) + " dB",
                        track.reduced (4.0f, 0.0f),
                        vertical_ ? juce::Justification::centredTop
                                  : juce::Justification::centredRight);
        }
    }

private:
    // ~1 second at 30 Hz.
    static constexpr int kPeakHoldFrames = 30;

    void timerCallback() override
    {
        if (peakHoldFrames_ > 0) { --peakHoldFrames_; return; }
        if (peak_ <= value_)     return;

        // Fall back toward the live value rather than to zero, so the hold line
        // never dips below the bar it is holding above.
        peak_ = juce::jmax (value_, peak_ - maxDb_ * 0.02f);
        repaint();
    }

    float        maxDb_ = 24.0f;
    float        value_ = 0.0f, peak_ = 0.0f;
    int          peakHoldFrames_ = 0;
    bool         vertical_ = false, dimmed_ = false;
    juce::String caption_ { "GR" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrMeter)
};

} // namespace echojay::device
