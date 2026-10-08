/*
    EedLimiterPanelV2.cpp - see EedLimiterPanelV2.h.
*/
#include "EedLimiterPanelV2.h"

using C = echojay::limv2::PanelColours;

namespace
{
    juce::Font panelFont (float pt, bool bold = false) { return juce::Font (juce::FontOptions (pt, bold ? juce::Font::bold : juce::Font::plain)); }
    juce::String dB1 (double v) { return v > -150.0 ? juce::String (v, 1) : juce::String ("--"); }
    constexpr float kGrScaleDb = 12.0f;        // the gain line and the GR bar: 0 .. -12 dB
    constexpr float kLevelLoDb = -36.0f, kLevelHiDb = 6.0f;   // IN / OUT bars
    constexpr float kWaveLoDb = -36.0f, kWaveHiDb = 12.0f;    // the picture's amplitude axis (centre .. edge)
    constexpr float kLufsLoDb = -36.0f, kLufsHiDb = 0.0f;     // the short-term bar
    constexpr double kSpeedSeconds[3] = { 1.0, 3.0, 10.0 };
    void styleBtn (juce::TextButton& b, bool toggles)
    {
        b.setClickingTogglesState (toggles);
        b.setColour (juce::TextButton::buttonColourId, C::bg3);
        b.setColour (juce::TextButton::buttonOnColourId, C::purple);
        b.setColour (juce::TextButton::textColourOffId, C::text2);
        b.setColour (juce::TextButton::textColourOnId, C::text);
    }
    // the first of the candidates (longest first) that fits the width in the font; "" if none - nothing is ever cut off
    juce::String fitText (const juce::Font& f, std::initializer_list<juce::String> candidates, int width)
    {
        for (const auto& c : candidates) if (juce::GlyphArrangement::getStringWidthInt (f, c) <= width) return c;
        return {};
    }
    void fittedText (juce::Graphics& g, const juce::Font& f, std::initializer_list<juce::String> candidates, juce::Rectangle<int> box, juce::Justification just)
    {
        g.setFont (f); const auto t = fitText (f, candidates, box.getWidth()); if (t.isNotEmpty()) g.drawText (t, box, just);
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
    styleBtn (lufsResetBtn_, false); lufsResetBtn_.onClick = [this] { loud_.reset(); grHoldDb_ = 0; repaint(); }; addAndMakeVisible (lufsResetBtn_);
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
    repaint();
}

void EedLimiterPanelV2::setSpeedIndex (int i)
{
    speed_ = juce::jlimit (0, 2, i);
    for (int k = 0; k < 3; ++k) speedBtn_[k]->setColour (juce::TextButton::buttonColourId, speed_ == k ? C::purple : C::bg3);
    // columns per picture = the picture's width in pixels; samples per column follow from the span
    const int cols = juce::jmax (64, pictureArea_.getWidth() - 4); tap_.setColumnSamples ((int) std::lround (kSpeedSeconds[speed_] * tap_.sampleRate() / cols));
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
    bool moved = false; inPeakDb_ -= 1.0f; outPeakDb_ -= 1.0f; grNowDb_ = juce::jmin (0.0f, grNowDb_ + 1.0f);   // bar ballistics: ~30 dB/s fall between refreshes
    for (; tapCursor_ < head; ++tapCursor_)
    {
        const auto& c = tap_.column (tapCursor_); picture_[(size_t) (pictureHead_ % kPictureCols)] = c; ++pictureHead_; moved = true;
        const float inPk = juce::jmax (std::abs (c.inMin), std::abs (c.inMax)), outPk = juce::jmax (std::abs (c.outMin), std::abs (c.outMax));
        inPeakDb_ = juce::jmax (inPeakDb_, inPk > 0 ? 20.0f * std::log10 (inPk) : -200.0f); outPeakDb_ = juce::jmax (outPeakDb_, outPk > 0 ? 20.0f * std::log10 (outPk) : -200.0f); grNowDb_ = juce::jmin (grNowDb_, c.grDb);
        grHoldDb_ = juce::jmin (grHoldDb_, c.grDb);
    }
    loud_.consume (tap_);
    if (++holdAge_ > 90) { holdAge_ = 0; grHoldDb_ *= 0.9f; }   // the GR max-hold eases back over seconds; the true-peak holds stay until RESET LUFS
    if (moved) repaint();
}

void EedLimiterPanelV2::resized()
{
    const bool sm = small();
    auto r = getLocalBounds().reduced (sm ? 6 : 8);
    headerArea_ = r.removeFromTop (sm ? 22 : 24);
    {
        auto header = headerArea_;
        bypassBtn_.setBounds (header.removeFromRight (sm ? 58 : 62).reduced (0, 2)); header.removeFromRight (5);
        truePeakBtn_.setBounds (header.removeFromRight (sm ? 58 : 62).reduced (0, 2)); header.removeFromRight (5);
        styleBox_.setBounds (header.removeFromRight (sm ? 96 : 104).reduced (0, 2));
    }
    r.removeFromTop (sm ? 4 : 6);
    // bottom-up: the ADVANCED row (when open) at the very bottom, then the main row - GAIN and CEILING up front, large,
    // the LUFS column, the ADVANCED button with the latency line - then everything left is the picture row
    if (advancedOpen_) { auto adv = r.removeFromBottom (sm ? 54 : 66); const int w = adv.getWidth() / 5; layoutDial (lookaheadDial_, lookaheadCap_, adv.removeFromLeft (w)); layoutDial (attackDial_, attackCap_, adv.removeFromLeft (w)); layoutDial (releaseDial_, releaseCap_, adv.removeFromLeft (w)); layoutDial (linkDial_, linkCap_, adv.removeFromLeft (w)); layoutDial (hpfDial_, hpfCap_, adv); r.removeFromBottom (sm ? 4 : 6); }
    auto main = r.removeFromBottom (sm ? 78 : 92);
    auto dials = main.removeFromLeft (sm ? 136 : 176); const int dw = dials.getWidth() / 2;
    layoutDial (gainDial_, gainCap_, dials.removeFromLeft (dw)); layoutDial (ceilingDial_, ceilingCap_, dials);
    main.removeFromLeft (sm ? 6 : 10);
    auto right = main.removeFromRight (sm ? 88 : 110); advBtn_.setBounds (right.removeFromTop (22).reduced (0, 1)); right.removeFromTop (4); latencyArea_ = right;
    main.removeFromRight (sm ? 6 : 10);
    lufsArea_ = main;
    r.removeFromBottom (sm ? 4 : 6);
    // the picture row: IN | picture | OUT GR - slim bars with their scales
    const int meterW = sm ? 24 : 28;
    inMeterArea_ = r.removeFromLeft (meterW); r.removeFromLeft (4);
    grMeterArea_ = r.removeFromRight (meterW); r.removeFromRight (3); outMeterArea_ = r.removeFromRight (meterW); r.removeFromRight (4);
    pictureArea_ = r;
    auto speeds = r.reduced (3).removeFromTop (14); speeds.removeFromRight (26); for (int i = 2; i >= 0; --i) { speedBtn_[i]->setBounds (speeds.removeFromRight (26)); speeds.removeFromRight (2); }
    setSpeedIndex (speed_);
}

void EedLimiterPanelV2::layoutDial (juce::Slider& s, juce::Label& cap, juce::Rectangle<int> area)
{
    cap.setBounds (area.removeFromTop (11)); s.setBounds (area);
}

void EedLimiterPanelV2::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);
    const bool sm = small();
    auto header = headerArea_;
    g.setColour (C::text); g.setFont (panelFont (sm ? 12.0f : 13.0f, true)); g.drawText ("LIMITER", header.removeFromLeft (sm ? 62 : 70), juce::Justification::centredLeft);
    const int subW = styleBox_.getX() - 8 - header.getX();
    g.setColour (C::text3);
    fittedText (g, panelFont (9.0f), { model_.truePeak ? "true peak, fixed latency" : "sample peak, fixed latency", model_.truePeak ? "true peak" : "sample peak", "" }, header.removeFromLeft (juce::jmax (0, subW)), juce::Justification::centredLeft);
    paintPicture (g, pictureArea_);
    paintLevelMeter (g, inMeterArea_, inPeakDb_, loud_.truePeakInDb(), "IN", true);
    paintLevelMeter (g, outMeterArea_, outPeakDb_, loud_.truePeakOutDb(), "OUT", false);
    paintGrMeter (g, grMeterArea_);
    paintLufs (g, lufsArea_);
    // the latency line, under ADVANCED: as long as fits
    g.setColour (C::text3);
    const juce::String smp = juce::String (model_.latencySamples), ms = model_.sampleRate > 0 ? juce::String (model_.latencySamples * 1000.0 / model_.sampleRate, 1) : juce::String ("--");
    auto lat = latencyArea_; const auto f = panelFont (sm ? 8.5f : 9.0f);
    fittedText (g, f, { "latency " + smp + " samples", "latency " + smp + " smp", "lat " + smp }, lat.removeFromTop (14), juce::Justification::centredLeft);
    fittedText (g, f, { ms + " ms, fixed, reported", ms + " ms, fixed", ms + " ms" }, lat.removeFromTop (14), juce::Justification::centredLeft);
    fittedText (g, f, { "to the host", "" }, lat.removeFromTop (14), juce::Justification::centredLeft);
}

