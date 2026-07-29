/*
    DeviceEditorBase.h  —  the shell every built-in device editor sits in
    (BUILTIN_SUITE_PLAN.md §2).

    Extracted from SurgicalEqEditor, which is where the header layout was
    designed. It owns the parts of a device editor that must be IDENTICAL across
    all 19 devices, so that a device's own code is only ever "place my controls":

      * the dark background fill,
      * the EchoJay logo, top-left, drawn from the real asset (the same one the
        main plugin header uses) rather than re-lettered as text,
      * the device title next to it, and an optional hint line,
      * the BYPASS button, hard right,
      * the header/content split, on shared metrics so two devices' headers line
        up exactly rather than approximately,
      * the inline chain-hosting sizing contract.

    INLINE HOSTING. These editors are hosted inside the chain rack, not in a
    floating window: ChainHost::createEditorForSlot hands the component straight
    to the rack, which sizes it to the space available. So an editor must (a)
    declare a sensible default size in its constructor, which is what the rack
    opens at, and (b) survive being laid out smaller than that without controls
    overlapping or escaping. resized() here enforces the second part by clamping
    the header and giving the remainder to the content, so a device that lays out
    only within `content` cannot break the contract.

    A subclass implements layoutContent() and, if it wants global controls of its
    own in the header (the EQ's OUT dial and analyzer toggles), layoutHeaderExtras().
*/

#pragma once

#include <JuceHeader.h>
#include "EedDeviceProcessor.h"
#include "EchoJayDeviceLookAndFeel.h"

class DeviceEditorBase : public juce::AudioProcessorEditor
{
public:
    DeviceEditorBase (EedDeviceProcessor& proc,
                      const juce::String& title,
                      int defaultWidth, int defaultHeight);
    ~DeviceEditorBase() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

protected:
    // Lay out this device's own controls. Called from resized() once the header
    // has been placed. Everything a device draws or positions belongs in here.
    virtual void layoutContent (juce::Rectangle<int> content) = 0;

    // Optional header controls, both filled from the RIGHT. Split in two because
    // BYPASS sits between them: a device-global output stage belongs outboard of
    // BYPASS (the EQ's OUT dial and auto-gain), while view toggles belong inboard
    // of it. `bar` is what remains after each call.
    //
    // Trailing gets the FULL header height, so a dial fits; leading is called
    // after the bar has been narrowed to button height, so buttons sit level with
    // the logo and the title.
    virtual void layoutHeaderTrailing (juce::Rectangle<int>& /*bar*/) {}
    virtual void layoutHeaderLeading  (juce::Rectangle<int>& /*bar*/) {}

    // Optional extra paint UNDER the header (grids, curves, meters).
    virtual void paintContent (juce::Graphics&) {}

    // A short line beside the title — usually the device's interaction hints.
    void setHeaderHint (const juce::String& h) { hint_ = h; repaint(); }

    juce::Rectangle<int> headerBounds()  const noexcept { return headerBounds_; }
    juce::Rectangle<int> contentBounds() const noexcept { return contentBounds_; }

    // The span in the header the title/hint may use: from just after the logo +
    // title, out to wherever the leftmost header control ended up. A subclass
    // painting in the header stays inside this and cannot collide.
    int headerTextLeft()  const noexcept { return textLeft_; }
    int headerTextRight() const noexcept { return textRight_; }

    juce::TextButton&   bypassButton() noexcept { return bypassBtn_; }
    EedDeviceProcessor& deviceProcessor() noexcept { return proc_; }

private:
    EedDeviceProcessor& proc_;
    juce::String        title_, hint_;

    // The device look is applied to the whole editor, so every child control
    // picks it up without each device re-styling.
    EchoJayLookAndFeel  lnf_;

    juce::TextButton    bypassBtn_ { "BYPASS" };

    juce::Rectangle<int> headerBounds_, contentBounds_;
    int textLeft_ = 0, textRight_ = 0;

    // True only for the window in which the base constructor's setSize() is
    // dispatching resized(), when layoutContent is still pure virtual. See the
    // constructor for why the content pass has to be skipped there.
    bool inBaseConstruction_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceEditorBase)
};
