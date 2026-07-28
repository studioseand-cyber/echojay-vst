/*
    SurgicalEqEditor.cpp  —  see SurgicalEqEditor.h.
*/

#include "SurgicalEqEditor.h"

#include <iterator>     // std::size

using echojay::BandSpec;
using echojay::BandType;

namespace
{
    constexpr int   kTopH        = 30;   // logo / global button bar
    constexpr int   kRowH        = 24;   // flat control height (combos, buttons)
    constexpr int   kCapH        = 12;   // caption band inside a dial
    constexpr int   kKnobW       = 62;   // dial column width
    constexpr int   kKnobH       = 58;   // caption + filmstrip + readout
    constexpr int   kStripH      = kRowH + 6 + kKnobH;   // selector row + dial row
    constexpr int   kPad         = 10;
    constexpr float kNodeR       = 5.0f;
    constexpr float kHitR        = 11.0f;
    constexpr float kCurveStepPx = 2.0f;

    // Gridlines. Decades are drawn brighter than the intermediate ticks.
    const float kFreqTicks[]  = { 30, 50, 100, 200, 300, 500, 1000,
                                  2000, 3000, 5000, 10000, 20000 };
    const bool  kFreqMajor[]  = { false, false, true, false, false, false, true,
                                  false, false, false, true, false };

    juce::Font uiFont (float pt, bool bold = false)
    {
        return juce::Font (juce::FontOptions (pt, bold ? juce::Font::bold
                                                       : juce::Font::plain));
    }

    juce::String freqText (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "k"
                             : juce::String ((int) hz);
    }
}

// ===========================================================================
// EqValueKnob
// ===========================================================================
EqValueKnob::EqValueKnob()
{
    addAndMakeVisible (knob_);
    knob_.onChange = [this] (float v01)
    {
        value_ = range_.convertFrom0to1 ((double) v01);
        refreshReadout();
        if (onValueChange) onValueChange();
    };

    readout_.setJustificationType (juce::Justification::centred);
    readout_.setColour (juce::Label::textColourId, EchoJayLookAndFeel::Colours::text);
    readout_.setColour (juce::Label::backgroundWhenEditingColourId,
                        EchoJayLookAndFeel::Colours::bg4);
    readout_.setColour (juce::Label::textWhenEditingColourId,
                        EchoJayLookAndFeel::Colours::text);
    // Exactness is the whole point of this EQ, so the readout stays typeable:
    // double-click it and enter the value you actually want.
    readout_.setEditable (false, true, false);
    readout_.onTextChange = [this]
    {
        const auto raw = readout_.getText().trim();
        // FREQ now DISPLAYS "1.2k", so a user editing that text must be able
        // to type it back: without this, "1.5k" would parse as 1.5 Hz and get
        // clamped to 20 — silently destroying the value they meant to nudge.
        const bool kilo = raw.containsIgnoreCase ("k");
        auto txt = raw.retainCharacters ("0123456789.-+").trim();
        double v = txt.getDoubleValue();
        if (kilo) v *= 1000.0;
        setRealValue (range_.snapToLegalValue (v));
        if (onValueChange) onValueChange();
    };
    addAndMakeVisible (readout_);
}

void EqValueKnob::setSpec (double lo, double hi, double skewMidPoint,
                           int decimals, const juce::String& suffix,
                           const juce::String& caption, double defaultValue)
{
    range_    = juce::NormalisableRange<double> (lo, hi);
    if (skewMidPoint > lo && skewMidPoint < hi)
        range_.setSkewForCentre (skewMidPoint);
    decimals_ = decimals;
    suffix_   = suffix;
    caption_  = caption;

    knob_.defaultNorm = (float) range_.convertTo0to1 (
        juce::jlimit (lo, hi, defaultValue));
    setRealValue (juce::jlimit (lo, hi, defaultValue));
}

void EqValueKnob::setRealValue (double v)
{
    value_ = juce::jlimit (range_.start, range_.end, v);
    knob_.setValue ((float) range_.convertTo0to1 (value_), false);
    refreshReadout();
}

void EqValueKnob::setDimmed (bool d)
{
    if (dimmed_ == d) return;
    dimmed_ = d;
    knob_.setEnabledLook (! d);
    readout_.setColour (juce::Label::textColourId,
                        d ? EchoJayLookAndFeel::Colours::text3
                          : EchoJayLookAndFeel::Colours::text);
    repaint();
}

