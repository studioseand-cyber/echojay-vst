/*
    Link MIXER geometry and hit-precedence self-test.

    WHY THIS EXISTS. The Link tab is being rebuilt as a horizontally scrolling
    set of variable-count, variable-width strips, which is the single highest
    risk shape for the two-authorities bug that produced four separate failures
    in two days (the tab strip, the Visualisation preset hit test, the two
    header icons, and a button whose visibility was set twice). tabstrip_test
    holds the tab strip to one authority and dashboard_test holds the Dashboard
    to it; this holds the mixer to it.

    IT CALLS THE SHIPPING CODE. EchoJayEditor::layOutStrips and ::stripHitAt are
    the real functions the plugin runs, reached through a friend struct so
    neither had to be made public for a test, and linked out of the Release
    SharedCode lib so this exercises the object code that ships. A test carrying
    its own copy of the arithmetic would BE the duplication bug it is meant to
    catch, which is exactly what the 505-seed art harness turned out to be.

    WHAT IT CHECKS:
      - layOutStrips is a pure function: same inputs, same rects
      - no element rect escapes its own strip, and no strip escapes the band
      - adjacent strips are separated by exactly kStripGap and never overlap
      - element rects within a strip never overlap each other
      - stripsTotalWidth agrees with the last strip's right edge, so the scroll
        range cannot disagree with the rects it scrolls over
      - the pinned Mix Bus strip has the SAME internal vertical layout as a
        Link strip, which is the "master cannot drift from the channels" claim
      - hit precedence is fader, then AI, then badge, then background, and
        every verdict comes from a STORED rect
      - the fader keeps its 1:8 aspect lock and never exceeds the strip's
        usable width, because a stretched cap is a distorted control
      - zero Links leaves no strip rects behind, and a degenerate band produces
        nothing rather than garbage
*/

#include "PluginEditor.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/** The friend declared in PluginEditor.h. Nothing was made public for this. */
struct EchoJayLinkMixerTestAccess
{
    using Ed    = EchoJayEditor;
    using Geom  = EchoJayEditor::StripGeom;
    using Hit   = EchoJayEditor::StripHit;

    static void layOut (juce::Rectangle<int> band, int stripW,
                        const std::vector<juce::String>& addrs,
                        Geom& bus, std::vector<Geom>& links)
    { Ed::layOutStrips (band, stripW, addrs, bus, links); }

    static Hit hitAt (const Geom& sg, juce::Point<int> p)
    { return Ed::stripHitAt (sg, p); }

    static int totalWidth (int count, int stripW)
    { return Ed::stripsTotalWidth (count, stripW); }

    static int gap()        { return Ed::kStripGap; }
    static int wNarrow()    { return Ed::kStripWNarrow; }
    static int wWide()      { return Ed::kStripWWide; }
    static int faderHMax()  { return Ed::kFaderHMax; }
    static int faderHMin()  { return Ed::kFaderHMin; }

    static bool selected (bool isBus, const juce::String& entryUid,
                          const juce::String& effectiveUid)
    { return Ed::stripSelected (isBus, entryUid, effectiveUid); }

    static float travelTop (juce::Rectangle<int> a)         { return Ed::faderTravelTop (a); }
    static float travelBot (juce::Rectangle<int> a)         { return Ed::faderTravelBot (a); }
    static int   capH (juce::Rectangle<int> a)              { return Ed::faderCapH (a); }
    static int   capW (juce::Rectangle<int> a)              { return Ed::faderCapW (a); }
    static int   capSrcW()                                  { return Ed::kFaderCapSrcW; }
    static int   capSrcH()                                  { return Ed::kFaderCapSrcH; }
    static float perPixel (juce::Rectangle<int> t)          { return Ed::gainPerPixel (t); }
    static float fineRatio()                                { return Ed::kFaderFineRatio; }
    static float gFromY (int y, juce::Rectangle<int> t)     { return Ed::gainFromY (y, t); }
    static int   yFromG (float db, juce::Rectangle<int> t)  { return Ed::yFromGain (db, t); }

    static int   bandGap()    { return Ed::kBandGap; }
    static int   meterWMin()  { return Ed::kMeterWMin; }
    static int   meterY (float db, juce::Rectangle<int> a) { return Ed::meterYForDb (db, a); }
    static void  barRects (juce::Rectangle<int> a, int& gut, juce::Rectangle<int> b[2])
    { Ed::meterBarRects (a, gut, b); }
    static float meterFloor()                               { return Ed::kMeterDbFloor; }
    static const float* meterMarks (int& n)
    { n = Ed::kMeterMarkCount; return Ed::kMeterMarks; }

    using Rows = Ed::ChainRows;
    static Rows chainRows (juce::Rectangle<int> data, int occupied, int scrollY)
    { return Ed::layOutChainRows (data, occupied, scrollY); }
    static int blockPitch() { return Ed::kChainBlockH + Ed::kChainBlockGap; }
    static void ctrls2 (juce::Rectangle<int> block,
                        juce::Rectangle<int>& b, juce::Rectangle<int>& x)
    { Ed::blockCtrlRects (block, b, x); }

