/*
    EedTapeEditor.cpp  —  see EedTapeEditor.h.
*/

#include "EedTapeEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kDefaultW = 420;
    constexpr int kDefaultH = 250;

    constexpr int kGap    = 12;
    constexpr int kRowGap = 8;
    constexpr int kCols   = 4;
}

EedTapeEditor::EedTapeEditor (EedTapeProcessor& p)
    : DeviceEditorBase (p, "TAPE", kDefaultW, kDefaultH), proc_ (p)
{
    auto setup = [this] (EchoJayDeviceKnob& k, const char* id,
                         double skewMid, int decimals, const juce::String& suffix,
                         const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial.
        const auto* spec = EedTapeProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k]
        {
            if (suppressCallbacks_) return;
            proc_.setParamValue (id, k.getRealValue());
            if (id == EedTapeProcessor::kSpeedIps) refreshSpeedHint();
        };
        addAndMakeVisible (k);
    };

    setup (speedKnob_,   EedTapeProcessor::kSpeedIps, 12.0, 1, " ips", "SPEED");
    setup (driveKnob_,   EedTapeProcessor::kDriveDb,   0.0, 1, " dB",  "DRIVE");
    setup (biasKnob_,    EedTapeProcessor::kBias,      0.0, 0, " %",   "BIAS");
    setup (bumpKnob_,    EedTapeProcessor::kHeadBump,  0.0, 1, " dB",  "BUMP");
    setup (wowKnob_,     EedTapeProcessor::kWow,       0.0, 0, " %",   "WOW");
    setup (flutterKnob_, EedTapeProcessor::kFlutter,   0.0, 0, " %",   "FLUTTER");
    setup (mixKnob_,     EedTapeProcessor::kMix,       0.0, 0, " %",   "MIX");
    setup (outKnob_,     EedTapeProcessor::kOutputDb,  0.0, 1, " dB",  "OUT");

    refreshSpeedHint();

    startTimerHz (15);
}

EedTapeEditor::~EedTapeEditor()
{
    stopTimer();
}

void EedTapeEditor::refreshSpeedHint()
{
    const float ips = proc_.engine().getSpeedIps();
    if (std::abs (ips - lastHintSpeed_) < 0.05f) return;
    lastHintSpeed_ = ips;

    // What the speed dial is really doing, in the units the effect is heard in.
    setHeaderHint ("bump " + juce::String (proc_.engine().headBumpHz(), 0) + " Hz  /  "
                   "top " + juce::String (proc_.engine().hfLossHz() / 1000.0f, 1) + " kHz");
}

void EedTapeEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    EchoJayDeviceKnob* rows[2][kCols] = {
        { &speedKnob_, &driveKnob_,   &biasKnob_, &bumpKnob_ },
        { &wowKnob_,   &flutterKnob_, &mixKnob_,  &outKnob_  },
    };

    auto r = content;

    // Both rows get the same height, so a rack that squeezes the device shrinks
    // them together rather than starving the second one.
    const int rowH = juce::jmax (1, juce::jmin (kKnobH, (r.getHeight() - kRowGap) / 2));

    for (int rowIndex = 0; rowIndex < 2; ++rowIndex)
    {
        if (r.getHeight() <= 0) return;

        auto band = r.removeFromTop (juce::jmin (rowH, r.getHeight()));
        if (rowIndex == 0 && r.getHeight() > kRowGap) r.removeFromTop (kRowGap);

        const int wanted = kKnobW * kCols + kGap * (kCols - 1);
        auto row = band.withSizeKeepingCentre (juce::jmin (wanted, band.getWidth()),
                                               band.getHeight());

        const int colW = juce::jmax (1, (row.getWidth() - kGap * (kCols - 1)) / kCols);
        for (int c = 0; c < kCols; ++c)
        {
            rows[rowIndex][c]->setBounds (row.removeFromLeft (colW));
            if (c < kCols - 1 && row.getWidth() > kGap) row.removeFromLeft (kGap);
        }
    }
}

void EedTapeEditor::syncFromProcessor()
{
    // Only write when it actually moved: setRealValue would otherwise fight a
    // drag in progress by snapping the knob to the value it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    struct { EchoJayDeviceKnob* knob; const char* id; } bound[] = {
        { &speedKnob_,   EedTapeProcessor::kSpeedIps },
        { &driveKnob_,   EedTapeProcessor::kDriveDb  },
        { &biasKnob_,    EedTapeProcessor::kBias     },
        { &bumpKnob_,    EedTapeProcessor::kHeadBump },
        { &wowKnob_,     EedTapeProcessor::kWow      },
        { &flutterKnob_, EedTapeProcessor::kFlutter  },
        { &mixKnob_,     EedTapeProcessor::kMix      },
        { &outKnob_,     EedTapeProcessor::kOutputDb },
    };

    for (auto& b : bound)
    {
        const double v = proc_.getParamValue (b.id);
        if (std::abs (v - b.knob->getRealValue()) > 1.0e-4)
            b.knob->setRealValue (v);
    }

    refreshSpeedHint();
}

void EedTapeEditor::timerCallback()
{
    syncFromProcessor();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
