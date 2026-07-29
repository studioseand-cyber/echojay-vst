/*
    Tab strip geometry self-test.

    WHY THIS EXISTS. The strip used to be laid out twice: paint() divided the
    width by a local `constexpr int kTabCount = 7`, and mouseDown() did the
    same arithmetic again eleven thousand lines away. Three things had to stay
    in step (the two counts and the assumption that the label order matches the
    Tab enum) and nothing enforced it. A refactor that silently misroutes
    clicks is worse than the duplication it replaces, so the refactor ships
    with this.

    WHAT IT CHECKS, exhaustively rather than by sampling: for every width that
    matters, every pixel column in the strip maps back to exactly the tab that
    was drawn there. That is a stronger claim than clicking each tab once,
    because it also catches gaps, overlaps and off-by-one at the seams, which
    a click test would sail past.

    IT CALLS THE SHIPPING FUNCTIONS. EchoJayEditor::computeTabRects and
    ::tabIndexIn are static and pure precisely so this file does not have to
    reimplement them. A test carrying its own copy of the arithmetic would be
    the original bug wearing a different hat.

    Widths under test: 900 is the full-mode floor (setResizeLimits) and is
    directly reachable from the window-size menu, 1170 is the default, 1400 is
    the large preset. 901 and 1399 are there because the last tab absorbs the
    integer-division remainder and the seams are where that shows up.
*/

#include "PluginEditor.h"

#include <cstdio>
#include <vector>

/** The friend declared in PluginEditor.h. Exists so the geometry helpers can
    stay private: a test is not a reason to widen a class's public surface. */
struct EchoJayTabStripTestAccess
{
    using TabRects = EchoJayEditor::TabRects;
    static constexpr int count   = EchoJayEditor::kTabCount;
    static constexpr int topBarH = EchoJayEditor::kTopBarH;
    static constexpr int tabBarH = EchoJayEditor::kTabBarH;
    static const char* name (int i)   { return EchoJayEditor::kTabNames[i]; }
    static TabRects rects (int width) { return EchoJayEditor::computeTabRects (width); }
    static int index (const TabRects& r, juce::Point<int> p)
    {
        return EchoJayEditor::tabIndexIn (r, p);
    }
};

namespace
{
using TA = EchoJayTabStripTestAccess;

int failures = 0;

void check (bool ok, const char* what, int a = 0, int b = 0)
{
    if (ok) return;
    ++failures;
    std::printf ("  FAIL  %s  (%d, %d)\n", what, a, b);
}

/** Every pixel column in [0, width) must resolve to the tab whose rect covers
    it, with no gap and no overlap, and the strip must reach both edges. */
void exerciseWidth (int width)
{
    const auto rects = TA::rects (width);
    const int  n     = TA::count;
    const int  y     = TA::topBarH + (TA::tabBarH / 2);

    check (rects[0].getX() == 0, "strip starts at x=0", rects[0].getX(), 0);
    check (rects[(size_t) (n - 1)].getRight() == width, "strip reaches the right edge",
           rects[(size_t) (n - 1)].getRight(), width);

    for (int i = 1; i < n; ++i)
        check (rects[(size_t) i].getX() == rects[(size_t) (i - 1)].getRight(),
               "tabs tile with no gap or overlap",
               rects[(size_t) (i - 1)].getRight(), rects[(size_t) i].getX());

    for (int i = 0; i < n; ++i)
        check (rects[(size_t) i].getWidth() > 0, "every tab is clickable", i,
               rects[(size_t) i].getWidth());

    // The exhaustive part: every column, not three clicks.
    std::vector<int> hits ((size_t) n, 0);
    for (int x = 0; x < width; ++x)
    {
        const int idx = TA::index (rects, { x, y });
        if (idx < 0)      { check (false, "column resolves to a tab", x, -1); continue; }
        if (! rects[(size_t) idx].contains (juce::Point<int> { x, y }))
            check (false, "resolved tab actually contains the point", x, idx);
        hits[(size_t) idx]++;
    }
    for (int i = 0; i < n; ++i)
        check (hits[(size_t) i] > 0, "every tab is reachable by a click", i, hits[(size_t) i]);

    // Above and below the strip must NOT resolve to a tab, or a click on the
    // header or on content would switch tabs.
    check (TA::index (rects, { width / 2, TA::topBarH - 1 }) < 0,
           "the header row is not part of the strip");
    check (TA::index (rects, { width / 2,
             TA::topBarH + TA::tabBarH }) < 0,
           "the content area is not part of the strip");

    std::printf ("  ok    width %4d: %d tabs, %dpx each, seams clean, all columns route\n",
                 width, n, rects[0].getWidth());
}
} // namespace

int main()
{
    std::printf ("tab strip geometry self-test (%d tabs)\n", TA::count);

    for (int w : { 900, 901, 1170, 1399, 1400, 1800 })
        exerciseWidth (w);

    // Degenerate widths must not crash or divide by zero. The editor cannot
    // reach these (the floor is 900) but paint() can run mid-resize.
    for (int w : { 0, 1, 7, 8 })
    {
        const auto rects = TA::rects (w);
        (void) TA::index (rects, { 0, TA::topBarH });
    }
    std::printf ("  ok    degenerate widths do not crash\n");

    if (failures)
    {
        std::printf ("tabstrip_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf ("tabstrip_test: PASS\n");
    return 0;
}
