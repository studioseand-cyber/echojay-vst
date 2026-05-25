#include "PluginChecklist.h"
#include <algorithm>
#include <map>

PluginChecklistComponent::PluginChecklistComponent(PluginScanner& scannerToUse)
    : scanner(scannerToUse)
{
    rebuildModel();
    relayout();
}

// ---- Model -----------------------------------------------------------------

void PluginChecklistComponent::rebuildModel()
{
    allRows.clear();
    groups.clear();
    disabled.clear();

    auto all = scanner.getPlugins();

    // Collect effect plugins into allRows, capturing current enabled state.
    std::map<juce::String, std::vector<int>> byManu;
    for (auto& p : all)
    {
        if (p.category != "Effect") continue;
        int idx = (int) allRows.size();
        allRows.push_back({ p.uid, p.name, p.manufacturer });
        if (! p.enabled)
            disabled.insert(p.uid);
        byManu[p.manufacturer].push_back(idx);
    }

    // Build groups. Order: Waves and the *Stock groups first (the ones the
    // user most likely needs to prune), then alphabetical.
    auto rank = [](const juce::String& m) -> int
    {
        if (m == "Waves") return 0;
        if (m.endsWithIgnoreCase("Stock")) return 1;
        if (m == "Custom") return 2;
        return 3;
    };
    std::vector<juce::String> manus;
    for (auto& kv : byManu) manus.push_back(kv.first);
    std::sort(manus.begin(), manus.end(),
              [&](const juce::String& a, const juce::String& b)
              {
                  int ra = rank(a), rb = rank(b);
                  if (ra != rb) return ra < rb;
                  return a.compareIgnoreCase(b) < 0;
              });

    for (auto& m : manus)
    {
        Group g;
        g.manufacturer = m;
        g.entryIndices = byManu[m];
        // Sort entries within the group by name.
        std::sort(g.entryIndices.begin(), g.entryIndices.end(),
                  [this](int a, int b)
                  { return allRows[(size_t) a].name.compareIgnoreCase(allRows[(size_t) b].name) < 0; });
        // Default collapsed when the library is large, so the user sees a
        // navigable list of sections rather than thousands of rows. Small
        // libraries start expanded.
        g.collapsed = (allRows.size() > 60);
        groups.push_back(std::move(g));
    }

    committedDisabled = disabled;
    dirty = false;
}

bool PluginChecklistComponent::entryMatchesFilter(const Entry& e) const
{
    if (filterQuery.isEmpty()) return true;
    auto q = filterQuery;
    return e.name.containsIgnoreCase(q) || e.manufacturer.containsIgnoreCase(q);
}

void PluginChecklistComponent::relayout()
{
    lines.clear();
    matchCount = 0;

    auto q = filterQuery.trim();
    const bool filtering = q.isNotEmpty();

    int y = 0;
    for (int gi = 0; gi < (int) groups.size(); ++gi)
    {
        auto& g = groups[(size_t) gi];

        // Determine which entries in this group match the filter.
        std::vector<int> matching;
        for (int ei : g.entryIndices)
            if (entryMatchesFilter(allRows[(size_t) ei]))
                matching.push_back(ei);

        if (matching.empty()) continue; // hide empty groups entirely
        matchCount += (int) matching.size();

        // Header line.
        lines.push_back({ Line::Header, gi, -1, y, kHeaderH });
        y += kHeaderH;

        // When filtering, force groups open so matches are visible regardless
        // of collapse state. Otherwise honour the group's collapsed flag.
        const bool showRows = filtering ? true : ! g.collapsed;
        if (showRows)
        {
            for (int ei : matching)
            {
                lines.push_back({ Line::PluginRow, gi, ei, y, kRowH });
                y += kRowH;
            }
        }
    }

    contentHeight = y;
    setSize(getWidth() > 0 ? getWidth() : 400, contentHeight);
}

// ---- Public API ------------------------------------------------------------

void PluginChecklistComponent::refresh()
{
    rebuildModel();
    relayout();
    repaint();
}

