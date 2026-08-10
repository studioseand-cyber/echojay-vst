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

/** V0: "just now" / "12m ago" / "3h ago" / "5d ago". SAME thresholds as the
    web renderer's relTime (public/js/dashboard.js) so the two surfaces never
    disagree about how old a row is. Empty when the stamp is unusable. */
juce::String relTime (const juce::String& iso)
{
    if (iso.isEmpty()) return {};
    const auto t = juce::Time::fromISO8601 (iso);
    if (t.toMilliseconds() <= 0) return {};
    const auto s = juce::jmax ((juce::int64) 0,
        (juce::Time::currentTimeMillis() - t.toMilliseconds()) / 1000);
    if (s < 90)     return "just now";
    if (s < 5400)   return juce::String ((s + 30) / 60) + "m ago";
    if (s < 129600) return juce::String ((s + 1800) / 3600) + "h ago";
    return juce::String ((s + 43200) / 86400) + "d ago";
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
            c.id        = str (v, "id");
            c.title     = str (v, "title");
            c.snippet   = str (v, "snippet");
            c.updatedAt = str (v, "updatedAt");
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
            c.summary         = str (v, "summary");
            c.notes           = str (v, "notes");
            c.artSeed         = str (v, "artSeed");
            c.updatedAt       = str (v, "updatedAt");
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

void DashboardView::setDirectUnread (int n)
{
    if (n == directUnread_) return;      // no relayout for an unchanged count
    directUnread_ = n;
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
    zones_.push_back ({ r, link, {}, {} });
}

void DashboardView::addUrlZone (juce::Rectangle<int> r, const juce::String& url)
{
    if (r.isEmpty() || url.isEmpty()) return;
    zones_.push_back ({ r, {}, url, {} });
}

void DashboardView::addHandoffZone (juce::Rectangle<int> r, const juce::String& path)
{
    if (r.isEmpty() || path.isEmpty()) return;
    zones_.push_back ({ r, {}, {}, path });
}

// ---------------------------------------------------------------------------
//  layout: the SOLE geometry author
// ---------------------------------------------------------------------------

namespace
{
constexpr int kPad         = 20;
constexpr int kGap         = 14;
constexpr int kHeaderH     = 22;
constexpr int kContinueH   = 58;
constexpr int kHeadingH    = 16;
constexpr int kRowH        = 38;
constexpr int kRowGap      = 6;
constexpr int kTileCaption = 34;
constexpr int kStepH       = 24;
constexpr int kAnnounceH   = 66;
constexpr int kDirectH     = 44;
constexpr int kTwoColMin   = 720;   // content width at which the lists split
constexpr int kMaxRows     = 5;     // surface=plugin already caps at 5; belt to that brace

// V0 projects rail: fixed tile edge (the tiles no longer shrink to fit),
// horizontal scroll takes the strain instead. kStackRoom is the headroom the
// decorative stack layers protrude into above each tile.
constexpr int kTileEdge    = 128;
constexpr int kTileGap     = 14;
constexpr int kStackRoom   = 10;

// V0 chain rows: taller than a chat row because they carry an art thumb and
// up to three lines. The notes line adds height only when there are notes.
constexpr int kChainRowH      = 50;
constexpr int kChainRowNotesH = 64;
}

int DashboardView::layout (int width)
{
    // EVERY rect is cleared first and re-authored below. A card that is not
    // shown on this pass therefore cannot keep the coordinates it had on the
    // last one, which is the shape of three of the four overlap bugs this
    // file's rules come from.
    headerRect_ = statusRect_ = {};
    continueRect_ = {};
    projectsHeadingRect_ = projectsEmptyRect_ = projectsClipRect_ = {};
    chatsHeadingRect_ = chatsEmptyRect_ = {};
    chainsHeadingRect_ = chainsEmptyRect_ = {};
    onboardingRect_ = onboardingHeadingRect_ = {};
    announceRect_ = directRect_ = signedOutRect_ = {};
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

    // The usage strip that was authored here is GONE (V0, 8 Aug 2026):
    // Settings is the usage surface. The parse keeps reading the block
    // because its presence is what validates a dashboard document.

    // ---- continue ---------------------------------------------------------
    if (payload_.continueCard.present)
    {
        continueRect_ = { x, y, w, kContinueH };
        addZone (continueRect_, payload_.continueCard.target);
        y += kContinueH + kGap;
    }

    // ---- projects: a horizontal rail of stacked cards (V0) ----------------
    {
        projectsHeadingRect_ = { x, y, w, kHeadingH };
        y += kHeadingH + 6;

        const int n = juce::jmin (kMaxRows, (int) payload_.projects.size());
        if (n == 0)
        {
            projectsEmptyRect_ = { x, y, w, 18 };
            railMaxScroll_ = railScrollX_ = 0;
            y += 18 + kGap;
        }
        else
        {
            // Fixed edge, scrolled sideways when the rail outgrows the width.
            // The offset is re-clamped on EVERY pass so a resize cannot leave
            // the rail scrolled past its own end, and the rects are authored
            // at their scrolled positions so paint and any future hit test
            // stay in agreement (the geometry rules at the top of the file).
            const int railW = n * (kTileEdge + kTileGap) - kTileGap;
            railMaxScroll_  = juce::jmax (0, railW - w);
            railScrollX_    = juce::jlimit (0, railMaxScroll_, railScrollX_);

            const int stripH = kStackRoom + kTileEdge + kTileCaption;
            projectsClipRect_ = { x, y, w, stripH };
            for (int i = 0; i < n; ++i)
                projectTileRects_.push_back ({ x + i * (kTileEdge + kTileGap) - railScrollX_,
                                               y + kStackRoom, kTileEdge, kTileEdge });
            y += stripH + kGap;
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
                // V0 rows are taller than chat rows: art thumb plus name,
                // notes (when present) and the derived summary. Height is
                // authored HERE, per row, so paint measures nothing.
                const int rh = c.notes.isNotEmpty() ? kChainRowNotesH : kChainRowH;
                juce::Rectangle<int> r { rightX, yR, colW, rh };
                chainRowRects_.push_back (r);

                // The share chip is the ONLY thing on this surface that opens
                // a browser to a content page, because /c/:slug is the one
                // page that works with no auth at all.
                juce::Rectangle<int> chip;
                if (c.shareSlug.isNotEmpty())
                    chip = juce::Rectangle<int> (r.getRight() - 8 - 58,
                                                 r.getY() + 8, 58, 20);
                chainShareChipRects_.push_back (chip);

                // Zone order matters: the chip is added FIRST so a click
                // inside it resolves to the chip, not to the row underneath.
                addUrlZone (chip, dashSiteRoot() + "/c/" + c.shareSlug);
                DashLink l; l.surface = "chain"; l.id = c.id;
                addZone (r, l);
                yR += rh + kRowGap;
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

    // ---- direct messages ---------------------------------------------------
    //
    // M2.5. The plugin cannot SHOW a DM: there is no thread surface here and
    // the thin-home rule says there should not be. What it can do is tell you
    // one arrived and take you somewhere that can show it.
    //
    // WITHOUT THIS the dot on the Dashboard tab label lights (total includes
    // direct, decided server side in M1) and leads to a tab with nothing about
    // messages on it. That is the "wrong badge on the wrong surface" error the
    // imports note in DashPoll.h exists to avoid, arriving from the other
    // direction. A badge that goes nowhere and a hidden message are both worse
    // than one line of text with a destination.
    if (directUnread_ > 0)
    {
        directRect_ = { x, y, w, kDirectH };
        // NOT addUrlZone: that launches a URL directly and is only ever given
        // PUBLIC pages. This needs a minted token first, so it goes through
        // the handoff zone instead. See the zones note below.
        addHandoffZone (directRect_, "/app?overlay=community");
        y += kDirectH + kGap;
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

    // THE COVER IS RESOLVED FIRST, because the stack behind the tile has to
    // show the same thing the tile does. Layers tinted from the seed sitting
    // behind an uploaded photo read as a rendering fault rather than as a
    // stack, so this lookup cannot happen after the stack is drawn.
    const juce::Image* cover = nullptr;
    if (p.artKind != "procedural" && p.artUrl.isNotEmpty())
    {
        auto it = artImages_.find (p.artUrl);
        if (it != artImages_.end() && it->second.isValid()) cover = &it->second;
    }

    // V0 stack: two edges peeking above the ONE tile, showing WHAT THE TILE
    // SHOWS. With a cover, each layer draws that image as though it were a
    // full tile anchored at the layer's own top, so what protrudes is the top
    // slice of the cover, which is what a card behind this one would show.
    // Without one, they are flat tints from the seed's hue anchor. Still one
    // artwork per project either way: this is an offset of the same image,
    // never a second one. derive() returns a stable fallback for a malformed
    // seed, so this cannot take down a paint pass.
    {
        const auto pr = ProjectArt::derive (p.artSeed);
        const auto backT  = ProjectArt::hslToColour ((float) pr.hueAnchor, 0.35f, 0.21f);
        const auto frontT = ProjectArt::hslToColour ((float) pr.hueAnchor, 0.40f, 0.30f);

        auto layer = [&] (juce::Rectangle<int> strip, float radius,
                          juce::Colour tint, float dim)
        {
            if (cover == nullptr)
            {
                g.setColour (tint);
                g.fillRoundedRectangle (strip.toFloat(), radius);
                return;
            }
            juce::Path clip;
            clip.addRoundedRectangle (strip.toFloat(), radius);
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (clip);
            // Full-tile geometry anchored at this strip's top edge: the clip
            // is what turns it into a sliver, so the slice matches the tile.
            g.drawImage (*cover,
                         juce::Rectangle<int> (strip.getX(), strip.getY(),
                                               strip.getWidth(), edge).toFloat(),
                         juce::RectanglePlacement (juce::RectanglePlacement::centred
                                                 | juce::RectanglePlacement::fillDestination));
            if (dim > 0.0f)
            {
                g.setColour (juce::Colours::black.withAlpha (dim));
                g.fillRect (strip);
            }
        };

        // Back layer first, then front over it. The back one is darkened so
        // depth survives whether the layer is a photo or a tint.
        layer ({ square.getX() + 16, square.getY() - 9, edge - 32, 14 }, 6.0f, backT, 0.30f);
        layer ({ square.getX() + 8,  square.getY() - 5, edge - 16, 14 }, 7.0f, frontT, 0.0f);
    }

    // An uploaded image draws INSTEAD of the procedural art, cover cropped so
    // an album cover is never letterboxed or squashed. Until it arrives (and
    // forever, if the fetch failed) the seed art draws, which is why art.seed
    // is sent for uploads too.
    if (cover != nullptr)
    {
        juce::Path clip;
        clip.addRoundedRectangle (square.toFloat(), 10.0f);
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (clip);
        g.drawImage (*cover, square.toFloat(),
                     juce::RectanglePlacement (juce::RectanglePlacement::centred
                                             | juce::RectanglePlacement::fillDestination));
    }
    else
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

    // All three counts, nonzero only: canonical caption (spec section 15),
    // matching the web's bits.join behaviour exactly, including the wording
    // when there is nothing to count.
    juce::String counts;
    const auto dot = juce::String::fromUTF8 ("  \xc2\xb7  ");
    if (p.chats > 0)
        counts << p.chats << " chat" << (p.chats == 1 ? "" : "s");
    if (p.captures > 0)
        counts << (counts.isEmpty() ? "" : dot) << p.captures
               << " capture" << (p.captures == 1 ? "" : "s");
    if (p.chains > 0)
        counts << (counts.isEmpty() ? "" : dot) << p.chains
               << " chain" << (p.chains == 1 ? "" : "s");
    // "Nothing yet", not "empty": a bare word sitting where "19 chats · 3
    // chains" sits on every neighbour reads as a missing value rather than a
    // state. Not "No chats yet" either, since this appears only when chats
    // AND captures AND chains are all zero.
    if (counts.isEmpty()) counts = "Nothing yet";
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

    // The usage strip is gone (V0): Settings is the usage surface.

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
    if (! projectTileRects_.empty())
    {
        // The rail clips to its strip so a scrolled tile can never bleed
        // into the page padding or the section above (the stack layers
        // protrude upward INSIDE the strip's headroom, not outside it).
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (projectsClipRect_);
        for (int i = 0; i < (int) projectTileRects_.size(); ++i)
            drawTile (g, payload_.projects[(size_t) i], projectTileRects_[(size_t) i]);
    }

    // ---- recent chats -----------------------------------------------------
    heading (g, chatsHeadingRect_, "RECENT CHATS");
    muted (g, chatsEmptyRect_, "No chats yet.");
    for (int i = 0; i < (int) chatRowRects_.size(); ++i)
    {
        const auto r = chatRowRects_[(size_t) i];
        const auto& c = payload_.recentChats[(size_t) i];
        card (g, r, r == hotRect);

        // The relative timestamp is canonical on rows (spec section 15); the
        // title gives up the width the stamp needs and no more.
        const auto when = relTime (c.updatedAt);
        const int whenW = when.isEmpty() ? 0 : 52;
        if (whenW > 0)
        {
            g.setColour (C::text3);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawText (when, r.getRight() - 10 - whenW, r.getY() + 5, whenW, 14,
                        juce::Justification::centredRight, true);
        }
        g.setColour (C::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (c.title.isNotEmpty() ? c.title : juce::String ("Untitled chat"),
                    r.getX() + 10, r.getY() + 5, r.getWidth() - 20 - whenW, 14,
                    juce::Justification::centredLeft, true);
        g.setColour (C::text3);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (c.snippet, r.getX() + 10, r.getY() + 20, r.getWidth() - 20, 13,
                    juce::Justification::centredLeft, true);
    }

    // ---- chains (V0 rows) -------------------------------------------------
    //
    // Art thumb left (SAME ProjectArt pipeline, seeded from the chain id),
    // name with the relative timestamp, the owner's share notes when present,
    // then the server-derived summary: the plugin list is what an engineer
    // actually reads. A pre-V0 payload has none of the new fields and falls
    // back to the old "N slots · score" sub-line with no thumb.
    heading (g, chainsHeadingRect_, "CHAINS");
    muted (g, chainsEmptyRect_, "No saved chains yet.");
    for (int i = 0; i < (int) chainRowRects_.size(); ++i)
    {
        const auto r = chainRowRects_[(size_t) i];
        const auto chip = chainShareChipRects_[(size_t) i];
        const auto& c = payload_.chains[(size_t) i];
        card (g, r, r == hotRect);

        int textX = r.getX() + 10;
        if (c.artSeed.isNotEmpty())
        {
            const auto img = ProjectArt::getCached (c.artSeed, 34);
            if (img.isValid())
                g.drawImageAt (img, r.getX() + 8, r.getY() + 8);
            textX = r.getX() + 8 + 34 + 10;
        }

        const int textRight = chip.isEmpty() ? r.getRight() - 10 : chip.getX() - 8;
        const int textW = juce::jmax (20, textRight - textX);

        const auto when = relTime (c.updatedAt);
        const int whenW = when.isEmpty() ? 0 : 52;
        if (whenW > 0)
        {
            g.setColour (C::text3);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawText (when, textRight - whenW, r.getY() + 7, whenW, 14,
                        juce::Justification::centredRight, true);
        }
        g.setColour (C::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (c.name, textX, r.getY() + 7, textW - whenW, 14,
                    juce::Justification::centredLeft, true);

        int lineY = r.getY() + 23;
        if (c.notes.isNotEmpty())
        {
            g.setColour (C::text2);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (c.notes, textX, lineY, textW, 13,
                        juce::Justification::centredLeft, true);
            lineY += 14;
        }

        juce::String sub = c.summary;
        if (sub.isEmpty())
        {
            // Pre-V0 payload: the old sub-line, so an outdated server never
            // renders an empty row.
            sub << c.slotCount << " slot" << (c.slotCount == 1 ? "" : "s");
            if (c.qualityScore >= 0)
                sub << juce::String::fromUTF8 ("  \xc2\xb7  ") << c.qualityScore;
        }
        if (c.source == "import")
            sub << juce::String::fromUTF8 ("  \xc2\xb7  imported");
        g.setColour (C::text3);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (sub, textX, lineY, textW, 13,
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

    // ---- direct messages ---------------------------------------------------
    if (! directRect_.isEmpty())
    {
        card (g, directRect_, directRect_ == hotRect);
        heading (g, { directRect_.getX() + 14, directRect_.getY() + 8,
                      directRect_.getWidth() - 28, kHeadingH }, "MESSAGES");

        // SINGULAR AND PLURAL, because "1 new messages" survives to production
        // and reads as unfinished. The count is capped in the wording at 9+
        // for the same reason the badge is: an exact 47 is not more useful
        // than "lots" and it makes the line wrap.
        juce::String line;
        if (directUnread_ == 1) line = "1 new message";
        else if (directUnread_ > 9) line = "9+ new messages";
        else line << directUnread_ << " new messages";
        line << ".  Open in your browser";

        g.setColour (C::text2);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (line, directRect_.getX() + 14, directRect_.getY() + 24,
                    directRect_.getWidth() - 28, 16,
                    juce::Justification::centredLeft, true);
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
    else if (z.handoffPath.isNotEmpty())
    {
        if (onOpenWithHandoff) onOpenWithHandoff (z.handoffPath);
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

void DashboardView::mouseWheelMove (const juce::MouseEvent& e,
                                    const juce::MouseWheelDetails& wheel)
{
    // Horizontal intent over the rail scrolls the rail: a sideways wheel or
    // trackpad swipe, or shift plus a vertical wheel (the convention every
    // DAW horizontal scroller follows). Everything else, including a plain
    // vertical wheel over the rail, passes to the owning Viewport so the
    // page keeps scrolling; a rail that eats vertical scroll is a wall.
    if (railMaxScroll_ > 0 && projectsClipRect_.contains (e.getPosition()))
    {
        float d = wheel.deltaX;
        if (d == 0.0f && e.mods.isShiftDown()) d = wheel.deltaY;
        if (d != 0.0f)
        {
            const int step = (int) (-d * 240.0f);
            const int next = juce::jlimit (0, railMaxScroll_, railScrollX_ + step);
            if (next != railScrollX_)
            {
                railScrollX_ = next;
                layout (getWidth());
                repaint();
            }
            return;
        }
    }
    juce::Component::mouseWheelMove (e, wheel);
}

} // namespace echojay
