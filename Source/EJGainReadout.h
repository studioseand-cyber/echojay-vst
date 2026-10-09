#pragma once
// =============================================================================================================
//  THE PER-SLOT IN / OUT READOUT, shared by BOTH editors (8 Oct 2026, 06d item 5).
//
//  Introduced for the V2's slot cards by the 30 Sep 2026 ruling ("IN AND OUT ON EVERY SLOT CARD") and lived as a
//  nested struct inside EchoJayEditor::ChainListPanel. Sean, 10:58 on 08a: the Link's own window shows the same
//  slot cards with NO readouts, and `git log -S` over Source/LinkEditor.h proves it was never there - not a
//  regression, simply never built. The ruling is that both views show the same readouts, from the same source of
//  truth, and behave the same.
//
//  SO IT IS ONE STRUCT, NOT TWO. Copying a hundred lines into the Link's editor would satisfy the ruling on the
//  day and then drift: the next change to the drag idiom, the entry box or the format would land on one card and
//  not the other, and "the same readouts" would quietly stop being true. There is one definition, both editors
//  construct it, and the guard compares readoutText() slot by slot so a divergence cannot pass.
//
//  WHAT IT IS: "IN +0.0" / "OUT -6.0", dB to one decimal, read from a live getter every tick (so a write by the
//  loop, the hold, the chat or the user's own drag all show the same way - there is one value, not a copy), and
//  interactive where a setter is given: vertical drag at 0.1 dB/px, 0.02 with shift (the knobs' idiom),
//  double-click to type, and the mouse wheel. No setter = read-only, which is what a remote rack gets.
// =============================================================================================================
#include <juce_gui_basics/juce_gui_basics.h>

namespace echojay
{

struct GainReadout : juce::Component, private juce::Timer
{
    juce::String tag;                       // "IN" / "OUT"
    // The dim colour at 0.0 dB. A member rather than a look-and-feel reference, because this readout is now
    // shared by two editors whose palettes differ; each sets it once where it builds the card.
    juce::Colour dimColour { juce::Colour (0xffb8c0d0).withAlpha (0.45f) };
    std::function<float()>      get;
    std::function<void(float)>  set;
    static constexpr float kMinDb = -24.0f, kMaxDb = 12.0f;   // the trims' own range, both ends
    float shown = 0.0f;
    juce::TextEditor entry;

    GainReadout()
    {
        setInterceptsMouseClicks (true, true);
        entry.setVisible (false);
        entry.setJustification (juce::Justification::centred);
        entry.setFont (juce::Font (juce::FontOptions (8.0f)));
        entry.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0b1220));
        entry.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff22d3ee));
        entry.setColour (juce::TextEditor::textColourId,       juce::Colour (0xff22d3ee));
        entry.onReturnKey = [this] { commitEntry(); };
        entry.onEscapeKey = [this] { entry.setVisible (false); repaint(); };
        entry.onFocusLost = [this] { if (entry.isVisible()) commitEntry(); };
        addChildComponent (entry);
        startTimerHz (20);
    }
    ~GainReadout() override { stopTimer(); }

    void commitEntry()
    {
        const auto t = entry.getText().trim().removeCharacters ("dB ").trim();
        if (t.isNotEmpty() && set)
            set (juce::jlimit (kMinDb, kMaxDb, t.getFloatValue()));
        entry.setVisible (false);
        refreshNow();
    }
    void refreshNow() { if (get) shown = get(); repaint(); }
    void timerCallback() override
    {
        if (! get || entry.isVisible()) return;
        const float v = get();
        if (std::abs (v - shown) > 0.005f) { shown = v; repaint(); }
    }
    void resized() override { entry.setBounds (getLocalBounds()); }

    juce::String valueText() const
    { return (shown >= 0.0f ? "+" : "") + juce::String (shown, 1); }
    /** The whole readout, for the guard: "IN +0.0" / "OUT -6.0". */
    juce::String readoutText() const { return tag + " " + valueText(); }
    bool isMoved() const { return std::abs (shown) > 0.05f; }

    void paint (juce::Graphics& g) override
    {
        if (entry.isVisible()) return;
        // Accent while the slot has been MOVED, dim at 0.0, so which slots the loop has touched is
        // readable at a glance without reading the numbers.
        g.setColour (isMoved() ? juce::Colour (0xff22d3ee)
                               : dimColour);
        g.setFont (juce::Font (juce::FontOptions (7.0f, isMoved() ? juce::Font::bold : juce::Font::plain)));
        g.drawText (readoutText(), 0, 0, getWidth(), getHeight(), juce::Justification::centredRight, false);
    }

    // ---- the gesture. A drag here never reaches the card, so it cannot select the slot or scroll the
    // strip: a child component takes the mouse first, which is what keeps this off every existing one.
    float dragFrom = 0.0f; int dragFromY = 0; bool dragged = false;
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragFrom = get ? get() : 0.0f; dragFromY = e.getPosition().y; dragged = false;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! set) return;
        dragged = true;
        // 0.1 dB per pixel, up is louder. Fine drag with shift, the same idiom as the knobs.
        const float perPx = e.mods.isShiftDown() ? 0.02f : 0.1f;
        const float want = juce::jlimit (kMinDb, kMaxDb,
                                         dragFrom + (float) (dragFromY - e.getPosition().y) * perPx);
        set (want);
        refreshNow();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! set) return;
        entry.setText (juce::String (shown, 1), false);
        entry.setVisible (true);
        entry.selectAll();
        entry.grabKeyboardFocus();
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (! set) return;
        set (juce::jlimit (kMinDb, kMaxDb, shown + (w.deltaY > 0 ? 0.1f : -0.1f)));
        refreshNow();
    }
};

/** 08c item B (9 Oct 2026): WHERE THE TWO READOUTS GO ON A CARD OF THIS WIDTH, decided once for both editors.
    The V2 puts them top-right and reserves the column in its name box; the Link drew its name across the FULL
    width and the readouts landed on top of it - Sean's Tube-Tech CL 1B, UAD UA 1176LN and NLS Buss were all
    unreadable on 8 Oct. "Identical rects" was the invariant I gave the Link and it was the wrong one: the rects
    already matched. What has to match is THE READING, and that is the name box as much as the readouts.

    I tried giving a 118 px card its own row for the readouts, under a full-width name, and withdrew it: both
    cards carry a wet/dry knob centred at y 17-42, so any extra row runs straight through it. The V2's geometry -
    a 46 px column top-right, the name yielding it - is the one that has shipped and that Sean passed, so it is
    now the only geometry, and the Link adopts the whole of it: reserved name box, and the pop-out glyph moved
    to the top-left where the V2 moved it in Build 2 instead of sitting under the readouts.

    `name`, `in` and `out` are the three rects: the name's text box and the two readouts. `nameLeftInset` is the
    editor's own left margin (16 on the V2's left-aligned name, 6 on the Link's centred one). */
struct CardReadoutLayout
{
    juce::Rectangle<int> name, in, out;
};
inline CardReadoutLayout layOutCardReadouts (int cardW, int nameLeftInset)
{
    constexpr int kReadoutW = 46, kReadoutH = 9, kGutter = 2;
    CardReadoutLayout l;
    const int colX = cardW - (kReadoutW + kGutter);
    l.name = { nameLeftInset, 3, juce::jmax (8, colX - nameLeftInset), 18 };
    l.in   = { colX, 2,  kReadoutW, kReadoutH };
    l.out  = { colX, 11, kReadoutW, kReadoutH };
    return l;
}

} // namespace echojay
