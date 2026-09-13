#pragma once

#include <JuceHeader.h>
#include <vector>

// THE REFERENCE BROWSER'S ROWS, AS A PURE FUNCTION OF THE LIBRARY.
//
// groupChainRows' shape, and for groupChainRows' reason: the rule that decides
// what appears in each pane is STATIC and free of editor state, so
// tools/mapfps_test exercises the code that ships rather than a copy of it. The
// rendering consumes what this returns and is not pinned, because nothing in
// the gate opens a window.
//
// Header-inline, the EJParamReads.h discipline: the gate links the PREVIOUS
// build's SharedCode lib, so anything a pin exercises must live in a header the
// test TU compiles directly, or the pin measures the last build and not this
// one.
//
// COMMIT ONE OF THREE. The left pane is a single entry. Folders are commit two
// and the transport is commit three, and neither is stubbed here: an entry that
// does nothing is worse than an absent one, so the panes contain only rows that
// act.
namespace echojay {

// What the browser is shown. Deliberately NOT ReferenceResult: that carries
// MeterData, a thumbnail and a 64-bin curve, none of which a row needs, and
// depending on it would make this rule impossible to exercise without an
// analyser. Name and path are the whole input.
struct RefBrowserEntry
{
    juce::String name;
    juce::String path;
};

struct RefBrowserRow
{
    enum class Kind
    {
        Heading,   // a pane's section label. Never selectable.
        Category,  // left pane: a scope to view. Commit one has exactly one.
        Track,     // right pane: one reference. index addresses the library.
        Notice,    // a statement of fact, e.g. that the library is empty.
        Invite     // the ONE actionable row in an empty pane: add a reference.
    };

    Kind         kind      = Kind::Notice;
    juce::String text;
    // Index into the vector this was built from, for Track rows ONLY. Every
    // other kind carries -1. A Track row whose index does not address its
    // source is the bug this field exists to make checkable.
    int          index     = -1;
    bool         selected  = false;
    // Whether a click on this row does anything. Headings and notices do not.
    bool         clickable = false;
};

struct RefBrowserPanes
{
    std::vector<RefBrowserRow> left;
    std::vector<RefBrowserRow> right;
};

// The invitation's wording is SHARED WITH THE SLOT MENU, not retyped. 16ed1f4
// put "Add a reference track..." in the menu's empty REFERENCES section; the
// empty browser says the same words and runs the same function, because two
// wordings for one action is how a user learns they are two actions.
inline const char* kRefBrowserInviteText () { return "Add a reference track..."; }

/** Builds both panes.

    selectedIndex addresses the SAME vector as refs. Out of range means nothing
    is selected, which is the honest rendering of a library whose selection has
    been deleted, rather than a clamp that silently points at a neighbour.
*/
inline RefBrowserPanes buildReferenceBrowserRows (const std::vector<RefBrowserEntry>& refs,
                                                  int selectedIndex)
{
    RefBrowserPanes out;

    // ---- LEFT: scopes. One, until folders land. --------------------------
    {
        RefBrowserRow h;
        h.kind = RefBrowserRow::Kind::Heading;
        h.text = "LIBRARY";
        out.left.push_back (h);

        RefBrowserRow all;
        all.kind      = RefBrowserRow::Kind::Category;
        all.text      = "All references (" + juce::String ((int) refs.size()) + ")";
        all.selected  = true;    // the only scope there is, so always the current one
        all.clickable = true;
        out.left.push_back (all);
    }

    // ---- RIGHT: the tracks themselves. -----------------------------------
    {
        RefBrowserRow h;
        h.kind = RefBrowserRow::Kind::Heading;
        h.text = "REFERENCES";
        out.right.push_back (h);

        if (refs.empty())
        {
            RefBrowserRow n;
            n.kind = RefBrowserRow::Kind::Notice;
            n.text = "No references yet.";
            out.right.push_back (n);

            RefBrowserRow inv;
            inv.kind      = RefBrowserRow::Kind::Invite;
            inv.text      = kRefBrowserInviteText();
            inv.clickable = true;
            out.right.push_back (inv);
        }
        else
        {
            // EVERY REFERENCE, EXACTLY ONCE, IN LIBRARY ORDER. No cap. The slot
            // menu caps at 99 because its ids live in a 100-wide band and
            // overrunning it decodes as another section; a scrolling pane has
            // no such band, so importing the cap would import a constraint
            // along with the number that exists to satisfy it.
            for (int i = 0; i < (int) refs.size(); ++i)
            {
                RefBrowserRow r;
                r.kind      = RefBrowserRow::Kind::Track;
                r.text      = refs[(size_t) i].name;
                r.index     = i;
                r.selected  = (i == selectedIndex);
                r.clickable = true;
                out.right.push_back (r);
            }
        }
    }

    return out;
}

/** The name shown in the title bar. Pure, and separate from the panes so the
    bar cannot disagree with the list about what is selected. */
inline juce::String refBrowserTitle (const std::vector<RefBrowserEntry>& refs,
                                     int selectedIndex)
{
    if (selectedIndex >= 0 && selectedIndex < (int) refs.size())
        return refs[(size_t) selectedIndex].name;
    return refs.empty() ? juce::String ("No references")
                        : juce::String ("Select a reference");
}

} // namespace echojay
