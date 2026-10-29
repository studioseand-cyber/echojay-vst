#pragma once
#include <JuceHeader.h>
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
          Kind::Live       NULLPTR. The live signal is not a capture. Returning latestCapture()
                           here would be the very substitution this function exists to stop;
          Reference / CodecFile / Empty   NULLPTR - none of them is a capture of this session.

        So a caller must handle nullptr, and when it gets one the honest move is her existing
        refusal rather than a guess. */
    virtual const CaptureSnapshot* captureForSlot (const CompareSlotState& slot) const = 0;
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
    virtual void postToSessionChat (const juce::String& kind, const juce::var& payload) = 0;
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