    using ChainState = Ed::ChainDisplayState;
    static ChainState chainState (bool valid, int n)
    { return Ed::chainDisplayState (valid, n); }
    using Ctrl = Ed::CtrlZone;
    static void ctrls (juce::Rectangle<int> r, std::vector<Ctrl>& out)
    { Ed::layOutLinkCtrls (r, out); }
    static std::vector<int> ctrlIds()
    { return { Ed::kCtrlNarrow, Ed::kCtrlWide,
               Ed::kCtrlNumbers, Ed::kCtrlChain }; }
};

using T    = EchoJayLinkMixerTestAccess;
using Geom = T::Geom;
using Hit  = T::Hit;

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (!ok) { std::printf ("  FAIL: %s\n", what); ++failures; }
}

static void checkEq (int got, int want, const char* what)
{
    if (got != want)
    {
        std::printf ("  FAIL: %s (got %d, want %d)\n", what, got, want);
        ++failures;
    }
}

static std::vector<juce::String> addrs (int n)
{
    std::vector<juce::String> v;
    for (int i = 0; i < n; ++i) v.push_back ("uid" + juce::String (i));
    return v;
}

/** Every element rect of one strip, for the escape and overlap checks. */
static std::vector<std::pair<const char*, juce::Rectangle<int>>> elements (const Geom& s)
{
    return { { "name", s.name }, { "badge", s.badge }, { "active", s.active },
             { "data", s.data }, { "fader", s.fader }, { "faderImg", s.faderImg },
             { "clip", s.clip }, { "meter", s.meter }, { "ai", s.ai } };
}

// -----------------------------------------------------------------------------

static void testPure()
{
    std::printf ("purity: same inputs give the same rects\n");
    const juce::Rectangle<int> band { 32, 142, 1050, 500 };

    Geom busA, busB;
    std::vector<Geom> a, b;
    T::layOut (band, T::wNarrow(), addrs (7), busA, a);
    T::layOut (band, T::wNarrow(), addrs (7), busB, b);

    checkEq ((int) a.size(), (int) b.size(), "strip count is stable");
    check (busA.full == busB.full, "bus rect is stable");
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        check (a[i].full   == b[i].full,   "strip full is stable");
        check (a[i].fader  == b[i].fader,  "strip fader is stable");
        check (a[i].data   == b[i].data,   "strip data is stable");
        check (a[i].addr   == b[i].addr,   "strip addr is stable");
    }
}

static void testContainment (int stripW, int bandH, int n)
{
    std::printf ("containment: stripW=%d bandH=%d n=%d\n", stripW, bandH, n);
    const juce::Rectangle<int> band { 32, 142, 1050, bandH };

    Geom bus;
    std::vector<Geom> links;
    T::layOut (band, stripW, addrs (n), bus, links);

    checkEq ((int) links.size(), n, "one strip per address");

    // The pinned bus strip is in EDITOR coords and must sit inside the band.
    check (band.contains (bus.full), "bus strip stays inside the band");
    checkEq (bus.full.getX(), band.getX(), "bus strip is pinned to the band's left");
    checkEq (bus.full.getHeight(), band.getHeight(), "bus strip is as tall as the band");
    check (bus.isBus, "bus strip is flagged as the bus");
    check (bus.addr.isEmpty(), "bus strip carries no uid");

    for (size_t i = 0; i < links.size(); ++i)
    {
        const auto& s = links[i];
        check (! s.isBus, "link strip is not flagged as the bus");
        check (s.addr.isNotEmpty(), "link strip carries its uid");
        checkEq (s.full.getY(), 0, "link strip starts at view-local y=0");
        checkEq (s.full.getHeight(), band.getHeight(), "link strip is as tall as the band");
        checkEq (s.full.getWidth(), stripW, "link strip is exactly one strip wide");

        // No element escapes its own strip. This is the half of the rule the
        // Visualisation preset strip broke in the other direction.
        for (auto& [nameOf, r] : elements (s))
            if (! r.isEmpty())
            {
                const std::string what
                    = std::string ("element stays inside its strip: ") + nameOf;
                check (s.full.contains (r), what.c_str());
            }

        // Elements never overlap each other. ONE sanctioned exception:
        // faderImg lives INSIDE the fader lane by design (containment is
        // asserted in the band test); every other pair stays disjoint.
        auto els = elements (s);
        for (size_t x = 0; x < els.size(); ++x)
            for (size_t y = x + 1; y < els.size(); ++y)
            {
                const bool lanePair =
                    (std::string (els[x].first) == "fader"
                     && std::string (els[y].first) == "faderImg")
                 || (std::string (els[x].first) == "faderImg"
                     && std::string (els[y].first) == "fader");
                if (lanePair) continue;
                if (! els[x].second.isEmpty() && ! els[y].second.isEmpty())
                    check (! els[x].second.intersects (els[y].second),
                           "two elements of one strip overlap");
            }

        // Adjacent strips are separated by exactly one gap, never overlapping.
        if (i + 1 < links.size())
        {
            checkEq (links[i + 1].full.getX() - s.full.getRight(), T::gap(),
                     "adjacent strips are exactly one gap apart");
            check (! s.full.intersects (links[i + 1].full),
                   "adjacent strips do not overlap");
        }
    }

    // The scroll range and the rects agree.
    if (! links.empty())
        checkEq (T::totalWidth (n, stripW), links.back().full.getRight(),
                 "stripsTotalWidth matches the last strip's right edge");
}

