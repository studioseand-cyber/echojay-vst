/*
    EedDelayEditor.cpp  —  see EedDelayEditor.h.
*/

#include "EedDelayEditor.h"

using namespace echojay::device;
using namespace echojay::device::metrics;

// Qualified alias, not the unqualified name: `Colours` is ambiguous against
// juce::Colours, which JuceHeader.h pulls into scope. Same convention as
// DeviceEditorBase.cpp and SurgicalEqEditor.
using C = echojay::device::Colours;

namespace
{
    // Five dial columns across, two rows, plus a control strip. The rack sizes
    // this down if it has to, and layoutContent survives that.
    constexpr int kDefaultW = 440;
    constexpr int kDefaultH = 268;

    constexpr int kGap      = 8;
    constexpr int kBtnW     = 92;
    constexpr int kDivBoxW  = 84;
}

EedDelayEditor::EedDelayEditor (EedDelayProcessor& p)
    : DeviceEditorBase (p, "DELAY", kDefaultW, kDefaultH), proc_ (p)
{
    auto setupKnob = [this] (EchoJayDeviceKnob& k, const char* id,
                             double skewMid, int decimals, const juce::String& suffix,
                             const juce::String& caption)
    {
        // Ranges come from the SCHEMA, never re-typed here: the knob physically
        // cannot travel somewhere the AI is not allowed to dial, and widening
        // one without the other is impossible.
        const auto* spec = EedDelayProcessor::schema().find (id);
        jassert (spec != nullptr);
        if (spec == nullptr) return;

        k.setSpec (spec->min, spec->max, skewMid, decimals, suffix, caption, spec->def);
        k.setRealValue (proc_.getParamValue (juce::String (id)));
        k.onValueChange = [this, id, &k] { if (! suppressCallbacks_) pushKnob (id, k); };
        addAndMakeVisible (k);
    };

    // Skew mid-points put the useful part of each range under the middle of the
    // dial: time and the two filter frequencies are all heard logarithmically,
    // so a linear taper would waste most of the travel on values nobody wants.
    setupKnob (timeKnob_,     EedDelayProcessor::kTimeMs,       300.0,  0, " ms", "TIME");
    setupKnob (feedbackKnob_, EedDelayProcessor::kFeedback,       0.0,  0, " %",  "FDBK");
    setupKnob (mixKnob_,      EedDelayProcessor::kMix,            0.0,  0, " %",  "MIX");
    setupKnob (hpKnob_,       EedDelayProcessor::kFilterHpHz,   200.0,  0, " Hz", "HP");
    setupKnob (lpKnob_,       EedDelayProcessor::kFilterLpHz,  2000.0,  0, " Hz", "LP");
    setupKnob (offsetKnob_,   EedDelayProcessor::kStereoOffset,   0.0,  0, " %",  "OFFSET");
    setupKnob (modRateKnob_,  EedDelayProcessor::kModRateHz,      1.0,  2, " Hz", "RATE");
    setupKnob (modDepthKnob_, EedDelayProcessor::kModDepthMs,     3.0,  1, " ms", "DEPTH");

    auto setupToggle = [this] (juce::TextButton& b, const char* id)
    {
        styleButton (b, true);
        b.setToggleState (proc_.getParamValue (juce::String (id)) >= 0.5,
                          juce::dontSendNotification);
        b.onClick = [this, &b, id]
        {
            proc_.setParamValue (juce::String (id), b.getToggleState() ? 1.0 : 0.0);
            refreshSyncState();
            refreshHint();
        };
        addAndMakeVisible (b);
    };

    setupToggle (syncBtn_,     EedDelayProcessor::kSync);
    setupToggle (pingPongBtn_, EedDelayProcessor::kPingPong);

    // The note-length selector. Item IDs are index + 1 because a JUCE ComboBox
    // treats 0 as "nothing selected".
    styleCombo (divisionBox_);
    for (int i = 0; i < echojay::DelayEngine::kNumDivisions; ++i)
        divisionBox_.addItem (echojay::DelayEngine::divisionName (i), i + 1);

    divisionBox_.setSelectedId ((int) proc_.getParamValue (EedDelayProcessor::kDivision) + 1,
                                juce::dontSendNotification);
    divisionBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (EedDelayProcessor::kDivision,
                             (double) (divisionBox_.getSelectedId() - 1));
        refreshHint();
    };
    addAndMakeVisible (divisionBox_);

    divisionLabel_.setText ("NOTE", juce::dontSendNotification);
    divisionLabel_.setColour (juce::Label::textColourId, C::text3);
    divisionLabel_.setFont (uiFont (9.0f, true));
    divisionLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (divisionLabel_);

    refreshSyncState();
    refreshHint();

    // The AI can move any of these while the editor is open, so poll for changes
    // the UI did not make. 15 Hz is plenty and costs nothing.
    startTimerHz (15);
}

