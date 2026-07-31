/*
    EedExciterEditor.cpp  —  see EedExciterEditor.h.
*/

#include "EedExciterEditor.h"
#include "EedHarmonicAnalysis.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kDefaultW = 520;

    constexpr int kVizH    = 96;
    constexpr int kMinVizH = 44;

    constexpr int kDefaultH = kTopH + 6 + kVizH + 6 + kRowH + 6 + kKnobH + 2 * kPad;

    constexpr int kGap = 14;
}

EedExciterEditor::EedExciterEditor (EedExciterProcessor& p)
    : DeviceEditorBase (p, "EXCITER", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("harmonics above the split");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        const auto* spec = EedExciterProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (id, k.getRealValue());
        };
        addAndMakeVisible (k);
    };

    // FREQ reads in kHz above 1000, the way a crossover is actually talked about.
    freqKnob_.formatValue = [] (double hz)
    {
        return hz >= 1000.0 ? juce::String (hz / 1000.0, 1) + " kHz"
                            : juce::String (hz, 0) + " Hz";
    };

    // Skewed so the useful 2-6 kHz region gets the middle of the travel instead
    // of being crammed into the last quarter of a linear sweep.
    setup (freqKnob_,   EedExciterProcessor::kFreqHz,   2500.0, 0, "",     "FREQ");
    setup (amountKnob_, EedExciterProcessor::kAmount,      0.0, 0, " %",   "AMOUNT");
    setup (focusKnob_,  EedExciterProcessor::kFocus,       0.0, 0, " %",   "FOCUS");
    setup (mixKnob_,    EedExciterProcessor::kMix,         0.0, 0, " %",   "MIX");
    setup (outKnob_,    EedExciterProcessor::kOutputDb,    0.0, 1, " dB",  "OUT");

    // The selector's items ARE the schema's choices, in the schema's order, so
    // the list a user sees and the list the model is taught cannot drift apart.
    styleCombo (modeBox_);
    if (const auto* spec = EedExciterProcessor::schema().find (EedExciterProcessor::kMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        modeBox_.setSelectedId ((int) proc_.getParamValue (EedExciterProcessor::kMode) + 1,
                                juce::dontSendNotification);
    }
    modeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedExciterProcessor::kMode, modeBox_.getSelectedId() - 1);
    };
    addAndMakeVisible (modeBox_);

    // ---- the signature visualisation --------------------------------------
    // Captioned for the BAND, not the signal: everything drawn here is about the
    // part above the split, and a user who reads these as whole-signal readouts
    // would think the device was doing nothing on bass-heavy material.
    shaper_.setCaption ("HIGH BAND");
    addAndMakeVisible (shaper_);

    bars_.setCaption ("GENERATED");
    bars_.setNumHarmonics (echojay::viz::HarmonicBars::kMaxHarmonics);   // 1..8
    bars_.setFloorDb (-60.0f);
    addAndMakeVisible (bars_);

    const auto frameSize = (std::size_t) echojay::ExciterEngine::SpectrumTap::size;
    dryFrame_.assign (frameSize, 0.0f);
    genFrame_.assign (frameSize, 0.0f);

    refreshTransfer();

    startTimerHz (15);
}

void EedExciterEditor::refreshTransfer()
{
    const int   mode   = proc_.engine().getMode();
    const float amount = proc_.engine().getAmount();

    if (mode == lastMode_ && std::abs (amount - lastAmount_) < 0.01f) return;

    lastMode_   = mode;
    lastAmount_ = amount;

    // The DSP's own arithmetic, line for line: drive the high band, shape it
    // through the mode's curve AND emphasis, scale by 1/g, and crossfade toward
    // it by amount. At amount 0 that is exactly x, so the view draws the
    // straight line the device really is.
    const auto  curve = echojay::ExciterEngine::curveForMode (mode);
    const auto  emph  = echojay::ExciterEngine::emphasisForMode (mode);
    const float a     = juce::jlimit (0.0f, 1.0f, amount * 0.01f);
    const float g     = echojay::ExciterEngine::driveForAmount (a);

    shaper_.setTransfer ([curve, emph, a, g] (float x)
    {
        const float shaped = echojay::harmonic::shapeEmphasis (curve, emph, x * g) / g;
        return x + a * (shaped - x);
    });

    // Named for the MODE, not the curve behind it: "ODD" says what it sounds
    // like, where "DIODE" would say how it is built.
    shaper_.setCurveName (juce::String (echojay::ExciterEngine::modeName (mode)).toUpperCase());
}

