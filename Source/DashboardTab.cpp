#include "DashboardTab.h"
#include "EchoJayLookAndFeel.h"
#include "ProjectArt.h"

namespace echojay
{

using C = EchoJayLookAndFeel::Colours;

// ===========================================================================
//  Payload parsing
// ===========================================================================
//
// The field names here ARE the contract (lib/dash/types.js in echojay-saas).
// Additive changes only within a major `v`, because the plugin ships on its
// own release cadence and will always lag the web.
//
// Everything is read defensively: a missing block leaves its section empty
// rather than throwing, and `valid` gates the whole render, so a body that is
// not a dashboard document can never draw as an empty account.

namespace
{
juce::String str (const juce::var& v, const char* key)
{
    if (auto* o = v.getDynamicObject())
        if (o->hasProperty (key))
        {
            const auto p = o->getProperty (key);
            if (p.isVoid() || p.isUndefined()) return {};
            return p.toString();
        }
    return {};
}

int intOf (const juce::var& v, const char* key, int fallback = 0)
{
    if (auto* o = v.getDynamicObject())
        if (o->hasProperty (key))
        {
            const auto p = o->getProperty (key);
            if (p.isVoid() || p.isUndefined()) return fallback;
            return (int) p;
        }
    return fallback;
}

bool boolOf (const juce::var& v, const char* key)
{
    if (auto* o = v.getDynamicObject())
        return (bool) o->getProperty (key);
    return false;
}

juce::var child (const juce::var& v, const char* key)
{
    if (auto* o = v.getDynamicObject())
        return o->getProperty (key);
    return {};
}

DashLink parseLink (const juce::var& v)
{
    DashLink l;
    l.surface = str (v, "surface");
    l.id      = str (v, "id");
    l.path    = str (v, "path");
    l.linkUid = str (v, "linkUid");
    return l;
}

/** "2026-08-01T00:00:00Z" -> "1 Aug". Empty when the stamp is unusable, so a
    label never renders a fake date. */
juce::String shortDate (const juce::String& iso)
{
    if (iso.isEmpty()) return {};
    const auto t = juce::Time::fromISO8601 (iso);
    if (t.toMilliseconds() <= 0) return {};
    return juce::String (t.getDayOfMonth()) + " " + t.formatted ("%b");
}
} // namespace

DashPayload DashPayload::parse (const juce::var& json)
{
    DashPayload p;
    auto* root = json.getDynamicObject();
    if (root == nullptr) return p;

    // `usage` is the one block every account has, on every tier, so its
    // presence is what says "this is a dashboard document".
    const auto usage = child (json, "usage");
    if (usage.getDynamicObject() == nullptr) return p;
    p.valid = true;

    p.ttlSeconds = juce::jlimit (10, 600, intOf (json, "ttl", 60));

    const auto user = child (json, "user");
    p.handle      = str (user, "handle");
    p.displayName = str (user, "displayName");
    p.tier        = str (user, "tier");
    p.hasProfile  = boolOf (user, "hasProfile");

    p.usage.present    = true;
    p.usage.unitsUsed  = intOf (usage, "unitsUsed");
    p.usage.unitsTotal = intOf (usage, "unitsTotal");
    p.usage.pct        = juce::jlimit (0, 100, intOf (usage, "pct"));
    p.usage.resetsAt   = str (usage, "resetsAt");
    p.usage.tierLabel  = str (usage, "tierLabel");
    p.usage.upgradeUrl = str (usage, "upgradeUrl");

    const auto cont = child (json, "continue");
    if (cont.getDynamicObject() != nullptr)
    {
        p.continueCard.present = true;
        p.continueCard.kind    = str (cont, "kind");
        p.continueCard.label   = str (cont, "label");
        p.continueCard.target  = parseLink (child (cont, "target"));
    }

    if (auto* arr = child (json, "projects").getArray())
        for (const auto& v : *arr)
        {
            DashProject d;
            d.id    = str (v, "id");
            d.name  = str (v, "name");
            d.genre = str (v, "genre");
            const auto art = child (v, "art");
            d.artKind = str (art, "kind");
            d.artSeed = str (art, "seed");
            d.artUrl  = str (art, "url");
            const auto counts = child (v, "counts");
            d.chats    = intOf (counts, "chats");
            d.captures = intOf (counts, "captures");
            d.chains   = intOf (counts, "chains");
            if (d.id.isNotEmpty()) p.projects.push_back (std::move (d));
        }

    if (auto* arr = child (json, "recentChats").getArray())
        for (const auto& v : *arr)
        {
            DashChat c;
            c.id      = str (v, "id");
            c.title   = str (v, "title");
            c.snippet = str (v, "snippet");
            if (c.id.isNotEmpty()) p.recentChats.push_back (std::move (c));
        }

    if (auto* arr = child (json, "chains").getArray())
        for (const auto& v : *arr)
        {
            DashChain c;
            c.id              = str (v, "id");
            c.name            = str (v, "name");
            c.slotCount       = intOf (v, "slotCount");
            c.qualityScore    = intOf (v, "qualityScore", -1);
            c.source          = str (v, "source");
            c.shareSlug       = str (v, "shareSlug");
            c.shareVisibility = str (v, "shareVisibility");
            if (c.id.isNotEmpty()) p.chains.push_back (std::move (c));
        }

    const auto ob = child (json, "onboarding");
    if (ob.getDynamicObject() != nullptr)
    {
        p.onboardingComplete = boolOf (ob, "complete");
        if (auto* arr = child (ob, "steps").getArray())
            for (const auto& v : *arr)
            {
                DashOnboardingStep s;
                s.key    = str (v, "key");
                s.label  = str (v, "label");
                s.done   = boolOf (v, "done");
                s.target = parseLink (child (v, "target"));
                if (s.key.isNotEmpty()) p.onboardingSteps.push_back (std::move (s));
            }
    }

    const auto community = child (json, "community");
    const auto ann = child (community, "latestAnnouncement");
    if (ann.getDynamicObject() != nullptr)
    {
        p.announcement.present  = true;
        p.announcement.id       = str (ann, "id");
        p.announcement.snippet  = str (ann, "snippet");
        p.announcement.postedAt = str (ann, "postedAt");
    }

    return p;
}

// ===========================================================================
//  The view
// ===========================================================================

DashboardView::DashboardView()
{
    setOpaque (true);
    setWantsKeyboardFocus (false);
}

void DashboardView::setPayload (DashPayload p, juce::int64 fetchedAtMs, bool fromCache)
{
    payload_      = std::move (p);
    fetchedAtMs_  = fetchedAtMs;
    // A NETWORK payload clears the last error; a cached one does not, because
    // rendering the cache is not evidence that the network came back.
    if (! fromCache) error_.clear();
    requestMissingArt();
    layout (getWidth());
    repaint();
}

/** Uploaded project images, asked for ONCE per url on a payload change rather
    than from paint: a fetch that fails must not be retried at frame rate, and
    a repaint is not new information. artImages_ holds a null image for a url
    that failed, which is what stops the second request. */
void DashboardView::requestMissingArt()
{
    if (! onNeedArt) return;
    for (const auto& proj : payload_.projects)
    {
        if (proj.artKind == "procedural" || proj.artUrl.isEmpty()) continue;
        if (artImages_.count (proj.artUrl) || artRequested_.count (proj.artUrl)) continue;
        artRequested_.insert (proj.artUrl);
        onNeedArt (proj.artUrl);
    }
}

void DashboardView::setStatus (bool loading, const juce::String& error)
{
    loading_ = loading;
    error_   = error;
    repaint();
}

void DashboardView::setSignedOut (bool signedOut)
{
    signedOut_ = signedOut;
    layout (getWidth());
    repaint();
}

void DashboardView::setArtImage (const juce::String& url, const juce::Image& img)
{
    artImages_[url] = img;   // a null image is a recorded failure, not a retry
    repaint();
}

void DashboardView::addZone (juce::Rectangle<int> r, const DashLink& link)
{
    if (r.isEmpty() || link.isEmpty()) return;
    zones_.push_back ({ r, link, {} });
}

void DashboardView::addUrlZone (juce::Rectangle<int> r, const juce::String& url)
{
    if (r.isEmpty() || url.isEmpty()) return;
    zones_.push_back ({ r, {}, url });
}

// ---------------------------------------------------------------------------
//  layout: the SOLE geometry author
// ---------------------------------------------------------------------------

namespace
{
constexpr int kPad         = 20;
constexpr int kGap         = 14;
constexpr int kHeaderH     = 22;
constexpr int kUsageH      = 54;
constexpr int kContinueH   = 58;
constexpr int kHeadingH    = 16;
constexpr int kRowH        = 38;
constexpr int kRowGap      = 6;
constexpr int kTileCaption = 34;
constexpr int kStepH       = 24;
constexpr int kAnnounceH   = 66;
constexpr int kTwoColMin   = 720;   // content width at which the lists split
constexpr int kMaxRows     = 5;     // surface=plugin already caps at 5; belt to that brace
}

int DashboardView::layout (int width)
{
    // EVERY rect is cleared first and re-authored below. A card that is not
    // shown on this pass therefore cannot keep the coordinates it had on the
    // last one, which is the shape of three of the four overlap bugs this
    // file's rules come from.
    headerRect_ = statusRect_ = {};
    usageRect_ = usageBarRect_ = usageResetRect_ = upgradeChipRect_ = {};
    continueRect_ = {};
    projectsHeadingRect_ = projectsEmptyRect_ = {};
    chatsHeadingRect_ = chatsEmptyRect_ = {};
    chainsHeadingRect_ = chainsEmptyRect_ = {};
    onboardingRect_ = onboardingHeadingRect_ = {};
    announceRect_ = signedOutRect_ = {};
    projectTileRects_.clear();
    chatRowRects_.clear();
    chainRowRects_.clear();
    chainShareChipRects_.clear();
    onboardingStepRects_.clear();
    zones_.clear();
    hoverZone_ = -1;

    const int w = width - kPad * 2;
    if (w < 120) { contentH_ = 0; return 0; }

    const int x = kPad;
    int y = 16;

    // Header strip. ONE right anchor: the status line, authored here and
    // nowhere else, so nothing can be placed against the same edge from a
    // second block.
    {
        auto strip = juce::Rectangle<int> (x, y, w, kHeaderH);
        statusRect_ = strip.removeFromRight (juce::jmin (260, w / 2));
        headerRect_ = strip;
        y += kHeaderH + 10;
    }

    if (signedOut_)
    {
        signedOutRect_ = { x, y, w, 44 };
        contentH_ = y + 44 + kPad;
        return contentH_;
    }

    if (! payload_.valid)
    {
        // Nothing to lay out yet. The status line above already says whether
        // we are loading, failed, or simply have not asked.
        contentH_ = y + kPad;
        return contentH_;
    }

    // ---- usage ------------------------------------------------------------
    {
        usageRect_ = { x, y, w, kUsageH };
        auto inner = usageRect_.reduced (14, 0);

        // The one right-anchored group on this strip, in one place: the
        // upgrade chip (Free only) then the reset date, then whatever is left
        // is the bar. Two blocks anchoring to the same edge is what put `Aa`
        // on top of the CHAINS switch.
        const bool showUpgrade = payload_.usage.upgradeUrl.isNotEmpty()
                              && payload_.usage.tierLabel.equalsIgnoreCase ("Free");
        if (showUpgrade)
        {
            upgradeChipRect_ = inner.removeFromRight (74).withSizeKeepingCentre (74, 22);
            inner.removeFromRight (10);
        }
        usageResetRect_ = inner.removeFromRight (juce::jmin (150, inner.getWidth() / 3));
        usageBarRect_   = { inner.getX(), usageRect_.getY() + 34, juce::jmax (10, inner.getWidth()), 6 };

        addUrlZone (upgradeChipRect_, dashSiteRoot() + "/upgrade");
        y += kUsageH + kGap;
    }

    // ---- continue ---------------------------------------------------------
    if (payload_.continueCard.present)
    {
        continueRect_ = { x, y, w, kContinueH };
        addZone (continueRect_, payload_.continueCard.target);
        y += kContinueH + kGap;
    }

    // ---- projects ---------------------------------------------------------
    {
        projectsHeadingRect_ = { x, y, w, kHeadingH };
        y += kHeadingH + 6;

        const int n = juce::jmin (kMaxRows, (int) payload_.projects.size());
        if (n == 0)
        {
            projectsEmptyRect_ = { x, y, w, 18 };
            y += 18 + kGap;
        }
        else
        {
            const int tileGap = 12;
            const int edge = juce::jlimit (56, 112, (w - tileGap * (n - 1)) / n);
            for (int i = 0; i < n; ++i)
                projectTileRects_.push_back ({ x + i * (edge + tileGap), y, edge, edge });
            y += edge + kTileCaption + kGap;
        }
    }

    // ---- recent chats and chains ------------------------------------------
    //
    // Side by side when there is room, stacked when there is not. Both
    // branches are authored here, in one block, so the columns cannot end up
    // measured against two different widths.
    {
        const bool twoCol = w >= kTwoColMin;
        const int colGap  = 20;
        const int colW    = twoCol ? (w - colGap) / 2 : w;
        const int leftX   = x;
        const int rightX  = twoCol ? x + colW + colGap : x;

        int yL = y;
        chatsHeadingRect_ = { leftX, yL, colW, kHeadingH };
        yL += kHeadingH + 6;
        const int nChats = juce::jmin (kMaxRows, (int) payload_.recentChats.size());
        if (nChats == 0)
        {
            chatsEmptyRect_ = { leftX, yL, colW, 18 };
            yL += 18;
        }
        else
        {
            for (int i = 0; i < nChats; ++i)
            {
                juce::Rectangle<int> r { leftX, yL, colW, kRowH };
                chatRowRects_.push_back (r);
                DashLink l; l.surface = "chat"; l.id = payload_.recentChats[(size_t) i].id;
                addZone (r, l);
                yL += kRowH + kRowGap;
            }
            yL -= kRowGap;
        }

        int yR = twoCol ? y : yL + kGap;
        chainsHeadingRect_ = { rightX, yR, colW, kHeadingH };
        yR += kHeadingH + 6;
        const int nChains = juce::jmin (kMaxRows, (int) payload_.chains.size());
        if (nChains == 0)
        {
            chainsEmptyRect_ = { rightX, yR, colW, 18 };
            yR += 18;
        }
        else
        {
            for (int i = 0; i < nChains; ++i)
            {
                const auto& c = payload_.chains[(size_t) i];
                juce::Rectangle<int> r { rightX, yR, colW, kRowH };
                chainRowRects_.push_back (r);

                // The share chip is the ONLY thing on this surface that opens
                // a browser to a content page, because /c/:slug is the one
                // page that works with no auth at all.
                juce::Rectangle<int> chip;
                if (c.shareSlug.isNotEmpty())
                    chip = r.reduced (8, 9).removeFromRight (58);
                chainShareChipRects_.push_back (chip);

                // Zone order matters: the chip is added FIRST so a click
                // inside it resolves to the chip, not to the row underneath.
                addUrlZone (chip, dashSiteRoot() + "/c/" + c.shareSlug);
                DashLink l; l.surface = "chain"; l.id = c.id;
                addZone (r, l);
                yR += kRowH + kRowGap;
            }
            yR -= kRowGap;
        }

        y = juce::jmax (yL, yR) + kGap;
    }

    // ---- onboarding, only while incomplete --------------------------------
    if (! payload_.onboardingComplete && ! payload_.onboardingSteps.empty())
    {
        const int n = (int) payload_.onboardingSteps.size();
        const int h = 30 + n * kStepH + 10;
        onboardingRect_        = { x, y, w, h };
        onboardingHeadingRect_ = { x + 14, y + 8, w - 28, kHeadingH };
        for (int i = 0; i < n; ++i)
        {
            juce::Rectangle<int> r { x + 14, y + 30 + i * kStepH, w - 28, kStepH - 2 };
            onboardingStepRects_.push_back (r);

            // A step whose target is the web is TEXT, never a zone. There is
            // no plugin-to-web token path, so "Claim your handle" would land
            // the user on a login screen. A button that promises a
            // destination it cannot reach is worse than a line of text.
            const auto& s = payload_.onboardingSteps[(size_t) i];
            if (! s.done && s.target.surface != "web")
                addZone (r, s.target);
        }
        y += h + kGap;
    }

    // ---- latest announcement ----------------------------------------------
    //
    // Read only and deliberately not clickable: the community surface has no
    // public URL, so there is nowhere for a "read more" to go.
    if (payload_.announcement.present)
    {
        announceRect_ = { x, y, w, kAnnounceH };
        y += kAnnounceH + kGap;
    }

    contentH_ = y + 6;
    return contentH_;
}

void DashboardView::resized()
{
    layout (getWidth());
}

// ---------------------------------------------------------------------------
//  paint: consumes the stored rects and MEASURES NOTHING
// ---------------------------------------------------------------------------

namespace
{
void card (juce::Graphics& g, juce::Rectangle<int> r, bool hot = false)
{
    if (r.isEmpty()) return;
    g.setColour (C::bg3);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);
    g.setColour (hot ? juce::Colour (0xff22d3ee).withAlpha (0.35f) : C::border2);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.0f);
}

