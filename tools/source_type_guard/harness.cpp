// source_type_guard — what a channel IS, and where that answer came from (Sean's round (b), 20 Oct 2026)
//
// *"A source type for each Link: the same type setting the V2 has, guessed automatically from the
// track name ... with Sean able to change it on the Link. Sent as channelRole on both roads."*
// Approved with the rules this guard exists to hold: **user > classifier > name, absent when unsure,
// whole words only.**
//
// THE LEG THAT MATTERS MOST IS THE ONE THAT DECLINES. A wrong type is worse than none - B picking a
// vocal chain for a drum bus is a worse failure than B asking - so the guesser's job is to be right
// or silent, and the assertions below are weighted that way: his own session's track names, with
// "Nafe Main_01" asserted to DECLINE because "Main" could be a lead vocal or a bus.
//
// Driven against the SHIPPED function (echojay::sourcetype::guessFromName), never a copy of it.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJSourceType.h"
#include "EJChainRole.h"   // round (b): the ONE channelRole composer both roads use
#include <cstdio>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

using namespace echojay::sourcetype;

/** A guess that landed, named by its wire spelling - which is what a failure line should print,
    because the wire spelling is what B reads. */
juce::String said (const Guess& g)
{
    return g.have ? wireNameFor (g.type) + " (" + g.why + ")"
                  : juce::String ("DECLINED: ") + g.why;
}

void expect (const char* name, ChannelType want, const char* label)
{
    const auto g = guessFromName (name);
    check (g.have && g.type == want, juce::String (label) + " - \"" + name + "\" -> "
                                       + wireNameFor (want), said (g));
    if (g.have)
        check (g.from == From::Name, juce::String (label) + " ...and its provenance is the NAME, "
                                     "never silently promoted", wireNameForFrom (g.from));
}