void EqValueKnob::refreshReadout()
{
    // juce::String(double, 0) does NOT mean "no decimals" — at <= 0 places it
    // falls back to the shortest round-tripping form, which is what made FREQ
    // read "87.1907 Hz". Integer readouts have to be rounded explicitly.
    const juce::String txt =
        formatValue ? formatValue (value_)
      : decimals_ > 0 ? juce::String (value_, decimals_) + suffix_
                      : juce::String (juce::roundToInt (value_)) + suffix_;

    readout_.setText (txt, juce::dontSendNotification);
    knob_.tip = caption_ + ": " + txt + "  (double-click the number to type a value)";
    knob_.setTooltip (knob_.tip);   // setValue is not the only path here
}

void EqValueKnob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (kCapH);                       // caption, painted below
    readout_.setBounds (r.removeFromBottom (13));
    const int d = juce::jmin (r.getWidth(), r.getHeight());
    knob_.setBounds (r.withSizeKeepingCentre (d, d));
}

void EqValueKnob::paint (juce::Graphics& g)
{
    g.setColour (dimmed_ ? EchoJayLookAndFeel::Colours::text3.withAlpha (0.5f)
                         : EchoJayLookAndFeel::Colours::text3);
    g.setFont (uiFont (9.0f, true));
    g.drawText (caption_, 0, 0, getWidth(), kCapH, juce::Justification::centred);
}

// ---------------------------------------------------------------------------
SurgicalEqEditor::SurgicalEqEditor (SurgicalEqProcessor& p)
    : juce::AudioProcessorEditor (p), proc_ (p)
{
    setLookAndFeel (&lnf_);
    setWantsKeyboardFocus (true);

    for (int i = 0; i < kNumBands; ++i)
        model_[i] = proc_.getBand (i);

    // select the first enabled band so the strip is meaningful on open
    for (int i = 0; i < kNumBands; ++i)
        if (model_[i].enabled) { selected_ = i; break; }

    buildControls();
    updateStripVisibility();
    syncControlsFromModel();

    setSize (640, 420);
    startTimerHz (30);
}

SurgicalEqEditor::~SurgicalEqEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

