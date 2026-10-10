/*
    EedLimiterPanelV2.h (session L, 8-9 Oct 2026): the redesigned limiter panel - the device SHOWS what it is doing.
    Approved by Sean 8 Oct (revision 2) with five fixes applied 9 Oct, then wired into EedLimiterEditor. Copies no
    FabFilter layout, colours, graphics or wording: EchoJay palette, EchoJay filmstrip dials, EchoJay type.

      header     LIMITER  (subtitle when it fits)  <style>  TRUE PK  BYPASS   - in the plugin the shared device shell
                 (DeviceEditorBase: logo, title, BYPASS) draws this row and the editor puts the style box and TRUE PK in
                 it, so the panel runs HEADERLESS there (setHeaderless); the preview app shows the panel's own row
      picture    THE CENTREPIECE: a scrolling display over the last 1 / 3 / 10 s: the OUTPUT waveform bright, and the
                 part of the INPUT the limiter removed (where it exceeds the output) in a dark red behind it, both
                 halves; the gain line (amber) on top, 0 at the top down to -12 dB with its scale on the right; one
                 faint amplitude scale on the left
      meters     slim bars: IN (left), OUT and GR (right), each with a dB scale and its number below; IN/OUT in the
                 normal palette, red only for the part above 0 dBFS, with a true-peak max-hold (OUT's number is red
                 only when the true peak is above the ceiling by more than 0.05 dB); GR fills DOWN from 0 with a
                 max-hold line and the deepest value
      dials      GAIN (input_db) and CEILING, large; a LUFS column: short-term bar, the integrated value large, M and
                 S small, its own RESET LUFS button; the ADVANCED button, the 1s/3s/10s speed buttons and one short
                 latency line
      advanced   LOOKAHEAD, ATTACK, RELEASE, LINK, RLS LINK, SC HPF - Pro-L 2's Default Setting as the defaults
                 (0.18 ms, 275 ms, 400 ms, 75 %, 100 %)

    Nothing truncates: every piece of text is chosen from a list of shorter forms until one fits its box (fitText).

    HEADER-ONLY on purpose: the plugin's source lists (CMakeLists.txt, session A's file) need no change to carry it.

    DATA. The panel owns nothing live: it reads a limv2::MeterTap (lock-free, fed by the engine per sample; the tap
    delays IN by the latency so it sits under OUT) on a 30 Hz timer, folds the columns into its own picture ring and
    the hops into a LoudnessReader. The host (the editor in the plugin, the preview app offline) gives it a Model of
    the dial values and a callback for changes.
*/
#pragma once
#include "EJLimiterPanelPalette.h"
#include "EJLimiterMeterTap.h"
#include <functional>
#include <vector>

class EedLimiterPanelV2 : public juce::Component, private juce::Timer
{
public:
    struct Model
    {
        double gainDb = 0.0, ceilingDb = 0.0; int style = 0; bool truePeak = true, bypassed = false;
        double lookaheadMs = 0.18, attackMs = 275.0, releaseMs = 400.0, linkPct = 75.0, releaseLinkPct = 100.0, scHpfHz = 0.0; int latencySamples = 0; double sampleRate = 48000.0;
        std::function<void (const juce::String& id, double value)> onChange;   // "input_db", "ceiling_db", "mode", "true_peak", "lookahead_ms", "attack_ms", "release_ms", "link_pct", "release_link_pct", "sc_hpf_hz", "bypass"
        bool sameValues (const Model& o) const noexcept
        {
            return gainDb == o.gainDb && ceilingDb == o.ceilingDb && style == o.style && truePeak == o.truePeak && bypassed == o.bypassed && lookaheadMs == o.lookaheadMs && attackMs == o.attackMs
                && releaseMs == o.releaseMs && linkPct == o.linkPct && releaseLinkPct == o.releaseLinkPct && scHpfHz == o.scHpfHz && latencySamples == o.latencySamples && sampleRate == o.sampleRate;
        }
    };
    static constexpr int kDefaultW = 480, kDefaultH = 338, kMinW = 340, kMinH = 260;

