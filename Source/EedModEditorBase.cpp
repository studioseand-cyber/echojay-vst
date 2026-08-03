/*
    EedModEditorBase.cpp  —  see EedModEditorBase.h.
*/

#include "EedModEditorBase.h"

#include <cmath>

using namespace echojay::device;
using namespace echojay::device::metrics;

namespace
{
    constexpr int kKnobGap  = 12;
    constexpr int kRowGap   = 8;
    constexpr int kToggleW  = 62;
}

EedModEditorBase::EedModEditorBase (EedDeviceProcessor& proc, const juce::String& title,
                                    int defaultWidth, int defaultHeight)
    : DeviceEditorBase (proc, title, defaultWidth, defaultHeight), proc_ (proc)
{
}

EedModEditorBase::~EedModEditorBase()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------
void EedModEditorBase::addParamKnob (const char* id,
                                     const juce::String& caption,
                                     int decimals,
                                     const juce::String& suffix,
                                     double skewMidPoint,
                                     std::function<juce::String(double)> format)
{
    // The range comes from the SCHEMA, never re-typed here. A param the device
    // does not publish has no dial, which is the loud version of the failure
    // rather than a knob that turns and does nothing.
    const auto* spec = proc_.paramSchema().find (juce::String (id).toStdString());
    jassert (spec != nullptr);
    if (spec == nullptr) return;

    auto entry = std::make_unique<Entry>();
    entry->id       = id;
    entry->discrete = (bool) format;

    // formatValue has to be set BEFORE setSpec: setSpec pushes the default
    // through the readout, and a formatter arriving afterwards would leave that
    // first draw showing a bare number.
    if (format) entry->knob.formatValue = std::move (format);

    entry->knob.setSpec (spec->min, spec->max, skewMidPoint, decimals, suffix,
                         caption, spec->def);
    entry->knob.setRealValue (proc_.getParamValue (juce::String (id)));

    auto* raw = entry.get();
    entry->knob.onValueChange = [this, raw]
    {
        if (suppressCallbacks_) return;
        proc_.setParamValue (raw->id, raw->knob.getRealValue());
        updateDimming();
    };

    addAndMakeVisible (entry->knob);
    knobs_.push_back (std::move (entry));
}

void EedModEditorBase::addHeaderToggle (const char* id, const juce::String& text)
{
    const auto* spec = proc_.paramSchema().find (juce::String (id).toStdString());
    jassert (spec != nullptr);
    if (spec == nullptr) return;

    auto t = std::make_unique<Toggle>();
    t->id = id;
    t->button.setButtonText (text);
    styleButton (t->button, true);
    t->button.setToggleState (proc_.getParamValue (juce::String (id)) >= 0.5,
                              juce::dontSendNotification);

    auto* raw = t.get();
    t->button.onClick = [this, raw]
    {
        proc_.setParamValue (raw->id, raw->button.getToggleState() ? 1.0 : 0.0);
        updateDimming();
    };

    addAndMakeVisible (t->button);
    toggles_.push_back (std::move (t));
}

void EedModEditorBase::addHeaderChoice (const char* id, int widthPx)
{
    const auto* spec = proc_.paramSchema().find (juce::String (id).toStdString());
    jassert (spec != nullptr && ! spec->choices.empty());
    if (spec == nullptr || spec->choices.empty()) return;

    auto c = std::make_unique<Choice>();
    c->id    = id;
    c->width = widthPx;

    styleCombo (c->box);
    // Item IDs are index + 1 because a JUCE ComboBox treats 0 as "nothing
    // selected" — same convention as the Delay and Reverb selectors.
    for (std::size_t i = 0; i < spec->choices.size(); ++i)
        c->box.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

    c->box.setSelectedId ((int) proc_.getParamValue (juce::String (id)) + 1,
                          juce::dontSendNotification);

    auto* raw = c.get();
    c->box.onChange = [this, raw]
    {
        if (suppressCallbacks_) return;

        // Through setParamValue, exactly as an AI move does, so a click and a
        // dialled mode cannot end up meaning different things.
        proc_.setParamValue (raw->id, (double) (raw->box.getSelectedId() - 1));

        // The mode decides which dials are on the panel at all, so a change
        // relayouts now rather than waiting for a resize that may never come.
        if (applyControlVisibility()) resized();
        updateDimming();
        refreshExtras();
    };

    addAndMakeVisible (c->box);
    choices_.push_back (std::move (c));
}

