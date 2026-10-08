/*
    EedLimiterPanelV2.cpp - see EedLimiterPanelV2.h.
*/
#include "EedLimiterPanelV2.h"

using C = echojay::limv2::PanelColours;

namespace
{
    juce::Font panelFont (float pt, bool bold = false) { return juce::Font (juce::FontOptions (pt, bold ? juce::Font::bold : juce::Font::plain)); }
    juce::String dB1 (double v) { return v > -150.0 ? juce::String (v, 1) : juce::String ("--"); }
    constexpr float kGrScaleDb = 12.0f;
    constexpr double kSpeedSeconds[3] = { 1.0, 3.0, 10.0 };
    void styleBtn (juce::TextButton& b, bool toggles)
    {
        b.setClickingTogglesState (toggles);
        b.setColour (juce::TextButton::buttonColourId, C::bg3);
        b.setColour (juce::TextButton::buttonOnColourId, C::purple);
        b.setColour (juce::TextButton::textColourOffId, C::text2);
        b.setColour (juce::TextButton::textColourOnId, C::text);
    }
}

void EedLimiterPanelV2::Dial::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider&)
{
    const float d = (float) juce::jmin (w, h); const juce::Rectangle<float> body ((float) x + ((float) w - d) * 0.5f, (float) y + ((float) h - d) * 0.5f, d, d);
    if (strip.isValid() && strip.getHeight() >= strip.getWidth() * 2)
    {
        const int frames = strip.getHeight() / strip.getWidth(); const int frame = juce::jlimit (0, frames - 1, (int) std::round (pos * (float) (frames - 1)));
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (strip, (int) body.getX(), (int) body.getY(), (int) body.getWidth(), (int) body.getHeight(), 0, frame * strip.getWidth(), strip.getWidth(), strip.getWidth());
        return;
    }
    // vector fallback: the same 7:30 -> 4:30 travel, EchoJay teal on the dark body
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f, a = a0 + (a1 - a0) * pos;
    g.setColour (C::bg4); g.fillEllipse (body.reduced (2.0f));
    g.setColour (C::border2); g.drawEllipse (body.reduced (2.0f), 1.0f);
    juce::Path arc; arc.addCentredArc (body.getCentreX(), body.getCentreY(), d * 0.46f, d * 0.46f, 0.0f, a0, a, true);
    g.setColour (C::blue); g.strokePath (arc, juce::PathStrokeType (2.0f));
    const float r = d * 0.36f; g.drawLine (body.getCentreX() + std::sin (a) * r * 0.45f, body.getCentreY() - std::cos (a) * r * 0.45f, body.getCentreX() + std::sin (a) * r, body.getCentreY() - std::cos (a) * r, 2.0f);
}

