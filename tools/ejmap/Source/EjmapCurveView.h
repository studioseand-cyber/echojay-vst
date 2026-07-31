/*
  EjmapCurveView.h

  M3 pass two: the sanitized curve, drawn, with its rejections visible.

  WHY DRAWN AT ALL. A sanitizer that silently truncates reads as "covered
  everything". The human accepting a curve is accepting the sanitizer's
  judgment, and they can only do that honestly if they can SEE what was cut:
  kept anchors as the line, rejected points marked distinctly, and the counts
  restated in the caption. The reject button is the other half of the same
  contract: a human who does not believe the curve forces the typed path
  rather than living with a wrong one.

  Norm runs on the x axis and value on the y axis, scaled to the RAW extent,
  not the sanitized one: scaling to the kept points would push the rejected
  ones off the canvas, which is the silent truncation this view exists to
  prevent.
*/

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "EjmapSweeper.h"

namespace ejmap
{

class CurveView : public juce::Component
{
public:
    CurveView()
    {
        addAndMakeVisible (rejectButton);
        rejectButton.setButtonText ("Reject curve - type anchors");
        rejectButton.onClick = [this] { if (onReject) onReject(); };
    }

    std::function<void()> onReject;

    void show (const SweepOutcome& outcomeIn, const juce::String& titleIn)
    {
        outcome = outcomeIn;
        title   = titleIn;
        setVisible (outcome.anchors.size() >= 2);
        repaint();
    }

    void clear()
    {
        outcome = {};
        setVisible (false);
    }

    void resized() override
    {
        rejectButton.setBounds (getLocalBounds().removeFromBottom (22).removeFromRight (190));
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (juce::Colour (0xff161c26));
        g.fillRoundedRectangle (area, 4.0f);

        auto plot = area.reduced (8.0f);
        plot.removeFromBottom (24.0f);          // caption + button strip
        plot.removeFromTop (16.0f);             // title

        if (outcome.rawAnchors.isEmpty())
            return;

        // Scale against the RAW extent so rejected points stay on canvas.
        float vLo = outcome.rawAnchors.getFirst()[0], vHi = vLo;
        for (const auto& a : outcome.rawAnchors)
        { vLo = juce::jmin (vLo, a[0]); vHi = juce::jmax (vHi, a[0]); }
        if (vHi - vLo < 1.0e-9f) { vLo -= 0.5f; vHi += 0.5f; }

        auto xFor = [&] (float n) { return plot.getX() + n * plot.getWidth(); };
        auto yFor = [&] (float v) { return plot.getBottom()
                                      - (v - vLo) / (vHi - vLo) * plot.getHeight(); };

        // Kept anchors: the line the map will interpolate.
        g.setColour (juce::Colour (0xff2f7f8c));
        juce::Path path;
        for (int i = 0; i < outcome.anchors.size(); ++i)
        {
            const auto pt = juce::Point<float> (xFor (outcome.anchors[i][1]),
                                                yFor (outcome.anchors[i][0]));
            if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
        }
        g.strokePath (path, juce::PathStrokeType (1.6f));

        g.setColour (juce::Colour (0xff9fd8e0));
        for (const auto& a : outcome.anchors)
            g.fillEllipse (xFor (a[1]) - 2.5f, yFor (a[0]) - 2.5f, 5.0f, 5.0f);

        // Rejected raw points: a distinct mark, never omitted.
        g.setColour (juce::Colour (0xffd86a6a));
        for (const auto& a : outcome.rawAnchors)
        {
            bool kept = false;
            for (const auto& k : outcome.anchors)
                if (juce::approximatelyEqual (k[0], a[0]) && juce::approximatelyEqual (k[1], a[1]))
                { kept = true; break; }
            if (! kept)
            {
                const float x = xFor (a[1]), y = yFor (a[0]);
                g.drawLine (x - 3.5f, y - 3.5f, x + 3.5f, y + 3.5f, 1.4f);
                g.drawLine (x - 3.5f, y + 3.5f, x + 3.5f, y - 3.5f, 1.4f);
            }
        }

        g.setColour (juce::Colour (0xff9fd8e0));
        g.setFont (12.0f);
        g.drawText (title, area.reduced (8.0f).removeFromTop (14.0f),
                    juce::Justification::topLeft);

        juce::String cap;
        cap << outcome.anchors.size() << " kept ("
            << outcome.method << (outcome.anchorsReversed ? ", descending" : ", ascending") << ")";
        if (outcome.rejectedPoints > 0) cap << "   " << outcome.rejectedPoints << " rejected (x)";
        if (outcome.unparsedPoints > 0) cap << "   " << outcome.unparsedPoints << " unparsed";
        if (outcome.identityDisplay)    cap << "   IDENTITY DISPLAY";
        g.setColour (juce::Colour (0xff8090a0));
        g.drawText (cap, area.reduced (8.0f).removeFromBottom (18.0f)
                          .withTrimmedRight (196.0f),
                    juce::Justification::centredLeft);
    }

private:
    SweepOutcome outcome;
    juce::String title;
    juce::TextButton rejectButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveView)
};

} // namespace ejmap
