#pragma once
// =============================================================================================
//  EJDontUse - "don't use that one", parsed (his 11:5x item 3, 21 Oct 2026)
// =============================================================================================
//  *"The MLimiterX is an expired demo. Can the plugin tell (it passes audio, so silent-slot
//  detection won't)? If not, give Sean a way to mark a plugin "don't use", stored like the silent
//  mark, and send it so B leaves it out."*
//
//  A HOST CANNOT TELL. It sees parameters and audio, and an expired demo that passes audio is
//  indistinguishable from a licensed one in both - which is exactly why the silent-slot detector
//  cannot see this case: silence is the only licence symptom a host can measure. So the mark is his.
//
//  AND THE PARSE LIVES HERE, apart from the editor, because this verb CHANGES A STORED SETTING. The
//  loudness verbs can afford a loose phrase match - a miss costs a round trip - but a false positive
//  here takes a plugin out of every future build on the strength of a sentence that merely mentioned
//  not using something. So the rule is narrow, it is in one place, and a guard holds both directions:
//  the sentences that must mark, and the sentences that must not.
#include <JuceHeader.h>

namespace echojay::dontuse
{

struct Parsed
{
    bool         marks = false;   // "don't use the MLimiterX" / "don't use it again"
    bool         lifts = false;   // "use MLimiterX again"
    juce::String name;            // what was left after the verb, or empty for "it"/"this"/"that"
    bool took() const noexcept { return marks || lifts; }
};

/** The verb, or nothing. THE SHAPE IS THE GATE: the sentence has to BEGIN with the verb, because
    that is what makes it an instruction rather than a remark ("I don't use that one much" begins
    with "i"). A mark may name a plugin or say "it"; a LIFT must name one, since there is nothing for
    "it" to refer to once the plugin is out of the rack. */
inline Parsed parse (const juce::String& msgIn)
{
    Parsed p;
    auto t = msgIn.trim().toLowerCase();
    while (t.endsWithChar ('.') || t.endsWithChar ('!') || t.endsWithChar ('?'))
        t = t.dropLastCharacters (1).trim();
    if (t.isEmpty()) return p;
    // A QUESTION IS NOT AN INSTRUCTION. "don't use it again?" is him asking what I think.
    if (msgIn.trim().endsWithChar ('?')) return p;

    static const char* kMark[] = { "don't use", "dont use", "do not use", "never use", "stop using" };
    juce::String rest;
    for (const char* lead : kMark)
        if (t.startsWith (lead)) { p.marks = true; rest = t.substring ((int) juce::String (lead).length()); break; }
    if (! p.marks && t.startsWith ("use "))
    {
        // A LIFT, and only with its tail: bare "use the CL 1B" is a request to USE it, which is
        // the server's business and not a settings change.
        if (t.endsWith (" again") || t.endsWith (" once more"))
        { p.lifts = true; rest = t.substring (4); }
    }
    if (! p.took()) return p;

    // What is left is the plugin, with the filler words taken out. "it"/"this"/"that" is not a name;
    // the caller resolves those from what the loop just complained about, or asks.
    rest = rest.replace (" again", " ").replace (" once more", " ").replace (" any more", " ")
               .replace (" anymore", " ");
    juce::StringArray words;
    words.addTokens (rest, " ", {});
    words.removeEmptyStrings();
    static const char* kFiller[] = { "the", "this", "that", "it", "them", "those", "these", "a", "an",
                                     "plugin", "please", "one", "ever", "my" };
    juce::StringArray kept;
    for (const auto& w : words)
    {
        bool drop = false;
        for (const char* f : kFiller) if (w == f) { drop = true; break; }
        if (! drop) kept.add (w);
    }
    p.name = kept.joinIntoString (" ").trim();
    // A LIFT WITH NO NAME TAKES NOTHING. "use it again" has no referent this side can trust.
    if (p.lifts && p.name.isEmpty()) { p.lifts = false; return Parsed{}; }
    return p;
}

}   // namespace echojay::dontuse