void EedModEditorBase::setRateGroup (const char* syncId, const char* rateId,
                                     const char* divisionId)
{
    syncId_     = syncId;
    rateId_     = rateId;
    divisionId_ = divisionId;
    updateDimming();
}

void EedModEditorBase::finishSetup()
{
    // The AI can move any of these while the editor is open, so poll for changes
    // the UI did not make. 20 Hz rather than the 15 this used before the visuals:
    // a handful of numbers does not need it, but a playhead sweeping a 6 Hz
    // tremolo does — at 15 Hz it lands about four times a cycle and reads as a
    // stutter rather than as motion. Matches EedDynamicsFaceEditor's rate.
    startTimerHz (20);
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
void EedModEditorBase::layoutHeaderLeading (juce::Rectangle<int>& bar)
{
    // The header lays out BEFORE the content (DeviceEditorBase::resized), so
    // visibility has to be settled here or a toggle the mode just hid would
    // still be measured. Idempotent, so layoutContent asking again is free.
    applyControlVisibility();

    // Selectors sit outermost (inboard of BYPASS), then the toggles. A toggle
    // the mode has taken off the panel gives its room back to the title.
    for (auto& c : choices_)
    {
        c->box.setBounds (bar.removeFromRight (juce::jmin (c->width, juce::jmax (0, bar.getWidth())))
                             .reduced (0, 3));
        bar.removeFromRight (6);
    }

    for (auto& t : toggles_)
    {
        if (! t->button.isVisible()) continue;
        t->button.setBounds (bar.removeFromRight (kToggleW).reduced (0, 3));
        bar.removeFromRight (6);
    }
}

void EedModEditorBase::layoutContent (juce::Rectangle<int> content)
{
    const int wantTop = topContentHeight();

    // The mode decides which dials exist right now; settle that before the
    // flow below counts them, or a hidden dial would still claim a column.
    applyControlVisibility();

    std::vector<Entry*> shown;
    shown.reserve (knobs_.size());
    for (auto& e : knobs_)
        if (e->knob.isVisible()) shown.push_back (e.get());

    if (content.isEmpty() || shown.empty())
    {
        // Tell the subclass anyway. A rack slot collapsing from a size that HAD
        // room for the picture to one that has no content area at all would
        // otherwise skip the notification, leaving the view visible at last
        // frame's bounds — a scope sitting at 480x108 inside a 1x1 editor,
        // saved from being seen only by the parent's clipping.
        if (wantTop > 0) layoutTopContent ({});
        return;
    }

    // The visualisation is reserved FIRST in code and LAST in priority: it only
    // gets a strip if a whole dial row survives underneath it. That ordering is
    // the inline-hosting contract — a collapsed rack slot loses the picture and
    // keeps the controls, never the other way round.
    juce::Rectangle<int> topArea;
    if (wantTop > 0)
    {
        const int h = juce::jmin (wantTop, juce::jmax (0, content.getHeight() - kKnobH));
        if (h > 0)
        {
            topArea = content.removeFromTop (h);
            content.removeFromTop (6);
        }
    }

    // Flow the dials into as many rows as the width forces. The rack can lay an
    // editor out narrower than its default, and a fixed single row would push
    // the last dials off the edge rather than wrapping them.
    const int perRow = juce::jmax (1, (content.getWidth() + kKnobGap)
                                        / (kKnobW + kKnobGap));
    const int rows   = ((int) shown.size() + perRow - 1) / perRow;

    const int rowH   = juce::jmin (kKnobH, juce::jmax (1,
                          (content.getHeight() - (rows - 1) * kRowGap) / juce::jmax (1, rows)));

    const int blockH = rows * rowH + (rows - 1) * kRowGap;
    auto area = content.withSizeKeepingCentre (content.getWidth(),
                                               juce::jmin (blockH, content.getHeight()));

    int index = 0;
    for (int r = 0; r < rows && index < (int) shown.size(); ++r)
    {
        const int inThisRow = juce::jmin (perRow, (int) shown.size() - index);
        const int rowW      = inThisRow * kKnobW + (inThisRow - 1) * kKnobGap;

        auto row = area.removeFromTop (rowH)
                       .withSizeKeepingCentre (juce::jmin (rowW, area.getWidth()), rowH);
        area.removeFromTop (kRowGap);

        const int colW = juce::jmax (1, (row.getWidth() - (inThisRow - 1) * kKnobGap)
                                          / inThisRow);
        for (int c = 0; c < inThisRow; ++c, ++index)
        {
            shown[(size_t) index]->knob.setBounds (row.removeFromLeft (colW));
            row.removeFromLeft (kKnobGap);
        }
    }

    // Unconditional, including when topArea came out empty: that is the signal
    // the subclass uses to hide its view.
    if (wantTop > 0) layoutTopContent (topArea);
}

bool EedModEditorBase::applyControlVisibility()
{
    bool changed = false;

    for (auto& e : knobs_)
    {
        const bool want = controlVisible (e->id.toRawUTF8());
        if (e->knob.isVisible() != want)
        {
            e->knob.setVisible (want);
            changed = true;
        }
    }

    for (auto& t : toggles_)
    {
        const bool want = controlVisible (t->id.toRawUTF8());
        if (t->button.isVisible() != want)
        {
            t->button.setVisible (want);
            changed = true;
        }
    }

    return changed;
}

// ---------------------------------------------------------------------------
// keeping the UI honest
// ---------------------------------------------------------------------------
void EedModEditorBase::updateDimming()
{
    if (syncId_.isEmpty()) return;

    const bool synced = proc_.getParamValue (syncId_) >= 0.5;

    for (auto& e : knobs_)
    {
        if (e->id == rateId_)     e->knob.setDimmed (synced);
        if (e->id == divisionId_) e->knob.setDimmed (! synced);
    }
}

void EedModEditorBase::syncFromProcessor()
{
    // Only write when the value actually moved: setRealValue would otherwise
    // fight a drag in progress by snapping the knob back to what it just sent.
    const juce::ScopedValueSetter<bool> guard (suppressCallbacks_, true);

    for (auto& e : knobs_)
    {
        const double v    = proc_.getParamValue (e->id);
        const double have = e->knob.getRealValue();

        // A discrete dial sits at a continuous position while the processor
        // holds the rounded one; comparing raw values would make every drag
        // snap back to the nearest step.
        const bool differs = e->discrete
                           ? std::lround (v) != std::lround (have)
                           : std::abs (v - have) > 1.0e-4;

        if (differs) e->knob.setRealValue (v);
    }

    for (auto& t : toggles_)
    {
        const bool on = proc_.getParamValue (t->id) >= 0.5;
        if (t->button.getToggleState() != on)
            t->button.setToggleState (on, juce::dontSendNotification);
    }

    // The AI can switch a mode while the editor is open, and the mode decides
    // which dials are on the panel — so a move from outside relayouts too.
    bool modeMoved = false;
    for (auto& c : choices_)
    {
        const int want = (int) proc_.getParamValue (c->id) + 1;
        if (c->box.getSelectedId() != want)
        {
            c->box.setSelectedId (want, juce::dontSendNotification);
            modeMoved = true;
        }
    }
    if (applyControlVisibility() || modeMoved) resized();
}

void EedModEditorBase::timerCallback()
{
    syncFromProcessor();
    updateDimming();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);

    refreshExtras();
}
