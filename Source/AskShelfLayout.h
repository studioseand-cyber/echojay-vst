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
    // ---- THE TOP STAYS INSIDE THE WINDOW (20 Oct 2026, B's six-chip capture) -------------------
    //
    // B: *"The agent-off pick card with SIX chips was drawn with none at all (five drew fine)."*
    //
    // The shelf sits ON the composer, so its top is `chatBox.getY() - height` and it grows UPWARDS.
    // Five chips are one row (76 px) and fit; six at a narrow column are two to six rows - the legs
    // measured 208 px at 240 px wide - and the top goes NEGATIVE. Every chip is then placed at a
    // negative offset, off the top of the window, which is "drawn with none at all" exactly.
    //
    // The CHAT COLUMN is the space there is, so the top is clamped to it and the height taken from
    // what is left. A shelf that cannot fit is shown as tall as it can be rather than off-screen:
    // the chips inside it then flow against the same width and the top rows stay reachable.
    const int topLimit = chatColumn.getY();
    const int wantTop  = chatBox.getY() - height;
    const int top      = juce::jmax (topLimit, wantTop);
    const int h        = juce::jmax (0, chatBox.getY() - top);
    return { x, top, w, h };
}

// ---- THE CHIP FLOW, AS A PURE FUNCTION (20 Oct 2026, B's six-chip capture) -------------------
//
// B: *"The agent-off pick card with SIX chips was drawn with none at all (five drew fine at 09:32).
// The chip row must lay out any number, wrapping to a second row, never drop the lot."*
//
// This lived inside EchoJayEditor::measureAskShelf, and the only guard that could reach it is
// ui_guard - which instantiates the whole editor and SIGSEGVs at startup in a session with no
// window server (verified at HEAD, without any change of mine). So the one thing that could settle
// B's report was untestable, and Sean's ruling was to extract it.
//
// THE ONLY IMPURE PART IS THE TEXT WIDTH, so that comes in as an oracle: the editor passes the real
// font's measurement, a guard passes a deterministic stub. Everything else - the padding, the chip
// width clamp, the wrap and the total height - is arithmetic, and arithmetic can be asserted.
//
// `chipH` is the caller's (EchoJayEditor::kAskChipH) rather than duplicated here, so the pill height
// has one definition.
struct AskChipFlow
{
    std::vector<juce::Rectangle<int>> rects;   // one per label, in order, relative to the shelf
    int height = 0;                            // what the shelf must reserve; 0 = nothing to draw
    int rows   = 0;                             // how many lines the chips flowed onto
};

inline AskChipFlow layOutAskChips (const juce::StringArray& labels, int shelfW, int chipH,
                                   const std::function<int (const juce::String&)>& textWidth)
{
    AskChipFlow f;
    if (labels.isEmpty() || shelfW <= 0 || chipH <= 0 || ! textWidth) return f;
    const int padX = 10, padY = 8, gap = 6;
    const int availR = juce::jmax (90, shelfW - padX);
    int x = padX, y = padY;
    int lastY = -1;
    for (const auto& label : labels)
    {
        if (label.trim().isEmpty()) continue;
        // THE CLAMP'S FLOOR MATTERS AS MUCH AS ITS CEILING: a chip is never narrower than 56 and
        // never wider than the shelf's usable width, so a very long label is cut to fit rather than
        // pushed off the edge - which is what would make it look "dropped".
        const int w = juce::jlimit (56, juce::jmax (56, availR - padX), textWidth (label) + 28);
        if (x > padX && x + w > availR) { x = padX; y += chipH + gap; }   // flow onto the next row
        f.rects.push_back ({ x, y, w, chipH });
        if (y != lastY) { ++f.rows; lastY = y; }
        x += w + gap;
    }
    if (f.rects.empty()) return f;
    f.height = y + chipH + padY;
    return f;
}
