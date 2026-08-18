#pragma once
#include <JuceHeader.h>
#include "EchoJayWetKnobFilmstrip.h"

// Compact wet/dry rotary shared by the main plugin's Chain tab rack and the
// Link surface's chain panel — ONE drawing/interaction path so the two
// surfaces cannot drift (same principle as the shared spectrum renderer).
// Value is 0..1 (dry..wet). Vertical drag, double-click resets to fully wet.
// Not a host parameter: house pattern is internal state + smoothed ramp in
// the audio path (see ChainHost), UI-only control here.
//
// Rendering: photoreal filmstrip (128 frames of exactly 300x300, vertical,
// stride 300 = image height 38400 / 128) selected by value, scaled into the
// knob bounds; a thin cyan value arc rides just OUTSIDE the metal so the wet
// amount reads at a glance at both knob sizes (44px master / 22px slots).
struct ChainWetKnob : public juce::Component,
                      public juce::SettableTooltipClient
{
    std::function<void(float)> onChange;      // fired live during drag (0..1)
    std::function<void()>      onGestureEnd;  // fired on mouse-up / double-click
    juce::String caption;                     // optional tiny cap under the arc

    void setValue(float v01, bool notify = false)
    {
        v01 = juce::jlimit(0.0f, 1.0f, v01);
        if (juce::approximatelyEqual(value_, v01)) return;
        value_ = v01;
        updateTooltip();
        if (notify && onChange) onChange(value_);
        repaint();
    }
    float getValue() const noexcept { return value_; }

    ChainWetKnob() { updateTooltip(); }

    static constexpr int kFrames      = 128;
    static constexpr int kFrameSize   = 300;   // stride: 38400 / 128 = 300 exactly

    // Decoded ONCE per process (static local), shared by every knob instance.
    static const juce::Image& filmstrip()
    {
        static const juce::Image strip = juce::ImageFileFormat::loadFrom(
            wetKnobFilmstripPNG, (size_t)wetKnobFilmstripPNGSize);
        return strip;
    }

    void paint(juce::Graphics& g) override
    {
        const int capH = caption.isNotEmpty() ? 10 : 0;
        auto knob = getLocalBounds().withTrimmedBottom(capH).toFloat();
        const float d = juce::jmin(knob.getWidth(), knob.getHeight());
        auto square = knob.withSizeKeepingCentre(d, d);

        // Value arc geometry — thin cyan ring just OUTSIDE the metal, never
        // covering it: the filmstrip body is inset by the ring's thickness.
        // Thickness scales with knob size (~6.5% of diameter, min 1.5px), so
        // at 44px it's a ~3px ring and at 22px still a clearly readable
        // ~1.5px — the arc (plus its end dot) is the primary indicator; the
        // metal's own marker is too small to read at these sizes.
        const float arcT = juce::jmax(1.5f, d * 0.065f);
        auto body = square.reduced(arcT + 1.0f);   // transparent PNG: no fill behind

        const auto& strip = filmstrip();
        if (strip.isValid())
        {
            const int frame = juce::jlimit(0, kFrames - 1,
                                           (int)std::round(value_ * (kFrames - 1)));
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.setOpacity(enabled_ ? 1.0f : 0.55f);
            g.drawImage(strip,
                        juce::roundToInt(body.getX()), juce::roundToInt(body.getY()),
                        juce::roundToInt(body.getWidth()), juce::roundToInt(body.getHeight()),
                        0, frame * kFrameSize, kFrameSize, kFrameSize);
            g.setOpacity(1.0f);
        }
        else
        {
            // Decode failure fallback — flat body so the control stays usable
            g.setColour(juce::Colour(0xff0E1020));
            g.fillEllipse(body);
            g.setColour(juce::Colour::fromFloatRGBA(1, 1, 1, 0.10f));
            g.drawEllipse(body, 1.0f);
        }

        // Cyan value arc, minimum-angle position to current value (270° sweep,
        // same 7:30 → 4:30 travel as the filmstrip frames), capped by a
        // brighter end dot marking the exact value position — the dot draws
        // even at value 0 so position always reads precisely, arc-length
        // ambiguity aside.
        {
            const auto centre = square.getCentre();
            const float rad   = d * 0.5f - arcT * 0.5f;
            const float a0    = juce::MathConstants<float>::pi * 1.25f;   // 7:30
            const float a1    = juce::MathConstants<float>::pi * 2.75f;   // 4:30
            const float av    = a0 + value_ * (a1 - a0);
            const auto base   = enabled_ ? juce::Colour(0xff22d3ee)
                                         : juce::Colour(0xff606078);
            if (value_ > 0.001f)
            {
                juce::Path arc;
                arc.addCentredArc(centre.x, centre.y, rad, rad, 0, a0, av, true);
                g.setColour(base.withAlpha(0.9f));
                g.strokePath(arc, juce::PathStrokeType(arcT, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            }
            const float dotR = arcT * 1.1f;   // slightly proud of the arc
            g.setColour(base.brighter(0.6f));
            g.fillEllipse(centre.x + std::sin(av) * rad - dotR,
                          centre.y - std::cos(av) * rad - dotR,
                          dotR * 2.0f, dotR * 2.0f);
        }

        if (capH > 0)
        {
            g.setColour(juce::Colour(0xffa0a0b8));
            g.setFont(juce::Font(juce::FontOptions(7.5f, juce::Font::bold)));
            g.drawText(caption, 0, getHeight() - capH, getWidth(), capH,
                       juce::Justification::centred);
        }
    }

    void setEnabledLook(bool on) { enabled_ = on; repaint(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click / ctrl-click: a small menu the owner fills (the master
        // knob offers "Reset level tally"), no drag begins
        if (e.mods.isPopupMenu()) { if (onPopup) onPopup(); return; }
        dragStartValue_ = value_;
        dragStartY_     = e.position.y;
    }

    std::function<void()> onPopup;   // right-click, optional
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        // 150 px of vertical travel = full range — same feel at any knob size
        const float delta = (dragStartY_ - e.position.y) / 150.0f;
        setValue(dragStartValue_ + delta, true);
    }

    void mouseUp(const juce::MouseEvent&) override
    { if (onGestureEnd) onGestureEnd(); }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        setValue(1.0f, true);
        if (onGestureEnd) onGestureEnd();
    }

protected:
    // Virtual so a subclass mapping this knob onto some OTHER quantity (the
    // surgical EQ's freq/gain/Q dials reuse the same filmstrip) can say what
    // it actually controls instead of claiming to be a wet/dry mix. Called
    // from the base constructor too, where it necessarily resolves to this
    // base implementation — harmless, since every later setValue re-runs it.
    virtual void updateTooltip()
    {
        setTooltip("Wet/dry mix: " + juce::String(juce::roundToInt(value_ * 100.0f))
                   + "% wet (double-click = 100%)");
    }

protected:
    float value_          = 1.0f;
    float dragStartValue_ = 1.0f;
    float dragStartY_     = 0.0f;
    bool  enabled_        = true;
};

// ---------------------------------------------------------------------------
// PreGainKnob: the rack-head pre-chain gain control (18 Aug 2026). Reuses
// ChainWetKnob's filmstrip and drag, remapped onto -24..+24 dB. The dB figure
// is drawn ALWAYS (not on hover), amber when the value was set by hand (a
// build will not overwrite it) and grey when auto, "--" when no input level
// has been heard rather than a confident 0. Shift = fine; double-click resets
// to auto (onResetAuto). onChange still fires 0..1; the owner maps to dB.
// ---------------------------------------------------------------------------
struct PreGainKnob : ChainWetKnob
{
    static constexpr float kLoDb = -24.0f, kHiDb = 24.0f, kSpanDb = 48.0f;
    bool userSet = false;      // set by the owner from the state / sidecar
    bool inputKnown = true;    // false = no level heard: show "--"
    std::function<void()> onResetAuto;   // double-click

    static float dbFromV(float v)  { return kLoDb + v * kSpanDb; }
    static float vFromDb(float db) { return juce::jlimit(0.0f, 1.0f, (db - kLoDb) / kSpanDb); }
    float db() const { return dbFromV(getValue()); }
    void  setDb(float db, bool notify = false) { setValue(vFromDb(db), notify); }

    void paint(juce::Graphics& g) override
    {
        // Draw the knob without the base caption, then our own dB label.
        const auto savedCap = caption;
        caption = {};
        ChainWetKnob::paint(g);
        caption = savedCap;

        const int capH = 10;
        auto r = getLocalBounds().removeFromBottom(capH);
        juce::String txt;
        juce::Colour col;
        const bool zeroish = std::abs(db()) < 0.05f;
        if (! inputKnown && zeroish) { txt = "--";                col = juce::Colour(0xff8a94a6); }
        else                          { txt = juce::String(db(), 1);
                                        col = userSet ? juce::Colour(0xfff59e0b)   // amber: hand-set
                                                      : juce::Colour(0xffa0a0b8); } // grey: auto
        g.setColour(col);
        g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
        g.drawText(txt, r, juce::Justification::centred);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        // 150 px = full range, same feel as the base; shift = 4x finer.
        const float travel = e.mods.isShiftDown() ? 600.0f : 150.0f;
        const float delta = (dragStartY_ - e.position.y) / travel;
        setValue(dragStartValue_ + delta, true);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onResetAuto) onResetAuto();
    }

protected:
    void updateTooltip() override
    {
        setTooltip("Pre-gain " + juce::String(db(), 1) + " dB into the rack"
                   + (userSet ? " (set by hand, builds will not change it)" : " (auto)")
                   + " (drag; shift = fine; double-click = auto)");
    }
};