void EedLimiterPanelV2::paintPicture (juce::Graphics& g, juce::Rectangle<int> a)
{
    if (a.isEmpty()) return;
    g.setColour (C::bg2); g.fillRoundedRectangle (a.toFloat(), 4.0f);
    g.setColour (C::border2); g.drawRoundedRectangle (a.toFloat(), 4.0f, 1.0f);
    const auto inner = a.reduced (2);
    // the amplitude axis: dB from the centre (-36) to the edges (+12), both halves; the gain line 0 at the top, -12 at the bottom
    const float midY = inner.getCentreY(), half = inner.getHeight() * 0.5f;
    auto dbToF = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db - kWaveLoDb) / (kWaveHiDb - kWaveLoDb)); };
    auto ampToY = [&] (float lin, bool up) { const float m = std::abs (lin); const float db = m > 1e-6f ? 20.0f * std::log10 (m) : kWaveLoDb; const float f = dbToF (db); return up ? midY - f * half : midY + f * half; };
    auto grToY = [&] (float grDb) { return (float) inner.getY() + juce::jlimit (0.0f, 1.0f, -grDb / kGrScaleDb) * (float) inner.getHeight(); };
    // scale lines: amplitude at 0 / -12 / -24 dB both sides (faint), the ceiling (brighter), gain at -3 / -6 / -9
    g.setColour (C::border);
    for (float db : { 0.0f, -12.0f, -24.0f }) { g.drawHorizontalLine ((int) (midY - dbToF (db) * half), (float) inner.getX(), (float) inner.getRight()); g.drawHorizontalLine ((int) (midY + dbToF (db) * half), (float) inner.getX(), (float) inner.getRight()); }
    const float cl = (float) std::pow (10.0, model_.ceilingDb / 20.0); g.setColour (C::border2); g.drawHorizontalLine ((int) ampToY (cl, true), (float) inner.getX(), (float) inner.getRight()); g.drawHorizontalLine ((int) ampToY (cl, false), (float) inner.getX(), (float) inner.getRight());
    const int cols = inner.getWidth(); const int avail = juce::jmin (cols, pictureHead_);
    // INPUT dim behind, OUTPUT bright, both halves: the column's max above the centre, its min below
    for (int pass = 0; pass < 2; ++pass)
    {
        g.setColour (pass == 0 ? C::blue.withAlpha (0.28f) : C::blue2.withAlpha (0.92f));
        for (int i = 0; i < avail; ++i)
        {
            const auto& c = picture_[(size_t) ((pictureHead_ - avail + i) % kPictureCols)]; const int x = inner.getRight() - avail + i;
            const float mx = pass == 0 ? c.inMax : c.outMax, mn = pass == 0 ? c.inMin : c.outMin;
            const float yTop = mx > 0.0f ? ampToY (mx, true) : midY, yBot = mn < 0.0f ? ampToY (mn, false) : midY;
            g.drawVerticalLine (x, yTop, juce::jmax (yBot, yTop + 1.0f));
        }
    }
    // the gain line on top
    juce::Path gr; bool started = false;
    for (int i = 0; i < avail; ++i)
    {
        const auto& c = picture_[(size_t) ((pictureHead_ - avail + i) % kPictureCols)]; const float x = (float) (inner.getRight() - avail + i), y = grToY (c.grDb);
        if (! started) { gr.startNewSubPath (x, y); started = true; } else gr.lineTo (x, y);
    }
    g.setColour (C::amber); g.strokePath (gr, juce::PathStrokeType (1.5f));
    // scales: amplitude (left, both halves) and gain (right, amber), the span at the bottom right
    const auto sf = panelFont (7.5f);
    g.setFont (sf); g.setColour (C::text3);
    for (float db : { 0.0f, -12.0f, -24.0f }) { const juce::String t = juce::String ((int) db); g.drawText (t, inner.getX() + 2, (int) (midY - dbToF (db) * half) - 5, 22, 10, juce::Justification::centredLeft); g.drawText (t, inner.getX() + 2, (int) (midY + dbToF (db) * half) - 5, 22, 10, juce::Justification::centredLeft); }
    g.setColour (C::amber.withAlpha (0.8f));
    for (float gdb : { -3.0f, -6.0f, -9.0f }) g.drawText (juce::String ((int) gdb), inner.getRight() - 20, (int) grToY (gdb) - 5, 18, 10, juce::Justification::centredRight);
    g.drawText ("GR 0", inner.getRight() - 26, inner.getY() + 1, 24, 10, juce::Justification::centredRight);   // the gain scale lives on the right, under the speed buttons' row end
    g.drawText ("-12", inner.getRight() - 20, inner.getBottom() - 11, 18, 10, juce::Justification::centredRight);
    g.setColour (C::text3); g.drawText (juce::String (kSpeedSeconds[speed_], 0) + " s", inner.getX() + 26, inner.getBottom() - 11, 30, 10, juce::Justification::centredLeft);
}

