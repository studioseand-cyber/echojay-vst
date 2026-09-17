#pragma once
// ChatBubbleStyle (hurdle 1 item 3, 17 Sep 2026): the ONE rule for a chat
// bubble's text colour. A local result bubble that reports a NOT DIALABLE slot
// under dial-only is painted in the house coral (the same colour the codec
// panel's error line uses), never in the assistant grey - "in red, not silently
// prose". Pure, so tools/ui_round_guard asserts it directly.
#include <JuceHeader.h>

inline juce::Colour chatBubbleWarningColour() noexcept { return juce::Colour (0xfff87171); }   // coral

inline juce::Colour chatBubbleTextColour (bool isUser, bool dialWarning,
                                          juce::Colour userText, juce::Colour assistantText) noexcept
{
    if (dialWarning && ! isUser) return chatBubbleWarningColour();
    return isUser ? userText : assistantText;
}