void EedExciterEditor::refreshBars()
{
    const double sr = proc_.getSampleRate();
    if (sr <= 0.0) return;

    const int n = proc_.engine().spectrumTap().read (dryFrame_.data(), genFrame_.data(),
                                                     (int) dryFrame_.size());
    if (n <= 0) return;

    float db[echojay::viz::HarmonicBars::kMaxHarmonics];
    for (auto& v : db) v = -60.0f;

    // The fundamental comes from the DRY side. The generated signal by
    // construction has almost nothing at the fundamental — that is what makes it
    // "the generated highs" — so asking it to find its own would either fail or
    // lock onto one of the harmonics and rescale the whole display by it.
    const auto est = echojay::harmonic::estimateFundamental (dryFrame_.data(), n, sr);

    if (est.locked)
    {
        const int usable = echojay::harmonic::wholeCycleLength (n, sr, est.hz);
        if (usable > 0)
            echojay::harmonic::harmonicMagnitudesDb (genFrame_.data(), dryFrame_.data(),
                                                     usable, sr, est.hz,
                                                     db, (int) std::size (db), -60.0f);
    }

    bars_.setMagnitudesDb (db, (int) std::size (db));
}

EedExciterEditor::~EedExciterEditor()
{
    stopTimer();
}

void EedExciterEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    auto r = content;

    // Controls first, picture second — a short rack slot loses the viz, never
    // the dials. Same shrink policy as every other device's visualisation.
    const int controlsH = kRowH + 6 + kKnobH;
    const int vizH      = juce::jmin (kVizH, juce::jmax (0, r.getHeight() - controlsH));
    const bool showViz  = vizH >= kMinVizH;

    shaper_.setVisible (showViz);
    bars_.setVisible (showViz);

    if (showViz)
    {
        auto viz = r.removeFromTop (vizH);
        if (r.getHeight() > 6) r.removeFromTop (6);

        const int barsW = juce::jlimit (70, 150, (int) ((float) viz.getWidth() * 0.42f));
        bars_.setBounds (viz.removeFromRight (barsW));
        if (viz.getWidth() > 6) viz.removeFromRight (6);
        shaper_.setBounds (viz);
    }

    auto modeRow = r.removeFromTop (juce::jmin (kRowH, r.getHeight()));
    modeBox_.setBounds (modeRow.withSizeKeepingCentre (
        juce::jmin (140, modeRow.getWidth()), modeRow.getHeight()));

    if (r.getHeight() > 6) r.removeFromTop (6);

    const int wanted = kKnobW * 5 + kGap * 4;
    auto row = r.withSizeKeepingCentre (juce::jmin (wanted, r.getWidth()),
                                        juce::jmin (kKnobH, r.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * 4) / 5);
    EchoJayDeviceKnob* dials[] = { &freqKnob_, &amountKnob_, &focusKnob_, &mixKnob_, &outKnob_ };
    for (int i = 0; i < 5; ++i)
    {
        dials[i]->setBounds (row.removeFromLeft (colW));
        if (i < 4 && row.getWidth() > kGap) row.removeFromLeft (kGap);
    }
}

void EedExciterEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; double tol; } bound[] = {
        { &freqKnob_,   EedExciterProcessor::kFreqHz,   0.5    },   // Hz: 1e-4 is noise
        { &amountKnob_, EedExciterProcessor::kAmount,   1.0e-4 },
        { &focusKnob_,  EedExciterProcessor::kFocus,    1.0e-4 },
        { &mixKnob_,    EedExciterProcessor::kMix,      1.0e-4 },
        { &outKnob_,    EedExciterProcessor::kOutputDb, 1.0e-4 },
    };

    for (auto& b : bound)
    {
        const double v = proc_.getParamValue (b.id);
        if (std::abs (v - b.knob->getRealValue()) > b.tol)
            b.knob->setRealValue (v);
    }

    const int modeId = (int) proc_.getParamValue (EedExciterProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != modeId)
        modeBox_.setSelectedId (modeId, juce::dontSendNotification);
}

void EedExciterEditor::timerCallback()
{
    syncFromProcessor();

    const bool bypassed = proc_.isBypassed();
    if (bypassButton().getToggleState() != bypassed)
        bypassButton().setToggleState (bypassed, juce::dontSendNotification);

    shaper_.setDimmed (bypassed);
    bars_.setDimmed (bypassed);

    refreshTransfer();
    shaper_.setInputLevel (proc_.engine().highBandLevel());

    if (bars_.isVisible()) refreshBars();
}
