/*
    EedDynamicsFaceEditor.h  —  the editor shape four of the six Dynamics faces
    share: a row of filmstrip dials over a gain-reduction meter.

    Compressor, Gate, Expander and Limiter differ ONLY in which params they
    publish and how deep their meter scale runs. Writing that layout four times
    would mean four places for the knob/schema wiring to drift, and the drift
    would be invisible — a knob quietly bound to the wrong id still turns.

    So a face editor supplies a KnobSpec table and a meter depth, and inherits
    the rest. It stays a real per-device class (EedCompressorEditor and friends)
    because the house rule is that a device owns its own files, and because the
    de-esser needs to add controls to exactly this shape rather than replace it.

    The GR source is a std::function rather than a base-class virtual on the
    processor: the four processors have no common dynamics base beyond
    EedDeviceProcessor, and adding one purely so this editor can call it would
    put UI concerns into the device contract.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedDynamicsEditorSupport.h"
#include "EedGrMeter.h"

#include <functional>
#include <vector>

class EedDynamicsFaceEditor : public DeviceEditorBase,
                              private juce::Timer
{
public:
    // `specs` must outlive the editor — in practice a file-scope constant array
    // in the device's own editor .cpp, which is where the knob order belongs.
    // `grSource` may be null for a device with no meter.
    EedDynamicsFaceEditor (EedDeviceProcessor& proc,
                           const juce::String& title,
                           const juce::String& hint,
                           const echojay::device::KnobSpec* specs, int numSpecs,
                           float meterMaxDb,
                           std::function<float()> grSource,
                           int defaultWidth, int defaultHeight);

    ~EedDynamicsFaceEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

    // A subclass adding its own controls lays them out in the strip this
    // reserves. Returns the height to take off the bottom, above the meter.
    virtual int extraContentHeight() const { return 0; }
    virtual void layoutExtraContent (juce::Rectangle<int>) {}

    // Called from the shared timer after the knobs and meter have been synced,
    // for a subclass that has state of its own to refresh.
    virtual void refreshExtras() {}

    echojay::device::EchoJayDeviceKnob& knob (int i) noexcept { return *knobs_[i]; }
    echojay::device::GrMeter&           meter() noexcept      { return meter_; }
    EedDeviceProcessor&                 device() noexcept     { return proc_; }

    // Guards the knob callbacks while the timer writes values into them.
    const bool* suppressFlag() const noexcept { return &suppressCallbacks_; }

private:
    void timerCallback() override;

    EedDeviceProcessor& proc_;

    const echojay::device::KnobSpec* specs_ = nullptr;
    int                              numSpecs_ = 0;

    // OwnedArray, not std::vector: EchoJayDeviceKnob is non-copyable and
    // non-movable by design, which is exactly what a vector needs to grow.
    juce::OwnedArray<echojay::device::EchoJayDeviceKnob> knobs_;
    echojay::device::GrMeter                            meter_;
    std::function<float()>                              grSource_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDynamicsFaceEditor)
};
