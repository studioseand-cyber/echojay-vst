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
    const CaptureSnapshot* verifyCapture() const override { return nullptr; }
    const ReferenceResult* selectedReference() const override { return ref ? &*ref : nullptr; }
    std::vector<MatchLink> liveLinks() const override { return links; }
    bool pickMixSlot (const juce::String&, juce::String&) override { return false; }
    bool pickReferenceSlot (const juce::String&, juce::String&) override { return false; }
    void openReferenceBrowser (bool) override {}
    void setAudible (MatchRefSide) override {}
    void seekCompareStream (int, float) override {}
    void postToSessionChat (const juce::String& kind, const juce::var& payload) override { posts.push_back ({ kind, payload }); }
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
    MatchSides buildMatchSides() const override { return {}; }
    void openMixPicker() override {}
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
