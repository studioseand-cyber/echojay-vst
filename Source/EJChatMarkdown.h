#pragma once

#include <juce_graphics/juce_graphics.h>

// ===========================================================================
//  EJChatMarkdown - the chat's inline markdown renderer
// ===========================================================================
//
// Its own header for ONE reason: so a guard can drive it. As a member of
// EchoJayEditor it was only reachable by linking the whole editor, which is
// how a renderer ends up with no test - and this one has to be identical for
// the paint pass and the height-measure pass or the scroll range drifts, which
// is exactly the kind of claim that needs a test rather than a comment.

namespace echojay
{

// ---- ITEM 3b (Sean, 13 Oct 2026): MARKDOWN IN A CHAT BUBBLE ---------------------------------------------
//
// Sean saw literal ** marks in agent replies. Not an agent bug: NO assistant bubble has ever rendered
// markdown - the chat builds one AttributedString with a single append, so whatever the model emitted was
// painted character for character. The agent made it visible because its replies are the ones that
// routinely come back with emphasis in them.
//
// ONE function, called by BOTH the paint pass and the height-measure pass. That is the whole reason it
// exists as a function: the two passes carry a comment saying they must match, and a renderer that was
// inlined twice would be the next thing to drift. Deliberately small - **bold**, *italic*, _italic_ and
// `code` - because a bubble is prose with emphasis, not a document. An unmatched marker is left as a
// literal character, so nothing a model writes can swallow the rest of a reply.
inline void appendChatMarkdown (juce::AttributedString& as, const juce::String& text,
                                float fontSize, juce::Colour colour)
{
    // Braces, not parentheses: `juce::Font plain (juce::FontOptions (fontSize))` declares a FUNCTION named
    // plain taking a FontOptions called fontSize. The compiler is right and the line looks correct, which is
    // the whole trouble with it.
    const juce::Font plain { juce::FontOptions (fontSize) };
    const juce::Font bold  { juce::FontOptions (fontSize, juce::Font::bold) };
    const juce::Font ital  { juce::FontOptions (fontSize, juce::Font::italic) };
    const juce::Font mono  { juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), fontSize, juce::Font::plain) };

    auto p = text.getCharPointer();
    juce::String run;
    bool b = false, i = false, c = false;

    auto flush = [&]
    {
        if (run.isEmpty()) return;
        as.append (run, c ? mono : (b ? bold : (i ? ital : plain)), colour);
        run.clear();
    };
    // A marker only OPENS when its partner exists later in the remaining text; otherwise the character is
    // just a character. Without that test, "2 * 3" or a lone asterisk would italicise the rest of a reply.
    auto closes = [] (const juce::String& rest, const juce::String& marker)
    { return rest.indexOf (marker) >= 0; };

    while (! p.isEmpty())
    {
        const auto ch = *p;
        const juce::String rest (p);
        if (ch == '`' && (c || closes (rest.substring (1), "`")))
        { flush(); c = ! c; ++p; continue; }
        if (! c && ch == '*' && rest.startsWith ("**"))
        {
            // A `**` is EITHER a bold marker or two literal asterisks - never one italic marker followed by
            // another. Caught by the guard: "gain **down" rendered as "gain down", because the bold rule
            // correctly declined (no closing **) and the italic rule below then opened on the first star and
            // closed on the second, deleting both. So `**` is decided here and never falls through.
            if (b || closes (rest.substring (2), "**")) { flush(); b = ! b; p += 2; continue; }
            run += "**"; p += 2; continue;
        }
        if (! c && ch == '*' && (i || closes (rest.substring (1), "*")))
        { flush(); i = ! i; ++p; continue; }
        if (! c && ch == '_' && (i || closes (rest.substring (1), "_")))
        { flush(); i = ! i; ++p; continue; }
        run += juce::String::charToString (ch);
        ++p;
    }
    flush();
}

} // namespace echojay
