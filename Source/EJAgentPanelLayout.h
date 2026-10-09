#pragma once
// =============================================================================
//  EJAgentPanelLayout — the agent panel's GEOMETRY as pure functions.
//
//  One author for every rect the panel paints or places a button in, so the
//  paint pass and the button placement cannot disagree (the AskShelfLayout /
//  layoutResultChips discipline), and so tools/agent_client_guard can assert
//  the no-truncation rule at every editor width without a window:
//    - a text row's height is MEASURED from a wrapped layout, never assumed;
//    - a right-hand button never overlaps the text it belongs to;
//    - choice pills flow into rows and wrap, never clip or stretch (18h rule).
//
//  Constants mirror the chat's own: 26 px chips at 12 pt, text + 24 wide,
//  at least 40, 8 px gap, 32 px row pitch; EchoJayChrome::kFieldCorner corners.
// =============================================================================
#include <JuceHeader.h>
#include <cmath>
#include <vector>

namespace echojay::agentui
{

struct Metrics
{
    static constexpr int pad        = 10;   // the card's inner padding
    static constexpr int headerH    = 28;   // status line + Stop
    static constexpr int footerH    = 36;   // the context buttons' row (0 when there are none)
    static constexpr int rowGap     = 6;
    static constexpr int glyphW     = 18;   // the tick / spinner column
    static constexpr int glyphGap   = 6;
    static constexpr int btnH       = 22;
    static constexpr int btnGap     = 8;
    static constexpr int undoW      = 52;
    static constexpr int toggleW    = 74;   // Approved / Skipped
    static constexpr int stopW      = 56;
    static constexpr int chipH      = 26;
    static constexpr int chipRowH   = 32;
    static constexpr int chipGap    = 8;
    static constexpr int chipMinW   = 40;
    static constexpr int scrollbarW = 8;
    static constexpr int indicatorW = 18;   // the listening ring
    static constexpr int minTextW   = 60;   // below this a row gives up its right button's width to the text
};

inline juce::Font bodyFont()   { return juce::Font (juce::FontOptions (13.0f)); }
inline juce::Font rowFont()    { return juce::Font (juce::FontOptions (12.5f)); }
inline juce::Font detailFont() { return juce::Font (juce::FontOptions (11.5f)); }
inline juce::Font chipFont()   { return juce::Font (juce::FontOptions (12.0f)); }
inline juce::Font headerFont() { return juce::Font (juce::FontOptions (12.0f, juce::Font::bold)); }

// The wrapped height of `text` at `width`. Never 0 for a non-empty text, and
// never less than the layout needs: this is the no-truncation rule's source.
inline int wrappedTextHeight (const juce::String& text, const juce::Font& font, int width)
{
    if (text.isEmpty() || width <= 0) return 0;
    juce::AttributedString as;
    as.setText (text);
    as.setFont (font);
    as.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout tl;
    tl.createLayout (as, (float) width);
    return (int) std::ceil (tl.getHeight()) + 2;
}

inline void drawWrapped (juce::Graphics& g, const juce::String& text, const juce::Font& font,
                         juce::Colour colour, juce::Rectangle<int> area)
{
    if (text.isEmpty() || area.isEmpty()) return;
    juce::AttributedString as;
    as.setText (text);
    as.setFont (font);
    as.setColour (colour);
    as.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout tl;
    tl.createLayout (as, (float) area.getWidth());
    tl.draw (g, area.toFloat());
}

// ---- chips (choice pills) -----------------------------------------------------
// The 18h rule, verbatim from layoutResultChips: natural width = text + 24, at
// least 40, at most the area width; a row wraps when the next chip would cross
// the right edge. Rects are relative to (x0, y0). Returns the total height.
inline int layoutChips (const juce::StringArray& labels, int x0, int y0, int width,
                        std::vector<juce::Rectangle<int>>& rectsOut)
{
    rectsOut.clear();
    if (labels.isEmpty() || width <= 0) return 0;
    const auto f = chipFont();
    int x = x0, y = y0;
    for (const auto& label : labels)
    {
        const int w = juce::jlimit (Metrics::chipMinW, juce::jmax (Metrics::chipMinW, width),
                                    f.getStringWidth (label) + 24);
        if (x > x0 && x + w > x0 + width) { x = x0; y += Metrics::chipRowH; }
        rectsOut.push_back ({ x, y, w, Metrics::chipH });
        x += w + Metrics::chipGap;
    }
    return (y - y0) + Metrics::chipRowH;
}

// ---- checklist rows ---------------------------------------------------------------
struct RowSpec
{
    juce::String label;      // the step's line (the server's summary)
    juce::String detail;     // the landed line / error / progress; may be empty
    int  rightButtonW = 0;   // 0 = no button (Undo, Approved/Skipped)
    bool hasGlyph = true;
};

struct RowRects
{
    juce::Rectangle<int> row;      // the whole row (for hit testing / background)
    juce::Rectangle<int> glyph;    // the tick column
    juce::Rectangle<int> label;    // wrapped text
    juce::Rectangle<int> detail;   // wrapped detail (empty when none)
    juce::Rectangle<int> button;   // the right-hand button (empty when none)
};

// Lays the rows down from (x0, y0) across `width`. Text wraps to whatever is
// left of the glyph column and the button; a button is only ever to the RIGHT
// of the text column and the two never share an x range. Returns the height.
inline int layoutRows (const std::vector<RowSpec>& rows, int x0, int y0, int width, std::vector<RowRects>& out)
{
    out.clear();
    int y = y0;
    for (const auto& r : rows)
    {
        RowRects rr;
        int x = x0;
        if (r.hasGlyph) { rr.glyph = { x, y, Metrics::glyphW, Metrics::btnH }; x += Metrics::glyphW + Metrics::glyphGap; }
        int btnW = r.rightButtonW > 0 ? r.rightButtonW : 0;
        int textW = (x0 + width) - x - (btnW > 0 ? btnW + Metrics::btnGap : 0);
        if (btnW > 0 && textW < Metrics::minTextW)
        {
            // too narrow to share: the button drops BELOW the text instead of eating it
            textW = (x0 + width) - x;
        }
        textW = juce::jmax (1, textW);
        const int labelH  = juce::jmax (Metrics::btnH, wrappedTextHeight (r.label, rowFont(), textW));
        const int detailH = r.detail.isNotEmpty() ? wrappedTextHeight (r.detail, detailFont(), textW) : 0;
        rr.label  = { x, y, textW, labelH };
        if (detailH > 0) rr.detail = { x, y + labelH, textW, detailH };
        int rowH = labelH + detailH;
        if (btnW > 0)
        {
            if (x + textW + Metrics::btnGap + btnW <= x0 + width)
                rr.button = { x0 + width - btnW, y, btnW, Metrics::btnH };
            else
            {
                rr.button = { x, y + rowH + 2, btnW, Metrics::btnH };
                rowH += Metrics::btnH + 2;
            }
        }
        rr.row = { x0, y, width, rowH };
        out.push_back (rr);
        y += rowH + Metrics::rowGap;
    }
    return rows.empty() ? 0 : (y - y0 - Metrics::rowGap);
}

// The invariants the guard asserts on a laid-out panel; returns the first
// violation as text, or "" when every rect passes. Shared with the panel's own
// debug build so the two cannot disagree about what "fits" means.
inline juce::String checkRows (const std::vector<RowSpec>& rows, const std::vector<RowRects>& rects, int x0, int width)
{
    if (rows.size() != rects.size()) return "row count mismatch";
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i]; const auto& rr = rects[i];
        if (rr.label.getX() < x0 || rr.label.getRight() > x0 + width) return "row " + juce::String ((int) i) + ": label leaves the column";
        if (rr.label.getHeight() < wrappedTextHeight (r.label, rowFont(), rr.label.getWidth())) return "row " + juce::String ((int) i) + ": label truncated";
        if (r.detail.isNotEmpty() && rr.detail.getHeight() < wrappedTextHeight (r.detail, detailFont(), rr.detail.getWidth())) return "row " + juce::String ((int) i) + ": detail truncated";
        if (! rr.button.isEmpty())
        {
            if (rr.button.getRight() > x0 + width || rr.button.getX() < x0) return "row " + juce::String ((int) i) + ": button leaves the column";
            if (rr.button.intersects (rr.label) || rr.button.intersects (rr.detail)) return "row " + juce::String ((int) i) + ": button over the text";
        }
        if (rr.glyph.intersects (rr.label)) return "row " + juce::String ((int) i) + ": glyph over the text";
        if (i > 0 && rects[i - 1].row.getBottom() > rr.row.getY()) return "row " + juce::String ((int) i) + ": rows overlap";
    }
    return {};
}

} // namespace echojay::agentui
