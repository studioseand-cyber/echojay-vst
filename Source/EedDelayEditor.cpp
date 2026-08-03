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
    // Five dial columns across, two rows, a control strip, and the taps view
    // seated above all of it. The rack sizes this down if it has to, and
    // layoutContent survives that — the view is the first thing to give up room.
    constexpr int kDefaultW = 460;
    constexpr int kTapsH    = 104;
    constexpr int kDefaultH = 268 + kTapsH + 6;

    constexpr int kGap      = 8;
    constexpr int kBtnW     = 92;
    constexpr int kDivBoxW  = 84;

    // Wide enough for "PINGPONG", the longest of the four.
    constexpr int kModeW    = 100;

    // Below this the taps are a row of specks with no readable spacing between
    // them, so the view is dropped rather than drawn uselessly small.
    constexpr int kMinTapsH = 44;
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
    setupKnob (diffusionKnob_,EedDelayProcessor::kDiffusion,      0.0,  0, " %",  "DIFFUSE");
    setupKnob (duckKnob_,     EedDelayProcessor::kDuck,           0.0,  0, " %",  "DUCK");

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
            refreshTaps();
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
        refreshTaps();
    };
    addAndMakeVisible (divisionBox_);

    divisionLabel_.setText ("NOTE", juce::dontSendNotification);
    divisionLabel_.setColour (juce::Label::textColourId, C::text3);
    divisionLabel_.setFont (uiFont (9.0f, true));
    divisionLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (divisionLabel_);

    // The MODE selector. Items ARE the schema's choices, in the schema's order, so
    // the list a user sees and the list the model is taught cannot drift apart.
    styleCombo (modeBox_);
    if (const auto* spec = EedDelayProcessor::schema().find (EedDelayProcessor::kMode))
    {
        for (std::size_t i = 0; i < spec->choices.size(); ++i)
            modeBox_.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

        modeBox_.setSelectedId ((int) proc_.getParamValue (EedDelayProcessor::kMode) + 1,
                                juce::dontSendNotification);
    }
    modeBox_.onChange = [this]
    {
        if (suppressCallbacks_) return;

        proc_.setParamValue (EedDelayProcessor::kMode,
                             (double) (modeBox_.getSelectedId() - 1));

        // The mode decides whether PING-PONG is on the panel at all, so a change
        // has to relayout now rather than wait for a resize that may never come.
        refreshModeState();
        resized();
        refreshHint();
        refreshTaps();
    };
    addAndMakeVisible (modeBox_);

    taps_.setCaption ("REPEATS");
    addAndMakeVisible (taps_);

    refreshSyncState();
    refreshModeState();
    refreshHint();
    refreshTaps();

    // The AI can move any of these while the editor is open, so poll for changes
    // the UI did not make. 15 Hz is plenty: the taps view is analytic, so it only
    // redraws when a param actually moves — there is no signal to keep up with.
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

    // The taps view is reserved from the TOP out of whatever is left once both
    // dial rows and the switch strip are whole. It is the readout; the dials are
    // the device, and a short rack slot loses the readout first (the inline-
    // hosting contract in DeviceEditorBase.h).
    {
        const int needed = kKnobH * 2 + kRowH + kGap * 2 + 6;
        const int want   = juce::jmin (kTapsH, juce::jmax (0, content.getHeight() - needed));
        const bool room  = want >= kMinTapsH && content.getWidth() >= 80;

        taps_.setVisible (room);
        if (room)
        {
            taps_.setBounds (content.removeFromTop (want));
            content.removeFromTop (6);
        }
    }

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
              { &timeKnob_, &feedbackKnob_, &mixKnob_, &duckKnob_, &hpKnob_, &lpKnob_ });

    if (content.getHeight() > kGap) content.removeFromTop (kGap);

    placeRow (content.removeFromTop (juce::jmin (rowH, content.getHeight())),
              { &offsetKnob_, &diffusionKnob_, &modRateKnob_, &modDepthKnob_ });

    if (content.getHeight() > kGap) content.removeFromTop (kGap);
    if (content.isEmpty()) return;

    // The switch strip, centred as a group. PING-PONG drops out of it entirely in
    // `pingpong` mode — and out of the WIDTH calculation too, so the rest of the
    // strip re-centres rather than leaving a hole where it was.
    const bool showPingPong = pingPongSwitchVisible();
    pingPongBtn_.setVisible (showPingPong);

    auto strip = content.removeFromTop (juce::jmin (kRowH, content.getHeight()));
    const int wanted = kBtnW + kGap + 40 + 4 + kDivBoxW
                     + (showPingPong ? kGap + kBtnW : 0);
    strip = strip.withSizeKeepingCentre (juce::jmin (wanted, strip.getWidth()),
                                         strip.getHeight());

    const double scale = (double) strip.getWidth() / (double) juce::jmax (1, wanted);
    auto take = [&strip, scale] (int w) { return strip.removeFromLeft (juce::jmax (1, (int) (w * scale))); };

    syncBtn_.setBounds (take (kBtnW));
    take (kGap);
    divisionLabel_.setBounds (take (40));
    take (4);
    divisionBox_.setBounds (take (kDivBoxW));

    if (showPingPong)
    {
        take (kGap);
        pingPongBtn_.setBounds (take (kBtnW));
    }
}