void PluginChecklistComponent::commit()
{
    if (! dirty) return;

    // Diff against the committed snapshot and push only the changes to the
    // scanner in two batches (one enable, one disable). This is the only time
    // we touch the scanner / disk / server — once, on Done.
    juce::StringArray toDisable, toEnable;
    for (auto& e : allRows)
    {
        bool nowDisabled = disabled.count(e.uid) > 0;
        bool wasDisabled = committedDisabled.count(e.uid) > 0;
        if (nowDisabled && ! wasDisabled) toDisable.add(e.uid);
        else if (! nowDisabled && wasDisabled) toEnable.add(e.uid);
    }
    if (! toDisable.isEmpty()) scanner.setManyEnabled(toDisable, false);
    if (! toEnable.isEmpty())  scanner.setManyEnabled(toEnable, true);

    committedDisabled = disabled;
    dirty = false;
}

void PluginChecklistComponent::selectAllVisible(bool enabled)
{
    auto q = filterQuery.trim();
    const bool filtering = q.isNotEmpty();
    for (auto& e : allRows)
    {
        if (filtering && ! entryMatchesFilter(e)) continue;
        if (enabled) disabled.erase(e.uid);
        else         disabled.insert(e.uid);
    }
    dirty = (disabled != committedDisabled);
    relayout();
    repaint();
    if (onChanged) onChanged();
}

void PluginChecklistComponent::setAllCollapsed(bool collapsed)
{
    for (auto& g : groups) g.collapsed = collapsed;
    relayout();
    repaint();
}

void PluginChecklistComponent::addManual(const juce::String& name)
{
    scanner.addManualPlugin(name);
    refresh();
    if (onChanged) onChanged();
}

void PluginChecklistComponent::setFilter(const juce::String& query)
{
    filterQuery = query.trim();
    relayout();
    repaint();
}

// ---- Layout / paint / input -------------------------------------------------

void PluginChecklistComponent::resized()
{
    // Width changed; height is driven by content. Re-assert size.
    setSize(getWidth(), contentHeight);
}

void PluginChecklistComponent::paint(juce::Graphics& g)
{
    int w = getWidth();

    // Only paint lines within the visible clip for speed on long lists.
    auto clip = g.getClipBounds();

    for (auto& ln : lines)
    {
        if (ln.y + ln.height < clip.getY() || ln.y > clip.getBottom())
            continue; // off-screen

        if (ln.kind == Line::Header)
        {
            auto& grp = groups[(size_t) ln.groupIndex];
            // Header background.
            g.setColour(C::bg3);
            g.fillRect(0, ln.y, w, kHeaderH);

            // Disclosure triangle.
            juce::Path tri;
            float cx = (float) kPadX + 5.0f;
            float cy = (float) ln.y + kHeaderH * 0.5f;
            if (grp.collapsed)
                tri.addTriangle(cx - 3, cy - 4, cx + 3, cy, cx - 3, cy + 4);   // ▶
            else
                tri.addTriangle(cx - 4, cy - 3, cx + 4, cy - 3, cx, cy + 4);   // ▼
            g.setColour(C::text2);
            g.fillPath(tri);

            // Count of enabled/total in this group.
            int total = (int) grp.entryIndices.size();
            int on = 0;
            for (int ei : grp.entryIndices)
                if (isEnabled(allRows[(size_t) ei].uid)) ++on;

            // Group checkbox on the right: ticked if ALL on, empty if all off,
            // a dash if mixed. One click toggles the whole manufacturer.
            int gboxX = w - kPadX - kCheckSz;
            int gboxY = ln.y + (kHeaderH - kCheckSz) / 2;
            juce::Rectangle<float> gbox((float) gboxX, (float) gboxY,
                                        (float) kCheckSz, (float) kCheckSz);
            bool allOn  = (on == total && total > 0);
            bool allOff = (on == 0);
            g.setColour(allOn ? C::blue : C::bg4);
            g.fillRoundedRectangle(gbox, 3.0f);
            g.setColour(allOn ? C::blue : C::border2);
            g.drawRoundedRectangle(gbox, 3.0f, 1.0f);
            if (allOn)
            {
                juce::Path check;
                check.startNewSubPath(gbox.getX() + 3.5f, gbox.getCentreY());
                check.lineTo(gbox.getX() + 6.5f, gbox.getBottom() - 4.0f);
                check.lineTo(gbox.getRight() - 3.5f, gbox.getY() + 4.0f);
                g.setColour(juce::Colours::white);
                g.strokePath(check, juce::PathStrokeType(2.0f));
            }
            else if (! allOff)
            {
                // Mixed: draw a dash.
                g.setColour(C::text2);
                g.fillRect(gbox.getX() + 3.0f, gbox.getCentreY() - 1.0f,
                           gbox.getWidth() - 6.0f, 2.0f);
            }

            g.setColour(C::text);
            g.setFont(juce::Font(juce::FontOptions(12.5f)).boldened());
            g.drawText(grp.manufacturer,
                       kPadX + 16, ln.y, w - kPadX * 2 - 90, kHeaderH,
                       juce::Justification::centredLeft);

            g.setColour(C::text3);
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText(juce::String(on) + "/" + juce::String(total),
                       w - kCheckSz - kPadX - 56, ln.y, 50, kHeaderH,
                       juce::Justification::centredRight);
        }
        else // PluginRow
        {
            auto& e = allRows[(size_t) ln.entryIndex];
            bool on = isEnabled(e.uid);

            // Checkbox.
            int boxX = kPadX + 18;
            int boxY = ln.y + (kRowH - kCheckSz) / 2;
            juce::Rectangle<float> box((float) boxX, (float) boxY, (float) kCheckSz, (float) kCheckSz);
            g.setColour(on ? C::blue : C::bg3);
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(on ? C::blue : C::border2);
            g.drawRoundedRectangle(box, 3.0f, 1.0f);
            if (on)
            {
                // Tick mark.
                juce::Path check;
                check.startNewSubPath(box.getX() + 3.5f, box.getCentreY());
                check.lineTo(box.getX() + 6.5f, box.getBottom() - 4.0f);
                check.lineTo(box.getRight() - 3.5f, box.getY() + 4.0f);
                g.setColour(juce::Colours::white);
                g.strokePath(check, juce::PathStrokeType(2.0f));
            }

            // Name.
            g.setColour(on ? C::text : C::text3);
            g.setFont(juce::Font(juce::FontOptions(13.0f)));
            g.drawText(e.name,
                       boxX + kCheckSz + 8, ln.y, w - (boxX + kCheckSz + 8) - kPadX, kRowH,
                       juce::Justification::centredLeft);
        }
    }
}

