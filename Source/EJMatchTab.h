#pragma once
#include <JuceHeader.h>
#include <optional>
#include "PluginProcessor.h"      // CaptureSnapshot, ChannelMeterData
#include "EJMatchPage.h"          // MatchAxis, MatchRefSide, MatchSide, the page's geometry and words
#include "ReferenceAnalyser.h"    // ReferenceResult - the MEASURED reference, see selectedReference
#include "EJCompareSlotState.h"    // CompareSlotState - the slots the page compares
#include "MeterEngine.h"           // MeterEngine, MeterData - the page's own fast meters
#include "EJSpectralEvidence.h"    // SpectralEvidence, which a slot answers with

/** ---- THE MATCH TAB'S ONE DOOR TO THE REST OF THE PLUGIN (C2, 27 Oct 2026) -------------------

    `docs/CONTRACT_MATCH_HOST_2026-10-27.md` is the contract; this header is its code. The Match tab
    calls `MatchHost` and nothing else, and the editor implements it. Two reasons it exists:

    1. K owns the Match front end on her own branch (`k/match`), and A owns the editor. A boundary
       that is a named interface is one line of A's to extend; a reach into the editor is a merge
       conflict on every rebase.
    2. The page's dependencies were 15 members of a 50,000-line class reached through an `owner`
       pointer. Written down, they are this - and most of them turned out to be four questions
       ("what was captured", "which reference", "which Links", "make a sound") wearing fifteen names.

    THIS HEADER MUST NOT INCLUDE PluginEditor.h. The editor includes this one to inherit the
    interface, so the dependency runs one way only: nothing here may name a type nested inside
    EchoJayEditor, which is also why slots are addressed BY NAME (MATCH_SCREEN_CONTRACT §2B) rather
    than by the editor's own CompareSlotState. */
namespace echojay
{

/** A Link as the Match page needs to see it: a projection, deliberately NOT LinkSlotInfo, so the
    tab cannot start addressing Links itself. */
struct MatchLink
{
    juce::String uid, name, role;
    juce::StringArray caps;
    juce::int64 framesReceived = -1;
};

/** The two sides of the compare NAMED BY ROLE, and the two slots they came from. Both used to be
    nested inside EchoJayEditor, which meant this header could not say what matchSlots answers
    without including the editor's - so they are lifted here and the editor aliases them. The
    members and names are unchanged, so no call site in either file moves. */
struct MatchSides { MatchSide mix, ref; };
struct MatchSlotPair { const CompareSlotState* mix = nullptr; const CompareSlotState* ref = nullptr; };

/** ---- WHICH CAPTURE A SLOT MEANS, AS A PURE LOOKUP (Kathy's R4-2 / C's point 55) -----------

    Extracted so it can be LEGGED. The behaviour that matters is not "the editor compiles" but:

      * a Snapshot slot resolves to the capture at ITS index, never the newest one;
      * two different snapshot slots resolve to two DIFFERENT captures;
      * every other slot kind resolves to nothing, because none of them is a capture of this
        session (see the table on captureForSlot below);
      * the answer is a VALUE, so no later call can change an answer already given. That last one
        is the property my first version broke by returning a pointer into a shared cache, and it
        is structural here rather than a rule a caller has to obey.

    A fake MatchHost could not test any of this - it would be testing the fake. This can. */
inline std::optional<CaptureSnapshot> captureForSlotIn (const std::vector<CaptureSnapshot>& snaps,
                                                        const CompareSlotState& slot)
{
    if (slot.kind != CompareSlotState::Kind::Snapshot) return std::nullopt;
    if (slot.index < 0 || slot.index >= (int) snaps.size()) return std::nullopt;
    return snaps[(size_t) slot.index];
}

/** ---- AND THE LIVE SLOT, WHICH IS NOW AN ANSWER (Sean's Match ruling, 29 Oct 2026) ---------

    *"AI MATCH must work with the "Live signal" slot, using what has played, with no separate
    capture needed ... If less than the minimum has played ... AI MATCH says "Play at least 30
    seconds of the song, then press AI MATCH again"."*

    A capture-SHAPED snapshot, built from the rolling series rather than from a capture: the hops
    are what played, `hopFramesHops` is their count, and the figures a capture gets from its own
    engines are deliberately left at their defaults here - this carries the SERIES, which is what
    her loudest-30 window and every band figure are computed from.

    IT RETURNS NOTHING BELOW THE TONE MINIMUM, which is requirement 5's floor: with less than
    thirty seconds there is no honest band answer, and her page shows his sentence instead. Above
    it, the dynamics may still be unready, and `liveBufferState().dynamicsReady` is what says so -
    this function does not pretend to answer that question by returning or withholding a snapshot.

    PURE, so a leg can hold it without audio, a transport or a rack. */
inline std::optional<CaptureSnapshot> liveCaptureFrom (const std::vector<MeterEngine::HopFrame>& hops,
                                                       const std::vector<MeterEngine::HopFrame>& hopsRackIn,
                                                       float playedS,
                                                       bool toneReady)
{
    if (! toneReady || hops.empty()) return std::nullopt;
    CaptureSnapshot s;
    s.hopFrames           = hops;
    s.hopFramesRackIn     = hopsRackIn;
    s.hopFramesHops       = (int) hops.size();
    s.hopFramesRackInHops = (int) hopsRackIn.size();
    s.durationSeconds     = playedS;   // what PLAYED, which is this snapshot's window
    s.isLive              = true;      // so nothing downstream reads it as a capture he took
    return s;
}

struct MatchHost
{
    virtual ~MatchHost() = default;

