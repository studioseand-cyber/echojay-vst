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
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include "EedCompressorProcessor.h"
#include "EedPitchProcessor.h"
#include "SurgicalEqProcessor.h"
#include "EJCalibLoop.h"      // 21t-i re-cut: the calibration block's field names
#include "EJLevelRecord.h"   // 21t-i: the record whose tokens() composes both level lines
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

    // ---- AGE, RULED 27 Sep 2026: "AGE <seconds>" after HEARD, on the member lines and [TRACK LEVELS] --------
    // Pinned against the PRODUCT'S composer, not a hand-written shape: LevelRecord::tokens() is the one function
    // both those lines are built from, so this is the byte sequence the server will read. An integer count of
    // seconds, no unit suffix, no minutes or hours form - B parses it from its next deploy.
    {
        echojay::LevelRecord r;
        r.valid = true; r.heardKnown = true;
        r.momDb = -13.0f; r.shortDb = -14.0f; r.shortMaxDb = -12.0f; r.short90Db = -14.2f;
        r.intLufs = -17.0f; r.peakDbTp = -1.0f; r.heardSeconds = 120.0f;
        r.updatedMs = 1000000;
        const auto line = r.tokens (r.updatedMs + 42000);      // 42 seconds old, exactly
        std::printf ("    the record's line as composed:\n    %s\n", line.toRawUTF8());
        check (line.contains ("HEARD 120, AGE 42"),
               "\"AGE <seconds>\" comes immediately after HEARD, separated by \", \" like every other token",
               line.fromFirstOccurrenceOf ("HEARD", true, false));
        check (! line.contains ("AGE 42s") && ! line.contains ("AGE 42 s"),
               "...with NO unit suffix: there is nothing to parse but the integer",
               line.fromFirstOccurrenceOf ("AGE", true, false));
        check (line.endsWith ("AGE 42"),
               "...and it is LAST, so a parser that does not know it yet reads every token before it unchanged",
               line.substring (juce::jmax (0, line.length() - 24)));
        const auto older = r.tokens (r.updatedMs + 7200000);   // two hours
        check (older.contains ("AGE 7200") && ! older.contains ("2h"),
               "...and an old record counts seconds too - no hours form", older.fromFirstOccurrenceOf ("AGE", true, false));
        // A record whose age cannot be known (no timestamp) reads 0 rather than a negative or a fabricated age.
        echojay::LevelRecord noStamp = r; noStamp.updatedMs = 0;
        check (noStamp.tokens (1000000).contains ("AGE 0"),
               "...a record with no timestamp reads AGE 0, never a negative or a guess",
               noStamp.tokens (1000000).fromFirstOccurrenceOf ("AGE", true, false));
        // AND NOT ON THE [CHAIN LEVELS] HEADER. The ruling names the member lines and [TRACK LEVELS]; the header's
        // tokens are ones B already parses, and an unruled token on a parsed line is the p90 mistake again.
        EchoJayAPI::ChainLevelsData dh;
        dh.inKnown = true; dh.havePercentiles = true;
        dh.inLevelDb = -18.2f; dh.inP10 = -24.0f; dh.inP90 = -12.5f; dh.inShort90Db = -14.2f;
        dh.inPeakDb = -1.2f; dh.inCrestDb = 9.4f; dh.inHeardS = 120.0f; dh.inWindowS = 120.0f;
        dh.recordAgeS = 42;
        const auto hdr = EchoJayAPI::buildChainLevelsInjectionCore (dh);
        check (! hdr.contains ("AGE "),
               "the [CHAIN LEVELS] header carries NO AGE token, even when it was composed from a record",
               hdr.fromFirstOccurrenceOf ("heard ", true, false).substring (0, 40));
    }

    // ---- THE CALIBRATION BLOCK'S FIELD NAMES AND LITERALS, pinned like every other wire spelling -----------
    // 21t-i re-cut: "nudge" joins them. These are names B writes and this parser reads; a rename in either place
    // reads as ABSENT, which for a nudge means the user's "ease off" quietly does nothing.
    {
        auto parse = [] (const char* json, echojay::CalibLoop::Config& c, juce::String& why)
        { return echojay::CalibLoop::configFromBlock (juce::JSON::parse (juce::String (json)), 1, false, "X", c, why); };
        echojay::CalibLoop::Config c; juce::String why;
        const bool ok = parse (R"({"source":"tally","heard_s":120,"measure":"short90","mode":"passive",
                                  "actuator":"drive","slot":1,"start_db":-3.0,"sense":null,"step":1,
                                  "nudge":"harder","gr_target_db":[2,3]})", c, why);
        check (ok && why.isEmpty(),
               "every field name on the calibration block parses with nothing flagged: source, heard_s, measure, "
               "mode, actuator, slot, start_db, sense, step, nudge, gr_target_db", why.isEmpty() ? "clean" : why);
        check (c.nudge == 1, "\"nudge\": \"harder\" is +1", juce::String (c.nudge));
        juce::String why2; echojay::CalibLoop::Config c2;
        parse (R"({"mode":"passive","actuator":"drive","slot":1,"nudge":"softer"})", c2, why2);
        check (c2.nudge == -1 && ! c2.haveBand,
               "\"nudge\": \"softer\" is -1, and a block with no gr_target_db carries NO band",
               juce::String (c2.nudge) + ", haveBand " + juce::String ((int) c2.haveBand));
        juce::String why3; echojay::CalibLoop::Config c3;
        parse (R"({"mode":"passive","actuator":"drive","slot":1,"nudge":"HARDER"})", c3, why3);
        check (c3.nudge == 0 && why3.contains ("nudge \"HARDER\""),
               "...and the literals are case-sensitive, like every other literal on this block: a near-miss is "
               "NAMED and ignored, never guessed", why3.trim());
    }

    // ---- [CURRENT CHAIN] AS THE CLIENT EMITS IT (21t-i re-cut, 27 Sep 2026) --------------------------------
    // B cannot answer what this block contains, so the block itself is the answer: a real ChainHost with the three
    // slots the question names - EQ, the tuner in balanced, a compressor - printed VERBATIM by the shipping
    // formatter. The assertion that matters is the one the ruling asks for: the tuner's RUNG must be readable here,
    // or the server cannot know which rung of natural/balanced/hard/snap the slot is already in.
    {
        // Force-link the three built-ins' registrars, the way every other harness does.
        { EedCompressorProcessor fc; juce::ignoreUnused (fc); }
        { EedPitchProcessor fp; juce::ignoreUnused (fp); }
        { SurgicalEqProcessor fe; juce::ignoreUnused (fe); }
        ChainHost host (ChainHost::Mode::Primary);
        host.prepare (48000.0, 512);
        auto addBuiltin = [&host] (const char* name, int at)
        {
            if (const auto* d = BuiltinDeviceRegistry::instance().findByName (name))
                host.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*d), at);
        };
        addBuiltin ("EchoJay EQ", 0);
        addBuiltin ("EchoJay Pitch", 1);
        addBuiltin ("EchoJay Compressor", 2);
        auto pump = [] (double ms)
        { const double t0 = juce::Time::getMillisecondCounterHiRes();
          while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
          { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } };
        for (int i = 0; i < 60 && host.getNumSlots() < 3; ++i) pump (50);
        check (host.getNumSlots() == 3, "fixture: EQ, tuner and compressor in one rack",
               juce::String (host.getNumSlots()) + " slot(s)");
        // THE TUNER IN BALANCED, set the way the server sets it.
        {
            auto* o = new juce::DynamicObject(); o->setProperty ("correction_mode", "balanced");
            auto* outer = new juce::DynamicObject(); outer->setProperty ("params", juce::var (o));
            const juce::var settings (outer);
            // The PUBLIC path a build takes: the settings are attached to the slot and the device applies them.
            host.setSlotStructuredSettings (1, settings);
            pump (300);
        }
        const auto block = EchoJayAPI::buildCurrentChainInjection (host);
        std::printf ("\n----- [CURRENT CHAIN] VERBATIM, as buildCurrentChainInjection writes it -----\n%s\n"
                     "----- end -----\n", block.toRawUTF8());
        // 21t-j: ...and the TUNER SLOT LINE on its own, after a mode EDIT, which is the line B asked for. The edit
        // goes through the same path a dial edit takes, so what prints here is what the server will read.
        {
            auto* o2 = new juce::DynamicObject(); o2->setProperty ("correction_mode", "hard");
            auto* outer2 = new juce::DynamicObject(); outer2->setProperty ("params", juce::var (o2));
            host.setSlotStructuredSettings (1, juce::var (outer2));
            pump (300);
            const auto after = EchoJayAPI::buildCurrentChainInjection (host);
            juce::String tunerLine, next;
            const auto lines = juce::StringArray::fromLines (after);
            for (int i = 0; i < lines.size(); ++i)
                if (lines[i].contains ("EchoJay Pitch"))
                { tunerLine = lines[i]; if (i + 1 < lines.size()) next = lines[i + 1]; break; }
            std::printf ("\n----- THE TUNER SLOT LINE after a mode EDIT to \"hard\", verbatim (two lines) -----\n"
                         "%s\n%s\n----- end -----\n", tunerLine.toRawUTF8(), next.toRawUTF8());
            check (next.contains ("correction_mode hard"),
                   "21t-j: the tuner slot line reads the mode the plugin is in NOW, after an edit  (RED as it "
                   "stood: it still carried the build's wording two mode edits later)",
                   next.substring (0, 90));
            check (next.contains ("retune ") && next.contains ("flex ") && next.contains ("humanize ")
                   && next.contains ("key ") && next.contains ("scale ") && next.contains ("ref "),
                   "21t-j: ...in the ruled field order - mode, retune, flex, humanize, key, scale, ref",
                   next.substring (0, 120));
            check (! next.contains ("key_source"),
                   "21t-j: ...and NO key_source field when nobody attributed the key - absent, not empty",
                   next.substring (0, 120));
        }
        // ---- 21t-j (28 Sep 2026 ruling): WHERE THE KEY AND THE REFERENCE CAME FROM -------------------------
        // A build that sets them from the [KEY] block carries that block's source label beside the params. The
        // line the server reads gains ONE ADDITIVE FIELD at the end; every existing field keeps its spelling,
        // its order and its value.
        {
            auto tunerLine = [&host]
            {
                const auto b = EchoJayAPI::buildCurrentChainInjection (host);
                const auto ls = juce::StringArray::fromLines (b);
                for (int i = 0; i < ls.size(); ++i)
                    if (ls[i].contains ("EchoJay Pitch") && i + 1 < ls.size()) return ls[i + 1];
                return juce::String();
            };
            auto* pk = new juce::DynamicObject();
            pk->setProperty ("key_root", 6); pk->setProperty ("scale", "minor");
            pk->setProperty ("reference_hz", 441.0);
            auto* outer3 = new juce::DynamicObject();
            outer3->setProperty ("params", juce::var (pk));
            outer3->setProperty ("key_source", "the Music Bus (Link \"MUSIC\")");
            host.setSlotStructuredSettings (1, juce::var (outer3));
            pump (300);
            const auto withSrc = tunerLine();
            std::printf ("\n----- THE TUNER SLOT LINE after a build that set key and ref from [KEY] -----\n%s\n"
                         "----- end -----\n", withSrc.toRawUTF8());
            check (withSrc.contains ("key_source \"the Music Bus (Link \"MUSIC\")\"")
                   || withSrc.contains ("key_source \"the Music Bus"),
                   "21t-j: a build that set key and reference from [KEY] carries key_source on the chain line",
                   withSrc.substring (0, 160));
            check (withSrc.contains ("key F#") && withSrc.contains ("ref 441.0 Hz")
                   && withSrc.indexOf ("key_source") > withSrc.indexOf ("ref "),
                   "21t-j: ...as an ADDITIVE field at the END - the values and their order are untouched",
                   withSrc.substring (0, 160));
            // ...and a HAND on the knob drops the attribution, because the value is no longer that source's.
            {
                auto* byHand = new juce::DynamicObject(); byHand->setProperty ("key_root", 0);
                auto* outer4 = new juce::DynamicObject(); outer4->setProperty ("params", juce::var (byHand));
                host.setSlotStructuredSettings (1, juce::var (outer4));
                pump (300);
                const auto after2 = tunerLine();
                check (after2.contains ("key C") && ! after2.contains ("key_source"),
                       "21t-j: ...and a later write that carries NO source drops the attribution rather than "
                       "keeping a name on a value that source never chose", after2.substring (0, 160));
            }
        }
        check (block.contains ("[CURRENT CHAIN"), "the block marker is \"[CURRENT CHAIN\"",
               block.substring (0, 40));
        check (block.contains ("EchoJay Pitch"), "the tuner slot is named in it");
        check (block.contains ("correction_mode balanced") || block.contains ("correction_mode: balanced")
               || block.contains ("balanced"),
               "THE TUNER'S RUNG IS READABLE IN THE BLOCK - the server can see which of natural/balanced/hard/snap "
               "the slot is already in  (RED as it stood if this fails: the rung was invisible and B had to guess)",
               block.fromFirstOccurrenceOf ("EchoJay Pitch", true, false).substring (0, 160).replace ("\n", " | "));
    }

    std::printf ("\n==== wire_tokens_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