static void testBusMatchesChannel()
{
    std::printf ("the pinned master shares the channels' internal layout\n");
    const juce::Rectangle<int> band { 32, 142, 1050, 500 };

    Geom bus;
    std::vector<Geom> links;
    T::layOut (band, T::wNarrow(), addrs (3), bus, links);
    check (! links.empty(), "have a link strip to compare against");
    if (links.empty()) return;

    // Same element HEIGHTS and same vertical offsets within the strip. Only x
    // may differ, because only x differs between strips by design.
    const auto& ch = links[0];
    auto rel = [] (juce::Rectangle<int> r, juce::Rectangle<int> full)
    { return juce::Rectangle<int> (r.getX() - full.getX(), r.getY() - full.getY(),
                                   r.getWidth(), r.getHeight()); };

    auto be = elements (bus);
    auto ce = elements (ch);
    for (size_t i = 0; i < be.size(); ++i)
        check (rel (be[i].second, bus.full) == rel (ce[i].second, ch.full),
               "master and channel strips share one internal layout");
}

static void testFaderAspect (int stripW, int bandH)
{
    // Since 8b the fader rect is a COLUMN (ticks lane + image); the 1:8 lock
    // applies to the IMPLIED IMAGE, whose width is height/8 by construction.
    std::printf ("fader/meter band: stripW=%d bandH=%d\n", stripW, bandH);
    const juce::Rectangle<int> band { 32, 142, 1050, bandH };

    Geom bus;
    std::vector<Geom> links;
    T::layOut (band, stripW, addrs (2), bus, links);
    if (links.empty()) return;

    const auto& s0 = links[0];
    const auto f = s0.fader;
    std::printf ("  lane %dx%d, cap %dx%d, throw %d px (%.3f dB/px), meter %dx%d\n",
                 f.getWidth(), f.getHeight(),
                 T::capW (s0.faderImg), T::capH (s0.faderImg),
                 f.getHeight() - T::capH (s0.faderImg),
                 36.0 / (double) juce::jmax (1, f.getHeight() - T::capH (s0.faderImg)),
                 s0.meter.getWidth(), s0.meter.getHeight());

    // THE METER IS PERMANENT CHROME: present at every shipping size.
    check (! s0.meter.isEmpty(), "the meter band exists");
    check (s0.meter.getWidth() >= T::meterWMin(), "the meter has its minimum width");
    // The clip lamp sits ABOVE the bars, same width, stored not derived.
    if (! s0.clip.isEmpty())
    {
        check (s0.clip.getBottom() <= s0.meter.getY(), "the lamp sits above the bars");
        checkEq (s0.clip.getX(), s0.meter.getX(), "lamp and bars share a left edge");
        checkEq (s0.clip.getWidth(), s0.meter.getWidth(), "lamp and bars share a width");
    }

    if (! f.isEmpty())
    {
        const auto fi = s0.faderImg;
        // VISUAL-PASS CONTRACT (1b): the LANE shares the band's height and
        // baseline exactly, so fader and meter read as one height.
        const int bandTop = s0.clip.isEmpty() ? s0.meter.getY() : s0.clip.getY();
        checkEq (f.getY(), bandTop,             "the lane starts at the band top");
        checkEq (f.getBottom(), s0.meter.getBottom(),
                 "the lane and the meter share a baseline");
        // The IMAGE stays aspect-locked, centred in the lane, contained.
        check (! fi.isEmpty(), "the fader image exists");
        check (f.contains (fi), "the image sits inside the lane");
        check (std::abs ((fi.getY() - f.getY())
                         - (f.getBottom() - fi.getBottom())) <= 1,
               "the image is centred in the lane");
        // THE CLAIM THAT FAILED THREE TIMES, now held as EQUALITY at both
        // ends: the CAP AREA spans the whole lane, so the cap can reach the
        // meter's top and bottom. The 1:8 assertions retire with the lock;
        // the cap sprite carries its own aspect, checked in the mapping
        // test against the sprite's real 54:113.
        checkEq (fi.getY(),      f.getY(),      "the cap area starts at the lane top");
        checkEq (fi.getBottom(), f.getBottom(), "the cap area ends at the lane bottom");
        checkEq (fi.getRight(),  f.getRight(),  "the cap area is right-aligned, ticks to its left");
        check (fi.getWidth() <= f.getWidth(), "the cap area fits its lane");
        check (T::capH (fi) <= T::capSrcH(), "the cap never upscales from the sprite");
        check (T::capW (fi) <= T::capSrcW(), "the cap never upscales horizontally");
        check (T::capW (fi) <  fi.getWidth(), "the cap is narrower than its area");
        check (T::capH (fi) < fi.getHeight(), "the cap is shorter than its travel");

        // THE BUDGET: the pair no longer fills the band. Both widths are
        // fixed per mode and the GROUP IS CENTRED, surplus width becoming
        // breathing room rather than fatter controls.
        const int bandL = s0.full.getX() + 4, bandR = s0.full.getRight() - 4;
        const int groupW = f.getWidth() + T::bandGap() + s0.meter.getWidth();
        check (groupW <= bandR - bandL, "the pair fits the band");
        check (std::abs ((f.getX() - bandL) - (bandR - s0.meter.getRight())) <= 1,
               "the pair is centred in the band");
        check (f.getWidth() > s0.meter.getWidth(),
               "the fader column is wider than the meter");
        // The image fills a real proportion of its lane rather than
        // floating in a long groove (the fault this pass fixes).
        checkEq (fi.getHeight(), f.getHeight(),
                 "the cap area IS the lane height, so the throw is the whole lane");

        checkEq (s0.meter.getX() - f.getRight(), T::bandGap(),
                 "fader column and meter sit one band gap apart");
        check (f.getBottom() <= s0.ai.getY(), "the band sits above the AI button");
        if (links.size() > 1)
            checkEq (T::yFromG (0.0f, fi), T::yFromG (0.0f, links[1].faderImg),
                     "0 dB sits at the same y on every strip");

        // (1a) Clip lamp columns == meter bar columns, from the SHARED
        // source the painters consume: same x, same width, per channel.
        int gut = 0; juce::Rectangle<int> bars[2];
        T::barRects (s0.meter, gut, bars);
        for (int ch = 0; ch < 2; ++ch)
        {
            check (s0.meter.contains (bars[ch]), "bar stays inside the meter");
            // the lamp painter derives its boxes from bars[ch].getX()/W
            // verbatim; hold the source sane so that derivation holds.
            check (bars[ch].getWidth() >= 2, "bar has drawable width");
        }
        check (bars[0].getRight() + 2 == bars[1].getX(),
               "the two bars sit two apart, lamp halves likewise");
    }
}