    // ---- what there is to compare -------------------------------------------------------------
    virtual const CaptureSnapshot* latestCapture() const = 0;

    /** ---- THE CAPTURE THE SLOT IS ACTUALLY SHOWING (Kathy's R4-2, 29 Oct 2026) -------------

        Her fault: the window reads `latestCapture()`, so an OLDER snapshot sitting in the mix slot
        gives bands from a different capture than the one on screen - two pictures of two different
        moments, presented as one. Her page refuses loudest-30 and the plan rather than show that,
        which is the right refusal and the reason this exists.

        WHAT IT CAN ANSWER, and what it cannot, because a host that quietly returns the wrong
        capture is worse than one that returns none:

          Kind::Snapshot   the capture at that index - this is her case, and it is the fix;
          Kind::WsCapture  NULLPTR. A workspace review carries `channels`
                           (WsChannelMeasurements), not a CaptureSnapshot, so there is nothing of
                           this type to return. Converting one is a separate piece of work and must
                           not be faked here;
          Kind::Live       THE ROLLING LIVE SERIES, by value, once the tone minimum has played
                           (Sean's ruling of 29 Oct 2026; C2 §R4-3). Below that minimum it is
                           still nullopt and her page shows "Play at least 30 seconds...". It is
                           NEVER latestCapture(): the substitution this function exists to stop
                           stays stopped, and the snapshot carries isLive so nothing downstream
                           calls what was playing "your capture";
          Reference / CodecFile / Empty   NULLPTR - none of them is a capture of this session.

        So a caller must handle an empty answer, and when it gets one the honest move is her
        existing refusal rather than a guess.

        ---- IT RETURNS BY VALUE, AND THAT IS THE WHOLE POINT (C's point 55, 29 Oct 2026) -------

        My first version returned `const CaptureSnapshot*` into `latestCaptureCache_` - the SAME
        mutable member latestCapture() writes - which reintroduced the exact fault this function
        exists to remove, one layer down:

          * captureForSlot(mix) then latestCapture() silently turned the first pointer into the
            NEWEST capture - the substitution R4-2 is about;
          * two snapshot slots fetched in turn gave two equal pointers to the second slot's data,
            so the window would have compared a capture with itself and found it identical.

        Caching by value avoided the 18 Sep crash and bought a silent wrong answer instead, which is
        the worse trade. An optional BY VALUE has no aliasing, no lifetime rule for a caller to
        obey, and no shared cache to interfere with - the copy costs one snapshot at UI rate, and
        getSnapshots() already returns its vector by value for the same reason. */
    virtual std::optional<CaptureSnapshot> captureForSlot (const CompareSlotState& slot) const = 0;

    /** ---- WHAT HAS PLAYED, AND WHAT IT IS READY FOR (Sean's Match ruling, 29 Oct 2026) -----

        C2 §R4-3. `captureForSlot(Kind::Live)` answers with a snapshot of the rolling series once
        the TONE minimum has played, and this says how much that is, what is still missing, and why
        nothing is being kept when nothing is - which is what her page shows instead of refusing
        everything (requirement 5).

        Safe on every repaint: it reads two counters and a flag. */
    virtual echojay::LiveBufferState liveBufferState() const = 0;

    /** Drop everything kept. Her page never needs this; the processor calls it on a source change,
        and it is on the boundary so a test can put the buffer in a known state. */
    virtual void clearLiveBuffer() = 0;
    virtual const CaptureSnapshot* verifyCapture() const = 0;
    /** THE REFERENCE AS MEASURED, not as filed. Two types in this codebase could answer "which
        reference": echojay::RefEntry is the LIBRARY RECORD (id, path, availability, the measurements
        last written to disk), and ReferenceResult is what ReferenceAnalyser produced in this session
        - the bands, the EQ curve, the thumbnail and, since A12b, the per-hop series. The page
        compares numbers, so it gets the measured one; it is also what a Reference slot's index
        points into (applyReferenceToSlot). C2 §1 named RefEntry and that was wrong. */
    virtual const ReferenceResult* selectedReference() const = 0;
    virtual std::vector<MatchLink> liveLinks() const = 0;

    // ---- the two slots, BY NAME (MATCH_SCREEN_CONTRACT §2B) -----------------------------------
    virtual bool pickMixSlot       (const juce::String& name, juce::String& why) = 0;
    virtual bool pickReferenceSlot (const juce::String& name, juce::String& why) = 0;
    virtual void openReferenceBrowser (bool forReferenceSide) = 0;