void EedLimiterPanelV2::paintLevelMeter (juce::Graphics& g, juce::Rectangle<int> a, float valueDb, float holdDb, const juce::String& caption, bool scaleOnRight)
{
    if (a.isEmpty()) return;
    auto col = a; auto cap = col.removeFromTop (12); auto num = col.removeFromBottom (12); col.removeFromTop (2); col.removeFromBottom (2);
    auto bar = col; auto scale = scaleOnRight ? bar.removeFromRight (bar.getWidth() - 9) : bar.removeFromLeft (bar.getWidth() - 9);
    auto yOf = [&] (float db) { return bar.getBottom() - (int) std::lround (juce::jlimit (0.0f, 1.0f, (db - kLevelLoDb) / (kLevelHiDb - kLevelLoDb)) * (float) bar.getHeight()); };
    g.setColour (C::bg3); g.fillRoundedRectangle (bar.toFloat(), 2.0f);
    const int y0 = yOf (0.0f), yv = yOf (valueDb);
    g.setColour (C::blue); g.fillRect (bar.getX(), juce::jmax (yv, y0), bar.getWidth(), bar.getBottom() - juce::jmax (yv, y0));   // the normal palette up to 0 dBFS
    if (yv < y0) { g.setColour (C::red); g.fillRect (bar.getX(), yv, bar.getWidth(), y0 - yv); }                                   // red only above 0
    g.setColour (C::text); g.drawHorizontalLine (yOf (holdDb), (float) bar.getX(), (float) bar.getRight());                        // the true-peak max-hold
    g.setColour (C::text3); g.setFont (panelFont (7.0f));
    for (float db : { 0.0f, -12.0f, -24.0f }) { g.drawHorizontalLine (yOf (db), (float) bar.getX(), (float) bar.getRight()); g.drawText (juce::String ((int) db), scale.withY (yOf (db) - 5).withHeight (10), scaleOnRight ? juce::Justification::centredLeft : juce::Justification::centredRight); }
    g.setFont (panelFont (8.5f, true)); g.drawText (caption, cap, juce::Justification::centred);
    g.setColour (holdDb > 0.0f ? C::red : C::text2); g.setFont (panelFont (8.5f)); g.drawText (dB1 (holdDb), num, juce::Justification::centred);
}