void EedDelayEditor::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    modeBox_.setBounds (
        bar.removeFromRight (juce::jmin (kModeW, juce::jmax (0, bar.getWidth())))
           .reduced (0, 3));
    bar.removeFromRight (6);
}

bool EedDelayEditor::pingPongSwitchVisible() const
{
    return proc_.engine().getMode() != echojay::DelayMode::PingPong;
}

void EedDelayEditor::refreshModeState()
{
    const int want = (int) proc_.getParamValue (EedDelayProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != want)
        modeBox_.setSelectedId (want, juce::dontSendNotification);
}

// ---------------------------------------------------------------------------
void EedDelayEditor::pushKnob (const char* id, const EchoJayDeviceKnob& k)
{
    // Through the schema path, exactly as an AI move would.
    proc_.setParamValue (juce::String (id), k.getRealValue());
    refreshHint();
    refreshTaps();
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

void EedDelayEditor::refreshTaps()
{
    // Analytic: everything the picture needs is already in the schema. Read it
    // the same way the AI writes it, so the drawing cannot disagree with the
    // device about what it is set to.
    //
    // effectiveTimeMs(), not the TIME dial: with sync on the dial is ignored by
    // the audio path, and drawing it would show one delay above a device playing
    // another.
    // effectivePingPong(), not the ping_pong param: the bounce is the OR of the
    // switch and the mode, so reading only the switch would draw a straight delay
    // over a device that is bouncing because its MODE says to.
    taps_.setTaps ((float) proc_.engine().effectiveTimeMs(),
                   (float) proc_.getParamValue (EedDelayProcessor::kFeedback),
                   (float) proc_.getParamValue (EedDelayProcessor::kMix),
                   proc_.engine().effectivePingPong());

    // A bypassed device is not producing these repeats, so the picture of them
    // greys out rather than sitting there asserting otherwise.
    taps_.setDimmed (proc_.isBypassed());
}

void EedDelayEditor::refreshHint()
{
    const auto& e = proc_.engine();

    juce::String h;
    h << echojay::delayModeName (e.getMode()) << " - ";

    if (e.getSync())
        h << echojay::DelayEngine::divisionName (e.getDivision())
          << " = " << juce::String (e.effectiveTimeMs(), 0) << " ms at "
          << juce::String (echojay::hostTempoBpm(), 1) << " BPM";
    else
        h << juce::String (e.effectiveTimeMs(), 0) << " ms free";

    // Named only when it is doing something, so the line stays short in the common
    // case and the mention is a signal rather than decoration.
    if (e.getDuckPct() > 0.0f)  h << ", ducking " << juce::String (e.getDuckPct(), 0) << "%";
    if (e.effectivePingPong())  h << ", bouncing";

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
    pull (diffusionKnob_,EedDelayProcessor::kDiffusion,    1.0e-3);
    pull (duckKnob_,     EedDelayProcessor::kDuck,         1.0e-3);

    // The AI can switch the mode while the editor is open, and the mode decides
    // whether PING-PONG is on the panel — so a move from outside relayouts too.
    const int wantMode = (int) proc_.getParamValue (EedDelayProcessor::kMode) + 1;
    if (modeBox_.getSelectedId() != wantMode)
    {
        modeBox_.setSelectedId (wantMode, juce::dontSendNotification);
        resized();
    }

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
    // both the hint and the picture are refreshed every tick rather than only on
    // a control change. Both are gated internally on "did it actually move?", so
    // a still editor costs no repaints.
    refreshHint();
    refreshTaps();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);
}