// ---------------------------------------------------------------------------
// controls
// ---------------------------------------------------------------------------
void SurgicalEqEditor::buildControls()
{
    auto styleButton = [this] (juce::TextButton& b, bool toggles)
    {
        b.setClickingTogglesState (toggles);
        b.setColour (juce::TextButton::buttonColourId,   C::bg3);
        b.setColour (juce::TextButton::buttonOnColourId, C::blue);   // teal glow
        b.setColour (juce::TextButton::textColourOffId,  C::text2);
        b.setColour (juce::TextButton::textColourOnId,   C::blue2);
        addAndMakeVisible (b);
    };

    // lo, hi, skew midpoint, decimals, unit suffix, caption, default.
    // Ranges and midpoint skews are carried over unchanged from the sliders
    // these knobs replaced.
    auto styleValue = [this] (EqValueKnob& k, double lo, double hi, double skewMid,
                              int decimals, const juce::String& suffix,
                              const juce::String& caption, double dflt,
                              std::function<juce::String(double)> fmt = {})
    {
        k.formatValue = std::move (fmt);          // must precede setSpec
        k.setSpec (lo, hi, skewMid, decimals, suffix, caption, dflt);
        k.onValueChange = [this] { if (! suppressCallbacks_) pushControlsToBand(); };
        addAndMakeVisible (k);
    };

    // ---- global -----------------------------------------------------------
    styleButton (bypassBtn_, true);
    bypassBtn_.setToggleState (proc_.isBypassed(), juce::dontSendNotification);
    bypassBtn_.onClick = [this] { proc_.setBypassed (bypassBtn_.getToggleState()); repaint(); };

    styleButton (scaleBtn_, false);
    scaleBtn_.onClick = [this]
    {
        // View scale only. The gain slider deliberately keeps its full ±24
        // range: re-ranging it would clamp the widget, and the next edit to any
        // other field would then write that clamped value back to the band.
        dbRange_ = (dbRange_ > 20.0f ? 18.0f : 24.0f);
        scaleBtn_.setButtonText (juce::String ((int) dbRange_) + " dB");
        curvesDirty_ = true;
        repaint();
    };

    styleButton (analyzerBtn_, true);
    analyzerBtn_.setEnabled (false);          // wired in Step B
    analyzerBtn_.setTooltip ("Spectrum analyzer — arrives in Step B");

    // ---- band type / slope ------------------------------------------------
    typeBox_.addItem ("Bell",       1);
    typeBox_.addItem ("Low Shelf",  2);
    typeBox_.addItem ("High Shelf", 3);
    typeBox_.addItem ("Notch",      4);
    typeBox_.addItem ("High Pass",  5);
    typeBox_.addItem ("Low Pass",   6);
    typeBox_.setColour (juce::ComboBox::backgroundColourId, C::bg3);
    typeBox_.setColour (juce::ComboBox::textColourId,       C::text);
    typeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;
        pushControlsToBand();
        updateStripVisibility();     // slope/gain relevance depends on type
    };
    addAndMakeVisible (typeBox_);

    for (int i = 1; i <= 8; ++i)                       // 12..96 dB/oct
        slopeBox_.addItem (juce::String (i * 12) + " dB/oct", i);
    slopeBox_.setColour (juce::ComboBox::backgroundColourId, C::bg3);
    slopeBox_.setColour (juce::ComboBox::textColourId,       C::text);
    slopeBox_.onChange = [this] { if (! suppressCallbacks_) pushControlsToBand(); };
    addAndMakeVisible (slopeBox_);

    // ---- numeric dials ----------------------------------------------------
    // FREQ borrows the x-axis's own labelling so the readout and the gridline
    // under the node always agree: whole Hz below 1k, "1.2k" above.
    styleValue (freqS_, kMinFreq, kMaxFreq, 1000.0, 0, " Hz", "FREQ", 1000.0,
                [] (double v) { return freqText ((float) v)
                                     + (v < 1000.0 ? juce::String (" Hz")
                                                   : juce::String()); });
    styleValue (gainS_, -kMaxGain, kMaxGain, 0.0,   1, " dB", "GAIN", 0.0);
    // Q spans the engine's usable range, not the narrower wheel-drag clamp, so a
    // high-Q band set by the AI displays truthfully instead of being pulled in.
    styleValue (qS_,    kMinQ, kSliderMaxQ, 2.0,    2, "",    "Q",    1.0);

    styleValue (thrS_,   -60.0,  0.0,    0.0,   1, " dB", "THRESH",  -20.0);
    styleValue (rangeS_, -24.0,  24.0,   0.0,   1, " dB", "RANGE",    -6.0);
    styleValue (atkS_,     0.1,  200.0,  10.0,  1, " ms", "ATTACK",   10.0);
    styleValue (relS_,     5.0,  1000.0, 100.0, 0, " ms", "RELEASE", 100.0);

    // ---- per-band toggles -------------------------------------------------
    styleButton (dynBtn_, true);
    dynBtn_.onClick = [this]
    {
        if (suppressCallbacks_) return;
        pushControlsToBand();
        updateStripVisibility();
    };

    styleButton (enableBtn_, true);
    enableBtn_.onClick = [this]
    {
        if (suppressCallbacks_ || selected_ < 0) return;
        Spec s = model_[selected_];
        s.enabled = enableBtn_.getToggleState();
        commitBand (selected_, s);
    };

    styleButton (soloBtn_, true);
    soloBtn_.onClick = [this]
    {
        proc_.setSoloBand (soloBtn_.getToggleState() ? selected_ : -1);
    };
}

void SurgicalEqEditor::updateStripVisibility()
{
    const bool have = (selected_ >= 0 && selected_ < kNumBands);
    const Spec s    = have ? model_[selected_] : Spec {};

    const bool isPass = have && typeIsPass (s.type);
    const bool hasGn  = have && typeHasGain (s.type);
    const bool dyn    = have && s.dynamic;

    // With no band selected the whole strip goes away, leaving room for the
    // "double-click to add a band" hint rather than a row of dead controls.
    typeBox_.setVisible   (have);
    freqS_.setVisible     (have);
    qS_.setVisible        (have);
    enableBtn_.setVisible (have);
    soloBtn_.setVisible   (have);
    dynBtn_.setVisible    (have);

    slopeBox_.setVisible (isPass);
    gainS_.setVisible    (hasGn);
    thrS_.setVisible     (dyn);
    rangeS_.setVisible   (dyn);
    atkS_.setVisible     (dyn);
    relS_.setVisible     (dyn);

    // Dynamic is a Bell-only feature in the engine.
    dynBtn_.setEnabled (have && s.type == BandType::Bell);

    // A disabled band's dials still work (so you can set it up before
    // switching it on) but read dimmed, since nothing they do is audible yet.
    const bool live = have && s.enabled;
    for (EqValueKnob* k : { &freqS_, &gainS_, &qS_, &thrS_, &rangeS_, &atkS_, &relS_ })
        k->setDimmed (! live);

    layoutStrip();
    repaint();
}