void EedLimiterPanelV2::paintGrMeter (juce::Graphics& g, juce::Rectangle<int> a)
{
    if (a.isEmpty()) return;
    auto col = a; auto cap = col.removeFromTop (12); auto num = col.removeFromBottom (12); col.removeFromTop (2); col.removeFromBottom (2);
    auto bar = col; auto scale = bar.removeFromRight (bar.getWidth() - 9);
    auto yOf = [&] (float grDb) { return bar.getY() + (int) std::lround (juce::jlimit (0.0f, 1.0f, -grDb / kGrScaleDb) * (float) bar.getHeight()); };
    g.setColour (C::bg3); g.fillRoundedRectangle (bar.toFloat(), 2.0f);
    g.setColour (C::amber); g.fillRect (bar.getX(), bar.getY(), bar.getWidth(), yOf (grNowDb_) - bar.getY());   // fills DOWN from 0 as reduction happens
    g.setColour (C::text); g.drawHorizontalLine (yOf (grHoldDb_), (float) bar.getX(), (float) bar.getRight());   // the max-hold
    g.setColour (C::text3); g.setFont (panelFont (7.0f));
    for (float gdb : { 0.0f, -3.0f, -6.0f, -9.0f }) { g.drawHorizontalLine (yOf (gdb), (float) bar.getX(), (float) bar.getRight()); g.drawText (juce::String ((int) gdb), scale.withY (yOf (gdb) - 5).withHeight (10), juce::Justification::centredLeft); }
    g.setFont (panelFont (8.5f, true)); g.drawText ("GR", cap, juce::Justification::centred);
    g.setColour (C::amber); g.setFont (panelFont (8.5f)); g.drawText (dB1 (grHoldDb_), num, juce::Justification::centred);
}

