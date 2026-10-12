// stream_markers_guard - 12 Oct 2026, Sean's item 8: NO BLOCK TYPE MAY BE KNOWN TO ONE PARSER AND NOT THE OTHER.
//
// THE DEFECT. EchoJay has two parsers for the same wire format: the extractors, which run on the SETTLED reply
// (EchoJayAPI::extractChainBlock and friends), and EJStreamBlockParser, which runs on the deltas as they arrive
// and decides what the user SEES while the reply is still streaming. The extractors knew seven block types.
// The incremental parser knew five. The two it did not know - PROPOSAL and LEVEL_MATCH - streamed into the chat
// bubble as ordinary prose: the marker, the JSON payload, all of it, visible until the settled reply replaced
// the text. Sean saw it in host and asked whether raw internal text can be painted before it is parsed. It can,
// and this is the gap that let it.
//
// WHY A GREP GUARD AND NOT A BEHAVIOURAL ONE. The fault is not in either parser's logic - both are correct about
// the markers they hold. It is that the SETS disagreed, and a set disagreeing with another set is a fact about
// the source text, so that is what is asserted. A behavioural leg would need one case per block type and would
// itself have to be extended for every new type: the same forgetting, one level up.
//
// THE DIRECTION OF THE RULE MATTERS. Forgetting to add a type here leaks raw JSON to the user, so the default
// has to be "refuse unless listed". Every <<<ECHOJAY_*>>> marker defined anywhere under Source/ must appear in
// EJStreamBlockParser.h's marker array, with its matching <<<END_*>>>.
#include <JuceHeader.h>
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) std::printf ("  ok    %s%s\n", what.toRawUTF8(),
                         detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    else  { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                         detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
}

juce::File sourceDir()
{
    // The guard runs with the repo root as its working directory (tools/tests/run_guard.sh sets it), and the
    // path is asserted rather than assumed: a guard that silently reads nothing passes for the wrong reason.
    return juce::File::getCurrentWorkingDirectory().getChildFile ("Source");
}

juce::StringArray markersIn (const juce::String& text, const juce::String& pattern)
{
    juce::StringArray out;
    int i = 0;
    for (;;)
    {
        const int a = text.indexOf (i, pattern);
        if (a < 0) break;
        const int b = text.indexOf (a, ">>>");
        if (b < 0) break;
        const auto m = text.substring (a, b + 3);
        // A REAL MARKER, NOT PROSE ABOUT MARKERS. The first cut matched any text between the brackets, so it
        // picked the literal "<<<ECHOJAY_*>>>" out of a comment written ABOUT this rule and then demanded the
        // parser know a block type called "*". The name is [A-Z0-9_]+ and nothing else.
        const auto inner = m.substring (3, m.length() - 3);
        bool nameOk = inner.isNotEmpty();
        for (int k = 0; k < inner.length() && nameOk; ++k)
        {
            const auto c = inner[k];
            nameOk = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }
        if (nameOk) out.addIfNotAlreadyThere (m);
        i = b + 3;
    }
    return out;
}
}

int main()
{
    std::printf ("stream_markers_guard: the delta parser knows every block the extractors know\n");

    const auto dir = sourceDir();
    check (dir.isDirectory(), "fixture: Source/ is readable from the working directory", dir.getFullPathName());
    if (! dir.isDirectory())
    { std::printf ("==== stream_markers_guard: RED (no Source/ - this is the harness, not the product) ====\n"); return 2; }

    const auto parserFile = dir.getChildFile ("EJStreamBlockParser.h");
    check (parserFile.existsAsFile(), "fixture: EJStreamBlockParser.h is readable");
    if (! parserFile.existsAsFile())
    { std::printf ("==== stream_markers_guard: RED ====\n"); return 2; }
    const auto parserText = parserFile.loadFileAsString();

    // Every ECHOJAY_ opening marker defined anywhere under Source/.
    juce::StringArray all;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.h"))
        all.addArray (markersIn (f.loadFileAsString(), "<<<ECHOJAY_"));
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.cpp"))
        all.addArray (markersIn (f.loadFileAsString(), "<<<ECHOJAY_"));
    all.removeDuplicates (false);
    all.sort (true);

    check (all.size() >= 5, "the sweep found the block markers at all (a zero here would pass vacuously)",
           juce::String (all.size()) + ": " + all.joinIntoString (" "));

    juce::StringArray missing;
    for (const auto& m : all)
    {
        // The marker must appear inside the parser's own array, as a quoted string.
        if (! parserText.contains ("\"" + m + "\"")) missing.add (m);
    }
    check (missing.isEmpty(),
           "every <<<ECHOJAY_*>>> block defined in Source/ is in EJStreamBlockParser's marker set - one the "
           "parser does not know is PAINTED to the user as prose while the reply streams",
           missing.isEmpty() ? juce::String ("all ") + juce::String (all.size()) + " known"
                             : "NOT KNOWN TO THE DELTA PARSER: " + missing.joinIntoString (", "));

    // ...and each one's closing marker with it, or the parser would open a block it can never close and
    // withhold the whole rest of the reply.
    juce::StringArray noClose;
    for (const auto& m : all)
    {
        const auto end = m.replace ("<<<ECHOJAY_", "<<<END_");
        if (parserText.contains ("\"" + m + "\"") && ! parserText.contains ("\"" + end + "\"")) noClose.add (end);
    }
    check (noClose.isEmpty(),
           "...and its closing marker too - an opened block with no close withholds the rest of the reply",
           noClose.isEmpty() ? juce::String ("all paired") : "MISSING: " + noClose.joinIntoString (", "));

    // The declared size must match the array, or the extra entries are never scanned.
    check (parserText.contains ("kNumMarkers = " + juce::String (all.size())),
           "kNumMarkers equals the number of block types, so every entry is actually scanned",
           "expected " + juce::String (all.size()));

    std::printf (failures == 0 ? "==== stream_markers_guard: GREEN (0 assertion(s) failed) ====\n"
                               : "==== stream_markers_guard: RED (%d assertion(s) failed) ====\n", failures);
    return failures == 0 ? 0 : 1;
}
