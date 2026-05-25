#pragma once
#include <JuceHeader.h>
#include "PluginScanner.h"
#include "EchoJayLookAndFeel.h"
#include <functional>
#include <set>
#include <vector>

// ============================================================================
// PluginChecklistComponent
// ============================================================================
// A scrollable, collapsible list of scanned plugins with checkboxes, grouped
// by manufacturer. Used by the post-scan review popup and the Settings view.
//
// DESIGN — why this is custom-drawn, not built from ToggleButton children:
// A serious plugin library can run into the THOUSANDS (we've seen 2000+).
// Creating a live juce::ToggleButton per plugin means thousands of child
// components — slow to lay out, janky to scroll, and laggy on every click.
// Instead this component draws rows itself in paint() and hit-tests clicks in
// mouseDown(). There are no child components, so it stays instant regardless
// of library size, and it can show EVERY plugin (no cap) with collapsible
// manufacturer sections.
//
// SELECTION MODEL — local until commit:
// Ticking is a pure UI operation. The component holds its own set of disabled
// uids; clicking a row just flips a bool in that set (instant, no scanner
// touch, no disk, no network). Only when the host calls commit() (on "Done" /
// "Save") is the whole selection pushed to the scanner once. This is what
// makes ticking feel real-time even with a huge library.
class PluginChecklistComponent : public juce::Component
{
public:
    using C = EchoJayLookAndFeel::Colours;

    explicit PluginChecklistComponent(PluginScanner& scannerToUse);
    ~PluginChecklistComponent() override = default;

    // Re-read the plugin list + current enabled state from the scanner and
    // rebuild the local model. Call when (re)showing the list.
    void refresh();

    // Push the local selection to the scanner (enable/disable per uid) in one
    // batch. Call from the host on "Done" / "Save". No-op if unchanged.
    void commit();

    // True if the local selection differs from what's committed.
    bool hasUncommittedChanges() const { return dirty; }

    // Bulk actions for the header buttons. Operate on the CURRENT filtered
    // view. Local only until commit().
    void selectAllVisible(bool enabled);

    // Expand / collapse all manufacturer sections.
    void setAllCollapsed(bool collapsed);

    // Add a manual (user-typed) plugin via the scanner, then refresh.
    void addManual(const juce::String& name);

    // Search filter (matches name or manufacturer, case-insensitive). Empty
    // clears it. Cheap — just re-lays the model.
    void setFilter(const juce::String& query);

    // Counts for an optional host hint line.
    int getMatchCount() const { return matchCount; }
    int getTotalCount() const { return (int) allRows.size(); }

    // Fired on any local selection change. NOT a commit — cheap.
    std::function<void()> onChanged;

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

    int getContentHeight() const { return contentHeight; }

private:
    struct Entry
    {
        juce::String uid;
        juce::String name;
        juce::String manufacturer;
    };

    struct Group
    {
        juce::String manufacturer;
        std::vector<int> entryIndices; // into allRows
        bool collapsed = false;
    };

    struct Line
    {
        enum Kind { Header, PluginRow } kind;
        int groupIndex = -1;
        int entryIndex = -1;
        int y = 0;
        int height = 0;
    };

    PluginScanner& scanner;

    std::vector<Entry> allRows;
    std::vector<Group> groups;
    std::vector<Line>  lines;

    std::set<juce::String> disabled;
    std::set<juce::String> committedDisabled;
    bool dirty = false;

    juce::String filterQuery;
    int matchCount = 0;
    int contentHeight = 0;

    static constexpr int kRowH    = 26;
    static constexpr int kHeaderH = 24;
    static constexpr int kPadX    = 8;
    static constexpr int kCheckSz = 16;

    void rebuildModel();
    void relayout();
    bool entryMatchesFilter(const Entry&) const;
    bool isEnabled(const juce::String& uid) const { return disabled.find(uid) == disabled.end(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginChecklistComponent)
};