void EedLimiterPanelV2::paintLufs (juce::Graphics& g, juce::Rectangle<int> a)
{
    if (a.isEmpty()) return;
    const bool sm = small();
    // the short-term bar on the left with a scale, then the integrated value large, M and S small, the RESET LUFS button
    auto col = a; auto barCol = col.removeFromLeft (sm ? 24 : 28); col.removeFromLeft (4);
    {
        auto cap = barCol.removeFromTop (12); auto bar = barCol; bar.removeFromTop (2); bar.removeFromBottom (2); auto scale = bar.removeFromRight (bar.getWidth() - 9);
        auto yOf = [&] (float lufs) { return bar.getBottom() - (int) std::lround (juce::jlimit (0.0f, 1.0f, (lufs - kLufsLoDb) / (kLufsHiDb - kLufsLoDb)) * (float) bar.getHeight()); };
        g.setColour (C::bg3); g.fillRoundedRectangle (bar.toFloat(), 2.0f);
        const float st = (float) loud_.shortTerm(); g.setColour (C::green); g.fillRect (bar.getX(), yOf (st), bar.getWidth(), bar.getBottom() - yOf (st));
        g.setColour (C::text3); g.setFont (panelFont (7.0f));
        for (float l : { -9.0f, -18.0f, -27.0f }) { g.drawHorizontalLine (yOf (l), (float) bar.getX(), (float) bar.getRight()); g.drawText (juce::String ((int) l), scale.withY (yOf (l) - 5).withHeight (10), juce::Justification::centredLeft); }
        g.setFont (panelFont (8.5f, true)); g.drawText ("S", cap, juce::Justification::centred);
    }
    // the button at the bottom of the text column, everything else above it
    lufsResetBtn_.setBounds (col.removeFromBottom (sm ? 18 : 20)); col.removeFromBottom (3);
    g.setColour (C::text3); fittedText (g, panelFont (8.5f, true), { "INTEGRATED LUFS", "INTEGRATED", "LUFS" }, col.removeFromTop (12), juce::Justification::centredLeft);
    const juce::String iv = dB1 (loud_.integrated());
    auto big = col.removeFromTop (sm ? 22 : 26);
    g.setColour (C::text); fittedText (g, panelFont (sm ? 19.0f : 22.0f, true), { iv + " LUFS", iv }, big, juce::Justification::centredLeft);
    g.setColour (C::text2); fittedText (g, panelFont (sm ? 8.5f : 9.0f), { "M " + dB1 (loud_.momentary()) + "   S " + dB1 (loud_.shortTerm()), "M " + dB1 (loud_.momentary()) + " S " + dB1 (loud_.shortTerm()) }, col.removeFromTop (12), juce::Justification::centredLeft);
}
