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
#include "DeviceEditorBase.h"

#include <vector>

// The EQ's labelled rotary is now the SHARED one: caption above, filmstrip knob,
// editable numeric readout below, in real units. It was extracted from here into
// EchoJayDeviceLookAndFeel so every built-in device wears the same dial
// (BUILTIN_SUITE_PLAN.md §2) — this alias keeps the EQ's own code reading as it
// did rather than renaming several hundred uses.
using EqValueKnob = echojay::device::EchoJayDeviceKnob;

// The EQ is a device editor like any other: the logo, title, BYPASS, palette and
// the inline-hosting sizing contract all come from DeviceEditorBase. What is left
// here is only what is genuinely EQ — the curve, the analyzer and the band strip.
// Refactoring the EQ onto the base is what proves the base is sufficient.
class SurgicalEqEditor : public DeviceEditorBase,
                         private juce::Timer
{
public:
    explicit SurgicalEqEditor (SurgicalEqProcessor& p);
    ~SurgicalEqEditor() override;

    // Graph interaction. Child controls in the strip handle their own mouse
    // events, so these only ever fire for the graph / background area.
    void mouseDown        (const juce::MouseEvent& e) override;
    void mouseDrag        (const juce::MouseEvent& e) override;
    void mouseUp          (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove   (const juce::MouseEvent& e,
                           const juce::MouseWheelDetails& w) override;
    bool keyPressed       (const juce::KeyPress& k) override;

protected:
    // The base owns the header and the header/content split; these are the EQ's
    // share of it.
    void layoutContent        (juce::Rectangle<int> content) override;
    void layoutHeaderTrailing (juce::Rectangle<int>& bar) override;
    void layoutHeaderLeading  (juce::Rectangle<int>& bar) override;
    void paintContent         (juce::Graphics& g) override;

private:
    using C    = echojay::device::Colours;
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

    // ---- analyzer (Step B) --------------------------------------------------
    // 4096-point, matching MeterEngine::kVisFftOrder: at 2048 the bottom two
    // octaves get only a handful of bins and the curve reads as flat steps
    // there no matter how it is drawn.
    static constexpr int   kFftOrder    = 12;
    static constexpr int   kFftSize     = 1 << kFftOrder;
    static constexpr int   kSpecBins    = kFftSize / 2;

    // ---- level calibration -------------------------------------------------
    // Magnitudes are produced exactly as MeterEngine produces them, so a given
    // signal yields the same dB numbers in both places. These are not free
    // parameters; changing one desynchronises the two.
    static constexpr float kSpecNormNumer  = 2.0f;    // MeterEngine visNorm = 2/N
    static constexpr float kSpecRawFloorDb = -120.0f; // MeterEngine's per-bin floor
    static constexpr float kSpecLerp       = 0.5f;    // kSpectrumVisLerp
    static constexpr int   kSpecMedianBins = 3;       // kSpectrumMedianBins
    static constexpr float kSpecTiltDbPerOct = 4.5f;  // kSpectrumTiltDbPerOct
    static constexpr float kSpecOctaveFrac = 12.0f;   // 1/12-octave smoothing

    // The dB->pixel mapping deliberately does NOT follow the METERS renderer.
    // That one AUTO-RANGES: its ceiling floats to the loudest tilted bin, so
    // the peak is always pinned near the top and quiet material climbs just as
    // high as loud material. That is right for a dedicated spectrum panel and
    // wrong for an overlay you are reading levels off, so this window is
    // FIXED: the same signal always lands at the same height, and quiet
    // material stays near the floor where it belongs.
    //
    // 0 .. -100 dBFS, Pro-Q's range, drawn on its OWN right-hand axis. The
    // analyzer and the EQ's gain grid measure different quantities and must
    // never be read off each other's scale.
    static constexpr float kSpecCeilDb  = 0.0f;
    static constexpr float kSpecFloorDb = -100.0f;

    void  updateSpectrum();          // timer: drain a tap, FFT, smooth
    void  rebuildSpectrumPath();     // region-aware knots + Catmull-Rom
    void  paintSpectrum (juce::Graphics& g) const;
    void  paintAnalyzerAxis (juce::Graphics& g) const;   // right-hand dBFS scale
    float specDbToY (float db) const noexcept;   // analyzer's OWN vertical scale

    // ---- dynamic metering (Step B) -----------------------------------------
    bool  updateDynamicMeters();     // returns true when a value moved visibly

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

    // The header rect lives in the base (headerBounds()); these two are the EQ's
    // own subdivision of the content area.
    juce::Rectangle<int> graphBounds_, stripBounds_;

    Spec  model_[kNumBands];          // last-known processor state (change detect)
    int   selected_ = -1;
    int   dragBand_ = -1;
    float dbRange_  = 18.0f;          // ±18 or ±24

    juce::Path       totalCurve_, totalFill_, bandCurve_;
    std::vector<float> curveFreqs_, curveMags_;
    bool  curvesDirty_ = true;

    // Analyzer state. Every buffer is sized once in the constructor and reused
    // — nothing here allocates on the timer or in paint.
    juce::dsp::FFT   fft_ { kFftOrder };
    // Hann built with MeterEngine's own formula (periodic, /N) rather than
    // JUCE's symmetric /(N-1) one, so the coherent gain — and therefore the
    // level — is identical rather than merely close.
    std::vector<float> window_;
    std::vector<float> fftScratch_;   // kFftSize  drained samples
    std::vector<float> fftData_;      // 2*kFftSize, as performFrequencyOnly… needs
    std::vector<float> specDb_;       // kSpecBins, decay-smoothed magnitudes
    std::vector<float> specWork_;     // median-gated intermediate
    std::vector<float> specDisplay_;  // fractional-octave smoothed, what is drawn
    std::vector<float> specPrefix_;   // running integral for O(n) octave averaging
    std::vector<juce::Point<float>> specPts_;   // spline knots, reused each frame
    juce::Path         spectrumPath_;
    bool               analyzerOn_   = true;
    bool               analyzerPost_ = true;    // default POST: show what the EQ did

    // Live dynamic-band gain reduction, polled on the timer (dB, <= 0 when
    // the band is pulling down).
    float dynGainDb_[kNumBands] = {};

    // global — BYPASS belongs to the base (bypassButton()).
    juce::TextButton analyzerBtn_ { "A" };
    juce::TextButton sourceBtn_   { "POST" };   // analyzer source: PRE / POST
    juce::TextButton scaleBtn_    { "18 dB" };

    // Device-global output stage, in the header rather than the per-band strip
    // because it belongs to the EQ, not to whichever band happens to be
    // selected — and the strip disappears when nothing is.
    EqValueKnob      outS_;
    juce::TextButton autoGainBtn_ { "AUTO" };
    juce::Label      autoGainLbl_;              // the makeup being applied

    // selected-band strip
    juce::ComboBox   typeBox_, slopeBox_;
    EqValueKnob      freqS_, gainS_, qS_;
    juce::TextButton dynBtn_    { "DYN" };
    juce::TextButton enableBtn_ { "ON" };
    juce::TextButton soloBtn_   { "S" };
    EqValueKnob      thrS_, rangeS_, atkS_, relS_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurgicalEqEditor)
};
