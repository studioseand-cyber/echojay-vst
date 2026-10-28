#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"      // CaptureSnapshot, ChannelMeterData
#include "EJMatchPage.h"          // MatchAxis, MatchRefSide, MatchSide, the page's geometry and words
#include "EJReferenceIndex.h"     // RefEntry

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

struct MatchHost
{
    virtual ~MatchHost() = default;

    // ---- what there is to compare -------------------------------------------------------------
    virtual const CaptureSnapshot* latestCapture() const = 0;
    virtual const CaptureSnapshot* verifyCapture() const = 0;
    virtual const RefEntry*        selectedReference() const = 0;
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
};

} // namespace echojay
