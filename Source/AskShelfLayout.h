#pragma once
// AskShelfLayout — round C (17 Sep 2026): the ask shelf / brief card sits on
// the composer and never extends past the chat column's right edge. The card
// took the composer's width (chatBoxRect_), which can be wider than the
// transcript column when a side panel is open (clipped at x~1615 of 2000 in
// Sean's session). Pinned by tools/ui_round_guard.
#include <JuceHeader.h>

inline juce::Rectangle<int> askShelfBounds (juce::Rectangle<int> chatBox, juce::Rectangle<int> chatColumn, int height)
{
    const int x = chatBox.getX();
    const int w = juce::jmax (0, juce::jmin (chatBox.getWidth(), chatColumn.getRight() - x));
    return { x, chatBox.getY() - height, w, height };
}
