/*
    SurgicalEqEditor.h  —  the interactive curve editor for EchoJay's built-in
    surgical EQ (SURGICAL_EQ_EDITOR_SPEC.md, Step A).

    Replaces the PlaceholderEditor. Draws the EQ's exact magnitude response over
    a log-frequency / linear-dB graph and lets the user edit bands directly:
    drag a node for freq+gain, wheel for Q, double-click to add/remove, plus a
    per-band control strip (type, numeric freq/gain/Q, slope, dynamic params,
    enable/solo) and global bypass.

    Everything here runs on the message thread and talks to the processor only
    through its typed accessors (getBand/setBand/setBypassed/setSoloBand and
    EqEngine's analytic response). No APVTS, matching EchoJay house style.

    The analyzer overlay and dynamic metering are Step B; the "A" button is
    present but inert so the layout does not shift when it lands.
*/

#pragma once

#include <JuceHeader.h>
#include "SurgicalEqProcessor.h"
#include "EchoJayLookAndFeel.h"
#include "ChainWetKnob.h"

#include <vector>

// ---------------------------------------------------------------------------
// A labelled rotary in real units: caption above, filmstrip knob, editable
// numeric readout below. The knob itself IS ChainWetKnob — the same photoreal
// 128-frame filmstrip and cyan value arc as the MIX knob, so the EQ's dials
// and the wet knob are literally one control with one drawing path.
//
// The 0..1 the filmstrip understands is mapped to the band's real range by a
// juce::NormalisableRange, which reproduces exactly the ranges and midpoint
// skews the previous LinearBar sliders carried.
// ---------------------------------------------------------------------------
class EqValueKnob : public juce::Component
{
public:
    EqValueKnob();

    // decimals/suffix drive the readout: "429 Hz", "16.5 dB", "1.00".
    void setSpec (double lo, double hi, double skewMidPoint,
                  int decimals, const juce::String& suffix,
                  const juce::String& caption, double defaultValue);

    void   setRealValue (double v);          // no callback
    double getRealValue() const noexcept { return value_; }
    void   setDimmed (bool d);

    std::function<void()> onValueChange;     // fired live during a drag / on typed entry

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    // Reuses the filmstrip; overrides only what is wet/dry-specific.
    struct Rotary : ChainWetKnob
    {
        juce::String tip;
        float        defaultNorm = 0.5f;
        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            setValue (defaultNorm, true);
            if (onGestureEnd) onGestureEnd();
        }
        void updateTooltip() override { if (tip.isNotEmpty()) setTooltip (tip); }
    };

    void refreshReadout();

    Rotary      knob_;
    juce::Label readout_;                    // double-click to type an exact value
    juce::String caption_, suffix_;
    juce::NormalisableRange<double> range_ { 0.0, 1.0 };
    double value_    = 0.0;
    int    decimals_ = 1;
    bool   dimmed_   = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqValueKnob)
};

class SurgicalEqEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit SurgicalEqEditor (SurgicalEqProcessor& p);
    ~SurgicalEqEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Graph interaction. Child controls in the strip handle their own mouse
    // events, so these only ever fire for the graph / background area.
    void mouseDown        (const juce::MouseEvent& e) override;
    void mouseDrag        (const juce::MouseEvent& e) override;
    void mouseUp          (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove   (const juce::MouseEvent& e,
                           const juce::MouseWheelDetails& w) override;
    bool keyPressed       (const juce::KeyPress& k) override;

private:
    using C    = EchoJayLookAndFeel::Colours;
    using Spec = echojay::BandSpec;
    using Type = echojay::BandType;

    static constexpr float kMinFreq  = 20.0f;
    static constexpr float kMaxFreq  = 20000.0f;
    static constexpr float kMinQ     = 0.1f;
    static constexpr float kMaxQ     = 40.0f;    // wheel-drag clamp (per spec)
    static constexpr float kSliderMaxQ = 100.0f; // engine's usable Q ceiling
    static constexpr float kMaxGain  = 24.0f;    // widest view scale
    static constexpr int   kNumBands = SurgicalEqProcessor::kNumBands;

    void timerCallback() override;

    // ---- coordinate mapping (single source of truth; drawing + hit-testing) --
    float freqToX (float hz)  const noexcept;
    float xToFreq (float x)   const noexcept;
    float gainToY (float db)  const noexcept;
    float yToGain (float y)   const noexcept;
    juce::Point<float> nodePos (const Spec& s) const noexcept;

    // ---- model helpers ------------------------------------------------------
    static bool typeHasGain (Type t) noexcept;   // bell / shelves
    static bool typeIsPass  (Type t) noexcept;   // HP / LP (slope, no gain)

    int  bandAt (juce::Point<float> p) const;    // node under a point, -1 if none
    int  firstFreeBand() const;                  // lowest disabled band, -1 if full
    void selectBand (int i);
    void commitBand (int i, const Spec& s);      // -> processor, mark dirty
    void addBandAt  (juce::Point<float> p);
    void disableBand (int i);

    // ---- painting -----------------------------------------------------------
    void rebuildCurves();
    void appendCurve (juce::Path& dest, int n, int step, bool closeToZeroLine) const;
    void paintGrid   (juce::Graphics& g) const;
    void paintCurves (juce::Graphics& g) const;
    void paintNodes  (juce::Graphics& g) const;

    // ---- controls -----------------------------------------------------------
    void buildControls();
    void layoutStrip();
    void syncControlsFromModel();     // model -> widgets (no callbacks)
    void pushControlsToBand();        // widgets -> model/processor
    void updateStripVisibility();

    SurgicalEqProcessor& proc_;
    EchoJayLookAndFeel   lnf_;

    juce::Rectangle<int> topBounds_, graphBounds_, stripBounds_;

    Spec  model_[kNumBands];          // last-known processor state (change detect)
    int   selected_ = -1;
    int   dragBand_ = -1;
    float dbRange_  = 18.0f;          // ±18 or ±24

    juce::Path       totalCurve_, totalFill_, bandCurve_;
    std::vector<float> curveFreqs_, curveMags_;
    bool  curvesDirty_ = true;

    // global
    juce::TextButton bypassBtn_   { "BYPASS" };
    juce::TextButton analyzerBtn_ { "A" };      // Step B
    juce::TextButton scaleBtn_    { "18 dB" };

    // selected-band strip
    juce::ComboBox   typeBox_, slopeBox_;
    EqValueKnob      freqS_, gainS_, qS_;
    juce::TextButton dynBtn_    { "DYN" };
    juce::TextButton enableBtn_ { "ON" };
    juce::TextButton soloBtn_   { "S" };
    EqValueKnob      thrS_, rangeS_, atkS_, relS_;

    // field captions painted above the controls (built in layoutStrip)
    struct FieldLabel { juce::Rectangle<int> r; juce::String text; };
    std::vector<FieldLabel> fieldLabels_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurgicalEqEditor)
};
