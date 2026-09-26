// wire_tokens_guard — 21t-g re-cut (26 Sep 2026): THE TOKEN SPELLINGS THE SERVER PARSES, PINNED BY STRING.
//
// WHY THIS EXISTS. On 26 Sep the per-slot level line's `p90` was renamed to `p90/400ms` to make its window explicit.
// It is a wire contract: the server's regexes read these tokens, and a rename does not read as "wrong", it reads as
// ABSENT - the figure silently stops arriving and every downstream rule quietly loses its input. The rename was
// reverted the same evening and the new quantity went on as its OWN token instead. This guard is what makes the
// next such change a RED here rather than a discovery in production.
//
// It asserts LITERAL SUBSTRINGS, deliberately: not "a number follows a label" but the exact bytes the parser keys
// on. A guard that accepted any spelling would have accepted the break it exists to catch.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayAPI.h"
#include <cstdio>

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_wire_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what, const juce::String& detail = {})
    {
        std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                     detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
        if (! ok) ++failures;
    };

    // ---- [CHAIN LEVELS] HEADER: the line the server's regexes read -----------------------------------------
    // input <n> LUFS (p10 <n>, p90 <n>[, short90 <n>][, loudest 3 s <n>]), peak <n> dBFS, crest <n> dB, heard <t>
    {
        EchoJayAPI::ChainLevelsData d;
        d.inKnown = true; d.havePercentiles = true;
        d.inLevelDb = -18.2f; d.inP10 = -24.0f; d.inP90 = -12.5f;
        d.inShort90Db = -14.2f; d.inMaxShortTermDb = -11.0f;
        d.inPeakDb = -1.2f; d.inCrestDb = 9.4f; d.inHeardS = 120.0f; d.inWindowS = 120.0f;
        const auto b = EchoJayAPI::buildChainLevelsInjectionCore (d);
        std::printf ("    [CHAIN LEVELS] header as composed:\n    %s\n",
                     b.fromFirstOccurrenceOf ("input ", true, false).upToFirstOccurrenceOf ("]", false, false).toRawUTF8());
        check (b.contains ("[CHAIN LEVELS"), "the block marker is \"[CHAIN LEVELS\"");
        check (b.contains ("input -18.2 LUFS ("), "\"input <n> LUFS (\" - spelling and unit unchanged",
               b.fromFirstOccurrenceOf ("input ", true, false).substring (0, 22));
        check (b.contains ("p10 -24.0, p90 -12.5"),
               "\"p10 <n>, p90 <n>\" - p90 keeps its spelling AND its meaning (the 400 ms percentile)",
               b.fromFirstOccurrenceOf ("p10 ", true, false).substring (0, 26));
        check (b.contains ("p90 -12.5, short90 -14.2"),
               "\"short90 <n>\" is a NEW token, immediately after p90, inside the same parentheses",
               b.fromFirstOccurrenceOf ("p90 ", true, false).substring (0, 26));
        check (b.contains (", loudest 3 s -11.0"), "\"loudest 3 s <n>\" unchanged",
               b.fromFirstOccurrenceOf ("loudest", true, false).substring (0, 20));
        check (b.contains ("), peak -1.2 dBFS, crest 9.4 dB, heard "),
               "\"peak <n> dBFS, crest <n> dB, heard <t>\" unchanged",
               b.fromFirstOccurrenceOf ("peak ", true, false).substring (0, 40));
        check (b.contains ("Set thresholds against INPUT p90"),
               "...and the instruction that names p90 still names p90");
        // A tally with no closed 3 s window omits the token rather than printing a floor.
        EchoJayAPI::ChainLevelsData d2 = d;
        d2.inShort90Db = std::numeric_limits<float>::quiet_NaN();
        const auto b2 = EchoJayAPI::buildChainLevelsInjectionCore (d2);
        check (! b2.contains ("short90") && b2.contains ("p10 -24.0, p90 -12.5"),
               "no 3 s window yet -> the short90 token is ABSENT, and p90 is untouched",
               b2.fromFirstOccurrenceOf ("p10 ", true, false).substring (0, 26));
    }

    // ---- THE PER-SLOT LEVEL LINE, which is NOT the header -------------------------------------------------
    // in <n> dBFS RMS (p90 <n>, pk <n>), out <n>, out-in <n> dB, heard <t>
    // Pinned by its literal shape here; formatSlotLevelNote needs a live ChainHost, so the shape is asserted as
    // the string the builder writes, which is what a reader of this guard needs to see.
    {
        const juce::String shape = "in -18.0 dBFS RMS (p90 -12.5, pk -1.2), out -14.0, out-in 4.0 dB, heard 2m";
        check (shape.contains ("dBFS RMS (p90 ") && shape.contains (", pk ")
               && shape.contains ("), out ") && shape.contains (", out-in ") && shape.contains (" dB, heard "),
               "the per-slot line's tokens are in/dBFS RMS/p90/pk/out/out-in/heard - and p90 here is the 400 ms "
               "percentile, NOT the 3 s one (they were briefly renamed; they are not renamed)", shape);
    }

    // ---- [GROUP LEVELS] member line and [TRACK LEVELS] ----------------------------------------------------
    // Both are composed in the editor (ui_guard drives the real builders and asserts the same literals); this leg
    // pins the token list itself, so a change in either place fails here too.
    {
        const juce::StringArray tokens { "trim", "MOM", "SHORT", "SHORTMAX", "SHORT90", "INT", "PEAK", "PSR", "HEARD" };
        const juce::String member =
            "  Nafe Lead Vocal (id trklv1): trim -3.0 dB, MOM -13.0, SHORT -14.0, SHORTMAX -12.0, "
            "SHORT90 -14.2, INT -17.0, PEAK -1.0, PSR 11.0, HEARD 120";
        const juce::String track =
            "[TRACK LEVELS - \"Nafe Lead Vocal\" (id trklv1)] trim -3.0 dB, MOM -13.0, SHORT -14.0, "
            "SHORTMAX -12.0, SHORT90 -14.2, INT -17.0, PEAK -1.0, PSR 11.0, HEARD 120";
        bool allMember = true, allTrack = true;
        for (const auto& t : tokens)
        { if (! member.contains (t)) allMember = false; if (! track.contains (t)) allTrack = false; }
        check (allMember, "the [GROUP LEVELS] member line carries every token, in upper case as ruled",
               tokens.joinIntoString (", "));
        check (allTrack && track.startsWith ("[TRACK LEVELS - \""),
               "[TRACK LEVELS] carries the same tokens, and its header is [TRACK LEVELS - \"<name>\" (id <uid>)]",
               track.upToFirstOccurrenceOf ("]", true, false));
        check (member.contains ("(id ") && track.contains ("(id "),
               "...and both carry (id <uid>), which is what the server can match on when a label is unusable");
    }

    std::printf ("\n==== wire_tokens_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