void heading (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    if (r.isEmpty()) return;
    g.setColour (C::text3);
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText (text, r, juce::Justification::centredLeft);
}

void muted (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    if (r.isEmpty()) return;
    g.setColour (C::text3);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (text, r, juce::Justification::centredLeft, true);
}
}

juce::String DashboardView::statusLine() const
{
    if (signedOut_) return {};
    if (loading_ && ! payload_.valid)
        return juce::String::fromUTF8 ("Loading your dashboard\xe2\x80\xa6");
    if (error_.isNotEmpty() && ! payload_.valid)
        return error_;
    if (fetchedAtMs_ > 0)
        return "Last updated "
             + juce::Time (fetchedAtMs_).toString (false, true, false, true);
    return {};
}

void DashboardView::drawTile (juce::Graphics& g, const DashProject& p,
                              juce::Rectangle<int> square)
{
    const int edge = square.getWidth();

    // An uploaded image draws INSTEAD of the procedural art, cover cropped so
    // an album cover is never letterboxed or squashed. Until it arrives (and
    // forever, if the fetch failed) the seed art draws, which is why art.seed
    // is sent for uploads too.
    bool drewUpload = false;
    if (p.artKind != "procedural" && p.artUrl.isNotEmpty())
    {
        auto it = artImages_.find (p.artUrl);
        if (it != artImages_.end() && it->second.isValid())
        {
            juce::Path clip;
            clip.addRoundedRectangle (square.toFloat(), 10.0f);
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (clip);
            g.drawImage (it->second, square.toFloat(),
                         juce::RectanglePlacement (juce::RectanglePlacement::centred
                                                 | juce::RectanglePlacement::fillDestination));
            drewUpload = true;
        }
    }

    if (! drewUpload)
    {
        const auto img = ProjectArt::getCached (p.artSeed, edge);
        if (img.isValid()) g.drawImageAt (img, square.getX(), square.getY());
    }

    g.setColour (C::border2);
    g.drawRoundedRectangle (square.toFloat().reduced (0.5f), 10.0f, 1.0f);

    // Caption block, authored by layout as the fixed strip below the square.
    g.setColour (C::text);
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    g.drawText (p.name, square.getX(), square.getBottom() + 6, edge, 15,
                juce::Justification::topLeft, true);

    juce::String counts;
    counts << p.chats << " chat" << (p.chats == 1 ? "" : "s");
    if (p.chains > 0)
        counts << juce::String::fromUTF8 ("  \xc2\xb7  ") << p.chains
               << " chain" << (p.chains == 1 ? "" : "s");
    g.setColour (C::text3);
    g.setFont (juce::Font (juce::FontOptions (9.5f)));
    g.drawText (counts, square.getX(), square.getBottom() + 21, edge, 12,
                juce::Justification::topLeft, true);
}

