#pragma once
// =============================================================================================
//  EJSlotMeters.h - PER-SLOT METERS ON A LINK'S SLOT CARDS (25 Oct 2026)
// =============================================================================================
//  Sean, after control passed on 84FB4A62:
//
//      *"Slot meters, from your design note docs/SLOT_METERS_2026-10-25.md: per-slot IN, OUT and
//      gain reduction on the Link slot cards in the V2, fed from the Link at 30 Hz. They must show
//      silence as silence, never a stale level, and fall back to blank (not zero) if the Link stops
//      publishing."*
//
//  ---- THIS ADDS NO MEASUREMENT -----------------------------------------------------------------
//
//  The Link runs the same ChainHost the main plugin does, and its per-slot tallies are already
//  filled on the audio thread for every slot, third party included - that is where the sidecar's
//  inLoudestDb / inTypicalDb come from today. So the audio thread's work is IDENTICAL with this
//  feature on or off, which is the bar stage A set for the live view ("the cost of the feature when
//  it is not on screen is zero, not small, zero"). What is new is a faster road for numbers that
//  are already taken: the sidecar is revision-gated and republished whole, so a card's figures move
//  when the rack changes rather than at a meter's own rate.
//
//  WRITTEN BY THE LINK'S MESSAGE THREAD on the 30 Hz tick it already has; READ BY THE V2'S MESSAGE
//  THREAD on its 20 Hz editor tick. Seqlocked exactly as LinkShm::publishMeterFrame is, and for the
//  same reason: a reader must be able to tell a torn frame from a frame.
//
//  ---- WHICH FIGURES, AND WHY NOT THE GATED ONES -----------------------------------------------
//
//  A meter wants a RECENT level, so this carries the tally's decayed `rmsDb` and `peakDb` - not
//  `levelDb` (gated and integrated: it answers "how loud is this programme", which does not move at
//  30 Hz) and not `truePeakDb` (max-hold since reset, which by construction never falls and so is
//  the exact opposite of a meter).
//
//  SILENCE SHOWS AS SILENCE because `rmsDb` decays: after audio stops it walks down to the floor and
//  the card follows it down. NOTHING MEASURED shows as NOTHING, because `known` is false until the
//  tally has heard its floor of audio, and an absent field is absent here rather than zero.
//
//  ---- GAIN REDUCTION: THE LINE THIS DOES NOT CROSS --------------------------------------------
//
//  *"EchoJay does not read a third party's gain-reduction meter and does not estimate one"* (his
//  ruling of 15/18 Oct, which SlotPicture already enforces through grKnown: a GR figure exists only
//  for an EchoJay device, which reports its own, or a third party whose map is CERTIFIED).
//
//  So the third figure has TWO PROVENANCES and the mask says which:
//
//    * kSlotMeterGr   - a real reduction, from the device or a certified map. May be labelled GR,
//                       because it is one. Taken from grHeldDb, which is what the device's own meter
//                       draws, so the card and the meter cannot disagree.
//    * kSlotMeterDiff - the DIFFERENCE ACROSS THE SLOT (out - in). Never labelled GR. On a
//                       compressor doing nothing but making up gain it reads positive; on one with
//                       its output trimmed it reads large with no compression at all. It is a useful
//                       number and it is not that number.
//
//  ---- HOW IT FAILS SAFE (his three words: silence, never stale, blank not zero) ---------------
//
//   1. A frame nobody has published reads as NO READING. seq 0 is never a frame of zeros.
//   2. A SEQ THAT HAS STOPPED MOVING is no reading, not the last reading held for ever. The V2
//      watches the sequence by the clock and blanks the cards after kSlotMeterStaleMs - so a Link
//      that stops publishing (quit, crashed, an older build) blanks rather than freezing on a
//      number that was true a minute ago. THIS is the difference between a meter and a photograph.
//   3. heardMs == 0 means NOTHING WAS MEASURED, never "silence" - the GR window's own rule.
//   4. A rack that changes shape under a frame DISCARDS it (structureRevision), so a figure never
//      lands on a card it does not belong to.
//   5. An older Link publishes no file at all, and that is the correct downgrade: the cards show
//      what they show today and nothing claims the meters are live when they are not.
#include <JuceHeader.h>
#include "LinkShm.h"
#include "EJGainReadout.h"   // the TRIM readouts' own rects: the flanks must clear them
#include <cstring>
#include <cmath>