    explicit EedLimiterPanelV2 (echojay::limv2::MeterTap& tap);
    ~EedLimiterPanelV2() override;

    void setModel (const Model& m);
    const Model& model() const noexcept { return model_; }
    void setKnobFilmstrip (const juce::Image& strip);   // 128 frames of 300x300, vertical (Assets/wet_knob_filmstrip.png); empty = vector dial
    void setSpeedIndex (int i);                         // 0 = 1 s, 1 = 3 s, 2 = 10 s across the picture
    void refreshNow();                                   // what the timer does, callable by an offline driver
    void setAdvancedOpen (bool open);
    bool isAdvancedOpen() const noexcept { return advancedOpen_; }
    void setHeaderless (bool h);                         // the plugin's device shell owns the header row
    // a dial moved by a user, by id (the test harness drives the same path a mouse drag takes)
    bool setDialValue (const juce::String& id, double value);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { refreshNow(); }
    void paintPicture (juce::Graphics&, juce::Rectangle<int>);
    void paintLevelMeter (juce::Graphics&, juce::Rectangle<int>, float peakDb, float holdDb, const juce::String& caption, bool scaleOnRight, bool holdIsOver);
    void paintGrMeter (juce::Graphics&, juce::Rectangle<int>);
    void paintLufs (juce::Graphics&, juce::Rectangle<int>);
    void layoutDial (juce::Slider&, juce::Label&, juce::Rectangle<int>);
    bool small() const { return getWidth() < 420 || getHeight() < 300; }
    juce::Slider* dialFor (const juce::String& id);

    struct Dial : public juce::LookAndFeel_V4
    {
        juce::Image strip;
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float, float, juce::Slider&) override;
    };

    echojay::limv2::MeterTap& tap_;
    echojay::limv2::LoudnessReader loud_;
    Model model_;
    Dial dialLnf_;
    juce::ComboBox styleBox_; juce::TextButton truePeakBtn_ { "TRUE PK" }, bypassBtn_ { "BYPASS" }, lufsResetBtn_ { "RESET LUFS" }, advBtn_ { "ADVANCED" };
    juce::TextButton speed1_ { "1s" }, speed3_ { "3s" }, speed10_ { "10s" }; juce::TextButton* speedBtn_[3] { &speed1_, &speed3_, &speed10_ };
    juce::Slider gainDial_, ceilingDial_, lookaheadDial_, attackDial_, releaseDial_, linkDial_, rlsLinkDial_, hpfDial_;
    juce::Label gainCap_, ceilingCap_, lookaheadCap_, attackCap_, releaseCap_, linkCap_, rlsLinkCap_, hpfCap_;
    bool advancedOpen_ = false; int speed_ = 1; bool suppress_ = false; bool headerless_ = false;

    // the picture: the last kPictureCols columns, newest last
    static constexpr int kPictureCols = 1024;
    std::vector<echojay::limv2::ColumnRecord> picture_; int pictureHead_ = 0; int tapCursor_ = 0;
    float inPeakDb_ = -200, outPeakDb_ = -200, grNowDb_ = 0, grHoldDb_ = 0; int holdAge_ = 0;
    juce::Rectangle<int> headerArea_, pictureArea_, inMeterArea_, outMeterArea_, grMeterArea_, lufsArea_, latencyArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterPanelV2)
};

