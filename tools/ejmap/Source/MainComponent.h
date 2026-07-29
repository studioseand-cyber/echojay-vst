/*
  MainComponent.h

  M1 shell. Scan, pick, load, show the editor, record the outcome.

  This is deliberately plain. The keyboard-driven mapping UI is M4 and replaces
  the right-hand side entirely. What has to be right here is the loop that gets a
  plugin editor on screen without losing the session when one of them dies.
*/

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginScanner.h"
#include "PluginHost.h"
#include "EjmapLedger.h"
#include "EjmapWatchdog.h"

namespace ejmap
{

class MainComponent  : public juce::Component,
                       private juce::ListBoxModel,
                       private juce::Timer
{
public:
    MainComponent()
        : watchdog (ledger), host (scanner.getFormatManager())
    {
        // Crash recovery runs before anything else touches a plugin.
        crashedId = ledger.recoverFromCrash();

        addAndMakeVisible (scanButton);
        scanButton.setButtonText ("Scan");
        scanButton.onClick = [this] { runScan(); };

        addAndMakeVisible (loadButton);
        loadButton.setButtonText ("Load selected");
        loadButton.setEnabled (false);
        loadButton.onClick = [this] { loadSelected(); };

        addAndMakeVisible (filterBox);
        filterBox.setTextToShowWhenEmpty ("Filter by name or vendor",
                                          juce::Colour (0xff5a6b74));
        filterBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff161c26));
        filterBox.setColour (juce::TextEditor::textColourId,       juce::Colour (0xff9fd8e0));
        filterBox.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff2a3540));
        filterBox.onTextChange = [this] { applyFilter(); };

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (22);

        addAndMakeVisible (status);
        status.setJustificationType (juce::Justification::centredLeft);
        status.setText (crashedId.isEmpty()
                          ? "Ready. Scan to enumerate installed plugins."
                          : "Previous session died loading " + crashedId
                              + ". Marked crash_on_load and quarantined.",
                        juce::dontSendNotification);

        addAndMakeVisible (summaryButton);
        summaryButton.setButtonText ("Summary");
        summaryButton.onClick = [this] { writeRunSummary(); };

        addAndMakeVisible (editorHolder);

        setSize (1280, 820);
        startTimerHz (4);
    }

    ~MainComponent() override
    {
        host.unload();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        auto top = r.removeFromTop (28);
        scanButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (6);
        loadButton.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (6);
        summaryButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (12);
        status.setBounds (top);

        r.removeFromTop (8);
        auto left = r.removeFromLeft (420);
        filterBox.setBounds (left.removeFromTop (24));
        left.removeFromTop (4);
        list.setBounds (left);
        r.removeFromLeft (8);
        editorHolder.setBounds (r);
        layoutEditor();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff10141c));   // dark navy, house style
    }