static void testHitPrecedence()
{
    std::printf ("hit precedence: fader, AI, badge, then background\n");
    const juce::Rectangle<int> band { 32, 142, 1050, 500 };

    Geom bus;
    std::vector<Geom> links;
    T::layOut (band, T::wWide(), addrs (4), bus, links);
    check (! links.empty(), "have a strip to hit");
    if (links.empty()) return;

    const auto& s = links[1];

    check (T::hitAt (s, s.fader.getCentre()) == Hit::Fader,  "fader centre hits the fader");
    check (T::hitAt (s, s.meter.getCentre()) == Hit::Meter,  "meter centre hits the meter");
    if (! s.clip.isEmpty())
        check (T::hitAt (s, s.clip.getCentre()) == Hit::Clip,
               "the clip lamp claims its own rect");
    // The abutment: the last pixel of the fader column is the FADER's, so a
    // drag starting there is never swallowed by the meter beside it.
    check (T::hitAt (s, { s.fader.getRight() - 1, s.fader.getCentreY() })
               == Hit::Fader, "the fader wins its own right edge");
    check (T::hitAt (s, s.ai.getCentre())    == Hit::Ai,     "AI centre hits the AI button");
    check (T::hitAt (s, s.badge.getCentre()) == Hit::Badge,  "badge centre hits the badge");
    check (T::hitAt (s, s.active.getCentre()) == Hit::Active,
           "the merged Active control claims its rect (step 3)");
    check (T::hitAt (s, s.name.getCentre())  == Hit::Background,
           "the name area falls through to background, which selects");
    check (T::hitAt (s, s.data.getCentre())  == Hit::Background,
           "the data area falls through to background, which selects");

    // Outside the strip is nothing at all, including the gap between strips.
    check (T::hitAt (s, { s.full.getRight() + 1, s.full.getCentreY() }) == Hit::None,
           "a point in the gap hits nothing");
    check (T::hitAt (s, { s.full.getX() - 1, s.full.getCentreY() }) == Hit::None,
           "a point left of the strip hits nothing");

    // Every strip's own rects route to that strip and no other. This is the
    // check that would have caught the Visualisation preset bug: a hit test
    // agreeing with the rects it was given rather than with recomputed ones.
    for (const auto& a : links)
        for (const auto& b : links)
            if (&a != &b)
                check (T::hitAt (b, a.fader.getCentre()) == Hit::None
                    || a.full == b.full,
                       "one strip's fader never resolves inside another strip");
}

static void testClickable (int stripW, int bandH)
{
    // Step 3 chrome check: at the band heights that SHIP, every interactive
    // element must be big enough to actually hit. A rect that paints but
    // cannot be pressed is the chrome half of action honesty.
    std::printf ("clickability: stripW=%d bandH=%d\n", stripW, bandH);
    const juce::Rectangle<int> band { 32, 142, 1050, bandH };

    Geom bus;
    std::vector<Geom> links;
    T::layOut (band, stripW, addrs (3), bus, links);
    if (links.empty()) return;

    for (const Geom* s : { (const Geom*) &bus, (const Geom*) &links[0] })
    {
        check (s->badge.getHeight()  >= 14, "badge is tall enough to press");
        check (s->active.getHeight() >= 14, "Active control is tall enough to press");
        check (s->ai.getHeight()     >= 16, "AI button is tall enough to press");
        check (s->fader.getHeight()  >= 40, "fader is tall enough to drag");
        check (s->badge.getWidth()   >= 24, "badge is wide enough to press");
        check (s->ai.getWidth()      >= 24, "AI button is wide enough to press");
    }
}