void SurgicalEqEditor::layoutStrip()
{
    if (stripBounds_.isEmpty()) return;

    // Row 1: type / slope selectors and the per-band toggles.
    // Row 2: the dials — freq/gain/Q, plus the dynamic four when engaged.
    //
    // Nothing in row 1 carries a caption. TYPE and SLOPE used to, which pushed
    // those two down by the caption height while the buttons beside them sat
    // at the row top, so the row never read as one line. The controls say what
    // they are anyway ("Bell", "24 dB/oct"), so dropping the captions levels
    // the row and buys the graph back 12px. The dials still caption
    // themselves, drawn inside each knob.
    struct Item { juce::Component* c; int w; };

    const Item row1[] = {
        { &typeBox_,  96 }, { &slopeBox_, 92 }, { &dynBtn_, 58 },
        { &enableBtn_, 46 }, { &soloBtn_,  36 },
    };
    const Item row2[] = {
        { &freqS_, kKnobW }, { &gainS_,  kKnobW }, { &qS_,   kKnobW },
        { &thrS_,  kKnobW }, { &rangeS_, kKnobW }, { &atkS_, kKnobW },
        { &relS_,  kKnobW },
    };

    auto layoutRow = [this] (const Item* items, int count, int y, int h)
    {
        int needed = 0, shown = 0;
        for (int i = 0; i < count; ++i)
            if (items[i].c->isVisible()) { needed += items[i].w; ++shown; }
        if (shown == 0) return;

        const int gaps  = juce::jmax (0, shown - 1);
        const int avail = stripBounds_.getWidth() - gaps * 6;
        const float scale = avail > 0 && needed > avail
                              ? (float) avail / (float) needed : 1.0f;

        int x = stripBounds_.getX();
        for (int i = 0; i < count; ++i)
        {
            auto& it = items[i];
            if (! it.c->isVisible()) continue;
            const int w = juce::jmax (18, (int) std::round ((float) it.w * scale));
            it.c->setBounds (x, y, w, h);
            x += w + 6;
        }
    };

    const int y1 = stripBounds_.getY();
    layoutRow (row1, (int) std::size (row1), y1, kRowH);
    layoutRow (row2, (int) std::size (row2), y1 + kRowH + 6, kKnobH);
}

void SurgicalEqEditor::syncControlsFromModel()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    const bool have = (selected_ >= 0 && selected_ < kNumBands);
    const Spec s    = have ? model_[selected_] : Spec {};

    typeBox_.setSelectedId ((int) s.type + 1, juce::dontSendNotification);
    slopeBox_.setSelectedId (juce::jlimit (1, 8, s.slopeDbPerOct / 12),
                             juce::dontSendNotification);

    freqS_.setRealValue (s.freqHz);
    gainS_.setRealValue (juce::jlimit ((double) -kMaxGain, (double) kMaxGain,
                                       (double) s.gainDb));
    qS_.setRealValue (juce::jlimit ((double) kMinQ, (double) kSliderMaxQ, (double) s.q));

    thrS_.setRealValue   (s.thresholdDb);
    rangeS_.setRealValue (s.rangeDb);
    atkS_.setRealValue   (s.attackMs);
    relS_.setRealValue   (s.releaseMs);

    dynBtn_.setToggleState    (s.dynamic, juce::dontSendNotification);
    enableBtn_.setToggleState (s.enabled, juce::dontSendNotification);
    enableBtn_.setButtonText  (s.enabled ? "ON" : "OFF");
    soloBtn_.setToggleState   (have && proc_.getSoloBand() == selected_,
                               juce::dontSendNotification);
}

