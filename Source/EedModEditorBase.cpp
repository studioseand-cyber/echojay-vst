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
    for (auto& t : toggles_)
    {
        t->button.setBounds (bar.removeFromRight (kToggleW).reduced (0, 3));
        bar.removeFromRight (6);
    }
}

void EedModEditorBase::layoutContent (juce::Rectangle<int> content)
{
    const int wantTop = topContentHeight();

    if (content.isEmpty() || knobs_.empty())
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
    const int rows   = ((int) knobs_.size() + perRow - 1) / perRow;

    const int rowH   = juce::jmin (kKnobH, juce::jmax (1,
                          (content.getHeight() - (rows - 1) * kRowGap) / juce::jmax (1, rows)));

    const int blockH = rows * rowH + (rows - 1) * kRowGap;
    auto area = content.withSizeKeepingCentre (content.getWidth(),
                                               juce::jmin (blockH, content.getHeight()));

    int index = 0;
    for (int r = 0; r < rows && index < (int) knobs_.size(); ++r)
    {
        const int inThisRow = juce::jmin (perRow, (int) knobs_.size() - index);
        const int rowW      = inThisRow * kKnobW + (inThisRow - 1) * kKnobGap;

        auto row = area.removeFromTop (rowH)
                       .withSizeKeepingCentre (juce::jmin (rowW, area.getWidth()), rowH);
        area.removeFromTop (kRowGap);

        const int colW = juce::jmax (1, (row.getWidth() - (inThisRow - 1) * kKnobGap)
                                          / inThisRow);
        for (int c = 0; c < inThisRow; ++c, ++index)
        {
            knobs_[(size_t) index]->knob.setBounds (row.removeFromLeft (colW));
            row.removeFromLeft (kKnobGap);
        }
    }

    // Unconditional, including when topArea came out empty: that is the signal
    // the subclass uses to hide its view.
    if (wantTop > 0) layoutTopContent (topArea);
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
}

void EedModEditorBase::timerCallback()
{
    syncFromProcessor();
    updateDimming();

    if (bypassButton().getToggleState() != proc_.isBypassed())
        bypassButton().setToggleState (proc_.isBypassed(), juce::dontSendNotification);

    refreshExtras();
}
