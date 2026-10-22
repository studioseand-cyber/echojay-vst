#pragma once
// =============================================================================================
//  slot_levels - B's SECTION 92 ITEM 2 (19 Oct 2026), + GAIN REDUCTION AND LINKS (20 Oct 2026)
// =============================================================================================
//  Sean, relaying B: *"send slot_levels on agent-off turns (one entry per slot: slot, name,
//  inputLoudestRmsDb, inputTypicalRmsDb, outputLoudestRmsDb, outputTypicalRmsDb, inputCrestDb,
//  outputCrestDb, inputLoudestPeakDbtp, windowSeconds, readBack) so agent off can answer 'how much
//  gain reduction'."*
//
//  ---- AND IT COULD NOT ANSWER IT (Sean's 16:04, 20 Oct 2026) --------------------------------
//
//  *"EchoJay 4-Band Compressor in slot 1, track playing, asked 'how much gain reduction is it
//  doing?' with the agent off, then with the agent on. Did the 4-Band's GR figure appear in what
//  was sent?"*  It did not, and his 16:03 request body says why twice over:
//
//    1. THIS BLOCK HAD NO GAIN-REDUCTION FIELD OF ANY KIND. Levels, crests, a true peak and
//       readBack - and no GR. So the question could not be answered from it even on the V2's OWN
//       rack, never mind a Link. The block was named for the question and did not carry its answer.
//    2. IT DESCRIBED THE WRONG RACK. All three call sites pass the V2's own ChainHost, so on a turn
//       aimed at a LINK the V2 hosts nothing, every row is dropped and the key is omitted entirely -
//       which is exactly what his body shows (no slot_levels at all, and the chain feed's slot 1
//       line reduced to `1: "EchoJay 4-Band Compressor" (EchoJayBuiltin)`).
//
//  The Link's side was already done: the per-slot GR fields have been on the rack sidecar since
//  18 Oct ("ITEM 13: THE LINK'S OWN MEASUREMENTS") and the publish digest moves on them, so a Link
//  republishes while it measures. Nothing needed adding to the wire. What needed fixing was HERE.
//
//  ---- ONE COMPOSER, TWO SOURCES ---------------------------------------------------------------
//
//  The rule this file was written for stands: *"two composers of one measurement is how the chat
//  and the card come to disagree about the same slot."*  So the row shape has ONE builder and the
//  two sources are ADAPTERS into a plain struct:
//
//      rowsFromChainHost (host, sampleRate)  -> std::vector<SlotLevelRow>   the open rack
//      rowsFromSidecar   (rack, sampleRate)  -> std::vector<SlotLevelRow>   a Link, from its sidecar
//      slotLevelsBlock   (rows)              -> juce::var                   THE one composer
//
//  A guard asserts a sidecar-built row equal field-for-field to a ChainHost-built row from the same
//  figures, which is the only assertion that can stop the two sources drifting.
//
//  EVERY FIGURE IS MEASURED OR ABSENT. A field whose percentile ring has not filled is OMITTED, not
//  sent as 0 - a zero here reads as "this slot is at 0 dBFS", which is the loudest claim the format
//  can make. Same rule the rest of the build keeps: absent means "cannot say".
//
//  THE UNITS, stated because B labels them: the RMS figures are PLAIN (unweighted) dBFS, from the
//  gated hop rings, p50 for typical and p90 for loudest - the pair Kathy's profiles are cut against.
//  The crests are peak minus RMS over the same window. inputLoudestPeakDbtp is true peak, dBTP.
//
//  AND THE TWO GR RULES, verbatim from the sidecar field they come from:
//    - grWindowFrames == 0 means NOTHING WAS MEASURED, never "no reduction" - so the whole GR group
//      is omitted rather than printed at its floor.
//    - grCertified is a property of the PLUGIN (it passed the EJ Maps sweep), not of whether audio
//      was heard. "NO estimates, ever" turns on it: an uncertified plugin's gain reduction is not
//      measured by this build, so no figure is sent and `grReadable: false` says so in one word.
#include <JuceHeader.h>
#include "ChainHost.h"
#include "LinkShm.h"

