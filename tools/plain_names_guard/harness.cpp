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

    std::printf ("\n==== plain_names_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