// ---------------------------------------------------------------------------------------------------------------------
// implementation (header-only)
// ---------------------------------------------------------------------------------------------------------------------
namespace echojay::limv2::paneldetail
{
    using C = PanelColours;
    inline juce::Font panelFont (float pt, bool bold = false) { return juce::Font (juce::FontOptions (pt, bold ? juce::Font::bold : juce::Font::plain)); }
    inline juce::String dB1 (double v) { return v > -150.0 ? juce::String (v, 1) : juce::String ("--"); }
    constexpr float kGrScaleDb = 12.0f;        // the gain line and the GR bar: 0 .. -12 dB
    constexpr float kLevelLoDb = -36.0f, kLevelHiDb = 6.0f;   // IN / OUT bars
    constexpr float kWaveLoDb = -36.0f, kWaveHiDb = 12.0f;    // the picture's amplitude axis (centre .. edge)
    constexpr float kLufsLoDb = -36.0f, kLufsHiDb = 0.0f;     // the short-term bar
    constexpr double kSpeedSeconds[3] = { 1.0, 3.0, 10.0 };
    inline void styleBtn (juce::TextButton& b, bool toggles)
    {
        b.setClickingTogglesState (toggles);
        b.setColour (juce::TextButton::buttonColourId, C::bg3);
        b.setColour (juce::TextButton::buttonOnColourId, C::purple);
        b.setColour (juce::TextButton::textColourOffId, C::text2);
        b.setColour (juce::TextButton::textColourOnId, C::text);
    }
    // the first of the candidates (longest first) that fits the width in the font; "" if none - nothing is ever cut off
    inline juce::String fitText (const juce::Font& f, std::initializer_list<juce::String> candidates, int width)
    {
        for (const auto& c : candidates) if (juce::GlyphArrangement::getStringWidthInt (f, c) <= width) return c;
        return {};
    }
    inline void fittedText (juce::Graphics& g, const juce::Font& f, std::initializer_list<juce::String> candidates, juce::Rectangle<int> box, juce::Justification just)
    {
        g.setFont (f); const auto t = fitText (f, candidates, box.getWidth()); if (t.isNotEmpty()) g.drawText (t, box, just);
    }
}

