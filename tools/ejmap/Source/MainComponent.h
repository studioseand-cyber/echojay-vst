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
#include "EchoJayAuRegistry.h"

#include <iostream>

namespace ejmap
{

class MainComponent  : public juce::Component,
                       private juce::ListBoxModel,
                       private juce::Timer
{
public:
    explicit MainComponent (juce::File ledgerRoot = {})
        : ledger (ledgerRoot), watchdog (ledger), host (scanner.getFormatManager())
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

        // Quarantine release. Manual is right; invisible is not. Without this
        // the only route was editing quarantine.json by hand.
        addAndMakeVisible (releaseButton);
        releaseButton.setButtonText ("Release");
        releaseButton.setEnabled (false);
        releaseButton.onClick = [this] { releaseSelected(); };

        addAndMakeVisible (summaryButton);
        summaryButton.setButtonText ("Summary");
        summaryButton.onClick = [this] { writeRunSummary(); };

        // Progress row, hidden until a scan is running.
        addChildComponent (progressBar);
        progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff161c26));
        progressBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff2f7f8c));

        addChildComponent (progressLabel);
        progressLabel.setJustificationType (juce::Justification::centredLeft);
        progressLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9fd8e0));

        addAndMakeVisible (editorHolder);

        // The scan costs 4.5 minutes and the watchdog stops the process by
        // design, so a relaunch used to mean a full rescan and ~900 more ledger
        // rows. Restore the last result and make Scan an explicit refresh.
        const auto restored = restoreScanCache();

        juce::String opening;
        if (crashedId.isNotEmpty())
            opening << "Previous session died loading " << crashedId << ". ";

        if (restored)
            opening << describeRestoredScan();
        else
            opening << "No saved scan. Scan to enumerate installed plugins.";

        status.setText (opening, juce::dontSendNotification);

        // Same reasoning as the title bar readback: on a machine where the
        // shell cannot screenshot, this is the only way to assert what the
        // window is actually showing.
        std::cout << "ejmap status: " << opening << std::endl;

        setSize (1280, 820);
        startTimerHz (4);
    }

    ~MainComponent() override
    {
        host.unload();
    }

    /** Scripted double-click proof.

        Queues two clicks and lets the pumped message loop inside load() deliver
        the second one. Uses triggerClick, which does NOT check isEnabled, so it
        defeats the BusyScope disable and tests the busy flag ALONE. That makes
        it a strictly harder test than a real double-click, where the disable
        would have to fail first.

        Skips the scan: it fabricates the one row it needs from the registry, so
        the proof takes seconds rather than four and a half minutes.
    */
    void selfTestReentry (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
        {
            std::cout << "SELFTEST: registry would not describe " << identifier << std::endl;
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            return;
        }

        ScannedPlugin sp;
        sp.desc = desc;
        rows.clearQuick();
        rows.add (sp);
        applyFilter();
        list.selectRow (0);

        std::cout << "SELFTEST: target " << desc.name << " (" << identifier << ")" << std::endl;

        // LAYER A, the realistic double-click. Button::handleCommandMessage
        // checks isEnabled(), so the second of these is blocked by BusyScope's
        // disable and never reaches loadSelected at all.
        std::cout << "SELFTEST: layer A - two button clicks" << std::endl;
        loadButton.triggerClick();
        loadButton.triggerClick();

        // LAYER B, the harder test. Straight onto the message queue, past the
        // button and past the disable, so it lands in loadSelected while the
        // first load is still inside its pumped editor-ready wait. Only the
        // busy flag can stop these.
        std::cout << "SELFTEST: layer B - two direct calls, bypassing the button" << std::endl;
        juce::MessageManager::callAsync ([this] { loadSelected(); });
        juce::MessageManager::callAsync ([this] { loadSelected(); });

        // LAYER C. After the first load has finished and its pump is RUNNING,
        // load again. That is the path where unload() must stop the pump before
        // freeing the instance it is rendering.
        juce::Timer::callAfterDelay (9000, [this]
        {
            std::cout << "SELFTEST: layer C - second load with the pump running" << std::endl;
            loadSelected();
        });

        juce::Timer::callAfterDelay (20000, [this]
        {
            std::cout << "SELFTEST: loadSelected entries=" << loadCalls
                      << " rejectedByFlag=" << loadRejected
                      << " maxDepth=" << maxLoadDepth << std::endl;
            std::cout << "SELFTEST: expected 4 entries (click1, direct1, direct2, layerC)"
                      << " - the 2nd button click never arrived, blocked by the disable"
                      << std::endl;
            std::cout << "SELFTEST: " << (maxLoadDepth <= 1 ? "PASS - never re-entered"
                                                            : "FAIL - RE-ENTERED") << std::endl;
            std::cout.flush();
            juce::JUCEApplication::getInstance()->quit();
        });
    }

    /** Proves the cache round-trips, that uninstalled entries are dropped, and
        that a quarantine applied AFTER the scan shows up on the restored list
        without a rescan.
    */
    void selfTestScanCache()
    {
        lastScan = PluginScanner::Result();
        auto add = [this] (juce::PluginDescription d)
        {
            ScannedPlugin sp; sp.desc = d; lastScan.plugins.add (sp);
        };

        add (echojay::auregistry::describeFromRegistry ("AudioUnit:Effects/aufx,SthB,OekS"));

        juce::PluginDescription ghostAu;
        ghostAu.pluginFormatName = "AudioUnit";
        ghostAu.fileOrIdentifier = "AudioUnit:Effects/aufx,ZZZZ,ZZZZ";
        ghostAu.name = "GhostAU";
        add (ghostAu);

        juce::PluginDescription realVst;
        realVst.pluginFormatName = "VST3";
        realVst.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/spiff.vst3";
        realVst.name = "spiff";
        add (realVst);

        juce::PluginDescription ghostVst;
        ghostVst.pluginFormatName = "VST3";
        ghostVst.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/DoesNotExist.vst3";
        ghostVst.name = "GhostVST3";
        add (ghostVst);

        lastScan.auEnumerated = 1419;
        saveScanCache();
        std::cout << "CACHETEST: saved " << lastScan.plugins.size() << " entries" << std::endl;

        // Quarantine one of them AFTER the scan was written. A relaunch is
        // exactly what follows a quarantine, so this must show without rescan.
        ledger.quarantine (realVst.fileOrIdentifier, "test_quarantine");

        rows.clearQuick();
        const bool ok = restoreScanCache();

        std::cout << "CACHETEST: restored=" << (ok ? "yes" : "no")
                  << " kept=" << rows.size() << " dropped=" << cacheDropped << std::endl;
        for (const auto& sp : rows)
            std::cout << "   kept: " << sp.desc.name << " [" << sp.desc.pluginFormatName
                      << "] quarantined=" << (ledger.isQuarantined (sp.pluginId()) ? "yes" : "no")
                      << std::endl;
        std::cout << "CACHETEST: status = " << describeRestoredScan() << std::endl;
        std::cout << "CACHETEST: " << ((ok && rows.size() == 2 && cacheDropped == 2)
                                         ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        juce::JUCEApplication::getInstance()->quit();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        auto top = r.removeFromTop (28);
        scanButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (6);
        loadButton.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (6);
        releaseButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (6);
        summaryButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (12);
        status.setBounds (top);

        // Only takes space while it is showing, so the idle layout is unchanged.
        if (progressBar.isVisible())
        {
            r.removeFromTop (6);
            auto prog = r.removeFromTop (20);
            progressBar.setBounds (prog.removeFromLeft (260));
            prog.removeFromLeft (10);
            progressLabel.setBounds (prog);
        }

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
            ++owner.loadDepth;
            owner.maxLoadDepth = juce::jmax (owner.maxLoadDepth, owner.loadDepth);
            owner.busy = true;
            owner.scanButton.setEnabled (false);
            owner.loadButton.setEnabled (false);
            owner.list.setEnabled (false);
            owner.filterBox.setEnabled (false);
            owner.releaseButton.setEnabled (false);
        }

        ~BusyScope()
        {
            --owner.loadDepth;
            owner.busy = false;
            owner.scanButton.setEnabled (true);
            owner.list.setEnabled (true);
            owner.filterBox.setEnabled (true);
            owner.updateButtonsForSelection();
        }

        MainComponent& owner;
        JUCE_DECLARE_NON_COPYABLE (BusyScope)
    };

    /** Bump when anything about what a cached entry MEANS changes, above all
        ScannedPlugin::pluginId.
    */
    static constexpr int kScanCacheVersion = 1;

    //==========================================================================
    juce::File scanCacheFile() const { return ledger.getRoot().getChildFile ("scan-cache.xml"); }

    /** Written after every completed scan. Carries the run that produced it, so
        a restored list can always name where it came from.
    */
    void saveScanCache() const
    {
        if (lastScan.plugins.isEmpty())
            return;   // never overwrite a good cache with an empty result

        juce::XmlElement root ("EJMAP_SCAN_CACHE");
        root.setAttribute ("cache_version", kScanCacheVersion);
        root.setAttribute ("run_id", ledger.currentRunId());
        root.setAttribute ("at", juce::Time::getCurrentTime().toISO8601 (true));
        root.setAttribute ("au_enumerated", lastScan.auEnumerated);
        root.setAttribute ("au_excluded_apple", lastScan.auExcludedApple);
        root.setAttribute ("au_excluded_echojay", lastScan.auExcludedEcho);
        root.setAttribute ("instruments_filtered", lastScan.instrumentsFiltered);
        root.setAttribute ("unused_type_filtered", lastScan.unusedTypeFiltered);
        root.setAttribute ("vst3_probed", lastScan.vst3Probed);
        root.setAttribute ("errors", lastScan.errors.size());

        for (const auto& sp : lastScan.plugins)
            if (auto desc = sp.desc.createXml())
                root.addChildElement (desc.release());

        root.writeTo (scanCacheFile());
    }

    /** Loads the last scan and validates every entry against what is installed
        NOW. Returns false when there is nothing usable to show.
    */
    bool restoreScanCache()
    {
        auto xml = juce::XmlDocument::parse (scanCacheFile());
        if (xml == nullptr || ! xml->hasTagName ("EJMAP_SCAN_CACHE"))
            return false;

        // A cache written by a build with different pluginId semantics would
        // restore entries keyed wrongly, and the ledger and quarantine key on
        // exactly that. Refuse rather than guess; the cost of being wrong is a
        // rescan, which is what would have happened anyway.
        if (xml->getIntAttribute ("cache_version", 0) != kScanCacheVersion)
            return false;

        cacheRunId = xml->getStringAttribute ("run_id");
        cacheAt    = juce::Time::fromISO8601 (xml->getStringAttribute ("at"));
        cacheDropped = 0;

        rows.clearQuick();

        for (auto* e : xml->getChildWithTagNameIterator ("PLUGIN"))
        {
            juce::PluginDescription d;
            if (! d.loadFromXml (*e))
                continue;

            if (! stillInstalled (d)) { ++cacheDropped; continue; }

            ScannedPlugin sp;
            sp.desc = d;
            rows.add (sp);
        }

        if (rows.isEmpty())
            return false;

        // Restore the census for display only. Labelled as belonging to that
        // run, never presented as a fresh count.
        lastScan = PluginScanner::Result();
        lastScan.auEnumerated       = xml->getIntAttribute ("au_enumerated");
        lastScan.auExcludedApple    = xml->getIntAttribute ("au_excluded_apple");
        lastScan.auExcludedEcho     = xml->getIntAttribute ("au_excluded_echojay");
        lastScan.instrumentsFiltered= xml->getIntAttribute ("instruments_filtered");
        lastScan.unusedTypeFiltered = xml->getIntAttribute ("unused_type_filtered");
        lastScan.vst3Probed         = xml->getIntAttribute ("vst3_probed");
        cacheErrors                 = xml->getIntAttribute ("errors");

        applyFilter();
        return true;
    }

    /** UNINSTALL DETECTION.

        Per entry, against the thing that actually says whether it is there:

          VST3       the bundle path exists on disk. A VST3 IS its bundle, so
                     this is exact and costs one stat.
          AudioUnit  the component is still in the AudioComponent registry, via
                     describeFromRegistry. No instantiation, no plugin code; the
                     whole 1419-component registry resolves in ~60 ms measured,
                     so 1319 lookups is not a launch cost worth optimising.

        Chosen over the alternatives deliberately. A file MODIFICATION time
        check would miss an uninstall entirely. Re-running the scan is the thing
        this whole change exists to avoid. Trusting the cache blindly would
        resurrect uninstalled plugins into the list, and the first the human
        would know is a load failure.

        Note what this does NOT do: it cannot see plugins installed SINCE the
        cached scan. That is why the age and run id are on screen and Scan is an
        explicit refresh.
    */
    static bool stillInstalled (const juce::PluginDescription& d)
    {
        if (d.pluginFormatName == "AudioUnit")
            return echojay::auregistry::describeFromRegistry (d.fileOrIdentifier)
                     .fileOrIdentifier.isNotEmpty();

        return juce::File (d.fileOrIdentifier).exists();
    }

    static juce::String ageOf (juce::Time t)
    {
        if (t == juce::Time())
            return "unknown age";

        const auto d = juce::Time::getCurrentTime() - t;
        if (d.inMinutes() < 1.0) return "just now";
        if (d.inHours()   < 1.0) return juce::String ((int) d.inMinutes()) + " min ago";
        if (d.inDays()    < 1.0) return juce::String (d.inHours(), 1) + " h ago";
        return juce::String (d.inDays(), 1) + " days ago";
    }

    /** Every restored list says where it came from and how old it is, so a
        stale list can never be mistaken for a fresh one.
    */
    juce::String describeRestoredScan() const
    {
        int quarantined = 0;
        for (const auto& sp : rows)
            if (ledger.isQuarantined (sp.pluginId()))
                ++quarantined;

        juce::String m;
        m << "RESTORED " << rows.size() << " plugins from scan " << cacheRunId
          << " (" << ageOf (cacheAt) << ")";
        if (quarantined > 0)
            m << "; " << quarantined << " quarantined";
        if (cacheDropped > 0)
            m << "; " << cacheDropped << " no longer installed, dropped";
        m << ". Scan to refresh.";
        return m;
    }

    void showProgress (bool shouldShow)
    {
        progressBar.setVisible (shouldShow);
        progressLabel.setVisible (shouldShow);
        resized();
    }

    void runScan()
    {
        if (busy)
            return;

        BusyScope guard (*this);
        status.setText ("Scanning...", juce::dontSendNotification);

        scanProgress = 0.0;
        progressLabel.setText ("starting...", juce::dontSendNotification);
        showProgress (true);
        lastPumpMs = 0;

        // WHY PUMP RATHER THAN MOVE THE SCAN TO A WORKER.
        //
        // scanVST3 runs on the message thread and opens plugin modules for the
        // 646 of 861 bundles with no moduleinfo.json. Moving that to a worker
        // would change which thread third-party module entry points run on,
        // which is a scan-behaviour change, not a UI change, and it is the one
        // thing this task said not to touch. It is also unverifiable short of a
        // full scan against every plugin on the machine.
        //
        // So the scan stays where it is and the callback pumps the loop, at
        // most every 50 ms, purely to let the window repaint.
        lastScan = scanner.scan (ledger, watchdog,
            [this] (const juce::String& phase, int done, int total, const juce::String& current)
            {
                scanProgress = total > 0 ? (double) done / (double) total : 0.0;

                juce::String t;
                t << phase << "  " << done << "/" << total << "   " << current;
                progressLabel.setText (t, juce::dontSendNotification);

                // Throttled. 861 unconditional pumps would add real time to a
                // scan whose whole problem is that it already takes 4.5 minutes.
                const auto now = juce::Time::getMillisecondCounter();
                if (now - lastPumpMs < 50)
                    return;

                lastPumpMs = now;

                if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
                    if (mm->isThisTheMessageThread())
                        mm->runDispatchLoopUntil (1);
            });

        showProgress (false);

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

        // Persist the result so the next launch does not pay 4.5 minutes for it.
        saveScanCache();
        cacheRunId = ledger.currentRunId();
        cacheAt    = juce::Time::getCurrentTime();
        cacheDropped = 0;

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
    /** Releases the selected plugin from quarantine. Still deliberate: it acts
        on one explicitly selected row and says what it did, including the
        reason the plugin was quarantined for, so the human sees what they are
        overriding.
    */
    void releaseSelected()
    {
        const int row = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, visibleRows.size()))
            return;

        const auto& sp = rows.getReference (visibleRows[row]);
        const auto id  = sp.pluginId();

        if (! ledger.isQuarantined (id))
            return;

        ledger.releaseFromQuarantine (id);
        list.repaint();
        updateButtonsForSelection();

        status.setText (sp.desc.name + " released from quarantine. It will be loadable again; "
                          "if it fails twice more it will be quarantined again.",
                        juce::dontSendNotification);
    }

    /** Load is for plugins that are not quarantined, Release is for ones that
        are. Exactly one of them is ever available for a given row.
    */
    void updateButtonsForSelection()
    {
        const int row = list.getSelectedRow();
        const bool haveRow = juce::isPositiveAndBelow (row, visibleRows.size());
        const bool quarantined = haveRow
                                   && ledger.isQuarantined (rows.getReference (visibleRows[row]).pluginId());

        loadButton.setEnabled (! busy && haveRow && ! quarantined);
        releaseButton.setEnabled (! busy && quarantined);
    }

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

        updateButtonsForSelection();
        list.repaint();
    }

    void loadSelected()
    {
        ++loadCalls;

        // A nested call during the editor-ready wait is a no-op, not a crash.
        // Counted, not silent: a rejection here is the guard doing its job and
        // the only way to tell that from "the second click never arrived".
        if (busy)
        {
            ++loadRejected;
            std::cout << "loadSelected: REJECTED re-entrant call #" << loadCalls
                      << " (busy, depth=" << loadDepth << ")" << std::endl;
            return;
        }

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
        updateButtonsForSelection();
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

    juce::TextButton scanButton, loadButton, releaseButton, summaryButton;
    bool busy = false;
    int  loadDepth = 0, maxLoadDepth = 0, loadCalls = 0, loadRejected = 0;

    juce::String cacheRunId;      // run that produced the list on screen
    juce::Time   cacheAt;
    int          cacheDropped = 0, cacheErrors = 0;
    juce::Label      status;
    double scanProgress = 0.0;                       // declared before the bar that references it
    juce::ProgressBar progressBar { scanProgress };
    juce::Label       progressLabel;
    juce::uint32      lastPumpMs = 0;
    juce::TextEditor filterBox;
    juce::ListBox    list;
    juce::Component  editorHolder;
    std::unique_ptr<juce::AudioProcessorEditor> hostedEditor;
    juce::Rectangle<int> lastEditorBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace ejmap