namespace echojay
{

// ---- THE LAYOUT, FROZEN THE WAY EVERY OTHER SHARED STRUCTURE HERE IS ------------------------

static constexpr juce::uint32 kSlotMeterMagic   = 0xEC4A5201u;
static constexpr juce::uint32 kSlotMeterVersion = 1u;
/** A Link's rack holds at most LinkProcessor::kMaxChainSlots (15). 16 is that, rounded up, so the
    frame is a flat 576 bytes and a rack can never overflow it. A Link with more publishes its first
    16 and says so in `slotCount`. */
static constexpr int kSlotMeterMax = 16;

/** The mask bits. EVERY FIGURE IS GATED SEPARATELY, because a slot can have an input reading and no
    output one (its instance failed to load), or a difference and no certified GR. An ungated dB
    field reading 0.0f would render as a reading - LinkMeterFrame's own words, and the trap this
    house has paid for more than once. */
static constexpr juce::uint32 kSlotMeterIn       = 1u << 0;
static constexpr juce::uint32 kSlotMeterOut      = 1u << 1;
static constexpr juce::uint32 kSlotMeterInPeak   = 1u << 2;
static constexpr juce::uint32 kSlotMeterOutPeak  = 1u << 3;
static constexpr juce::uint32 kSlotMeterGr       = 1u << 4;   // a REAL reduction: device or certified
static constexpr juce::uint32 kSlotMeterDiff     = 1u << 5;   // out - in; never labelled GR
static constexpr juce::uint32 kSlotMeterBypassed = 1u << 6;   // out of circuit: not measured, and why

struct alignas(64) SlotMeterHeader
{
    juce::uint32 magic;             //  0
    juce::uint32 version;           //  4
    juce::uint32 seq;               //  8  seqlock; ODD = a write is in progress
    juce::uint32 slotCount;         // 12  how many entries are meant (<= kSlotMeterMax)
    juce::uint32 structureRevision; // 16  the rack's shape when this was taken
    juce::uint32 publishCount;      // 20  ticks published since this file was created
    juce::uint32 sweepMicros;       // 24  what the last pass over the slots cost the Link
    juce::uint32 _pad[9];           // 28..63
};
static_assert (sizeof (SlotMeterHeader) == 64, "SlotMeterHeader must be 64 bytes");
static_assert (offsetof (SlotMeterHeader, seq)               ==  8, "seq moved");
static_assert (offsetof (SlotMeterHeader, slotCount)         == 12, "slotCount moved");
static_assert (offsetof (SlotMeterHeader, structureRevision) == 16, "structureRevision moved");

/** ONE SLOT'S READING. 32 bytes, which is the stride of the array it lives in - the same rule
    LinkMeterFrame states about itself. A new figure replaces a pad word; it never grows this. */
struct SlotMeterOne
{
    float        inDb      = -200.0f;   //  0  decayed RMS, dBFS
    float        outDb     = -200.0f;   //  4
    float        inPeakDb  = -200.0f;   //  8  recent sample peak, decayed hold
    float        outPeakDb = -200.0f;   // 12
    float        thirdDb   = 0.0f;      // 16  GR or the difference - the MASK says which
    juce::uint32 fields    = 0u;        // 20  kSlotMeter* bits
    juce::uint32 heardMs   = 0u;        // 24  0 = NOTHING WAS MEASURED, never "silence"
    juce::uint32 _pad      = 0u;        // 28
};
static_assert (sizeof (SlotMeterOne) == 32, "SlotMeterOne must be 32 bytes - the size IS the stride");

struct SlotMeterFrame
{
    SlotMeterHeader hdr;
    SlotMeterOne    slot[kSlotMeterMax];
};
static_assert (sizeof (SlotMeterFrame) == 64 + kSlotMeterMax * 32,
               "SlotMeterFrame grew - the file's size is its contract with every installed binary");

static constexpr size_t kSlotMeterFileSize = sizeof (SlotMeterFrame);

/** ONE name, from the uid, for both processes - not two derivations of one fact. */
inline juce::String slotMetersFileNameFor (const juce::String& uid)
{
    juce::String safe;
    for (auto c : uid)
        safe << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                                                               : juce::String ("_"));
    return "slotmeters_" + safe + ".bin";
}