void SurgicalEqEditor::pushControlsToBand()
{
    if (selected_ < 0 || selected_ >= kNumBands) return;

    Spec s = model_[selected_];
    s.type          = (BandType) juce::jlimit (0, 5, typeBox_.getSelectedId() - 1);
    s.freqHz        = (float) freqS_.getRealValue();
    s.q             = (float) qS_.getRealValue();
    s.slopeDbPerOct = juce::jlimit (1, 8, slopeBox_.getSelectedId()) * 12;
    if (typeHasGain (s.type))
        s.gainDb = (float) gainS_.getRealValue();

    s.dynamic = dynBtn_.getToggleState() && s.type == BandType::Bell;
    if (s.dynamic)
    {
        s.thresholdDb = (float) thrS_.getRealValue();
        s.rangeDb     = (float) rangeS_.getRealValue();
        s.attackMs    = (float) atkS_.getRealValue();
        s.releaseMs   = (float) relS_.getRealValue();
    }

    commitBand (selected_, s);
}

// ---------------------------------------------------------------------------
// model helpers
// ---------------------------------------------------------------------------
bool SurgicalEqEditor::typeHasGain (BandType t) noexcept
{
    return t == BandType::Bell || t == BandType::LowShelf || t == BandType::HighShelf;
}

bool SurgicalEqEditor::typeIsPass (BandType t) noexcept
{
    return t == BandType::HighPass || t == BandType::LowPass;
}

int SurgicalEqEditor::firstFreeBand() const
{
    for (int i = 0; i < kNumBands; ++i)
        if (! model_[i].enabled) return i;
    return -1;
}

void SurgicalEqEditor::selectBand (int i)
{
    if (i == selected_) return;
    selected_ = i;
    if (soloBtn_.getToggleState())
        proc_.setSoloBand (i);
    updateStripVisibility();
    syncControlsFromModel();
    curvesDirty_ = true;
    repaint();
}

void SurgicalEqEditor::commitBand (int i, const Spec& s)
{
    if (i < 0 || i >= kNumBands) return;
    model_[i] = s;
    proc_.setBand (i, s);
    curvesDirty_ = true;
    repaint();
}

void SurgicalEqEditor::addBandAt (juce::Point<float> p)
{
    const int idx = firstFreeBand();
    if (idx < 0) return;                       // all 24 in use

    Spec s;
    s.enabled = true;
    s.type    = BandType::Bell;
    s.freqHz  = juce::jlimit (kMinFreq, kMaxFreq, xToFreq (p.x));
    s.gainDb  = juce::jlimit (-dbRange_, dbRange_, yToGain (p.y));
    s.q       = 1.0f;

    commitBand (idx, s);
    selected_ = idx;                           // select without re-entering solo logic
    if (soloBtn_.getToggleState()) proc_.setSoloBand (idx);
    updateStripVisibility();
    syncControlsFromModel();
}

void SurgicalEqEditor::disableBand (int i)
{
    if (i < 0 || i >= kNumBands) return;
    Spec s = model_[i];
    s.enabled = false;
    commitBand (i, s);
    if (selected_ == i)
    {
        if (soloBtn_.getToggleState())
        {
            soloBtn_.setToggleState (false, juce::dontSendNotification);
            proc_.setSoloBand (-1);
        }
        updateStripVisibility();
        syncControlsFromModel();
    }
}

// ---------------------------------------------------------------------------
// coordinate mapping
// ---------------------------------------------------------------------------
float SurgicalEqEditor::freqToX (float hz) const noexcept
{
    const float t = std::log10 (juce::jlimit (kMinFreq, kMaxFreq, hz) / kMinFreq)
                  / std::log10 (kMaxFreq / kMinFreq);
    return (float) graphBounds_.getX() + t * (float) graphBounds_.getWidth();
}

float SurgicalEqEditor::xToFreq (float x) const noexcept
{
    if (graphBounds_.getWidth() <= 0) return kMinFreq;
    const float t = juce::jlimit (0.0f, 1.0f,
        (x - (float) graphBounds_.getX()) / (float) graphBounds_.getWidth());
    return kMinFreq * std::pow (10.0f, t * std::log10 (kMaxFreq / kMinFreq));
}

float SurgicalEqEditor::gainToY (float db) const noexcept
{
    const float t = (db + dbRange_) / (2.0f * dbRange_);
    return (float) graphBounds_.getY() + (1.0f - t) * (float) graphBounds_.getHeight();
}

