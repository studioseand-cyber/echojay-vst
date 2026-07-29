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
        top.removeFromLeft (12);
        status.setBounds (top);

        r.removeFromTop (8);
        list.setBounds (r.removeFromLeft (420));
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
    void runScan()
    {
        status.setText ("Scanning...", juce::dontSendNotification);
        scanButton.setEnabled (false);

        lastScan = scanner.scan (ledger, watchdog);

        // Quarantined plugins stay in the list but are not loadable. Hiding them
        // would make a crash look like an uninstall.
        rows.clear();
        for (const auto& p : lastScan.plugins)
            rows.add (p);

        list.updateContent();
        scanButton.setEnabled (true);

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

    void loadSelected()
    {
        const int row = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, rows.size()))
            return;

        const auto& sp = rows.getReference (row);
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
    int getNumRows() override { return rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, rows.size()))
            return;

        const auto& sp = rows.getReference (row);
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
    juce::Array<ScannedPlugin> rows;
    juce::String crashedId;

    juce::TextButton scanButton, loadButton;
    juce::Label      status;
    juce::ListBox    list;
    juce::Component  editorHolder;
    std::unique_ptr<juce::AudioProcessorEditor> hostedEditor;
    juce::Rectangle<int> lastEditorBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace ejmap
