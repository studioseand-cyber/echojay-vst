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

/** A folder. Membership keys on PATH because references have no ids yet; item
    137 owns the migration when they do, and this is one of the places that
    changes with it. */
struct RefFolder
{
    juce::String              name;
    std::vector<juce::String> paths;
};

/** The scope the browser and the ARROWS are working in. Empty name means ALL
    REFERENCES, which always exists and always shows everything. */
struct RefScope
{
    enum class Kind { All = 0, Folder, Unfiled };
    Kind         kind = Kind::All;
    juce::String folder;          // meaningful only for Kind::Folder
};

struct RefBrowserRow
{
    enum class Kind
    {
        Heading,     // a pane's section label. Never selectable.
        Category,    // left pane: a scope to view.
        Track,       // right pane: one reference. index addresses the library.
        Missing,     // right pane: in this folder, but its file no longer
                     // resolves. LISTED, not silently dropped, and not
                     // selectable. Same direction as the index schema's
                     // RefAvailability, where an unknown state reads as
                     // Missing rather than Present.
        Notice,      // a statement of fact, e.g. that the library is empty.
        Invite,      // the ONE actionable row in an empty pane: add a reference.
        NewFolder    // left pane: the + row that creates one.
    };

    Kind         kind      = Kind::Notice;
    juce::String text;
    // Category rows carry the scope they select; Track and Missing rows carry
    // the path they are, so a right-click menu acts on the row it was opened
    // on rather than on a re-derived index.
    RefScope     scope;
    juce::String path;
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
inline const char* kRefBrowserAllName   () { return "All references"; }
inline const char* kRefBrowserUnfiledName () { return "Unfiled"; }
inline const char* kRefBrowserNewFolderText () { return "+ New folder"; }

/** Is this path in any folder? */
inline bool refPathIsFiled (const std::vector<RefFolder>& folders, const juce::String& path)
{
    for (auto& f : folders)
        for (auto& p : f.paths)
            if (p == path) return true;
    return false;
}

/** The folder a path belongs to, or empty. ONE FOLDER EACH is the model, so the
    FIRST match wins and that is a decision, not an accident: if a path somehow
    appears in two folders, it shows in the earlier one rather than in both,
    because a reference appearing twice is worse than it appearing in the
    wrong one. */
inline juce::String refFolderOf (const std::vector<RefFolder>& folders, const juce::String& path)
{
    for (auto& f : folders)
        for (auto& p : f.paths)
            if (p == path) return f.name;
    return {};
}

/** Does a scope admit this reference? */
inline bool refScopeAdmits (const RefScope& scope,
                            const std::vector<RefFolder>& folders,
                            const juce::String& path)
{
    switch (scope.kind)
    {
        case RefScope::Kind::All:     return true;
        case RefScope::Kind::Unfiled: return ! refPathIsFiled (folders, path);
        case RefScope::Kind::Folder:  return refFolderOf (folders, path) == scope.folder;
    }
    return true;
}

/** How many references a scope holds. THIS IS THE COUNT THE ARROWS STEP
    THROUGH, so it is defined here beside the rule that filters the pane rather
    than recounted at the bar, where it could disagree with what is shown.

    MISSING PATHS ARE NOT COUNTED. They are listed, because a folder that
    silently forgets a track is worse than one showing an unavailable row, but
    they cannot be stepped onto: there is nothing to audition. */
inline int refScopeCount (const std::vector<RefBrowserEntry>& refs,
                          const std::vector<RefFolder>& folders,
                          const RefScope& scope)
{
    int n = 0;
    for (auto& r : refs)
        if (refScopeAdmits (scope, folders, r.path)) ++n;
    return n;
}

/** The library index of the nth reference WITHIN a scope, or -1.

    The arrows step 0..refScopeCount-1 and this turns that into the index
    applyReferenceToSlot wants. Without it the bar would have to filter the
    library a second time and the two filters could drift. */
inline int refScopeIndexToLibrary (const std::vector<RefBrowserEntry>& refs,
                                   const std::vector<RefFolder>& folders,
                                   const RefScope& scope,
                                   int nth)
{
    if (nth < 0) return -1;
    int n = 0;
    for (int i = 0; i < (int) refs.size(); ++i)
        if (refScopeAdmits (scope, folders, refs[(size_t) i].path))
            if (n++ == nth) return i;
    return -1;
}

/** And back again: where a library index sits within its scope, or -1. */
inline int refLibraryIndexToScope (const std::vector<RefBrowserEntry>& refs,
                                   const std::vector<RefFolder>& folders,
                                   const RefScope& scope,
                                   int libraryIndex)
{
    if (libraryIndex < 0 || libraryIndex >= (int) refs.size()) return -1;
    if (! refScopeAdmits (scope, folders, refs[(size_t) libraryIndex].path)) return -1;
    int n = 0;
    for (int i = 0; i < libraryIndex; ++i)
        if (refScopeAdmits (scope, folders, refs[(size_t) i].path)) ++n;
    return n;
}

/** How the bar names the scope. EMPTY FOR ALL REFERENCES: that is the default
    and a chip saying so would be noise on every bar. */
inline juce::String refScopeChipText (const RefScope& scope)
{
    switch (scope.kind)
    {
        case RefScope::Kind::All:     return {};
        case RefScope::Kind::Unfiled: return juce::String (kRefBrowserUnfiledName()).toUpperCase();
        case RefScope::Kind::Folder:  return scope.folder.toUpperCase();
    }
    return {};
}

/** A scope that no longer exists falls back to ALL rather than showing an empty
    pane the user cannot account for. Deleting a folder while it is selected is
    the ordinary way this happens. */
inline RefScope refScopeOrAll (const RefScope& scope, const std::vector<RefFolder>& folders)
{
    if (scope.kind != RefScope::Kind::Folder) return scope;
    for (auto& f : folders)
        if (f.name == scope.folder) return scope;
    return {};
}

/** The name shown in the title bar. Pure, and separate from the panes so the
    bar cannot disagree with the list about what is selected. */
/** Builds both panes.

    selectedIndex is a LIBRARY index, not a scope index: it is what the slots
    and applyReferenceToSlot use, and translating at the boundary rather than
    storing a scope-relative number means a scope change cannot silently
    repoint the selection at a different reference.
*/
inline RefBrowserPanes buildReferenceBrowserRows (const std::vector<RefBrowserEntry>& refs,
                                                  const std::vector<RefFolder>& folders,
                                                  const RefScope& scopeIn,
                                                  int selectedIndex)
{
    RefBrowserPanes out;
    const RefScope scope = refScopeOrAll (scopeIn, folders);

    // ---- LEFT: the scopes. -----------------------------------------------
    {
        RefBrowserRow h;
        h.kind = RefBrowserRow::Kind::Heading;
        h.text = "LIBRARY";
        out.left.push_back (h);

        RefBrowserRow all;
        all.kind      = RefBrowserRow::Kind::Category;
        all.text      = juce::String (kRefBrowserAllName()) + " ("
                      + juce::String ((int) refs.size()) + ")";
        all.scope     = {};
        all.selected  = (scope.kind == RefScope::Kind::All);
        all.clickable = true;
        out.left.push_back (all);

        // FOLDERS IN CREATION ORDER, not sorted: a list that reorders itself
        // when a folder is renamed loses the user's place.
        for (auto& f : folders)
        {
            RefScope fs; fs.kind = RefScope::Kind::Folder; fs.folder = f.name;
            RefBrowserRow r;
            r.kind      = RefBrowserRow::Kind::Category;
            r.text      = f.name + " (" + juce::String (refScopeCount (refs, folders, fs)) + ")";
            r.scope     = fs;
            r.selected  = (scope.kind == RefScope::Kind::Folder && scope.folder == f.name);
            r.clickable = true;
            out.left.push_back (r);
        }

        // UNFILED APPEARS ONLY WHEN SOMETHING IS UNFILED. A permanent empty
        // Unfiled row tells a user with no folders that they have a filing
        // system they have never used.
        RefScope us; us.kind = RefScope::Kind::Unfiled;
        const int unfiled = refScopeCount (refs, folders, us);
        if (unfiled > 0 && ! folders.empty())
        {
            RefBrowserRow r;
            r.kind      = RefBrowserRow::Kind::Category;
            r.text      = juce::String (kRefBrowserUnfiledName()) + " ("
                        + juce::String (unfiled) + ")";
            r.scope     = us;
            r.selected  = (scope.kind == RefScope::Kind::Unfiled);
            r.clickable = true;
            out.left.push_back (r);
        }

        RefBrowserRow nf;
        nf.kind      = RefBrowserRow::Kind::NewFolder;
        nf.text      = kRefBrowserNewFolderText();
        nf.clickable = true;
        out.left.push_back (nf);
    }

    // ---- RIGHT: the tracks in the selected scope. ------------------------
    {
        RefBrowserRow h;
        h.kind = RefBrowserRow::Kind::Heading;
        h.text = scope.kind == RefScope::Kind::All
                   ? juce::String ("REFERENCES") : refScopeChipText (scope);
        out.right.push_back (h);

        int shown = 0;
        for (int i = 0; i < (int) refs.size(); ++i)
        {
            if (! refScopeAdmits (scope, folders, refs[(size_t) i].path)) continue;
            RefBrowserRow r;
            r.kind      = RefBrowserRow::Kind::Track;
            r.text      = refs[(size_t) i].name;
            r.path      = refs[(size_t) i].path;
            r.index     = i;
            r.selected  = (i == selectedIndex);
            r.clickable = true;
            out.right.push_back (r);
            ++shown;
        }

        // MEMBERS WHOSE FILE IS GONE ARE STILL LISTED. The restore path drops
        // a reference whose file no longer resolves
        // (PluginProcessor.cpp:4344), so a folder can outlive its reference
        // entirely. Dropping the row too would let a folder quietly forget a
        // track; an unavailable row says what happened. Not selectable and
        // NOT COUNTED: there is nothing to audition.
        if (scope.kind == RefScope::Kind::Folder)
            for (auto& f : folders)
            {
                if (f.name != scope.folder) continue;
                for (auto& pth : f.paths)
                {
                    bool live = false;
                    for (auto& r : refs) if (r.path == pth) { live = true; break; }
                    if (live) continue;
                    RefBrowserRow r;
                    r.kind = RefBrowserRow::Kind::Missing;
                    r.text = juce::File (pth).getFileName() + "  (unavailable)";
                    r.path = pth;
                    out.right.push_back (r);
                }
            }

        if (shown == 0)
        {
            RefBrowserRow n;
            n.kind = RefBrowserRow::Kind::Notice;
            // AN EMPTY FOLDER NAMES ITSELF rather than claiming there are no
            // references. There are; they are elsewhere.
            n.text = scope.kind == RefScope::Kind::All
                       ? juce::String ("No references yet.")
                       : "Nothing in " + (scope.kind == RefScope::Kind::Unfiled
                                            ? juce::String (kRefBrowserUnfiledName())
                                            : scope.folder) + " yet.";
            out.right.push_back (n);

            // The invitation belongs to the LIBRARY, not to a folder: adding a
            // track from inside Drums would not put it in Drums, and an
            // affordance that does something other than it says is the defect
            // this project keeps removing.
            if (scope.kind == RefScope::Kind::All)
            {
                RefBrowserRow inv;
                inv.kind      = RefBrowserRow::Kind::Invite;
                inv.text      = kRefBrowserInviteText();
                inv.clickable = true;
                out.right.push_back (inv);
            }
        }
    }

    return out;
}

inline const char* kRefBrowserScopeName () { return "ALL REFERENCES"; }

/** The title bar: scope then selection. THE SCOPE IS REAL NOW. It was the
    constant "ALL REFERENCES" while there was only one scope; with folders it
    is whichever scope the panes are showing, which is what makes the pairing
    load-bearing rather than decorative. */
inline juce::String refBrowserScopeTitle (const RefScope& scope)
{
    const auto chip = refScopeChipText (scope);
    return chip.isEmpty() ? juce::String (kRefBrowserScopeName()) : chip;
}

inline juce::String refBrowserTitle (const std::vector<RefBrowserEntry>& refs,
                                     const RefScope& scope,
                                     int selectedIndex)
{
    // BOTH PARTS, SCOPE THEN SELECTION. A title naming only the selection
    // cannot say which library it came out of, and with folders that is the
    // difference between "Drums - kick.wav" and "ALL REFERENCES - kick.wav".
    if (selectedIndex >= 0 && selectedIndex < (int) refs.size())
        return refBrowserScopeTitle (scope) + " - " + refs[(size_t) selectedIndex].name;
    // THE EMPTY CASES ARE UNCHANGED, and rb PIN8 and rb PIN4 pin both strings.
    return refs.empty() ? juce::String ("No references")
                        : juce::String ("Select a reference");
}

} // namespace echojay
