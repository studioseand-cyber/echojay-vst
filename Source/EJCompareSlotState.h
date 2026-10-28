#pragma once
#include <JuceHeader.h>

/** ---- WHAT A COMPARE SLOT IS (lifted out of EchoJayEditor, 28 Oct 2026) ----------------------

    Nine lines, no behaviour, and it was nested inside a 50,000-line class - which is fine until two
    files need it. `MatchHost` does: six of the calls the Match tab makes take one, and C2's rule is
    that `EJMatchTab.h` never includes `PluginEditor.h`, so the boundary cannot name a type that
    lives inside the editor.

    So it lives here, in the namespace, and `EchoJayEditor` keeps a `using` alias - every existing
    reference to `CompareSlotState` still compiles and means this. One definition, no call-site churn:
    a rename across hundreds of uses would be a day of risk for nothing. */
namespace echojay
{

struct CompareSlotState
{
    enum class Kind { Empty, Live, Snapshot, WsCapture, Reference, CodecFile };
    Kind kind = Kind::Empty;
    int  index = -1;          // Snapshot or Reference index
    juce::String wsReviewId;  // WsCapture: review ID from workspace
    juce::String label;       // display name shown in the slot button
    juce::String codecPath;   // CodecFile: rendered temp wav path
    std::vector<float> codecThumb;  // CodecFile: abs-peak thumbnail
};

} // namespace echojay