EedLimiterPanelV2::EedLimiterPanelV2 (echojay::limv2::MeterTap& tap) : tap_ (tap)
{
    picture_.assign ((size_t) kPictureCols, {});
    setLookAndFeel (&dialLnf_);

    styleBox_.addItemList ({ "Transparent", "Punchy", "Clip" }, 1); styleBox_.setSelectedId (1, juce::dontSendNotification);
    styleBox_.setColour (juce::ComboBox::backgroundColourId, C::bg3); styleBox_.setColour (juce::ComboBox::textColourId, C::text); styleBox_.setColour (juce::ComboBox::outlineColourId, C::border2);
    styleBox_.onChange = [this] { if (! suppress_ && model_.onChange) model_.onChange ("mode", (double) (styleBox_.getSelectedId() - 1)); };
    addAndMakeVisible (styleBox_);
    styleBtn (truePeakBtn_, true); truePeakBtn_.onClick = [this] { if (! suppress_ && model_.onChange) model_.onChange ("true_peak", truePeakBtn_.getToggleState() ? 1.0 : 0.0); }; addAndMakeVisible (truePeakBtn_);
    styleBtn (bypassBtn_, true); bypassBtn_.setColour (juce::TextButton::buttonOnColourId, C::amber); bypassBtn_.onClick = [this] { if (! suppress_ && model_.onChange) model_.onChange ("bypass", bypassBtn_.getToggleState() ? 1.0 : 0.0); }; addAndMakeVisible (bypassBtn_);
    styleBtn (resetBtn_, false); resetBtn_.onClick = [this] { loud_.reset(); grHoldDb_ = 0; inHoldDb_ = outHoldDb_ = -200; repaint(); }; addAndMakeVisible (resetBtn_);
    styleBtn (advBtn_, true); advBtn_.onClick = [this] { setAdvancedOpen (advBtn_.getToggleState()); }; addAndMakeVisible (advBtn_);
    for (int i = 0; i < 3; ++i) { styleBtn (*speedBtn_[i], false); speedBtn_[i]->onClick = [this, i] { setSpeedIndex (i); }; addAndMakeVisible (*speedBtn_[i]); }

    auto dial = [this] (juce::Slider& s, juce::Label& cap, const juce::String& caption, double lo, double hi, double step, const juce::String& suffix, const char* id)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 14);
        s.setRange (lo, hi, step); s.setTextValueSuffix (suffix);
        s.setColour (juce::Slider::textBoxTextColourId, C::text); s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack); s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.onValueChange = [this, &s, id] { if (! suppress_ && model_.onChange) model_.onChange (id, s.getValue()); };
        cap.setText (caption, juce::dontSendNotification); cap.setJustificationType (juce::Justification::centred); cap.setFont (panelFont (9.0f, true)); cap.setColour (juce::Label::textColourId, C::text3);
        addAndMakeVisible (s); addAndMakeVisible (cap);
    };
    dial (gainDial_, gainCap_, "GAIN", -12.0, 12.0, 0.1, " dB", "input_db");
    dial (ceilingDial_, ceilingCap_, "CEILING", -24.0, 0.0, 0.1, " dB", "ceiling_db");
    dial (lookaheadDial_, lookaheadCap_, "LOOKAHEAD", 0.0, 5.0, 0.01, " ms", "lookahead_ms");
    dial (attackDial_, attackCap_, "ATTACK", 10.0, 2000.0, 1.0, " ms", "attack_ms");
    dial (releaseDial_, releaseCap_, "RELEASE", 1.0, 1000.0, 1.0, " ms", "release_ms");
    dial (linkDial_, linkCap_, "LINK", 0.0, 100.0, 1.0, " %", "link_pct");
    dial (hpfDial_, hpfCap_, "SC HPF", 0.0, 500.0, 1.0, " Hz", "sc_hpf_hz");
    lookaheadDial_.setSkewFactorFromMidPoint (0.5); attackDial_.setSkewFactorFromMidPoint (275.0); releaseDial_.setSkewFactorFromMidPoint (400.0);

    lufsLabel_.setFont (panelFont (10.0f)); lufsLabel_.setColour (juce::Label::textColourId, C::text); lufsLabel_.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (lufsLabel_);
    latencyLabel_.setFont (panelFont (9.0f)); latencyLabel_.setColour (juce::Label::textColourId, C::text3); latencyLabel_.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (latencyLabel_);

    setSpeedIndex (1); setAdvancedOpen (false);
    setSize (kDefaultW, kDefaultH);
    startTimerHz (30);
}

EedLimiterPanelV2::~EedLimiterPanelV2() { stopTimer(); setLookAndFeel (nullptr); }

void EedLimiterPanelV2::setKnobFilmstrip (const juce::Image& strip) { dialLnf_.strip = strip; repaint(); }

void EedLimiterPanelV2::setModel (const Model& m)
{
    model_ = m; const juce::ScopedValueSetter<bool> guard (suppress_, true);
    gainDial_.setValue (m.gainDb, juce::dontSendNotification); ceilingDial_.setValue (m.ceilingDb, juce::dontSendNotification);
    lookaheadDial_.setValue (m.lookaheadMs, juce::dontSendNotification); attackDial_.setValue (m.attackMs, juce::dontSendNotification); releaseDial_.setValue (m.releaseMs, juce::dontSendNotification);
    linkDial_.setValue (m.linkPct, juce::dontSendNotification); hpfDial_.setValue (m.scHpfHz, juce::dontSendNotification);
    styleBox_.setSelectedId (juce::jlimit (0, 2, m.style) + 1, juce::dontSendNotification); truePeakBtn_.setToggleState (m.truePeak, juce::dontSendNotification); bypassBtn_.setToggleState (m.bypassed, juce::dontSendNotification);
    juce::String lat = "latency " + juce::String (m.latencySamples) + " samples"; if (m.sampleRate > 0 && m.latencySamples > 0) lat += " (" + juce::String (m.latencySamples * 1000.0 / m.sampleRate, 1) + " ms)"; lat += " - fixed, reported to the host";
    latencyLabel_.setText (lat, juce::dontSendNotification);
    repaint();
}

