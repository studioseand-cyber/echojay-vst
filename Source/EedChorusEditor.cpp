/*
    EedChorusEditor.cpp  —  see EedChorusEditor.h.
*/

#include "EedChorusEditor.h"

#include <cmath>

namespace
{
    // Nine dials since the depth pass: they flow as a row of seven and a row of
    // two (the SPREAD/TONE pair reads as "the new pair" under the classics).
    constexpr int kDefaultW = 580;
    constexpr int kTopH     = 116;
    constexpr int kDefaultH = 150 + kTopH + 6;

    // Wide enough for "DIMENSION", the longest of the three.
    constexpr int kModeW = 108;

    constexpr int kMinTopH  = 46;

    // The scope takes the narrower share: one cycle of a sine reads fine in a
    // small box, while the comb needs width to separate its notches on a log
    // axis. Below kMinPairW there is only room for one, and the comb is dropped
    // — the scope is the view that shows the device is doing anything at all.
    constexpr int kMinPairW  = 300;
    constexpr int kScopeW    = 150;
    constexpr int kViewGap   = 6;

    // How many comb notches to draw. The comb is periodic in LINEAR frequency
    // but the axis is LOGARITHMIC, so the notches crowd together as they climb
    // and above a certain point they are closer than the view's own notch width
    // — drawing those would render a smear that claims to be detail. Stop when
    // adjacent notches are nearer than this many octaves apart.
    constexpr float kMinNotchSpacingOct = 0.22f;
}

EedChorusEditor::EedChorusEditor (EedChorusProcessor& p)
    : EedModEditorBase (p, "CHORUS", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("drifting detuned voices");

    using P = EedChorusProcessor;

    addHeaderToggle (P::kSync, "SYNC");
    addHeaderChoice (P::kMode, kModeW);

    addParamKnob (P::kRateHz,   "RATE",   2, " Hz", 2.0);
    addParamKnob (P::kDivision, "DIV",    0, "",    0.0, echojay::mod::divisionReadout);
    addParamKnob (P::kDepth,    "DEPTH",  0, " %");
    // Skewed low: the difference between 2 ms and 8 ms is the difference between
    // a flanger and a chorus, and it deserves more of the dial than 40-50 ms does.
    addParamKnob (P::kDelayMs,  "DELAY",  1, " ms", 10.0);
    addParamKnob (P::kVoices,   "VOICES", 0, "",    0.0,
                  [] (double v) { return juce::String ((int) std::lround (v)); });
    addParamKnob (P::kFeedback, "FDBK",   0, " %");
    addParamKnob (P::kMix,      "MIX",    0, " %");
    addParamKnob (P::kSpread,   "SPREAD", 0, " %");
    addParamKnob (P::kTone,     "TONE",   1, " dB");

    setRateGroup (P::kSync, P::kRateHz, P::kDivision);

    scope_.setCaption ("LFO");
    comb_.setCaption ("COMB");
    addAndMakeVisible (scope_);
    addAndMakeVisible (comb_);

    refreshExtras();
    finishSetup();
}

int EedChorusEditor::topContentHeight() const
{
    return kTopH;
}

void EedChorusEditor::layoutTopContent (juce::Rectangle<int> area)
{
    const bool room = area.getHeight() >= kMinTopH && area.getWidth() >= 80;

    scope_.setVisible (room);
    comb_.setVisible (false);
    if (! room) return;

    if (area.getWidth() >= kMinPairW)
    {
        scope_.setBounds (area.removeFromLeft (kScopeW));
        area.removeFromLeft (kViewGap);
        comb_.setBounds (area);
        comb_.setVisible (true);
    }
    else
    {
        scope_.setBounds (area);
    }
}

float EedChorusEditor::lfoPhase() const
{
    return proc_.engine().lfo().displayPhase();
}

bool EedChorusEditor::controlVisible (const char* id) const
{
    using P = EedChorusProcessor;

    // Dimension multiplies the sweep by zero: RATE, DEPTH and the sync pair
    // shape a modulation that no longer exists, so they come off the panel —
    // the same rule the Delay applies to PING-PONG in pingpong mode. Still
    // schema params, still dialable, still saved.
    if (proc_.engine().getMode() == echojay::ChorusMode::Dimension)
    {
        const juce::String s (id);
        if (s == P::kRateHz || s == P::kDivision || s == P::kDepth || s == P::kSync)
            return false;
    }
    return true;
}