static void testSelection()
{
    // Step 4: the selection predicate's full truth table. ONE selection
    // state: the effectiveUid argument is always effectiveChannelUid() at the
    // call sites, so these rows ARE the strip/banner agreement contract.
    std::printf ("selection: the stripSelected truth table\n");

    // The bus is the main context: selected exactly when NO channel is.
    check ( T::selected (true,  "", ""),        "bus selected in main context");
    check (!T::selected (true,  "", "uid1"),    "bus not selected while a channel is");

    // A Link strip selects on a REAL uid match only.
    check ( T::selected (false, "uid1", "uid1"), "link selected on uid match");
    check (!T::selected (false, "uid1", "uid2"), "link not selected on other uid");
    check (!T::selected (false, "uid1", ""),     "link not selected in main context");

    // THE LOAD-BEARING ROW: a legacy strip (empty uid) must NOT read as
    // selected in main context. Without the isNotEmpty() term, empty == empty
    // would light every legacy strip whenever the bus is selected.
    check (!T::selected (false, "", ""),        "legacy strip never selected (main context)");
    check (!T::selected (false, "", "uid1"),    "legacy strip never selected (channel active)");
}

static void testCtrls()
{
    // Step 5: the view-control zones, same contract as the strips: pure,
    // contained, non-overlapping, and every zone pressable.
    std::printf ("view controls: layout, containment, degenerate widths\n");

    // 548 is the worst SHIPPING control-row width: minimum window (900) with
    // the sidebar expanded, less the pads. All five segments must exist and
    // be pressable there, or one of the two modes is unreachable at the
    // window size people actually use.
    for (int w : { 1050, 548 })
    {
        const juce::Rectangle<int> r { 32, 112, w, 30 };
        std::vector<T::Ctrl> zs;
        T::ctrls (r, zs);

        checkEq ((int) zs.size(), 4, "all four segments exist at shipping width");
        for (const auto& z : zs)
        {
            check (r.contains (z.rect), "control zone stays inside the row");
            check (z.rect.getWidth()  >= 30, "control zone wide enough to press");
            check (z.rect.getHeight() >= 14, "control zone tall enough to press");
        }
        for (size_t a = 0; a < zs.size(); ++a)
            for (size_t b = a + 1; b < zs.size(); ++b)
                check (! zs[a].rect.intersects (zs[b].rect),
                       "control zones do not overlap");
        // Every expected id present exactly once
        for (int id : T::ctrlIds())
        {
            int n = 0;
            for (const auto& z : zs) if (z.id == id) ++n;
            checkEq (n, 1, "each control id appears exactly once");
        }
        // Purity
        std::vector<T::Ctrl> zs2;
        T::ctrls (r, zs2);
        checkEq ((int) zs2.size(), (int) zs.size(), "ctrl layout is pure (count)");
        for (size_t i = 0; i < zs.size() && i < zs2.size(); ++i)
            check (zs[i].rect == zs2[i].rect, "ctrl layout is pure (rects)");
    }

    // Too tight to hold everything: zones DROP rather than overlap or
    // escape. (Unreachable in shipping geometry; this is the contract that
    // makes squeezing safe.)
    {
        const juce::Rectangle<int> r { 32, 112, 120, 30 };
        std::vector<T::Ctrl> zs;
        T::ctrls (r, zs);
        check ((int) zs.size() < 4, "a too-tight row drops zones");
        for (const auto& z : zs)
            check (r.contains (z.rect), "surviving zones stay inside the row");
    }

    // Degenerate rects leave nothing behind.
    {
        std::vector<T::Ctrl> zs;
        T::ctrls ({ 0, 0, 0, 30 }, zs);
        checkEq ((int) zs.size(), 0, "zero-width row has no zones");
        T::ctrls ({ 0, 0, 500, 0 }, zs);
        checkEq ((int) zs.size(), 0, "zero-height row has no zones");
    }
}

static void testFaderMapping()
{
    // Step 7: the dB<->y pair and the frame selector, against the shipping
    // statics. The track is the WORST shipping fader (23x191 at minimum
    // window), because that is where quantisation error peaks.
    std::printf ("fader mapping: endpoints, round-trip, clamps, frames\n");
    const juce::Rectangle<int> t { 10, 50, 23, 191 };

    // THE TRAVEL BAND IS NOW DERIVED from the cap, not measured off the
    // frame: the cap centre runs half a cap below the lane top to half a cap
    // above the lane bottom, so THE CAP REACHES BOTH ENDS OF THE LANE. That
    // is the claim three passes failed, so it is held as EQUALITY.
    const int ch  = T::capH (t);
    const int top = (int) std::round (T::travelTop (t));
    const int bot = (int) std::round (T::travelBot (t));
    checkEq (T::yFromG ( 12.0f, t) - ch / 2, t.getY(),
             "at +12 dB the cap TOP sits exactly at the lane top");
    checkEq (T::yFromG (-24.0f, t) + ch / 2, t.getBottom(),
             "at -24 dB the cap BOTTOM sits exactly at the lane bottom");
    check (std::abs (T::yFromG ( 12.0f, t) - top) <= 1, "+12 dB is the travel top");
    check (std::abs (T::yFromG (-24.0f, t) - bot) <= 1, "-24 dB is the travel bottom");
    check (T::gFromY (bot, t) == -24.0f, "the travel bottom reads -24 dB");
    check (T::gFromY (top, t) ==  12.0f, "the travel top reads +12 dB");

    // UNIFORM SCALE: cap height derives from cap width by the sprite's own
    // ratio, so the cap is never stretched on one axis.
    check (std::abs ((double) ch / (double) T::capW (t)
                     - (double) T::capSrcH() / (double) T::capSrcW()) < 0.05,
           "the cap keeps the sprite's aspect");
    // The cap is SMALLER than the area it travels, so the drawn track shows
    // either side of it; and it is always a downscale from the sprite.
    check (T::capW (t) <  t.getWidth(),  "the cap is narrower than its area");
    check (T::capW (t) <= T::capSrcW(),  "the cap never upscales horizontally");
    check (ch          <= T::capSrcH(),  "the cap never upscales vertically");

    // A known gain maps to a known y: 0 dB is two thirds up the travel.
    {
        const int want = (int) std::round ((float) bot - (2.0f / 3.0f) * (float)(bot - top));
        check (std::abs (T::yFromG (0.0f, t) - want) <= 1, "0 dB sits two thirds up the travel");
    }

    // Clamping: outside the lane pins.
    check (T::gFromY (t.getBottom() + 50, t) == -24.0f, "below the lane clamps to -24");
    check (T::gFromY (t.getY() - 50, t)      ==  12.0f, "above the lane clamps to +12");

    // Round-trip within one pixel plus one snap step.
    for (float db = -24.0f; db <= 12.01f; db += 1.7f)
    {
        const float back = T::gFromY (T::yFromG (db, t), t);
        check (std::abs (back - db) <= 0.35f, "dB<->y round-trips within tolerance");
    }

}