EedDelayEditor::~EedDelayEditor()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
void EedDelayEditor::layoutContent (juce::Rectangle<int> content)
{
    if (content.isEmpty()) return;

    // Row 1: the five dials that define the echo. Row 2: the three that place it
    // in the stereo field and move it. Row 3: the switches.
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

    // Give each row what it needs and no more, so a rack that hands us less than
    // the default height shrinks the gaps rather than overlapping the rows.
    const int rowH = juce::jmin (kKnobH, juce::jmax (1, (content.getHeight() - kRowH - kGap) / 2));

    placeRow (content.removeFromTop (rowH),
              { &timeKnob_, &feedbackKnob_, &mixKnob_, &hpKnob_, &lpKnob_ });

    if (content.getHeight() > kGap) content.removeFromTop (kGap);

    placeRow (content.removeFromTop (juce::jmin (rowH, content.getHeight())),
              { &offsetKnob_, &modRateKnob_, &modDepthKnob_ });

    if (content.getHeight() > kGap) content.removeFromTop (kGap);
    if (content.isEmpty()) return;

    // The switch strip, centred as a group.
    auto strip = content.removeFromTop (juce::jmin (kRowH, content.getHeight()));
    const int wanted = kBtnW + kGap + 40 + 4 + kDivBoxW + kGap + kBtnW;
    strip = strip.withSizeKeepingCentre (juce::jmin (wanted, strip.getWidth()),
                                         strip.getHeight());

    const double scale = (double) strip.getWidth() / (double) juce::jmax (1, wanted);
    auto take = [&strip, scale] (int w) { return strip.removeFromLeft (juce::jmax (1, (int) (w * scale))); };

    syncBtn_.setBounds (take (kBtnW));
    take (kGap);
    divisionLabel_.setBounds (take (40));
    take (4);
    divisionBox_.setBounds (take (kDivBoxW));
    take (kGap);
    pingPongBtn_.setBounds (take (kBtnW));
}

// ---------------------------------------------------------------------------
void EedDelayEditor::pushKnob (const char* id, const EchoJayDeviceKnob& k)
{
    // Through the schema path, exactly as an AI move would.
    proc_.setParamValue (juce::String (id), k.getRealValue());
    refreshHint();
}

void EedDelayEditor::refreshSyncState()
{
    const bool synced = proc_.getParamValue (EedDelayProcessor::kSync) >= 0.5;

    // Dim rather than hide: a control that disappears makes the layout jump, and
    // the dimmed dial still shows the free-running time you will return to.
    timeKnob_.setDimmed (synced);
    divisionBox_.setEnabled (synced);
    divisionLabel_.setColour (juce::Label::textColourId,
                              synced ? C::text2 : C::text3);
    divisionLabel_.repaint();
}

void EedDelayEditor::refreshHint()
{
    const auto& e = proc_.engine();

    juce::String h;
    if (e.getSync())
        h << echojay::DelayEngine::divisionName (e.getDivision())
          << " = " << juce::String (e.effectiveTimeMs(), 0) << " ms at "
          << juce::String (echojay::hostTempoBpm(), 1) << " BPM";
    else
        h << juce::String (e.effectiveTimeMs(), 0) << " ms free";

    // setHeaderHint repaints, so only touch it when the text actually changed —
    // otherwise the 15 Hz timer repaints the whole editor forever.
    if (h == lastHint_) return;
    lastHint_ = h;
    setHeaderHint (h);
}

void EedDelayEditor::syncFromProcessor()
{
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    // Only write when a value actually moved: setRealValue would otherwise fight
    // a drag in progress by snapping the knob to the value it just sent.
    auto pull = [this] (EchoJayDeviceKnob& k, const char* id, double tol)
    {
        const double v = proc_.getParamValue (juce::String (id));
        if (std::abs (v - k.getRealValue()) > tol) k.setRealValue (v);
    };

    pull (timeKnob_,     EedDelayProcessor::kTimeMs,       1.0e-3);
    pull (feedbackKnob_, EedDelayProcessor::kFeedback,     1.0e-3);
    pull (mixKnob_,      EedDelayProcessor::kMix,          1.0e-3);
    pull (hpKnob_,       EedDelayProcessor::kFilterHpHz,   1.0e-3);
    pull (lpKnob_,       EedDelayProcessor::kFilterLpHz,   1.0e-3);
    pull (offsetKnob_,   EedDelayProcessor::kStereoOffset, 1.0e-3);
    pull (modRateKnob_,  EedDelayProcessor::kModRateHz,    1.0e-4);
    pull (modDepthKnob_, EedDelayProcessor::kModDepthMs,   1.0e-3);

    const bool synced = proc_.getParamValue (EedDelayProcessor::kSync) >= 0.5;
    if (syncBtn_.getToggleState() != synced)
    {
        syncBtn_.setToggleState (synced, juce::dontSendNotification);
        refreshSyncState();
    }

    const bool pp = proc_.getParamValue (EedDelayProcessor::kPingPong) >= 0.5;
    if (pingPongBtn_.getToggleState() != pp)
        pingPongBtn_.setToggleState (pp, juce::dontSendNotification);

    const int div = (int) proc_.getParamValue (EedDelayProcessor::kDivision);
    if (divisionBox_.getSelectedId() != div + 1)
        divisionBox_.setSelectedId (div + 1, juce::dontSendNotification);
}

void EedDelayEditor::timerCallback()
{
    syncFromProcessor();

    // The tempo can change under a synced delay without any parameter moving, so
    // the hint is refreshed every tick rather than only on a control change.
    refreshHint();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