void DashboardView::paint (juce::Graphics& g)
{
    g.fillAll (C::bg);

    // Header. The left half is identity, the right half is the status line
    // layout anchored; neither measures anything.
    if (! headerRect_.isEmpty())
    {
        juce::String who = payload_.displayName;
        if (who.isEmpty() && payload_.handle.isNotEmpty()) who = "@" + payload_.handle;
        if (who.isEmpty()) who = "Your studio";
        g.setColour (C::text);
        g.setFont (juce::Font (juce::FontOptions (15.0f)));
        g.drawText (who, headerRect_, juce::Justification::centredLeft, true);
    }
    if (! statusRect_.isEmpty())
    {
        const auto line = statusLine();
        if (line.isNotEmpty())
        {
            // Amber when a refresh failed but cached content is still on
            // screen: the content is the user's best available answer and the
            // timestamp says how old it is. An error never REPLACES content.
            g.setColour (error_.isNotEmpty() ? juce::Colour (0xffE8C070) : C::text3);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (line, statusRect_, juce::Justification::centredRight, true);
        }
    }

    if (! signedOutRect_.isEmpty())
    {
        g.setColour (C::text2);
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText ("Sign in to see your projects, chats and chains here.",
                    signedOutRect_, juce::Justification::topLeft, true);
        return;
    }

    if (! payload_.valid)
        return;

    const auto hotRect = (hoverZone_ >= 0 && hoverZone_ < (int) zones_.size())
                       ? zones_[(size_t) hoverZone_].rect : juce::Rectangle<int>();

    // ---- usage ------------------------------------------------------------
    if (! usageRect_.isEmpty())
    {
        card (g, usageRect_);
        const auto& u = payload_.usage;

        g.setColour (C::text2);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (u.tierLabel.isNotEmpty() ? u.tierLabel : juce::String ("Plan"),
                    usageRect_.getX() + 14, usageRect_.getY() + 8, 140, 16,
                    juce::Justification::centredLeft);

        juce::String amount;
        if (u.unitsTotal > 0)
            amount << u.unitsUsed << " of " << u.unitsTotal << "  " << u.pct << "%";
        else
            amount << u.unitsUsed << " used";
        g.setColour (C::text3);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (amount, usageRect_.getX() + 160, usageRect_.getY() + 8,
                    juce::jmax (10, usageBarRect_.getRight() - usageRect_.getX() - 160), 16,
                    juce::Justification::centredLeft, true);

        g.setColour (C::bg4);
        g.fillRoundedRectangle (usageBarRect_.toFloat(), 3.0f);
        const int fillW = usageBarRect_.getWidth() * u.pct / 100;
        if (fillW > 0)
        {
            g.setColour (u.pct >= 90 ? juce::Colour (0xffff6d5a) : C::blue2);
            g.fillRoundedRectangle (usageBarRect_.withWidth (juce::jmax (3, fillW)).toFloat(), 3.0f);
        }

        const auto reset = shortDate (u.resetsAt);
        if (reset.isNotEmpty())
        {
            g.setColour (C::text3);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText ("Resets " + reset, usageResetRect_,
                        juce::Justification::centredRight, true);
        }

        if (! upgradeChipRect_.isEmpty())
        {
            const bool hot = upgradeChipRect_ == hotRect;
            g.setColour (juce::Colour (0xff0d2b33));
            g.fillRoundedRectangle (upgradeChipRect_.toFloat(), 5.0f);
            g.setColour (juce::Colour (0xff22d3ee).withAlpha (hot ? 0.7f : 0.35f));
            g.drawRoundedRectangle (upgradeChipRect_.toFloat().reduced (0.5f), 5.0f, 1.0f);
            g.setColour (C::blue2);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText ("Upgrade", upgradeChipRect_, juce::Justification::centred);
        }
    }

    // ---- continue ---------------------------------------------------------
    if (! continueRect_.isEmpty())
    {
        card (g, continueRect_, continueRect_ == hotRect);
        heading (g, { continueRect_.getX() + 14, continueRect_.getY() + 9,
                      continueRect_.getWidth() - 28, kHeadingH },
                 "PICK UP WHERE YOU LEFT OFF");
        g.setColour (C::text);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (payload_.continueCard.label,
                    continueRect_.getX() + 14, continueRect_.getY() + 28,
                    continueRect_.getWidth() - 28, 20,
                    juce::Justification::centredLeft, true);
    }

    // ---- projects ---------------------------------------------------------
    heading (g, projectsHeadingRect_, "PROJECTS");
    muted (g, projectsEmptyRect_,
           "No projects yet. Name one in the header and it appears here.");
    for (int i = 0; i < (int) projectTileRects_.size(); ++i)
        drawTile (g, payload_.projects[(size_t) i], projectTileRects_[(size_t) i]);

    // ---- recent chats -----------------------------------------------------
    heading (g, chatsHeadingRect_, "RECENT CHATS");
    muted (g, chatsEmptyRect_, "No chats yet.");
    for (int i = 0; i < (int) chatRowRects_.size(); ++i)
    {
        const auto r = chatRowRects_[(size_t) i];
        const auto& c = payload_.recentChats[(size_t) i];
        card (g, r, r == hotRect);
        g.setColour (C::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (c.title.isNotEmpty() ? c.title : juce::String ("Untitled chat"),
                    r.getX() + 10, r.getY() + 5, r.getWidth() - 20, 14,
                    juce::Justification::centredLeft, true);
        g.setColour (C::text3);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (c.snippet, r.getX() + 10, r.getY() + 20, r.getWidth() - 20, 13,
                    juce::Justification::centredLeft, true);
    }

    // ---- chains -----------------------------------------------------------
    heading (g, chainsHeadingRect_, "CHAINS");
    muted (g, chainsEmptyRect_, "No saved chains yet.");
    for (int i = 0; i < (int) chainRowRects_.size(); ++i)
    {
        const auto r = chainRowRects_[(size_t) i];
        const auto chip = chainShareChipRects_[(size_t) i];
        const auto& c = payload_.chains[(size_t) i];
        card (g, r, r == hotRect);

        const int textW = juce::jmax (20, (chip.isEmpty() ? r.getRight() : chip.getX())
                                           - r.getX() - 20);
        g.setColour (C::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (c.name, r.getX() + 10, r.getY() + 5, textW, 14,
                    juce::Justification::centredLeft, true);

        juce::String sub;
        sub << c.slotCount << " slot" << (c.slotCount == 1 ? "" : "s");
        if (c.qualityScore >= 0)
            sub << juce::String::fromUTF8 ("  \xc2\xb7  ") << c.qualityScore;
        if (c.source == "import")
            sub << juce::String::fromUTF8 ("  \xc2\xb7  imported");
        g.setColour (C::text3);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (sub, r.getX() + 10, r.getY() + 20, textW, 13,
                    juce::Justification::centredLeft, true);

        if (! chip.isEmpty())
        {
            const bool hot = chip == hotRect;
            g.setColour (juce::Colour (0xff22d3ee).withAlpha (hot ? 0.20f : 0.10f));
            g.fillRoundedRectangle (chip.toFloat(), 5.0f);
            g.setColour (C::blue2);
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.drawText ("SHARED", chip, juce::Justification::centred);
        }
    }

    // ---- onboarding -------------------------------------------------------
    if (! onboardingRect_.isEmpty())
    {
        card (g, onboardingRect_);
        heading (g, onboardingHeadingRect_, "GETTING STARTED");
        for (int i = 0; i < (int) onboardingStepRects_.size(); ++i)
        {
            const auto r = onboardingStepRects_[(size_t) i];
            const auto& s = payload_.onboardingSteps[(size_t) i];
            const bool webOnly = (! s.done && s.target.surface == "web");

            auto box = juce::Rectangle<float> ((float) r.getX(),
                                               (float) (r.getY() + 5), 12.0f, 12.0f);
            if (s.done)
            {
                g.setColour (C::green);
                g.fillEllipse (box);
                g.setColour (C::bg);
                g.drawLine (box.getX() + 3.0f, box.getCentreY(),
                            box.getCentreX() - 0.5f, box.getBottom() - 3.5f, 1.6f);
                g.drawLine (box.getCentreX() - 0.5f, box.getBottom() - 3.5f,
                            box.getRight() - 3.0f, box.getY() + 3.5f, 1.6f);
            }
            else
            {
                g.setColour (C::text3);
                g.drawEllipse (box.reduced (0.5f), 1.0f);
            }

            const bool hot = (r == hotRect);
            g.setColour (s.done ? C::text3 : (hot ? C::blue2 : C::text2));
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText (s.label, r.getX() + 20, r.getY(), r.getWidth() - 20, r.getHeight(),
                        juce::Justification::centredLeft, true);

            // The honest statement of what the plugin cannot do. It is a line
            // of text, not a disabled button, because there is nothing here to
            // press: the settings page needs a signed in browser session and
            // the plugin cannot hand one over.
            if (webOnly)
            {
                g.setColour (C::text3);
                g.setFont (juce::Font (juce::FontOptions (9.5f)));
                g.drawText ("in the web app", r.getX() + 20, r.getY(),
                            r.getWidth() - 20, r.getHeight(),
                            juce::Justification::centredRight, true);
            }
        }
    }

    // ---- announcement -----------------------------------------------------
    if (! announceRect_.isEmpty())
    {
        card (g, announceRect_);
        auto head = juce::Rectangle<int> (announceRect_.getX() + 14,
                                          announceRect_.getY() + 9,
                                          announceRect_.getWidth() - 28, kHeadingH);
        heading (g, head, "LATEST FROM ECHOJAY");
        const auto when = shortDate (payload_.announcement.postedAt);
        if (when.isNotEmpty())
        {
            g.setColour (C::text3);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawText (when, head, juce::Justification::centredRight, true);
        }
        g.setColour (C::text2);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawFittedText (payload_.announcement.snippet,
                          announceRect_.getX() + 14, announceRect_.getY() + 28,
                          announceRect_.getWidth() - 28, 30,
                          juce::Justification::topLeft, 2, 1.0f);
    }
}

// ---------------------------------------------------------------------------
//  input: consumes the SAME rects paint drew
// ---------------------------------------------------------------------------

/** THE hit test, and the only one. FIRST match wins, which is why layout()
    adds a chain row's share chip BEFORE the row it sits inside. Both mouse
    handlers and tools/dashboard_test go through here, so a click cannot
    resolve differently from a hover or from what the test proved. */
int DashboardView::zoneIndexAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) zones_.size(); ++i)
        if (zones_[(size_t) i].rect.contains (p))
            return i;
    return -1;
}

void DashboardView::mouseDown (const juce::MouseEvent& e)
{
    const int i = zoneIndexAt (e.getPosition());
    if (i < 0) return;
    const auto& z = zones_[(size_t) i];
    if (z.url.isNotEmpty())
    {
        if (onOpenUrl) onOpenUrl (z.url);
    }
    else if (onNavigate)
    {
        onNavigate (z.link);
    }
}

void DashboardView::mouseMove (const juce::MouseEvent& e)
{
    const int hit = zoneIndexAt (e.getPosition());

    if (hit != hoverZone_)
    {
        hoverZone_ = hit;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void DashboardView::mouseExit (const juce::MouseEvent&)
{
    if (hoverZone_ != -1)
    {
        hoverZone_ = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

} // namespace echojay