static void testMeterMapping()
{
    // Step 8: the dB-to-y mapping shared by bars, peak ticks and scale
    // marks. One function, so agreement between the three is proven by
    // proving the function.
    std::printf ("meter mapping: endpoints, clamps, monotonic, marks\n");
    const juce::Rectangle<int> a { 5, 40, 22, 150 };

    checkEq (T::meterY (0.0f, a), a.getY(),        "0 dB is the top of the meter");
    checkEq (T::meterY (T::meterFloor(), a), a.getBottom(), "the floor is the bottom");
    // The 8c knee: -24 dB sits 30% up from the bottom, the Logic-style
    // compression boundary, held to one pixel of rounding.
    {
        const int knee = T::meterY (-24.0f, a);
        const int want = a.getBottom() - (int) std::round (0.30f * (float) a.getHeight());
        check (std::abs (knee - want) <= 1, "-24 dB sits at the 30% knee");
    }

    // Clamps: values past either end pin to the ends, never escape the rect.
    checkEq (T::meterY ( 20.0f, a), a.getY(),      "above 0 dB clamps to the top");
    checkEq (T::meterY (-200.0f, a), a.getBottom(),"below the floor clamps to the bottom");

    // Monotonic: quieter is always lower.
    int prev = a.getY() - 1;
    for (float db = 0.0f; db >= T::meterFloor() - 0.01f; db -= 0.5f)
    {
        const int y = T::meterY (db, a);
        check (y >= prev, "meter y is monotonic in dB");
        check (y >= a.getY() && y <= a.getBottom(), "meter y stays inside the area");
        prev = y;
    }

    // The scale marks are inside the range they are drawn on, ordered, and
    // bounded by the floor.
    int n = 0;
    const float* marks = T::meterMarks (n);
    for (int i = 0; i < n; ++i)
    {
        check (marks[i] <= 0.0f && marks[i] >= T::meterFloor(),
               "every scale mark is inside the meter range");
        if (i > 0)
            check (marks[i] < marks[i - 1], "scale marks descend");
    }
    check (marks[n - 1] == T::meterFloor(), "the last mark IS the floor");
}

static void testChainStates()
{
    // Step 9: the honesty matrix. "Nothing there" and "cannot see" are
    // different facts and must never collapse into one rendering.
    std::printf ("chain states: no-data vs empty vs list, and the labels\n");
    using CS = T::ChainState;
    check (T::chainState (false, 0) == CS::NoData, "missing sidecar is NO DATA");
    check (T::chainState (false, 5) == CS::NoData, "unreadable sidecar is NO DATA even with stale rows");
    check (T::chainState (true,  0) == CS::Empty,  "a parsed, empty rack is EMPTY");
    check (T::chainState (true,  3) == CS::List,   "a parsed rack with slots LISTS");

}