// ---- PUBLISH AND READ, SEQLOCKED EXACTLY AS THE METER FRAME IS -------------------------------

inline void publishSlotMeters (void* map, const SlotMeterHeader& h,
                               const SlotMeterOne* slots, int count)
{
    if (map == nullptr) return;
    auto* dst = reinterpret_cast<SlotMeterFrame*> (map);
    const juce::uint32 s = LinkShm::loadRelaxed (&dst->hdr.seq) & ~1u;
    LinkShm::storeRelease (&dst->hdr.seq, s + 1);                  // odd: writing
    // The header after `seq`, then the slots. `seq` itself is never memcpy'd over: it is the lock,
    // not payload - exactly as publishMeterFrame leaves it alone.
    std::memcpy (reinterpret_cast<juce::uint8*> (dst) + 12,
                 reinterpret_cast<const juce::uint8*> (&h) + 12,
                 sizeof (SlotMeterHeader) - 12);
    dst->hdr.magic   = kSlotMeterMagic;
    dst->hdr.version = kSlotMeterVersion;
    const int n = juce::jlimit (0, kSlotMeterMax, count);
    if (n > 0 && slots != nullptr)
        std::memcpy (dst->slot, slots, (size_t) n * sizeof (SlotMeterOne));
    // AND THE SLOTS NOBODY PUBLISHED ARE CLEARED, not left holding the last rack's readings: a
    // rack that loses a slot must not leave a figure behind on a card that comes back later.
    for (int i = n; i < kSlotMeterMax; ++i) dst->slot[i] = SlotMeterOne {};
    LinkShm::storeRelease (&dst->hdr.seq, s + 2);                  // even: stable
}

/** FALSE on a torn read that would not settle, on a file that is not one of ours, or on a frame
    that has never been published - and a caller that gets false shows NOTHING, never a zero.
    `out.seq` is the staleness signal: an unchanged seq means no new frame, never "nothing moved". */
inline bool readSlotMeters (const void* map, SlotMeterHeader& out,
                            SlotMeterOne* slots, int maxSlots, int& countOut)
{
    countOut = 0;
    if (map == nullptr) return false;
    const auto* src = reinterpret_cast<const SlotMeterFrame*> (map);
    for (int tries = 0; tries < 4; ++tries)
    {
        const juce::uint32 s1 = LinkShm::loadAcquire (&src->hdr.seq);
        if ((s1 & 1u) != 0u || s1 == 0u) continue;                 // mid-write, or never published
        SlotMeterHeader h = src->hdr;
        const int n = juce::jlimit (0, juce::jmin (kSlotMeterMax, maxSlots), (int) h.slotCount);
        if (n > 0 && slots != nullptr)
            std::memcpy (slots, src->slot, (size_t) n * sizeof (SlotMeterOne));
        if (LinkShm::loadAcquire (&src->hdr.seq) != s1) continue;   // it moved under us: retry
        if (h.magic != kSlotMeterMagic || h.version != kSlotMeterVersion) return false;
        out = h; countOut = n;
        return true;
    }
    return false;
}

// ---- THE PUBLISHER'S OWN DECISION: WHICH FIGURES DOES THIS SLOT HAVE? -----------------------

/** Pure, so the guard holds it without a Link, a plugin or a rack. Every argument is a fact the
    Link already has from ChainHost::getSlotLevels and ChainHost::slotPicture.

    `bypassed` wins over everything: a slot out of circuit is NOT MEASURED, and saying so is the
    difference between "this plugin is doing nothing" and "this plugin is not in the signal".

    THE THIRD FIGURE IS GR ONLY WHEN IT REALLY IS ONE (grKnown, which SlotPicture sets for an
    EchoJay device or a CERTIFIED map). Otherwise it is the difference across the slot, which needs
    BOTH legs to exist - a difference computed from one reading is arithmetic on a floor. */