inline void EedLimiterPanelV2::Dial::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider&)
{
    using namespace echojay::limv2::paneldetail;
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

inline EedLimiterPanelV2::EedLimiterPanelV2 (echojay::limv2::MeterTap& tap) : tap_ (tap)
{
    using namespace echojay::limv2::paneldetail;
    picture_.assign ((size_t) kPictureCols, {});
    setLookAndFeel (&dialLnf_);

    styleBox_.addItemList ({ "Transparent", "Punchy", "Clip", "Modern", "Allround" }, 1);   // item id = mode + 1, the schema's order styleBox_.setSelectedId (1, juce::dontSendNotification);
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
        s.setRange (lo, hi, step); s.setTextValueSuffix (suffix); s.setName (id);
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
    dial (rlsLinkDial_, rlsLinkCap_, "RLS LINK", 0.0, 100.0, 1.0, " %", "release_link_pct");
    dial (hpfDial_, hpfCap_, "SC HPF", 0.0, 500.0, 1.0, " Hz", "sc_hpf_hz");
    lookaheadDial_.setSkewFactorFromMidPoint (0.5); attackDial_.setSkewFactorFromMidPoint (275.0); releaseDial_.setSkewFactorFromMidPoint (400.0);

    setSpeedIndex (1); setAdvancedOpen (false);
    setSize (kDefaultW, kDefaultH);
    startTimerHz (30);
}

inline EedLimiterPanelV2::~EedLimiterPanelV2() { stopTimer(); setLookAndFeel (nullptr); }

inline void EedLimiterPanelV2::setKnobFilmstrip (const juce::Image& strip) { dialLnf_.strip = strip; repaint(); }

inline void EedLimiterPanelV2::setHeaderless (bool h)
{
    headerless_ = h; for (auto* c : { &styleBox_ }) c->setVisible (! h); for (auto* b : { &truePeakBtn_, &bypassBtn_ }) b->setVisible (! h); resized(); repaint();
}

inline juce::Slider* EedLimiterPanelV2::dialFor (const juce::String& id)
{
    for (auto* s : { &gainDial_, &ceilingDial_, &lookaheadDial_, &attackDial_, &releaseDial_, &linkDial_, &rlsLinkDial_, &hpfDial_ }) if (s->getName() == id) return s;
    return nullptr;
}

inline bool EedLimiterPanelV2::setDialValue (const juce::String& id, double value)
{
    if (auto* s = dialFor (id)) { s->setValue (value, juce::sendNotificationSync); return true; }
    return false;
}

inline void EedLimiterPanelV2::setModel (const Model& m)
{
    model_ = m; const juce::ScopedValueSetter<bool> guard (suppress_, true);
    gainDial_.setValue (m.gainDb, juce::dontSendNotification); ceilingDial_.setValue (m.ceilingDb, juce::dontSendNotification);
    lookaheadDial_.setValue (m.lookaheadMs, juce::dontSendNotification); attackDial_.setValue (m.attackMs, juce::dontSendNotification); releaseDial_.setValue (m.releaseMs, juce::dontSendNotification);
    linkDial_.setValue (m.linkPct, juce::dontSendNotification); rlsLinkDial_.setValue (m.releaseLinkPct, juce::dontSendNotification); hpfDial_.setValue (m.scHpfHz, juce::dontSendNotification);
    styleBox_.setSelectedId (juce::jlimit (0, 4, m.style) + 1, juce::dontSendNotification); truePeakBtn_.setToggleState (m.truePeak, juce::dontSendNotification); bypassBtn_.setToggleState (m.bypassed, juce::dontSendNotification);
    repaint();
}

inline void EedLimiterPanelV2::setSpeedIndex (int i)
{
    using namespace echojay::limv2::paneldetail;
    speed_ = juce::jlimit (0, 2, i);
    for (int k = 0; k < 3; ++k) speedBtn_[k]->setColour (juce::TextButton::buttonColourId, speed_ == k ? C::purple : C::bg3);
    // columns per picture = the picture's width in pixels; samples per column follow from the span
    const int cols = juce::jmax (64, pictureArea_.getWidth() - 4); tap_.setColumnSamples ((int) std::lround (kSpeedSeconds[speed_] * tap_.sampleRate() / cols));
    repaint();
}

inline void EedLimiterPanelV2::setAdvancedOpen (bool open)
{
    advancedOpen_ = open; advBtn_.setToggleState (open, juce::dontSendNotification);
    for (auto* c : { &lookaheadDial_, &attackDial_, &releaseDial_, &linkDial_, &rlsLinkDial_, &hpfDial_ }) c->setVisible (open);
    for (auto* c : { &lookaheadCap_, &attackCap_, &releaseCap_, &linkCap_, &rlsLinkCap_, &hpfCap_ }) c->setVisible (open);
    resized();
}

inline void EedLimiterPanelV2::refreshNow()
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

inline void EedLimiterPanelV2::resized()
{
    const bool sm = small();
    auto r = getLocalBounds().reduced (sm ? 6 : 8);
    if (! headerless_)
    {
        headerArea_ = r.removeFromTop (sm ? 22 : 24);
        auto header = headerArea_;
        bypassBtn_.setBounds (header.removeFromRight (sm ? 58 : 62).reduced (0, 2)); header.removeFromRight (5);
        truePeakBtn_.setBounds (header.removeFromRight (sm ? 58 : 62).reduced (0, 2)); header.removeFromRight (5);
        styleBox_.setBounds (header.removeFromRight (sm ? 96 : 104).reduced (0, 2));
        r.removeFromTop (sm ? 4 : 6);
    }
    else headerArea_ = {};
    // bottom-up: the ADVANCED row (when open) at the very bottom, then the main row - GAIN and CEILING up front, large,
    // the LUFS column, the right column (ADVANCED, the speed buttons, the latency line) - then everything left is the picture row
    if (advancedOpen_) { auto adv = r.removeFromBottom (sm ? 54 : 66); const int w = adv.getWidth() / 6; layoutDial (lookaheadDial_, lookaheadCap_, adv.removeFromLeft (w)); layoutDial (attackDial_, attackCap_, adv.removeFromLeft (w)); layoutDial (releaseDial_, releaseCap_, adv.removeFromLeft (w)); layoutDial (linkDial_, linkCap_, adv.removeFromLeft (w)); layoutDial (rlsLinkDial_, rlsLinkCap_, adv.removeFromLeft (w)); layoutDial (hpfDial_, hpfCap_, adv); r.removeFromBottom (sm ? 4 : 6); }
    auto main = r.removeFromBottom (sm ? 78 : 92);
    auto dials = main.removeFromLeft (sm ? 136 : 176); const int dw = dials.getWidth() / 2;
    layoutDial (gainDial_, gainCap_, dials.removeFromLeft (dw)); layoutDial (ceilingDial_, ceilingCap_, dials);
    main.removeFromLeft (sm ? 6 : 10);
    auto right = main.removeFromRight (sm ? 88 : 110);
    advBtn_.setBounds (right.removeFromTop (22).reduced (0, 1)); right.removeFromTop (4);
    { auto speeds = right.removeFromTop (sm ? 16 : 18); const int bw = (speeds.getWidth() - 4) / 3; for (int i = 0; i < 3; ++i) { speedBtn_[i]->setBounds (speeds.removeFromLeft (bw)); speeds.removeFromLeft (2); } }
    right.removeFromTop (4); latencyArea_ = right;
    main.removeFromRight (sm ? 6 : 10);
    lufsArea_ = main;
    r.removeFromBottom (sm ? 4 : 6);
    // the picture row: IN | picture | OUT GR - slim bars with their scales
    const int meterW = sm ? 24 : 28;
    inMeterArea_ = r.removeFromLeft (meterW); r.removeFromLeft (4);
    grMeterArea_ = r.removeFromRight (meterW); r.removeFromRight (3); outMeterArea_ = r.removeFromRight (meterW); r.removeFromRight (4);
    pictureArea_ = r;
    setSpeedIndex (speed_);
}

inline void EedLimiterPanelV2::layoutDial (juce::Slider& s, juce::Label& cap, juce::Rectangle<int> area)
{
    cap.setBounds (area.removeFromTop (11)); s.setBounds (area);
}

inline void EedLimiterPanelV2::paint (juce::Graphics& g)
{
    using namespace echojay::limv2::paneldetail;
    g.fillAll (C::bg);
    const bool sm = small();
    if (! headerless_)
    {
        auto header = headerArea_;
        g.setColour (C::text); g.setFont (panelFont (sm ? 12.0f : 13.0f, true)); g.drawText ("LIMITER", header.removeFromLeft (sm ? 62 : 70), juce::Justification::centredLeft);
        const int subW = styleBox_.getX() - 8 - header.getX();
        g.setColour (C::text3);
        fittedText (g, panelFont (9.0f), { model_.truePeak ? "true peak, fixed latency" : "sample peak, fixed latency", model_.truePeak ? "true peak" : "sample peak", "" }, header.removeFromLeft (juce::jmax (0, subW)), juce::Justification::centredLeft);
    }
    paintPicture (g, pictureArea_);
    paintLevelMeter (g, inMeterArea_, inPeakDb_, loud_.truePeakInDb(), "IN", true, loud_.truePeakInDb() > 0.0f);
    paintLevelMeter (g, outMeterArea_, outPeakDb_, loud_.truePeakOutDb(), "OUT", false, loud_.truePeakOutDb() > (float) model_.ceilingDb + 0.05f);   // red only ABOVE the ceiling by more than 0.05 dB
    paintGrMeter (g, grMeterArea_);
    paintLufs (g, lufsArea_);
    // one short latency line, under the speed buttons
    g.setColour (C::text3);
    const juce::String ms = model_.sampleRate > 0 ? juce::String (model_.latencySamples * 1000.0 / model_.sampleRate, 1) : juce::String ("--");
    fittedText (g, panelFont (sm ? 8.5f : 9.0f), { "latency " + ms + " ms (fixed)", "latency " + ms + " ms", ms + " ms" }, latencyArea_.removeFromTop (14), juce::Justification::centredLeft);
}

inline void EedLimiterPanelV2::paintPicture (juce::Graphics& g, juce::Rectangle<int> a)
{
    using namespace echojay::limv2::paneldetail;
    if (a.isEmpty()) return;
    g.setColour (C::bg2); g.fillRoundedRectangle (a.toFloat(), 4.0f);
    g.setColour (C::border2); g.drawRoundedRectangle (a.toFloat(), 4.0f, 1.0f);
    const auto inner = a.reduced (2);
    // the amplitude axis: dB from the centre (-36) to the edges (+12), both halves; the gain line 0 at the top, -12 at the bottom
    const float midY = inner.getCentreY(), half = inner.getHeight() * 0.5f;
    auto dbToF = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db - kWaveLoDb) / (kWaveHiDb - kWaveLoDb)); };
    auto ampToY = [&] (float lin, bool up) { const float m = std::abs (lin); const float db = m > 1e-6f ? 20.0f * std::log10 (m) : kWaveLoDb; const float f = dbToF (db); return up ? midY - f * half : midY + f * half; };
    auto grToY = [&] (float grDb) { return (float) inner.getY() + juce::jlimit (0.0f, 1.0f, -grDb / kGrScaleDb) * (float) inner.getHeight(); };
    // scale lines: amplitude at 0 / -12 / -24 dB both sides (faint), the ceiling (brighter)
    g.setColour (C::border);
    for (float db : { 0.0f, -12.0f, -24.0f }) { g.drawHorizontalLine ((int) (midY - dbToF (db) * half), (float) inner.getX(), (float) inner.getRight()); g.drawHorizontalLine ((int) (midY + dbToF (db) * half), (float) inner.getX(), (float) inner.getRight()); }
    const float cl = (float) std::pow (10.0, model_.ceilingDb / 20.0); g.setColour (C::border2); g.drawHorizontalLine ((int) ampToY (cl, true), (float) inner.getX(), (float) inner.getRight()); g.drawHorizontalLine ((int) ampToY (cl, false), (float) inner.getX(), (float) inner.getRight());
    const int cols = inner.getWidth(); const int avail = juce::jmin (cols, pictureHead_);
    // the INPUT first, in the dark shade (what shows of it is the part the limiter removed), then the OUTPUT bright over it
    const juce::Colour removed = C::red.darker (0.45f).withAlpha (0.9f);   // a dark red: what the limiter took off, unmistakable against the cyan
    for (int pass = 0; pass < 2; ++pass)
    {
        g.setColour (pass == 0 ? removed : C::blue2.withAlpha (0.92f));
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
    // scales: ONE faint amplitude scale (left, upper half), the gain scale (right, amber), the span at the bottom left
    const auto sf = panelFont (7.5f);
    g.setFont (sf); g.setColour (C::text3.withAlpha (0.75f));
    for (float db : { 0.0f, -12.0f, -24.0f }) g.drawText (juce::String ((int) db), inner.getX() + 2, (int) (midY - dbToF (db) * half) - 5, 22, 10, juce::Justification::centredLeft);
    g.setColour (C::amber.withAlpha (0.8f));
    for (float gdb : { 0.0f, -3.0f, -6.0f, -9.0f }) g.drawText (gdb == 0.0f ? juce::String ("GR 0") : juce::String ((int) gdb), inner.getRight() - 26, (int) grToY (gdb) - (gdb == 0.0f ? -1 : 5), 24, 10, juce::Justification::centredRight);
    g.drawText ("-12", inner.getRight() - 20, inner.getBottom() - 11, 18, 10, juce::Justification::centredRight);
    g.setColour (C::text3); g.drawText (juce::String (kSpeedSeconds[speed_], 0) + " s", inner.getX() + 2, inner.getBottom() - 11, 30, 10, juce::Justification::centredLeft);
}

inline void EedLimiterPanelV2::paintLevelMeter (juce::Graphics& g, juce::Rectangle<int> a, float valueDb, float holdDb, const juce::String& caption, bool scaleOnRight, bool holdIsOver)
{
    using namespace echojay::limv2::paneldetail;
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
    g.setColour (holdIsOver ? C::red : C::text2); g.setFont (panelFont (8.5f)); g.drawText (dB1 (holdDb), num, juce::Justification::centred);
}

inline void EedLimiterPanelV2::paintGrMeter (juce::Graphics& g, juce::Rectangle<int> a)
{
    using namespace echojay::limv2::paneldetail;
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

inline void EedLimiterPanelV2::paintLufs (juce::Graphics& g, juce::Rectangle<int> a)
{
    using namespace echojay::limv2::paneldetail;
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