private:
    //==========================================================================
    /** Re-entrancy guard.

        The editor-ready wait pumps the message loop, which is required (a
        bridged editor connects on the message thread) but leaves every control
        live while load() is still on the stack. A second click would re-enter
        loadSelected, call host.unload(), and destroy the editor the OUTER
        load() is still holding a pointer to.

        Two layers on purpose. Disabling the controls is what the human sees;
        the flag is what actually holds, because a double-click, a keyboard
        shortcut or a future call site does not have to go through a button.
    */
    struct BusyScope
    {
        explicit BusyScope (MainComponent& o) : owner (o)
        {
            owner.busy = true;
            owner.scanButton.setEnabled (false);
            owner.loadButton.setEnabled (false);
            owner.list.setEnabled (false);
            owner.filterBox.setEnabled (false);
        }

        ~BusyScope()
        {
            owner.busy = false;
            owner.scanButton.setEnabled (true);
            owner.list.setEnabled (true);
            owner.filterBox.setEnabled (true);
            owner.loadButton.setEnabled (owner.list.getSelectedRow() >= 0);
        }

        MainComponent& owner;
        JUCE_DECLARE_NON_COPYABLE (BusyScope)
    };

    void runScan()
    {
        if (busy)
            return;

        BusyScope guard (*this);
        status.setText ("Scanning...", juce::dontSendNotification);

        lastScan = scanner.scan (ledger, watchdog);

        // Quarantined plugins stay in the list but are not loadable. Hiding them
        // would make a crash look like an uninstall.
        rows.clear();
        for (const auto& p : lastScan.plugins)
            rows.add (p);

        // Rebuilds the DISPLAYED set only. lastScan and rows keep the whole
        // result, so every census number below counts what was scanned, not
        // what happens to be on screen.
        applyFilter();

        juce::String msg;
        msg << rows.size() << " effects, " << lastScan.distinctProducts << " distinct products; "
            << "AU walk saw " << lastScan.auEnumerated
            << " (Apple " << lastScan.auExcludedApple
            << ", EchoJay " << lastScan.auExcludedEcho << " excluded); "
            << lastScan.instrumentsFiltered << " instruments, "
            << lastScan.unusedTypeFiltered << " unmapped types dropped; "
            << "VST3 probed " << lastScan.vst3Probed
            << (lastScan.vst3Quarantined > 0
                  ? ", " + juce::String (lastScan.vst3Quarantined) + " quarantined"
                  : juce::String())
            << "; " << lastScan.errors.size() << " enumeration errors";

        status.setText (msg, juce::dontSendNotification);

        // Every enumeration error is written, never counted and discarded. So
        // is every drop: the by-type breakdown is what makes "1419 seen, 1355
        // listed" a statement rather than a discrepancy.
        juce::StringArray report;
        report.add ("au_enumerated=" + juce::String (lastScan.auEnumerated));
        report.add ("au_excluded_apple=" + juce::String (lastScan.auExcludedApple));
        report.add ("au_excluded_echojay=" + juce::String (lastScan.auExcludedEcho));
        report.add ("au_unparsed_identifier=" + juce::String (lastScan.auUnparsed));
        report.add ("instruments_filtered=" + juce::String (lastScan.instrumentsFiltered));
        report.add ("unused_types_filtered=" + juce::String (lastScan.unusedTypeFiltered));
        report.add ("vst3_probed=" + juce::String (lastScan.vst3Probed));
        report.add ("vst3_quarantined_skipped=" + juce::String (lastScan.vst3Quarantined));
        report.add ("run_id=" + ledger.currentRunId());
        report.add ("listed=" + juce::String (rows.size()));
        for (const auto& [typeCode, n] : lastScan.droppedByType)
            report.add ("dropped[" + typeCode + "]=" + juce::String (n));

        // Per-run filenames. These used to be replaced on every scan while the
        // ledger appended, so the three artifacts describing one scan disagreed
        // by construction and the logs answered for whichever run went last.
        ledger.runArtifact ("scan-census", "log")
              .replaceWithText (report.joinIntoString ("\n") + "\n");

        if (! lastScan.errors.isEmpty())
        {
            ledger.runArtifact ("scan-errors", "log")
                  .replaceWithText (lastScan.errors.joinIntoString ("\n") + "\n");
        }
    }

    /** M1's "counts from disk" row. Reads the ledger back off the file through
        getOutcomeCounts, scoped to THIS run, and writes what it found. Never an
        in-memory tally: that is the rule this project adopted after the tripwire
        bug survived its own test.
    */
    void writeRunSummary()
    {
        juce::StringArray out;
        out.add ("run_id=" + ledger.currentRunId());

        for (const auto* stage : { "load", "scan" })
        {
            auto& counts = ledger.getOutcomeCounts (stage, ledger.currentRunId());
            int total = 0;
            juce::StringArray parts;
            for (juce::HashMap<juce::String, int>::Iterator it (counts); it.next();)
            {
                parts.add (juce::String (it.getKey()) + "=" + juce::String (it.getValue()));
                total += it.getValue();
            }
            parts.sort (true);
            out.add (juce::String (stage) + "_total=" + juce::String (total));
            for (const auto& p : parts)
                out.add ("  " + juce::String (stage) + "." + p);
        }

        out.add ("--- every run on disk ---");
        for (const auto& r : ledger.listRuns())
            out.add (r.runId + "  rows=" + juce::String (r.rowCount) + "  first_at=" + r.startedAt);

        auto f = ledger.runArtifact ("run-summary", "log");
        f.replaceWithText (out.joinIntoString ("\n") + "\n");
        status.setText ("Run summary written to " + f.getFileName(), juce::dontSendNotification);
    }

    /** Case-insensitive substring over name and vendor. Rebuilds visibleRows,
        an index list into rows; rows itself is never touched, so a reference
        taken into it by a load in progress stays valid.

        Selection is preserved by plugin id rather than by row index, because
        the index means something different after every keystroke and Load
        selected acts on it.
    */
    void applyFilter()
    {
        juce::String previouslySelected;
        const int sel = list.getSelectedRow();
        if (juce::isPositiveAndBelow (sel, visibleRows.size()))
            previouslySelected = rows.getReference (visibleRows[sel]).pluginId();

        const auto needle = filterBox.getText().trim();

        visibleRows.clearQuick();
        for (int i = 0; i < rows.size(); ++i)
        {
            const auto& d = rows.getReference (i).desc;
            if (needle.isEmpty()
                 || d.name.containsIgnoreCase (needle)
                 || d.manufacturerName.containsIgnoreCase (needle))
                visibleRows.add (i);
        }

        list.updateContent();

        int restored = -1;
        if (previouslySelected.isNotEmpty())
            for (int r = 0; r < visibleRows.size(); ++r)
                if (rows.getReference (visibleRows[r]).pluginId() == previouslySelected)
                    { restored = r; break; }

        if (restored >= 0) list.selectRow (restored, false, true);
        else               list.deselectAllRows();

        loadButton.setEnabled (! busy && list.getSelectedRow() >= 0);
        list.repaint();
    }

    void loadSelected()
    {
        // A nested call during the editor-ready wait is a no-op, not a crash.
        if (busy)
            return;

        BusyScope guard (*this);

        const int row = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, visibleRows.size()))
            return;

        // The selected row indexes the FILTERED view, not the scan result.
        const auto& sp = rows.getReference (visibleRows[row]);
        const auto id  = sp.pluginId();

        if (ledger.isQuarantined (id))
        {
            status.setText (id + " is quarantined. Release it explicitly to retry.",
                            juce::dontSendNotification);
            return;
        }

        detachEditor();
        host.unload();

        ledger.beginLoad (id, sp.desc.name, sp.desc.manufacturerName,
                          sp.desc.pluginFormatName, sp.desc.version,
                          "load", "createPluginInstance");

        const auto t0 = juce::Time::getMillisecondCounter();
        auto result = host.load (sp.desc, watchdog);
        const auto elapsed = juce::Time::getMillisecondCounter() - t0;

        LedgerRecord rec;
        rec.pluginId   = id;
        rec.name       = sp.desc.name;
        rec.vendor     = sp.desc.manufacturerName;
        rec.format     = sp.desc.pluginFormatName;
        rec.version    = sp.desc.version;
        rec.outcome    = result.outcome;
        rec.detail     = result.detail;
        rec.paramCount = result.paramCount;
        ledger.endLoad (rec);

        if (result.outcome == LoadOutcome::ok)
        {
            attachEditor();
            status.setText (sp.desc.name + ": " + juce::String (result.paramCount)
                              + " params, editor open in " + juce::String (elapsed) + " ms",
                            juce::dontSendNotification);
        }
        else
        {
            status.setText (sp.desc.name + ": " + toString (result.outcome)
                              + (result.detail.isEmpty() ? "" : " (" + result.detail + ")"),
                            juce::dontSendNotification);
        }
    }

    //==========================================================================
    void attachEditor()
    {
        hostedEditor = host.releaseEditor();
        if (hostedEditor != nullptr)
        {
            editorHolder.addAndMakeVisible (hostedEditor.get());
            layoutEditor();
        }
    }

    void detachEditor()
    {
        if (hostedEditor != nullptr)
        {
            editorHolder.removeChildComponent (hostedEditor.get());
            hostedEditor.reset();
        }
    }

    void layoutEditor()
    {
        if (hostedEditor == nullptr)
            return;

        // Never scale the editor. M2 records mouse position inside these bounds
        // and a transform would make ui_hint coordinates lie.
        hostedEditor->setTopLeftPosition (0, 0);
    }

    //==========================================================================
    int getNumRows() override { return visibleRows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, visibleRows.size()))
            return;

        const auto& sp = rows.getReference (visibleRows[row]);
        const bool quarantined = ledger.isQuarantined (sp.pluginId());

        if (selected)
            g.fillAll (juce::Colour (0xff1d3a44));

        g.setColour (quarantined ? juce::Colours::orangered
                                 : juce::Colour (0xff9fd8e0));
        g.setFont (13.0f);
        g.drawText (sp.desc.name + "   [" + sp.desc.pluginFormatName + "]"
                      + (quarantined ? "  QUARANTINED" : ""),
                    6, 0, w - 12, h, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int) override
    {
        loadButton.setEnabled (list.getSelectedRow() >= 0);
    }

    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { loadSelected(); }

    void timerCallback() override
    {
        // The editor can resize itself at any time. Follow it rather than
        // constraining it.
        if (hostedEditor != nullptr)
        {
            const auto b = hostedEditor->getBounds();
            if (b != lastEditorBounds)
            {
                lastEditorBounds = b;
                layoutEditor();
            }
        }
    }

    //==========================================================================
    Ledger        ledger;
    Watchdog      watchdog;
    PluginScanner scanner;
    PluginHost    host;

    PluginScanner::Result lastScan;
    juce::Array<ScannedPlugin> rows;        // the whole scan result, never filtered
    juce::Array<int>           visibleRows; // indices into rows, what the list shows
    juce::String crashedId;

    juce::TextButton scanButton, loadButton, summaryButton;
    bool busy = false;
    juce::Label      status;
    juce::TextEditor filterBox;
    juce::ListBox    list;
    juce::Component  editorHolder;
    std::unique_ptr<juce::AudioProcessorEditor> hostedEditor;
    juce::Rectangle<int> lastEditorBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace ejmap
