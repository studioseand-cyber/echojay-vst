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

    /** ---- SEAN'S RULING, 15 Oct 2026: A 2-SECOND HOLD, AND A FIGURE TO READ -----------------------
        *"add a 2-second peak-hold tick on the EchoJay Compressor GR meter so I can verify the figure
        by eye."* The point is to be able to compare what the meter shows with what check(gr) reports,
        which a 1-second hold with no number does not allow.

        SETTABLE RATHER THAN CHANGED: this component is shared by the whole Dynamics cluster, and he
        asked for it on the compressor. The default stays exactly as the other five faces have it, so
        this adds a capability instead of altering theirs. */
    void setPeakHoldMs (int ms) { peakHoldMs_ = juce::jmax (0, ms); }
    /** ---- SEAN, 15 Oct 2026: THE HELD FIGURE COMES FROM THE DEVICE, NOT FROM THE UI --------------
        "the tile must match the device's own GR meter (tile said 0.4 while the meter said 1.2)."

        They disagreed because this component computed its OWN hold while the tile sampled the live
        float at a different instant. When the owner pushes the device's held value here, the meter
        stops computing a second answer and draws the one the tile and the agent also read. */
    void setHeldDb (float heldPositiveDb)
    {
        const auto v = juce::jmax (0.0f, heldPositiveDb);
        if (std::abs (v - peak_) < 0.01f) return;
        peak_ = v;
        peakHoldFrames_ = 0;          // the DEVICE owns the hold now; this component only draws it
        deviceOwnsHold_ = true;
        repaint();
    }
    /** Draw the held figure as text beside the tick, so the eye has a number to compare. */
    void setShowHeldFigure (bool show) { showHeldFigure_ = show; repaint(); }

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
        if (v > peak_) { peak_ = v; peakHoldFrames_ = juce::jmax (1, (peakHoldMs_ * 30) / 1000); }
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

                // THE NUMBER BESIDE THE TICK (Sean, 15 Oct 2026): a held line with no figure cannot
                // be compared with what check(gr) reports, which is the whole reason for the hold.
                //
                // ---- SEAN'S #15 (16 Oct 2026): AND IT MUST NOT SIT ON TOP OF THE LIVE FIGURE -----
                // *"the held figure overlaps another label ('-4.4 dB' drawn over 'dB')."* Both were
                // drawn centred-right in the same track: the live value at the right edge and the
                // held figure in the last 30 px of it. On a horizontal meter the held figure now owns
                // the right end and the live value is drawn to the LEFT of it (see heldFigureW_
                // below, which the live-value block reads), so the two cannot share a pixel.
                if (showHeldFigure_)
                {
                    g.setColour (C::amber.brighter (0.4f));
                    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
                    const auto txt = juce::String (peak_, 1);
                    if (vertical_)
                        g.drawText (txt, track.getX(), (int) (track.getY() + track.getHeight() * pt) - 11,
                                    (int) track.getWidth(), 10, juce::Justification::centred, false);
                    else
                        g.drawText (txt, labelBoxes (track, false, true).held.toNearestInt(),
                                    juce::Justification::centredRight, false);
                }
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
            // The held figure owns the right end of a horizontal track (Sean's #15), so the live
            // value is drawn clear of it. The reserve is taken only when the held figure is actually
            // being drawn - a meter with no hold still uses the whole track.
            const auto valueBox = labelBoxes (track, vertical_,
                                              showHeldFigure_ && peak_ > 0.05f).live;
            g.setColour (C::text);
            g.setFont (uiFont (9.0f, true));
            g.drawText ("-" + juce::String (value_, 1) + " dB",
                        valueBox,
                        vertical_ ? juce::Justification::centredTop
                                  : juce::Justification::centredRight);
        }
    }

private:
    // The hold, in milliseconds. 1000 is what every face had before the compressor asked for 2000;
    // the timer runs at 30 Hz, so the frame count is derived rather than written twice.
    int peakHoldMs_ = 1000;

    void timerCallback() override
    {
        if (deviceOwnsHold_)     return;          // the device's hold is pushed, not decayed here
        if (peakHoldFrames_ > 0) { --peakHoldFrames_; return; }
        if (peak_ <= value_)     return;

        // Fall back toward the live value rather than to zero, so the hold line
        // never dips below the bar it is holding above.
        peak_ = juce::jmax (value_, peak_ - maxDb_ * 0.02f);
        repaint();
    }

    /** The right-hand band a horizontal meter keeps for the HELD figure, so the live value cannot
        be drawn over it (Sean's #15, 16 Oct 2026). Wide enough for "12.5" at 9 pt bold plus a gap. */
    static constexpr int heldFigureW_ = 32;
public:
    /** ---- THE TWO LABEL BOXES, AS A PURE FUNCTION (Sean's #15, 16 Oct 2026) ---------------------
        *"the held figure overlaps another label ('-4.4 dB' drawn over 'dB'). Fix the layout, add a
        rig row for it."* The overlap was two drawText calls reasoning separately about one track, so
        the fix is one function that both of them (and a guard) read: whatever it returns cannot
        overlap, and a test can hold it to that without a window, a device or an audio thread.
        `heldShown` is false on a meter with no hold, where the live value owns the whole track. */
    struct LabelBoxes { juce::Rectangle<float> live, held; };
    static LabelBoxes labelBoxes (juce::Rectangle<float> track, bool vertical, bool heldShown)
    {
        LabelBoxes b;
        b.live = track.reduced (4.0f, 0.0f);
        if (vertical || ! heldShown) return b;                 // vertical draws the held figure on the bar
        b.held = { track.getRight() - (float) heldFigureW_, track.getY(),
                   (float) heldFigureW_ - 2.0f, track.getHeight() };
        b.live = b.live.withWidth (juce::jmax (20.0f, b.live.getWidth() - (float) heldFigureW_));
        return b;
    }
private:
    float        maxDb_ = 24.0f;
    float        value_ = 0.0f, peak_ = 0.0f;
    int          peakHoldFrames_ = 0;
    bool         vertical_ = false, dimmed_ = false, showHeldFigure_ = false;
    bool         deviceOwnsHold_ = false;
    juce::String caption_ { "GR" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrMeter)
};

} // namespace echojay::device
