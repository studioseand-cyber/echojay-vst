/*
    PitchRibbonView.h  —  the pitch ribbon (PITCH_CORRECTION_SPEC.md §7).

    The signature view of EchoJay Pitch, and the most legible visualisation in
    the suite for one reason: the thing being shown IS the thing being
    corrected. Two traces, and the GAP BETWEEN THEM is the correction, so
    retune speed and flex become directly visible - fast retune shows the
    traces snapping together, high flex shows them running parallel and apart.

      * horizontal: time, scrolling, ~4 seconds
      * vertical:   pitch, with SCALE DEGREES AS HORIZONTAL LINES - enabled
                    bright, disabled dim, so the key is readable at a glance
      * dim trace:  detected pitch
      * bright:     corrected pitch
      * UNVOICED FRAMES ARE GAPS, never zero. A line dropping to the floor on
        every consonant looks like a bug, and would be read as one.

    Easing reuses DwellGlow's time constant rather than inventing another, so
    the ribbon moves with the same hand as the EQ and the Key Detector's wheel.
    That work is done; this does not rewrite it.
*/

#pragma once

#include <JuceHeader.h>
#include "DwellGlow.h"

#include <array>
#include <vector>

namespace echojay::viz
{

class PitchRibbonView
{
public:
    // ~4 seconds at the editor's 30 Hz poll.
    static constexpr int   kColumns  = 120;
    static constexpr float kSpanSemitones = 24.0f;   // +/- one octave about centre

    struct Column
    {
        bool  voiced    = false;
        float detected  = 0.0f;   // MIDI note number
        float corrected = 0.0f;
        bool  correcting = false; // a target existed for this frame
    };

    void reset() noexcept
    {
        cols_.assign (kColumns, Column{});
        head_ = 0;
        centre_ = 0.0f;
        haveCentre_ = false;
    }

    // One frame. detectedMidi/correctedMidi are MIDI note numbers; voiced
    // false pushes a GAP, which is the whole point of carrying the flag
    // separately rather than encoding it as zero.
    void push (bool voiced, float detectedMidi, float correctedMidi, bool correcting) noexcept
    {
        if (cols_.size() != (size_t) kColumns) reset();

        Column c;
        c.voiced     = voiced;
        c.detected   = detectedMidi;
        c.corrected  = correcting ? correctedMidi : detectedMidi;
        c.correcting = correcting;
        cols_[(size_t) head_] = c;
        head_ = (head_ + 1) % kColumns;

        // The view follows the singer rather than being pinned to a fixed
        // range, eased at the SAME time constant as the dwell glow so it
        // drifts rather than lurching.
        if (voiced)
        {
            if (! haveCentre_) { centre_ = detectedMidi; haveCentre_ = true; }
            else
            {
                const float k = 1.0f - std::exp (-0.033f / 0.5f);   // slow follow
                centre_ += (detectedMidi - centre_) * k;
            }
        }
    }

    // degreeEnabled: 12 flags relative to keyRootPc. Drawn as horizontal lines
    // at every octave in view.
    void paint (juce::Graphics& g, juce::Rectangle<int> area,
                const std::array<bool, 12>& degreeEnabled, int keyRootPc,
                bool dimmed) const
    {
        if (area.getWidth() < 8 || area.getHeight() < 8) return;

        const auto bounds = area.toFloat();
        g.setColour (juce::Colour (0xff0A0C18));
        g.fillRoundedRectangle (bounds, 3.0f);

        if (! haveCentre_)
        {
            g.setColour (juce::Colour (0xff606078));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText ("no pitch yet", area, juce::Justification::centred);
            return;
        }

        const float lo = centre_ - kSpanSemitones * 0.5f;
        const float hi = centre_ + kSpanSemitones * 0.5f;
        auto yFor = [&] (float midi)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (midi - lo) / (hi - lo));
            return bounds.getBottom() - t * bounds.getHeight();
        };

        // ---- scale degrees as lines ------------------------------------
        // Enabled bright, disabled dim: the key is legible without reading a
        // label, which is the whole reason they are lines and not a legend.
        const int firstNote = (int) std::floor (lo);
        const int lastNote  = (int) std::ceil (hi);
        for (int n = firstNote; n <= lastNote; ++n)
        {
            const int pc  = ((n % 12) + 12) % 12;
            const int deg = ((pc - keyRootPc) % 12 + 12) % 12;
            const bool on = degreeEnabled[(size_t) deg];

            const float y = yFor ((float) n);
            g.setColour (on ? juce::Colour (0xff22d3ee).withAlpha (dimmed ? 0.10f : 0.22f)
                            : juce::Colour (0xffffffff).withAlpha (0.04f));
            g.drawHorizontalLine ((int) y, bounds.getX(), bounds.getRight());

            // The ROOT gets a little more weight, so the key has an anchor.
            if (on && deg == 0)
            {
                g.setColour (juce::Colour (0xff22d3ee).withAlpha (dimmed ? 0.16f : 0.34f));
                g.drawHorizontalLine ((int) y, bounds.getX(), bounds.getRight());
            }
        }

        // ---- the two traces --------------------------------------------
        const float colW = bounds.getWidth() / (float) kColumns;

        auto trace = [&] (bool corrected, juce::Colour colour, float thickness)
        {
            juce::Path p;
            bool open = false;
            for (int i = 0; i < kColumns; ++i)
            {
                // head_ is the NEXT slot to write, so the oldest column is
                // there and time runs left to right from it.
                const Column& c = cols_[(size_t) ((head_ + i) % kColumns)];
                const float x = bounds.getX() + (float) i * colW;

                // UNVOICED IS A GAP. Closing the subpath here is what stops the
                // trace diving to the floor on every consonant.
                if (! c.voiced) { open = false; continue; }

                const float y = yFor (corrected ? c.corrected : c.detected);
                if (! open) { p.startNewSubPath (x, y); open = true; }
                else        { p.lineTo (x, y); }
            }
            if (! p.isEmpty())
            {
                g.setColour (colour);
                g.strokePath (p, juce::PathStrokeType (thickness));
            }
        };

        // Detected DIM, corrected BRIGHT: the gap between them is the
        // correction, so the eye reads the amount of work being done.
        trace (false, juce::Colour (0xffa0a0b8).withAlpha (dimmed ? 0.25f : 0.45f), 1.0f);
        trace (true,  DwellGlow::heatColour (dimmed ? 0.35f : 0.85f), 1.6f);

        g.setColour (juce::Colour::fromFloatRGBA (1, 1, 1, 0.06f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
    }

private:
    std::vector<Column> cols_ { (size_t) kColumns };
    int   head_ = 0;
    float centre_ = 0.0f;
    bool  haveCentre_ = false;
};

} // namespace echojay::viz