inline SlotMeterOne slotMeterEntry (bool bypassed, bool measured,
                                    bool inKnown, float inRmsDb, float inPeakDb,
                                    bool outKnown, float outRmsDb, float outPeakDb,
                                    bool grKnown, float grHeldDb,
                                    float heardSeconds)
{
    SlotMeterOne e;
    if (bypassed) { e.fields = kSlotMeterBypassed; return e; }
    if (! measured) return e;                        // no blend node: nothing measures this slot

    auto finite = [] (float v) { return std::isfinite (v) && v > -199.0f; };

    if (inKnown && finite (inRmsDb))   { e.inDb      = inRmsDb;   e.fields |= kSlotMeterIn; }
    if (inKnown && finite (inPeakDb))  { e.inPeakDb  = inPeakDb;  e.fields |= kSlotMeterInPeak; }
    if (outKnown && finite (outRmsDb)) { e.outDb     = outRmsDb;  e.fields |= kSlotMeterOut; }
    if (outKnown && finite (outPeakDb)){ e.outPeakDb = outPeakDb; e.fields |= kSlotMeterOutPeak; }

    if (grKnown && std::isfinite (grHeldDb))
    { e.thirdDb = grHeldDb; e.fields |= kSlotMeterGr; }
    else if ((e.fields & kSlotMeterIn) != 0u && (e.fields & kSlotMeterOut) != 0u)
    { e.thirdDb = e.outDb - e.inDb; e.fields |= kSlotMeterDiff; }

    e.heardMs = (juce::uint32) juce::jlimit (0.0, 4.0e9, (double) heardSeconds * 1000.0);
    return e;
}

// ---- THE READER'S OWN DECISION: DRAW IT, OR BLANK THE CARDS? --------------------------------

/** How long a sequence may stand still before the cards blank. The Link publishes at 30 Hz, so
    this is fifteen missed ticks: long enough that a busy message thread does not flicker the
    cards, short enough that he never reads a level from a Link that has gone. */
static constexpr double kSlotMeterStaleMs = 500.0;

enum class SlotMeterUse { Draw, Blank };

struct SlotMeterDecision
{
    SlotMeterUse what = SlotMeterUse::Blank;
    const char*  why  = "";
};

/** `msSinceSeqMoved` is measured by the caller from the last time the sequence CHANGED, not from
    the last time it read - a reader that polls a dead file would otherwise reset its own clock for
    ever. `liveRevision` < 0 means the V2 does not know the rack's revision yet, which is not a
    reason to refuse: the Link's own revision is then the only one there is. */
inline SlotMeterDecision slotMeterDecision (bool frameRead,
                                            juce::uint32 frameRevision,
                                            int liveRevision,
                                            double msSinceSeqMoved)
{
    if (! frameRead)
        return { SlotMeterUse::Blank, "this Link does not publish slot meters - an older build, so "
                                      "the cards show what they always showed" };
    if (msSinceSeqMoved > kSlotMeterStaleMs)
        return { SlotMeterUse::Blank, "this Link has stopped publishing - the cards blank rather "
                                      "than hold a level that was true a moment ago" };
    if (liveRevision >= 0 && frameRevision != (juce::uint32) liveRevision)
        return { SlotMeterUse::Blank, "this frame was taken before the rack changed shape, so its "
                                      "figures belong to cards that are no longer there" };
    return { SlotMeterUse::Draw, "" };
}

/** What ONE card draws for one figure. Returns false for "no reading", and a caller that gets
    false draws the dash it draws today - never 0.0, and never the last number.

    THE WHOLE POINT OF THIS FUNCTION is that there is one place that turns a mask bit and a float
    into "a figure or a dash", so a card cannot read a floor as a measurement by forgetting a gate. */
inline bool slotMeterFigure (const SlotMeterOne& e, juce::uint32 field, float& dbOut)
{
    if ((e.fields & field) == 0u) return false;
    if (field == kSlotMeterGr || field == kSlotMeterDiff) { dbOut = e.thirdDb; return true; }
    if (field == kSlotMeterIn)      { dbOut = e.inDb;      return true; }
    if (field == kSlotMeterOut)     { dbOut = e.outDb;     return true; }
    if (field == kSlotMeterInPeak)  { dbOut = e.inPeakDb;  return true; }
    if (field == kSlotMeterOutPeak) { dbOut = e.outPeakDb; return true; }
    return false;
}

/** The third figure's LABEL, from its own provenance. Two words, and they are never swapped:
    "GR" is a claim about a plugin's own reduction and "Δ" is a claim about two of our own readings. */
inline const char* slotMeterThirdLabel (const SlotMeterOne& e)
{
    if ((e.fields & kSlotMeterGr)   != 0u) return "GR";
    if ((e.fields & kSlotMeterDiff) != 0u) return "\xce\x94";    // a Greek capital delta, UTF-8
    return "";
}