float SurgicalEqEditor::yToGain (float y) const noexcept
{
    if (graphBounds_.getHeight() <= 0) return 0.0f;
    const float t = (y - (float) graphBounds_.getY()) / (float) graphBounds_.getHeight();
    return (1.0f - t) * 2.0f * dbRange_ - dbRange_;
}

juce::Point<float> SurgicalEqEditor::nodePos (const Spec& s) const noexcept
{
    // Types with no gain sit on the 0 dB line at their corner frequency.
    const float db = typeHasGain (s.type) ? s.gainDb : 0.0f;
    return { freqToX (s.freqHz), gainToY (juce::jlimit (-dbRange_, dbRange_, db)) };
}

int SurgicalEqEditor::bandAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDist = kHitR;
    for (int i = 0; i < kNumBands; ++i)
    {
        if (! model_[i].enabled) continue;
        const float d = nodePos (model_[i]).getDistanceFrom (p);
        if (d <= bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// mouse / keyboard
// ---------------------------------------------------------------------------
void SurgicalEqEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! graphBounds_.contains (e.getPosition())) return;

    const int hit = bandAt (e.position);
    if (hit >= 0)
    {
        selectBand (hit);
        dragBand_ = hit;
    }
    grabKeyboardFocus();
}

void SurgicalEqEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand_ < 0) return;

    Spec s = model_[dragBand_];
    s.freqHz = juce::jlimit (kMinFreq, kMaxFreq, xToFreq (e.position.x));
    if (typeHasGain (s.type))
        s.gainDb = juce::jlimit (-dbRange_, dbRange_, yToGain (e.position.y));

    commitBand (dragBand_, s);

    // Mirror the drag into the dials. setRealValue never fires the change
    // callback, so this cannot loop back into pushControlsToBand.
    freqS_.setRealValue (s.freqHz);
    gainS_.setRealValue (s.gainDb);
}

void SurgicalEqEditor::mouseUp (const juce::MouseEvent&)
{
    dragBand_ = -1;
}

void SurgicalEqEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! graphBounds_.contains (e.getPosition())) return;

    const int hit = bandAt (e.position);
    if (hit >= 0) disableBand (hit);
    else          addBandAt (e.position);

    dragBand_ = -1;
}

void SurgicalEqEditor::mouseWheelMove (const juce::MouseEvent& e,
                                       const juce::MouseWheelDetails& w)
{
    if (! graphBounds_.contains (e.getPosition())) return;

    const int under  = bandAt (e.position);
    const int target = under >= 0 ? under : selected_;
    if (target < 0 || ! model_[target].enabled) return;

    Spec s = model_[target];
    s.q = juce::jlimit (kMinQ, kMaxQ, s.q * std::exp (w.deltaY * 2.0f));
    commitBand (target, s);

    if (target == selected_)
        qS_.setRealValue (s.q);
}

