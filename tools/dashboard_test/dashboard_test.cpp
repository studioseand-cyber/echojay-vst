/*
    Dashboard tab geometry and hit-routing self-test.

    WHY THIS EXISTS. PluginEditor.cpp has produced four overlap bugs and every
    rule in SESSION_C_BUILD_SPEC.md section 8 is one of them. The Dashboard is
    a new surface with the same shape of risk: rects authored in one place,
    consumed by a painter and a hit test that must not disagree. tabstrip_test
    holds the tab strip to that; this holds the tab's content to it.

    IT CALLS THE SHIPPING CODE. DashboardView::layout and ::zoneIndexAt are the
    real functions the plugin runs, reached through a friend struct so nothing
    had to be made public for a test. A test carrying its own copy of the
    arithmetic would be the duplication bug wearing a different hat, which is
    exactly what the 505-seed art harness turned out to be.

    WHAT IT CHECKS:
      - every clickable zone routes to ITSELF, so the share chip sitting inside
        a chain row resolves to the chip and not to the row underneath it
      - no zone escapes the content box
      - layout is a pure function of the width: same width, same answer
      - sections that are NOT shown leave no rects behind, which is the stale
        coordinates half of the overlap bugs
      - a "web" onboarding target produces NO zone, because there is no
        plugin-to-web token path and a button would promise a login screen
*/

#include "DashboardTab.h"

#include <cstdio>
#include <vector>

/** The friend declared in DashboardTab.h. */
struct EchoJayDashboardTestAccess
{
    using View = echojay::DashboardView;

    static int layout (View& v, int w)            { return v.layout (w); }
    static int zoneAt (const View& v, juce::Point<int> p) { return v.zoneIndexAt (p); }
    static int zoneCount (const View& v)          { return (int) v.zones_.size(); }
    static juce::Rectangle<int> zoneRect (const View& v, int i)
    {
        return v.zones_[(size_t) i].rect;
    }
    static juce::String zoneUrl (const View& v, int i) { return v.zones_[(size_t) i].url; }
    static juce::String zoneSurface (const View& v, int i)
    {
        return v.zones_[(size_t) i].link.surface;
    }
    static int tileCount (const View& v)   { return (int) v.projectTileRects_.size(); }
    static int chatRowCount (const View& v){ return (int) v.chatRowRects_.size(); }
    static int chainRowCount(const View& v){ return (int) v.chainRowRects_.size(); }
    static int stepCount (const View& v)   { return (int) v.onboardingStepRects_.size(); }
    static bool announceEmpty (const View& v) { return v.announceRect_.isEmpty(); }
    static bool continueEmpty (const View& v) { return v.continueRect_.isEmpty(); }
    static bool upgradeEmpty (const View& v)  { return v.upgradeChipRect_.isEmpty(); }
    static std::vector<juce::Rectangle<int>> allRects (const View& v)
    {
        std::vector<juce::Rectangle<int>> r {
            v.headerRect_, v.statusRect_, v.usageRect_, v.usageBarRect_,
            v.usageResetRect_, v.upgradeChipRect_, v.continueRect_,
            v.projectsHeadingRect_, v.projectsEmptyRect_, v.chatsHeadingRect_,
            v.chatsEmptyRect_, v.chainsHeadingRect_, v.chainsEmptyRect_,
            v.onboardingRect_, v.onboardingHeadingRect_, v.announceRect_,
            v.signedOutRect_ };
        for (auto& t : v.projectTileRects_)     r.push_back (t);
        for (auto& t : v.chatRowRects_)         r.push_back (t);
        for (auto& t : v.chainRowRects_)        r.push_back (t);
        for (auto& t : v.chainShareChipRects_)  r.push_back (t);
        for (auto& t : v.onboardingStepRects_)  r.push_back (t);
        return r;
    }
};

