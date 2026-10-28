#pragma once
#include <JuceHeader.h>

/** ---- NOTHING CLAIMS "ADDED" WHILE THE ADD IS STILL WAITING (28 Oct 2026, Sean's point 2) -----

    SCEN RP4 caught it: while a card sat in `awaiting_approval` the screen read *"I added a Tube-Tech
    CL 1B compressor to the kick with threshold around -15 dB, ratio 4:1..."*. Nothing had been added.
    The row had been passing for want of a world - with no live Link the replay never reached that
    state - and the reader fix of 28 Oct gave it one.

    Sean's rule: it must say **planned** (or similar) until the plugin confirms, and the same rule
    holds for the plan card (A2) and the walk-through (A5), which are not built yet. So the rule is
    written here, once, as a pure function: the places that draw a pending line call it, and a guard
    drives it in both directions.

    WHAT THIS DOES NOT DO: it never invents a claim, never changes a line that is already honest, and
    never touches a line whose work HAS landed. It rewrites one tense, on a line that is waiting, and
    leaves every figure in the sentence exactly where it was - the numbers are the model's and they
    are what the user is approving. */
namespace echojay
{

/** Turn a claim made in the past tense into the tense a waiting line is entitled to.

    `line`    the sentence as written (the model's words, or ours)
    `pending` true while the work is still waiting for approval or for the plugin's confirmation */
inline juce::String pendingClaimText (const juce::String& line, bool pending)
{
    if (! pending || line.isEmpty()) return line;

    struct Swap { const char* was; const char* now; };
    // Only verbs that CLAIM the work is done. "Adding" or "will add" are already honest and are left
    // alone, as is anything that was never a claim. The list is deliberately short: a long list of
    // clever rewrites would start changing sentences nobody complained about.
    static const Swap swaps[] = {
        { "I added",     "I'll add"     }, { "I've added",  "I'll add"     },
        { "I removed",   "I'll remove"  }, { "I've removed","I'll remove"  },
        { "I replaced",  "I'll replace" }, { "I set",       "I'll set"     },
        { "I've set",    "I'll set"     }, { "I changed",   "I'll change"  },
        { "I built",     "I'll build"   }, { "I dialled",   "I'll dial"    },
        { "Added",       "Planned"      }, { "Removed",     "To remove"    },
        { "Replaced",    "To replace"   }, { "Built",       "Planned"      },
    };
    for (const auto& s : swaps)
        if (line.startsWith (s.was))
            return juce::String (s.now) + line.substring ((int) juce::String (s.was).length());
    return line;
}

/** Does this line claim, in the past tense, that work is done? Used to decide whether a WAITING row
    needs the rewrite at all - and by a row that would rather refuse than rewrite. */
inline bool claimsItIsDone (const juce::String& line)
{
    return pendingClaimText (line, true) != line;
}

} // namespace echojay