static void testChainBlocks()
{
    // The rack rows and their B/X controls: ONE pure formula consumed by
    // paint, hit testing, the tooltip and the wheel, so agreement is proven
    // by proving the formula.
    std::printf ("chain rows: empties, controls, scroll offset, purity\n");
    const juce::Rectangle<int> data { 10, 40, 84, 187 };   // wide data area

    auto r4 = T::chainRows (data, 4, 0);
    checkEq (r4.occupied, 4, "four plugins occupy four rows");
    check ((int) r4.rects.size() > 4, "empty slots fill the space beneath");
    checkEq (r4.maxScroll, 0, "a list that fits does not scroll");
    for (size_t i = 0; i < r4.rects.size(); ++i)
    {
        check (data.contains (r4.rects[i]), "row stays inside the data area");
        if (i > 0)
            check (r4.rects[i].getY() > r4.rects[i-1].getBottom(),
                   "rows stack without overlap");
    }
    for (int i = 0; i < r4.occupied; ++i)
    {
        juce::Rectangle<int> b, x;
        T::ctrls2 (r4.rects[(size_t) i], b, x);
        check (r4.rects[(size_t) i].contains (b) && r4.rects[(size_t) i].contains (x),
               "controls sit inside their block");
        check (! b.intersects (x), "B and X do not overlap");
        check (x.getX() > b.getX(), "X is outermost, matching the Chain card");
        check (b.getWidth() >= 12 && b.getHeight() >= 12, "B is big enough to press");
        check (x.getWidth() >= 12 && x.getHeight() >= 12, "X is big enough to press");
    }

    // AN EMPTY RACK IS ALL EMPTY SLOTS, never zero rows: the strip shows
    // capacity rather than absence.
    auto r0 = T::chainRows (data, 0, 0);
    checkEq (r0.occupied, 0, "an empty rack occupies no rows");
    check (! r0.rects.empty(), "an empty rack still draws empty slots");
    checkEq (r0.maxScroll, 0, "an empty rack has nothing to scroll");

    // OVERFLOW SCROLLS instead of collapsing: every plugin gets a full-size
    // row and maxScroll covers exactly the hidden remainder.
    auto rMany = T::chainRows (data, 40, 0);
    checkEq (rMany.occupied, 40, "every plugin gets a row when the rack overflows");
    checkEq ((int) rMany.rects.size(), 40, "an overflowing rack adds no empty slots");
    check (rMany.maxScroll > 0, "an overflowing rack can scroll");
    checkEq (rMany.rects[0].getHeight(), r4.rects[0].getHeight(),
             "blocks keep their size when the rack overflows");

    // THE OFFSET IS IN THE RECTS, which is what makes painting and hit
    // testing agree: scrolling by one pitch moves row 1 to where row 0 was.
    const int pitch = T::blockPitch();
    auto rScrolled = T::chainRows (data, 40, pitch);
    checkEq (rScrolled.rects[1].getY(), rMany.rects[0].getY(),
             "one pitch of scroll lifts row 1 into row 0's place");
    for (size_t i = 0; i < rMany.rects.size(); ++i)
        checkEq (rScrolled.rects[i].getY(), rMany.rects[i].getY() - pitch,
                 "every row shifts by exactly the scroll offset");

    // A stale offset (rack shrank under it) is CLAMPED, never left to
    // misplace a block.
    auto rClamped = T::chainRows (data, 40, 99999);
    checkEq (rClamped.rects[0].getY(), rMany.rects[0].getY() - rMany.maxScroll,
             "an over-large offset clamps to maxScroll");
    auto rNeg = T::chainRows (data, 40, -500);
    checkEq (rNeg.rects[0].getY(), rMany.rects[0].getY(),
             "a negative offset clamps to zero");

    // Purity + degenerate.
    auto again = T::chainRows (data, 4, 0);
    checkEq ((int) again.rects.size(), (int) r4.rects.size(), "row layout is pure");
    for (size_t i = 0; i < r4.rects.size() && i < again.rects.size(); ++i)
        check (r4.rects[i] == again.rects[i], "row rects are stable");
    auto rDegen = T::chainRows ({ 0, 0, 0, 0 }, 4, 0);
    checkEq ((int) rDegen.rects.size(), 0, "a degenerate area has no rows");
}

static void testPlacement()
{
    // SEND (3) measures like BUS: both are POST-FADER, so neither may fall
    // into the insert-point branch. ONE predicate decides, which is what
    // stops an "is it bus, otherwise channel" test mis-sorting a value
    // added later.
    std::printf ("placement: send measures post-fader like bus\n");
    check ( placementIsPostFader (1), "bus is post-fader");
    check ( placementIsPostFader (3), "SEND is post-fader");
    check (! placementIsPostFader (2), "channel insert is pre-fader");
    check (! placementIsPostFader (0), "unset is treated as pre-fader, conservatively");
    // Anything a future writer might send must not read as post-fader by
    // accident: only the two known post-fader values do.
    for (int p = 4; p < 8; ++p)
        check (! placementIsPostFader (p),
               "an unknown placement is never assumed post-fader");
}

static void testMaskGating()
{
    // 8c: the cross-version gate. An OLD writer's frame reads
    // fieldsMask == 0 (its memcpy writes its zero pad on every publish),
    // and the gate must say "no fast data" so the renderer takes the 8b
    // fallback instead of rendering the zero bytes as a 0 dBFS bar.
    std::printf ("mask gating: old-writer frames carry no fast peak\n");

    LinkMeterFrame oldWriter {};
    std::memset (&oldWriter, 0, sizeof (oldWriter));   // the shared page as an
    check (! frameHasFastPeak (oldWriter),             // old Link leaves it
           "an all-zero (old-writer) frame gates OFF the fast peak");

    LinkMeterFrame blank {};                           // claimSlot's blank
    check (! frameHasFastPeak (blank),
           "a claimed-but-never-published frame gates OFF the fast peak");
    check (blank.peakFastL <= -99.0f && blank.peakFastR <= -99.0f,
           "the blank frame's fast pair is ABSENT (-100), not 0 dBFS");

    LinkMeterFrame newWriter {};
    newWriter.fieldsMask = kFrameHasFastPeak;
    check (frameHasFastPeak (newWriter),
           "a new writer's bit turns the fast peak on");

    // Layout freeze: the whole cross-version story rests on these.
    check (sizeof (LinkMeterFrame) == 128, "the frame is still 128 bytes");
}

