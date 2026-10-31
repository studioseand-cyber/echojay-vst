#pragma once

// =============================================================================
//  A MatchHost FOR THE PINS (round 5).
// =============================================================================
//
// It answers the calls the Match page's hand-off makes and records the ones
// that send something. captureForSlot is A's real lookup (captureForSlotIn) over
// the fake's own snapshots, so a pin tests the rule A ships rather than a copy
// of it; latestCapture is the newest snapshot, as A's is. Everything the
// hand-off never calls answers a plain default.
// =============================================================================

#include "EJMatchTab.h"
#include "PluginProcessor.h"     // CaptureSnapshot
#include "ReferenceAnalyser.h"   // ReferenceResult

namespace k5pins
{
using namespace echojay;

struct FakeMatchHost : MatchHost
{
    std::vector<CaptureSnapshot> snaps;
    std::optional<ReferenceResult> ref;
    std::vector<MatchLink> links;
    CompareSlotState mixSlot, refSlot;
    bool v2 = true, agent = false;
    juce::StringArray caps;

    // What the page sent, in order.
    struct Post { juce::String kind; juce::var payload; };
    std::vector<Post> posts;
    std::vector<juce::var> drafts;
    juce::StringArray opened;
    // How draftPlan answers.
    bool draftOk = true;
    juce::String draftPlanId = "plan_1", draftWhy;

    mutable CaptureSnapshot latestCache;
    const CaptureSnapshot* latestCapture() const override
    {
        if (snaps.empty()) return nullptr;
        latestCache = snaps.back();
        return &latestCache;
    }
    std::optional<CaptureSnapshot> captureForSlot (const CompareSlotState& s) const override
    {
        return captureForSlotIn (snaps, s);
    }
    // 8c: the verify capture, when a pin sets one.
    std::optional<CaptureSnapshot> verify;
    const CaptureSnapshot* verifyCapture() const override { return verify ? &*verify : nullptr; }
    const ReferenceResult* selectedReference() const override { return ref ? &*ref : nullptr; }
    std::vector<MatchLink> liveLinks() const override { return links; }
    bool pickMixSlot (const juce::String&, juce::String&) override { return false; }
    bool pickReferenceSlot (const juce::String&, juce::String&) override { return false; }
    void openReferenceBrowser (bool) override {}
    // 8b: what the audition switch asked for, in order.
    std::vector<MatchRefSide> audibleCalls;
    void setAudible (MatchRefSide s) override { audibleCalls.push_back (s); }
    void seekCompareStream (int, float) override {}
    // ---- THE POST ANSWERS WITH AN ID (A, 31 Oct 2026, R6-1 pulled forward) ----------------
    //
    // postToSessionChat returns the posted message's id, empty when nothing was posted, so the
    // page can only claim "Report in chat" when a card really arrived. This fake mints a readable
    // one per post and can be told to refuse, which is the case the page's line depends on.
    bool postRefuses = false;                 ///< set true to make the next post land nowhere
    juce::String postToSessionChat (const juce::String& kind, const juce::var& payload) override
    {
        if (postRefuses) return {};
        posts.push_back ({ kind, payload });
        return "fake-msg-" + juce::String ((int) posts.size());
    }
    /** Every openSessionChatAtMessage, in order, so a pin can assert the page LINKED rather than
        posted again. Its own array: `opened` below already holds the plan ids openSessionChatAt
        was given, and one list holding two kinds of handle is a list that proves neither. */
    juce::StringArray openedMessages;
    void openSessionChatAtMessage (const juce::String& messageId) override { openedMessages.add (messageId); }
    void draftPlan (const juce::var& block, std::function<void (bool, juce::String, juce::String)> cb) override
    {
        drafts.push_back (block);
        if (cb) cb (draftOk, draftOk ? draftPlanId : juce::String(), draftWhy);
    }
    void openSessionChatAt (const juce::String& planId) override { opened.add (planId); }
    bool isV2() const override { return v2; }
    bool agentOn() const override { return agent; }
    bool hasCap (const juce::String& c) const override { return caps.contains (c); }
    juce::String pluginVersion() const override { return "2.31.0"; }
    juce::String slotDisplayName (const CompareSlotState& s) const override { return s.label; }
    SpectralEvidence slotSpectralEvidence (const CompareSlotState&) const override { return {}; }
    void toggleComparePlay (bool) override {}
    float compareStreamFrac (int) const override { return 0.0f; }
    MeterEngine* matchFastEngine (const CompareSlotState&, int) const override { return nullptr; }
    bool matchFastFrame (const CompareSlotState&, int, MeterData&) const override { return false; }
    bool matchWavePoints (const CompareSlotState&, std::vector<float>&, bool&, float&) const override { return false; }
    bool matchRefIsTop() const override { return false; }
    void setMatchRefIsTop (bool) override {}
    void goBackToCompare() override {}
    double streamSampleRate() const override { return 48000.0; }
    bool compareSlotAudible (int) const override { return false; }
    bool compareSlotPlaying (int) const override { return false; }
    MatchSlotPair matchSlots() const override { return { &mixSlot, &refSlot }; }
    // The whole-file sides the page windows (8a: matchPressOn reads them).
    MatchSides sides;
    MatchSides buildMatchSides() const override { return sides; }
    void openMixPicker() override {}

    // ---- THE LIVE BUFFER (A, 31 Oct 2026, on merging round 8) ------------------------------
    //
    // MatchHost gained two pure virtuals on A's side for item 2b, liveBufferState() and
    // clearLiveBuffer(), so this fake stopped being constructible and every k3 pin from k5a
    // onward failed to COMPILE in the merged tree. A's pure virtuals, so A's repair, and it goes
    // here rather than into defaults on the base class: a host that cannot answer "what has
    // played" should have to say so, which is what a pure virtual is for.
    //
    // The answer below is a buffer that has heard nothing, which is the honest state for a fake
    // with no audio and keeps every existing pin's expectation unchanged. `live` is public so a
    // pin that wants to drive 2b's wording can set it without touching this class.
    echojay::LiveBufferState live;
    echojay::LiveBufferState liveBufferState() const override { return live; }
    int  liveClears = 0;              ///< so a pin can assert the page cleared it
    void clearLiveBuffer() override { ++liveClears; live = {}; }
};

/** A Snapshot slot at index i. */
inline CompareSlotState snapshotSlot (int i)
{
    CompareSlotState s;
    s.kind = CompareSlotState::Kind::Snapshot;
    s.index = i;
    s.label = "Capture " + juce::String (i + 1);
    return s;
}

} // namespace k5pins