void expectDecline (const char* name, const char* label)
{
    const auto g = guessFromName (name);
    check (! g.have, juce::String (label) + " - \"" + name + "\" DECLINES", said (g));
    check (g.why.isNotEmpty(), juce::String (label) + " ...with a reason, because a declined guess "
                               "has to be explainable", g.why);
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("source_type_guard: the guess, its provenance, and what it refuses to guess\n");

    // ---- ST1: HIS OWN SESSION'S TRACK NAMES ---------------------------------------------------
    // The names in his 20 Oct log, verbatim. Two resolve, two must not - and the two that must not
    // are the ones that sent B looking for a delay on a vocal.
    std::printf ("== ST1: his own track names ==\n");
    expect ("Nafe Verse Adlibs_1_01", ChannelType::Adlibs,  "ST1a");
    expect ("88_7even Kick_2_01",     ChannelType::Kick,    "ST1b");
    expectDecline ("Nafe Main_01",            "ST1c (the one from his 16:0x round)");
    expectDecline ("Nafe 2 Auto Main_1_01",   "ST1d");
    expectDecline ("Nafe_1_01",               "ST1e (a name that is only a name)");

    // ---- ST2: WHOLE WORDS ONLY ----------------------------------------------------------------
    // The rule that keeps a guesser trustworthy. A substring match here is unrecoverable: it is
    // wrong in a way the user cannot see and the server cannot question.
    std::printf ("== ST2: whole words only - a bassoon is not a bass ==\n");
    expectDecline ("Bassoon",   "ST2a");
    expectDecline ("Vocoder",   "ST2b");
    expectDecline ("Subharmonic", "ST2c");
    expect ("Bass", ChannelType::Bass808, "ST2d the whole word still matches");

    // ---- ST3: PHRASES BEAT THE WORDS INSIDE THEM ----------------------------------------------
    // Without the tier, "bass guitar" is a collision between "bass" and "guitar" and would decline -
    // which would make the guesser useless on exactly the names that are most specific.
    std::printf ("== ST3: the most specific match wins ==\n");
    expect ("Bass Guitar",      ChannelType::BassGuitar,     "ST3a");
    expect ("Sub Bass",         ChannelType::SubBass,        "ST3b");
    expect ("Synth Bass 2",     ChannelType::SynthBass,      "ST3c");
    expect ("Drum Bus",         ChannelType::DrumBus,        "ST3d");
    expect ("Lead Vox",         ChannelType::LeadVocal,      "ST3e");
    expect ("Backing Vocal 3",  ChannelType::BackingVocal,   "ST3f");
    expect ("Electric Gtr L",   ChannelType::ElectricGuitar, "ST3g");
    expect ("Stereo Out",       ChannelType::MasterBus,      "ST3h");

    // ---- ST4: TWO THINGS AT ONCE DECLINES -----------------------------------------------------
    std::printf ("== ST4: a name describing two sources is not guessed ==\n");
    expectDecline ("Kick Snare",      "ST4a two subtypes in one family");
    expectDecline ("Vocal Drums",     "ST4b two families");
    expectDecline ("Gtr",             "ST4c a family with no generic member (acoustic or electric?)");
    expectDecline ("Synth",           "ST4d likewise (lead, pad, pluck or bus?)");

    // ---- ST5: THE NOISE WORDS ARE NOT EVIDENCE, BUT THEY DO NOT VETO --------------------------
    // "main", "bus", "stem" and the rest appear in every session and name nothing on their own. They
    // must not become guesses - and they must not suppress a real word standing beside them.
    std::printf ("== ST5: structure words name nothing, and block nothing ==\n");
    expectDecline ("Stems",                "ST5a");
    expectDecline ("Group 4",              "ST5b");
    expect ("Vocal Main",     ChannelType::LeadVocal, "ST5c a noise word does not veto a real one");
    expect ("Kick Print",     ChannelType::Kick,      "ST5d");

    // ---- ST6: THE PRECEDENCE, AS ONE FUNCTION -------------------------------------------------
    // user > classifier > name, and the same provenance may refresh itself (B re-answering, or a
    // rename re-guessing). A guess must never overwrite a decision: that is the whole point of
    // carrying the provenance at all.
    std::printf ("== ST6: user > classifier > name ==\n");
    check (mayReplace (From::None, From::Name),          "ST6a a guess fills an empty field");
    check (mayReplace (From::Name, From::Classifier),    "ST6b the classifier beats a name guess");
    check (mayReplace (From::Classifier, From::User),    "ST6c the user beats the classifier");
    check (mayReplace (From::Name, From::User),          "ST6d the user beats a name guess");
    check (! mayReplace (From::User, From::Name),        "ST6e A RENAME DOES NOT OVERWRITE WHAT HE SET");
    check (! mayReplace (From::User, From::Classifier),  "ST6f nor does the classifier");
    check (! mayReplace (From::Classifier, From::Name),  "ST6g nor a name guess over the classifier");
    check (mayReplace (From::User, From::User),          "ST6h ...but he can change his own answer");
    check (mayReplace (From::Name, From::Name),          "ST6i and a rename re-guesses");

    // ---- ST7: THE WIRE SPELLING ROUND-TRIPS ---------------------------------------------------
    // Derived from each type's display name rather than kept in a second table, so this is the leg
    // that catches a rename breaking the wire. Every type must survive name -> wire -> name.
    std::printf ("== ST7: every type's wire spelling round-trips ==\n");
    {
        int bad = 0; juce::String firstBad;
        for (int i = 0; i < channelTypeNames.size(); ++i)
        {
            const auto t = (ChannelType) i;
            const auto wire = wireNameFor (t);
            ChannelType back = ChannelType::Other;
            if (wire.isEmpty() || ! channelTypeForWireName (wire, back) || back != t)
            {
                ++bad;
                if (firstBad.isEmpty())
                    firstBad = channelTypeNames[i] + " -> \"" + wire + "\"";
            }
        }
        check (bad == 0, "ST7a all " + juce::String (channelTypeNames.size())
                           + " types round-trip name -> wire -> name",
               bad == 0 ? juce::String ("all") : juce::String (bad) + " broken, first: " + firstBad);
        check (wireNameFor (ChannelType::Bass808) == "bass_808",
               "ST7b \"Bass / 808\" spells bass_808 - the slash is dropped, not left in the wire",
               wireNameFor (ChannelType::Bass808));
        check (wireNameFor (ChannelType::HiHat) == "hi_hat",
               "ST7c \"Hi-Hat\" spells hi_hat", wireNameFor (ChannelType::HiHat));
        ChannelType junk = ChannelType::Other;
        check (! channelTypeForWireName ("tuba_solo", junk),
               "ST7d a wire name that is not ours is NOT STATED, never a default");
        check (! channelTypeForWireName ("", junk),
               "ST7e ...and neither is an empty one");
    }

    // ---- ST8: THE FAMILY TEST IS ONLY FOR CONFLICTS -------------------------------------------
    std::printf ("== ST8: families ==\n");
    check (familyOf (ChannelType::LeadVocal) == Family::Vocal
             && familyOf (ChannelType::Adlibs) == Family::Vocal,
           "ST8a the vocal types share a family");
    check (familyOf (ChannelType::Kick) != familyOf (ChannelType::Bass808),
           "ST8b a kick and an 808 are different families, which is why \"Kick 808\" declines");
    expectDecline ("Kick 808", "ST8c");

    // ---- ST9: THE WIRE SHAPE, FROM THE ONE COMPOSER -------------------------------------------
    // Both roads send channelRole through ChainRole::toVar() - agent off always did, and the agent
    // road now does too (round (b)); before this round it built its own object beside it, which is
    // how it came to send no source type at all. So the drift risk is structural now, and what is
    // left to assert is the thing a reader depends on: every new key appears when it is stated and
    // is ABSENT when it is not, because absent is what tells the server to ask.
    std::printf ("== ST9: channelRole carries the source, and omits what is not stated ==\n");
    {
        echojay::ChainRole bare;
        bare.trackName = "Nafe Main_01";
        const auto bv = bare.toVar();
        check (bv.getProperty ("kind", juce::var()).toString() == "channel"
                 && bv.getProperty ("name", juce::var()).toString() == "Nafe Main_01",
               "ST9a a role with nothing stated is still kind + name, exactly as every build before "
               "this one sent it");
        check (! bv.hasProperty ("sourceType") && ! bv.hasProperty ("sourceTypeFrom")
                 && ! bv.hasProperty ("placement") && ! bv.hasProperty ("sourceTypeName"),
               "ST9b ...and not one of the new keys is invented - absent is what tells the server to "
               "ask instead of assuming");

        echojay::ChainRole full;
        full.trackName = "Nafe Verse Adlibs_1_01";
        full.placement = "insert";
        full.sourceType = wireNameFor (ChannelType::Adlibs);
        full.sourceTypeFrom = wireNameForFrom (From::Name);
        const auto fv = full.toVar();
        check (fv.getProperty ("sourceType", juce::var()).toString() == "adlibs",
               "ST9c a stated type travels, in the wire spelling B reads",
               fv.getProperty ("sourceType", juce::var()).toString());
        check (fv.getProperty ("sourceTypeFrom", juce::var()).toString() == "name",
               "ST9d ...WITH its provenance, so a guess can never be read as a decision",
               fv.getProperty ("sourceTypeFrom", juce::var()).toString());
        check (fv.getProperty ("placement", juce::var()).toString() == "insert",
               "ST9e ...and where it sits, which is a different fact from what it is");
        check (! fv.hasProperty ("sourceTypeName"),
               "ST9f the custom name stays absent unless the type is \"other\"");

        echojay::ChainRole other;
        other.sourceType = wireNameFor (ChannelType::Other);
        other.sourceTypeFrom = wireNameForFrom (From::User);
        other.sourceTypeName = "Tuvan throat choir";
        const auto ov = other.toVar();
        check (ov.getProperty ("sourceType", juce::var()).toString() == "other"
                 && ov.getProperty ("sourceTypeName", juce::var()).toString() == "Tuvan throat choir"
                 && ov.getProperty ("sourceTypeFrom", juce::var()).toString() == "user",
               "ST9g \"other\" carries his own words, as the V2's selector already allows");
    }

    std::printf ("\n==== source_type_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
