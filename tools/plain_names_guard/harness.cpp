// plain_names_guard (16 Oct 2026): SEAN'S #5 - the not-confirmed line says "the frequency, gain and Q of
// both bands", never "band1.freq_hz, band1.gain_db, band1.q".
//
// WHY THIS GUARD EXISTS AT ALL, AND WHY IT IS LATE. EJParamPlainNames.h was given its own header for one
// stated reason - *"so a guard can drive it. A renderer reachable only by linking the executor is a
// renderer with no test, which is how the chat's markdown ended up eating Sean's track names"* - and then
// no guard was written. Tonight, writing row T1, I found the renderer was being fed the WRONG LIST: the
// executor handed it `missed`, whose entries are whole sentences ("slot 1 attack_db is 0.00 (asked 6.00)"),
// and the renderer takes the last whitespace token out of a spaced string, so Sean's line would have read
// "6.00)". The header's own comment predicted exactly this and the test it asked for was never there.
//
// SO IT DRIVES BOTH HALVES: the renderer itself, AND the contract that it is fed bare ids. The second half
// is the one that actually broke, and a guard on the renderer alone would have been green through it.
#include <JuceHeader.h>
#include "EJParamPlainNames.h"
#include <cstdio>
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(),
                 d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
juce::String p (const juce::StringArray& ids) { return echojay::params::plainParamList (ids); }
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("plain_names_guard: parameter ids become words, and groups collapse (Sean's #5)\n");

    // ---- SEAN'S OWN EXAMPLE, VERBATIM. This is the whole specification. ---------------------------
    check (p ({ "band1.freq_hz", "band1.gain_db", "band1.q",
                "band2.freq_hz", "band2.gain_db", "band2.q" })
             == "the frequency, gain and Q of both bands",
           "his example: six ids over two bands -> \"the frequency, gain and Q of both bands\"",
           p ({ "band1.freq_hz", "band1.gain_db", "band1.q", "band2.freq_hz", "band2.gain_db", "band2.q" }));

    check (p ({ "band1.freq_hz", "band1.q" }) == "the frequency and Q of band 1",
           "one band, two controls -> named singly", p ({ "band1.freq_hz", "band1.q" }));

    check (p ({ "band1.gain_db", "band2.gain_db", "band3.gain_db" }) == "the gain of all 3 bands",
           "beyond two, say how many", p ({ "band1.gain_db", "band2.gain_db", "band3.gain_db" }));

    check (p ({ "threshold_db", "ratio", "attack_ms" }) == "the threshold, ratio and attack",
           "bare leaves, no group", p ({ "threshold_db", "ratio", "attack_ms" }));

    // ---- NO RAW ID SURVIVES, which is the thing Sean actually asked for -------------------------
    {
        const juce::StringArray ids { "band1.freq_hz", "band2.gain_db", "attack_ms", "wobble_factor_hz",
                                      "pre_gain_db", "hpf_hz" };
        const auto out = p (ids);
        bool clean = true;
        for (const auto& id : ids) if (out.contains (id)) clean = false;
        check (clean && ! out.containsChar ('_') && ! out.contains ("_hz") && ! out.contains ("_db"),
               "not one raw id, and not one underscore, reaches the sentence", out);
        // An id the table does not know is still NAMED - never dropped, because an unnamed failure is
        // worse than an awkward one.
        check (out.contains ("wobble factor"), "an unknown id is opened out into words, not dropped", out);
    }

    // ---- THE CONTRACT: THE RENDERER IS FED BARE IDS ---------------------------------------------
    //
    // THE BUG THIS GUARD WAS WRITTEN FOR. The executor's `missed` list holds SENTENCES. Handing one to
    // the renderer is a mistake the renderer cannot detect and cannot repair, so what it must do is be
    // visibly, unmistakably wrong rather than plausibly wrong - and it is: the last whitespace token.
    // This leg pins that, so anyone tempted to "make the renderer cope" sees that coping is the fault.
    {
        const auto wrong = p ({ "slot 1 attack_db is 0.00 (asked 6.00)" });
        check (! wrong.contains ("attack"),
               "a whole SENTENCE handed to the renderer does NOT come out as a name - which is why the "
               "executor must pass bare ids (it passes matchedIds / missedIds, not matched / missed)",
               wrong);
        const auto right = p ({ "attack_db" });
        check (right == "the attack", "...and the bare id it should have been given reads correctly", right);
    }

    // ---- EMPTY IS EMPTY. A caller appends this to a sentence; a stray "the" would be worse than nothing.
    check (p ({}).isEmpty() && p ({ "", "  " }).isEmpty(), "nothing in, nothing out");

    // ---- THE HOUSE STYLE: no Oxford comma, duplicates collapsed --------------------------------
    check (echojay::params::joinWords ({ "a", "b", "c" }) == "a, b and c", "a, b and c");
    check (echojay::params::joinWords ({ "a", "a", "b" }) == "a and b", "a duplicate is said once");

    // ---- plainifyLine: A WHOLE LINE, BOTH DIRECTIONS (16 Oct 2026) -----------------------------
    //
    // The joint live pass drew "set slot 1: attack_ms 20, release_ms 60 - on 88_7even Kick_2_02"
    // from the server's own plan summary. Invariant I4 caught it. Two directions matter equally
    // here, and a one-sided test would ship the worse of the two faults:
    //   - an id MUST become a word, or the row reads as a wire format;
    //   - a NAME must survive untouched, because a track can be called "88_7even Kick_2_02" and
    //     mangling someone's track name is worse than printing an id.
    {
        const auto L = [] (const char* t) { return echojay::params::plainifyLine (t); };
        check (L ("set slot 1: attack_ms 20, release_ms 60") == "set slot 1: attack 20, release 60",
               "plainifyLine turns the ids into words", L ("set slot 1: attack_ms 20, release_ms 60"));
        check (L ("threshold_db -18, ratio 4") == "threshold -18, ratio 4",
               "...including the ones it knows by name", L ("threshold_db -18, ratio 4"));
        check (L ("band1.freq_hz 240") == "band 1 frequency 240",
               "...and a dotted id reads as words too", L ("band1.freq_hz 240"));
        // AND THE NAMES, UNTOUCHED - the half that keeps this safe to run on every line.
        check (L ("on 88_7even Kick_2_02") == "on 88_7even Kick_2_02",
               "a track name with underscores and digits is left exactly as it was",
               L ("on 88_7even Kick_2_02"));
        check (L ("Added Pro-C 3 to 88_7even Vox_Lead_01.") == "Added Pro-C 3 to 88_7even Vox_Lead_01.",
               "...and so is a plugin name and the sentence's full stop",
               L ("Added Pro-C 3 to 88_7even Vox_Lead_01."));
        check (L ("Renaissance Axx (s)") == "Renaissance Axx (s)", "...and a variant suffix in brackets",
               L ("Renaissance Axx (s)"));
        check (L ("") .isEmpty(), "nothing in, nothing out");
    }

    // ---- (pin) THE EXECUTOR PASSES THE ID LISTS, not the prose ones ----------------------------
    // A source pin, because the defect was in the CALL and not in the callee, and nothing else here can
    // see a call. Same idiom as checklist_reason_guard's paint pin.
    {
        const auto src = juce::File ("/Users/SeanD/echojay-vst/Source/EJAgentExecutorDo.cpp")
                             .loadFileAsString();
        check (src.contains ("plainParamList (all.missedIds)")
               && src.contains ("plainParamList (all.matchedIds)")
               && ! src.contains ("plainParamList (all.missed)"),
               "(pin) confirmSettings renders the ID lists (missedIds / matchedIds), never the prose lists");
        check (src.contains ("\"Added it. Settings not confirmed")
               && ! src.contains ("\"Not applied: add"),
               "(pin) an add that landed leads with \"Added it\" - never \"Not applied: add X\" (Sean's #5)");
    }

    // ---- SH: ONE PHRASE PER CONTROL, NOT PER CHANNEL (Sean's 19:5x item 2, 20 Oct 2026) -------
    //
    // *"The add step's read-back dumps every parameter raw: "optical threshold 1 1, optical gain 1
    // 2, discrete threshold 1 1, discrete ratio 1 2 ..." for the Shadow Hills. Use the plain
    // settings summary: the few controls that matter, in plain words, once per control and not per
    // channel."*
    //
    // Every phrase in that dump was already plain and already true - plainValuePhrase made them all.
    // What made it unreadable is that the Shadow Hills exposes its controls PER CHANNEL and the
    // server dialled every one. His own string is the fixture.
    {
        std::printf ("== SH: the Shadow Hills read-back, collapsed ==\n");
        using echojay::params::plainReadBackList;
        // ---- THE FIXTURE'S VALUES MUST NOT LOOK LIKE CHANNEL NUMBERS (fixed 21 Oct 2026) -----
        //
        // My first cut used values of 1 and 2 - the same digits as the channel indices - so the
        // collapsed phrase "optical threshold 1" (name + VALUE 1) was indistinguishable from a
        // phrase that had kept its channel number. SH1 went red on a correct collapse, and no
        // reading of the output could have told the two apart: the leg could not fail honestly and
        // could not pass honestly either.
        //
        // The values are now distinct from any channel index, so "the channel number is gone" is a
        // statement the output can actually settle. His own figures (10, from the 21:03 log) are the
        // model for it.
        const juce::StringArray sh
        {
            "optical threshold 1 10", "optical threshold 2 10",
            "optical gain 1 24",      "optical gain 2 24",
            "discrete threshold 1 31","discrete threshold 2 31",
            "discrete ratio 1 47",    "discrete ratio 2 47"
        };
        const auto got = plainReadBackList (sh);
        // ---- RE-AIMED BY HIS 21:03 RULING -------------------------------------------------
        // *"When channels collapse, drop the channel number from the phrase: "discrete threshold
        // 10". If the channels differ, say so ("discrete threshold 10 left, 8 right") rather than
        // hiding one."*  The first cut kept the first channel's phrase whole, so the number rode
        // along and B's closing read "Discrete Threshold 1 10" - true, and describing one channel as
        // though it were the control.
        check (got.contains ("optical threshold 10")
                 && ! got.contains ("optical threshold 1 10")
                 && ! got.contains ("optical threshold 2 10"),
               "SH1 the CHANNEL NUMBER is gone once the channels collapse, and the VALUE stays - "
               "\"optical threshold 10\", never \"optical threshold 1 10\"",
               got);
        check (got.contains ("optical gain 24") && got.contains ("discrete threshold 31")
                 && got.contains ("discrete ratio 47"),
               "SH2 ...for every distinct control, value intact", got);
        {
            juce::StringArray parts; parts.addTokens (got, ",", {});
            check (parts.size() == 4,
                   "SH3 eight phrases become FOUR - once per control (his words)",
                   juce::String (parts.size()) + ": " + got);
        }
        // ---- SH3b: HIS OWN CASE, verbatim from the 21:03 log ----------------------------------
        // EJParamApply: Discrete Threshold 1: APPLIED 0.391 ... landed "10"
        //               Discrete Threshold 2: APPLIED 0.391 ... landed "10"
        // Both channels written, both the same - so one phrase, no channel number.
        check (plainReadBackList ({ "discrete threshold 1 10", "discrete threshold 2 10" })
                 == "discrete threshold 10",
               "SH3b his exact case: two channels at the same value read back as \"discrete "
               "threshold 10\"",
               plainReadBackList ({ "discrete threshold 1 10", "discrete threshold 2 10" }));
        // ---- SH3c: AND WHEN THEY DIFFER, BOTH ARE SAID -----------------------------------------
        // The half of his ruling that matters most: hiding one would be the read-back lying about a
        // control that is not where it says.
        check (plainReadBackList ({ "discrete threshold 1 10", "discrete threshold 2 8" })
                 == "discrete threshold 10 left, 8 right",
               "SH3c RED-FIRST: channels that DIFFER are both reported, never one hidden",
               plainReadBackList ({ "discrete threshold 1 10", "discrete threshold 2 8" }));
        // ---- SH3d: A SINGLE CHANNEL KEEPS ITS NUMBER -------------------------------------------
        // One channel's value is not the control's value, and dropping the number would claim more
        // than was written - which is the very thing he asked the log to confirm.
        check (plainReadBackList ({ "discrete threshold 1 10" }) == "discrete threshold 1 10",
               "SH3d one channel only KEEPS its number - it would otherwise claim the whole control",
               plainReadBackList ({ "discrete threshold 1 10" }));
        // THE CAP COUNTS THE REMAINDER rather than truncating: "and 14 more" is information, a
        // sentence that stops mid-list is not.
        {
            juce::StringArray many;
            for (int i = 1; i <= 20; ++i) many.add ("control" + juce::String (i) + " " + juce::String (i));
            const auto capped = plainReadBackList (many, 6);
            check (capped.contains ("and 14 more"),
                   "SH4 past the cap the remainder is COUNTED, not truncated", capped);
            juce::StringArray parts; parts.addTokens (capped, ",", {});
            check (parts.size() == 7,   // six phrases + the "and N more" tail
                   "SH5 ...and only the few that matter are spelled out",
                   juce::String (parts.size()) + ": " + capped);
        }
        check (plainReadBackList ({}).isEmpty(),
               "SH6 nothing in, nothing out - never an empty list with punctuation");
        check (plainReadBackList ({ "threshold -11.8" }) == "threshold -11.8",
               "SH7 a single control is untouched",
               plainReadBackList ({ "threshold -11.8" }));
        // A PHRASE WITH NO NAME LEFT after stripping is kept as-is rather than collapsed into
        // another: a value with no control is odd, and dropping it would hide it.
        check (plainReadBackList ({ "1 2", "3 4" }).isNotEmpty(),
               "SH8 a phrase that is only numbers is kept, not silently swallowed",
               plainReadBackList ({ "1 2", "3 4" }));
    }

    // ---- LR: A SIDE CAN BE A WORD (his 11:27 item 1, 21 Oct 2026) ----------------------------
    //
    // *"The read-back collapse doesn't catch L/R-named channels ("left reduction 2.7", "l threshold
    // 4.5"). Treat L/R/left/right like trailing channel numbers, and say if the sides differ."*
    //
    // His two examples put the side FIRST, which is why the existing collapse never saw it: it only
    // popped NUMBERS off the tail. A side is now taken from either end of the name, and then every
    // rule the numbered channels already have applies to it unchanged.
    std::printf ("== LR: L/R-named sides collapse, and differing sides are both said ==\n");
    {
        using echojay::params::plainReadBackList;

        check (plainReadBackList ({ "left reduction 2.7", "right reduction 2.7" }) == "reduction 2.7",
               "LR1 RED-FIRST: his own phrases, agreeing, collapse to ONE - two sides of one control "
               "read as one fact (before this they were two phrases, repeated verbatim)",
               plainReadBackList ({ "left reduction 2.7", "right reduction 2.7" }));

        check (plainReadBackList ({ "l threshold 4.5", "r threshold 4.5" }) == "threshold 4.5",
               "LR2 ...and the one-letter form too, which is the other half of his example",
               plainReadBackList ({ "l threshold 4.5", "r threshold 4.5" }));

        check (plainReadBackList ({ "left reduction 2.7", "right reduction 3.4" })
                   == "reduction 2.7 left, 3.4 right",
               "LR3 THEY DIFFER: both are said, each with its own side - the half of his 21:03 ruling "
               "that matters most, now reached through words as well as numbers",
               plainReadBackList ({ "left reduction 2.7", "right reduction 3.4" }));

        check (plainReadBackList ({ "right reduction 3.4", "left reduction 2.7" })
                   == "reduction 3.4 right, 2.7 left",
               "LR4 ...and when they arrive RIGHT FIRST the sides are still their own - the old code "
               "labelled by arrival order, which would have swapped the two channels in the sentence "
               "and stated it confidently",
               plainReadBackList ({ "right reduction 3.4", "left reduction 2.7" }));

        check (plainReadBackList ({ "threshold l 4.5", "threshold r 4.5" }) == "threshold 4.5",
               "LR5 a TRAILING side word reads the same as a leading one - plugins write both",
               plainReadBackList ({ "threshold l 4.5", "threshold r 4.5" }));

        // ONE side only: the label STAYS, for the reason his 21:03 ruling gave for channel numbers -
        // one channel's value is not the control's value, and dropping the label would claim both
        // were written.
        check (plainReadBackList ({ "left reduction 2.7" }) == "reduction 2.7 on the left",
               "LR6 one side alone keeps its side - it does not get to speak for the other channel",
               plainReadBackList ({ "left reduction 2.7" }));

        // ---- AND THE OTHER DIRECTION: WORDS THAT ONLY LOOK LIKE SIDES ---------------------
        // The whole risk of this change is eating a real word. A side is a WHOLE token, and it is
        // never taken when nothing would be left of the name.
        check (plainReadBackList ({ "low cut 80" }) == "low cut 80",
               "LR7 \"low\" is not \"l\"", plainReadBackList ({ "low cut 80" }));
        check (plainReadBackList ({ "release 120" }) == "release 120",
               "LR8 \"release\" is not \"r\"", plainReadBackList ({ "release 120" }));
        check (plainReadBackList ({ "left 2.7" }) == "left 2.7",
               "LR9 a control whose whole name IS the side keeps it - taking it would leave a value "
               "with nothing to belong to", plainReadBackList ({ "left 2.7" }));
        check (plainReadBackList ({ "discrete threshold 1 10", "discrete threshold 2 10" })
                   == "discrete threshold 10",
               "LR10 and the NUMBERED collapse he ruled on at 21:03 still reads as it did",
               plainReadBackList ({ "discrete threshold 1 10", "discrete threshold 2 10" }));
    }

    // ---- SLP: THE ROW SENTENCE NAMES THE POSITION (his 11:27 item 2) -------------------------
    std::printf ("== SLP: the landed row says which slot ==\n");
    {
        using echojay::params::landedRowSentence;
        check (landedRowSentence ("add", "bx opto Pedal", "threshold -12", {}, 4)
                   == "Added bx opto Pedal in slot 4. Read back: threshold -12.",
               "SLP1 the position sits with the name it was read with",
               landedRowSentence ("add", "bx opto Pedal", "threshold -12", {}, 4));
        check (landedRowSentence ("add", "bx opto Pedal", "threshold -12", {}, 0)
                   == "Added bx opto Pedal. Read back: threshold -12.",
               "SLP2 and zero - the rack did not say - claims nothing about the order",
               landedRowSentence ("add", "bx opto Pedal", "threshold -12", {}, 0));
    }

    std::printf ("\n==== plain_names_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