void EedLimiterPanelV2::setSpeedIndex (int i)
{
    speed_ = juce::jlimit (0, 2, i);
    for (int k = 0; k < 3; ++k) speedBtn_[k]->setColour (juce::TextButton::buttonColourId, speed_ == k ? C::purple : C::bg3);
    // columns per picture = the picture's width in pixels; samples per column follow from the span
    const int cols = juce::jmax (64, pictureArea_.getWidth()); tap_.setColumnSamples ((int) std::lround (kSpeedSeconds[speed_] * tap_.sampleRate() / cols));
    repaint();
}

void EedLimiterPanelV2::setAdvancedOpen (bool open)
{
    advancedOpen_ = open; advBtn_.setToggleState (open, juce::dontSendNotification);
    for (auto* c : { &lookaheadDial_, &attackDial_, &releaseDial_, &linkDial_, &hpfDial_ }) c->setVisible (open);
    for (auto* c : { &lookaheadCap_, &attackCap_, &releaseCap_, &linkCap_, &hpfCap_ }) c->setVisible (open);
    resized();
}

void EedLimiterPanelV2::refreshNow()
{
    const int head = tap_.columnHead(); if (tapCursor_ < head - echojay::limv2::MeterTap::kColumns) tapCursor_ = head - echojay::limv2::MeterTap::kColumns;
    bool moved = false;
    for (; tapCursor_ < head; ++tapCursor_)
    {
        const auto& c = tap_.column (tapCursor_); picture_[(size_t) (pictureHead_ % kPictureCols)] = c; ++pictureHead_; moved = true;
        const float inPk = juce::jmax (std::abs (c.inMin), std::abs (c.inMax)), outPk = juce::jmax (std::abs (c.outMin), std::abs (c.outMax));
        inPeakDb_ = inPk > 0 ? 20.0f * std::log10 (inPk) : -200.0f; outPeakDb_ = outPk > 0 ? 20.0f * std::log10 (outPk) : -200.0f; grNowDb_ = c.grDb;
        grHoldDb_ = juce::jmin (grHoldDb_, c.grDb); inHoldDb_ = juce::jmax (inHoldDb_, inPeakDb_); outHoldDb_ = juce::jmax (outHoldDb_, outPeakDb_);
    }
    loud_.consume (tap_);
    if (++holdAge_ > 90) { holdAge_ = 0; grHoldDb_ *= 0.9f; }   // the GR hold eases back over seconds; the true-peak holds stay until RESET
    lufsLabel_.setText ("M " + dB1 (loud_.momentary()) + "   S " + dB1 (loud_.shortTerm()) + "   I " + dB1 (loud_.integrated()) + " LUFS", juce::dontSendNotification);
    if (moved) repaint();
}

