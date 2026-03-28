#pragma once
#include <JuceHeader.h>

class EchoJayLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Colour palette matching the web app CSS variables
    struct Colours {
        static inline const juce::Colour bg       { 0xff0a0a0f };
        static inline const juce::Colour bg2      { 0xff12121a };
        static inline const juce::Colour bg3      { 0xff1a1a26 };
        static inline const juce::Colour bg4      { 0xff22222e };
        static inline const juce::Colour text      { 0xfff0f0f5 };
        static inline const juce::Colour text2     { 0xffa0a0b8 };
        static inline const juce::Colour text3     { 0xff606078 };
        static inline const juce::Colour blue      { 0xff3b82f6 };
        static inline const juce::Colour blue2     { 0xff60a5fa };
        static inline const juce::Colour purple    { 0xffa855f7 };
        static inline const juce::Colour green     { 0xff4ade80 };
        static inline const juce::Colour red       { 0xffef4444 };
        static inline const juce::Colour amber     { 0xfff59e0b };
        static inline const juce::Colour border    { juce::Colour::fromFloatRGBA(1, 1, 1, 0.05f) };
        static inline const juce::Colour border2   { juce::Colour::fromFloatRGBA(1, 1, 1, 0.1f) };
    };
    
    EchoJayLookAndFeel()
    {
        // Try to load DM Sans from the system, fall back to a clean sans-serif
        auto typeface = juce::Typeface::createSystemTypefaceFor(
            juce::Font(juce::FontOptions("DM Sans", 13.0f, juce::Font::plain)));
        if (typeface == nullptr)
            typeface = juce::Typeface::createSystemTypefaceFor(
                juce::Font(juce::FontOptions("SF Pro", 13.0f, juce::Font::plain)));
        if (typeface == nullptr)
            typeface = juce::Typeface::createSystemTypefaceFor(
                juce::Font(juce::FontOptions("Segoe UI", 13.0f, juce::Font::plain)));
        
        setDefaultSansSerifTypeface(typeface);
        
        // Global colour overrides
        setColour(juce::ResizableWindow::backgroundColourId, Colours::bg);
        setColour(juce::PopupMenu::backgroundColourId, Colours::bg3);
        setColour(juce::PopupMenu::textColourId, Colours::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, Colours::bg4);
        setColour(juce::PopupMenu::highlightedTextColourId, Colours::text);
        setColour(juce::ScrollBar::thumbColourId, Colours::bg4);
        setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    }
    
    // ============ Buttons ============
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, 
                               const juce::Colour& bgColour, bool isMouseOver, bool isButtonDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        float cornerSize = 8.0f;
        
        auto col = bgColour;
        if (isButtonDown)
            col = col.brighter(0.1f);
        else if (isMouseOver)
            col = col.brighter(0.05f);
        
        // Check if this is a "gradient" button (blue or purple primary)
        bool isGradient = (col.getRed() < 100 && col.getBlue() > 200) || 
                          (col.getRed() > 150 && col.getBlue() > 200);
        
        if (isGradient && col.getAlpha() > 200)
        {
            // Gradient fill for primary buttons
            juce::ColourGradient grad(Colours::blue, bounds.getX(), bounds.getY(),
                                      Colours::purple, bounds.getRight(), bounds.getBottom(), false);
            if (isMouseOver) grad = juce::ColourGradient(Colours::blue.brighter(0.1f), bounds.getX(), bounds.getY(),
                                                          Colours::purple.brighter(0.1f), bounds.getRight(), bounds.getBottom(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(bounds, cornerSize);
        }
        else
        {
            // Solid fill for secondary buttons
            g.setColour(col);
            g.fillRoundedRectangle(bounds, cornerSize);
            
            // Subtle border
            g.setColour(Colours::border2);
            g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
        }
    }
    
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                         bool isMouseOver, bool isButtonDown) override
    {
        auto font = juce::Font(juce::FontOptions(12.0f, juce::Font::bold));
        g.setFont(font);
        g.setColour(button.findColour(isButtonDown ? juce::TextButton::textColourOnId 
                                                    : juce::TextButton::textColourOffId));
        
        auto bounds = button.getLocalBounds();
        g.drawText(button.getButtonText(), bounds, juce::Justification::centred, true);
    }
    
    // ============ ComboBox ============
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        float cornerSize = 8.0f;
        
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle(bounds, cornerSize);
        
        g.setColour(Colours::border2);
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
        
        // Dropdown arrow
        auto arrowArea = bounds.removeFromRight(24.0f).reduced(8.0f, (float)height * 0.35f);
        juce::Path arrow;
        arrow.addTriangle(arrowArea.getX(), arrowArea.getY(),
                          arrowArea.getRight(), arrowArea.getY(),
                          arrowArea.getCentreX(), arrowArea.getBottom());
        g.setColour(Colours::text3);
        g.fillPath(arrow);
    }
    
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(8, 0, box.getWidth() - 28, box.getHeight());
        label.setFont(juce::Font(juce::FontOptions(12.0f)));
    }
    
    // ============ TextEditor ============
    void fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle(bounds, 8.0f);
    }
    
    void drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        bool focused = editor.hasKeyboardFocus(true);
        g.setColour(focused ? Colours::blue.withAlpha(0.5f) : Colours::border2);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, focused ? 1.5f : 1.0f);
    }
    
    // ============ ToggleButton ============
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        float cornerSize = 6.0f;
        
        bool isOn = button.getToggleState();
        
        // Pill-style toggle (like the web app's DAW selector)
        if (isOn)
        {
            g.setColour(Colours::blue);
            g.fillRoundedRectangle(bounds, cornerSize);
        }
        else
        {
            g.setColour(Colours::bg3);
            g.fillRoundedRectangle(bounds, cornerSize);
            g.setColour(Colours::border2);
            g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
        }
        
        if (shouldDrawButtonAsHighlighted && !isOn)
        {
            g.setColour(Colours::border2.brighter(0.1f));
            g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
        }
        
        g.setColour(isOn ? juce::Colours::white : Colours::text2);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::plain)));
        g.drawText(button.getButtonText(), bounds, juce::Justification::centred);
    }
    
    // ============ ScrollBar ============
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override
    {
        auto thumbBounds = isScrollbarVertical
            ? juce::Rectangle<float>((float)x + 2.0f, (float)thumbStartPosition, (float)width - 4.0f, (float)thumbSize)
            : juce::Rectangle<float>((float)thumbStartPosition, (float)y + 2.0f, (float)thumbSize, (float)height - 4.0f);
        
        g.setColour(Colours::bg4.withAlpha(isMouseOver ? 0.8f : 0.4f));
        g.fillRoundedRectangle(thumbBounds, 3.0f);
    }
    
    int getDefaultScrollbarWidth() override { return 8; }
    
    // ============ Label ============
    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        g.setColour(label.findColour(juce::Label::textColourId));
        g.setFont(label.getFont());
        
        auto textArea = label.getBorderSize().subtractedFrom(label.getLocalBounds());
        g.drawFittedText(label.getText(), textArea, label.getJustificationType(),
                         juce::jmax(1, (int)(textArea.getHeight() / label.getFont().getHeight())), 
                         label.getMinimumHorizontalScale());
    }
    
    // ============ Helpers for custom painting ============
    
    // Draw the gradient EchoJay logo
    static void drawLogo(juce::Graphics& g, juce::Rectangle<float> bounds, float fontSize = 20.0f)
    {
        juce::ColourGradient grad(Colours::blue, bounds.getX(), bounds.getCentreY(),
                                   Colours::purple, bounds.getRight(), bounds.getCentreY(), false);
        g.setGradientFill(grad);
        g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)));
        g.drawText("EchoJay", bounds, juce::Justification::centredLeft);
    }
    
    // Draw a meter value card (matching web app's meter card style)
    static void drawMeterCard(juce::Graphics& g, juce::Rectangle<float> bounds,
                               const juce::String& label, const juce::String& value,
                               juce::Colour valueColour, const juce::String& unit = "")
    {
        // Card background
        g.setColour(Colours::bg2);
        g.fillRoundedRectangle(bounds, 10.0f);
        g.setColour(Colours::border);
        g.drawRoundedRectangle(bounds, 10.0f, 1.0f);
        
        // Label (small, uppercase, muted)
        g.setColour(Colours::text3);
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText(label, bounds.reduced(12, 0).withHeight(24).translated(0, 6),
                   juce::Justification::centredLeft);
        
        // Value (large, coloured)
        g.setColour(valueColour);
        g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
        auto valueArea = bounds.reduced(12, 0).withTrimmedTop(24);
        g.drawText(value + (unit.isEmpty() ? "" : " " + unit), valueArea, 
                   juce::Justification::centredLeft);
    }
    
    // Draw a spectrum bar
    static void drawSpectrumBar(juce::Graphics& g, juce::Rectangle<float> bounds,
                                 float normValue, const juce::String& label)
    {
        float barH = bounds.getHeight() - 16.0f;
        float fillH = normValue * barH;
        
        auto barRect = juce::Rectangle<float>(
            bounds.getX() + 2, bounds.getY() + barH - fillH,
            bounds.getWidth() - 4, fillH);
        
        // Gradient bar
        juce::ColourGradient grad(Colours::blue.withAlpha(0.7f), 0, barRect.getBottom(),
                                   Colours::purple.withAlpha(0.7f), 0, barRect.getY(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(barRect, 3.0f);
        
        // Label
        g.setColour(Colours::text3);
        g.setFont(juce::Font(juce::FontOptions(8.0f)));
        g.drawText(label, bounds.getX(), bounds.getBottom() - 14, 
                   (int)bounds.getWidth(), 12, juce::Justification::centred);
    }
    
    // Draw noise/grain texture overlay
    static void drawGrainOverlay(juce::Graphics& g, juce::Rectangle<int> bounds, float opacity = 0.02f)
    {
        // Simple noise approximation using random dots
        juce::Random rng(42); // Fixed seed for consistency
        g.setColour(juce::Colours::white.withAlpha(opacity));
        for (int i = 0; i < bounds.getWidth() * bounds.getHeight() / 80; ++i)
        {
            float x = bounds.getX() + rng.nextFloat() * bounds.getWidth();
            float y = bounds.getY() + rng.nextFloat() * bounds.getHeight();
            g.fillRect(x, y, 1.0f, 1.0f);
        }
    }
    
    // Draw a section label (like "SPECTRUM", "CAPTURES" in the web app)
    static void drawSectionLabel(juce::Graphics& g, int x, int y, int w, const juce::String& text)
    {
        g.setColour(Colours::text3);
        g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        g.drawText(text, x, y, w, 14, juce::Justification::centredLeft);
    }
    
    // Draw form field label (like "YOUR NAME", "DAW(S)" in settings)
    static void drawFieldLabel(juce::Graphics& g, int x, int y, int w, const juce::String& text)
    {
        g.setColour(Colours::text3);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(text, x, y, w, 14, juce::Justification::centredLeft);
    }
    
    // Draw a capture row in the captures list
    static void drawCaptureRow(juce::Graphics& g, juce::Rectangle<float> bounds,
                                const juce::String& info, bool isSelected = false)
    {
        g.setColour(isSelected ? Colours::bg4 : Colours::bg3);
        g.fillRoundedRectangle(bounds, 6.0f);
        
        if (isSelected)
        {
            g.setColour(Colours::blue.withAlpha(0.3f));
            g.drawRoundedRectangle(bounds, 6.0f, 1.0f);
        }
        
        g.setColour(Colours::text2);
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(info, bounds.reduced(10, 0), juce::Justification::centredLeft);
    }
    
    // Draw a chat message bubble
    static void drawChatBubble(juce::Graphics& g, juce::Rectangle<float> bounds,
                                const juce::String& text, bool isAssistant)
    {
        g.setColour(isAssistant ? Colours::purple.withAlpha(0.08f) : Colours::bg4);
        g.fillRoundedRectangle(bounds, 8.0f);
        
        g.setColour(isAssistant ? Colours::text2 : Colours::text);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        
        auto textBounds = bounds.reduced(10, 8);
        g.drawFittedText(text, textBounds.toNearestInt(), juce::Justification::topLeft, 100);
    }
    
    // Draw tier badge (PRO / STUDIO / FREE)
    static void drawTierBadge(juce::Graphics& g, int x, int y, int tierLevel)
    {
        if (tierLevel >= 2)
        {
            // STUDIO — purple-to-pink gradient pill
            auto bounds = juce::Rectangle<float>((float)x, (float)y, 52.0f, 16.0f);
            juce::ColourGradient grad(Colours::purple, bounds.getX(), bounds.getCentreY(),
                                       juce::Colour(0xFFE040A0), bounds.getRight(), bounds.getCentreY(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText("STUDIO", bounds, juce::Justification::centred);
        }
        else if (tierLevel >= 1)
        {
            // PRO — blue-to-purple gradient pill
            auto bounds = juce::Rectangle<float>((float)x, (float)y, 36.0f, 16.0f);
            juce::ColourGradient grad(Colours::blue, bounds.getX(), bounds.getCentreY(),
                                       Colours::purple, bounds.getRight(), bounds.getCentreY(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawText("PRO", bounds, juce::Justification::centred);
        }
        // tierLevel 0 (free) — no badge drawn here, handled by caller
    }

    // Legacy wrapper for compatibility
    static void drawProBadge(juce::Graphics& g, int x, int y)
    {
        drawTierBadge(g, x, y, 1);
    }
};
