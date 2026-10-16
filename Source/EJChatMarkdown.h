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
    // ---- SEAN, 15 OCT 2026: AN UNDERSCORE INSIDE A WORD IS A CHARACTER, NOT EMPHASIS -------------
    //
    // "88_Omnisphere_01" rendered as italic "88Omnisphere01". Track and plugin names are full of
    // underscores - 88_7even Kick_2_01, Aitch_4_01 - and this renderer was eating them, which means
    // the chat has been quietly misspelling the names of the things it is talking about.
    //
    // The rule is CommonMark's own, and it exists for exactly this reason: `_` only opens or closes
    // emphasis at a WORD BOUNDARY, while `*` may do so inside a word. So snake_case survives and
    // _italic_ still works, with no escaping anywhere and nothing for a caller to remember.
    //
    // A GUARD LEG HAD THE OLD BEHAVIOUR WRITTEN DOWN AS CORRECT ("a_b_c" -> "abc"). That is what a
    // test is for: the contract changed by Sean's ruling, so the leg changed with it and says why.
    juce::juce_wchar prevCh = 0;

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
        { flush(); c = ! c; prevCh = ch; ++p; continue; }
        if (! c && ch == '*' && rest.startsWith ("**"))
        {
            // A `**` is EITHER a bold marker or two literal asterisks - never one italic marker followed by
            // another. Caught by the guard: "gain **down" rendered as "gain down", because the bold rule
            // correctly declined (no closing **) and the italic rule below then opened on the first star and
            // closed on the second, deleting both. So `**` is decided here and never falls through.
            if (b || closes (rest.substring (2), "**")) { flush(); b = ! b; prevCh = ch; p += 2; continue; }
            run += "**"; prevCh = ch; p += 2; continue;
        }
        if (! c && ch == '*' && (i || closes (rest.substring (1), "*")))
        { flush(); i = ! i; prevCh = ch; ++p; continue; }
        if (! c && ch == '_' && (i || closes (rest.substring (1), "_")))
        {
            // At a word boundary? Opening looks BACK (nothing alphanumeric before it), closing looks
            // FORWARD (nothing alphanumeric after it). An underscore that fails its test is a literal
            // character and falls through, which is what keeps 88_Omnisphere_01 intact.
            const auto nextCh = rest.length() > 1 ? rest[1] : juce::juce_wchar (0);
            const bool boundary = i ? ! juce::CharacterFunctions::isLetterOrDigit (nextCh)
                                    : ! juce::CharacterFunctions::isLetterOrDigit (prevCh);
            if (boundary) { flush(); i = ! i; prevCh = ch; ++p; continue; }
        }
        run += juce::String::charToString (ch);
        prevCh = ch;
        ++p;
    }
    flush();
}

} // namespace echojay
