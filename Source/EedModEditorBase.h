/*
    EedModEditorBase.h  —  the editor scaffold shared by the Modulation cluster.

    DeviceEditorBase gives every built-in the EchoJay identity (logo, title,
    bypass, palette, inline hosting). This adds the one layer above it that all
    four modulation devices genuinely share, so none of them re-writes it:

      * a row of filmstrip dials built FROM THE SCHEMA — a dial cannot travel
        anywhere the AI is not allowed to dial, because the range is read from
        the ParamSpec rather than re-typed next to it,
      * every dial driven through setParamValue/getParamValue, so a knob turn and
        an AI move take the identical path and cannot disagree about clamping,
      * the 15 Hz poll that keeps the UI honest when the AI moves a param while
        the editor is open,
      * the RATE / SYNC / DIVISION group, which is the same three controls with
        the same interlock on all four devices: SYNC in the header, and whichever
        of RATE and DIVISION is not currently in charge dimmed rather than
        hidden, so the device never silently ignores a dial you can still see.

    A device editor is then a LIST: its params, in the order they should read.

    DISCRETE PARAMS (shape, division, voices, stages) are ordinary dials with a
    formatter that names the value — "SINE", "1/8", "4 VOICES". They are not
    snapped mid-drag: the filmstrip tracks the drag from where it started, so
    writing back to it would fight the gesture. Instead the readout names the
    rounded value and the processor rounds on the way in, which is also exactly
    what happens when the AI sends shape: 2.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedDeviceProcessor.h"
#include "EedLfoCore.h"

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace echojay::mod
{
    // Readouts for the two discrete params every device in this cluster shares.
    // They name the value the DSP will actually use — i.e. the ROUNDED one — so
    // the dial cannot read "SQR" while the engine is running a triangle.
    inline juce::String shapeReadout (double v)
    {
        return juce::String (echojay::LfoCore::shapeName ((int) std::lround (v)));
    }

    inline juce::String divisionReadout (double v)
    {
        return juce::String (echojay::LfoCore::division ((int) std::lround (v)).name);
    }
}

class EedModEditorBase : public DeviceEditorBase,
                         private juce::Timer
{
public:
    ~EedModEditorBase() override;

protected:
    EedModEditorBase (EedDeviceProcessor& proc, const juce::String& title,
                      int defaultWidth, int defaultHeight);

    // Add one dial for a schema param. `format` overrides the numeric readout
    // (used to name a discrete value); passing one also marks the param discrete,
    // so the UI compares rounded values when syncing.
    void addParamKnob (const char* id,
                       const juce::String& caption,
                       int decimals,
                       const juce::String& suffix,
                       double skewMidPoint = 0.0,
                       std::function<juce::String(double)> format = {});

    // A header toggle for a boolean schema param.
    void addHeaderToggle (const char* id, const juce::String& text);

    // Wire the RATE/SYNC/DIVISION interlock: whichever of the two rate dials is
    // not in charge is dimmed. Call after all three have been added.
    void setRateGroup (const char* syncId, const char* rateId, const char* divisionId);

    // Start polling. Call last in the subclass constructor.
    void finishSetup();

    // A visualisation seated ABOVE the dials (VISUALS_PLAN.md: "Editors grow
    // their default height to seat the viz above the dials"). Same three hooks,
    // same names and same shrink policy as EedDynamicsFaceEditor, so the two
    // clusters' editors read alike: the picture is the LAST thing given room and
    // the FIRST thing to lose it. A rack slot laid out short keeps its controls.
    virtual int  topContentHeight() const { return 0; }

    // Called even when the area comes out EMPTY — that is how a subclass learns
    // to HIDE its view rather than leave it at last frame's bounds, sitting on
    // top of the dials it just lost its room to.
    virtual void layoutTopContent (juce::Rectangle<int>) {}

    // Called from the shared timer, after the knobs and toggles have been
    // synced, for a subclass with a picture to refresh.
    virtual void refreshExtras() {}

    void layoutContent (juce::Rectangle<int> content) override;
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

    // The LFO phase every Modulation visual rides. Reads the device's tap.
    virtual float lfoPhase() const { return 0.0f; }

private:
    struct Entry
    {
        juce::String id;
        echojay::device::EchoJayDeviceKnob knob;
        bool discrete = false;
    };

    void timerCallback() override;
    void syncFromProcessor();
    void updateDimming();

    EedDeviceProcessor& proc_;

    // unique_ptr because a knob is a Component: neither copyable nor movable, so
    // the vector cannot hold them by value.
    std::vector<std::unique_ptr<Entry>> knobs_;

    struct Toggle
    {
        juce::String id;
        juce::TextButton button;
    };
    std::vector<std::unique_ptr<Toggle>> toggles_;

    juce::String syncId_, rateId_, divisionId_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedModEditorBase)
};