namespace
{
using TA = EchoJayDashboardTestAccess;

int failures = 0;

void check (bool ok, const char* what, int a = 0, int b = 0)
{
    if (ok) return;
    ++failures;
    std::printf ("  FAIL  %s  (%d, %d)\n", what, a, b);
}

/** A realistic surface=plugin payload: five of everything (the server caps the
    plugin surface at five), one shared chain, one upload, an incomplete
    onboarding block whose last step targets the web, and an announcement. */
juce::var makePayload (bool full)
{
    auto obj = [] { return juce::DynamicObject::Ptr (new juce::DynamicObject()); };

    auto usage = obj();
    usage->setProperty ("unitsUsed", 412);
    usage->setProperty ("unitsTotal", 1200);
    usage->setProperty ("pct", 34);
    usage->setProperty ("resetsAt", "2026-08-01T00:00:00Z");
    usage->setProperty ("tierLabel", full ? "Free" : "Studio");
    usage->setProperty ("upgradeUrl", "/upgrade");

    auto root = obj();
    root->setProperty ("v", 1);
    root->setProperty ("ttl", 60);
    root->setProperty ("usage", juce::var (usage.get()));

    auto user = obj();
    user->setProperty ("handle", "seand");
    user->setProperty ("displayName", "Sean D");
    user->setProperty ("tier", full ? "free" : "studio");
    user->setProperty ("hasProfile", true);
    root->setProperty ("user", juce::var (user.get()));

    if (! full)
    {
        // The sparse case: no continue card, no rows, onboarding complete, no
        // announcement. Everything those sections own must be gone.
        auto ob = obj();
        ob->setProperty ("complete", true);
        ob->setProperty ("steps", juce::var (juce::Array<juce::var>()));
        root->setProperty ("onboarding", juce::var (ob.get()));
        root->setProperty ("projects", juce::var (juce::Array<juce::var>()));
        root->setProperty ("recentChats", juce::var (juce::Array<juce::var>()));
        root->setProperty ("chains", juce::var (juce::Array<juce::var>()));
        return juce::var (root.get());
    }

    auto target = obj();
    target->setProperty ("surface", "chat");
    target->setProperty ("id", "c_88");
    auto cont = obj();
    cont->setProperty ("kind", "chat");
    cont->setProperty ("label", "Drill vocal, bus glue");
    cont->setProperty ("target", juce::var (target.get()));
    root->setProperty ("continue", juce::var (cont.get()));

    juce::Array<juce::var> projects;
    for (int i = 0; i < 5; ++i)
    {
        auto art = obj();
        art->setProperty ("kind", i == 2 ? "upload" : "procedural");
        art->setProperty ("seed", "9f3a1c7d22b40e51");
        art->setProperty ("url", i == 2 ? juce::var ("https://example.invalid/a.jpg")
                                        : juce::var());
        auto counts = obj();
        counts->setProperty ("chats", 4);
        counts->setProperty ("captures", 9);
        counts->setProperty ("chains", 2);
        auto p = obj();
        p->setProperty ("id", "p_" + juce::String (i));
        p->setProperty ("name", "A project with a fairly long name " + juce::String (i));
        p->setProperty ("genre", "drill");
        p->setProperty ("art", juce::var (art.get()));
        p->setProperty ("counts", juce::var (counts.get()));
        projects.add (juce::var (p.get()));
    }
    root->setProperty ("projects", projects);

    juce::Array<juce::var> chats;
    for (int i = 0; i < 5; ++i)
    {
        auto c = obj();
        c->setProperty ("id", "c_" + juce::String (i));
        c->setProperty ("title", "Bus glue " + juce::String (i));
        c->setProperty ("snippet", "Try the 1176 after the tube pre and see what");
        chats.add (juce::var (c.get()));
    }
    root->setProperty ("recentChats", chats);

    juce::Array<juce::var> chains;
    for (int i = 0; i < 5; ++i)
    {
        auto c = obj();
        c->setProperty ("id", "ch_" + juce::String (i));
        c->setProperty ("name", "Drill vocal " + juce::String (i));
        c->setProperty ("slotCount", 6);
        c->setProperty ("qualityScore", 82);
        c->setProperty ("source", i == 4 ? "import" : "plugin");
        // Two shared, so the chip path is exercised more than once.
        if (i == 0 || i == 3)
        {
            c->setProperty ("shareSlug", "ab12cd34ef");
            c->setProperty ("shareVisibility", "public");
        }
        chains.add (juce::var (c.get()));
    }
    root->setProperty ("chains", chains);

    juce::Array<juce::var> steps;
    const char* keys[]   = { "scan", "capture", "chain", "profile" };
    const char* labels[] = { "Scan your plugins", "Run your first capture",
                             "Save a chain", "Claim your handle" };
    const char* surf[]   = { "settings", "meters", "chain", "web" };
    for (int i = 0; i < 4; ++i)
    {
        auto t = obj();
        t->setProperty ("surface", surf[i]);
        if (i == 3) t->setProperty ("path", "/settings/profile");
        auto s = obj();
        s->setProperty ("key", keys[i]);
        s->setProperty ("label", labels[i]);
        s->setProperty ("done", i < 2);
        s->setProperty ("target", juce::var (t.get()));
        steps.add (juce::var (s.get()));
    }
    auto ob = obj();
    ob->setProperty ("complete", false);
    ob->setProperty ("steps", steps);
    root->setProperty ("onboarding", juce::var (ob.get()));

    auto ann = obj();
    ann->setProperty ("id", "a_1");
    ann->setProperty ("snippet", "v2.24 adds the Dashboard tab to the plugin.");
    ann->setProperty ("postedAt", "2026-07-28T10:00:00Z");
    auto community = obj();
    community->setProperty ("latestAnnouncement", juce::var (ann.get()));
    root->setProperty ("community", juce::var (community.get()));

    return juce::var (root.get());
}

/** Widths the plugin can actually present: the 900px full-mode floor minus the
    scrollbar, the default, the large preset, and the two-column threshold from
    both sides. */
void exerciseWidth (echojay::DashboardView& v, int width, const char* label)
{
    const int h = TA::layout (v, width);
    check (h > 0, "content has a height", width, h);

    // Pure in width: the owner calls layout() to size the component and the
    // component's own resized() calls it again. Those two must agree.
    const int again = TA::layout (v, width);
    check (h == again, "layout is a pure function of the width", h, again);

    const juce::Rectangle<int> content (0, 0, width, h);
    for (const auto& r : TA::allRects (v))
    {
        if (r.isEmpty()) continue;
        check (content.contains (r), "rect stays inside the content box",
               r.getRight(), r.getBottom());
    }

    const int n = TA::zoneCount (v);
    check (n > 0, "the surface has clickable zones", n, 0);
    for (int i = 0; i < n; ++i)
    {
        const auto r = TA::zoneRect (v, i);
        check (! r.isEmpty(), "no empty zone was registered", i, 0);
        check (content.contains (r), "zone stays inside the content box", i,
               r.getBottom());
        // THE routing check. A share chip lives inside its chain row, so this
        // is what proves the chip was added first and wins the hit test.
        const int hit = TA::zoneAt (v, r.getCentre());
        check (hit == i, "zone centre routes to that zone", i, hit);
    }

    std::printf ("  ok    %-22s width %4d: h=%4d, %2d zones, all route\n",
                 label, width, h, n);
}
} // namespace