bool SurgicalEqEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
    {
        if (selected_ >= 0 && model_[selected_].enabled)
        {
            disableBand (selected_);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// timer
// ---------------------------------------------------------------------------
void SurgicalEqEditor::timerCallback()
{
    // Pick up changes made behind our back (an AI eq_bands apply, state restore).
    bool changed = false;
    for (int i = 0; i < kNumBands; ++i)
    {
        const Spec s = proc_.getBand (i);
        if (s != model_[i]) { model_[i] = s; changed = true; }
    }

    if (changed)
    {
        if (selected_ >= 0 && ! model_[selected_].enabled)
            selected_ = -1;
        updateStripVisibility();
        syncControlsFromModel();
        curvesDirty_ = true;
    }

    if (bypassBtn_.getToggleState() != proc_.isBypassed())
    {
        bypassBtn_.setToggleState (proc_.isBypassed(), juce::dontSendNotification);
        repaint();
    }

    if (curvesDirty_)
    {
        rebuildCurves();
        repaint();
    }
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
void SurgicalEqEditor::resized()
{
    auto r = getLocalBounds().reduced (kPad);

    topBounds_ = r.removeFromTop (kTopH);
    r.removeFromTop (6);

    stripBounds_ = r.removeFromBottom (juce::jmin (kStripH, juce::jmax (0, r.getHeight() - 60)));
    r.removeFromBottom (8);
    graphBounds_ = r;

    // global buttons, right-aligned in the top bar
    auto top = topBounds_;
    bypassBtn_.setBounds (top.removeFromRight (74).reduced (0, 3));
    top.removeFromRight (6);
    scaleBtn_.setBounds  (top.removeFromRight (58).reduced (0, 3));
    top.removeFromRight (6);
    analyzerBtn_.setBounds (top.removeFromRight (32).reduced (0, 3));

    layoutStrip();
    curvesDirty_ = true;
}

// ---------------------------------------------------------------------------
// curves
// ---------------------------------------------------------------------------
void SurgicalEqEditor::appendCurve (juce::Path& dest, int n, int step,
                                    bool closeToZeroLine) const
{
    const float yTop = (float) graphBounds_.getY() - 4.0f;
    const float yBot = (float) graphBounds_.getBottom() + 4.0f;

    for (int i = 0; i < n; ++i)
    {
        const float x = (float) graphBounds_.getX() + (float) (i * step);
        const float y = juce::jlimit (yTop, yBot, gainToY (curveMags_[(size_t) i]));
        if (i == 0)
        {
            if (closeToZeroLine) { dest.startNewSubPath (x, gainToY (0.0f)); dest.lineTo (x, y); }
            else                   dest.startNewSubPath (x, y);
        }
        else dest.lineTo (x, y);
    }

    if (closeToZeroLine && n > 0)
    {
        const float xEnd = (float) graphBounds_.getX() + (float) ((n - 1) * step);
        dest.lineTo (xEnd, gainToY (0.0f));
        dest.closeSubPath();
    }
}

void SurgicalEqEditor::rebuildCurves()
{
    curvesDirty_ = false;
    totalCurve_.clear();
    totalFill_.clear();
    bandCurve_.clear();

    if (graphBounds_.getWidth() < 4 || graphBounds_.getHeight() < 4) return;

    const int step = (int) kCurveStepPx;
    const int n    = juce::jmax (2, graphBounds_.getWidth() / step + 1);

    curveFreqs_.resize ((size_t) n);
    curveMags_.resize ((size_t) n);

    for (int i = 0; i < n; ++i)
        curveFreqs_[(size_t) i] = xToFreq ((float) graphBounds_.getX()
                                           + (float) (i * step));

    auto& eng = proc_.getEngine();

    eng.getMagnitudeResponse (curveFreqs_.data(), curveMags_.data(), n);
    appendCurve (totalCurve_, n, step, false);
    appendCurve (totalFill_,  n, step, true);

    if (selected_ >= 0 && selected_ < kNumBands && model_[selected_].enabled)
    {
        eng.getBandMagnitudeResponse (selected_, curveFreqs_.data(),
                                      curveMags_.data(), n);
        appendCurve (bandCurve_, n, step, false);
    }
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------
void SurgicalEqEditor::paintGrid (juce::Graphics& g) const
{
    const auto gb = graphBounds_.toFloat();

    // dB lines
    g.setFont (uiFont (9.0f));
    for (int db = -(int) dbRange_; db <= (int) dbRange_; db += 6)
    {
        const float y = gainToY ((float) db);
        const bool zero = (db == 0);
        g.setColour (zero ? C::border2 : C::border);
        g.fillRect (gb.getX(), y, gb.getWidth(), zero ? 1.0f : 0.5f);

        if (db != 0)
        {
            g.setColour (C::text3.withAlpha (0.7f));
            g.drawText (juce::String (db > 0 ? "+" : "") + juce::String (db),
                        (int) gb.getX() + 4, (int) y - 7, 30, 14,
                        juce::Justification::centredLeft);
        }
    }

    // frequency lines
    for (size_t i = 0; i < std::size (kFreqTicks); ++i)
    {
        const float f = kFreqTicks[i];
        if (f < kMinFreq || f > kMaxFreq) continue;
        const float x = freqToX (f);

        g.setColour (kFreqMajor[i] ? C::border2 : C::border);
        g.fillRect (x, gb.getY(), kFreqMajor[i] ? 1.0f : 0.5f, gb.getHeight());

        if (kFreqMajor[i])
        {
            g.setColour (C::text3.withAlpha (0.7f));
            g.drawText (freqText (f), (int) x - 20, (int) gb.getBottom() - 14, 40, 12,
                        juce::Justification::centred);
        }
    }
}

void SurgicalEqEditor::paintCurves (juce::Graphics& g) const
{
    const float dim = proc_.isBypassed() ? 0.3f : 1.0f;

    if (! bandCurve_.isEmpty())
    {
        g.setColour (C::text2.withAlpha (0.35f * dim));
        g.strokePath (bandCurve_, juce::PathStrokeType (1.0f));
    }

    if (! totalFill_.isEmpty())
    {
        g.setColour (C::blue.withAlpha (0.12f * dim));
        g.fillPath (totalFill_);
    }

    if (! totalCurve_.isEmpty())
    {
        g.setColour (C::blue2.withAlpha (dim));
        g.strokePath (totalCurve_, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
    }
}

void SurgicalEqEditor::paintNodes (juce::Graphics& g) const
{
    for (int i = 0; i < kNumBands; ++i)
    {
        if (! model_[i].enabled) continue;

        const Spec& s   = model_[i];
        const auto  p   = nodePos (s);
        const bool  sel = (i == selected_);
        const float r   = sel ? kNodeR + 2.0f : kNodeR;

        auto col = s.dynamic ? C::amber : (typeIsPass (s.type) ? C::purple : C::blue2);
        if (proc_.isBypassed()) col = col.withAlpha (0.35f);

        if (sel)   // vertical guide + halo on the selected node
        {
            g.setColour (col.withAlpha (0.25f));
            g.fillRect (p.x - 0.5f, (float) graphBounds_.getY(), 1.0f,
                        (float) graphBounds_.getHeight());
            g.setColour (col.withAlpha (0.20f));
            g.fillEllipse (p.x - r - 4.0f, p.y - r - 4.0f, (r + 4.0f) * 2.0f, (r + 4.0f) * 2.0f);
        }

        g.setColour (C::bg);
        g.fillEllipse (p.x - r - 1.0f, p.y - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f);
        g.setColour (col);
        g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);

        g.setColour (C::bg.withAlpha (0.9f));
        g.setFont (uiFont (8.0f, true));
        g.drawText (juce::String (i + 1), (int) (p.x - r), (int) (p.y - r),
                    (int) (r * 2.0f), (int) (r * 2.0f), juce::Justification::centred);
    }
}

void SurgicalEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);

    // ---- top bar ----------------------------------------------------------
    // The real logo asset, same one the main plugin header draws, scaled into
    // the header bar rather than a text title.
    const juce::Rectangle<float> logoBox ((float) topBounds_.getX(),
                                          (float) topBounds_.getY(),
                                          104.0f, (float) topBounds_.getHeight());
    EchoJayLookAndFeel::drawLogo (g, logoBox, 13.0f);

    const int afterLogo = (int) logoBox.getRight() + 6;
    g.setColour (C::text2);
    g.setFont (uiFont (11.0f, true));
    g.drawText ("EQ", afterLogo, topBounds_.getY(), 24, topBounds_.getHeight(),
                juce::Justification::centredLeft);

    g.setColour (C::text3);
    g.setFont (uiFont (9.0f));
    g.drawText ("double-click: add / remove band     drag: freq + gain     wheel: Q",
                afterLogo + 30, topBounds_.getY(),
                juce::jmax (0, topBounds_.getRight() - (afterLogo + 30) - 180),
                topBounds_.getHeight(), juce::Justification::centredLeft);

    // ---- graph panel ------------------------------------------------------
    const auto gb = graphBounds_.toFloat();
    g.setColour (C::bg2);
    g.fillRoundedRectangle (gb, 8.0f);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (graphBounds_);
        paintGrid (g);
        paintCurves (g);
        paintNodes (g);
    }

    g.setColour (C::border2);
    g.drawRoundedRectangle (gb.reduced (0.5f), 8.0f, 1.0f);

    if (proc_.isBypassed())
    {
        g.setColour (C::amber.withAlpha (0.85f));
        g.setFont (uiFont (11.0f, true));
        g.drawText ("BYPASSED", graphBounds_.getX(), graphBounds_.getY() + 6,
                    graphBounds_.getWidth(), 16, juce::Justification::centred);
    }

    if (selected_ < 0)
    {
        g.setColour (C::text3);
        g.setFont (uiFont (11.0f));
        g.drawText (firstFreeBand() < 0 ? "all 24 bands in use"
                                        : "double-click the graph to add a band",
                    stripBounds_, juce::Justification::centred);
    }

    // No strip captions to paint: the selector row is self-describing and each
    // dial draws its own caption and readout inside its own bounds.
}
