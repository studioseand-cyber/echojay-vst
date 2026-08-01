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
#include "EjmapScanProgress.h"
#include "EjmapSupervisor.h"
#include "EjmapCapture.h"
#include "EjmapMouseRing.h"
#include "EjmapSweeper.h"
#include "EjmapCurveView.h"
#include "EjmapAssignPanel.h"
#include "EchoJayParamMaps.h"   // fingerprintForDescription
#include "EjmapMouth.h"
#include "EjmapBuildInfo.h"     // EJMAP_APPLY_HEADER_SHA, stamped as compiled
#include <sys/stat.h>            // machine_id 0600, ejextract convention

#include <iostream>

namespace ejmap
{

class MainComponent  : public juce::Component,
                       private juce::ListBoxModel,
                       private juce::Timer
{
public:
    explicit MainComponent (juce::File ledgerRoot = {},
                            bool supervised = false,
                            int restartCount = 0,
                            juce::String afterExit = {})
        : ledger (ledgerRoot), watchdog (ledger), capture (watchdog), host (scanner.getFormatManager()),
          isSupervised (supervised), restartsThisSession (restartCount),
          lastExitCause (std::move (afterExit))
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
        // Test signal. Off by default and disabled while armed: a signal that
        // changes what the plugin does during capture is its own problem.
        addAndMakeVisible (signalToggle);
        signalToggle.setButtonText ("Test signal");
        signalToggle.setEnabled (false);
        signalToggle.onClick = [this]
        {
            host.setTestSignalEnabled (signalToggle.getToggleState());
            captureReadout.setText (signalToggle.getToggleState()
                                      ? "Test signal ON (1 kHz -12 dBFS). Re-run the baseline to "
                                        "let the noise mask see moving meters."
                                      : "Test signal OFF. Capture runs against silence.",
                                    juce::dontSendNotification);
        };

        addAndMakeVisible (armButton);
        armButton.setButtonText ("Arm");
        armButton.setEnabled (false);
        armButton.onClick = [this] { armCapture(); };

        // Sweep runs against the LAST CAPTURED INDEX, never the whole plugin:
        // the settle-and-read cost is per parameter and the mapper only needs
        // curves for what a human actually captured.
        addAndMakeVisible (sweepButton);
        sweepButton.setButtonText ("Sweep");
        sweepButton.setEnabled (false);
        sweepButton.onClick = [this] { sweepCaptured(); };

        // Typed anchors: the fallback for both liar classes (flat, identity
        // display) and for any curve the human rejects. Enabled after any
        // sweep, because the human's distrust of a curve is a valid reason
        // regardless of what the sweep concluded.
        addAndMakeVisible (typeButton);
        typeButton.setButtonText ("Type anchors");
        typeButton.setEnabled (false);
        typeButton.onClick = [this] { startTypedAnchors(); };

        addChildComponent (curveView);
        curveView.onReject = [this] { startTypedAnchors(); };

        addAndMakeVisible (assignButton);
        assignButton.setButtonText ("Assign");
        assignButton.setEnabled (false);
        assignButton.onClick = [this] { startAssignment(); };

        addAndMakeVisible (uploadButton);
        uploadButton.setButtonText ("Upload...");
        uploadButton.setEnabled (false);
        uploadButton.onClick = [this] { openUploadCard(); };

        addAndMakeVisible (deepToggle);
        deepToggle.setButtonText ("Deep");
        deepToggle.onClick = [this] { assignPanel.deepMode = deepToggle.getToggleState(); };

        addChildComponent (assignPanel);
        wireAssignHooks();

        addChildComponent (typedPrompt);
        typedPrompt.setColour (juce::Label::textColourId, juce::Colour (0xffd8b06a));
        addChildComponent (typedEntry);
        typedEntry.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff161c26));
        typedEntry.onReturnKey = [this] { typedNext(); };
        addChildComponent (typedNextButton);
        typedNextButton.setButtonText ("Next");
        typedNextButton.onClick = [this] { typedNext(); };
        addChildComponent (typedCancelButton);
        typedCancelButton.setButtonText ("Cancel");
        typedCancelButton.onClick = [this] { typedCancel(); };

        // Candidate picker: appears only for a multi-parameter gesture, where
        // the engine cannot know which of the moved parameters the human meant.
        addChildComponent (candidatePicker);
        candidatePicker.setTextWhenNothingSelected ("pick the parameter you meant");
        candidatePicker.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff161c26));
        candidatePicker.setColour (juce::ComboBox::textColourId, juce::Colour (0xff9fd8e0));
        candidatePicker.onChange = [this] { chooseCandidate(); };

        // Mask strip: what capture is refusing to watch, and the way back.
        // Promotion blinded two real controls on this machine (Q1 Band 1 Gain,
        // Pro-Q 3 Band 1 Frequency) with no route back but editing files.
        // Same reasoning as quarantine release: manual is right, invisible is
        // not.
        addChildComponent (maskPicker);
        maskPicker.setTextWhenNothingSelected ("masked parameters");
        maskPicker.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff161c26));
        maskPicker.setColour (juce::ComboBox::textColourId, juce::Colour (0xffd8b06a));
        addChildComponent (unmaskButton);
        unmaskButton.setButtonText ("Unmask");
        unmaskButton.onClick = [this] { unmaskSelected(); };

        // What counts as human evidence behind a moved index, answered from
        // things the engine has no business owning: a mouse grab inside the
        // hosted editor near the detection, or the plugin's own gesture report
        // on that index since arm. The second covers what the first cannot
        // see -- a MIDI controller or a GUI whose touch never involves our
        // mouse -- exactly the hole the mouse-only probe recorded.
        capture.setListenerBank (&listeners);
        capture.setHumanEvidenceProbe ([this] (int idx, const CaptureEngine::Result& r)
        {
            return mouseGrabInEditorNear (r.detectedAtMs)
                || listeners.sawGestureOn (idx, r.armedAtMs);
        });

        addAndMakeVisible (captureReadout);
        captureReadout.setJustificationType (juce::Justification::centredLeft);
        captureReadout.setColour (juce::Label::textColourId, juce::Colour (0xff9fd8e0));

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
        // What died is whatever recovery just named. Never auto-reloadable:
        // a host-attributed crash is spared from quarantine on purpose, so
        // nothing else would stop it being loaded straight back into the same
        // fault.
        doNotAutoReload = crashedId;

        if (restartsThisSession > 0)
            recordRestartRow();

        const int autoReleased = releaseStaleScanQuarantines();
        const auto restored = restoreScanCache();

        juce::String opening;

        // Auto-relaunch must not make a degrading session invisible. This leads
        // the status line and stays until a load succeeds.
        if (restartsThisSession > 0)
        {
            opening << "RESTARTED after " << (crashedId.isEmpty() ? "an abnormal exit" : crashedId)
                    << " (" << lastExitCause << "). "
                    << restartsThisSession << " of "
                    << SupervisorLimits::kMaxConsecutive << " restarts used. ";
        }
        else if (crashedId.isNotEmpty())
        {
            opening << "Previous session died loading " << crashedId << ". ";
        }

        if (autoReleased > 0)
            opening << autoReleased << " scan quarantine(s) auto-released (plugin changed on disk). ";

        if (restored)
            opening << describeRestoredScan();
        else
            opening << "No saved scan. Scan to enumerate installed plugins.";

        status.setText (opening, juce::dontSendNotification);

        // Same reasoning as the title bar readback: on a machine where the
        // shell cannot screenshot, this is the only way to assert what the
        // window is actually showing.
        std::cout << "ejmap status: " << opening << std::endl;

        setSize (1440, 900);
        startTimerHz (4);
    }

    ~MainComponent() override
    {
        capture.stop();
        listeners.detach();      // while the instance's parameters still exist
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

    /** Proves the resume/stamp mechanism and the scan-quarantine auto-release,
        against a synthetic bundle so nothing installed is touched.
    */
    void selfTestProgressAndRelease()
    {
        auto root  = ledger.getRoot();
        auto fake  = root.getChildFile ("Fake.vst3");
        auto bin   = fake.getChildFile ("Contents").getChildFile ("MacOS").getChildFile ("Fake");
        bin.getParentDirectory().createDirectory();
        bin.replaceWithText ("v1");

        const auto path = fake.getFullPathName();
        int fails = 0;
        auto check = [&fails] (bool ok, const char* what)
        {
            std::cout << "  " << (ok ? "ok   " : "FAIL ") << what << std::endl;
            if (! ok) ++fails;
        };

        juce::PluginDescription d;
        d.name = "FakePlug"; d.pluginFormatName = "VST3"; d.fileOrIdentifier = path;
        juce::OwnedArray<juce::PluginDescription> found;
        found.add (new juce::PluginDescription (d));

        auto pfile = root.getChildFile ("selftest-progress.jsonl");
        {
            ScanProgress w (pfile);
            w.begin ("testrun");
            w.record (path, "ok", found);
        }

        {
            ScanProgress r (pfile);
            check (r.load(), "progress file loads");
            check (r.hasEntryFor (path), "entry present");
            auto* e = r.usableEntryFor (path);
            check (e != nullptr, "entry usable while the bundle is unchanged");
            check (e != nullptr && e->descriptions.size() == 1
                     && e->descriptions[0].name == "FakePlug", "description round-trips");
        }

        // Same bundle, updated in place: same path, same list, different build.
        juce::Thread::sleep (1100);          // mtime granularity
        bin.replaceWithText ("v2-longer-content");

        {
            ScanProgress r (pfile);
            r.load();
            check (r.hasEntryFor (path), "entry still present after update");
            check (r.usableEntryFor (path) == nullptr,
                   "entry REJECTED after the bundle changed on disk");
        }

        // An unreadable or foreign format is refused, never interpreted.
        {
            auto lines = juce::StringArray::fromLines (pfile.loadFileAsString());
            lines.set (0, "{\"format\":\"ejmap-scan-progress\",\"version\":99}");
            pfile.replaceWithText (lines.joinIntoString ("\n"));
            ScanProgress r (pfile);
            check (! r.load(), "foreign format version refused");
        }

        // Auto-release: a SCAN quarantine on a bundle that has since changed.
        bin.replaceWithText ("v3");
        ledger.quarantine (path, "hang_in_findAllTypesForFile", "scan", {});
        check (ledger.isQuarantined (path), "scan quarantine applied");

        juce::Thread::sleep (1100);
        bin.replaceWithText ("v4-different-again");
        check (releaseStaleScanQuarantines() == 1, "scan quarantine auto-released after change");
        check (! ledger.isQuarantined (path), "no longer quarantined");

        // A LOAD quarantine on the same changed bundle must NOT auto-release.
        ledger.quarantine (path, "crash_on_load", "load", {});
        juce::Thread::sleep (1100);
        bin.replaceWithText ("v5-changed-yet-again");
        check (releaseStaleScanQuarantines() == 0, "load quarantine NOT auto-released");
        check (ledger.isQuarantined (path), "load quarantine still held");

        std::cout << "PROGRESSTEST: " << (fails == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        juce::JUCEApplication::getInstance()->quit();
    }

    /** Drives the classifier by MOVING PARAMETERS PROGRAMMATICALLY, so the
        mechanism is provable without a human at the knobs. The wiggle test is
        the human's; this is the part that can be measured here.
    */
    void selfTestCapture (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "CAPTURETEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        rows.clearQuick(); rows.add (sp); applyFilter(); list.selectRow (0);

        loadedName = desc.name;
        loadedId   = sp.pluginId();

        ledger.beginLoad (sp.pluginId(), desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        LedgerRecord rec; rec.pluginId = sp.pluginId(); rec.name = desc.name;
        rec.format = desc.pluginFormatName; rec.outcome = res.outcome; rec.detail = res.detail;
        rec.paramCount = res.paramCount; ledger.endLoad (rec);

        if (res.outcome != LoadOutcome::ok)
        {
            loadedName.clear(); loadedId.clear();
            std::cout << "CAPTURETEST: load failed: " << res.detail << std::endl; quitNow(); return;
        }

        auto* inst = host.getInstance();
        listeners.attach (*inst);      // the listener stage needs real callbacks
        cal  = capture.calibrate (*inst, sp.pluginId());
        std::cout << "CAPTURETEST: " << desc.name << " | " << cal.describe() << std::endl;

        mask = capture.buildNoiseMask (*inst, cal, sp.pluginId());
        std::cout << "CAPTURETEST: noise mask " << mask.indices.size() << " of " << cal.paramCount
                  << " over " << mask.samples << " samples / "
                  << juce::String (mask.seconds, 1) << "s" << std::endl;

        stage = 0;
        runCaptureStage();
    }

    void quitNow() { juce::JUCEApplication::getInstance()->quit(); }

    /** Loads one plugin repeatedly from ordinary message context, which is what
        a button click is, until it stalls or the attempt budget runs out.

        The stall is racy: whether a bridged AU's out-of-process creation
        completes inline or needs a message-thread turn is a timing matter. So
        the reproduction is repetition, not cleverness.
    */
    void selfTestStall (const juce::String& identifier, int attempts)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "STALLTEST: unknown identifier" << std::endl; quitNow(); return; }

        stallDesc = desc;
        stallLeft = attempts;
        stallAttempt = 0;
        std::cout << "STALLTEST: " << desc.name << ", up to " << attempts << " attempts" << std::endl;
        nextStallAttempt();
    }

    void nextStallAttempt()
    {
        if (stallLeft-- <= 0)
        {
            std::cout << "STALLTEST: no stall in " << stallAttempt << " attempts" << std::endl;
            std::cout.flush(); quitNow(); return;
        }

        ++stallAttempt;
        ScannedPlugin sp; sp.desc = stallDesc;
        loadedName = stallDesc.name; loadedId = sp.pluginId();

        std::cout << "STALLTEST: attempt " << stallAttempt << "..." << std::endl;
        std::cout.flush();

        ledger.beginLoad (sp.pluginId(), stallDesc.name, stallDesc.manufacturerName,
                          stallDesc.pluginFormatName, stallDesc.version,
                          "load", "createPluginInstance");
        const auto t0 = juce::Time::getMillisecondCounter();
        auto res = host.load (stallDesc, watchdog);
        const auto ms = juce::Time::getMillisecondCounter() - t0;

        LedgerRecord rec; rec.pluginId = sp.pluginId(); rec.name = stallDesc.name;
        rec.format = stallDesc.pluginFormatName; rec.outcome = res.outcome;
        rec.detail = res.detail; ledger.endLoad (rec);

        std::cout << "STALLTEST: attempt " << stallAttempt << " -> " << toString (res.outcome)
                  << " in " << ms << " ms" << std::endl;
        std::cout.flush();

        capture.stop();
        listeners.detach();
        host.unload();

        juce::MessageManager::callAsync ([this] { nextStallAttempt(); });
    }

    /** Demonstrates the noise mask actually excluding something.

        Baseline twice on the same plugin: once against silence, once with the
        test signal running. If the plugin exposes a level or gain-reduction
        readout as a parameter, the second mask is non-empty and the first is
        not, which is the mask doing its job rather than being trivially empty.
        Then arms and confirms capture still works with those indices excluded.
    */
    void selfTestNoiseMask (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "MASKTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId();
        auto res = host.load (desc, watchdog);
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "MASKTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        cal = capture.calibrate (*inst, loadedId);
        std::cout << "MASKTEST: " << desc.name << " | " << cal.describe() << std::endl;

        host.setTestSignalEnabled (false);
        juce::Thread::sleep (400);
        auto silent = capture.buildNoiseMask (*inst, cal, loadedId);
        std::cout << "MASKTEST: silence  -> mask " << silent.indices.size()
                  << " of " << cal.paramCount
                  << " (" << silent.samples << " samples)" << std::endl;

        host.setTestSignalEnabled (true);
        juce::Thread::sleep (800);          // let the plugin's detectors settle
        auto signal = capture.buildNoiseMask (*inst, cal, loadedId);
        std::cout << "MASKTEST: 1kHz signal -> mask " << signal.indices.size()
                  << " of " << cal.paramCount
                  << " (" << signal.samples << " samples)" << std::endl;

        for (int i = 0; i < juce::jmin (8, signal.indices.size()); ++i)
        {
            const int idx = signal.indices[i];
            std::cout << "    masked index " << idx << "  "
                      << inst->getParameters()[idx]->getName (40) << std::endl;
        }

        const bool demonstrated = signal.indices.size() > silent.indices.size();
        std::cout << "MASKTEST: mask excluded " << (signal.indices.size() - silent.indices.size())
                  << " self-changing parameter(s) that silence never revealed -> "
                  << (demonstrated ? "DEMONSTRATED" : "NOT DEMONSTRATED (no signal-driven "
                                                      "parameter on this plugin)") << std::endl;

        // Capture must still work with those indices excluded.
        mask = signal;
        host.setTestSignalEnabled (false);
        juce::Thread::sleep (300);
        capture.arm (*inst, cal, mask, [this, demonstrated] (const CaptureEngine::Result& r)
        {
            std::cout << "MASKTEST: capture with mask active -> " << r.kindString()
                      << " indices=" << r.indices.size() << std::endl;
            std::cout << "MASKTEST: " << (demonstrated ? "PASS" : "INCONCLUSIVE") << std::endl;
            std::cout.flush();
            quitNow();
        });

        juce::Timer::callAfterDelay (500, [this]
        {
            auto* in = host.getInstance();
            if (in == nullptr) return;
            for (auto* pp : in->getParameters())
                if (! pp->isDiscrete() && pp->isAutomatable() && ! mask.indices.contains (
                        in->getParameters().indexOf (pp)))
                { pp->setValueNotifyingHost (0.2f); juce::Thread::sleep (30);
                  pp->setValueNotifyingHost (0.6f); break; }
        });
    }

    /** Proves the promotion-suppression probe with a REAL held button, not by
        inspection: three capture cycles under a grab inside the editor must not
        promote, three more with the mouse up must, and the unmask path must
        release what they promoted. The harness posts the grab with CGEventPost;
        grab_visible is printed per cycle so a machine where event posting is
        blocked reads as "cannot prove" rather than as a failure of the probe.
    */
    void selfTestPromoSuppress (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "SUPPRESSTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId();
        auto res = host.load (desc, watchdog);
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "SUPPRESSTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        // The probe reads grabs against the hosted editor's bounds, so the
        // editor must actually be attached and on screen, exactly as in the app.
        attachEditor();
        resized();
        listeners.attach (*host.getInstance());

        auto* inst = host.getInstance();
        cal = capture.calibrate (*inst, loadedId);
        mask = capture.buildNoiseMask (*inst, cal, loadedId);
        capture.resetCycleCounts();
        promotionsFlushed = 0;
        lastCapturedIndex = -1;
        sweepButton.setEnabled (false);

        suppressIdx = -1;
        auto& params = inst->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            if (mask.indices.contains (i)) continue;
            auto* pp = params[i];
            if (pp->isDiscrete() || ! pp->isAutomatable()) continue;
            pp->setValueNotifyingHost (0.20f); juce::Thread::sleep (15);
            const float lo = pp->getValue();
            pp->setValueNotifyingHost (0.50f); juce::Thread::sleep (15);
            const float hi = pp->getValue();
            if (std::abs (lo - 0.20f) < 0.02f && std::abs (hi - 0.50f) < 0.02f)
            { suppressIdx = i; break; }
        }
        if (suppressIdx < 0)
        { std::cout << "SUPPRESSTEST: no usable parameter" << std::endl; quitNow(); return; }

        const auto eb = hostedEditor != nullptr ? hostedEditor->getScreenBounds()
                                                : juce::Rectangle<int>();
        if (eb.isEmpty())
        { std::cout << "SUPPRESSTEST: no editor" << std::endl; quitNow(); return; }

        std::cout << "SUPPRESSTEST: " << desc.name << " | param " << suppressIdx
                  << " (" << params[suppressIdx]->getName (32) << ")" << std::endl;
        std::cout << "SUPPRESSTEST: EDITOR_BOUNDS " << eb.getX() << " " << eb.getY()
                  << " " << eb.getWidth() << " " << eb.getHeight() << std::endl;
        std::cout << "SUPPRESSTEST: phase A in 3000 ms; hold the left button inside the editor"
                  << std::endl;
        std::cout.flush();

        // The prompts also go to the WINDOW, because the holder of the mouse
        // is a human watching the editor, not the terminal.
        captureReadout.setText ("SUPPRESS TEST: press and HOLD the left button on the "
                                "editor's empty display area NOW, until told to release",
                                juce::dontSendNotification);

        stage = 0;
        grabSeenA = false;
        juce::Timer::callAfterDelay (3000, [this] { suppressCycle(); });
    }

    void suppressCycle()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) return;

        if (stage == 3)     // phase A verdict: grabbed cycles must not promote
        {
            // No grab ever visible means the harness (or the human) never
            // placed one, and every cycle legitimately counted. That is not
            // evidence about the probe either way, and reporting it as FAIL
            // would read as the probe being broken. Say what actually happened.
            if (! grabSeenA)
            {
                std::cout << "SUPPRESSTEST: CANNOT PROVE - no grab was visible in any phase-A "
                             "cycle, so suppression was never exercised. Hold the mouse inside "
                             "the editor during phase A and run again." << std::endl;
                std::cout.flush(); quitNow(); return;
            }

            const bool suppressed = mask.promotions.isEmpty()
                                      && ! mask.indices.contains (suppressIdx);
            if (! suppressed) ++failures;
            std::cout << "  " << (suppressed ? "ok   " : "FAIL ")
                      << "3 cycles under a grab inside the editor -> not promoted" << std::endl;
            std::cout << "SUPPRESSTEST: RELEASE the mouse now; phase B in 3500 ms" << std::endl;
            std::cout.flush();
            captureReadout.setText ("SUPPRESS TEST: RELEASE the mouse now and keep hands off",
                                    juce::dontSendNotification);
            ++stage;
            juce::Timer::callAfterDelay (3500, [this] { suppressCycle(); });
            return;
        }

        if (stage == 7)     // phase B verdict + reversibility through the UI path
        {
            const bool promoted = mask.indices.contains (suppressIdx)
                                    && ! mask.promotions.isEmpty();
            if (! promoted) ++failures;
            std::cout << "  " << (promoted ? "ok   " : "FAIL ")
                      << "3 more cycles with the mouse up -> promoted" << std::endl;

            refreshMaskUi();
            for (int i = 0; i < maskPicker.getNumItems(); ++i)
                if (maskPicker.getItemText (i).startsWith (juce::String (suppressIdx) + ":"))
                    maskPicker.setSelectedId (maskPicker.getItemId (i), juce::dontSendNotification);
            unmaskSelected();

            const bool released = ! mask.indices.contains (suppressIdx)
                                    && ledger.runArtifact ("captures", "jsonl").loadFileAsString()
                                         .contains ("mask_released");
            if (! released) ++failures;
            std::cout << "  " << (released ? "ok   " : "FAIL ")
                      << "unmask through the UI path -> released and recorded" << std::endl;

            std::cout << "SUPPRESSTEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
            std::cout.flush(); quitNow(); return;
        }

        inst->getParameters()[suppressIdx]->setValueNotifyingHost (0.20f);
        juce::Thread::sleep (120);      // let the base settle before the snapshot

        capture.arm (*inst, cal, mask, [this] (const CaptureEngine::Result& r)
        {
            const bool grab = mouseGrabInEditorNear (r.detectedAtMs);
            if (stage < 3 && grab)
                grabSeenA = true;

            std::cout << "  cycle " << stage << ": " << r.kindString()
                      << " grab_visible=" << (grab ? "yes" : "no") << std::endl;
            std::cout.flush();
            flushPromotionRows();
            ++stage;
            juce::Timer::callAfterDelay (400, [this] { suppressCycle(); });
        });

        juce::Timer::callAfterDelay (300, [this]
        {
            auto* in = host.getInstance();
            if (in != nullptr)
                in->getParameters()[suppressIdx]->setValueNotifyingHost (0.50f);
        });
    }

    /** Loads a plugin and sweeps one parameter, chosen by index or by name
        fragment; with no spec, the first automatable parameter. The M3 gate
        runs through here: Valhalla for the text-liar class, mpressor for
        descending anchors.
    */
    void selfTestSweep (const juce::String& identifier, const juce::String& paramSpec)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);

        // VST3s resolve through the scan cache, the same source the app's
        // Load button uses -- never findAllTypesForFile, which is banned in
        // ejmap. Seed a throwaway root by copying scan-cache.xml into it.
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }

        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "SWEEPTEST: unknown identifier (not in the AU registry, and "
                       "not in this root's scan cache)" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId();

        // Inflight protocol, exactly as the app path: the mpressor crashes
        // had to be reconstructed from DiagnosticReports because this test
        // left no row behind. A crash between beginLoad and endLoad now
        // leaves inflight.json, which is attributable evidence.
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version,
                          "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        {
            LedgerRecord rec;
            rec.pluginId = loadedId; rec.name = desc.name;
            rec.vendor = desc.manufacturerName; rec.format = desc.pluginFormatName;
            rec.version = desc.version; rec.outcome = res.outcome;
            rec.detail = res.detail; rec.paramCount = res.paramCount;
            ledger.endLoad (rec);
        }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "SWEEPTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        listeners.attach (*inst);
        cal = capture.calibrate (*inst, loadedId);

        auto& params = inst->getParameters();
        int idx = -1;
        if (paramSpec.isNotEmpty())
        {
            if (paramSpec.containsOnly ("0123456789"))
                idx = paramSpec.getIntValue();
            else
                for (int i = 0; i < params.size(); ++i)
                    if (params[i]->getName (64).containsIgnoreCase (paramSpec))
                    { idx = i; break; }
        }
        else
        {
            for (int i = 0; i < params.size(); ++i)
                if (params[i]->isAutomatable()) { idx = i; break; }
        }

        if (! juce::isPositiveAndBelow (idx, params.size()))
        { std::cout << "SWEEPTEST: no parameter matches \"" << paramSpec << "\"" << std::endl;
          quitNow(); return; }

        const auto name = params[idx]->getName (48);
        std::cout << "SWEEPTEST: " << desc.name << " | param " << idx
                  << " (" << name << ")" << std::endl;

        beginSweepInflight (idx, name);
        host.pausePumpForMutation();
        auto sw = sweepOneIndex (*inst, idx, watchdog, loadedId);
        host.resumePumpAfterMutation();
        endSweepInflight (idx, sw);
        recordSweep (idx, name, sw);

        std::cout << "SWEEPTEST: ok=" << (sw.ok ? "yes" : "no")
                  << " method=" << sw.method
                  << " flat=" << (sw.flat ? "yes" : "no")
                  << " reversed=" << (sw.anchorsReversed ? "yes" : "no")
                  << " identity_display=" << (sw.identityDisplay ? "yes" : "no")
                  << " anchors=" << sw.anchors.size()
                  << " rejected=" << sw.rejectedPoints
                  << " unparsed=" << sw.unparsedPoints
                  << " duration=" << sw.durationMs << "ms" << std::endl;
        std::cout << "SWEEPTEST: reason: " << sw.reason << std::endl;
        for (int i = 0; i < juce::jmin (5, sw.anchors.size()); ++i)
            std::cout << "    anchor [" << sw.anchors[i][0] << ", " << sw.anchors[i][1] << "]"
                      << std::endl;
        if (sw.anchors.size() > 5)
            std::cout << "    ... " << (sw.anchors.size() - 5) << " more" << std::endl;
        std::cout << "SWEEPTEST: DONE" << std::endl;
        std::cout.flush();
        quitNow();
    }

    /** Applies a REAL on-disk map through the real applySettings on the live
        plugin: the headline assertion against human-produced artifacts, not
        synthetic ones. Reports where each request landed and that the named
        imposter's value never moved.
    */
    void selfTestApplyMap (const juce::String& identifier, const juce::String& mapPath,
                           const juce::String& imposterName)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "APPLYTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        { LedgerRecord rec; rec.pluginId = loadedId; rec.name = desc.name;
          rec.outcome = res.outcome; rec.detail = res.detail; rec.paramCount = res.paramCount;
          ledger.endLoad (rec); }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "APPLYTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        auto map = juce::JSON::parse (juce::File::getCurrentWorkingDirectory()
                                        .getChildFile (mapPath).loadFileAsString());
        if (! map.isObject())
        { std::cout << "APPLYTEST: map did not parse: " << mapPath << std::endl; quitNow(); return; }

        std::cout << "APPLYTEST: " << desc.name << " | map " << mapPath << std::endl;
        auto& ps = inst->getParameters();
        const int impIdx = paramIndexByName (imposterName);
        int fails = 0;
        auto ok = [&fails] (bool cond, const juce::String& what)
        {
            if (! cond) ++fails;
            std::cout << "  " << (cond ? "ok   " : "FAIL ") << what << std::endl;
        };

        auto nameOf = [&ps] (int i) { return juce::isPositiveAndBelow (i, ps.size())
                                               ? ps[i]->getName (48) : juce::String(); };

        // 250 Hz + 3 dB as a flat band-class request (the chain's shape).
        {
            const float imp = impIdx >= 0 ? ps[impIdx]->getValue() : 0.0f;
            auto* st = new juce::DynamicObject();
            st->setProperty ("freq_hz", 250);
            st->setProperty ("gain_db", 3);
            host.pausePumpForMutation();
            auto results = echojay::applySettings (*inst, map, juce::var (st));
            host.resumePumpAfterMutation();
            for (const auto& r : results)
                if (r.semantic == "freq_hz" || r.semantic == "gain_db")
                    std::cout << "    250Hz req " << r.semantic << " -> ["
                              << r.index << "] " << nameOf (r.index)
                              << (r.applied ? "  applied, " : "  NOT APPLIED, ")
                              << r.note << std::endl;
            bool landedOk = false;
            for (const auto& r : results)
                if (r.semantic == "freq_hz") landedOk = r.applied;
            ok (landedOk, "250 Hz applied to a band");
            if (impIdx >= 0)
                ok (juce::approximatelyEqual (ps[impIdx]->getValue(), imp),
                    imposterName + " [" + juce::String (impIdx) + "] value unchanged at 250 Hz");
        }

        // 8 kHz as an explicit bands request.
        {
            const float imp = impIdx >= 0 ? ps[impIdx]->getValue() : 0.0f;
            auto* band = new juce::DynamicObject();
            band->setProperty ("freq_hz", 8000);
            band->setProperty ("gain_db", -2);
            juce::Array<juce::var> bands; bands.add (juce::var (band));
            auto* st = new juce::DynamicObject();
            st->setProperty ("bands", juce::var (bands));
            host.pausePumpForMutation();
            auto results = echojay::applySettings (*inst, map, juce::var (st));
            host.resumePumpAfterMutation();
            for (const auto& r : results)
                std::cout << "    8kHz req " << r.semantic << " -> ["
                          << r.index << "] " << nameOf (r.index)
                          << (r.applied ? "  applied, " : "  NOT APPLIED, ")
                          << r.note << std::endl;
            bool landedOk = false;
            for (const auto& r : results)
                if (r.semantic == "freq_hz") landedOk = r.applied;
            ok (landedOk, "8 kHz applied to a band");
            if (impIdx >= 0)
                ok (juce::approximatelyEqual (ps[impIdx]->getValue(), imp),
                    imposterName + " value unchanged at 8 kHz");
        }

        std::cout << "APPLYTEST: " << (fails == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        quitNow();
    }

    /** M10: the mouth, dry run, stub and store read-back, against a REAL map
        file. Corrupt variants prove each rejection names its reason.
    */
    void selfTestUpload (const juce::String& identifier, const juce::String& mapPath,
                         const juce::String& tester)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "UPLOADTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        auto res = host.load (desc, watchdog);
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "UPLOADTEST: load failed" << std::endl; quitNow(); return; }
        cal = capture.calibrate (*host.getInstance(), loadedId);
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);

        failures = 0;
        auto ok = [this] (bool cond, const juce::String& what)
        {
            if (! cond) ++failures;
            std::cout << "  " << (cond ? "ok   " : "FAIL ") << what << std::endl;
            std::cout.flush();
        };

        auto src = juce::File::getCurrentWorkingDirectory().getChildFile (mapPath);
        if (! src.existsAsFile())
        { std::cout << "UPLOADTEST: map file does not exist: " << src.getFullPathName()
                    << "\nUPLOADTEST: FAIL" << std::endl; quitNow(); return; }
        auto mapsDir = ledger.getRoot().getChildFile ("maps");
        mapsDir.createDirectory();
        auto local = mapsDir.getChildFile (currentFp + ".json");
        auto mapVar = juce::JSON::parse (src.loadFileAsString());
        // Re-key the map to THIS load's fp so identity checks are live.
        if (auto* o = mapVar.getDynamicObject()) o->setProperty ("fp", currentFp);
        local.replaceWithText (juce::JSON::toString (mapVar, false));

        std::cout << "UPLOADTEST: " << desc.name << " | map " << src.getFileName() << std::endl;

        // 1. No tester name -> the gate refuses and says how to fix it.
        ledger.getRoot().getChildFile ("tester.json").deleteFile();
        auto v1 = Mouth::structuralGate (juce::JSON::parse (local.loadFileAsString()), testerName());
        ok (! v1.pass() && v1.rejections.joinIntoString ("|").contains ("--tester"),
            "no tester name -> refused, with the fix named");

        // 2. Set the explicit local name. The map AS SUBMITTED by the pre-M10
        //    binary has empty machine_id/apply_header_sha -- the gate must
        //    refuse it naming provenance. (Real finding: pre-M10 maps need
        //    re-submission through the stamping binary before upload.)
        ledger.getRoot().getChildFile ("tester.json")
              .replaceWithText ("{\"name\": \"" + tester + "\"}");
        auto vPre = Mouth::structuralGate (juce::JSON::parse (local.loadFileAsString()), testerName());
        ok (! vPre.pass() && vPre.rejections.joinIntoString ("|").contains ("provenance."),
            "pre-M10 map (empty machine_id) refused naming provenance");

        // 2b. Stamp provenance from the SAME sources submitMap uses now, then
        //     the gate passes. This simulates a map submitted by this binary.
        {
            auto mv = juce::JSON::parse (local.loadFileAsString());
            auto prov = mv.getProperty ("provenance", juce::var());
            if (auto* pd = prov.getDynamicObject())
            {
                pd->setProperty ("tester_id", testerName());
                pd->setProperty ("machine_id", machineIdString());
                pd->setProperty ("ejmap_version", juce::String (EJMAP_VERSION) + " (" + EJMAP_GIT_HASH + ")");
                pd->setProperty ("apply_header_sha", juce::String (EJMAP_APPLY_HEADER_SHA));
            }
            local.replaceWithText (juce::JSON::toString (mv, false));
        }
        auto v2 = Mouth::structuralGate (juce::JSON::parse (local.loadFileAsString()), testerName());
        for (const auto& r : v2.rejections) std::cout << "    gate: " << r << std::endl;
        ok (v2.pass(), "gate passes once provenance is stamped by this binary's sources");

        // 3. Corrupt variants: each rejection NAMES its defect.
        {
            auto bad = juce::JSON::parse (local.loadFileAsString());
            bad.getDynamicObject()->setProperty ("schema", "9.9");
            auto vb = Mouth::structuralGate (bad, testerName());
            ok (! vb.pass() && vb.rejections.joinIntoString ("|").contains ("schema"),
                "wrong schema refused by name");
        }
        {
            auto bad = juce::JSON::parse (local.loadFileAsString());
            auto params = bad.getProperty ("params", juce::var());
            if (auto* po = params.getDynamicObject(); po != nullptr && po->getProperties().size() > 0)
            {
                auto first = po->getProperties().getName (0);
                po->getProperty (first).getDynamicObject()->setProperty ("index", 99999);
                auto vb = Mouth::structuralGate (bad, testerName());
                ok (! vb.pass() && vb.rejections.joinIntoString ("|").contains ("out of range"),
                    "out-of-range index refused by name");
            }
        }

        // 4. The upload card end to end: gate, dry run, stub.
        openUploadCard();
        auto dry = ledger.getRoot().getChildFile ("upload").getChildFile (currentFp + ".http");
        ok (dry.existsAsFile(), "dry-run request written");
        {
            juce::MemoryBlock all; dry.loadFileAsData (all);
            const auto text = dry.loadFileAsString();
            const auto headEnd = text.indexOf ("\r\n\r\n");
            ok (headEnd > 0 && text.startsWith ("POST /"),
                "dry run is an exact HTTP request with an absolute path");
            const int bodyBytes = (int) all.getSize() - (headEnd + 4);
            const auto clLine = text.upToFirstOccurrenceOf ("\r\n\r\n", false, false);
            ok (clLine.contains ("Content-Length: " + juce::String (bodyBytes)),
                "Content-Length matches the exact body bytes (" + juce::String (bodyBytes) + ")");
            juce::MemoryBlock mapBytes; local.loadFileAsData (mapBytes);
            ok ((int) mapBytes.getSize() == bodyBytes,
                "body is the map file verbatim");
            ok (text.contains (".invalid") || juce::SystemStats::getEnvironmentVariable ("EJMAP_UPLOAD_URL", "").isNotEmpty(),
                "URL is the clearly-unset placeholder until a real endpoint exists");
        }

        // 5. Store read-back, field by field, against the STUB'S OWN STORE --
        //    the stored data, never the emitted event.
        auto stored = ledger.getRoot().getChildFile ("stub-store").getChildFile (currentFp + ".json");
        ok (stored.existsAsFile(), "stub store holds the accepted map");
        {
            auto a = juce::JSON::parse (local.loadFileAsString());
            auto b = juce::JSON::parse (stored.loadFileAsString());
            ok (juce::JSON::toString (a, false) == juce::JSON::toString (b, false),
                "store read-back matches the local JSON field by field");
        }
        ok (Mouth::queueState (ledger.getRoot(), currentFp) == "stub_accepted",
            "queue state is stub_accepted -- 'uploaded' does not exist yet");
        ok (uploadCardText.contains ("NOTHING HAS BEEN UPLOADED"),
            "the card says NOTHING HAS BEEN UPLOADED, in those words");
        ok (uploadCardText.contains ("STUB MOUTH (not the real endpoint)"),
            "every stub line carries the label");

        std::cout << "UPLOADTEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        quitNow();
    }

    /** Drives the Tier 2 controls phase: sweep, flagged-only table, exclude,
        accept, submit -- then applies a named control LIVE by name through
        the real applySettings against the just-written map.
    */
    void selfTestControls (const juce::String& identifier, const juce::String& modeName,
                           const juce::String& modeLabel, const juce::String& expectNames)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "CTRLTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        { LedgerRecord rec; rec.pluginId = loadedId; rec.name = desc.name;
          rec.outcome = res.outcome; rec.detail = res.detail; rec.paramCount = res.paramCount;
          ledger.endLoad (rec); }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "CTRLTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        listeners.attach (*inst);
        cal = capture.calibrate (*inst, loadedId);
        mask = capture.buildNoiseMask (*inst, cal, loadedId);
        capture.resetCycleCounts();
        promotionsFlushed = 0;
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);

        failures = 0;
        auto ok = [this] (bool cond, const juce::String& what)
        {
            if (! cond) ++failures;
            std::cout << "  " << (cond ? "ok   " : "FAIL ") << what << std::endl;
            std::cout.flush();
        };

        std::cout << "CTRLTEST: " << desc.name << " | " << cal.paramCount << " params" << std::endl;
        startAssignmentForCategory ("compressor");

        int ctrlRow = -1;
        for (int i = 0; i < assignPanel.rows.size(); ++i)
            if (assignPanel.rows.getReference (i).kind == "controls") { ctrlRow = i; break; }
        ok (ctrlRow >= 0, "the controls row exists");
        assignPanel.selectRow (ctrlRow);
        std::cout << "---- the controls card ----\n" << assignPanel.textRender() << std::endl;

        assignPanel.dispatchAction ("space");     // sweep + table
        const auto table = assignPanel.bandTableText();
        std::cout << "---- controls table (default view) ----\n"
                  << assignPanel.textRender() << std::endl;

        ok (table.contains ("swept clean, recorded as setread"),
            "flagged-only default: the clean majority is a COUNT line");
        if (modeName.isNotEmpty())
            ok (table.contains (modeName), "the mode finding is a decision row: " + modeName);

        // A clean control's name must NOT be in the default view but MUST
        // appear on F. Find one: first entry not needing a decision.
        juce::String cleanName;
        for (const auto& e : assignPanel.controlEntries)
            if (! e.needsDecision()) { cleanName = e.name; break; }
        if (cleanName.isNotEmpty())
        {
            ok (! table.contains (cleanName + "  ["),
                "clean control '" + cleanName + "' hidden by default");
            assignPanel.dispatchAction ("expand");
            ok (assignPanel.bandTableText().contains (cleanName + "  ["),
                "F expands to show it");
            assignPanel.dispatchAction ("expand");
        }

        // Exclude one clean control, recorded.
        if (cleanName.isNotEmpty())
        {
            assignPanel.selectControlByName (cleanName);
            assignPanel.dispatchAction ("notpresent");
            ok (ledger.runArtifact ("captures", "jsonl").loadFileAsString()
                  .contains ("control_excluded"),
                "exclusion recorded as a row");
        }

        assignPanel.dispatchAction ("space");     // accept
        const int builtControls = assignPanel.controlsForSubmit().size();
        ok (builtControls > 0,
            "accept built " + juce::String (builtControls) + " named controls");

        // THE KILL-AT-REVIEW PATH, exercised as the app lives it: re-begin
        // from disk (what a process restart does) and the cargo must ride
        // the session file, not just the row's claim. The spiff gate failed
        // exactly here: the row restored CONFIRMED while controls:{} was
        // written, and the old self-test never restarted so it never saw it.
        startAssignmentForCategory ("compressor");
        ok (assignPanel.controlsForSubmit().size() == builtControls,
            "restore keeps the cargo: " + juce::String (assignPanel.controlsForSubmit().size())
              + " of " + juce::String (builtControls) + " controls after re-begin");

        // W on the resolved controls row RE-SWEEPS, never arms a parameter
        // capture (the spiff run armed five times and once captured boost
        // depth as if "controls" were a knob).
        {
            int cr = -1;
            for (int i = 0; i < assignPanel.rows.size(); ++i)
                if (assignPanel.rows.getReference (i).kind == "controls") { cr = i; break; }
            assignPanel.selectRow (cr);
            assignPanel.dispatchAction ("wiggle");
            ok (assignPanel.controlsPhase,
                "W on the controls row re-sweeps the surface (table reopened, no capture armed)");
            assignPanel.dispatchAction ("space");   // accept again
        }

        assignPanel.dispatchAction ("submit");
        ok (assignPanel.isSummaryShowing() && assignPanel.isSubmitEnabled(),
            "review offers submit (controls row is confirmed work)");
        assignPanel.confirmSubmitFromSummary();

        auto f = ledger.getRoot().getChildFile ("maps").getChildFile (currentFp + ".json");
        ok (f.existsAsFile(), "map written");
        auto map = juce::JSON::parse (f.loadFileAsString());
        auto controls = map.getProperty ("controls", juce::var());
        ok (controls.isObject(), "map carries controls{}");
        if (cleanName.isNotEmpty())
            ok (! controls.getProperty (cleanName, juce::var()).isObject(),
                "excluded control absent from the map");

        // Named expectations (the spiff gate names).
        if (expectNames.isNotEmpty())
        {
            juce::StringArray names;
            names.addTokens (expectNames, ";", "");
            for (const auto& nm : names)
                ok (controls.getProperty (nm.trim(), juce::var()).isObject(),
                    "named control present: " + nm.trim());
        }

        // MAP TRUTH, asserted directly: set the label's recorded norm with a
        // pumped settle and read what the plugin displays. This bypasses
        // applyOne's readback, which the probe below shows reading the
        // PRE-WRITE state on bridged AUs.
        if (modeName.isNotEmpty())
        {
            juce::String truth;
            if (auto* lo = map.getProperty ("controls", juce::var())
                              .getProperty (modeName, juce::var())
                              .getProperty ("labels", juce::var()).getDynamicObject())
                for (auto& kv : lo->getProperties())
                    if (kv.name.toString() == modeLabel)
                        truth = assignPanel.hooks.spotCheck
                                  ? assignPanel.hooks.spotCheck (paramIndexByName (modeName),
                                                                 (double) kv.value)
                                  : juce::String();
            ok (truth == modeLabel,
                "map truth: setting " + modeName + "'s recorded norm for '" + modeLabel
                  + "' displays \"" + truth + "\"");

            // THE FINDING, reported not asserted: applyOne's immediate
            // readback on a BRIDGED AU reads the pre-write display and
            // falsely reverts a correct write. Shared-header behaviour;
            // the fix is a decision for the plugin side.
            auto* st = new juce::DynamicObject();
            st->setProperty (modeName, modeLabel);
            host.pausePumpForMutation();
            auto results = echojay::applySettings (*inst, map, juce::var (st));
            host.resumePumpAfterMutation();
            if (results.size() == 1)
                std::cout << "  FINDING: applyOne verdict on the bridge: "
                          << (results[0].applied ? "applied" : "REVERTED (stale readback)")
                          << " - " << results[0].note << std::endl;
        }
        // Anchored map truth the same way: recorded mid-anchor norm displays
        // its recorded value.
        {
            for (const auto& c : assignPanel.controlsForSubmit())
                if (c.kind == "anchored" && ! c.duplicate && c.anchors.size() >= 3
                     && ! c.identityDisplay && c.name != cleanName)
                {
                    const int mid = c.anchors.size() / 2;
                    const auto landed = assignPanel.hooks.spotCheck
                                          ? assignPanel.hooks.spotCheck (c.indices[0],
                                                                         c.anchors[mid].normalised)
                                          : juce::String();
                    double v = 0.0;
                    const bool parsed = echojay::parseLeadingFloat (landed, v);
                    ok (parsed && std::abs (v - c.anchors[mid].value)
                                    <= juce::jmax (0.02 * std::abs (c.rangeHi - c.rangeLo), 0.51),
                        "map truth: '" + c.name + "' mid anchor norm displays \"" + landed
                          + "\" vs recorded " + juce::String (c.anchors[mid].value, 2));
                    break;
                }
        }

        std::cout << "CTRLTEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        quitNow();
    }

    /** Reproduces the AMEK category defect and proves the fix: no proposal
        -> the panel ASKS before any row exists; work confirmed under a wrong
        category SURVIVES the correction; the checklist rebuilds around it.
    */
    void selfTestCategory (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "CATTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        { LedgerRecord rec; rec.pluginId = loadedId; rec.name = desc.name;
          rec.outcome = res.outcome; rec.detail = res.detail; rec.paramCount = res.paramCount;
          ledger.endLoad (rec); }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "CATTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        listeners.attach (*inst);
        cal = capture.calibrate (*inst, loadedId);
        mask = capture.buildNoiseMask (*inst, cal, loadedId);
        capture.resetCycleCounts();
        promotionsFlushed = 0;
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);

        auto ok = [this] (bool cond, const juce::String& what)
        {
            if (! cond) ++failures;
            std::cout << "  " << (cond ? "ok   " : "FAIL ") << what << std::endl;
            std::cout.flush();
        };

        std::cout << "CATTEST: " << desc.name << std::endl;
        failures = 0;

        startAssignment();       // the normal human path, no override
        ok (assignPanel.isAwaitingCategory() && assignPanel.rows.isEmpty(),
            "no proposal -> the panel ASKS before any row exists");
        std::cout << "---- the ask card ----\n" << assignPanel.textRender() << std::endl;

        assignPanel.dispatchAction ("space");
        ok (assignPanel.isAwaitingCategory(),
            "every action refused until the category is chosen");

        assignPanel.pickCategory ("compressor");   // the WRONG one, as lived
        ok (! assignPanel.isAwaitingCategory() && assignPanel.rows.size() > 0
              && assignPanel.progressText().startsWith ("compressor"),
            "wrong pick builds the compressor checklist, category visible in the header");

        // Confirm input_db -> Input Gain under the wrong category.
        int inputRow = -1;
        for (int i = 0; i < assignPanel.rows.size(); ++i)
            if (assignPanel.rows.getReference (i).semantic == "input_db") { inputRow = i; break; }
        assignPanel.selectRow (inputRow);
        const int inputIdx = paramIndexByName ("Input Gain");
        inst->getParameters()[inputIdx]->setValueNotifyingHost (0.20f);
        juce::Thread::sleep (120);
        assignPanel.actionWiggle();
        juce::Timer::callAfterDelay (300, [this, inputIdx]
        {
            auto* i2 = host.getInstance();
            if (i2 != nullptr) i2->getParameters()[inputIdx]->setValueNotifyingHost (0.55f);
        });
        stage = 0;
        juce::Timer::callAfterDelay (1400, [this, ok] { categoryTestPart2 (ok); });
    }

    void categoryTestPart2 (std::function<void (bool, const juce::String&)> ok)
    {
        int inputRow = -1;
        for (int i = 0; i < assignPanel.rows.size(); ++i)
            if (assignPanel.rows.getReference (i).semantic == "input_db") { inputRow = i; break; }
        ok (inputRow >= 0
              && assignPanel.rows.getReference (inputRow).state == AssignRow::State::confirmed,
            "input_db confirmed under the wrong category");

        assignPanel.pickCategory ("eq");           // the correction

        int inputAfter = -1, bandsRow = -1, kneeRow = -1;
        for (int i = 0; i < assignPanel.rows.size(); ++i)
        {
            const auto& r = assignPanel.rows.getReference (i);
            if (r.semantic == "input_db") inputAfter = i;
            if (r.kind == "bands")        bandsRow = i;
            if (r.semantic == "knee_db")  kneeRow = i;
        }
        ok (inputAfter >= 0
              && assignPanel.rows.getReference (inputAfter).state == AssignRow::State::confirmed,
            "input_db SURVIVES the category change as a confirmed extra (no re-doing)");
        ok (bandsRow >= 0, "the bands flow row exists after the correction");
        ok (kneeRow < 0, "unresolved compressor-only rows are gone (knee_db)");
        ok (assignPanel.progressText().startsWith ("eq"),
            "the header shows the corrected category");
        ok (ledger.runArtifact ("captures", "jsonl").loadFileAsString()
              .contains ("category_changed"),
            "the correction is recorded as evidence");
        std::cout << "---- after the correction ----\n" << assignPanel.textRender() << std::endl;

        std::cout << "CATTEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        quitNow();
    }

    /** Drives the band flow end to end: guided captures with synthetic
        touches, the table, the exclusion footer (which must be explicit even
        when empty), accept, and a LIVE apply through the real applySettings
        asserting the imposter's value never moves. memberSpec is
        "freq1;gain1;q1;lastFreq" by parameter NAME; "-" for a missing Q.
        imposterName may be empty (footer must then say none-found explicitly).
    */
    void selfTestBands (const juce::String& identifier, const juce::String& memberSpec,
                        const juce::String& imposterName)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "BANDTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        { LedgerRecord rec; rec.pluginId = loadedId; rec.name = desc.name;
          rec.outcome = res.outcome; rec.detail = res.detail; rec.paramCount = res.paramCount;
          ledger.endLoad (rec); }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "BANDTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        listeners.attach (*inst);
        cal = capture.calibrate (*inst, loadedId);
        mask = capture.buildNoiseMask (*inst, cal, loadedId);
        capture.resetCycleCounts();
        promotionsFlushed = 0;

        bandMemberNames.clear();
        bandMemberNames.addTokens (memberSpec, ";", "");
        bandImposterName = imposterName;
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);

        std::cout << "BANDTEST: " << desc.name << " | " << cal.paramCount << " params | members: "
                  << memberSpec << std::endl;

        startAssignmentForCategory ("eq");
        stage = 0;
        failures = 0;
        juce::Timer::callAfterDelay (300, [this] { bandTestStep(); });
    }

    void startAssignmentForCategory (const juce::String& cat)
    {
        auto proposals = ProposalSet::load (ledger.getRoot(), currentFp);
        auto ev        = EvidenceIndex::build (ledger.getRoot(), loadedId);
        assigning = true;
        assignPanel.setVisible (true);
        list.setVisible (false);
        filterBox.setVisible (false);
        assignPanel.begin (ledger.getRoot(), currentFp, loadedId, proposals, ev, cat);
        resized();
    }

    int paramIndexByName (const juce::String& nm)
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) return -1;
        auto& ps = inst->getParameters();
        for (int i = 0; i < ps.size(); ++i)
            if (ps[i]->getName (64).equalsIgnoreCase (nm)) return i;
        return -1;
    }

    void bandTestStep()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) return;
        auto ok = [this] (bool cond, const juce::String& what)
        {
            if (! cond) ++failures;
            std::cout << "  " << (cond ? "ok   " : "FAIL ") << what << std::endl;
            std::cout.flush();
        };
        auto dump = [this] (const char* tag)
        {
            std::cout << "---- after " << tag << " ----\n"
                      << assignPanel.textRender() << std::endl;
        };

        if (stage == 0)
        {
            int bandsRow = -1;
            for (int i = 0; i < assignPanel.rows.size(); ++i)
                if (assignPanel.rows.getReference (i).kind == "bands") { bandsRow = i; break; }
            ok (bandsRow >= 0, "eq category folds freq/gain/q into ONE bands row");
            assignPanel.selectRow (bandsRow);
            dump ("selecting the bands row");
            assignPanel.dispatchAction ("space");
            ok (assignPanel.currentBandStep() == AssignPanel::BandStep::capFreq1,
                "SPACE begins the flow; the freq card is LIVE");
            dump ("bands begin (freq card armed)");
            stage = 1; bandMemberCursor = 0;
            juce::Timer::callAfterDelay (400, [this] { bandTestDriveCapture(); });
            return;
        }

        if (stage == 2)     // table reached
        {
            const auto table = assignPanel.bandTableText();
            std::cout << "---- BAND TABLE ----\n" << table << std::endl;
            ok (assignPanel.currentBandStep() == AssignPanel::BandStep::table,
                "all four touches landed: the table is up");
            ok (table.contains ("WATCHED & UNMOVED"),
                "the exclusion footer is present");
            if (bandImposterName.isNotEmpty())
                ok (table.contains (bandImposterName),
                    "footer names the imposter: " + bandImposterName);
            else
                ok (table.contains ("(none: among the"),
                    "footer says EXPLICITLY that no imposters were found (never vacuous)");

            assignPanel.dispatchAction ("space");     // accept
            const int builtGroups = assignPanel.groupsForSubmit().size();
            ok (builtGroups >= 2, "accept built " + juce::String (builtGroups) + " band groups");
            dump ("accepting the band table");

            // The kill-at-review path for GROUPS: same hole as controls, found
            // by inspection when the controls one failed live. Re-begin from
            // disk; the groups must ride the session file.
            startAssignmentForCategory ("eq");
            ok (assignPanel.groupsForSubmit().size() == builtGroups,
                "restore keeps the groups: " + juce::String (assignPanel.groupsForSubmit().size())
                  + " of " + juce::String (builtGroups) + " after re-begin");

            // W on the resolved bands row restarts the GUIDED FLOW, not a
            // bare capture; drive it straight back through the four touches.
            {
                int br = -1;
                for (int i = 0; i < assignPanel.rows.size(); ++i)
                    if (assignPanel.rows.getReference (i).kind == "bands") { br = i; break; }
                assignPanel.selectRow (br);
                assignPanel.dispatchAction ("wiggle");
                ok (assignPanel.currentBandStep() == AssignPanel::BandStep::capFreq1,
                    "W on the bands row restarts the guided flow at band 1's freq card");
                bandMemberCursor = 0;
                ++stage;
                juce::Timer::callAfterDelay (400, [this] { bandTestDriveCapture(); });
                return;
            }
        }

        if (stage == 3)
        {
            // Back at the table after the W-redo drive: accept and finish.
            ok (assignPanel.currentBandStep() == AssignPanel::BandStep::table,
                "W-redo drove back to the band table");
            assignPanel.dispatchAction ("space");
            const auto& groups = assignPanel.groupsForSubmit();
            ok (groups.size() >= 2, "re-accept rebuilt " + juce::String (groups.size()) + " groups");

            // The [-1] pair (AMEK re-run): bands AND controls both confirmed,
            // both carrying the placeholder, and the duplicate check refused
            // submit with nothing in the UI able to clear it. The bands row
            // here is real (just accepted); the controls row is set confirmed
            // directly -- the -1 pair is the shape under test, not the
            // controls flow -- with one control staged so the cargo refusal
            // stays quiet.
            {
                int cr = -1;
                for (int i = 0; i < assignPanel.rows.size(); ++i)
                    if (assignPanel.rows.getReference (i).kind == "controls") { cr = i; break; }
                ok (cr >= 0, "a controls surface row exists to pair with bands");
                const auto savedState = assignPanel.rows.getReference (cr).state;
                assignPanel.rows.getReference (cr).state = AssignRow::State::confirmed;
                assignPanel.rows.getReference (cr).skipReason = "1 control (synthetic, selftest)";
                NamedControl syn;
                syn.name = "synthetic control (selftest)";
                syn.indices.add (0);
                assignPanel.pendingControls.add (syn);

                ok (! assignPanel.duplicateConflicts().joinIntoString ("|").contains ("[-1]"),
                    "two surface placeholders are NOT a duplicate-index conflict");

                // Positive control: the check must still catch two PARAMETER
                // rows on one index (the API-2500 class), or the exclusion
                // quietly neutered it. The bands flow confirms no Tier 1 rows,
                // so both sides are manufactured on real rows and reverted --
                // the shape is the subject, and the shape is state + index.
                int pa = -1, pb = -1;
                for (int i = 0; i < assignPanel.rows.size(); ++i)
                {
                    const auto& r = assignPanel.rows.getReference (i);
                    if (r.kind == "bands" || r.kind == "controls") continue;
                    if (pa < 0) pa = i; else if (pb < 0) { pb = i; break; }
                }
                ok (pa >= 0 && pb >= 0, "two parameter rows exist for the positive control");
                if (pa >= 0 && pb >= 0)
                {
                    auto& ra = assignPanel.rows.getReference (pa);
                    auto& rb = assignPanel.rows.getReference (pb);
                    const auto sa = ra.state;  const int ia = ra.resolvedIndex;
                    const auto sb = rb.state;  const int ib = rb.resolvedIndex;
                    ra.state = AssignRow::State::confirmed; ra.resolvedIndex = 5;
                    rb.state = AssignRow::State::confirmed; rb.resolvedIndex = 5;
                    ok (! assignPanel.duplicateConflicts().isEmpty(),
                        "two parameter rows on one index still refuse (check not neutered)");
                    ra.state = sa; ra.resolvedIndex = ia;
                    rb.state = sb; rb.resolvedIndex = ib;
                }

                assignPanel.rows.getReference (cr).state = savedState;
                assignPanel.rows.getReference (cr).skipReason = {};
                assignPanel.pendingControls.removeLast();
            }

            // LIVE APPLY through the real applySettings: the headline
            // assertion on the real plugin.
            MapPayload p;
            p.fp = currentFp; p.category = "eq";
            for (const auto& g : groups) p.groups.add (g);
            auto map = juce::JSON::parse (p.toJson());

            auto& ps = inst->getParameters();
            const int imposterIdx = bandImposterName.isNotEmpty()
                                      ? paramIndexByName (bandImposterName) : -1;
            const float impBefore = imposterIdx >= 0 ? ps[imposterIdx]->getValue() : 0.0f;

            juce::SortedSet<int> freqMembers;
            for (const auto& g : groups)
                for (const auto& m : g.params)
                    if (m.semantic == "freq_hz") freqMembers.add (m.indices[0]);

            host.pausePumpForMutation();
            auto* settings = new juce::DynamicObject();
            settings->setProperty ("freq_hz", 250);
            settings->setProperty ("gain_db", 3);
            auto results = echojay::applySettings (*inst, map, juce::var (settings));
            host.resumePumpAfterMutation();

            int landed = -1; juce::String note;
            for (const auto& r : results)
                if (r.semantic == "freq_hz" && r.applied) { landed = r.index; note = r.note; }
            ok (landed >= 0 && freqMembers.contains (landed),
                "250 Hz landed INSIDE the group: [" + juce::String (landed) + "] " + note);
            if (imposterIdx >= 0)
                ok (juce::approximatelyEqual (ps[imposterIdx]->getValue(), impBefore),
                    bandImposterName + "'s VALUE is untouched by the 250 Hz request");

            // 8 kHz must land on a band whose own range covers it, and not
            // the same band 250 Hz used unless its range genuinely spans both.
            const float impBefore2 = imposterIdx >= 0 ? ps[imposterIdx]->getValue() : 0.0f;
            auto* band = new juce::DynamicObject();
            band->setProperty ("freq_hz", 8000);
            juce::Array<juce::var> bandsArr; bandsArr.add (juce::var (band));
            auto* settings2 = new juce::DynamicObject();
            settings2->setProperty ("bands", juce::var (bandsArr));
            host.pausePumpForMutation();
            auto results2 = echojay::applySettings (*inst, map, juce::var (settings2));
            host.resumePumpAfterMutation();
            int landed2 = -1;
            for (const auto& r : results2)
                if (r.semantic == "freq_hz" && r.applied) landed2 = r.index;
            bool rangeOk = false;
            for (const auto& g : groups)
                for (const auto& m : g.params)
                    if (m.semantic == "freq_hz" && m.indices[0] == landed2)
                        rangeOk = g.freqHi >= 8000.0;
            ok (landed2 >= 0 && freqMembers.contains (landed2) && rangeOk,
                "8 kHz landed inside the group on a band whose range covers it ["
                  + juce::String (landed2) + "]");
            if (imposterIdx >= 0)
                ok (juce::approximatelyEqual (ps[imposterIdx]->getValue(), impBefore2),
                    bandImposterName + " untouched at 8 kHz");

            std::cout << "BANDTEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
            std::cout.flush();
            quitNow();
            return;
        }
    }

    /** Feeds the guided cards: pre-set the named param, then move it, letting
        the live card capture it exactly as a human touch would.
    */
    void bandTestDriveCapture()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) return;

        if (assignPanel.bandPickPending() && bandMemberCursor >= 1)
        {
            // Print the pick AS RENDERED: it must be the card (headline +
            // numbered buttons), not a status line under someone else's card.
            std::cout << "---- lockstep pick card ----\n"
                      << assignPanel.textRender() << std::endl;
            const int prev = paramIndexByName (bandMemberNames[bandMemberCursor - 1].trim());
            assignPanel.bandPickByParamIndex (prev);
            juce::Timer::callAfterDelay (300, [this] { bandTestDriveCapture(); });
            return;
        }

        if (assignPanel.currentBandStep() == AssignPanel::BandStep::table)
        { if (stage < 2) stage = 2; bandTestStep(); return; }   // W-redo re-arrives at stage 3

        if (bandMemberCursor >= bandMemberNames.size())
        { std::cout << "BANDTEST: ran out of member names before the table" << std::endl;
          std::cout.flush(); quitNow(); return; }

        const auto nm = bandMemberNames[bandMemberCursor++].trim();
        if (nm == "-")
        {
            assignPanel.dispatchAction ("notpresent");     // no Q on this band
            juce::Timer::callAfterDelay (400, [this] { bandTestDriveCapture(); });
            return;
        }

        const int idx = paramIndexByName (nm);
        if (idx < 0)
        { std::cout << "BANDTEST: no param named '" << nm << "'" << std::endl;
          std::cout.flush(); quitNow(); return; }

        // A lockstep pair from the PREVIOUS touch may be waiting for a pick:
        // pick the control the driver actually meant, by index, as a human
        // does by reading the names.
        if (assignPanel.bandPickPending())
        {
            const int prev = paramIndexByName (bandMemberNames[bandMemberCursor - 2].trim());
            assignPanel.bandPickByParamIndex (prev);
        }

        // ONE write per card. The first version pre-set then moved on a
        // delay, and the delayed move raced the NEXT card's auto-arm: every
        // card captured the previous control's leftover write. A human
        // touches once; the driver now does too.
        {
            auto* pp = inst->getParameters()[idx];
            const float v = pp->getValue();
            pp->setValueNotifyingHost (v < 0.5f ? v + 0.3f : v - 0.3f);
        }
        juce::Timer::callAfterDelay (900, [this] { bandTestDriveCapture(); });
    }

    /** Drives the assignment loop through the SAME action methods the keys
        call, against a SYNTHETIC proposal file written for this plugin's own
        fp (and saying so): the corroboration gate, the W mismatch dataset,
        the bulk-ignore floor, the skip records and the submit are all the
        production paths; only the proposal content and the finger on the
        keys are simulated.
    */
    void selfTestAssign (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "ASSIGNTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        { LedgerRecord rec; rec.pluginId = loadedId; rec.name = desc.name;
          rec.outcome = res.outcome; rec.detail = res.detail; rec.paramCount = res.paramCount;
          ledger.endLoad (rec); }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "ASSIGNTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        listeners.attach (*inst);
        cal = capture.calibrate (*inst, loadedId);
        mask = capture.buildNoiseMask (*inst, cal, loadedId);
        capture.resetCycleCounts();
        promotionsFlushed = 0;

        // Qualify four continuous write-responsive params, as the capture test does.
        auto& params = inst->getParameters();
        qualified.clearQuick();
        for (int i = 0; i < params.size() && qualified.size() < 4; ++i)
        {
            if (mask.indices.contains (i)) continue;
            auto* pp = params[i];
            if (pp->isDiscrete() || ! pp->isAutomatable()) continue;
            pp->setValueNotifyingHost (0.20f); juce::Thread::sleep (15);
            const float lo = pp->getValue();
            pp->setValueNotifyingHost (0.50f); juce::Thread::sleep (15);
            const float hi = pp->getValue();
            if (std::abs (lo - 0.20f) < 0.02f && std::abs (hi - 0.50f) < 0.02f) qualified.add (i);
        }
        if (qualified.size() < 4)
        { std::cout << "ASSIGNTEST: too few qualified params" << std::endl; quitNow(); return; }

        // A discrete param whose displays are LABELS, not numbers: the knee
        // class. Found by behaviour (getText both ends, neither parses), never
        // by name.
        kneeTestIdx = -1;
        for (int i = 0; i < params.size(); ++i)
        {
            auto* pp = params[i];
            if (! pp->isDiscrete() || ! pp->isAutomatable()) continue;
            double v = 0.0;
            if (! echojay::parseLeadingFloat (pp->getText (0.0f, 64), v)
                 && ! echojay::parseLeadingFloat (pp->getText (1.0f, 64), v))
            { kneeTestIdx = i; break; }
        }

        // Two ignore subjects: one whose name matches no dial-set token
        // (eligible for bulk) and one whose name does (must be withheld).
        int ignEligible = -1, ignWithheld = -1;
        for (int i = 0; i < params.size(); ++i)
        {
            const auto nm = params[i]->getName (48);
            if (qualified.contains (i) || i == kneeTestIdx) continue;   // the knee switch gets captured mid-test
            if (ignEligible < 0 && ! DialSets::nameSuggestsDialSet (nm)) ignEligible = i;
            if (ignWithheld < 0 &&   DialSets::nameSuggestsDialSet (nm)) ignWithheld = i;
            if (ignEligible >= 0 && ignWithheld >= 0) break;
        }

        // The synthetic proposal file, for THIS plugin's own fp.
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);
        auto pdir = ledger.getRoot().getChildFile ("proposals");
        pdir.createDirectory();
        juce::String pj;
        pj << "{ \"category\": \"compressor\", \"params\": ["
           << "{\"index\": " << qualified[0] << ", \"kind\": \"threshold_db\", \"confidence\": \"high\", \"reason\": \"synthetic proposal (selftest)\"},"
           << "{\"index\": " << qualified[1] << ", \"kind\": \"ratio\", \"confidence\": \"med\", \"reason\": \"synthetic WRONG proposal (selftest)\"},"
           << "{\"index\": " << qualified[3] << ", \"kind\": \"attack_ms\", \"confidence\": \"high\", \"reason\": \"synthetic corroborated proposal (selftest)\"},"
           << (kneeTestIdx >= 0
                 ? juce::String ("{\"index\": ") + juce::String (kneeTestIdx)
                     + ", \"kind\": \"knee_db\", \"confidence\": \"med\", \"reason\": \"synthetic knee at a discrete switch (selftest)\"},"
                 : juce::String())
           << "{\"index\": " << ignEligible << ", \"kind\": \"ignore\", \"confidence\": \"high\", \"reason\": \"synthetic utility (selftest)\"},"
           << "{\"index\": 0, \"kind\": \"mode\", \"confidence\": \"med\", \"reason\": \"synthetic switch A (selftest)\"},"
           << "{\"index\": 1, \"kind\": \"mode\", \"confidence\": \"med\", \"reason\": \"synthetic switch B (selftest)\"},"
           << "{\"index\": " << ignWithheld << ", \"kind\": \"ignore\", \"confidence\": \"high\", \"reason\": \"synthetic but dial-set-named (selftest)\"}"
           << "] }";
        pdir.getChildFile (currentFp + ".json").replaceWithText (pj);
        std::cout << "ASSIGNTEST: " << desc.name << " | fp " << currentFp.substring (0, 12)
                  << "... | SYNTHETIC proposals written for this fp (and said so)" << std::endl;

        // Pre-capture qualified[3] so the attack_ms proposal is corroborated
        // by a real captures row on disk before assignment begins.
        params[qualified[3]]->setValueNotifyingHost (0.20f);
        juce::Thread::sleep (120);
        capture.arm (*inst, cal, mask, [this] (const CaptureEngine::Result& r)
        {
            const int intended = r.indices.size() == 1 ? r.indices[0] : -1;
            recordCapture (r.kindString(), intended,
                           r.names.isEmpty() ? juce::String() : r.names[0],
                           {}, {}, r.reason, r.sameDirection, r.magnitudeRatio, r.capturedBy);
            stage = 0;
            juce::Timer::callAfterDelay (300, [this] { assignTestStep(); });
        });
        juce::Timer::callAfterDelay (300, [this]
        {
            auto* in = host.getInstance();
            if (in != nullptr) in->getParameters()[qualified[3]]->setValueNotifyingHost (0.55f);
        });
    }

    void assignTestStep()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) return;
        auto& params = inst->getParameters();

        auto findRow = [this] (const juce::String& sem) -> int
        {
            for (int i = 0; i < assignPanel.rows.size(); ++i)
                if (assignPanel.rows.getReference (i).semantic == sem) return i;
            return -1;
        };
        auto ok = [this] (bool cond, const juce::String& what)
        {
            if (! cond) ++failures;
            std::cout << "  " << (cond ? "ok   " : "FAIL ") << what << std::endl;
            std::cout.flush();
        };
        // The rendering, printed after every action: the loop must be
        // judgeable from the transcript alone -- where am I, what happened,
        // what next.
        auto dump = [this] (const char* tag)
        {
            std::cout << "---- after " << tag << " ----\n"
                      << assignPanel.textRender() << std::endl;
        };

        switch (stage)
        {
        case 0:
        {
            startAssignment();
            ok (assigning && assignPanel.rows.size() > 0, "assignment began with rows");
            ok (assignPanel.selectedRow() == 0, "queue: entering Assign lands on row one");
            dump ("enter");

            // THE ROW STATES ITS QUESTION. Print what a human now reads for
            // the three shapes the first stopwatch run tripped over: a
            // proposal row, an unmapped row, and two mode switches that must
            // not wear one uniform.
            juce::StringArray modeLabels;
            for (int i = 0; i < assignPanel.rows.size(); ++i)
            {
                auto& rr = assignPanel.rows.getReference (i);
                if (rr.semantic == "mode") modeLabels.add (assignPanel.displayLabel (rr));
            }
            ok (modeLabels.size() == 2 && modeLabels[0] != modeLabels[1],
                "two mode rows render distinctly: '" + modeLabels[0] + "' vs '" + modeLabels[1] + "'");

            assignPanel.selectRow (findRow ("threshold_db"));
            std::cout << "  QUESTION(proposal): "
                      << assignPanel.currentQuestionText().replace ("\n", " / ") << std::endl;
            assignPanel.selectRow (findRow ("makeup_db"));
            std::cout << "  QUESTION(unmapped): "
                      << assignPanel.currentQuestionText().replace ("\n", " / ") << std::endl;

            // Uncorroborated SPACE refused: the ratio proposal has no evidence.
            // The legend must agree with the strip BEFORE the action runs.
            const int r = findRow ("ratio");
            assignPanel.selectRow (r);
            ok (! assignPanel.keyValid ("space"),
                "legend: SPACE greyed on the uncorroborated row");
            assignPanel.dispatchAction ("space");
            ok (r >= 0 && assignPanel.rows.getReference (r).state == AssignRow::State::proposed,
                "uncorroborated SPACE refused (ratio row unchanged)");
            ok (assignPanel.textRender().contains ("KEY REFUSED (SPACE)"),
                "the refusal is audible: KEY REFUSED in the status line");
            dump ("SPACE on uncorroborated ratio");

            // A mode row is M immediately, no wiggle: the third run had six
            // of fifteen rows in this shape.
            int modeRow = -1;
            for (int i = 0; i < assignPanel.rows.size(); ++i)
                if (assignPanel.rows.getReference (i).semantic == "mode") { modeRow = i; break; }
            assignPanel.selectRow (modeRow);
            ok (modeRow >= 0 && assignPanel.keyValid ("modematerial"),
                "mode row: M lit with no wiggle required");
            assignPanel.dispatchAction ("modematerial");
            ok (assignPanel.rows.getReference (modeRow).state == AssignRow::State::modeMaterial
                  && assignPanel.rows.getReference (modeRow).trust == "llm-classified",
                "mode row resolved by M alone, trust llm-classified (no hand touched it)");

            // The second mode switch is its own finding, not a competitor:
            // resolving the first must NOT supersede it.
            int modeRow2 = -1;
            for (int i = modeRow + 1; i < assignPanel.rows.size(); ++i)
                if (assignPanel.rows.getReference (i).semantic == "mode") { modeRow2 = i; break; }
            ok (modeRow2 >= 0 && ! assignPanel.rows.getReference (modeRow2).isResolved(),
                "second mode row NOT superseded by the first (six switches = six findings)");
            assignPanel.selectRow (modeRow2);
            assignPanel.dispatchAction ("modematerial");
            ok (assignPanel.rows.getReference (modeRow2).state == AssignRow::State::modeMaterial,
                "second mode row resolves independently");
            dump ("M on both mode rows");

            // Corroborated SPACE accepted: attack_ms has a capture on disk.
            const int a = findRow ("attack_ms");
            assignPanel.selectRow (a);
            ok (assignPanel.keyValid ("space"),
                "legend: SPACE lit on the corroborated row");
            assignPanel.actionSpace();
            ok (a >= 0 && assignPanel.rows.getReference (a).state == AssignRow::State::confirmed
                  && assignPanel.rows.getReference (a).corroboration == "capture"
                  && assignPanel.rows.getReference (a).trust == "llm-classified",
                "corroborated SPACE accepted (attack_ms, corroboration=capture)");
            ok (assignPanel.selectedRow() != a,
                "queue: accepting auto-advanced off the resolved row");
            dump ("SPACE on corroborated attack_ms");

            // W on threshold: move the PROPOSED index.
            assignPanel.selectRow (findRow ("threshold_db"));
            params[qualified[0]]->setValueNotifyingHost (0.20f);
            juce::Thread::sleep (120);
            assignPanel.actionWiggle();
            juce::Timer::callAfterDelay (300, [this]
            {
                auto* in = host.getInstance();
                if (in != nullptr) in->getParameters()[qualified[0]]->setValueNotifyingHost (0.55f);
            });
            ++stage;
            juce::Timer::callAfterDelay (1200, [this] { assignTestStep(); });
            return;
        }
        case 1:
        {
            const int t = findRow ("threshold_db");
            ok (t >= 0 && assignPanel.rows.getReference (t).state == AssignRow::State::confirmed
                  && assignPanel.rows.getReference (t).trust == "human-verified"
                  && ! assignPanel.rows.getReference (t).proposalMismatch,
                "W on the proposed index -> confirmed human-verified, no mismatch");

            // W on ratio but move a DIFFERENT index: the mismatch dataset row.
            assignPanel.selectRow (findRow ("ratio"));
            params[qualified[2]]->setValueNotifyingHost (0.20f);
            juce::Thread::sleep (120);
            assignPanel.actionWiggle();
            juce::Timer::callAfterDelay (300, [this]
            {
                auto* in = host.getInstance();
                if (in != nullptr) in->getParameters()[qualified[2]]->setValueNotifyingHost (0.55f);
            });
            ++stage;
            juce::Timer::callAfterDelay (1200, [this] { assignTestStep(); });
            return;
        }
        case 2:
        {
            // THE KNEE CLASS: a continuous semantic pointed at a discrete
            // labelled control. W captures it, the sweep refuses with
            // nonNumeric, the strip offers M, and M resolves it as its own
            // outcome -- not absent, not deferred.
            if (kneeTestIdx < 0)
            {
                std::cout << "  (no label-displaying discrete param on this plugin; knee class untested)"
                          << std::endl;
                ++stage; assignTestStep(); return;
            }
            const int k = findRow ("knee_db");
            assignPanel.selectRow (k);

            // N against a live proposal warns and waits for the insist:
            // "absent" against evidence is the falsehood class.
            assignPanel.dispatchAction ("notpresent");
            ok (assignPanel.rows.getReference (k).state == AssignRow::State::proposed
                  && assignPanel.textRender().contains ("Press N again to insist"),
                "N over a live proposal warns instead of writing ABSENT");
            dump ("N once on the proposed knee");

            auto* in = host.getInstance();
            in->getParameters()[kneeTestIdx]->setValueNotifyingHost (0.0f);
            juce::Thread::sleep (120);
            assignPanel.actionWiggle();
            juce::Timer::callAfterDelay (300, [this]
            {
                auto* i2 = host.getInstance();
                if (i2 != nullptr) i2->getParameters()[kneeTestIdx]->setValueNotifyingHost (1.0f);
            });
            ++stage;
            juce::Timer::callAfterDelay (1200, [this] { assignTestStep(); });
            return;
        }
        case 3:
        {
            // Measured on the Pro-Q 3 AU: every discrete param's getText is a
            // bare number, so kneeTestIdx can be -1 and these assertions have
            // no subject. Guard, never index row -1. The knee class stays
            // covered by the VST3 run, where the labels exist.
            if (kneeTestIdx >= 0)
            {
            const int k = findRow ("knee_db");
            const auto& kr = assignPanel.rows.getReference (k);
            dump ("W capture on the knee switch");
            ok (k >= 0 && kr.state == AssignRow::State::swept && kr.sweep.nonNumeric,
                "knee: sweep refused with nonNumeric, row held open (not skipped)");
            ok (assignPanel.keyValid ("modematerial"),
                "legend: M lit at the moment the refusal appeared");
            ok (! assignPanel.keyValid ("space"),
                "legend: SPACE not offered for a labelled switch");
            std::cout << "  QUESTION(knee): "
                      << assignPanel.currentQuestionText().replace ("\n", " / ") << std::endl;

            assignPanel.dispatchAction ("modematerial");
            ok (assignPanel.rows.getReference (k).state == AssignRow::State::modeMaterial,
                "M resolved the row as mode_material with the finding recorded");
            dump ("M on the refused knee");
            ok (ledger.runArtifact ("tier2-candidates", "jsonl").existsAsFile(),
                "Tier 2 breadcrumb written (labels for M6 named controls)");
            }

            const int r = findRow ("ratio");
            const auto& rr = assignPanel.rows.getReference (r);
            ok (r >= 0 && rr.state == AssignRow::State::confirmed && rr.proposalMismatch
                  && rr.resolvedIndex == qualified[2],
                "W mismatch: row re-pointed to the captured index");
            ok (ledger.runArtifact ("misclassified", "jsonl").existsAsFile(),
                "mismatch written to misclassified-<run>.jsonl (the labelled dataset)");

            // The three skips, driven through the CLICK path (dispatchAction is
            // what a legend chip fires), proving mouse and key are one code path.
            const int m = findRow ("makeup_db");
            if (m >= 0) { assignPanel.selectRow (m); assignPanel.dispatchAction ("notpresent"); }
            ok (m >= 0 && assignPanel.rows.getReference (m).state == AssignRow::State::skipNotPresent
                  && assignPanel.rows.getReference (m).skipReason.isNotEmpty(),
                "N skip recorded with a canned reason");

            // Bulk ignores: first press counts, second fires; the withheld one stays.
            assignPanel.actionBulkIgnores();
            assignPanel.actionBulkIgnores();
            int accepted = 0, unresolvedIgnores = 0;
            for (const auto& ir : assignPanel.ignoreRows)
            { accepted += ir.isSkipped(); unresolvedIgnores += ! ir.isResolved(); }
            ok (accepted == 1 && unresolvedIgnores == 1,
                "bulk ignores: 1 accepted, 1 withheld by the floor (dial-set name)");

            // THE DUPLICATE GATE. Confirm release_ms onto the SAME index
            // attack_ms holds (the makeup/output [8] defect): review must
            // refuse, submit must be disabled, and W+D must clear it.
            const int rel = findRow ("release_ms");
            assignPanel.selectRow (rel);
            auto* in2 = host.getInstance();
            in2->getParameters()[qualified[3]]->setValueNotifyingHost (0.20f);
            juce::Thread::sleep (120);
            assignPanel.actionWiggle();
            juce::Timer::callAfterDelay (300, [this]
            {
                auto* i3 = host.getInstance();
                if (i3 != nullptr) i3->getParameters()[qualified[3]]->setValueNotifyingHost (0.55f);
            });
            ++stage;
            juce::Timer::callAfterDelay (1200, [this] { assignTestStep(); });
            return;
        }
        case 4:
        {
            // CAPTURE-TIME CONFLICT: the C1 finding. The tool knew at capture
            // that [q3] already belonged to attack_ms; it must ask NOW, in the
            // card, not minutes later at review.
            const int rel = findRow ("release_ms");
            auto& rr = assignPanel.rows.getReference (rel);
            ok (rr.state != AssignRow::State::confirmed && rr.conflictWith == "attack_ms",
                "capture-time: row HELD at the conflict, not confirmed");
            ok (assignPanel.textRender().contains ("already assigned to attack_ms"),
                "the card asks immediately, naming the holder");
            dump ("conflict card at capture time");

            // Path 1: D clears it.
            assignPanel.selectRow (rel);
            assignPanel.dispatchAction ("defer");
            ok (assignPanel.rows.getReference (rel).state == AssignRow::State::skipDeferred
                  && assignPanel.rows.getReference (rel).conflictWith.isEmpty(),
                "D resolves the conflict card (deferred, flag cleared)");

            // Path 2: re-stage and INSIST (a plugin genuinely sharing a param).
            assignPanel.selectRow (rel);
            assignPanel.dispatchAction ("wiggle");    // re-open
            auto* i3 = host.getInstance();
            i3->getParameters()[qualified[3]]->setValueNotifyingHost (0.20f);
            juce::Timer::callAfterDelay (300, [this]
            {
                auto* i4 = host.getInstance();
                if (i4 != nullptr) i4->getParameters()[qualified[3]]->setValueNotifyingHost (0.55f);
            });
            ++stage;
            juce::Timer::callAfterDelay (1200, [this] { assignTestStep(); });
            return;
        }
        case 5:
        {
            const int rel = findRow ("release_ms");
            ok (assignPanel.rows.getReference (rel).conflictWith == "attack_ms",
                "re-capture raised the conflict card again");
            assignPanel.selectRow (rel);
            assignPanel.dispatchAction ("space");     // the insist
            ok (assignPanel.rows.getReference (rel).state == AssignRow::State::confirmed
                  && assignPanel.rows.getReference (rel).sharedInsisted,
                "SPACE insisted: shared control confirmed and recorded as a decision");
            dump ("after insisting the shared control");

            assignPanel.dispatchAction ("submit");
            ok (assignPanel.isSummaryShowing() && assignPanel.isSubmitEnabled(),
                "review allows the insisted pair (defence in depth exempts decisions)");
            dump ("review with an insisted shared control");
            ok (assignPanel.confirmSubmitFromSummary(), "submit confirmed from the review screen");
            auto f = ledger.getRoot().getChildFile ("maps").getChildFile (currentFp + ".json");
            ok (f.existsAsFile(), "map written to maps/<fp>.json");
            auto v = juce::JSON::parse (f.loadFileAsString());
            ok (v.getProperty ("schema", "").toString() == ejmap::kMapSchemaString,
                "map carries schema " + juce::String (ejmap::kMapSchemaString));
            auto pms = v.getProperty ("params", juce::var());
            auto th  = pms.getProperty ("threshold_db", juce::var());
            auto anchors = echojay::anchorsFromVar (th);
            ok (anchors.size() >= 2, "threshold_db anchors present and [value, norm] ordered");
            auto rbv = v.getProperty ("evidence", juce::var()).getProperty ("readback", juce::var());
            ok (rbv.getDynamicObject() != nullptr && rbv.getDynamicObject()->getProperties().size() > 0,
                "write-back verify recorded in evidence.readback");
            if (kneeTestIdx >= 0)
                ok (juce::JSON::toString (v.getProperty ("skips", juce::var())).contains ("mode_material"),
                    "map carries knee_db as mode_material, not as absent");
            {
                auto pms2 = v.getProperty ("params", juce::var());
                const int ia = (int) pms2.getProperty ("attack_ms", juce::var()).getProperty ("index", -1);
                const int ir = (int) pms2.getProperty ("release_ms", juce::var()).getProperty ("index", -2);
                ok (ia >= 0 && ia == ir,
                    "map carries the insisted shared control on both semantics");
            }

            std::cout << "ASSIGNTEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
            std::cout.flush();
            quitNow();
            return;
        }
        }
    }

    /** Drives the typed-anchor flow with a SIMULATED TYPIST: each prompt is
        answered with the plugin's own getCurrentValueAsText, which is what a
        faithful human transcribes when the GUI shows the same number. Stated
        on every run because it is the one simulation in the test: everything
        else -- the parking writes, the parser, the sanitizer, the record, the
        interpolation -- is the production code path.
    */
    void selfTestTyped (const juce::String& identifier, const juce::String& paramSpec)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "TYPEDTEST: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId();
        ledger.beginLoad (loadedId, desc.name, desc.manufacturerName,
                          desc.pluginFormatName, desc.version, "load", "createPluginInstance");
        auto res = host.load (desc, watchdog);
        {
            LedgerRecord rec; rec.pluginId = loadedId; rec.name = desc.name;
            rec.outcome = res.outcome; rec.detail = res.detail; rec.paramCount = res.paramCount;
            ledger.endLoad (rec);
        }
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "TYPEDTEST: load failed: " << res.detail << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        listeners.attach (*inst);
        cal = capture.calibrate (*inst, loadedId);

        auto& params = inst->getParameters();
        int idx = -1;
        if (paramSpec.containsOnly ("0123456789") && paramSpec.isNotEmpty())
            idx = paramSpec.getIntValue();
        else
            for (int i = 0; i < params.size(); ++i)
                if (params[i]->getName (64).containsIgnoreCase (paramSpec)) { idx = i; break; }
        if (! juce::isPositiveAndBelow (idx, params.size()))
        { std::cout << "TYPEDTEST: no parameter matches" << std::endl; quitNow(); return; }

        lastSweptIndex = idx;
        lastSweptName  = params[idx]->getName (48);
        std::cout << "TYPEDTEST: " << desc.name << " | param " << idx
                  << " (" << lastSweptName << ")" << std::endl;
        std::cout << "TYPEDTEST: typist SIMULATED from getCurrentValueAsText; the "
                     "production path is a human transcribing the GUI" << std::endl;

        int fails = 0;
        startTypedAnchors();
        if (! typedActive) { std::cout << "TYPEDTEST: flow did not start" << std::endl; quitNow(); return; }

        for (int step = 0; step < 5 && typedActive; ++step)
        {
            const auto shown = params[idx]->getCurrentValueAsText();
            typedEntry.setText (shown, juce::dontSendNotification);
            std::cout << "  step " << (step + 1) << "/5 n=" << kTypedSteps[step]
                      << " GUI shows \"" << shown << "\"" << std::endl;
            typedNext();
        }

        if (typedActive) { std::cout << "TYPEDTEST: FAIL - flow still active" << std::endl; quitNow(); return; }

        const auto& sw = lastSweepOutcome;
        const bool rowOk = ledger.runArtifact ("captures", "jsonl").loadFileAsString()
                             .contains ("human-typed");
        if (! (sw.ok && sw.method == "human-typed" && rowOk)) ++fails;
        std::cout << "  " << (sw.ok && rowOk ? "ok   " : "FAIL ")
                  << "typed table sanitized and recorded: " << sw.reason << std::endl;

        // The gate's last clause: the typed anchors must interpolate correctly
        // through the REAL EchoJayParamApply.h. Ask for a value BETWEEN two
        // anchors, write the interpolated norm, and read the display back.
        if (sw.anchors.size() >= 3)
        {
            const int i = sw.anchors.size() / 2;
            const float vTarget = 0.5f * (sw.anchors[i][0] + sw.anchors[i + 1][0]);
            const float n = juce::jlimit (0.0f, 1.0f,
                                echojay::interpolateAnchors (sw.anchors, vTarget));
            writeNormGesture (idx, n);
            const auto landed = params[idx]->getCurrentValueAsText();
            double vLanded = 0.0;
            const bool parsed = echojay::parseLeadingFloat (landed, vLanded);

            float lo = sw.anchors.getFirst()[0], hi = lo;
            for (const auto& a : sw.anchors) { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
            const float gap = std::abs (sw.anchors[i + 1][0] - sw.anchors[i][0]);
            const float tol = juce::jmax (0.02f * (hi - lo), 0.6f * gap);

            const bool match = parsed && std::abs ((float) vLanded - vTarget) <= tol;
            if (! match) ++fails;
            std::cout << "  " << (match ? "ok   " : "FAIL ")
                      << "interpolateAnchors round trip: asked " << vTarget
                      << ", wrote n=" << juce::String (n, 3)
                      << ", display shows \"" << landed << "\" (tol " << tol << ")" << std::endl;
        }
        else
        {
            ++fails;
            std::cout << "  FAIL too few anchors for the interpolation check" << std::endl;
        }

        std::cout << "TYPEDTEST: " << (fails == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout.flush();
        quitNow();
    }

    /** Each stage arms, then moves parameters, then asserts the classification. */
    void runCaptureStage()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) { quitNow(); return; }

        auto& params = inst->getParameters();

        // QUALIFY the pool once. A parameter that is discrete, read-only, or
        // simply ignores writes produces a delta that is not the one the test
        // asked for, and the engine then correctly reports "uncorrelated" for
        // what the test believed was a matched pair. Isolating the classifier
        // from parameter semantics is the point: this test is about
        // classification, not about how a given plugin quantises.
        if (qualified.isEmpty())
        {
            for (int i = 0; i < params.size() && qualified.size() < 12; ++i)
            {
                if (mask.indices.contains (i)) continue;
                auto* pp = params[i];
                // getNumSteps() returns a large DEFAULT for continuous params,
                // not 0, so testing it excludes everything. isDiscrete plus an
                // actual write-and-read-back is the reliable filter.
                if (pp->isDiscrete() || ! pp->isAutomatable()) continue;

                pp->setValueNotifyingHost (0.20f);
                juce::Thread::sleep (15);
                const float lo = pp->getValue();
                pp->setValueNotifyingHost (0.50f);
                juce::Thread::sleep (15);
                const float hi = pp->getValue();

                if (std::abs (lo - 0.20f) < 0.02f && std::abs (hi - 0.50f) < 0.02f)
                    qualified.add (i);
            }
            std::cout << "CAPTURETEST: qualified " << qualified.size()
                      << " continuous, write-responsive params" << std::endl;
        }

        const auto& usable = qualified;
        if (usable.size() < 10) { std::cout << "CAPTURETEST: too few qualified params" << std::endl; quitNow(); return; }

        if (stage >= 8)
        {
            // Stages 4-6 moved usable[9] three times with no mouse anywhere
            // near the editor. Promotion must have fired on the third, through
            // the new message-thread path, and left a mask_promoted row.
            const bool promoted = mask.indices.contains (usable[9])
                                    && ! mask.promotions.isEmpty();
            const bool rowExists = ledger.runArtifact ("captures", "jsonl").loadFileAsString()
                                     .contains ("mask_promoted");
            if (! promoted || ! rowExists) ++failures;
            std::cout << "  " << (promoted && rowExists ? "ok   " : "FAIL ")
                      << "same index, 3 no-mouse cycles -> promoted to mask and recorded"
                      << "  (masked=" << (promoted ? "yes" : "no")
                      << " row=" << (rowExists ? "yes" : "no") << ")" << std::endl;

            // Prove the refusal, rather than trusting that it would fire. A guard
            // never observed refusing is not a guard.
            auto capturesFile = ledger.runArtifact ("captures", "jsonl");
            const auto before = capturesFile.existsAsFile()
                                  ? juce::StringArray::fromLines (capturesFile.loadFileAsString()).size() : 0;
            const auto keep = loadedId;
            loadedId.clear();
            recordCapture ("captured", 1, "Deliberately Unattributed", {}, {}, "refusal probe");
            loadedId = keep;
            const auto after = capturesFile.existsAsFile()
                                 ? juce::StringArray::fromLines (capturesFile.loadFileAsString()).size() : 0;
            const bool refused = (after == before)
                                   && ledger.runArtifact ("captures-rejected", "log").existsAsFile();
            if (! refused) ++failures;
            std::cout << "  " << (refused ? "ok   " : "FAIL ")
                      << "row with empty plugin_id REFUSED and logged" << std::endl;

            std::cout << "CAPTURETEST: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
            std::cout.flush(); quitNow(); return;
        }

        const int st = stage;

        // Pre-set to a known base BEFORE arming, so the snapshot is taken at a
        // value we can move away from. Adding a delta to whatever the parameter
        // happened to be clamps to a no-op when it is already near a limit,
        // which is what made the first version of this test report failures
        // that were the test's, not the engine's.
        juce::Array<int> targets;
        if      (st == 0) targets = { usable[0] };
        else if (st == 1) targets = { usable[1], usable[2] };
        else if (st == 2) targets = { usable[3], usable[4] };
        else if (st == 3) for (int k = 0; k < 9; ++k) targets.add (usable[k]);
        else if (st <= 6) targets = { usable[9] };            // stages 4-6: the promotion probe
        else              targets = { usable[7], usable[8] }; // stage 7: gesture resolution

        for (int i = 0; i < targets.size(); ++i)
        {
            // stage 2 wants opposed directions, so one starts high.
            const float base = (st == 2 && i == 1) ? 0.80f : 0.20f;
            params[targets[i]]->setValueNotifyingHost (base);
        }
        juce::Thread::sleep (120);      // let the base settle before the snapshot

        capture.arm (*inst, cal, mask, [this, st] (const CaptureEngine::Result& r)
        {
            // Stages 1 and 2 both land on "gesture" now that Kind::twins is
            // retired, so the kind alone no longer separates them. The test
            // asserts the SHAPE as well, which is the thing that actually still
            // differs: same-direction for the correlated pair, mixed for the
            // opposed one. Without this the two stages would be one assertion
            // run twice and the shape measurement would be uncovered.
            static const char* names[] = { "one moved -> captured",
                                           "two correlated -> gesture, same direction",
                                           "two opposed -> gesture, mixed directions",
                                           "nine moved -> too_many",
                                           "promotion probe cycle 1 -> captured, not yet masked",
                                           "promotion probe cycle 2 -> captured, not yet masked",
                                           "promotion probe cycle 3 -> captured, then promoted",
                                           "two moved, plugin gestured one -> captured via listener" };
            static const char* want[]  = { "captured", "gesture", "gesture", "too_many",
                                           "captured", "captured", "captured", "captured" };

            bool ok = (r.kindString() == juce::String (want[st]));
            if (st == 1) ok = ok && r.sameDirection;
            if (st == 2) ok = ok && ! r.sameDirection;

            // Stage 7 is the listener layer's claim: two parameters moved in
            // one window, the plugin reported a gesture on exactly one, and
            // that one is the capture -- no human pick, follower kept as
            // co-moved. Assert the attribution, not just the kind, or the
            // stage would pass whenever anything got captured.
            if (st == 7) ok = ok && r.capturedBy == "gesture"
                                 && r.primaryIndex == qualified[7]
                                 && r.indices.size() == 2;

            // The probe index must still be capturable on cycles 1 and 2: a
            // promotion that fires early is as wrong as one that never fires.
            if (st == 4 || st == 5) ok = ok && mask.promotions.isEmpty();

            if (! ok) ++failures;
            std::cout << "  " << (ok ? "ok   " : "FAIL ") << names[st]
                      << "  -> got " << r.kindString()
                      << " indices=" << r.indices.size()
                      << " sameDirection=" << (r.sameDirection ? "yes" : "no")
                      << " ratio=" << juce::String (r.magnitudeRatio, 2) << std::endl;

            // Exercise the RECORD WRITER, not just the classifier. Without this
            // the test reported PASS while writing nothing, so persistence was
            // never covered by the thing that claimed to cover it. A gesture is
            // recorded here as the raw gesture row; in the app it is recorded
            // again once the human picks, which is the same writer either way.
            // Same primary/co split as the app callback: a gesture-resolved
            // capture's row must carry the plugin-named index, not -1.
            const int intended = r.primaryIndex >= 0
                                   ? r.primaryIndex
                                   : (r.indices.size() == 1 ? r.indices[0] : -1);
            juce::Array<int> co;
            juce::StringArray coN;
            for (int i = 0; i < r.indices.size(); ++i)
                if (r.indices[i] != intended)
                { co.add (r.indices[i]); coN.add (i < r.names.size() ? r.names[i] : juce::String()); }

            const int pos = r.indices.indexOf (intended);
            recordCapture (r.kindString(), intended,
                           pos >= 0 && pos < r.names.size() ? r.names[pos] : juce::String(),
                           co, coN,
                           r.reason, r.sameDirection, r.magnitudeRatio, r.capturedBy);
            flushPromotionRows();      // a promotion this result caused becomes a row here
            ++stage;
            juce::Timer::callAfterDelay (400, [this] { runCaptureStage(); });
        });

        juce::Timer::callAfterDelay (300, [this, targets, st]
        {
            auto* in = host.getInstance();
            if (in == nullptr) return;
            auto& ps = in->getParameters();

            // Stage 7 moves two but gestures ONE, which is exactly what a GUI
            // does when a touched control drags a follower along: the host
            // sees beginEdit/BeginGesture only for the hand's parameter.
            if (st == 7)
                ps[targets[0]]->beginChangeGesture();

            for (int i = 0; i < targets.size(); ++i)
            {
                const float to = (st == 2 && i == 1) ? 0.50f : 0.50f;   // +0.30 / -0.30
                ps[targets[i]]->setValueNotifyingHost (to);
            }

            if (st == 7)
                ps[targets[0]]->endChangeGesture();
        });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        auto top = r.removeFromTop (28);
        scanButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (6);
        loadButton.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (6);
        signalToggle.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (6);
        armButton.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (6);
        sweepButton.setBounds (top.removeFromLeft (76));
        top.removeFromLeft (6);
        assignButton.setBounds (top.removeFromLeft (76));
        top.removeFromLeft (6);
        uploadButton.setBounds (top.removeFromLeft (80));
        top.removeFromLeft (6);
        deepToggle.setBounds (top.removeFromLeft (64));
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

        // Capture readout sits under the top row, full width.
        r.removeFromTop (4);
        {
            auto row = r.removeFromTop (20);
            if (candidatePicker.isVisible())
            {
                candidatePicker.setBounds (row.removeFromLeft (300));
                row.removeFromLeft (8);
            }
            captureReadout.setBounds (row);
        }

        // Mask strip: only takes space while something is masked.
        if (maskPicker.isVisible())
        {
            r.removeFromTop (4);
            auto row = r.removeFromTop (20);
            maskPicker.setBounds (row.removeFromLeft (420));
            row.removeFromLeft (6);
            unmaskButton.setBounds (row.removeFromLeft (80));
        }

        // Typed-anchor row, while the flow is active.
        if (typedPrompt.isVisible())
        {
            r.removeFromTop (4);
            auto row = r.removeFromTop (22);
            typedPrompt.setBounds (row.removeFromLeft (330));
            row.removeFromLeft (6);
            typedEntry.setBounds (row.removeFromLeft (150));
            row.removeFromLeft (6);
            typedNextButton.setBounds (row.removeFromLeft (60));
            row.removeFromLeft (6);
            typedCancelButton.setBounds (row.removeFromLeft (70));
        }

        // Curve view: costs height only when a curve exists to look at.
        if (curveView.isVisible())
        {
            r.removeFromTop (4);
            curveView.setBounds (r.removeFromTop (150));
        }

        r.removeFromTop (8);
        auto left = r.removeFromLeft (assigning ? 500 : 420);
        if (assigning)
        {
            assignPanel.setBounds (left);
        }
        else
        {
            filterBox.setBounds (left.removeFromTop (24));
            left.removeFromTop (4);
            list.setBounds (left);
        }
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
            owner.armButton.setEnabled (false);
            owner.signalToggle.setEnabled (false);
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

    /** AUTO-RELEASE, SCOPED.

        SCAN quarantines release themselves when the plugin changes on disk. A
        plugin update is the usual reason a hang stops happening, and a scan
        quarantine costs everyone: the bundle is never probed again, so its
        plugins stay missing from every future list.

        LOAD quarantines stay manual, deliberately. A crash on load is about the
        plugin's interaction with THIS host, and a version bump is not evidence
        that it is fixed. The human decides, with the Release button.

        VST3 uses the executable's stamp, which is exact. AU uses the registry
        version string, which is weaker and is flagged as such in the entry: a
        vendor can ship a fix without bumping it. It is the only change signal
        the registry offers short of mapping a component back to its bundle, and
        that mapping already failed for 14 components here.
    */
    int releaseStaleScanQuarantines()
    {
        int released = 0;

        for (const auto& e : ledger.getQuarantineEntries())
        {
            if (e.stage != "scan")
                continue;               // load quarantines are the human's call

            bool changed = false;

            if (e.isAudioUnit())
            {
                const auto now = echojay::auregistry::describeFromRegistry (e.pluginId).version;
                changed = e.auVersion.isNotEmpty() && now.isNotEmpty() && now != e.auVersion;
            }
            else
            {
                const auto now = diskStampFor (juce::File (e.pluginId));
                // Only a stamp we can compare counts. "Cannot tell" holds the
                // quarantine rather than releasing on a missing reading.
                changed = e.stamp.valid && now.valid && now != e.stamp;
            }

            if (changed)
            {
                ledger.releaseFromQuarantine (e.pluginId);
                ++released;
                std::cout << "auto-released scan quarantine: " << e.pluginId
                          << " (changed on disk since " << e.at << ")" << std::endl;
            }
        }

        return released;
    }

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

        // RESUME. A previous attempt at this scan may have died inside a bundle.
        // Anything it already probed, whose bundle has not changed since, is
        // restored rather than re-probed.
        ScanProgress progress (ledger.getRoot().getChildFile ("scan-progress.jsonl"));
        const bool resuming = progress.load();
        if (! resuming)
            progress.begin (ledger.currentRunId());

        scanProgress = 0.0;
        progressLabel.setText (resuming ? "resuming from " + juce::String (progress.size())
                                            + " already-probed bundles..."
                                        : "starting...",
                               juce::dontSendNotification);
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
        lastScan = scanner.scan (ledger, watchdog, progress,
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

        // Completed, so the progress file has done its job.
        progress.finish();

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
            << (lastScan.vst3Resumed > 0
                  ? ", " + juce::String (lastScan.vst3Resumed) + " resumed"
                  : juce::String())
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
        report.add ("vst3_resumed=" + juce::String (lastScan.vst3Resumed));
        report.add ("vst3_restamped_reprobed=" + juce::String (lastScan.vst3Restamped));
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

    /** A session that restarted eleven times is a fact about this machine's
        plugin set, and the status line clears. The ledger does not.
    */
    void recordRestartRow()
    {
        LedgerRecord r;
        r.pluginId = crashedId;
        r.name     = crashedId.isEmpty() ? "(unknown)" : crashedId;
        r.stage    = "session";
        r.outcome  = LoadOutcome::restarted;
        r.detail   = "auto-relaunched by supervisor after " + lastExitCause
                       + "; restart " + juce::String (restartsThisSession)
                       + " of " + juce::String (SupervisorLimits::kMaxConsecutive)
                       + (crashedId.isEmpty() ? juce::String()
                                              : "; previous session died in " + crashedId);
        ledger.endLoad (r);
    }

    /** Written the first time a load succeeds, so the supervisor can tell a
        session that was usable from one that died before it could be used.
    */
    void markLoadSucceeded()
    {
        if (markedLoadOk)
            return;
        markedLoadOk = true;
        loadOkMarker (ledger.getRoot()).replaceWithText ("ok");
    }

    /** Runs ONLY after load() has returned, which means the editor-ready wait
        has already settled. A bridged editor reaches real size about 2.5 s in,
        and calibrating against a half-constructed plugin would measure the
        wrong thing.
    */
    void prepareCapture (const juce::String& name, const juce::String& pluginId)
    {
        capture.stop();
        cal = CaptureEngine::Calibration();
        mask = CaptureEngine::NoiseMask();
        armButton.setEnabled (false);

        // A new load is a new promotion context. Without this, counts keyed on
        // bare indices survive into the next plugin: Pro-Q 3 run 114642 shows
        // idx 2 re-promoted at 11:56:44, on its FIRST appearance after a
        // reload whose fresh baseline was clean.
        capture.resetCycleCounts();
        promotionsFlushed = 0;
        lastCapturedIndex = -1;
        sweepButton.setEnabled (false);

        auto* inst = host.getInstance();
        if (inst == nullptr)
            return;

        // Identity is already set by the load path. Deliberately NOT set here:
        // it must exist before anything can be captured, not as a side effect of
        // preparing to capture.
        candidatePicker.setVisible (false);
        cal = capture.calibrate (*inst, pluginId);
        if (! cal.valid)
        {
            captureReadout.setText (name + ": no automatable parameters, nothing to capture",
                                    juce::dontSendNotification);
            return;
        }

        captureReadout.setText ("Calibrating " + name + ": " + cal.describe()
                                  + "  -  baseline...", juce::dontSendNotification);

        mask = capture.buildNoiseMask (*inst, cal, pluginId);

        juce::String t;
        t << name << "  -  " << cal.describe()
          << "  -  noise mask " << mask.indices.size() << " of " << cal.paramCount
          << " (" << mask.method << ", " << mask.samples << " samples, "
          << juce::String (mask.seconds, 2) << "s)";
        captureReadout.setText (t, juce::dontSendNotification);

        refreshMaskUi();
        armButton.setEnabled (true);
    }

    //==========================================================================
    /** Provenance for one masked index: the promotion that put it there, or
        the baseline that did. The human deciding whether to unmask is
        overriding this exact sentence, so it is shown rather than implied.
    */
    juce::String maskProvenance (int index) const
    {
        for (const auto& p : mask.promotions)
            if (p.index == index)
                return p.reason;

        return "baseline (" + mask.method + ", " + juce::String (mask.samples) + " samples)";
    }

    /** Every promotion becomes its own row, once. A promotion is the record
        claiming a parameter is self-changing; a claim that strong does not get
        to live only in a readout that clears.
    */
    void flushPromotionRows()
    {
        auto* inst = host.getInstance();

        for (; promotionsFlushed < mask.promotions.size(); ++promotionsFlushed)
        {
            const auto& p = mask.promotions.getReference (promotionsFlushed);
            const auto name = (inst != nullptr
                                 && juce::isPositiveAndBelow (p.index, inst->getParameters().size()))
                                ? inst->getParameters()[p.index]->getName (48)
                                : juce::String();
            recordCapture ("mask_promoted", p.index, name, {}, {}, p.reason);
        }
    }

    void refreshMaskUi()
    {
        maskPicker.clear (juce::dontSendNotification);

        auto* inst = host.getInstance();
        for (int i = 0; i < mask.indices.size(); ++i)
        {
            const int idx = mask.indices[i];
            const auto name = (inst != nullptr
                                 && juce::isPositiveAndBelow (idx, inst->getParameters().size()))
                                ? inst->getParameters()[idx]->getName (32)
                                : juce::String ("?");
            maskPicker.addItem (juce::String (idx) + ":  " + name + "  -  " + maskProvenance (idx),
                                i + 1);
        }

        const bool show = inst != nullptr && ! mask.indices.isEmpty();
        maskPicker.setVisible (show);
        unmaskButton.setVisible (show);
        unmaskButton.setEnabled (show && ! capture.isArmed());
        resized();
    }

    /** Releases the selected index from the noise mask. Deliberate and
        recorded, exactly like quarantine release: it acts on one explicitly
        selected row, says what it is overriding, and writes a row so the
        release is evidence rather than an edit.
    */
    void unmaskSelected()
    {
        const int sel = maskPicker.getSelectedId() - 1;      // ids are 1-based
        if (! juce::isPositiveAndBelow (sel, mask.indices.size()) || capture.isArmed())
            return;

        const int idx = mask.indices[sel];
        const auto provenance = maskProvenance (idx);

        auto* inst = host.getInstance();
        const auto name = (inst != nullptr
                             && juce::isPositiveAndBelow (idx, inst->getParameters().size()))
                            ? inst->getParameters()[idx]->getName (48)
                            : juce::String();

        mask.indices.removeValue (idx);
        for (int i = mask.promotions.size(); --i >= 0;)
            if (mask.promotions.getReference (i).index == idx)
                mask.promotions.remove (i);

        // The count that promoted it is wrong by the human's ruling. Clear it,
        // or the next appearance re-promotes instantly and the release was a
        // no-op with extra steps.
        capture.clearCycleCount (idx);

        recordCapture ("mask_released", idx, name, {}, {},
                       "human release; was: " + provenance);

        captureReadout.setText ("Unmasked " + juce::String (idx) + ": " + name
                                  + ". It is watchable again; if it moves in "
                                  + juce::String (CaptureEngine::kPromoteAfterCycles)
                                  + " arm cycles with no hand on the editor it will be re-promoted.",
                                juce::dontSendNotification);
        refreshMaskUi();
    }

    /** Builds the hint from the ring, and records the denominator with it.

        Normalised against the editor's bounds AS MEASURED NOW, and those bounds
        travel in the hint (schema 2.2). A fraction without its denominator
        cannot be checked later, and these editors do change size: bridged ones
        reach real size ~2.5 s after creation, resizable ones move mid-session.
    */
    UiHint hintFor (juce::uint32 detectedAtMs)
    {
        UiHint h;

        if (hostedEditor == nullptr)
            return h;

        MouseRing::Sample s;
        if (! ring.bestFor (detectedAtMs, s))
            return h;                    // ring had nothing that old: null, not a guess

        const int ew = hostedEditor->getWidth();
        const int eh = hostedEditor->getHeight();
        if (ew <= 0 || eh <= 0)
            return h;

        const auto local = hostedEditor->getLocalPoint (nullptr, s.screenPos);
        if (! hostedEditor->getLocalBounds().contains (local))
            return h;                    // outside the editor: null rather than a lie

        h.valid        = true;
        h.x            = (float) local.x / (float) ew;
        h.y            = (float) local.y / (float) eh;
        h.w            = 48.0f / (float) ew;      // default hint box, tunable
        h.h            = 48.0f / (float) eh;
        h.editorWidth  = ew;
        h.editorHeight = eh;
        h.screen       = s.screen;
        return h;
    }

    //==========================================================================
    /** Sweeps the last captured index through the shared sweep code and
        records the outcome, success or refusal alike.
    */
    void sweepCaptured()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr || lastCapturedIndex < 0)
            return;

        sweepButton.setEnabled (false);
        armButton.setEnabled (false);

        const int idx = lastCapturedIndex;
        const auto name = juce::isPositiveAndBelow (idx, inst->getParameters().size())
                            ? inst->getParameters()[idx]->getName (48) : juce::String();

        captureReadout.setText ("Sweeping " + juce::String (idx) + ": " + name + "...",
                                juce::dontSendNotification);

        auto outcome = runSweep (idx);

        lastSweptIndex   = idx;
        lastSweptName    = name;
        lastSweepOutcome = outcome;
        curveView.show (outcome, juce::String (idx) + ": " + name);
        typeButton.setEnabled (true);
        resized();

        juce::String t;
        t << "SWEEP " << idx << ":" << name << "  -  "
          << (outcome.ok ? "ok" : (outcome.flat ? "TEXT LIAR" : "refused"))
          << "  -  " << outcome.reason
          << "  (" << outcome.durationMs << " ms)";
        captureReadout.setText (t, juce::dontSendNotification);
        std::cout << "SWEEP: " << t << std::endl;

        sweepButton.setEnabled (true);
        armButton.setEnabled (true);
    }

    //==========================================================================
    /** The typed-anchor path: the human transcribes the GUI reading at five
        parked positions. The fallback for both liar classes -- flat (the
        display carries nothing) and identity (the display is fabricated and
        plausible) -- and for any curve the human rejects. Corpus-measured
        scope: whole-plugin fabrication is 9 products, so this is a fallback,
        not the main path.
    */
    static constexpr float kTypedSteps[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

    void writeNormGesture (int idx, float n)
    {
        auto* inst = host.getInstance();
        if (inst == nullptr || ! juce::isPositiveAndBelow (idx, inst->getParameters().size()))
            return;
        auto* p = inst->getParameters()[idx];
        // The same begin/change/end shape applyOne uses: a bare value poke is
        // not what hosts do, and some plugins only repaint under a gesture.
        p->beginChangeGesture();
        p->setValueNotifyingHost (n);
        p->endChangeGesture();
    }

    void startTypedAnchors()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr || lastSweptIndex < 0 || typedActive || capture.isArmed())
            return;

        // The flow mutates the plugin and ends in a full state restore, so it
        // runs under the same inflight record as a sweep.
        beginSweepInflight (lastSweptIndex, lastSweptName + " (typed)");

        host.pausePumpForMutation();
        inst->getStateInformation (typedStateBefore);
        host.resumePumpAfterMutation();

        typedActive = true;
        typedStep = 0;
        typedPairs.clear();
        typedTexts.clear();

        typedPrompt.setVisible (true);
        typedEntry.setVisible (true);
        typedNextButton.setVisible (true);
        typedCancelButton.setVisible (true);
        typeButton.setEnabled (false);
        armButton.setEnabled (false);
        sweepButton.setEnabled (false);

        typedParkAndPrompt();
        resized();
    }

    void typedParkAndPrompt()
    {
        writeNormGesture (lastSweptIndex, kTypedSteps[typedStep]);
        typedPrompt.setText ("Typed anchors " + juce::String (typedStep + 1) + "/5 (n="
                               + juce::String (kTypedSteps[typedStep], 2) + "): what does the GUI show for "
                               + lastSweptName + "?",
                             juce::dontSendNotification);
        typedEntry.setText ({}, juce::dontSendNotification);
        typedEntry.grabKeyboardFocus();
    }

    void typedNext()
    {
        if (! typedActive)
            return;

        const auto text = typedEntry.getText().trim();
        double v = 0.0;
        if (! echojay::parseLeadingFloat (text, v))
        {
            typedPrompt.setText ("Could not parse \"" + text + "\" - type the number the GUI shows",
                                 juce::dontSendNotification);
            return;
        }

        juce::Array<float> pair;
        pair.add ((float) v);
        pair.add (kTypedSteps[typedStep]);
        typedPairs.add (pair);
        typedTexts.add (text);

        if (++typedStep < 5)
        {
            typedParkAndPrompt();
            return;
        }
        typedFinish();
    }

    void typedFinish()
    {
        auto* inst = host.getInstance();

        // Restore the instance exactly as it arrived, same bracket as setread.
        if (inst != nullptr && typedStateBefore.getSize() > 0)
        {
            host.pausePumpForMutation();
            inst->setStateInformation (typedStateBefore.getData(), (int) typedStateBefore.getSize());
            host.resumePumpAfterMutation();
        }

        // Same pipeline as a machine sweep: raw pairs, shared sanitizer,
        // recorded whatever the verdict. Only the method differs.
        SweepOutcome sw;
        sw.method = "human-typed";
        sw.rawAnchors = typedPairs;
        for (int i = 0; i < typedTexts.size(); ++i)
            sw.points.add ({ kTypedSteps[i], typedTexts[i] });

        auto eff = echojay::dominantMonotonicTable (typedPairs);
        if (eff.ok)
        {
            sw.anchors = eff.table;
            sw.rejectedPoints = typedPairs.size() - eff.table.size();
            sw.anchorsReversed = eff.table.getFirst()[0] > eff.table.getLast()[0];
            float lo = eff.table.getFirst()[0], hi = lo;
            for (const auto& a : eff.table) { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
            sw.ok = (hi - lo) >= 1.0e-6f;
            sw.reason = juce::String (sw.anchors.size()) + " anchors (human-typed"
                      + (sw.anchorsReversed ? ", descending" : ", ascending")
                      + (sw.rejectedPoints > 0
                           ? ", " + juce::String (sw.rejectedPoints) + " rejected by the sanitizer"
                           : juce::String())
                      + (sw.ok ? juce::String() : juce::String(", DEGENERATE SPAN, refused")) + ")";
        }
        else
        {
            sw.rejectedPoints = typedPairs.size();
            sw.reason = "sanitizer refused the typed table: no strictly-monotonic run covers "
                        "60% of 5 typed points. Typos happen; run the typed path again.";
        }

        endSweepInflight (lastSweptIndex, sw);
        recordSweep (lastSweptIndex, lastSweptName, sw);

        lastSweepOutcome = sw;
        curveView.show (sw, juce::String (lastSweptIndex) + ": " + lastSweptName + " (typed)");
        if (assigning)
            assignPanel.typedCompleted (sw);

        captureReadout.setText ("TYPED " + juce::String (lastSweptIndex) + ":" + lastSweptName
                                  + "  -  " + sw.reason, juce::dontSendNotification);
        std::cout << "TYPED: " << lastSweptIndex << ":" << lastSweptName
                  << "  -  " << sw.reason << std::endl;

        typedTeardownUi();
    }

    void typedCancel()
    {
        if (! typedActive)
            return;

        auto* inst = host.getInstance();
        if (inst != nullptr && typedStateBefore.getSize() > 0)
        {
            host.pausePumpForMutation();
            inst->setStateInformation (typedStateBefore.getData(), (int) typedStateBefore.getSize());
            host.resumePumpAfterMutation();
        }

        SweepOutcome sw;
        sw.method = "human-typed";
        sw.reason = "typed path cancelled by the human at step "
                  + juce::String (typedStep + 1) + " of 5; state restored, nothing written";
        endSweepInflight (lastSweptIndex, sw);
        // Deliberately NO captures row: a cancel is not evidence about the
        // parameter. The ledger row above records that the flow ran and ended.

        captureReadout.setText ("Typed path cancelled; state restored.", juce::dontSendNotification);
        typedTeardownUi();
    }

    void typedTeardownUi()
    {
        typedActive = false;
        typedPrompt.setVisible (false);
        typedEntry.setVisible (false);
        typedNextButton.setVisible (false);
        typedCancelButton.setVisible (false);
        typeButton.setEnabled (lastSweptIndex >= 0);
        armButton.setEnabled (true);
        sweepButton.setEnabled (lastCapturedIndex >= 0);
        resized();
    }

    //==========================================================================
    //==========================================================================
    // M4 assignment wiring.

    void wireAssignHooks()
    {
        assignPanel.hooks.paramName = [this] (int idx) -> juce::String
        {
            auto* inst = host.getInstance();
            return inst != nullptr && juce::isPositiveAndBelow (idx, inst->getParameters().size())
                     ? inst->getParameters()[idx]->getName (48) : juce::String();
        };
        assignPanel.hooks.paramCount = [this]
        {
            auto* inst = host.getInstance();
            return inst != nullptr ? inst->getParameters().size() : 0;
        };
        assignPanel.hooks.maskCount = [this] { return mask.indices.size(); };
        assignPanel.hooks.maskIndices = [this]
        {
            juce::Array<int> out;
            for (int i = 0; i < mask.indices.size(); ++i) out.add (mask.indices[i]);
            return out;
        };
        assignPanel.hooks.loadBreadcrumbs = [this]() -> juce::var
        {
            // tier2-candidates-*.jsonl, this plugin only, deduped by index
            // with the LATEST row winning (the doubled Knee crumb class).
            std::map<int, juce::var> byIndex;
            for (const auto& entry : juce::RangedDirectoryIterator (ledger.getRoot(), false,
                                                                    "tier2-candidates-*.jsonl"))
            {
                juce::StringArray lines;
                lines.addLines (entry.getFile().loadFileAsString());
                for (const auto& line : lines)
                {
                    if (line.trim().isEmpty()) continue;
                    auto v = juce::JSON::parse (line);
                    if (v.getProperty ("plugin_id", "").toString() != loadedId) continue;
                    byIndex[(int) v.getProperty ("index", -1)] = v;
                }
            }
            juce::Array<juce::var> out;
            for (auto& kv : byIndex) out.add (kv.second);
            return juce::var (out);
        };
        assignPanel.hooks.status = [this] (const juce::String& line)
        {
            captureReadout.setText (line, juce::dontSendNotification);
            std::cout << "ASSIGN: " << line << std::endl;
        };
        assignPanel.hooks.sweepIndex = [this] (int idx) { return runSweep (idx); };
        assignPanel.hooks.sweepIndexSetread = [this] (int idx)
        {
            auto* inst = host.getInstance();
            if (inst == nullptr) return SweepOutcome();
            const auto name = assignPanel.hooks.paramName (idx);
            beginSweepInflight (idx, name + " (setread verify)");
            host.pausePumpForMutation();
            auto outcome = sweepOneIndex (*inst, idx, watchdog, loadedId, true);
            host.resumePumpAfterMutation();
            endSweepInflight (idx, outcome);
            recordSweep (idx, name, outcome);
            return outcome;
        };
        assignPanel.hooks.spotCheck = [this] (int idx, double norm) -> juce::String
        {
            auto* inst = host.getInstance();
            if (inst == nullptr || ! juce::isPositiveAndBelow (idx, inst->getParameters().size()))
                return {};
            auto* p = inst->getParameters()[idx];
            host.pausePumpForMutation();
            const float before = p->getValue();
            p->setValue ((float) norm);
            // READ UNTIL STABLE, not for a fixed wait: the bridge is a lag
            // pipeline and a fixed pump interval sometimes serves the
            // PREVIOUS update (measured: two runs disagreed about which end
            // of a 3-position switch was Hard). Two consecutive identical
            // reads, bounded at ~300 ms, is the truth condition.
            juce::String t, prev;
            for (int tries = 0; tries < 20; ++tries)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil (15);
                t = p->getCurrentValueAsText();
                if (t == prev && tries > 0) break;
                prev = t;
            }
            p->setValue (before);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (15);
            host.resumePumpAfterMutation();
            return t;
        };
        assignPanel.hooks.startTyped = [this] (int idx)
        {
            lastSweptIndex = idx;
            lastSweptName  = assignPanel.hooks.paramName (idx);
            startTypedAnchors();
        };
        assignPanel.hooks.cancelArm = [this] { capture.stop(); };
        assignPanel.hooks.armForRow = [this]
        {
            auto* inst = host.getInstance();
            if (inst == nullptr || ! cal.valid) return;
            capture.arm (*inst, cal, mask, [this] (const CaptureEngine::Result& r)
            {
                // Same evidence as any capture: the row is written before the
                // panel acts on it, so an assignment capture and a manual one
                // are indistinguishable on disk.
                lastHint = hintFor (r.detectedAtMs);
                const int intended = r.primaryIndex >= 0 ? r.primaryIndex
                                       : (r.indices.size() == 1 ? r.indices[0] : -1);
                juce::Array<int> co; juce::StringArray coN;
                for (int i = 0; i < r.indices.size(); ++i)
                    if (r.indices[i] != intended)
                    { co.add (r.indices[i]); coN.add (i < r.names.size() ? r.names[i] : juce::String()); }
                const int pos = r.indices.indexOf (intended);
                recordCapture (r.kindString(), intended,
                               pos >= 0 && pos < r.names.size() ? r.names[pos] : juce::String(),
                               co, coN, r.reason, r.sameDirection, r.magnitudeRatio, r.capturedBy);
                flushPromotionRows();
                refreshMaskUi();
                assignPanel.captureArrived (r);
            });
        };
        assignPanel.hooks.writeRow = [this] (const juce::var& v)
        {
            if (loadedId.isEmpty()) return;
            auto f = ledger.runArtifact ("captures", "jsonl");
            juce::FileOutputStream out (f);
            if (out.openedOk())
            {
                out.setPosition (f.getSize());
                out.writeText (juce::JSON::toString (v, true) + "\n", false, false, nullptr);
                out.flush();
            }
        };
        assignPanel.hooks.writeTier2Crumb = [this] (const juce::var& v)
        {
            auto f = ledger.runArtifact ("tier2-candidates", "jsonl");
            juce::FileOutputStream out (f);
            if (out.openedOk())
            {
                out.setPosition (f.getSize());
                out.writeText (juce::JSON::toString (v, true) + "\n", false, false, nullptr);
                out.flush();
            }
        };
        assignPanel.hooks.writeMisclassified = [this] (const juce::var& v)
        {
            auto f = ledger.runArtifact ("misclassified", "jsonl");
            juce::FileOutputStream out (f);
            if (out.openedOk())
            {
                out.setPosition (f.getSize());
                out.writeText (juce::JSON::toString (v, true) + "\n", false, false, nullptr);
                out.flush();
            }
        };
        assignPanel.hooks.submit = [this] (juce::Array<AssignRow>& rws,
                                           const juce::String& cat, const juce::String& lane)
        { submitMap (rws, cat, lane); };
        assignPanel.hooks.exitPanel = [this] { endAssignment(); };
    }

    void startAssignment()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr || ! cal.valid || assigning)
            return;

        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);
        auto proposals = ProposalSet::load (ledger.getRoot(), currentFp);
        auto ev        = EvidenceIndex::build (ledger.getRoot(), loadedId);

        assigning = true;
        assignPanel.setVisible (true);
        list.setVisible (false);
        filterBox.setVisible (false);
        assignPanel.begin (ledger.getRoot(), currentFp, loadedId, proposals, ev);
        resized();
        assignPanel.grabKeyboardFocus();
    }

    void endAssignment()
    {
        assigning = false;
        assignPanel.setVisible (false);
        list.setVisible (true);
        filterBox.setVisible (true);
        resized();
    }

    /** Submit: rows become a MapPayload, every confirmed row gets a write-back
        verify (write the interpolated norm for a mid-table value through the
        REAL interpolateAnchors, read the display, record the ReadbackResult),
        and the payload lands in <root>/maps/<fp>.json where the drift gate's
        corpus round-trip will check it on every future commit.
    */
    void submitMap (juce::Array<AssignRow>& rws, const juce::String& cat, const juce::String& lane)
    {
        auto* inst = host.getInstance();
        if (inst == nullptr || loadedId.isEmpty() || currentFp.isEmpty())
            return;

        {
            std::map<int, juce::StringArray> byIndex;
            for (auto& r : rws)
                if (r.state == AssignRow::State::confirmed && ! r.sharedInsisted)
                    byIndex[r.resolvedIndex].add (r.semantic);
            for (auto& kv : byIndex)
                if (kv.second.size() > 1)
                {
                    captureReadout.setText ("SUBMIT REFUSED: index " + juce::String (kv.first)
                        + " claimed by " + kv.second.joinIntoString (" AND ")
                        + ". One of them is wrong.", juce::dontSendNotification);
                    std::cout << "ASSIGN: SUBMIT REFUSED: duplicate index " << kv.first
                              << " (" << kv.second.joinIntoString (" AND ") << ")" << std::endl;
                    return;
                }
        }

        MapPayload p;
        p.fp = currentFp;
        p.category = cat;
        p.mode = lane == "deep" ? Mode::deep : Mode::fast;
        p.identity.format     = loadedDesc.pluginFormatName;
        p.identity.uid        = juce::String::toHexString (loadedDesc.uniqueId);
        p.identity.name       = loadedDesc.name;
        p.identity.vendor     = loadedDesc.manufacturerName;
        p.identity.version    = loadedDesc.version;
        p.identity.paramCount = cal.paramCount;
        p.provenance.ejmapVersion = juce::String (EJMAP_VERSION) + " (" + EJMAP_GIT_HASH + ")";
        p.provenance.applyHeaderSha = EJMAP_APPLY_HEADER_SHA;   // hashed AS COMPILED, at build time
        p.provenance.machineId = machineIdString();
        p.provenance.testerId  = testerName();
        p.provenance.pluginVersion = loadedDesc.version;
        p.provenance.hostOs = juce::SystemStats::getOperatingSystemName();
        p.provenance.at = juce::Time::getCurrentTime().toISO8601 (true);
        p.evidence.durationSeconds = assignPanel.sessionSeconds();

        // Per-index capture evidence from the rows on disk: the latest
        // captured row's ui_hint and mechanism travel with the param entry.
        std::map<int, juce::var> hintByIdx;
        std::map<int, juce::String> byByIdx;
        for (const auto& entry : juce::RangedDirectoryIterator (ledger.getRoot(), false,
                                                                "captures-*.jsonl"))
        {
            juce::StringArray lines; lines.addLines (entry.getFile().loadFileAsString());
            for (const auto& line : lines)
            {
                if (line.trim().isEmpty()) continue;
                auto v = juce::JSON::parse (line);
                if (v.getProperty ("plugin_id", "").toString() != loadedId) continue;
                const auto kind = v.getProperty ("kind", "").toString();
                if (kind != "captured" && kind != "captured_from_gesture") continue;
                const int idx = (int) v.getProperty ("index", -1);
                if (idx < 0) continue;
                if (! v.getProperty ("ui_hint", juce::var()).isVoid())
                    hintByIdx[idx] = v.getProperty ("ui_hint", juce::var());
                byByIdx[idx] = v.getProperty ("captured_by", "poll").toString();
            }
        }
        for (int i = 0; i < mask.indices.size(); ++i)
            p.evidence.noiseMask.add (mask.indices[i]);

        // Write-back verify needs the instance restored afterwards.
        juce::MemoryBlock stateBefore;
        host.pausePumpForMutation();
        inst->getStateInformation (stateBefore);
        host.resumePumpAfterMutation();

        for (auto& r : rws)
        {
            if (r.state == AssignRow::State::confirmed && r.sweep.anchors.size() >= 2)
            {
                ParamMapping m;
                m.semantic  = r.semantic;
                m.indices.add (r.resolvedIndex);
                m.paramName = assignPanel.hooks.paramName (r.resolvedIndex);
                m.kind      = r.semantic;
                for (const auto& a : r.sweep.anchors)
                    m.anchors.add ({ (double) a[1], (double) a[0] });   // AnchorPoint{norm, value}
                m.anchorsReversed = r.sweep.anchorsReversed;
                m.trust  = r.trust == "human-verified" ? Trust::humanVerified : Trust::llmClassified;
                m.method = r.sweep.method == "setread" ? AnchorMethod::setread
                          : r.sweep.method == "human-typed" ? AnchorMethod::humanTyped
                          : AnchorMethod::gettext;
                {
                    auto hb = byByIdx.find (r.resolvedIndex);
                    m.capturedBy = hb == byByIdx.end() ? CaptureSource::poll
                                 : hb->second == "gesture"      ? CaptureSource::listener
                                 : hb->second == "poll+gesture" ? CaptureSource::both
                                                                : CaptureSource::poll;
                    auto hh = hintByIdx.find (r.resolvedIndex);
                    if (hh != hintByIdx.end() && hh->second.isObject())
                    {
                        m.uiHint.valid = true;
                        m.uiHint.x = (float) (double) hh->second.getProperty ("x", 0.0);
                        m.uiHint.y = (float) (double) hh->second.getProperty ("y", 0.0);
                        m.uiHint.w = (float) (double) hh->second.getProperty ("w", 0.0);
                        m.uiHint.h = (float) (double) hh->second.getProperty ("h", 0.0);
                        m.uiHint.editorWidth  = (int) hh->second.getProperty ("editor_w", 0);
                        m.uiHint.editorHeight = (int) hh->second.getProperty ("editor_h", 0);
                        m.uiHint.screen = hh->second.getProperty ("screen", "").toString();
                    }
                }
                p.params.add (m);

                // Write-back verify through the real interpolation.
                if (r.sweep.anchors.size() >= 3)
                {
                    const int mid = r.sweep.anchors.size() / 2;
                    const float vAsk = 0.5f * (r.sweep.anchors[mid][0] + r.sweep.anchors[mid - 1][0]);
                    const float n = juce::jlimit (0.0f, 1.0f,
                                        echojay::interpolateAnchors (r.sweep.anchors, vAsk));
                    writeNormGesture (r.resolvedIndex, n);
                    const auto landed = inst->getParameters()[r.resolvedIndex]->getCurrentValueAsText();
                    double vLanded = 0.0;
                    const bool parsed = echojay::parseLeadingFloat (landed, vLanded);
                    float lo = r.sweep.anchors.getFirst()[0], hi = lo;
                    for (const auto& a : r.sweep.anchors)
                    { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                    const float tol = juce::jmax (0.02f * (hi - lo),
                                        0.6f * std::abs (r.sweep.anchors[mid][0] - r.sweep.anchors[mid - 1][0]));
                    ReadbackResult rb;
                    rb.semantic = r.semantic;
                    rb.asked = juce::String (vAsk, 4);
                    rb.wrote = juce::String (n, 6);
                    rb.read  = landed;
                    rb.match = parsed && std::abs ((float) vLanded - vAsk) <= tol;
                    p.evidence.readback.add (rb);
                }
            }
            else if (r.state == AssignRow::State::modeMaterial && r.semantic.isNotEmpty())
            {
                // The outcome the evidence demanded: the control exists, is
                // discrete, and is recorded as such -- not absent, not
                // deferred. The Tier 2 breadcrumb carries its labels.
                p.skips.add (SkipRecord (r.semantic, SkipOutcome::modeMaterial, r.skipReason));
            }
            else if (r.isSkipped() && r.semantic.isNotEmpty())
            {
                const auto oc = r.state == AssignRow::State::skipNotPresent ? SkipOutcome::notPresent
                              : r.state == AssignRow::State::skipNotAutomatable ? SkipOutcome::notAutomatable
                              : SkipOutcome::deferred;
                p.skips.add (SkipRecord (r.semantic, oc, r.skipReason));
            }
        }

        if (stateBefore.getSize() > 0)
        {
            host.pausePumpForMutation();
            inst->setStateInformation (stateBefore.getData(), (int) stateBefore.getSize());
            host.resumePumpAfterMutation();
        }

        p.groups = assignPanel.groupsForSubmit();
        for (const auto& c : assignPanel.controlsForSubmit())
            p.controls.add (c);

        if (p.hasUnresolvedContradiction())
        {
            captureReadout.setText ("SUBMIT REFUSED: unresolved probe contradiction.",
                                    juce::dontSendNotification);
            return;
        }

        auto dir = ledger.getRoot().getChildFile ("maps");
        dir.createDirectory();
        auto f = dir.getChildFile (currentFp + ".json");
        f.replaceWithText (p.toJson());

        int rb = 0, rbOk = 0;
        for (const auto& x : p.evidence.readback) { ++rb; rbOk += x.match; }

        // Every population the map carries is COUNTED on the submit line, so
        // a zero is visible at the moment of writing rather than found in
        // the file afterwards -- the spiff gate failed as "4 params, 1
        // skips" with 35 controls silently absent.
        LedgerRecord rec;
        rec.pluginId = loadedId; rec.name = loadedName; rec.stage = "submit";
        rec.outcome = LoadOutcome::ok;
        rec.detail = "map " + currentFp.substring (0, 12) + "... "
                   + juce::String (p.params.size()) + " params, "
                   + juce::String (p.groups.size()) + " groups, "
                   + juce::String (p.controls.size()) + " controls, "
                   + juce::String (p.skips.size()) + " skips, readback "
                   + juce::String (rbOk) + "/" + juce::String (rb);
        ledger.endLoad (rec);

        juce::String t;
        t << "SUBMITTED " << f.getFileName() << ": " << p.params.size() << " params, "
          << p.groups.size() << " groups, " << p.controls.size() << " controls, "
          << p.skips.size() << " skips, readback " << rbOk << "/" << rb;
        captureReadout.setText (t, juce::dontSendNotification);
        std::cout << "ASSIGN: " << t << std::endl;

        endAssignment();
    }

    //==========================================================================
    // M10: provenance identity + the upload card.

    /** Same file, same convention as ejextract: a UUID at
        ~/Library/EchoJay/machine_id, mode 0600.
    */
    static juce::String machineIdString()
    {
        auto f = juce::File ("~/Library/EchoJay").getChildFile ("machine_id");
        auto id = f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
        if (id.length() != 36)
        {
            id = juce::Uuid().toDashedString();
            f.getParentDirectory().createDirectory();
            f.replaceWithText (id + "\n");
            ::chmod (f.getFullPathName().toRawUTF8(), 0600);
        }
        return id;
    }

    /** EXPLICIT local name, set with --tester <name>, persisted in the root.
        Deliberately never derived from the hostname: "admins-MacBook-Pro" in
        the provenance of every map is not a tester identity.
    */
    juce::String testerName()
    {
        auto v = juce::JSON::parse (ledger.getRoot().getChildFile ("tester.json")
                                        .loadFileAsString());
        return v.getProperty ("name", "").toString();
    }

    /** The upload card. EXPLICIT ACTION ONLY: upload is the one irreversible
        step in the tool and is reachable by button, never by auto-advance.
    */
    void openUploadCard()
    {
        auto f = ledger.getRoot().getChildFile ("maps").getChildFile (currentFp + ".json");
        if (currentFp.isEmpty() || ! f.existsAsFile())
        {
            captureReadout.setText ("No submitted map for this plugin yet: Assign and submit first.",
                                    juce::dontSendNotification);
            return;
        }

        juce::MemoryBlock body;
        f.loadFileAsData (body);
        auto map = juce::JSON::parse (f.loadFileAsString());
        auto verdict = Mouth::structuralGate (map, testerName());

        juce::String t;
        t << "UPLOAD - " << loadedName << "\n"
          << "map: " << f.getFileName() << "  (" << (int) body.getSize() << " bytes)\n"
          << "queue state: " << Mouth::queueState (ledger.getRoot(), currentFp) << "\n\n";
        if (verdict.pass())
        {
            Mouth::setQueueState (ledger.getRoot(), currentFp, "gated", {});
            t << "STRUCTURAL GATE: PASS\n\n";

            auto dry = Mouth::writeDryRun (ledger.getRoot(), currentFp, body,
                                           testerName(), machineIdString(),
                                           juce::String (EJMAP_VERSION) + " (" + EJMAP_GIT_HASH + ")");
            Mouth::setQueueState (ledger.getRoot(), currentFp, "dry_run_written", {});
            t << "DRY RUN written: " << dry.getFullPathName() << "\n"
              << "  (the exact bytes, headers and URL shape; diff THIS against the real\n"
              << "   endpoint's contract before anything is sent)\n\n";

            auto stub = Mouth::stubMouthSubmit (ledger.getRoot(), currentFp,
                                                f.loadFileAsString(), testerName());
            Mouth::setQueueState (ledger.getRoot(), currentFp,
                                  stub.accepted ? "stub_accepted" : "stub_rejected",
                                  stub.reasons.joinIntoString ("; "));
            for (const auto& r : stub.reasons) t << r << "\n";
            t << "\nNOTHING HAS BEEN UPLOADED. There is no real endpoint yet; the stub\n"
                 "is a hypothesis about the server, not a test of it.\n";
        }
        else
        {
            Mouth::setQueueState (ledger.getRoot(), currentFp, "rejected",
                                  verdict.rejections.joinIntoString ("; "));
            t << "STRUCTURAL GATE: REFUSED\n";
            for (const auto& r : verdict.rejections) t << "  - " << r << "\n";
        }

        std::cout << "UPLOAD:\n" << t << std::endl;
        captureReadout.setText (t.upToFirstOccurrenceOf ("\n\n", false, false)
                                  + "  (full report on stdout)", juce::dontSendNotification);
        uploadCardText = t;
    }

    juce::String uploadCardText;

    /** One sweep, full protocol: inflight record (a sweep is a crash window
        on unknown plugins), pump paused and drained (mutations are not
        specified against a concurrent processBlock), row recorded whatever
        the verdict. The Sweep button and the assignment loop both come
        through here, so their evidence is identical.
    */
    SweepOutcome runSweep (int idx)
    {
        auto* inst = host.getInstance();
        if (inst == nullptr) return {};
        const auto name = juce::isPositiveAndBelow (idx, inst->getParameters().size())
                            ? inst->getParameters()[idx]->getName (48) : juce::String();
        beginSweepInflight (idx, name);
        host.pausePumpForMutation();
        auto outcome = sweepOneIndex (*inst, idx, watchdog, loadedId);
        host.resumePumpAfterMutation();
        endSweepInflight (idx, outcome);
        recordSweep (idx, name, outcome);
        return outcome;
    }

    /** The sweep is a crash window on plugins nothing has swept before, so it
        runs under the same inflight protocol as a load: begin writes
        inflight.json, a crash leaves it behind as attributable evidence, and
        a completed sweep records its outcome as a ledger row.
    */
    void beginSweepInflight (int index, const juce::String& name)
    {
        ledger.beginLoad (loadedId, loadedName, {}, {}, {},
                          "sweep", "sweepOneIndex idx=" + juce::String (index)
                                     + " (" + name + ")");
    }

    void endSweepInflight (int index, const SweepOutcome& sw)
    {
        LedgerRecord rec;
        rec.pluginId = loadedId;
        rec.name     = loadedName;
        rec.stage    = "sweep";
        rec.outcome  = LoadOutcome::ok;     // the process survived; the sweep's own
                                            // verdict lives in the captures row
        rec.detail   = "idx " + juce::String (index) + ": "
                     + (sw.ok ? "anchors" : (sw.flat ? "text liar" : "refused"))
                     + " (" + sw.method + ") " + sw.reason.substring (0, 140);
        ledger.endLoad (rec);
    }

    /** The sweep row. Points travel with the anchors because anchor values
        are raw parsed floats with no unit normalisation (assignment owns the
        unit), so the display texts are the evidence a later parse needs.
    */
    void recordSweep (int index, const juce::String& name, const SweepOutcome& sw)
    {
        if (loadedId.isEmpty())
        {
            std::cout << "SWEEP: REFUSED to write a sweep row: no plugin identity" << std::endl;
            return;
        }

        auto* o = new juce::DynamicObject();
        o->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
        o->setProperty ("kind", "sweep");
        o->setProperty ("plugin", loadedName);
        o->setProperty ("plugin_id", loadedId);
        o->setProperty ("param_count", cal.paramCount);
        o->setProperty ("index", index);
        o->setProperty ("param_name", name);
        o->setProperty ("ok", sw.ok);
        o->setProperty ("method", sw.method);
        o->setProperty ("flat", sw.flat);
        o->setProperty ("setread_refused", sw.setreadRefused);
        o->setProperty ("anchors_reversed", sw.anchorsReversed);
        o->setProperty ("identity_display", sw.identityDisplay);
        if (sw.unitFamily.isNotEmpty())
            o->setProperty ("unit_family", sw.unitFamily);
        o->setProperty ("rejected_points", sw.rejectedPoints);
        o->setProperty ("unparsed_points", sw.unparsedPoints);
        o->setProperty ("duration_ms", sw.durationMs);
        o->setProperty ("reason", sw.reason);

        juce::Array<juce::var> anch;
        for (const auto& a : sw.anchors)
        {
            juce::Array<juce::var> pair; pair.add (a[0]); pair.add (a[1]);
            anch.add (juce::var (pair));
        }
        o->setProperty ("anchors", juce::var (anch));

        juce::Array<juce::var> pts;
        for (const auto& sp : sw.points)
        {
            auto* pt = new juce::DynamicObject();
            pt->setProperty ("n", sp.n);
            pt->setProperty ("t", sp.t);
            pts.add (juce::var (pt));
        }
        o->setProperty ("points", juce::var (pts));

        auto f = ledger.runArtifact ("captures", "jsonl");
        juce::FileOutputStream out (f);
        if (out.openedOk())
        {
            out.setPosition (f.getSize());
            out.writeText (juce::JSON::toString (juce::var (o), true) + "\n", false, false, nullptr);
            out.flush();
        }
    }

    /** The noise-promotion evidence probe: was a mouse button held INSIDE THE
        HOSTED EDITOR shortly before the detection tick?

        Both halves of the predicate carry weight.

        The window (kGrabWindowMs) rather than the whole ring: a knob drag has
        the button down at detection or released at most one poll tick before
        it (the floor is 4 Hz, so 250 ms); a grab from two seconds ago explains
        nothing about a meter tick now.

        Inside the editor rather than anywhere: the Arm click itself is a
        mouse-down, and a self-changing parameter is typically detected within
        one tick of arming -- so "any recent mouse-down" would suppress
        promotion for exactly the self-changing indices promotion exists to
        catch. The Arm button is outside the hosted editor; a hand on the
        plugin is inside it.

        What this cannot see: a MIDI controller or host automation moving a
        parameter with no mouse involved. Those count toward promotion today
        and are wrong to count; the listener bank's gesture reports are the
        evidence that covers them, and they join this probe when that lands.
    */
    static constexpr int kGrabWindowMs = 750;

    bool mouseGrabInEditorNear (juce::uint32 detectedAtMs)
    {
        if (hostedEditor == nullptr)
            return false;

        MouseRing::Sample s;
        if (! ring.downWithin (detectedAtMs > (juce::uint32) kGrabWindowMs
                                 ? detectedAtMs - (juce::uint32) kGrabWindowMs : 0,
                               detectedAtMs, s))
            return false;

        return hostedEditor->getLocalBounds()
                 .contains (hostedEditor->getLocalPoint (nullptr, s.screenPos));
    }

    /** Human picks which of a gesture's parameters they meant. The others stay
        as co-moved rather than being thrown away: that Band 1 Freq and Band 1
        Gain move together is a fact about the plugin.
    */
    void chooseCandidate()
    {
        const int sel = candidatePicker.getSelectedId() - 1;   // ids are 1-based
        if (! juce::isPositiveAndBelow (sel, lastGesture.indices.size()))
            return;

        const int intended = lastGesture.indices[sel];

        juce::Array<int> coMoved;
        juce::StringArray coNames;
        for (int i = 0; i < lastGesture.indices.size(); ++i)
            if (i != sel)
            {
                coMoved.add (lastGesture.indices[i]);
                coNames.add (lastGesture.names[i]);
            }

        juce::String t;
        t << "CAPTURED index " << intended << " (" << lastGesture.names[sel] << ")";
        if (! coMoved.isEmpty())
        {
            t << "  co-moved [";
            for (int i = 0; i < coMoved.size(); ++i)
                t << (i ? ", " : "") << coMoved[i] << ":" << coNames[i];
            t << "]";
        }
        captureReadout.setText (t, juce::dontSendNotification);
        std::cout << "CAPTURE: " << t << std::endl;

        recordCapture ("captured_from_gesture", intended, lastGesture.names[sel],
                       coMoved, coNames, lastGesture.reason,
                       lastGesture.sameDirection, lastGesture.magnitudeRatio,
                       "human_pick");

        candidatePicker.setVisible (false);
        resized();
        refreshMaskUi();
        lastCapturedIndex = intended;
        sweepButton.setEnabled (true);
        armButton.setEnabled (true);
    }

    /** Captures are written per run, so co-moved sets survive the readout. The
        payload writer is M10; this is the interim record, and it is a record
        rather than a print.

        THE WATCHED SET IS RECORDED, NOT JUST THE MOVED SET.
        param_count and noise_mask are written on every row so that the
        complement -- the parameters that were being watched and did NOT move --
        is derivable from the record alone. Without it these rows cannot support
        M5's negative discriminator, and a capture made today would have to be
        made again.

        The mask is written PER ROW rather than once per plugin because it grows:
        retroactive promotion adds indices mid-session, so the mask that applied
        to this capture is not necessarily the mask that applied to the last one.

        SCOPING RULE FOR THE NEGATIVE SIGNAL. Read this before using the
        complement for anything.

          A parameter not moving is evidence ONLY relative to a gesture that
          moved something. "Mono Maker did not move" means something when paired
          with "and this same gesture moved LF Freq and LF Gain"; it means
          nothing on its own, because the human may simply not have touched
          anything that would have moved it.

          Therefore the complement is valid ONLY within one row -- one arm, one
          gesture -- and MUST NEVER be accumulated across arms. Unioning
          non-movement over a session would conclude that almost every parameter
          is outside almost every group, which is both false and the exact shape
          of a confident wrong answer. If a stronger negative is wanted, it comes
          from more anchored gestures, each scoped to its own row, never from
          summing absences.
    */
    /** sameDirection / magnitudeRatio describe the SHAPE of a multi-parameter
        move and are written only when there is a co-moved set, because the shape
        of one delta is not a shape. They are observations, not a classification:
        Kind::twins used to turn "same direction, within 1.5x" into an asserted
        mechanism, and that assertion is retired. The numbers survive it.
    */
    void recordCapture (const juce::String& kind, int intended, const juce::String& name,
                        const juce::Array<int>& coMoved, const juce::StringArray& coNames,
                        const juce::String& reason,
                        bool sameDirection = false, float magnitudeRatio = 0.0f,
                        const juce::String& capturedBy = {})
    {
        // REFUSE rather than write an unattributable row. A capture with no
        // plugin id is indistinguishable from evidence but cannot be traced to
        // anything, so it is worse than an absence: it would be counted. The
        // refusal itself is recorded, per the rule that nothing disappears
        // without a reason on disk.
        if (loadedId.isEmpty())
        {
            juce::String msg;
            msg << "REFUSED to write a capture row: no plugin identity (kind=" << kind
                << ", index=" << intended << ", reason=" << reason.substring (0, 80) << ")";
            std::cout << "CAPTURE: " << msg << std::endl;

            auto rej = ledger.runArtifact ("captures-rejected", "log");
            juce::FileOutputStream rout (rej);
            if (rout.openedOk())
            {
                rout.setPosition (rej.getSize());
                rout.writeText (juce::Time::getCurrentTime().toISO8601 (true) + "  " + msg + "\n",
                                false, false, nullptr);
                rout.flush();
            }
            return;
        }

        auto* o = new juce::DynamicObject();
        o->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
        o->setProperty ("kind", kind);
        o->setProperty ("plugin", loadedName);
        o->setProperty ("plugin_id", loadedId);
        o->setProperty ("rate_hz", cal.rateHz);
        o->setProperty ("sweep_us", cal.sweepMicros);
        o->setProperty ("noise_mask_samples", mask.samples);
        o->setProperty ("noise_mask_method", mask.method);

        // The watched set, recorded so the complement is derivable:
        // watched = [0, param_count) minus noise_mask.
        o->setProperty ("param_count", cal.paramCount);
        juce::Array<juce::var> maskIdx;
        for (int i = 0; i < mask.indices.size(); ++i)
            maskIdx.add (mask.indices[i]);
        o->setProperty ("noise_mask", juce::var (maskIdx));
        o->setProperty ("negative_signal_scope", "single_row_only");
        if (intended >= 0)
        {
            o->setProperty ("index", intended);
            o->setProperty ("param_name", name);
        }
        juce::Array<juce::var> cm;
        for (int i = 0; i < coMoved.size(); ++i)
        {
            auto* c = new juce::DynamicObject();
            c->setProperty ("index", coMoved[i]);
            c->setProperty ("param_name", coNames[i]);
            cm.add (juce::var (c));
        }
        o->setProperty ("co_moved", juce::var (cm));
        if (! coMoved.isEmpty())
        {
            o->setProperty ("same_direction", sameDirection);
            o->setProperty ("magnitude_ratio", magnitudeRatio);
        }
        if (capturedBy.isNotEmpty())
            o->setProperty ("captured_by", capturedBy);
        o->setProperty ("ui_hint", lastHint.toVar());
        o->setProperty ("reason", reason);

        auto f = ledger.runArtifact ("captures", "jsonl");
        juce::FileOutputStream out (f);
        if (out.openedOk())
        {
            out.setPosition (f.getSize());
            out.writeText (juce::JSON::toString (juce::var (o), true) + "\n", false, false, nullptr);
            out.flush();
        }
    }

    void armCapture()
    {
        auto* inst = host.getInstance();
        if (inst == nullptr || ! cal.valid)
            return;

        armButton.setEnabled (false);
        unmaskButton.setEnabled (false);     // the poll reads the mask while armed
        captureReadout.setText ("ARMED at " + juce::String (cal.rateHz, 1)
                                  + " Hz - move one control", juce::dontSendNotification);

        capture.arm (*inst, cal, mask, [this] (const CaptureEngine::Result& r)
        {
            // Every result names its parameters. "[0, 2, 3, 7, 8]" is not
            // something a human can act on.
            juce::String t;
            t << r.kindString().toUpperCase();
            for (int i = 0; i < r.indices.size(); ++i)
                t << (i ? ", " : "  ") << r.indices[i] << ":"
                  << (i < r.names.size() ? r.names[i] : juce::String());
            t << "  -  " << r.reason;
            if (! mask.promotions.isEmpty())
                t << "   |  index " << mask.promotions.getReference (mask.promotions.size() - 1).index
                  << " " << mask.promotions.getReference (mask.promotions.size() - 1).reason;

            lastHint = hintFor (r.detectedAtMs);
            if (lastHint.valid)
                t << "  ui_hint " << juce::String (lastHint.x, 3) << "," << juce::String (lastHint.y, 3)
                  << " of " << lastHint.editorWidth << "x" << lastHint.editorHeight
                  << " on " << lastHint.screen;
            else
                t << "  ui_hint null (mouse outside the editor, or no sample)";

            captureReadout.setText (t, juce::dontSendNotification);
            std::cout << "CAPTURE: " << t << std::endl;

            if (r.kind == CaptureEngine::Result::Kind::gesture)
            {
                // The RAW gesture is recorded here, before the human is asked.
                // It used to be recorded only after a pick, so an abandoned
                // picker left nothing -- a silent drop, and a consequential
                // one: run 114642's first promotion (11:52:02) needed two
                // prior idx-2 appearances, and neither left a row. The pick,
                // when it comes, writes captured_from_gesture as its own row,
                // which is the same two-row shape the self-test documents.
                recordCapture (r.kindString(), -1, juce::String(),
                               r.indices, r.names,
                               r.reason, r.sameDirection, r.magnitudeRatio);
                flushPromotionRows();
                refreshMaskUi();

                lastGesture = r;
                candidatePicker.clear (juce::dontSendNotification);
                for (int i = 0; i < r.indices.size(); ++i)
                    candidatePicker.addItem (juce::String (r.indices[i]) + ":  " + r.names[i], i + 1);
                candidatePicker.setVisible (true);
                resized();
                return;   // Arm stays disabled until a choice is made
            }

            // The primary is what was captured; everything else in the moved
            // set is co-moved. For a poll-only single capture that reduces to
            // the old behaviour exactly; for a gesture-resolved multi-move the
            // plugin's named index is the row's index and the followers are
            // co-moved, with captured_by saying which mechanism decided.
            const int intended = r.primaryIndex >= 0
                                   ? r.primaryIndex
                                   : (r.indices.size() == 1 ? r.indices[0] : -1);
            juce::Array<int> co;
            juce::StringArray coN;
            for (int i = 0; i < r.indices.size(); ++i)
                if (r.indices[i] != intended)
                { co.add (r.indices[i]); coN.add (i < r.names.size() ? r.names[i] : juce::String()); }

            const int pos = r.indices.indexOf (intended);
            recordCapture (r.kindString(), intended,
                           pos >= 0 && pos < r.names.size() ? r.names[pos] : juce::String(),
                           co, coN,
                           r.reason, r.sameDirection, r.magnitudeRatio, r.capturedBy);
            flushPromotionRows();
            refreshMaskUi();

            if (r.kind == CaptureEngine::Result::Kind::captured && intended >= 0)
            {
                lastCapturedIndex = intended;
                sweepButton.setEnabled (true);
            }
            armButton.setEnabled (true);
        });
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

        // The plugin that just killed the session gets one refusal. A
        // host-attributed crash is spared from quarantine deliberately, so
        // without this a restart plus muscle memory walks straight back in.
        if (id == doNotAutoReload)
        {
            doNotAutoReload.clear();
            status.setText (sp.desc.name + " is what the previous session died in. "
                            "Click Load again to try it anyway.",
                            juce::dontSendNotification);
            return;
        }

        capture.stop();          // before unload: teardown is what a read can race
        listeners.detach();      // same rule: they hang off the instance's parameters
        detachEditor();
        host.unload();

        // Capture identity is established HERE, in the load path, not later in
        // prepareCapture. A capture row that cannot be attributed to a plugin is
        // worse than a missing row, because it still counts as evidence.
        loadedName = sp.desc.name;
        loadedId   = id;

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
            markLoadSucceeded();
            loadedDesc = sp.desc;
            attachEditor();
            listeners.attach (*host.getInstance());
            prepareCapture (sp.desc.name, id);
            assignButton.setEnabled (true);
            uploadButton.setEnabled (true);
            status.setText (sp.desc.name + ": " + juce::String (result.paramCount)
                              + " params, editor open in " + juce::String (elapsed) + " ms",
                            juce::dontSendNotification);
        }
        else
        {
            // Nothing is loaded, so nothing may be attributed to it.
            loadedName.clear();
            loadedId.clear();

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
    CaptureEngine capture;
    PluginScanner scanner;
    PluginHost    host;
    ParamListenerBank listeners;   // after host: destroyed (and detached) first

    PluginScanner::Result lastScan;
    juce::Array<ScannedPlugin> rows;        // the whole scan result, never filtered
    juce::Array<int>           visibleRows; // indices into rows, what the list shows
    juce::String crashedId;

    juce::TextButton scanButton, loadButton, releaseButton, summaryButton, armButton, sweepButton, typeButton, assignButton, uploadButton;
    juce::ToggleButton deepToggle;
    AssignPanel assignPanel;
    bool assigning = false;
    juce::String currentFp;
    juce::PluginDescription loadedDesc;
    int lastCapturedIndex = -1;    // what Sweep targets: the last captured index
    int lastSweptIndex = -1;       // what Type targets: the last swept index
    juce::String lastSweptName;
    SweepOutcome lastSweepOutcome;

    CurveView curveView;

    // Typed-anchor flow state. One step per typedSteps entry; the plugin is
    // parked at each step while the human transcribes the GUI reading.
    bool typedActive = false;
    int  typedStep = 0;
    juce::Array<juce::Array<float>> typedPairs;
    juce::StringArray typedTexts;
    juce::MemoryBlock typedStateBefore;
    juce::Label      typedPrompt;
    juce::TextEditor typedEntry;
    juce::TextButton typedNextButton, typedCancelButton;
    juce::Label      captureReadout;
    CaptureEngine::Calibration cal;
    CaptureEngine::NoiseMask   mask;
    int stage = 0, failures = 0;
    int suppressIdx = -1;         // promotion-suppression self-test target
    int kneeTestIdx = -1;         // assign self-test: the labelled discrete switch
    juce::StringArray bandMemberNames;   // band self-test: member names to touch
    juce::String bandImposterName;
    int bandMemberCursor = 0;
    bool grabSeenA = false;       // any phase-A cycle saw the grab
    juce::ComboBox candidatePicker;
    juce::ComboBox maskPicker;
    juce::TextButton unmaskButton;
    int promotionsFlushed = 0;    // promotions already written as rows
    CaptureEngine::Result lastGesture;
    juce::String loadedName, loadedId;
    juce::PluginDescription stallDesc;
    int stallLeft = 0, stallAttempt = 0;
    MouseRing    ring;
    UiHint       lastHint;
    juce::ToggleButton signalToggle;
    juce::Array<int> qualified;
    bool busy = false;
    int  loadDepth = 0, maxLoadDepth = 0, loadCalls = 0, loadRejected = 0;

    bool         isSupervised = false;
    int          restartsThisSession = 0;
    juce::String lastExitCause, doNotAutoReload;
    bool         markedLoadOk = false;

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