int main()
{
    std::printf ("dashboard geometry self-test\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    echojay::DashboardView view;

    // ---- populated account -------------------------------------------------
    view.setPayload (echojay::DashPayload::parse (makePayload (true)),
                     juce::Time::currentTimeMillis(), false);

    for (int w : { 892, 900, 1170, 1400, 1800 })
        exerciseWidth (view, w, "populated");

    // The column split. layout() measures the CONTENT width (the component
    // width less the 20px padding either side), so the threshold falls at a
    // component width of 760, not 720. These bracket it from both sides, and
    // 400 is well under it.
    for (int w : { 400, 759, 760 })
        exerciseWidth (view, w, "populated, narrow");

    // The sections that must exist, once, on a populated account.
    TA::layout (view, 1170);
    check (TA::tileCount (view) == 5,      "five project tiles", TA::tileCount (view));
    check (TA::chatRowCount (view) == 5,   "five chat rows",     TA::chatRowCount (view));
    check (TA::chainRowCount (view) == 5,  "five chain rows",    TA::chainRowCount (view));
    check (TA::stepCount (view) == 4,      "four onboarding steps", TA::stepCount (view));
    check (! TA::announceEmpty (view),     "announcement card is laid out");
    check (! TA::continueEmpty (view),     "continue card is laid out");
    check (! TA::upgradeEmpty (view),      "Free tier gets the upgrade chip");

    // "Claim your handle" targets the web. There is no plugin-to-web token
    // path, so it must be TEXT: no zone anywhere may carry surface "web".
    {
        bool webZone = false;
        for (int i = 0; i < TA::zoneCount (view); ++i)
            if (TA::zoneSurface (view, i) == "web") webZone = true;
        check (! webZone, "no zone follows a web target");
    }

    // The only urls offered are the two that work with NO auth at all.
    {
        int urls = 0;
        for (int i = 0; i < TA::zoneCount (view); ++i)
        {
            const auto u = TA::zoneUrl (view, i);
            if (u.isEmpty()) continue;
            ++urls;
            check (u.startsWith ("https://www.echojay.ai/c/")
                || u == "https://www.echojay.ai/upgrade",
                   "url zone points at an auth-free page", i, 0);
        }
        check (urls == 3, "two share chips plus one upgrade chip", urls, 3);
    }

    // ---- sparse account, and the stale-rect rule ---------------------------
    //
    // Sections that are not shown must leave NOTHING behind. A card whose
    // rects survive a payload change is the stale-coordinates half of every
    // overlap bug this surface's rules come from.
    view.setPayload (echojay::DashPayload::parse (makePayload (false)),
                     juce::Time::currentTimeMillis(), false);
    TA::layout (view, 1170);
    check (TA::tileCount (view) == 0,     "no tiles on an empty account", TA::tileCount (view));
    check (TA::chatRowCount (view) == 0,  "no chat rows", TA::chatRowCount (view));
    check (TA::chainRowCount (view) == 0, "no chain rows", TA::chainRowCount (view));
    check (TA::stepCount (view) == 0,     "no onboarding rows once complete", TA::stepCount (view));
    check (TA::announceEmpty (view),      "no announcement rect");
    check (TA::continueEmpty (view),      "no continue rect");
    check (TA::upgradeEmpty (view),       "no upgrade chip on a paid tier");
    check (TA::zoneCount (view) == 0,     "nothing is clickable", TA::zoneCount (view));
    std::printf ("  ok    empty account leaves no rects and no zones behind\n");

    // ---- a body that is not a dashboard document ---------------------------
    check (! echojay::DashPayload::parse (juce::var()).valid,
           "a void body is not a payload");
    check (! echojay::DashPayload::parse (juce::JSON::parse ("{\"error\":\"nope\"}")).valid,
           "an error body is not a payload");
    std::printf ("  ok    a non-dashboard body is rejected, not drawn as empty\n");

    // ---- degenerate widths -------------------------------------------------
    // The editor cannot reach these (the floor is 900) but layout runs
    // mid-resize and must not divide by zero or produce inverted rects.
    view.setPayload (echojay::DashPayload::parse (makePayload (true)),
                     juce::Time::currentTimeMillis(), false);
    for (int w : { 0, 1, 40, 119, 120 })
    {
        const int h = TA::layout (view, w);
        check (h >= 0, "degenerate width yields a sane height", w, h);
        for (int i = 0; i < TA::zoneCount (view); ++i)
            check (! TA::zoneRect (view, i).isEmpty(), "no empty zone at any width", w, i);
    }
    std::printf ("  ok    degenerate widths do not crash\n");

    if (failures)
    {
        std::printf ("dashboard_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf ("dashboard_test: PASS\n");
    return 0;
}