    // ---- listening ----------------------------------------------------------------------------
    virtual void setAudible (MatchRefSide side) = 0;
    virtual void seekCompareStream (int slotIdx, float fraction) = 0;

    // ---- the plan, and the chat ---------------------------------------------------------------
    /** Posts a card to the session chat and RETURNS THE MESSAGE'S ID, empty when nothing was
        posted (Kathy's R6-1 option 1, pulled forward on 31 Oct 2026 after Sean's 13:45 test).

        The id is what makes "Report in chat" truthful: the page shows that line only when a card
        really arrived, and openSessionChatAtMessage takes the page back to this exact card.

        SYNCHRONOUS, deliberately. An id that arrives later cannot be used by the press that caused
        it, and a page that has to wait for one would be showing a promise rather than a fact.

        Until A3's Whole session chat exists the card goes to the chat that is OPEN NOW (Sean's
        ruling, Kathy agreeing), never into nothing - which is what it did until today. */
    virtual juce::String postToSessionChat (const juce::String& kind, const juce::var& payload) = 0;

    /** Opens the chat that holds `messageId` and brings that message into view.

        READ ONLY: it never posts and never duplicates, however many times it is called and
        whichever chat is open when it is. A repeated press for the same comparison points here
        instead of posting a second card (Kathy's 6b). A messageId that no chat holds is a no-op
        that says so in the log rather than opening something arbitrary. */
    virtual void openSessionChatAtMessage (const juce::String& messageId) = 0;
    virtual void draftPlan (const juce::var& matchBlock,
                            std::function<void (bool ok, juce::String planId, juce::String why)> cb) = 0;
    virtual void openSessionChatAt (const juce::String& planId) = 0;

    // ---- which build this is ------------------------------------------------------------------
    virtual bool isV2() const = 0;
    virtual bool agentOn() const = 0;
    virtual bool hasCap (const juce::String& cap) const = 0;
    virtual juce::String pluginVersion() const = 0;

    // ---- WHAT THE PAGE ACTUALLY REACHED FOR (A0b, 28 Oct 2026) ------------------------------
    //
    // The carve moved the page's implementation; its fifteen `owner->` uses survived it, pointing
    // straight into the editor. These are those uses, written down. Four of them are the page's own
    // business and moved onto the tab instead (the sides, the slot pair, the mix picker and which
    // side is the reference); the rest are genuinely the editor's and belong here.
    //
    // Every one takes or returns a type this header can name - which is why CompareSlotState was
    // lifted out of EchoJayEditor first. A boundary that cannot be stated without the other side's
    // header is not a boundary.
    virtual juce::String slotDisplayName (const CompareSlotState& slot) const = 0;
    virtual echojay::SpectralEvidence slotSpectralEvidence (const CompareSlotState& slot) const = 0;
    virtual void  toggleComparePlay (bool isTop) = 0;
    virtual float compareStreamFrac (int slotIdx) const = 0;
    /** The fast meters a trail is drawn from: the engine when there is one, and one frame of it.
        Both require the slot to be loaded AND playing, which is the page's own precondition. */
    virtual MeterEngine* matchFastEngine (const CompareSlotState& slot, int slotIdx) const = 0;
    virtual bool matchFastFrame (const CompareSlotState& slot, int slotIdx, MeterData& out) const = 0;
    /** The signature is the editor's own, not a tidied one: `rolling` says whether the picture is a
        live rolling window, and `spanSeconds` how much time it covers. A boundary that renames its
        own arguments is a second thing to keep in step. */
    virtual bool matchWavePoints (const CompareSlotState& slot, std::vector<float>& outAbs,
                                  bool& rolling, float& spanSeconds) const = 0;
    /** Which side carries the reference, decided by CONTENT when the page opens and held by the
        editor because setRefSubTab's Match branch is what sets it. */
    virtual bool matchRefIsTop() const = 0;
    virtual void setMatchRefIsTop (bool topIsReference) = 0;
    /** Back to Compare - the page's Escape, and the only tab move it ever asks for. */
    virtual void goBackToCompare() = 0;
    /** The sample rate the streams are running at, for a cadence the page draws against. */
    virtual double streamSampleRate() const = 0;
    /** Which slot is audible right now, and whether a stream is loaded and playing for a side. */
    virtual bool compareSlotAudible (int slotIdx) const = 0;
    virtual bool compareSlotPlaying (int slotIdx) const = 0;

    /** THE PAIR AND THE SIDES. The page reads both in one tick: the slots for the curves (each
        one's spectral evidence) and the sides for the six bands. They stay the HOST's answer
        rather than the page's because which slot is the reference is held by the editor
        (setRefSubTab's Match branch sets it) and a second derivation here would be a second
        answer to the same question. */
    virtual MatchSlotPair matchSlots() const = 0;
    virtual MatchSides buildMatchSides() const = 0;

    /** The left picker. openReferenceBrowser is the right one; this one offers Live, the session
        snapshots and the chat captures and NEVER a reference, which is half of why the two roles
        cannot swap slots. */
    virtual void openMixPicker() = 0;
};

} // namespace echojay