void EedChorusEditor::refreshExtras()
{
    using P = EedChorusProcessor;

    const auto  mode    = proc_.engine().getMode();
    const bool  byp     = proc_.isBypassed();
    const float depth   = (float) proc_.getParamValue (P::kDepth);
    const float baseMs  = (float) proc_.getParamValue (P::kDelayMs);
    const float fb      = (float) proc_.getParamValue (P::kFeedback);
    const float mix     = (float) proc_.getParamValue (P::kMix);
    const float spread  = (float) proc_.getParamValue (P::kSpread);
    const float phase   = lfoPhase();

    // The chorus pins its LFO to a sine — a stepped waveform in a delay time is
    // a pitch jump, so ChorusEngine::prepare sets it and there is no shape param
    // to read. The scope has to say the same thing the DSP is doing — which in
    // dimension mode is NO sweep at all, so the scope draws a flat line at zero
    // rather than a wave the delay lines are not riding.
    const float sweepScale = echojay::chorusModeSpec (mode).sweepScale;

    scope_.setShape (echojay::LfoCore::kSine);
    scope_.setDepthPercent (depth * sweepScale);
    // Spread is stored in % of counter-phase: 100% == 180 degrees.
    scope_.setStereoPhaseDeg (spread * 1.8f);
    scope_.setDimmed (byp);
    scope_.setPhase (phase);

    // ---- where the comb currently sits -------------------------------------
    // The delay the DSP is reading at RIGHT NOW, from the engine's own sweep
    // rule rather than a second copy of it: base + sweepMsFor(base) * lfo, for
    // voice 0. The other voices are the same comb at a different point in the
    // cycle, so drawing one of them is drawing the comb; drawing all four would
    // be four interleaved combs and no legible picture. The mode's sweep scale
    // rides along, so dimension's comb sits still like its delay lines do.
    const float depth01 = juce::jlimit (0.0f, 1.0f, depth * 0.01f) * sweepScale;
    const float lfoV    = echojay::LfoCore::shapeAt (echojay::LfoCore::kSine, phase) * depth01;
    const float delayMs = baseMs + echojay::ChorusEngine::sweepMsFor (baseMs) * lfoV;

    float notches[echojay::viz::SweepView::kMaxNotches];
    int   n = 0;

    if (delayMs > 0.0f)
    {
        // A delay summed with its dry signal cancels at the odd harmonics of
        // 1/(2T) — that is the comb, and sweeping T is what makes it move.
        const float spacing = 1000.0f / (2.0f * delayMs);   // Hz between notches
        for (int k = 0; k < echojay::viz::SweepView::kMaxNotches; ++k)
        {
            const float f = spacing * (float) (2 * k + 1);
            if (f > echojay::viz::SweepView::kMaxFreq) break;

            // Stop once the next notch would be closer than the view can draw
            // as a separate well.
            if (k > 0 && std::log2 (f / notches[k - 1]) < kMinNotchSpacingOct) break;

            notches[n++] = f;
        }
    }

    // Indicative depth, matching SweepView's stated contract (it draws Gaussian
    // wells, not an exact response): a comb cuts deeper the more of it you hear
    // and the harder it is fed back. MIX 0 flattens it to nothing, which is the
    // honest picture of a fully dry device.
    const float mix01 = juce::jlimit (0.0f, 1.0f, mix * 0.01f);
    const float depthDb = -(5.0f + 12.0f * std::abs (fb) * 0.01f) * mix01;

    comb_.setDimmed (byp);
    comb_.setNotches (notches, n, depthDb);

    // The hint names the mode: the same dials are a very different device in
    // dimension, and the voice count can exceed the dial in ensemble.
    juce::String h;
    switch (mode)
    {
        case echojay::ChorusMode::Ensemble:
            h = "ensemble - " + juce::String (proc_.engine().effectiveVoices())
              + " voices, detuned";
            break;
        case echojay::ChorusMode::Dimension:
            h = "dimension - static wide double, no wobble";
            break;
        case echojay::ChorusMode::Classic:
        default:
            h = "drifting detuned voices";
            break;
    }
    if (h != lastHint_)
    {
        lastHint_ = h;
        setHeaderHint (h);
    }
}
