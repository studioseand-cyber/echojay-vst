/*
    EedExciterEditor.cpp  —  see EedExciterEditor.h.
*/

#include "EedExciterEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kDefaultW = 380;
    constexpr int kDefaultH = 196;

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

    startTimerHz (15);
}

EedExciterEditor::~EedExciterEditor()
{
    stopTimer();
}

void EedExciterEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    auto r = content;

    auto modeRow = r.removeFromTop (juce::jmin (kRowH, r.getHeight()));
    modeBox_.setBounds (modeRow.withSizeKeepingCentre (
        juce::jmin (140, modeRow.getWidth()), modeRow.getHeight()));

    if (r.getHeight() > 6) r.removeFromTop (6);

    const int wanted = kKnobW * 4 + kGap * 3;
    auto row = r.withSizeKeepingCentre (juce::jmin (wanted, r.getWidth()),
                                        juce::jmin (kKnobH, r.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * 3) / 4);
    EchoJayDeviceKnob* dials[] = { &freqKnob_, &amountKnob_, &mixKnob_, &outKnob_ };
    for (int i = 0; i < 4; ++i)
    {
        dials[i]->setBounds (row.removeFromLeft (colW));
        if (i < 3 && row.getWidth() > kGap) row.removeFromLeft (kGap);
    }
}

void EedExciterEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; double tol; } bound[] = {
        { &freqKnob_,   EedExciterProcessor::kFreqHz,   0.5    },   // Hz: 1e-4 is noise
        { &amountKnob_, EedExciterProcessor::kAmount,   1.0e-4 },
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

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
