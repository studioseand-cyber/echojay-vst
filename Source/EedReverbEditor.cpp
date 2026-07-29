/*
    EedReverbEditor.cpp  —  see EedReverbEditor.h.
*/

#include "EedReverbEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    // Five dial columns across, two rows. The rack sizes this down if it has to,
    // and layoutContent survives that.
    constexpr int kDefaultW = 440;
    constexpr int kDefaultH = 236;
    constexpr int kGap      = 8;
}

EedReverbEditor::EedReverbEditor (EedReverbProcessor& p)
    : DeviceEditorBase (p, "REVERB", kDefaultW, kDefaultH), proc_ (p)
{
    auto setupKnob = [this] (EchoJayDeviceKnob& k, const char* id,
                             double skewMid, int decimals, const juce::String& suffix,
                             const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial.
        const auto* spec = EedReverbProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k] { if (! suppressCallbacks_) pushKnob (id, k); };
        addAndMakeVisible (k);
    };

    // Decay and the low cut are heard logarithmically, so their skew mid-points
    // put the musically busy part of the range under the middle of the dial:
    // most of the useful decay settings live under 4 seconds, not under 10.
    setupKnob (sizeKnob_,      EedReverbProcessor::kSize,         0.0, 0, " %",  "SIZE");
    setupKnob (decayKnob_,     EedReverbProcessor::kDecayS,       2.5, 2, " s",  "DECAY");
    setupKnob (predelayKnob_,  EedReverbProcessor::kPredelayMs,  40.0, 0, " ms", "PREDLY");
    setupKnob (mixKnob_,       EedReverbProcessor::kMix,          0.0, 0, " %",  "MIX");
    setupKnob (dampingKnob_,   EedReverbProcessor::kDamping,      0.0, 0, " %",  "DAMP");
    setupKnob (lowCutKnob_,    EedReverbProcessor::kLowCutHz,   150.0, 0, " Hz", "LOCUT");
    setupKnob (widthKnob_,     EedReverbProcessor::kWidth,        0.0, 0, " %",  "WIDTH");
    setupKnob (earlyLateKnob_, EedReverbProcessor::kEarlyLate,    0.0, 0, " %",  "EARLY/LATE");
    setupKnob (modDepthKnob_,  EedReverbProcessor::kModDepth,     0.0, 0, " %",  "MOD");

    refreshHint();

    // The AI can move any of these while the editor is open, so poll for changes
    // the UI did not make.
    startTimerHz (15);
}

EedReverbEditor::~EedReverbEditor()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
void EedReverbEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    auto placeRow = [] (juce::Rectangle<int> row,
                        std::initializer_list<EchoJayDeviceKnob*> knobs)
    {
        const int n = (int) knobs.size();
        if (n == 0 || row.isEmpty()) return;

        const int wanted = kKnobW * n + kGap * (n - 1);
        auto strip = row.withSizeKeepingCentre (juce::jmin (wanted, row.getWidth()),
                                                row.getHeight());

        const int colW = juce::jmax (1, (strip.getWidth() - kGap * (n - 1)) / n);
        for (auto* k : knobs)
        {
            k->setBounds (strip.removeFromLeft (colW));
            strip.removeFromLeft (kGap);
        }
    };

    // Two rows, each taking half of whatever height the rack gave us. Clamped to
    // the dial's natural height so a tall slot does not stretch the knobs.
    const int rowH = juce::jmin (kKnobH, juce::jmax (1, (content.getHeight() - kGap) / 2));

    // Top row: the four controls that define the space itself.
    placeRow (content.removeFromTop (rowH),
              { &sizeKnob_, &decayKnob_, &predelayKnob_, &mixKnob_ });

    if (content.getHeight() > kGap) content.removeFromTop (kGap);

    // Bottom row: what happens inside it.
    placeRow (content.removeFromTop (juce::jmin (rowH, content.getHeight())),
              { &dampingKnob_, &lowCutKnob_, &widthKnob_, &earlyLateKnob_, &modDepthKnob_ });
}

// ---------------------------------------------------------------------------
void EedReverbEditor::pushKnob (const char* id, const EchoJayDeviceKnob& k)
{
    // Through the schema path, exactly as an AI move would.
    proc_.setParamValue (juce::String (id), k.getRealValue());
    refreshHint();
}

void EedReverbEditor::refreshHint()
{
    const auto& e = proc_.engine();

    // effectiveDecaySeconds, not the decay knob: the damping filter is inside
    // the feedback loop, so it shortens the tail it is filtering. Reporting the
    // asked-for number here would be a readout the ear disagrees with.
    juce::String h;
    h << juce::String (e.roomSizeMetres(), 0) << " m room, "
      << juce::String (e.effectiveDecaySeconds(), 1) << " s tail";

    // setHeaderHint repaints, so only touch it when the text actually changed —
    // otherwise the 15 Hz timer repaints the whole editor forever.
    if (h == lastHint_) return;
    lastHint_ = h;
    setHeaderHint (h);
}

void EedReverbEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    // Only write when a value actually moved: setRealValue would otherwise fight
    // a drag in progress by snapping the knob to the value it just sent.
    auto pull = [this] (EchoJayDeviceKnob& k, const char* id, double tol)
    {
        const double v = proc_.getParamValue (juce::String (id));
        if (std::abs (v - k.getRealValue()) > tol) k.setRealValue (v);
    };

    pull (sizeKnob_,      EedReverbProcessor::kSize,       1.0e-3);
    pull (decayKnob_,     EedReverbProcessor::kDecayS,     1.0e-4);
    pull (predelayKnob_,  EedReverbProcessor::kPredelayMs, 1.0e-3);
    pull (mixKnob_,       EedReverbProcessor::kMix,        1.0e-3);
    pull (dampingKnob_,   EedReverbProcessor::kDamping,    1.0e-3);
    pull (lowCutKnob_,    EedReverbProcessor::kLowCutHz,   1.0e-3);
    pull (widthKnob_,     EedReverbProcessor::kWidth,      1.0e-3);
    pull (earlyLateKnob_, EedReverbProcessor::kEarlyLate,  1.0e-3);
    pull (modDepthKnob_,  EedReverbProcessor::kModDepth,   1.0e-3);
}

void EedReverbEditor::timerCallback()
{
    syncFromProcessor();
    refreshHint();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