namespace echojay
{

/** One slot's measurements, source-agnostic. Every figure has a `have` flag beside it because
    ABSENT and ZERO are different statements and this struct is the only place both sources meet. */
struct SlotLevelRow
{
    int          slot1 = 0;              // 1-BASED, as every other wire number is
    juce::String name;
    juce::String readBack;               // what the plugin SHOWS; empty = not confirmed

    bool  haveInRms = false;   float inTypicalRmsDb = 0.0f,  inLoudestRmsDb = 0.0f;
    bool  haveOutRms = false;  float outTypicalRmsDb = 0.0f, outLoudestRmsDb = 0.0f;
    bool  haveInCrest = false;  float inCrestDb = 0.0f;
    bool  haveOutCrest = false; float outCrestDb = 0.0f;
    bool  havePeak = false;     float inLoudestPeakDbtp = 0.0f;
    float windowSeconds = 0.0f;          // 0 = absent

    /** THE GR GROUP. `grReadableStated` is whether we know enough to say anything about
        readability at all; `grReadable` is the PLUGIN's certification. `haveGr` is the window
        gate - grKnown AND a window with frames in it - and it governs every figure below it. */
    bool  grReadableStated = false, grReadable = false;
    bool  haveGr = false;
    float grNowDb = 0.0f, grHeldDb = 0.0f, grLoudestDb = 0.0f, grTypicalDb = 0.0f, grPeakDb = 0.0f;
    double grWindowFrames = 0.0;         // 0 = nothing measured
    double grWindowSeconds = 0.0;        // 0 = unknown sample rate, so omitted (frames still gate)
};

/** THE ONE COMPOSER. Empty array when nothing has been heard; the caller omits the field entirely
    rather than sending an empty one. */
inline juce::var slotLevelsBlock (const std::vector<SlotLevelRow>& rows)
{
    juce::Array<juce::var> out;
    for (const auto& r : rows)
    {
        if (r.slot1 <= 0) continue;
        // THE VAR OWNS IT from the first line: DynamicObject is reference-counted, so a row that
        // turns out to have no figures is dropped by letting the var go out of scope rather than by
        // deleting a raw pointer whose count is still zero.
        juce::var rowVar (new juce::DynamicObject());
        auto* o = rowVar.getDynamicObject();
        o->setProperty ("slot", r.slot1);
        o->setProperty ("name", r.name);
        int figures = 0;

        if (r.haveInRms)
        {
            o->setProperty ("inputTypicalRmsDb", (double) r.inTypicalRmsDb);
            o->setProperty ("inputLoudestRmsDb", (double) r.inLoudestRmsDb);
            ++figures;
        }
        if (r.haveOutRms)
        {
            o->setProperty ("outputTypicalRmsDb", (double) r.outTypicalRmsDb);
            o->setProperty ("outputLoudestRmsDb", (double) r.outLoudestRmsDb);
            ++figures;
        }
        if (r.haveInCrest)  { o->setProperty ("inputCrestDb",  (double) r.inCrestDb);  ++figures; }
        if (r.haveOutCrest) { o->setProperty ("outputCrestDb", (double) r.outCrestDb); ++figures; }
        if (r.havePeak)     { o->setProperty ("inputLoudestPeakDbtp", (double) r.inLoudestPeakDbtp); ++figures; }
        if (r.windowSeconds > 0.0f) { o->setProperty ("windowSeconds", (double) r.windowSeconds); ++figures; }

        // ---- GAIN REDUCTION, UNDER ITS TWO RULES ---------------------------------------------
        //
        // `grReadable` is sent whenever it is KNOWN, figure or no figure, because "this plugin's
        // gain reduction is not measured by this build" is an answer to his question and silence is
        // not. It is also the field that stops a reader inferring 0 dB from the absence of figures.
        // ...BUT IT DOES NOT EARN A ROW ON ITS OWN. Leg SL92f holds a ruling: *"a loaded slot that
        // has heard nothing gets NO row - not a row of zeros"*, because such a row "tells B nothing
        // and invites exactly the zero reading this block exists to avoid". A row carrying only
        // `grReadable` is that row in a new costume, so it is not counted as a figure. This costs
        // his 16:04 case nothing - the track was playing, so the slot has figures - and it keeps the
        // rule that a row appears when there is something MEASURED to put in it.
        if (r.grReadableStated) o->setProperty ("grReadable", r.grReadable);
        // ...AND THE FIGURES ONLY WITH A WINDOW. grWindowFrames == 0 is "nothing was measured",
        // never "no reduction", so the group goes out together or not at all. A floor printed as a
        // figure is the failure this convention exists to stop.
        if (r.haveGr && r.grWindowFrames > 0.0)
        {
            o->setProperty ("grLoudestDb", (double) r.grLoudestDb);   // the loud phrases (p95)
            o->setProperty ("grTypicalDb", (double) r.grTypicalDb);   // the median
            o->setProperty ("grHeldDb",    (double) r.grHeldDb);      // 2 s peak-hold
            o->setProperty ("grPeakDb",    (double) r.grPeakDb);      // deepest in the window
            o->setProperty ("grNowDb",     (double) r.grNowDb);       // the live needle
            o->setProperty ("grWindowFrames", r.grWindowFrames);
            if (r.grWindowSeconds > 0.0) o->setProperty ("grWindowSeconds", r.grWindowSeconds);
            ++figures;
        }

        // What the plugin SHOWS, never the request (Sean's #1, 16 Oct). Absent when nothing was read
        // back, which B must read as "not confirmed" and never as "nothing is set".
        if (r.readBack.isNotEmpty()) o->setProperty ("readBack", r.readBack);

        // NOT ONE FIGURE = NOTHING TO SAY. readBack alone does not earn a row either: it is what the
        // plugin SHOWS, not what the slot heard, and this block is about what it heard.
        if (figures == 0) continue;
        out.add (rowVar);
    }
    return out.isEmpty() ? juce::var() : juce::var (out);
}

/** THE OPEN RACK. `sampleRate` only converts the GR window to seconds; 0 omits that one field and
    changes nothing else, because the window GATE is in frames. */
inline std::vector<SlotLevelRow> rowsFromChainHost (const ChainHost& host, double sampleRate = 0.0)
{
    std::vector<SlotLevelRow> rows;
    for (int i = 0; i < host.getNumSlots(); ++i)
    {
        const auto lv = host.getSlotLevels (i);
        const auto pic = host.slotPicture (i);
        // `measured` IS NOT "HAS HEARD AUDIO" (leg SL92f, 19 Oct 2026). It says this slot has a tally
        // pair, which a slot has from the moment it loads - so gating on it alone composed a row for
        // a plugin that had heard nothing, carrying a slot number, a name and not one figure.
        //
        // 20 Oct 2026: ...but it must no longer gate the GR group with it. A certified device whose
        // tallies have not filled still has a readability answer worth sending, and `grReadable` is
        // the field his 16:04 question turns on. So the row is always BUILT and the composer's
        // figure count is what decides whether it is kept - which is the rule the file already had.
        const auto info = host.getSlotInfo (i);
        SlotLevelRow r;
        r.slot1 = i + 1;
        r.name  = info.name;
        r.readBack = info.landedValues;

        if (lv.measured)
        {
            float typ = 0.0f, loud = 0.0f;
            if (lv.in.hopLevelPercentiles (typ, loud))
            { r.haveInRms = true; r.inTypicalRmsDb = typ; r.inLoudestRmsDb = loud; }
            if (lv.out.hopLevelPercentiles (typ, loud))
            { r.haveOutRms = true; r.outTypicalRmsDb = typ; r.outLoudestRmsDb = loud; }
            float crest = 0.0f;
            if (lv.in.hopCrest  (crest)) { r.haveInCrest = true;  r.inCrestDb  = crest; }
            if (lv.out.hopCrest (crest)) { r.haveOutCrest = true; r.outCrestDb = crest; }

            // The loudest TRUE PEAK of the input hops. Taken from the hop ring rather than the
            // integrated max-hold, so it describes the same window as the figures beside it - a peak
            // from a different window is the "-17.5 under -14.1" complaint in another costume.
            float peak = -200.0f;
            for (int k = 0; k < lv.in.hopTruePeakCount && k < (int) lv.in.hopTruePeakDb.size(); ++k)
                if (lv.in.hopTruePeakDb[(size_t) k] > -199.0f)
                { peak = juce::jmax (peak, lv.in.hopTruePeakDb[(size_t) k]); r.havePeak = true; }
            if (r.havePeak) r.inLoudestPeakDbtp = peak;

            r.windowSeconds = lv.in.hopLevelSeconds();
        }

        // THE PICTURE IS THE AUTHORITY ON GR, here and on the Link - slotPicture is what computes it
        // (dynamic_cast to EedDeviceProcessor + reportsGainReduction), and the 4-Band reports its
        // deepest non-bypassed band since the 12:02 override.
        if (pic.valid || pic.grKnown || pic.grCertified)
        {
            r.grReadableStated = true;
            r.grReadable = pic.grCertified;
        }
        if (pic.grWindowKnown())
        {
            r.haveGr = true;
            r.grNowDb = pic.grDb; r.grHeldDb = pic.grHeldDb; r.grLoudestDb = pic.grLoudestDb;
            r.grTypicalDb = pic.grTypicalDb; r.grPeakDb = pic.grPeakDb;
            r.grWindowFrames = (double) pic.grWindowFrames;
            if (sampleRate > 0.0) r.grWindowSeconds = r.grWindowFrames / sampleRate;
        }
        rows.push_back (r);
    }
    return rows;
}

/** A LINK, from its own published sidecar. Every figure this needs has been on the sidecar since
    18 Oct; nothing is estimated here and nothing is recomputed - the Link measured it. */
inline std::vector<SlotLevelRow> rowsFromSidecar (const LinkShm::RackSidecar& rack, double sampleRate = 0.0)
{
    std::vector<SlotLevelRow> rows;
    if (! rack.valid) return rows;
    for (size_t i = 0; i < rack.slots.size(); ++i)
    {
        const auto& s = rack.slots[i];
        SlotLevelRow r;
        r.slot1 = (int) i + 1;
        r.name  = s.name;
        r.readBack = s.landedValues;

        if (s.inRmsKnown)
        { r.haveInRms = true; r.inTypicalRmsDb = s.inTypicalRmsDb; r.inLoudestRmsDb = s.inLoudestRmsDb; }
        if (s.outRmsKnown)
        { r.haveOutRms = true; r.outTypicalRmsDb = s.outTypicalRmsDb; r.outLoudestRmsDb = s.outLoudestRmsDb; }
        if (s.inCrestKnown)  { r.haveInCrest = true;  r.inCrestDb  = s.inCrestDb; }
        if (s.outCrestKnown) { r.haveOutCrest = true; r.outCrestDb = s.outCrestDb; }
        // The sidecar's inLoudestPeakDbtp sits at the -200 no-reading floor until there is a
        // reading, and a reader must not print a floor as a figure (the field's own note).
        if (s.inLoudestPeakDbtp > -199.0f)
        { r.havePeak = true; r.inLoudestPeakDbtp = s.inLoudestPeakDbtp; }
        if (s.inWindowSeconds > 0.0f) r.windowSeconds = s.inWindowSeconds;
        else if (s.inRmsSeconds > 0.0f) r.windowSeconds = s.inRmsSeconds;

        if (s.pictureValid || s.grKnown || s.grCertified)
        { r.grReadableStated = true; r.grReadable = s.grCertified; }
        if (s.grKnown && s.grWindowFrames > 0.0)
        {
            r.haveGr = true;
            r.grNowDb = s.grDb; r.grHeldDb = s.grHeldDb; r.grLoudestDb = s.grLoudestDb;
            r.grTypicalDb = s.grTypicalDb; r.grPeakDb = s.grPeakDb;
            r.grWindowFrames = s.grWindowFrames;
            if (sampleRate > 0.0) r.grWindowSeconds = r.grWindowFrames / sampleRate;
        }
        rows.push_back (r);
    }
    return rows;
}

/** The shape every existing caller and the existing legs use. Kept so the three call sites and
    level_slot_guard's SL92e-h legs are unchanged by the refactor. */
inline juce::var slotLevelsBlock (const ChainHost& host, double sampleRate = 0.0)
{
    return slotLevelsBlock (rowsFromChainHost (host, sampleRate));
}

/** ...and the Link twin, by target. */
inline juce::var slotLevelsBlock (const LinkShm::RackSidecar& rack, double sampleRate = 0.0)
{
    return slotLevelsBlock (rowsFromSidecar (rack, sampleRate));
}

}   // namespace echojay