void EedLimiterPanelV2::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto header = r.removeFromTop (24);
    bypassBtn_.setBounds (header.removeFromRight (62).reduced (0, 2)); header.removeFromRight (6);
    truePeakBtn_.setBounds (header.removeFromRight (62).reduced (0, 2)); header.removeFromRight (6);
    styleBox_.setBounds (header.removeFromRight (104).reduced (0, 2));
    r.removeFromTop (6);
    const bool small = getWidth() < 420 || getHeight() < 300;
    // bottom-up: the ADVANCED row (when open) at the very bottom, then the main row - GAIN and CEILING up front, large,
    // with the LUFS / true-peak / latency readouts and the ADVANCED and RESET buttons beside them - then the picture
    if (advancedOpen_) { auto adv = r.removeFromBottom (small ? 58 : 68); const int w = adv.getWidth() / 5; layoutDial (lookaheadDial_, lookaheadCap_, adv.removeFromLeft (w)); layoutDial (attackDial_, attackCap_, adv.removeFromLeft (w)); layoutDial (releaseDial_, releaseCap_, adv.removeFromLeft (w)); layoutDial (linkDial_, linkCap_, adv.removeFromLeft (w)); layoutDial (hpfDial_, hpfCap_, adv); r.removeFromBottom (6); }
    auto main = r.removeFromBottom (small ? 84 : 98);
    auto dials = main.removeFromLeft (small ? 150 : 190); const int dw = dials.getWidth() / 2;
    layoutDial (gainDial_, gainCap_, dials.removeFromLeft (dw)); layoutDial (ceilingDial_, ceilingCap_, dials);
    main.removeFromLeft (10);
    auto right = main.removeFromRight (small ? 64 : 76); advBtn_.setBounds (right.removeFromTop (22).reduced (0, 1)); right.removeFromTop (6); resetBtn_.setBounds (right.removeFromTop (22).reduced (0, 1));
    main.removeFromRight (8);
    lufsLabel_.setBounds (main.removeFromTop (20)); readoutArea_ = main.removeFromTop (small ? 16 : 20); latencyLabel_.setBounds (main.removeFromTop (16));
    r.removeFromBottom (6);
    // the picture row: IN meter | picture | OUT meter + GR meter
    const int meterW = small ? 34 : 44;
    inMeterArea_ = r.removeFromLeft (meterW); r.removeFromLeft (6);
    grMeterArea_ = r.removeFromRight (meterW); r.removeFromRight (4); outMeterArea_ = r.removeFromRight (meterW); r.removeFromRight (6);
    auto speeds = r.removeFromTop (16); for (int i = 2; i >= 0; --i) { speedBtn_[i]->setBounds (speeds.removeFromRight (30).reduced (1)); speeds.removeFromRight (2); }
    r.removeFromTop (2); pictureArea_ = r;
    setSpeedIndex (speed_);
}

void EedLimiterPanelV2::layoutDial (juce::Slider& s, juce::Label& cap, juce::Rectangle<int> area)
{
    cap.setBounds (area.removeFromTop (11)); s.setBounds (area);
}

void EedLimiterPanelV2::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);
    auto header = getLocalBounds().reduced (8).removeFromTop (24);
    g.setColour (C::text); g.setFont (panelFont (13.0f, true)); g.drawText ("LIMITER", header.removeFromLeft (70), juce::Justification::centredLeft);
    g.setColour (C::text3); g.setFont (panelFont (9.0f)); g.drawText (model_.truePeak ? "true peak, fixed latency" : "sample peak, fixed latency", header.removeFromLeft (150), juce::Justification::centredLeft);
    paintPicture (g, pictureArea_);
    paintMeter (g, inMeterArea_, inPeakDb_, inHoldDb_, "IN", dB1 (inHoldDb_), false);
    paintMeter (g, outMeterArea_, outPeakDb_, outHoldDb_, "OUT", dB1 (outHoldDb_), false);
    paintMeter (g, grMeterArea_, grNowDb_, grHoldDb_, "GR", dB1 (grHoldDb_), true);
    // readouts: true-peak holds from the tap's hops (dBTP), beside the LUFS line
    g.setColour (C::text2); g.setFont (panelFont (9.5f));
    g.drawText ("in " + dB1 (loud_.truePeakInDb()) + " dBTP max   out " + dB1 (loud_.truePeakOutDb()) + " dBTP max   ceiling " + juce::String (model_.ceilingDb, 1) + " dB", readoutArea_, juce::Justification::centredLeft);
}

