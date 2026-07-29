/*
    DashboardTab.h

    Session C: the plugin Dashboard tab.

    ONE PAYLOAD, TWO RENDERERS. This is the second renderer. Everything drawn
    here comes from GET /api/v2/dashboard?surface=plugin, the same document the
    web app renders, and there is never a second source of truth for dashboard
    content. The parse below is the whole contract: field names match
    lib/dash/types.js in echojay-saas and must not drift.

    THIN, NOT A MIRROR. Read and navigate only. There is NO text entry anywhere
    on this surface and there must never be one: the moment it grows a form it
    stops being a day's work and starts being a second web app.

    GEOMETRY, from the first line rather than retrofitted. PluginEditor.cpp has
    produced four overlap bugs and every rule below is one of them:

      - layout(width) is the SOLE geometry author. It stores rects and returns
        the content height. paint() consumes those rects and MEASURES NOTHING.
        mouseDown() consumes the same rects, so a hit test cannot disagree with
        what was drawn.
      - Every rect is authored on EVERY layout pass, cleared first, so a card
        that is not shown this pass cannot keep last pass's coordinates.
      - This whole surface is ONE child component. It is shown exactly when the
        Dashboard tab is selected, from a single unconditional expression in
        EchoJayEditor::resized(). Nothing it draws can leak onto another tab,
        because it is not the editor drawing it.

    WHAT IT DOES NOT DO. There is no plugin-to-web token path in either repo
    (surveyed 29 Jul 2026), so every outbound link here has to work with no
    auth at all. That leaves the public chain page /c/:slug and /upgrade, which
    the plugin already opens signed out. Anything else would land the user on a
    login screen, so it is rendered as text and never as a button. The
    onboarding "Claim your handle" step is the visible case.
*/

#pragma once

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <set>
#include <vector>

/** tools/dashboard_test. Forward declared at GLOBAL scope on purpose: a bare
    `friend struct X;` inside a class in namespace echojay would befriend
    echojay::X, not the test's ::X, and the test would fail to compile with a
    wall of private-member errors that look like a missing friend rather than a
    misplaced one. */
struct EchoJayDashboardTestAccess;

namespace echojay
{

/** An abstract deep link, NOT a URL scheme. The renderer interprets it: the
    plugin switches tab and selects the id, the web app routes. `path` is only
    ever set when surface == "web". */
struct DashLink
{
    juce::String surface, id, path, linkUid;
    bool isEmpty() const noexcept { return surface.isEmpty(); }
};

struct DashUsage
{
    bool present = false;
    int unitsUsed = 0, unitsTotal = 0;
    /** Pre-rounded SERVER side so both renderers show the same number.
        Do not recompute it from used/total. */
    int pct = 0;
    juce::String resetsAt, tierLabel, upgradeUrl;
};

struct DashContinue
{
    bool present = false;
    juce::String kind, label;
    DashLink target;
};

struct DashProject
{
    juce::String id, name, genre;
    /** "procedural" | "upload" | "audio". seed is ALWAYS present, including
        for uploads, so there is something honest to draw before an uploaded
        image arrives and something to fall back to if it never does. */
    juce::String artKind, artSeed, artUrl;
    int chats = 0, captures = 0, chains = 0;
};

struct DashChat
{
    juce::String id, title, snippet;
};

struct DashChain
{
    juce::String id, name, shareSlug, shareVisibility, source;
    int slotCount = 0;
    int qualityScore = -1;   // -1 = null server side
};

struct DashOnboardingStep
{
    juce::String key, label;
    bool done = false;
    DashLink target;
};

struct DashAnnouncement
{
    bool present = false;
    juce::String id, snippet, postedAt;
};

/** The parsed payload. `valid` is false for anything that did not parse as a
    dashboard document, so a garbled body can never render as an empty
    dashboard that looks like an empty account. */
struct DashPayload
{
    bool valid = false;
    juce::String handle, displayName, tier;
    bool hasProfile = false;
    int ttlSeconds = 60;

    DashUsage usage;
    DashContinue continueCard;
    std::vector<DashProject> projects;
    std::vector<DashChat> recentChats;
    std::vector<DashChain> chains;

    bool onboardingComplete = true;
    std::vector<DashOnboardingStep> onboardingSteps;
    DashAnnouncement announcement;

