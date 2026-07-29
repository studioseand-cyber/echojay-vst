/*
    EedSaturationEditor.cpp  —  see EedSaturationEditor.h.
*/

#include "EedSaturationEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // The size the rack opens at. layoutContent must still survive being given
    // less than this — that is the inline-hosting contract.
    constexpr int kDefaultW = 380;
    constexpr int kDefaultH = 196;

    constexpr int kGap = 14;
}

EedSaturationEditor::EedSaturationEditor (EedSaturationProcessor& p)
    : DeviceEditorBase (p, "SATURATION", kDefaultW, kDefaultH), proc_ (p)
{
    setHeaderHint ("oversampled waveshaping");

    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening one
        // without the other becomes impossible.
        const auto* spec = EedSaturationProcessor::schema().find (id);
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

    setup (driveKnob_, EedSaturationProcessor::kDriveDb,  0.0, 1, " dB", "DRIVE");
    setup (toneKnob_,  EedSaturationProcessor::kToneDb,   0.0, 1, " dB", "TONE");
    setup (mixKnob_,   EedSaturationProcessor::kMix,      0.0, 0, " %",  "MIX");
    setup (outKnob_,   EedSaturationProcessor::kOutputDb, 0.0, 1, " dB", "OUT");

    // The selector's items ARE the schema's choices, in the schema's order, so
    // the list a user sees and the list the model is taught cannot drift apart.
    styleCombo (typeBox_);
    if (const auto* spec = EedSaturationProcessor::schema().find (EedSaturationProcessor::kType))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            typeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        typeBox_.setSelectedId ((int) proc_.getParamValue (EedSaturationProcessor::kType) + 1,
                                juce::dontSendNotification);
    }
    typeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedSaturationProcessor::kType, typeBox_.getSelectedId() - 1);
    };
    addAndMakeVisible (typeBox_);

    // The AI can move these while the editor is open, so poll for changes the UI
    // did not make. 15 Hz is plenty for five numbers and costs nothing.
    startTimerHz (15);
}

EedSaturationEditor::~EedSaturationEditor()
{
    stopTimer();
}

void EedSaturationEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    auto r = content;

    auto typeRow = r.removeFromTop (juce::jmin (kRowH, r.getHeight()));
    typeBox_.setBounds (typeRow.withSizeKeepingCentre (
        juce::jmin (170, typeRow.getWidth()), typeRow.getHeight()));

    if (r.getHeight() > 6) r.removeFromTop (6);

    // Four dial columns, centred as a group, so the device stays balanced at any
    // width the rack gives it.
    const int wanted = kKnobW * 4 + kGap * 3;
    auto row = r.withSizeKeepingCentre (juce::jmin (wanted, r.getWidth()),
                                        juce::jmin (kKnobH, r.getHeight()));

    const int colW = juce::jmax (1, (row.getWidth() - kGap * 3) / 4);
    EchoJayDeviceKnob* dials[] = { &driveKnob_, &toneKnob_, &mixKnob_, &outKnob_ };
    for (int i = 0; i < 4; ++i)
    {
        dials[i]->setBounds (row.removeFromLeft (colW));
        if (i < 3 && row.getWidth() > kGap) row.removeFromLeft (kGap);
    }
}

void EedSaturationEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; } bound[] = {
        { &driveKnob_, EedSaturationProcessor::kDriveDb  },
        { &toneKnob_,  EedSaturationProcessor::kToneDb   },
        { &mixKnob_,   EedSaturationProcessor::kMix      },
        { &outKnob_,   EedSaturationProcessor::kOutputDb },
    };

    for (auto& b : bound)
    {
        const double v = proc_.getParamValue (b.id);
        if (std::abs (v - b.knob->getRealValue()) > 1.0e-4)
            b.knob->setRealValue (v);
    }

    const int typeId = (int) proc_.getParamValue (EedSaturationProcessor::kType) + 1;
    if (typeBox_.getSelectedId() != typeId)
        typeBox_.setSelectedId (typeId, juce::dontSendNotification);
}

void EedSaturationEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
