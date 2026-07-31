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

    static int   frameFor (float db)                        { return Ed::faderFrameForGain (db); }
    static float gFromY (int y, juce::Rectangle<int> t)     { return Ed::gainFromY (y, t); }
    static int   yFromG (float db, juce::Rectangle<int> t)  { return Ed::yFromGain (db, t); }
    static int   frames()                                   { return Ed::kFaderFrames; }
    static int   frameH()                                   { return Ed::kFaderFrameH; }

    static int   bandGap()    { return Ed::kBandGap; }
    static int   meterWMin()  { return Ed::kMeterWMin; }
    static int   meterY (float db, juce::Rectangle<int> a) { return Ed::meterYForDb (db, a); }
    static float meterFloor()                               { return Ed::kMeterDbFloor; }
    static const float* meterMarks (int& n)
    { n = Ed::kMeterMarkCount; return Ed::kMeterMarks; }

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
             { "data", s.data }, { "fader", s.fader }, { "clip", s.clip },
             { "meter", s.meter }, { "ai", s.ai } };
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

        // Elements never overlap each other.
        auto els = elements (s);
        for (size_t x = 0; x < els.size(); ++x)
            for (size_t y = x + 1; y < els.size(); ++y)
                if (! els[x].second.isEmpty() && ! els[y].second.isEmpty())
                    check (! els[x].second.intersects (els[y].second),
                           "two elements of one strip overlap");

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
    std::printf ("  fader col %dx%d (img %d wide), meter %dx%d, data %dx%d\n",
                 f.getWidth(), f.getHeight(), f.getHeight() / 8,
                 s0.meter.getWidth(), s0.meter.getHeight(),
                 s0.data.getWidth(), s0.data.getHeight());

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
        // The implied image (height/8 wide) must FIT the column, and must
        // never upscale from the 60px source.
        check (f.getHeight() / 8 <= f.getWidth(), "the 1:8 image fits its column");
        check (f.getHeight() / 8 <= 60, "the fader never upscales from the source");
        check (f.getHeight() % 8 == 0, "column height is exactly image width * 8");

        // The split: fader column left of the meter, exactly one band gap
        // apart, never overlapping (the overlap is also held by the
        // elements() sweep in testContainment).
        checkEq (s0.meter.getX() - f.getRight(), T::bandGap(),
                 "fader column and meter sit one band gap apart");
        check (f.getBottom() <= s0.ai.getY(), "the band sits above the AI button");
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

    // Endpoints: bottom of the rect is -24, top is +12.
    check (T::gFromY (t.getBottom(), t) == -24.0f, "rect bottom reads -24 dB");
    check (T::gFromY (t.getY(), t)      ==  12.0f, "rect top reads +12 dB");
    checkEq (T::yFromG (-24.0f, t), t.getBottom(), "-24 dB sits at the rect bottom");
    checkEq (T::yFromG ( 12.0f, t), t.getY(),      "+12 dB sits at the rect top");

    // Clamping: outside the rect pins the range, never escapes it.
    check (T::gFromY (t.getBottom() + 50, t) == -24.0f, "below the rect clamps to -24");
    check (T::gFromY (t.getY() - 50, t)      ==  12.0f, "above the rect clamps to +12");

    // Round-trip within one pixel + one snap step: 36 dB over 191 px is
    // 0.188 dB per px, snap is 0.1, so 0.25 covers both.
    for (float db = -24.0f; db <= 12.01f; db += 1.7f)
    {
        const float back = T::gFromY (T::yFromG (db, t), t);
        check (std::abs (back - db) <= 0.25f, "dB<->y round-trips within tolerance");
    }

    // Frames: full range maps 0..127, monotonic, and clamped so no gain can
    // index past the strip (frame*480 + 480 <= 61440 always).
    checkEq (T::frameFor (-24.0f), 0,               "-24 dB is frame 0");
    checkEq (T::frameFor ( 12.0f), T::frames() - 1, "+12 dB is frame 127");
    checkEq (T::frameFor (-999.0f), 0,              "far below range clamps to frame 0");
    checkEq (T::frameFor ( 999.0f), T::frames() - 1,"far above range clamps to frame 127");
    int prev = -1;
    for (float db = -24.0f; db <= 12.01f; db += 0.1f)
    {
        const int f = T::frameFor (db);
        check (f >= prev, "frame index is monotonic in gain");
        check (f >= 0 && f < T::frames(), "frame index stays in [0,127]");
        prev = f;
    }
    check ((T::frames() - 1) * T::frameH() + T::frameH() <= 61440,
           "last frame's source rect stays inside the 60x61440 strip");
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
    testMaskGating();
    testBandSqueeze();
    testContentMigration();
    testDegenerate();

    std::printf (failures == 0 ? "EJLinkMixer selftest: PASS\n"
                               : "EJLinkMixer selftest: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