/** And what a slot with NO readings says, in the card's words rather than in a number. */
inline const char* slotMeterNoReadingWhy (const SlotMeterOne& e)
{
    if ((e.fields & kSlotMeterBypassed) != 0u) return "bypassed - not in the signal";
    if (e.fields == 0u)                        return "nothing measured yet";
    return "";
}

// ---- WHERE THEY GO ON THE TILE, FROM ONE AUTHOR ---------------------------------------------
//
// The 15 Oct lesson, applied before it can bite: the picture caption was drawn at y=47 and the
// button row was laid out at y=47, from two places that had never been compared, so the caption sat
// on the buttons. These rects come from here, so the tile's paint, its resized() and the guard all
// read one piece of arithmetic.
//
// THE FREE BAND IS THE KNOB'S OWN ROW. A 118x64 tile is: the name row and the IN/OUT TRIM readouts
// (0..16), the wet knob centred at 17..39, the trim/badge caption band (39..47) and the button row
// (47..64). The only unused space is the knob's FLANKS - the knob is 22 px wide in a 118 px tile -
// and that is where these go: IN and OUT on the left of it, the third figure on the right.
struct SlotMeterFlanks { juce::Rectangle<int> left, right; };

inline SlotMeterFlanks slotMeterFlanksFor (int w, int h, int knobW = 22, int knobY = 17,
                                           int knobH = 22)
{
    const int knobX = (w - knobW) / 2;
    const int lw    = juce::jmax (0, knobX - 4);          // 2 px inset, 2 px clear of the knob
    const int rx    = knobX + knobW + 2;
    const int rw    = juce::jmax (0, w - 2 - rx);
    juce::ignoreUnused (h);
    juce::Rectangle<int> left  { 2,  knobY, lw, knobH };
    juce::Rectangle<int> right { rx, knobY, rw, knobH };
    // ---- AND THE TRIM READOUTS ARE ASKED WHERE THEY ARE, NOT ASSUMED (SH22r, 25 Oct 2026) ----
    //
    // The first cut of this put both flanks at the knob's own top (y 17) and the right one ran 3 px
    // into the OUT trim readout, which sits at y 11..20 in the top-right corner. The guard caught it
    // before the build existed - which is the whole reason that leg asks layOutCardReadouts rather
    // than a typed-out rectangle.
    //
    // So the relationship has ONE author: whichever flank would sit under a readout starts below it,
    // and the other keeps its full height. If the readouts ever move, this follows them.
    const auto lay = layOutCardReadouts (w, 16);
    const int clearTop = lay.out.getBottom() + 1;
    const int bottom   = knobY + knobH;
    auto lower = [clearTop, bottom] (juce::Rectangle<int> r)
    { return juce::Rectangle<int> { r.getX(), clearTop, r.getWidth(), juce::jmax (0, bottom - clearTop) }; };
    if (left .intersects (lay.in) || left .intersects (lay.out)) left  = lower (left);
    if (right.intersects (lay.in) || right.intersects (lay.out)) right = lower (right);
    return { left, right };
}

/** THE STRIP ITSELF - two of them per tile, one per flank. Built on the GainReadout precedent
    (Source/EJGainReadout.h): a tiny component with its OWN 20 Hz timer polling a getter, rather than
    a value pushed by the panel's rebuild. The reason is the same one that is written out there - the
    panel's refresh is signature-gated so a tick cannot tear down children under a gesture, and
    putting a 30 Hz meter into that signature would rebuild the panel thirty times a second.

    IT NEVER TAKES A CLICK. setInterceptsMouseClicks(false,false), so selecting the tile, dragging
    the knob and the B/X/</> buttons all behave exactly as they did - a meter that swallowed a click
    on a slot card would be a worse fault than no meter. */
struct SlotMeterStrip : juce::Component, private juce::Timer
{
    enum class Which { InOut, Third };
    Which which = Which::InOut;
    juce::Colour textColour { juce::Colour (0xffb8c0d0) };
    juce::Colour barColour  { juce::Colour (0xff22d3ee) };
    /** TRUE with `out` filled means draw it; FALSE means draw NOTHING - no dash where a figure
        would be, no zero, no last reading. The caller's own staleness rule decides. */
    std::function<bool (SlotMeterOne&)> get;

    /** The meter's own range. -60 is where a slot's RMS stops being informative on a card this
        size; the bar clamps, the figure does not. */
    static constexpr float kFloorDb = -60.0f, kTopDb = 0.0f;