void EedLimiterPanelV2::paintPicture (juce::Graphics& g, juce::Rectangle<int> a)
{
    if (a.isEmpty()) return;
    g.setColour (C::bg2); g.fillRoundedRectangle (a.toFloat(), 4.0f);
    g.setColour (C::border2); g.drawRoundedRectangle (a.toFloat(), 4.0f, 1.0f);
    const auto inner = a.reduced (2);
    // scales: waveforms on a dB axis from -36 dBFS (centre) to +12 dBFS (edges); the gain line 0 at the top, -12 at the bottom
    const float midY = inner.getCentreY(), half = inner.getHeight() * 0.5f;
    auto ampToY = [&] (float lin, bool up) { const float db = lin > 1e-6f ? 20.0f * std::log10 (std::abs (lin)) : -36.0f; const float f = juce::jlimit (0.0f, 1.0f, (db + 36.0f) / 48.0f); return up ? midY - f * half : midY + f * half; };
    const int cols = inner.getWidth(); const int avail = juce::jmin (cols, pictureHead_);
    // the ceiling: a line at the ceiling's level, both sides
    const float cl = (float) std::pow (10.0, model_.ceilingDb / 20.0); g.setColour (C::border2); g.drawHorizontalLine ((int) ampToY (cl, true), (float) inner.getX(), (float) inner.getRight()); g.drawHorizontalLine ((int) ampToY (cl, false), (float) inner.getX(), (float) inner.getRight());
    for (int i = 0; i < avail; ++i)
    {
        const auto& c = picture_[(size_t) ((pictureHead_ - avail + i) % kPictureCols)]; const float x = (float) (inner.getRight() - avail + i);
        g.setColour (C::blue.withAlpha (0.35f)); g.drawVerticalLine ((int) x, ampToY (c.inMax, true), ampToY (c.inMin, false));
        g.setColour (C::blue2.withAlpha (0.9f)); g.drawVerticalLine ((int) x, ampToY (c.outMax, true), ampToY (c.outMin, false));
    }
    juce::Path gr; bool started = false;
    for (int i = 0; i < avail; ++i)
    {
        const auto& c = picture_[(size_t) ((pictureHead_ - avail + i) % kPictureCols)]; const float x = (float) (inner.getRight() - avail + i);
        const float y = (float) inner.getY() + juce::jlimit (0.0f, 1.0f, -c.grDb / kGrScaleDb) * (float) inner.getHeight();
        if (! started) { gr.startNewSubPath (x, y); started = true; } else gr.lineTo (x, y);
    }
    g.setColour (C::amber); g.strokePath (gr, juce::PathStrokeType (1.5f));
    g.setColour (C::text3); g.setFont (panelFont (8.0f)); g.drawText ("GR 0", inner.getX() + 3, inner.getY() + 1, 40, 10, juce::Justification::centredLeft); g.drawText ("-12", inner.getX() + 3, inner.getBottom() - 11, 40, 10, juce::Justification::centredLeft);
    g.drawText (juce::String (kSpeedSeconds[speed_], 0) + " s", inner.getRight() - 40, inner.getBottom() - 11, 37, 10, juce::Justification::centredRight);
}

void EedLimiterPanelV2::paintMeter (juce::Graphics& g, juce::Rectangle<int> a, float valueDb, float holdDb, const juce::String& caption, const juce::String& readout, bool isGr)
{
    if (a.isEmpty()) return;
    auto bar = a; auto cap = bar.removeFromTop (12); auto num = bar.removeFromBottom (12); bar.reduce (6, 2);
    g.setColour (C::bg3); g.fillRoundedRectangle (bar.toFloat(), 2.0f);
    if (isGr)
    {   // down from the top, amber
        const float f = juce::jlimit (0.0f, 1.0f, -valueDb / kGrScaleDb), fh = juce::jlimit (0.0f, 1.0f, -holdDb / kGrScaleDb);
        g.setColour (C::amber); g.fillRect (bar.getX(), bar.getY(), bar.getWidth(), (int) (f * (float) bar.getHeight()));
        g.setColour (C::text); g.drawHorizontalLine (bar.getY() + (int) (fh * (float) bar.getHeight()), (float) bar.getX(), (float) bar.getRight());
    }
    else
    {   // up from the bottom, -36 .. 0 dBFS, teal; red above 0
        auto fOf = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db + 36.0f) / 36.0f); };
        const float f = fOf (valueDb), fh = fOf (holdDb);
        g.setColour (valueDb > 0.0f ? C::red : C::blue); g.fillRect (bar.getX(), bar.getBottom() - (int) (f * (float) bar.getHeight()), bar.getWidth(), (int) (f * (float) bar.getHeight()));
        g.setColour (C::text); g.drawHorizontalLine (bar.getBottom() - (int) (fh * (float) bar.getHeight()), (float) bar.getX(), (float) bar.getRight());
    }
    g.setColour (C::text3); g.setFont (panelFont (8.5f, true)); g.drawText (caption, cap, juce::Justification::centred);
    g.setColour (C::text2); g.setFont (panelFont (8.5f)); g.drawText (readout, num, juce::Justification::centred);
}