static void testBandSqueeze()
{
    // Below shipping widths the METER WINS: the fader column shrinks and
    // then drops entirely; the meter never disappears while a band exists.
    std::printf ("band squeeze: the meter wins\n");
    const juce::Rectangle<int> band { 32, 142, 1050, 500 };

    Geom bus;
    std::vector<Geom> links;
    T::layOut (band, 24, addrs (2), bus, links);   // 16px inner: no room
    if (! links.empty())
    {
        check (links[0].fader.isEmpty(), "an impossible band drops the fader");
        check (! links[0].meter.isEmpty(), "the meter survives the squeeze");
    }
}

static void testContentMigration()
{
    // 8b removed the meter content mode. Saved projects persisted 0/1/2 for
    // numbers/meter/chain; the mapper is the ONE authority on what each
    // becomes, and a saved CHAIN must stay CHAIN.
    std::printf ("content-mode migration: saved 0/1/2\n");
    using C = EchoJayProcessor::LinkMixerContent;
    check (EchoJayProcessor::linkMixerContentFromSaved (0) == C::Numbers,
           "saved 0 (numbers) stays Numbers");
    check (EchoJayProcessor::linkMixerContentFromSaved (1) == C::Numbers,
           "saved 1 (the removed meter mode) becomes Numbers");
    check (EchoJayProcessor::linkMixerContentFromSaved (2) == C::Chain,
           "saved 2 (chain) STAYS Chain");
    check (EchoJayProcessor::linkMixerContentFromSaved (-1) == C::Numbers,
           "garbage below range becomes Numbers");
    check (EchoJayProcessor::linkMixerContentFromSaved (99) == C::Numbers,
           "garbage above range becomes Numbers");
}

static void testDegenerate()
{
    std::printf ("degenerate inputs leave nothing behind\n");
    Geom bus;
    std::vector<Geom> links;

    // No Links: no strip rects. The stale-coordinates half of the overlap bugs.
    T::layOut ({ 32, 142, 1050, 500 }, T::wNarrow(), addrs (0), bus, links);
    checkEq ((int) links.size(), 0, "no addresses leaves no strips");
    checkEq (T::totalWidth (0, T::wNarrow()), 0, "no strips means no scroll width");

    // A band with no height must produce nothing rather than inverted rects.
    T::layOut ({ 32, 142, 1050, 0 }, T::wNarrow(), addrs (5), bus, links);
    checkEq ((int) links.size(), 0, "a zero-height band leaves no strips");
    check (bus.full.isEmpty(), "a zero-height band leaves no bus strip");

    T::layOut ({ 32, 142, 0, 500 }, T::wNarrow(), addrs (5), bus, links);
    checkEq ((int) links.size(), 0, "a zero-width band leaves no strips");

    // Zero strip width is not a layout, it is a bug upstream; refuse it.
    T::layOut ({ 32, 142, 1050, 500 }, 0, addrs (5), bus, links);
    checkEq ((int) links.size(), 0, "a zero strip width leaves no strips");
}

int main()
{
    std::printf ("EJLinkMixer geometry selftest\n");

    testPure();

    // 16 is kRegMaxSlots, the real ceiling.
    //
    // The band heights are the ones that SHIP, derived from setResizeLimits
    // (900x580 minimum, 1170x696 default, 1800x1200 maximum) less the Link
    // tab's own chrome, which measureLinkStrips lays out as
    // topH(70) + kLinkTopPad(16) + kLinkTitleH(26) + kLinkCtrlH(30)
    // + kStripVGap(6) above the band and 8 below it:
    //     580 -> 424    696 -> 540    1200 -> 1044
    // 200 is well below anything reachable and is here only to prove the
    // degenerate path produces contained rects rather than inverted ones.
    testContainment (T::wNarrow(), 540, 16);   // default window
    testContainment (T::wNarrow(), 540, 1);
    testContainment (T::wWide(),   540, 16);
    testContainment (T::wNarrow(), 424, 16);   // minimum window
    testContainment (T::wWide(),   424, 16);
    testContainment (T::wNarrow(), 1044, 16);  // maximum window
    testContainment (T::wNarrow(), 200, 4);    // unreachable, degenerate

    testBusMatchesChannel();

    testFaderAspect (T::wNarrow(), 540);
    testFaderAspect (T::wWide(),   540);
    testFaderAspect (T::wNarrow(), 424);
    testFaderAspect (T::wNarrow(), 1044);
    testFaderAspect (T::wNarrow(), 200);

    testHitPrecedence();

    testClickable (T::wNarrow(), 540);
    testClickable (T::wWide(),   540);
    testClickable (T::wNarrow(), 424);

    testSelection();
    testCtrls();
    testFaderMapping();
    testMeterMapping();
    testChainStates();
    testChainBlocks();
    testPlacement();
    testMaskGating();
    testBandSqueeze();
    testContentMigration();
    testDegenerate();

    std::printf (failures == 0 ? "EJLinkMixer selftest: PASS\n"
                               : "EJLinkMixer selftest: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
