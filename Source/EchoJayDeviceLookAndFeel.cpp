/*
    EchoJayDeviceLookAndFeel.cpp  —  see EchoJayDeviceLookAndFeel.h.

    Moved from SurgicalEqEditor.cpp unchanged (it was EqValueKnob there), so the
    EQ's dials look and behave exactly as before.
*/

#include "EchoJayDeviceLookAndFeel.h"

namespace echojay::device
{

using namespace metrics;

EchoJayDeviceKnob::EchoJayDeviceKnob()
{
    addAndMakeVisible (knob_);
    knob_.onChange = [this] (float v01)
    {
        value_ = range_.convertFrom0to1 ((double) v01);
        refreshReadout();
        if (onValueChange) onValueChange();
    };

    readout_.setJustificationType (juce::Justification::centred);
    readout_.setColour (juce::Label::textColourId, Colours::text);
    readout_.setColour (juce::Label::backgroundWhenEditingColourId, Colours::bg4);
    readout_.setColour (juce::Label::textWhenEditingColourId,       Colours::text);
    // Exactness is the whole point of these devices, so the readout stays
    // typeable: double-click it and enter the value you actually want.
    readout_.setEditable (false, true, false);
    readout_.onTextChange = [this]
    {
        const auto raw = readout_.getText().trim();
        // A FREQ dial DISPLAYS "1.2k", so a user editing that text must be able
        // to type it back: without this, "1.5k" would parse as 1.5 Hz and get
        // clamped to the minimum — silently destroying the value they meant to
        // nudge.
        const bool kilo = raw.containsIgnoreCase ("k");
        auto txt = raw.retainCharacters ("0123456789.-+").trim();
        double v = txt.getDoubleValue();
        if (kilo) v *= 1000.0;
        setRealValue (range_.snapToLegalValue (v));
        if (onValueChange) onValueChange();
    };
    addAndMakeVisible (readout_);
}

void EchoJayDeviceKnob::setSpec (double lo, double hi, double skewMidPoint,
                                 int decimals, const juce::String& suffix,
                                 const juce::String& caption, double defaultValue)
{
    range_ = juce::NormalisableRange<double> (lo, hi);
    if (skewMidPoint > lo && skewMidPoint < hi)
        range_.setSkewForCentre (skewMidPoint);
    decimals_ = decimals;
    suffix_   = suffix;
    caption_  = caption;

    knob_.defaultNorm = (float) range_.convertTo0to1 (juce::jlimit (lo, hi, defaultValue));
    setRealValue (juce::jlimit (lo, hi, defaultValue));
}

void EchoJayDeviceKnob::setRealValue (double v)
{
    value_ = juce::jlimit (range_.start, range_.end, v);
    knob_.setValue ((float) range_.convertTo0to1 (value_), false);
    refreshReadout();
}

void EchoJayDeviceKnob::setDimmed (bool d)
{
    if (dimmed_ == d) return;
    dimmed_ = d;
    knob_.setEnabledLook (! d);
    readout_.setColour (juce::Label::textColourId, d ? Colours::text3 : Colours::text);
    repaint();
}

void EchoJayDeviceKnob::refreshReadout()
{
    // juce::String(double, 0) does NOT mean "no decimals" — at <= 0 places it
    // falls back to the shortest round-tripping form, which is what made a FREQ
    // dial read "87.1907 Hz". Integer readouts have to be rounded explicitly.
    const juce::String txt =
        formatValue ? formatValue (value_)
      : decimals_ > 0 ? juce::String (value_, decimals_) + suffix_
                      : juce::String (juce::roundToInt (value_)) + suffix_;

    readout_.setText (txt, juce::dontSendNotification);
    knob_.tip = caption_ + ": " + txt + "  (double-click the number to type a value)";
    knob_.setTooltip (knob_.tip);   // setValue is not the only path here
}

void EchoJayDeviceKnob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (kCapH);                       // caption, painted below
    readout_.setBounds (r.removeFromBottom (13));
    const int d = juce::jmin (r.getWidth(), r.getHeight());
    knob_.setBounds (r.withSizeKeepingCentre (d, d));
}

void EchoJayDeviceKnob::paint (juce::Graphics& g)
{
    g.setColour (dimmed_ ? Colours::text3.withAlpha (0.5f) : Colours::text3);
    g.setFont (uiFont (9.0f, true));
    g.drawText (caption_, 0, 0, getWidth(), kCapH, juce::Justification::centred);
}

} // namespace echojay::device
