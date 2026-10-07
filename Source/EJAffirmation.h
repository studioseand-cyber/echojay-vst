#pragma once
// =============================================================================
//  "YES" - THE CLIENT'S NARROW HALF (06d test 5, 7 Oct 2026)
//
//  B's server-side rule is the SOURCE OF TRUTH for what counts as affirming an
//  offer (api/_offer-affirmed.js: AFFIRM_HEAD, AFFIRM_FILL, a typo reach, and a
//  by-reference form for "do what you suggested"). This is NOT a second
//  implementation of it and must never grow into one.
//
//  It exists only as a local FAST PATH: when the last reply carried a staged
//  proposal, a plain "yes" can be applied here with no model call at all. So
//  the asymmetry is deliberate and runs one way:
//    - a MISS costs a round trip. The turn goes to B exactly as it does today,
//      B's wider rule resolves it, and the ops come back. Nothing is lost.
//    - a FALSE POSITIVE writes a change the user did not ask for.
//  Therefore this list is short, every word of the message must be in it, one
//  negation anywhere refuses, and anything longer than six words refuses. When
//  in doubt it says no and lets the server decide.
// =============================================================================
#include <juce_core/juce_core.h>

namespace echojay
{

inline bool isProposalAffirmation (const juce::String& typed)
{
    // The first line only: "yes" followed by a new request is a request.
    const auto first = juce::StringArray::fromLines (typed)[0]
                           .toLowerCase()
                           .retainCharacters ("abcdefghijklmnopqrstuvwxyz' ")
                           .trim();
    if (first.isEmpty()) return false;

    auto words = juce::StringArray::fromTokens (first, " ", "");
    words.removeEmptyStrings();
    if (words.isEmpty() || words.size() > 6) return false;

    static const char* kNo[] = { "no", "nope", "nah", "not", "never", "dont", "don't",
                                 "stop", "wait", "hold", "cancel", "undo", "instead", nullptr };
    static const char* kHead[] = { "yes", "yeah", "yep", "yup", "ok", "okay", "sure",
                                   "do", "go", "apply", "please", "alright", "confirmed", nullptr };
    static const char* kFill[] = { "it", "that", "those", "them", "the", "both", "ahead", "on",
                                   "do", "go", "apply", "please", "yes", "ok", "okay", "sure",
                                   "change", "changes", "edit", "edits", "tweak", "tweaks", "one", nullptr };
    const auto inList = [] (const juce::String& w, const char* const* list)
    {
        for (int i = 0; list[i] != nullptr; ++i) if (w == list[i]) return true;
        return false;
    };

    for (const auto& w : words) if (inList (w, kNo)) return false;
    if (! inList (words[0], kHead)) return false;
    for (int i = 1; i < words.size(); ++i)
        if (! (inList (words[i], kHead) || inList (words[i], kFill))) return false;
    return true;
}

} // namespace echojay