    static DashPayload parse (const juce::var& json);
};

/**
    The Dashboard surface. Lives inside a juce::Viewport so it scrolls at the
    900x580 full-mode floor without the layout having to fight for room.
*/
class DashboardView final : public juce::Component
{
public:
    DashboardView();
    ~DashboardView() override = default;

    /** Content. `fromCache` says whether this came off disk: a cached payload
        does NOT clear the last error, because rendering the cache is not
        evidence that the network came back. */
    void setPayload (DashPayload p, juce::int64 fetchedAtMs, bool fromCache);
    void setStatus (bool loading, const juce::String& error);
    void setSignedOut (bool signedOut);

    bool hasPayload() const noexcept { return payload_.valid; }

    /** THE geometry author. Stores every rect and returns the total content
        height. Pure in width: calling it twice with the same width produces
        the same rects, which is what lets the owner call it once to size the
        component and let resized() call it again. */
    int layout (int width);
    int getContentHeight() const noexcept { return contentH_; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    /** A deep link the user asked to follow. The owner interprets it. */
    std::function<void (const DashLink&)> onNavigate;
    /** An absolute URL that works with NO auth. The owner opens the browser. */
    std::function<void (const juce::String&)> onOpenUrl;
    /** An uploaded project image this view needs and does not have. Fired once
        per url; the owner answers with setArtImage. */
    std::function<void (const juce::String&)> onNeedArt;

    /** Hands back an image requested through onNeedArt. A null image records
        the failure so the request is not repeated on every repaint, and the
        procedural seed keeps the tile honest. */
    void setArtImage (const juce::String& url, const juce::Image& img);

private:
    // ---- one hit zone: rect plus what a click on it means -------------------
    struct Zone
    {
        juce::Rectangle<int> rect;
        DashLink link;         // empty when the zone opens a url instead
        juce::String url;      // empty when the zone follows a link
    };

    // ---- rects, ALL authored by layout(), ALL cleared at its top ------------
    juce::Rectangle<int> headerRect_, statusRect_;
    juce::Rectangle<int> usageRect_, usageBarRect_, usageResetRect_, upgradeChipRect_;
    juce::Rectangle<int> continueRect_;
    juce::Rectangle<int> projectsHeadingRect_, projectsEmptyRect_;
    std::vector<juce::Rectangle<int>> projectTileRects_;   // art square only
    juce::Rectangle<int> chatsHeadingRect_, chatsEmptyRect_;
    std::vector<juce::Rectangle<int>> chatRowRects_;
    juce::Rectangle<int> chainsHeadingRect_, chainsEmptyRect_;
    std::vector<juce::Rectangle<int>> chainRowRects_;
    std::vector<juce::Rectangle<int>> chainShareChipRects_;  // empty rect = no share
    juce::Rectangle<int> onboardingRect_, onboardingHeadingRect_;
    std::vector<juce::Rectangle<int>> onboardingStepRects_;
    juce::Rectangle<int> announceRect_;
    juce::Rectangle<int> signedOutRect_;

    /** Lets tools/dashboard_test reach the rects and the hit test without
        widening the shipped API. Same reasoning as
        EchoJayTabStripTestAccess in PluginEditor.h: a test is not a reason to
        make state public, and a friend declaration changes no layout, size or
        vtable. The test must exercise the code that SHIPS, not a copy of it;
        a copy in a test is the duplication bug in a different file. */
    friend struct ::EchoJayDashboardTestAccess;

    std::vector<Zone> zones_;
    int hoverZone_ = -1;
    int contentH_ = 0;

    DashPayload payload_;
    juce::int64 fetchedAtMs_ = 0;
    bool loading_ = false;
    bool signedOut_ = false;
    juce::String error_;

    // Uploaded project art. An entry with a null image is a request that
    // FAILED and must not be retried every paint.
    std::map<juce::String, juce::Image> artImages_;
    std::set<juce::String> artRequested_;

    /** THE hit test. FIRST match wins; see the note on the definition. */
    int zoneIndexAt (juce::Point<int> p) const;

    void requestMissingArt();
    void addZone (juce::Rectangle<int> r, const DashLink& link);
    void addUrlZone (juce::Rectangle<int> r, const juce::String& url);
    void drawTile (juce::Graphics& g, const DashProject& p, juce::Rectangle<int> square);
    juce::String statusLine() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DashboardView)
};

/** Site root for the few links that work with no auth at all. */
inline juce::String dashSiteRoot() { return "https://www.echojay.ai"; }

} // namespace echojay