void PluginChecklistComponent::mouseDown(const juce::MouseEvent& ev)
{
    int my = ev.y;
    for (auto& ln : lines)
    {
        if (my < ln.y || my >= ln.y + ln.height) continue;

        if (ln.kind == Line::Header)
        {
            auto& grp = groups[(size_t) ln.groupIndex];

            // Right-side checkbox zone toggles the WHOLE manufacturer on/off.
            // If the group is fully enabled, this disables all; otherwise it
            // enables all (so a mixed or all-off group becomes all-on first).
            int gboxX = getWidth() - kPadX - kCheckSz;
            if (ev.x >= gboxX - 4) // small margin for easier hits
            {
                int total = (int) grp.entryIndices.size();
                int on = 0;
                for (int ei : grp.entryIndices)
                    if (isEnabled(allRows[(size_t) ei].uid)) ++on;
                bool enableAll = ! (on == total && total > 0); // all-on -> turn off
                for (int ei : grp.entryIndices)
                {
                    auto& uid = allRows[(size_t) ei].uid;
                    if (enableAll) disabled.erase(uid);
                    else           disabled.insert(uid);
                }
                dirty = (disabled != committedDisabled);
                repaint(); // counts + rows in this group change
                if (onChanged) onChanged();
                return;
            }

            // Otherwise: toggle collapse for this group.
            grp.collapsed = ! grp.collapsed;
            relayout();
            repaint();
        }
        else // PluginRow — toggle enabled (local only, instant)
        {
            auto& e = allRows[(size_t) ln.entryIndex];
            if (disabled.count(e.uid)) disabled.erase(e.uid);
            else                       disabled.insert(e.uid);
            dirty = (disabled != committedDisabled);
            // Repaint just this row's band for snappiness.
            repaint(0, ln.y, getWidth(), ln.height);
            if (onChanged) onChanged();
        }
        return;
    }
}