    SlotMeterStrip()
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (20);
    }
    ~SlotMeterStrip() override { stopTimer(); }

    void timerCallback() override
    {
        if (! get) { if (had_) { had_ = false; repaint(); } return; }
        SlotMeterOne e;
        const bool have = get (e);
        // REPAINT ONLY WHEN SOMETHING MOVED, to 0.1 dB. A card that repaints twenty times a second
        // on a silent rack is twenty repaints a second of the whole strip.
        const bool changed = have != had_
            || (have && (! near_ (e.inDb, shown_.inDb) || ! near_ (e.outDb, shown_.outDb)
                         || ! near_ (e.thirdDb, shown_.thirdDb) || e.fields != shown_.fields));
        had_ = have; if (have) shown_ = e;
        if (changed) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (! had_) return;                       // blank, not zero, and not a dash
        if (which == Which::Third) { paintThird (g); return; }
        const int h = getHeight();
        const int rowH = juce::jmax (4, (h - 2) / 2);
        paintOne (g, "IN",  kSlotMeterIn,  { 0, 0, getWidth(), rowH });
        paintOne (g, "OUT", kSlotMeterOut, { 0, h - rowH, getWidth(), rowH });
    }

private:
    static bool near_ (float a, float b) { return std::abs (a - b) < 0.1f; }

    void paintOne (juce::Graphics& g, const char* tag, juce::uint32 field,
                   juce::Rectangle<int> r)
    {
        float db = 0.0f;
        const bool have = slotMeterFigure (shown_, field, db);
        g.setFont (juce::Font (juce::FontOptions (6.5f)));
        g.setColour (textColour.withAlpha (have ? 0.85f : 0.35f));
        g.drawText (tag, r.getX(), r.getY(), 14, r.getHeight(), juce::Justification::centredLeft);
        if (! have)
        {
            // NO READING IS A DASH, and a dash is not a number: it cannot be misread as 0.0 dB,
            // which is what an ungated field would have drawn here.
            g.drawText ("-", r.getX() + 15, r.getY(), r.getWidth() - 15, r.getHeight(),
                        juce::Justification::centredLeft);
            return;
        }
        const int barX = r.getX() + 15;
        const int barW = juce::jmax (1, r.getWidth() - 15 - 22);
        const float t = juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kTopDb - kFloorDb));
        g.setColour (barColour.withAlpha (0.18f));
        g.fillRect (barX, r.getCentreY() - 1, barW, 3);
        g.setColour (db > -1.0f ? juce::Colour (0xfff59e0b) : barColour.withAlpha (0.8f));
        g.fillRect (barX, r.getCentreY() - 1, juce::jmax (1, (int) (barW * t)), 3);
        g.setColour (textColour.withAlpha (0.8f));
        g.drawText (juce::String (db, 1), r.getRight() - 22, r.getY(), 22, r.getHeight(),
                    juce::Justification::centredRight);
    }

    void paintThird (juce::Graphics& g)
    {
        const auto* label = slotMeterThirdLabel (shown_);
        if (*label == 0)
        {
            // Nothing to say, said as nothing - not as "GR 0.0".
            const auto* why = slotMeterNoReadingWhy (shown_);
            if (*why == 0) return;
            g.setFont (juce::Font (juce::FontOptions (6.0f)));
            g.setColour (textColour.withAlpha (0.35f));
            g.drawText (juce::String (why), getLocalBounds(), juce::Justification::centredRight, true);
            return;
        }
        const bool isGr = (shown_.fields & kSlotMeterGr) != 0u;
        g.setFont (juce::Font (juce::FontOptions (6.5f, juce::Font::bold)));
        g.setColour (textColour.withAlpha (isGr ? 0.85f : 0.55f));
        g.drawText (juce::String::fromUTF8 (label), getLocalBounds().removeFromTop (getHeight() / 2),
                    juce::Justification::centredRight);
        g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
        g.setColour (isGr ? juce::Colour (0xff22d3ee) : textColour.withAlpha (0.7f));
        g.drawText (juce::String (shown_.thirdDb, 1),
                    getLocalBounds().removeFromBottom (getHeight() / 2),
                    juce::Justification::centredRight);
    }

    SlotMeterOne shown_;
    bool had_ = false;
};

} // namespace echojay
