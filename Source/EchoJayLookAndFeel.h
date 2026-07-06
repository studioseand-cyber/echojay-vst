#pragma once
#include <JuceHeader.h>
#include "EchoJayLogo.h"

class EchoJayLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Colour palette matching the web app CSS variables
    struct Colours {
        static inline const juce::Colour bg       { 0xff080A12 };
        static inline const juce::Colour bg2      { 0xff0A0C18 };
        static inline const juce::Colour bg3      { 0xff0E1020 };
        static inline const juce::Colour bg4      { 0xff141626 };
        static inline const juce::Colour text      { 0xfff0f0f5 };
        static inline const juce::Colour text2     { 0xffa0a0b8 };
        static inline const juce::Colour text3     { 0xff606078 };
        static inline const juce::Colour blue      { 0xff06b6d4 };
        static inline const juce::Colour blue2     { 0xff22d3ee };
        static inline const juce::Colour purple    { 0xff0891b2 };
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

    // ============ Tooltips ============
    // Dark navy panel, light small text, subtle cyan border. Max width 320
    // so longer meter explanations wrap; constrained within the parent so
    // tips never clip at the window edge.
    static constexpr int kTooltipMaxWidth = 320;

    static void layoutTooltipText(const juce::String& text, juce::TextLayout& tl)
    {
        juce::AttributedString s;
        s.append(text, juce::Font(juce::FontOptions(12.0f)), Colours::text);
        s.setLineSpacing(2.0f);
        tl.createLayout(s, (float)kTooltipMaxWidth - 18.0f);
    }

    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText,
                                          juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override
    {
        juce::TextLayout tl;
        layoutTooltipText(tipText, tl);
        int w = (int)std::ceil(tl.getWidth())  + 18;
        int h = (int)std::ceil(tl.getHeight()) + 14;
        return juce::Rectangle<int>(
                   screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 10) : screenPos.x + 16,
                   screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 4)  : screenPos.y + 20,
                   w, h)
               .constrainedWithin(parentArea);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        juce::Rectangle<float> b(0.0f, 0.0f, (float)width, (float)height);
        g.setColour(Colours::bg2);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(Colours::blue2.withAlpha(0.35f));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
        juce::TextLayout tl;
        layoutTooltipText(text, tl);
        tl.draw(g, b.reduced(9.0f, 7.0f));
    }

    // ============ Buttons ============
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, 
                               const juce::Colour& bgColour, bool isMouseOver, bool isButtonDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        float cornerSize = 6.0f;
        
        auto col = bgColour;
        if (isButtonDown)
            col = col.brighter(0.1f);
        else if (isMouseOver)
            col = col.brighter(0.05f);
        
        // Fully invisible overlay buttons — no painting at all
        if (button.getAlpha() < 0.01f)
            return;
        
        // Transparent buttons — square hover fill, no rounded corners
        if (col.getAlpha() < 10)
        {
            if (isMouseOver || isButtonDown)
            {
                g.setColour(juce::Colour(0xff06b6d4).withAlpha(isButtonDown ? 0.12f : 0.06f));
                g.fillRect(bounds);
            }
            return;
        }
        
        // Check if this is a primary button (purple-ish)
        bool isPrimary = (col.getGreen() > 150 && col.getBlue() > 150 && col.getRed() < 50);
        
        if (isPrimary)
        {
            // Dark teal glow button — subtle fill, brighter on hover
            g.setColour(juce::Colour(0xff06b6d4).withAlpha(isButtonDown ? 0.18f : (isMouseOver ? 0.14f : 0.08f)));
            g.fillRoundedRectangle(bounds, cornerSize);
        }
        else
        {
            // Secondary buttons — subtle solid fill
            g.setColour(col);
            g.fillRoundedRectangle(bounds, cornerSize);
            g.setColour(Colours::border2);
            g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
        }
    }
    
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                         bool isMouseOver, bool isButtonDown) override
    {
        auto font = juce::Font(juce::FontOptions(11.0f, juce::Font::bold));
        g.setFont(font);
        auto textCol = button.findColour(isButtonDown ? juce::TextButton::textColourOnId 
                                                      : juce::TextButton::textColourOffId);
        if (isMouseOver)
            textCol = textCol.brighter(0.15f);
        g.setColour(textCol);
        
        auto bounds = button.getLocalBounds();
        g.drawText(button.getButtonText(), bounds, juce::Justification::centred, true);
    }
    
    // ============ ComboBox ============
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        float cornerSize = 6.0f;
        
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle(bounds, cornerSize);
        
        g.setColour(Colours::border2);
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
        
        // Dropdown arrow — smaller, cleaner
        float arrowSize = 5.0f;
        float arrowX = (float)width - 16.0f;
        float arrowY = (float)height * 0.5f - 2.0f;
        juce::Path arrow;
        arrow.addTriangle(arrowX - arrowSize, arrowY,
                          arrowX + arrowSize, arrowY,
                          arrowX, arrowY + arrowSize);
        g.setColour(Colours::text3);
        g.fillPath(arrow);
    }
    
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(8, 0, box.getWidth() - 28, box.getHeight());
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
    }
    
    // ============ PopupMenu ============
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, (float)width, (float)height);
        g.setColour(juce::Colour(0xff0E1020));
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(juce::Colour(0xff141626));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
    }
    
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu,
                            const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        if (isSeparator)
        {
            auto sepArea = area.reduced(8, 0);
            g.setColour(juce::Colour(0xff141626));
            g.fillRect(sepArea.getX(), sepArea.getCentreY(), sepArea.getWidth(), 1);
            return;
        }
        
        auto r = area.reduced(4, 1);
        
        if (isHighlighted && isActive)
        {
            g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.2f));
            g.fillRoundedRectangle(r.toFloat(), 4.0f);
        }
        
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        
        if (!isActive)
            g.setColour(Colours::text3.withAlpha(0.4f));
        else if (isHighlighted)
            g.setColour(juce::Colours::white);
        else if (textColour != nullptr)
            g.setColour(*textColour);
        else
            g.setColour(Colours::text2);
        
        auto textArea = r.reduced(8, 0);
        
        if (isTicked)
        {
            g.setColour(juce::Colour(0xff06b6d4));
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText(juce::String(juce::CharPointer_UTF8("\xe2\x9c\x93")) + " " + text, 
                       textArea, juce::Justification::centredLeft, true);
        }
        else
        {
            g.drawText(text, textArea, juce::Justification::centredLeft, true);
        }
        
        if (hasSubMenu)
        {
            float arrowH = 6.0f;
            float arrowX = (float)r.getRight() - 10.0f;
            float arrowY = (float)r.getCentreY();
            juce::Path arrow;
            arrow.addTriangle(arrowX, arrowY - arrowH * 0.5f,
                              arrowX, arrowY + arrowH * 0.5f,
                              arrowX + 5.0f, arrowY);
            g.setColour(Colours::text3);
            g.fillPath(arrow);
        }
    }
    
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                    int standardMenuItemHeight, int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = 8;
        }
        else
        {
            auto font = juce::Font(juce::FontOptions(12.0f));
            idealWidth = font.getStringWidth(text) + 40;
            idealHeight = 26; // compact row height
        }
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
        
        // Pill-style toggle — glow when selected
        if (isOn)
        {
            g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.15f));
            g.fillRoundedRectangle(bounds, cornerSize);
            g.setColour(juce::Colour(0xff06b6d4).withAlpha(0.5f));
            g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
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
        // Use JUCE's reference-counted ImageCache rather than a function-local
        // static juce::Image. A static Image lives until DLL unload, and if it
        // still owns a GPU/OpenGL texture at that point, freeing it under the
        // Windows loader lock deadlocks the host (confirmed via hang dump:
        // HungIn_LoaderLock). ImageCache entries are released during normal
        // teardown by ImageCache::releaseUnusedImages(), so nothing GPU-backed
        // survives to DLL-unload time.
        juce::Image logoImg = juce::ImageCache::getFromMemory(echoJayLogoPNG, (int)echoJayLogoPNGSize);
        if (logoImg.isValid())
        {
            float aspect = (float)logoImg.getWidth() / (float)logoImg.getHeight();
            float drawH = bounds.getHeight() * 0.8f;
            float drawW = drawH * aspect;
            float x = bounds.getX();
            float y = bounds.getCentreY() - drawH / 2.0f;
            g.setOpacity(1.0f);
            g.drawImage(logoImg, 
                        juce::Rectangle<float>(x, y, drawW, drawH),
                        juce::RectanglePlacement::stretchToFit);
        }
        else
        {
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)));
            g.drawText("EchoJay", bounds, juce::Justification::centredLeft);
        }
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
            // STUDIO — purple-to-pink gradient pill (compact)
            auto bounds = juce::Rectangle<float>((float)x, (float)y, 36.0f, 14.0f);
            juce::ColourGradient grad(Colours::purple, bounds.getX(), bounds.getCentreY(),
                                       juce::Colour(0xFFE040A0), bounds.getRight(), bounds.getCentreY(), false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(bounds, 3.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
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
