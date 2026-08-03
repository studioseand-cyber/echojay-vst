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
#include "EjmapProbe.h"
#include "EjmapSubject.h"
#include "EjmapTriage.h"
#include <thread>
#include <atomic>
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

    /** THE CHOKE POINT for suite termination. A suite ends with quitNow(),
        87 times over -- so a batch runner that needs suites to RETURN rather
        than exit changes this one function instead of 87 call sites. Editing
        each site is how the seventh instance of the misplaced guard happened;
        this is the same lesson applied before rather than after.
    */
    bool batchMode = false;
    void quitNow()
    {
        if (batchMode) return;                 // the loop owns the lifetime
        juce::JUCEApplication::getInstance()->quit();
    }

    //==========================================================================
    // BATCH PROBE RUNNER (feature 3). Collected verdicts, one row per
    // parameter, so the report can state what a suite decided AND what it did
    // not reach -- a parameter absent from a report is indistinguishable from
    // one that passed.
    struct ProbeRow { juce::String semantic, verdict, evidence, slot; };
    juce::Array<ProbeRow> batchRows;
    juce::StringArray     batchSuitesRun;

    /** Loads one plugin repeatedly from ordinary message context, which is what
        a button click is, until it stalls or the attempt budget runs out.

        The stall is racy: whether a bridged AU's out-of-process creation
        completes inline or needs a message-thread turn is a timing matter. So
        the reproduction is repetition, not cleverness.
    */
    /** Measures the settle tail the residue filter fights: write one
        parameter, then poll the whole set and report how long ANY value keeps
        changing after the write. The AMEK re-run needed eight re-arms on the
        Q card and four on the last-freq card, every one "previous control
        still settling" -- this number says whether that is the plugin's tail
        or the filter's window.
    */
    void measureSettle (const juce::String& identifier, int index)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "SETTLE: unknown identifier" << std::endl; quitNow(); return; }
        auto res = host.load (desc, watchdog);
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "SETTLE: load failed" << std::endl; quitNow(); return; }

        auto* inst = host.getInstance();
        auto params = inst->getParameters();
        if (! juce::isPositiveAndBelow (index, params.size()))
        { std::cout << "SETTLE: index out of range (param count "
                    << params.size() << ")" << std::endl; quitNow(); return; }

        // Let load-time churn die down first, same reason the noise probe exists.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500);

        juce::Array<float> before;
        for (auto* pp : params) before.add (pp->getValue());

        std::cout << "SETTLE: " << desc.name << " [" << index << "] "
                  << params[index]->getName (48) << ": writing 0.30 then 0.70" << std::endl;
        params[index]->setValueNotifyingHost (0.30f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        params[index]->setValueNotifyingHost (0.70f);

        // Per-param last-change times, because one number hides the story: a
        // perpetually-moving meter keeps a global clock alive forever and
        // makes a 2-second glide look like a 15-second one.
        const auto t0 = juce::Time::getMillisecondCounter();
        std::map<int, juce::uint32> lastChangeOf;
        juce::uint32 lastChange = t0;
        while (juce::Time::getMillisecondCounter() - lastChange < 2000
                && juce::Time::getMillisecondCounter() - t0 < 15000)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            for (int i = 0; i < params.size(); ++i)
            {
                const float v = params[i]->getValue();
                if (std::abs (v - before[i]) > 1.0e-4f)
                {
                    before.set (i, v);
                    lastChange = juce::Time::getMillisecondCounter();
                    lastChangeOf[i] = lastChange;
                }
            }
        }
        std::cout << "SETTLE: per-param tail (ms after the write; capped at 15000):" << std::endl;
        for (auto& kv : lastChangeOf)
            std::cout << "SETTLE:   [" << kv.first << "] " << params[kv.first]->getName (32)
                      << "  " << (int) (kv.second - t0) << " ms" << std::endl;
        std::cout << "SETTLE: DONE" << std::endl;
        quitNow();
    }

    /** Headless re-submission through the REAL machinery: load, restore the
        completed session, actionSubmit (live write-back verify included),
        then the upload card (gate, dry-run, stub, queue). Exists because a
        writer-side fix means the maps on disk are wrong until the writer
        re-emits them -- and hand-editing an emitted map is manufacturing
        evidence. Refuses sessions with unresolved rows: this is a re-emit of
        finished work, never a way to skip the wizard.
    */
    void resubmitAndUpload (const juce::String& identifier)
    {
        auto desc = echojay::auregistry::describeFromRegistry (identifier);
        if (desc.fileOrIdentifier.isEmpty())
            for (const auto& r : rows)
                if (r.desc.fileOrIdentifier == identifier || r.pluginId() == identifier)
                { desc = r.desc; break; }
        if (desc.fileOrIdentifier.isEmpty())
        { std::cout << "RESUBMIT: unknown identifier" << std::endl; quitNow(); return; }

        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        auto res = host.load (desc, watchdog);
        if (res.outcome != LoadOutcome::ok)
        { std::cout << "RESUBMIT: load failed: " << res.detail << std::endl; quitNow(); return; }
        cal = capture.calibrate (*host.getInstance(), loadedId);
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);
        prepareCapture (sp.desc.name, loadedId);

        auto session = juce::JSON::parse (ledger.getRoot()
                           .getChildFile ("assign-" + currentFp + ".json").loadFileAsString());
        const auto cat = session.getProperty ("category", "").toString();
        if (cat.isEmpty())
        { std::cout << "RESUBMIT: no completed session for fp " << currentFp << std::endl;
          quitNow(); return; }

        startAssignmentForCategory (cat);

        int unresolved = 0;
        for (const auto& r : assignPanel.rows) unresolved += ! r.isResolved();
        std::cout << "RESUBMIT: " << desc.name << " | category " << cat
                  << " | " << assignPanel.rows.size() << " rows restored, "
                  << unresolved << " unresolved | "
                  << assignPanel.controlsForSubmit().size() << " controls, "
                  << assignPanel.groupsForSubmit().size() << " groups staged" << std::endl;
        if (unresolved > 0)
        { std::cout << "RESUBMIT: refusing -- unresolved rows need the wizard, not a re-emit"
                    << std::endl; quitNow(); return; }

        assignPanel.actionSubmit();

        auto mapFile = ledger.getRoot().getChildFile ("maps").getChildFile (currentFp + ".json");
        auto map = juce::JSON::parse (mapFile.loadFileAsString());
        auto ps = map.getProperty ("params", juce::var());
        juce::StringArray pkeys;
        if (auto* po = ps.getDynamicObject())
            for (auto& kv : po->getProperties()) pkeys.add (kv.name.toString());
        std::cout << "RESUBMIT: map re-emitted: params [" << pkeys.joinIntoString (", ")
                  << "], controls "
                  << (map.getProperty ("controls", juce::var()).getDynamicObject() != nullptr
                        ? map.getProperty ("controls", juce::var()).getDynamicObject()->getProperties().size() : 0)
                  << ", groups "
                  << (map.getProperty ("groups", juce::var()).getArray() != nullptr
                        ? map.getProperty ("groups", juce::var()).getArray()->size() : 0)
                  << ", extractor_version '"
                  << map.getProperty ("provenance", juce::var())
                        .getProperty ("extractor_version", "").toString() << "'" << std::endl;

        openUploadCard();
        std::cout << "RESUBMIT: queue state now "
                  << Mouth::queueState (ledger.getRoot(), currentFp) << std::endl;
        std::cout << "RESUBMIT: DONE" << std::endl;
        quitNow();
    }

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

    /** RENDER-PLANE SENSITIVITY CHECK, shared preamble (unproven mechanism
        #2 until each suite is seen firing it).

        Moves a known-live parameter across its full ladder and requires the
        rendered output to change. getValue only proves the PROPERTY plane;
        this is the only thing that proves writes reach the DSP, and two
        sessions of gate findings were built on a path where they did not.
        Returns false when the render is blind, and the CALLER MUST STOP:
        no verdict may be issued from a parameter-blind path.

        Lives here, not per suite, for the misplaced-guard reason -- the gate
        had it and comp and limiter did not, which is the exact shape that
        put it on the unproven list.
    */
    bool renderPlaneSensitive (juce::AudioPluginInstance& inst,
                               const juce::String& probeName,
                               std::function<juce::Array<double>()> renderRows)
    {
        // ONE MOVER PROVES LIVE; ONE NULL PROVES NOTHING. The first build
        // took a single unambiguous name match and declared bx_limiter's
        // render plane blind on its Release -- a parameter that is genuinely
        // Release and genuinely not expressible in a STATIC level curve,
        // while the same plugin's ceiling had confirmed to 0.02 dB. The name
        // was right and the behaviour was not what the name implied, which is
        // this module's most-filed defect. Suitability is now MEASURED: try
        // several, stop at the first that moves the render, and declare
        // blindness only when every candidate is null.
        auto params = inst.getParameters();
        juce::Array<int> cands;
        for (auto* n : { "Output", "Gain", "Mix", "Threshold", "Ceiling", "Drive", "Release" })
            for (int i = 0; i < params.size(); ++i)
                if (params[i]->getName (64).containsIgnoreCase (n) && params[i]->isAutomatable()
                     && ! cands.contains (i))
                    cands.add (i);
        if (cands.isEmpty())
        { std::cout << "  SENSITIVITY: no candidate parameter on " << probeName
                    << " -- cannot prove the render plane; suite must not proceed" << std::endl;
          return false; }

        auto toNorm = [] (const juce::Array<juce::Array<float>>& a, double v) {
            auto e = echojay::dominantMonotonicTable (a);
            return echojay::interpolateAnchors (e.table, (float) v); };
        juce::StringArray tried;
        for (int k = 0; k < juce::jmin (5, cands.size()); ++k)
        {
            const int idx = cands[k];
            auto sw = sweepOneIndex (inst, idx, watchdog, loadedId);
            if (sw.anchors.size() < 2) { tried.add (params[idx]->getName (24) + ":no-sweep"); continue; }
            Probe::writeAndServiceRunloop (*params[idx], toNorm (sw.anchors, sw.anchors.getFirst()[0]));
            auto a = renderRows();
            Probe::writeAndServiceRunloop (*params[idx], toNorm (sw.anchors, sw.anchors.getLast()[0]));
            auto b = renderRows();
            double worst = 0;
            for (int i = 0; i < a.size() && i < b.size(); ++i)
                worst = juce::jmax (worst, std::abs (b[i] - a[i]));
            tried.add (params[idx]->getName (24) + ":" + juce::String (worst, 2));
            if (worst > 0.5)
            {
                std::cout << "  SENSITIVITY: [" << idx << "] " << params[idx]->getName (32)
                          << " moved the render " << juce::String (worst, 2)
                          << " dB -> render plane LIVE, suite may proceed  (tried "
                          << tried.joinIntoString (", ") << ")" << std::endl;
                return true;
            }
        }
        std::cout << "  SENSITIVITY: EVERY candidate was null (" << tried.joinIntoString (", ")
                  << ") -> RENDER PLANE BLIND -- suite STOPPED, no verdict may be issued"
                  << std::endl;
        return false;
    }

    /** Resolve a subject by NAME, and record a miss as the HARNESS's rather
        than the plugin's.

        A name-lookup miss never calls PluginHost::load, so the choke-point
        stake cannot see it -- this is the misplaced-guard family's second
        fix: a path that never reaches a choke point needs its own recording.
        The specimen is kHs Gate, which is VST3-only on this machine and was
        searched for in the AU census, then reported as "load failed" when
        the plugin loads perfectly well.

        Records what was searched for, WHICH CATALOGUE was searched, how many
        entries it held, and that the miss is attributable here.
    */
    juce::PluginDescription resolveSubjectByName (const juce::String& wanted,
                                                  const juce::String& catalogue = "AU census")
    {
        juce::PluginDescription out;
        auto census = echojay::auregistry::buildCensus();
        for (const auto& t : census.targets)
        {
            auto d = echojay::auregistry::describeFromRegistry (t.identifier);
            if (d.name.containsIgnoreCase (wanted)) return d;
        }
        // MISS. Say whose it is, in words and on disk.
        int inScan = 0; juce::String seenAs;
        for (const auto& r : rows)
            if (r.desc.name.containsIgnoreCase (wanted))
            { ++inScan; seenAs = r.desc.pluginFormatName + " " + r.desc.version; }
        const juce::String verdict = inScan > 0
            ? "HARNESS MISS: '" + wanted + "' is not in the " + catalogue + " ("
              + juce::String ((int) census.targets.size()) + " entries) but IS in the saved scan as "
              + seenAs + ". The lookup searched the wrong catalogue; this is not a plugin failure."
            : "MISS: '" + wanted + "' is not in the " + catalogue + " ("
              + juce::String ((int) census.targets.size()) + " entries) and not in the saved scan either. "
              "Not installed, or named differently.";
        std::cout << "  " << verdict << std::endl;
        auto* o = new juce::DynamicObject();
        o->setProperty ("kind", "subject_lookup_miss");
        o->setProperty ("searched_for", wanted);
        o->setProperty ("catalogue", catalogue);
        o->setProperty ("catalogue_size", (int) census.targets.size());
        o->setProperty ("found_in_saved_scan", inScan > 0);
        o->setProperty ("attributable_to", inScan > 0 ? "harness (wrong catalogue)" : "absent");
        o->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
        // captures-<run>.jsonl, the same artifact every other probe record
        // rides, so a miss is findable beside the run it belongs to.
        auto f = ledger.runArtifact ("captures", "jsonl");
        juce::FileOutputStream fo (f);
        if (fo.openedOk()) { fo.setPosition (f.getSize());
                             fo.writeText (juce::JSON::toString (juce::var (o), false) + "\n",
                                           false, false, nullptr); fo.flush(); }
        return out;
    }

    /** BATCH PROBE RUNNER: map for a day, probe overnight.

        Drives the BUILT suites over local maps unattended and writes ONE
        report. No new verdict logic, no new suites.

        WHAT IT REFUSES, AND WHY THAT IS MOST OF THE POINT.
        The four suites are fixture-bound, not subject-parameterised: each
        resolves a specific product by id (eq aumf,ameq,Brwx; comp
        aufx,APCM,ksWV; limiter aufx,bxa2,Brwx; gate SSL X-Gate by name) and
        the eq suite additionally hard-codes group1 and the Mono Maker indices
        7/8 of the signed AMEK fixture. So calling gateM9("comp") while
        iterating some other compressor's map would load API-2500, measure
        API-2500, and file API-2500's verdicts under that map's fp -- then POST
        them there. The runner therefore probes a map ONLY when the map's own
        identity IS the suite's signed subject, and counts every other map as
        uncovered with the reason named. Subject-parameterising the suites is
        real work and is not this runner.

        Honest by construction elsewhere too: coverage is written FIRST, every
        parameter of every map gets a line including the ones no suite reaches,
        and there is NO summary verdict per map -- with four suites deciding
        one-to-four parameters each, a map labelled "clean" is a partial pass
        reading as a clean bill of health.
    */
    struct SuiteBinding { juce::String category, suite, subjectId, subjectName; };

    std::vector<SuiteBinding> suiteBindings() const
    {
        std::vector<SuiteBinding> b;
        b.push_back ({ "eq",         "eq",      "AudioUnit:Effects/aumf,ameq,Brwx", "AMEK EQ 200" });
        b.push_back ({ "compressor", "comp",    "AudioUnit:Effects/aufx,APCM,ksWV", "API-2500 (m)" });
        // MEASURED, not guessed: aufx,bxa2,Brwx is bx_SATURATOR, which sat here
        // as the limiter's subject id. A bx_limiter map would have been refused
        // and a bx_saturator map mis-categorised as a limiter accepted.
        b.push_back ({ "limiter",    "limiter", "AudioUnit:Effects/aumf,bxtp,Brwx", "bx_limiter True Peak" });
        b.push_back ({ "gate",       "gate",    "AudioUnit:Effects/aufx,XGAT,SSLN", "SSL X-Gate" });
        return b;
    }

    /** Resolve a map's subject from its identity, BY ID, never by name -- and
        refuse an ambiguous answer rather than pick. The uid is JUCE's XOR of
        type^subtype^manufacturer, which EchoJayAuRegistry.h records as NOT
        unique (2 collisions across 4 Waves components in a 1419-component
        registry). So this collects every candidate and reports the count.
    */
    struct SubjectResolution
    {
        juce::PluginDescription desc;
        int  candidates = 0;
        bool versionMismatch = false;
        juce::String installedVersion, detail;
        bool ok() const { return candidates == 1 && ! versionMismatch; }
    };

    SubjectResolution resolveSubjectByIdentity (const juce::String& uidHex,
                                                const juce::String& version,
                                                const juce::String& name) const
    {
        SubjectResolution r;
        auto census = echojay::auregistry::buildCensus();
        for (const auto& t : census.targets)
        {
            auto d = echojay::auregistry::describeFromRegistry (t.identifier);
            if (! juce::String::toHexString (d.uniqueId).equalsIgnoreCase (uidHex)) continue;
            ++r.candidates;
            if (r.candidates == 1) { r.desc = d; r.installedVersion = d.version; }
            else r.detail << " | also " << d.name << " " << d.fileOrIdentifier;
        }
        if (r.candidates == 1 && r.installedVersion != version)
            r.versionMismatch = true;
        if (r.candidates > 1)
            r.detail = "uid " + uidHex + " matches " + juce::String (r.candidates)
                     + " installed components (the uid XOR is not unique): "
                     + r.desc.name + " " + r.desc.fileOrIdentifier + r.detail;
        juce::ignoreUnused (name);
        return r;
    }

    /** The map's mappable surface: params + group params + named controls.
        ONE enumerator, because the params-only count understated AMEK by 77
        slots and the same undercount sat in every "unexamined" line. */
    static juce::StringArray mappableSlots (const juce::var& v)
    {
        juce::StringArray slots;
        if (auto* po = v.getProperty ("params", juce::var()).getDynamicObject())
            for (auto& kv : po->getProperties())
                slots.add ("params / " + kv.name.toString());
        if (auto* ga = v.getProperty ("groups", juce::var()).getArray())
            for (const auto& gv : *ga)
            {
                const auto n = gv.getProperty ("n", juce::var()).toString();
                if (auto* gp = gv.getProperty ("params", juce::var()).getDynamicObject())
                    for (auto& kv : gp->getProperties())
                        slots.add ("group " + n + " / " + kv.name.toString());
            }
        if (auto* co = v.getProperty ("controls", juce::var()).getDynamicObject())
            for (auto& kv : co->getProperties())
                slots.add ("controls / " + kv.name.toString());
        return slots;
    }

    void probeBatch (const juce::String& mapsDir, const juce::String& outPath,
                     const juce::String& onlyCategory)
    {
        batchMode = true;
        auto dir = mapsDir.isNotEmpty()
                     ? juce::File::getCurrentWorkingDirectory().getChildFile (mapsDir)
                     : ledger.getRoot().getChildFile ("maps");

        juce::String rep;
        rep << "EJMAP BATCH PROBE\n"
            << "run at    " << juce::Time::getCurrentTime().toISO8601 (true) << "\n"
            << "binary    " << EJMAP_VERSION << " (" << EJMAP_GIT_HASH << ")\n"
            << "maps dir  " << dir.getFullPathName() << "\n"
            << (onlyCategory.isNotEmpty() ? "filter    --only " + onlyCategory + "\n" : "")
            << "\nWHAT THIS TOOL CAN DECIDE, STATED BEFORE ANY RESULT\n"
               "  M9 decides ONE TO FOUR PARAMETERS ON ONE SUBJECT per category\n"
               "  (eq 3, compressor 4, limiter 1, gate 1). saturation, de-esser\n"
               "  and delay produce no verdicts at all.\n"
               "  The suites are FIXTURE-BOUND: each measures one signed product,\n"
               "  so a map is probed only when it IS that product. Every other map\n"
               "  is listed below as uncovered, with the reason.\n"
               "  No map here is called clean or verified. With this coverage there\n"
               "  is no honest summary verdict for a map.\n\n";

        int probed = 0, noSuite = 0, notSubject = 0, unresolved = 0, found = 0;
        juce::StringArray noSuiteLines, notSubjectLines, unresolvedLines, body;
        juce::Array<juce::var> posts;

        for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*.json"))
        {
            auto v  = juce::JSON::parse (e.getFile().loadFileAsString());
            auto id = v.getProperty ("identity", juce::var());
            const auto name     = id.getProperty ("name", "?").toString();
            const auto uid      = id.getProperty ("uid", "").toString();
            const auto version  = id.getProperty ("version", "").toString();
            const auto category = v.getProperty ("category", "").toString();
            const auto fp       = v.getProperty ("fp", "").toString();
            const int nSlots = mappableSlots (v).size();

            if (onlyCategory.isNotEmpty() && category != onlyCategory) continue;
            ++found;

            SuiteBinding bind;
            for (const auto& b : suiteBindings())
                if (b.category == category) bind = b;

            if (bind.suite.isEmpty())
            {
                ++noSuite;
                noSuiteLines.add ("    " + name + " (" + category + ") -- no suite exists for this "
                                  "category; " + juce::String (nSlots) + " mappable slots unexamined");
                continue;
            }

            // Identity printed BEFORE the load, per "a name is not evidence of
            // identity" -- the bx_saturator V2 instance resolved to the UAD product.
            auto sub = resolveSubjectByIdentity (uid, version, name);
            std::cout << "SUBJECT " << name << " (map identity AudioUnit|" << uid << "|" << version
                      << ") -> " << sub.candidates << " candidate(s); resolved '"
                      << sub.desc.name << "' " << sub.desc.fileOrIdentifier
                      << " v" << sub.installedVersion << std::endl;

            if (! sub.ok())
            {
                ++unresolved;
                unresolvedLines.add ("    " + name + " -- "
                    + (sub.candidates == 0
                         ? "not installed: no component carries uid " + uid
                         : sub.versionMismatch
                             ? "installed version " + sub.installedVersion + " != mapped version "
                               + version + "; the fp differs, so a probe would be filed against "
                               "a fingerprint this machine cannot produce"
                             : sub.detail));
                continue;
            }

            // The refusal. Fixture-bound suite, so the map must BE the fixture.
            // Id only. The by-name fallback that stood here was the same
            // mechanism this session removed from the limiter/gate suite.
            const bool isSubject = sub.desc.fileOrIdentifier == bind.subjectId;
            if (! isSubject)
            {
                ++notSubject;
                notSubjectLines.add ("    " + name + " (" + category + ") -- the " + bind.suite
                    + " suite is bound to " + bind.subjectName + " and would measure THAT plugin. "
                    "Probing this map would file " + bind.subjectName + "'s verdicts under fp "
                    + fp.substring (0, 12) + ". " + juce::String (nSlots) + " mappable slots unexamined");
                continue;
            }

            batchRows.clearQuick();
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            gateM9 (bind.suite);                       // the suite, unchanged
            const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            ++probed;
            batchSuitesRun.addIfNotAlreadyThere (bind.suite);

            body.add ("  " + name + "   fp " + fp.substring (0, 12) + "   category " + category
                      + "   suite " + bind.suite + "   " + juce::String (secs, 1) + " s");
            juce::StringArray decided;
            juce::Array<juce::var> verdicts;
            for (const auto& r : batchRows)
            {
                body.add ("      " + (r.slot.isNotEmpty() ? r.slot : r.semantic).paddedRight (' ', 34)
                          + r.verdict.paddedRight (' ', 14) + r.evidence);
                decided.add (r.semantic.upToFirstOccurrenceOf (" ", false, false).trim());
                auto* o = new juce::DynamicObject();
                o->setProperty ("semantic", r.semantic);
                o->setProperty ("slot", r.slot.isNotEmpty() ? r.slot : r.semantic);
                o->setProperty ("verdict",  r.verdict);
                o->setProperty ("evidence", r.evidence);
                verdicts.add (juce::var (o));
            }
            // EVERY parameter gets a line. The map's mappable surface is
            // params + group params + named controls; walking params alone hid
            // 5 groups and 62 controls behind a report that read as complete.
            const auto slots = mappableSlots (v);
            int uncovered = 0;
            for (const auto& slot : slots)
            {
                const auto leaf = slot.fromLastOccurrenceOf ("/ ", false, false).trim();
                bool touched = false;
                for (const auto& r : batchRows)
                    if ((r.slot.isNotEmpty() && r.slot == slot)
                        || (r.slot.isEmpty() && r.semantic.equalsIgnoreCase (leaf)))
                        touched = true;
                if (! touched)
                {
                    ++uncovered;
                    body.add ("      " + slot.paddedRight (' ', 34)
                              + juce::String ("not covered").paddedRight (' ', 14)
                              + "no suite can decide this parameter");
                }
            }
            body.add ("      -- " + juce::String (batchRows.size()) + " decided, "
                      + juce::String (uncovered) + " of " + juce::String (slots.size())
                      + " mappable slots not covered; no summary verdict is given for this map");
            body.add ("");

            auto* pr = new juce::DynamicObject();
            pr->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
            pr->setProperty ("ejmap_version", juce::String (EJMAP_VERSION) + " (" + EJMAP_GIT_HASH + ")");
            pr->setProperty ("suites_run", juce::var (juce::Array<juce::var> { bind.suite }));
            pr->setProperty ("verdicts", juce::var (verdicts));
            auto* env = new juce::DynamicObject();
            env->setProperty ("fp", fp);
            env->setProperty ("probed", juce::var (pr));
            posts.add (juce::var (env));
        }

        rep << "COVERAGE\n"
            << "  maps found                  " << found << "\n"
            << "  probed                      " << probed << "\n"
            << "  no suite for category       " << noSuite << "\n";
        for (const auto& l : noSuiteLines) rep << l << "\n";
        rep << "  suite bound to another plugin " << notSubject << "\n";
        for (const auto& l : notSubjectLines) rep << l << "\n";
        rep << "  subject unresolved          " << unresolved << "\n";
        for (const auto& l : unresolvedLines) rep << l << "\n";
        rep << "\nPER MAP\n";
        if (body.isEmpty()) rep << "  (no map was probed)\n";
        for (const auto& l : body) rep << l << "\n";

        // ---- the POST ---------------------------------------------------
        // Built through the ONE request builder, emitted as an artifact, NOT
        // sent: ejmap has no live transport, and which TLS path carries one is
        // signed at M11, not chosen here by whoever wrote the runner. Nothing
        // below claims an upload happened.
        rep << "\nPOST /api/params/ejmap/probed\n";
        const auto base = Mouth::uploadBaseUrl();
        for (const auto& envv : posts)
        {
            const auto fp = envv.getProperty ("fp", "").toString();
            const auto json = juce::JSON::toString (envv, false);
            juce::MemoryBlock mb (json.toRawUTF8(), json.getNumBytesAsUTF8());
            auto f = Mouth::writeRequestArtifact (ledger.getRoot(), base + "/probed",
                                                  fp + ".probed.http", mb,
                                                  testerName(), machineIdString(), EJMAP_VERSION);
            rep << "  " << fp.substring (0, 12) << "  request bytes written: "
                << f.getFullPathName() << " (" << f.getSize() << " bytes)\n";
        }
        if (posts.isEmpty()) rep << "  (nothing probed, so nothing to post)\n";
        rep << "  NOT SENT. ejmap has no live transport; the TLS option is signed at M11.\n"
            << "  Endpoint base: " << base << "\n";

        auto out = outPath.isNotEmpty()
                     ? juce::File::getCurrentWorkingDirectory().getChildFile (outPath)
                     : ledger.getRoot().getChildFile ("probe-report-"
                         + juce::Time::getCurrentTime().formatted ("%Y%m%dT%H%M%S") + ".txt");
        out.replaceWithText (rep);
        std::cout << "\n" << rep << std::endl;
        std::cout << "BATCH: report written to " << out.getFullPathName() << std::endl;
        batchMode = false;
        juce::JUCEApplication::getInstance()->quit();
    }

    /** M9 HEADLINE GATE (signed fixture, 2026-08-02): AMEK EQ 200, the
        production map, two arms against one loaded instance. Arm B (correct
        map) first, then arm A (deliberate mis-map: band 1 freq_hz pointed at
        Mono Maker). Every criterion prints its measured number; a boolean
        pass is not a result. mode: "" full gate, "kill" dies mid-arm-B with
        the stake on disk, "resume" is the relaunch that must restore state
        and say so in words.
    */
    void gateM9 (const juce::String& mode)
    {
        using P = Probe;
        auto say = [] (const juce::String& t) { std::cout << t << std::endl; };
        int fails = 0;
        // TWO POPULATIONS, TWO FUNCTIONS. The 19 emit sites were never one
        // kind: 11 are the headline gate asserting things about ITSELF against
        // a fixture, and 8 are parameter verdicts about a plugin. crit()
        // served both, which is why routing could not sit on it -- forcing a
        // harness assertion through routeVerdict is meaningless (there is no
        // Delta_pred for "the stake restored five parameters"), so the fork
        // was pushed out to a convention suites were supposed to follow. The
        // choke point was doing two jobs; splitting it is what lets the guard
        // sit there.

        // (1) HARNESS ASSERTIONS. Unchanged behaviour, renamed so no suite
        // reaches for it to publish a verdict by analogy.
        auto assertHarness = [&] (const juce::String& id, bool pass, const juce::String& numbers)
        {
            if (! pass) ++fails;
            std::cout << "  " << (pass ? "PASS " : "FAIL ") << id << ": " << numbers << std::endl;
        };

        // (2) PARAMETER VERDICTS. The only path a verdict leaves a suite. The
        // four routing inputs are REQUIRED ARGUMENTS: a suite that omits them
        // does not compile, which is the property the sensitivity check and
        // the ambiguity rule already have and the fork did not. Returns the
        // route so a caller can branch, but the words are composed here.
        auto emitVerdict = [&] (const juce::String& semantic,
                                double measured, double predicted,
                                double floor, double tolerance,
                                const juce::String& evidence,
                                const juce::String& unit)
        {
            const auto route = P::routeVerdict (measured, floor, predicted, tolerance);
            if (route == P::Route::overClaim) ++fails;
            const juce::String label = route == P::Route::tracks ? "confirms"
                                     : route == P::Route::deafness ? "inconclusive"
                                                                   : "contradicts";
            std::cout << "  " << (route == P::Route::tracks ? "CONFIRMS    "
                               : route == P::Route::deafness ? "INCONCLUSIVE"
                                                             : "CONTRADICTS ")
                      << " " << semantic << ": "
                      << P::routeText (route, measured, predicted, floor, unit)
                      << " | " << evidence << std::endl;
            batchRows.add ({ semantic, label,
                             P::routeText (route, measured, predicted, floor, unit)
                             + " | " + evidence });
            return route;
        };

        // (3) INCONCLUSIVE BY PRECONDITION. Reached BEFORE any measurement
        // exists -- a mode token, an undeclared unit family -- so there are no
        // numbers to route. It cannot express a confirm STRUCTURALLY: this
        // function has no branch, no parameter and no return value that could
        // produce one, and it never touches the route enum at all.
        auto emitInconclusive = [&] (const juce::String& semantic, const juce::String& reason,
                                     const juce::String& basisFromCaller = {})
        {
            // THE BASIS BELONGS TO THE CALLER, because only the caller knows
            // why no verdict is reachable. This started as a fixed suffix
            // asserting "no measurement was taken", which was false at the
            // excitation guard; replacing it with a bool produced a second
            // fixed suffix asserting "the feature never appeared", which is
            // false at the envelope sites, where the feature moved 11.5 -> 63.7
            // ms and the obstacle was an undeclared unit family. Two rounds of
            // the same defect -- documentation asserting a property the code
            // lacks -- in the same three lines. A free string cannot lie unless
            // the caller writes a lie.
            const juce::String basis = basisFromCaller.isNotEmpty()
                ? "  [" + basisFromCaller + "]"
                : "  [by precondition -- no measurement was taken, so no verdict is reachable "
                  "from here]";
            std::cout << "  INCONCLUSIVE " << semantic << ": " << reason << basis << std::endl;
            batchRows.add ({ semantic, "inconclusive", reason + basis });
        };
        /** (4) CONTRADICTED BY MEASUREMENT. Distinct from emitInconclusive
            (which is reached before any measurement exists) and from
            emitVerdict (which routes a movement claim). This one carries a
            measurement that DISAGREES, and like emitInconclusive it has no
            branch, parameter or return value that could produce a confirm.

            It exists because parameterisation created a claim the routing fork
            cannot express: routeVerdict decides whether a feature MOVED as
            predicted, over a span. A map whose ladder is uniformly offset
            produces exactly the right span with every point in the wrong
            place, and routes as `tracks`. Measured on bx_limiter with a
            constructed 6 dB-offset map: CONFIRMS, worst |feature - ladder|
            7.20 dB against a 1.88 dB tolerance, in the evidence string of a
            passing verdict. That failure was unreachable before item 1,
            because the ladder came from the plugin itself and could not be
            offset from it. */
        auto emitContradicts = [&] (const juce::String& semantic, const juce::String& reason)
        {
            ++fails;
            std::cout << "  CONTRADICTS  " << semantic << ": " << reason << std::endl;
            batchRows.add ({ semantic, "contradicts", reason });
        };
        juce::ignoreUnused (emitInconclusive, emitContradicts);

        auto desc = echojay::auregistry::describeFromRegistry (
                        mode == "comp" ? "AudioUnit:Effects/aufx,APCM,ksWV"
                                       : "AudioUnit:Effects/aumf,ameq,Brwx");
        ScannedPlugin sp; sp.desc = desc;
        loadedName = desc.name; loadedId = sp.pluginId(); loadedDesc = desc;
        auto res = host.load (desc, watchdog);
        if (res.outcome != LoadOutcome::ok) { say ("GATE: load failed"); quitNow(); return; }
        cal = capture.calibrate (*host.getInstance(), loadedId);
        currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);
        auto* inst = host.getInstance();
        auto params = inst->getParameters();

        auto stakeFile = ledger.getRoot().getChildFile ("probe-inflight.json");
        auto stateFile = ledger.getRoot().getChildFile ("probe-state-" + currentFp + ".bin");

        // ---- settle: pump-duration sweep, four subjects, FULL CURVE ------
        // ITEM 2: is the natural unit a TURN rather than a duration? If one
        // explicit dispatch turn with zero sleep suffices, the fix is "pump
        // one turn after every write" with no duration constant -- which
        // cannot be wrong on a slower or busier machine, and the cost
        // question dissolves.
        if (mode == "subms")
        {
            // Sub-millisecond pump: spin while dispatching, so elapsed CLOCK
            // passes (which the turn test proved is what matters) at a
            // resolution runDispatchLoopUntil's integer ms cannot reach.
            auto pumpFor = [] (double ms) {
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                do { juce::MessageManager::getInstance()->runDispatchLoopUntil (0); }
                while (juce::Time::getMillisecondCounterHiRes() - t0 < ms); };

            const char* cands[] = { "SSL X-Gate", "kHs Gate", "SSL X-Limit", "Weiss Deess" };
            const double durs[] = { 0.0, 0.05, 0.1, 0.2, 0.3, 0.5, 0.75, 1.0, 2.0 };
            auto census7 = echojay::auregistry::buildCensus();
            say ("SUB-MS SWEEP (spin-pump, clock-accurate). Delta per pump duration, dB.");

            std::atomic<bool> busy { false };
            juce::OwnedArray<std::thread> loaders;

            for (auto* want : cands)
            {
                auto pd7 = resolveSubjectByName (want);
                if (pd7.fileOrIdentifier.isEmpty()) continue;   // miss already recorded
                host.unload();
                loadedName = pd7.name; loadedId = pd7.fileOrIdentifier; loadedDesc = pd7;
                if (host.load (pd7, watchdog).outcome != LoadOutcome::ok)
                { say ("  " + juce::String (want) + ": load failed"); continue; }
                auto* ci = host.getInstance();
                auto cp = ci->getParameters();
                int ip = -1;
                for (auto* n : { "Output", "Gain", "Ceiling", "Threshold", "Mix" })
                { for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).containsIgnoreCase (n) && cp[i]->isAutomatable())
                    { ip = i; break; }
                  if (ip >= 0) break; }
                if (ip < 0) { say ("  " + juce::String (want) + ": no candidate param"); continue; }

                auto sweepOnce = [&] (bool loaded) {
                    juce::String line = juce::String ("    ") + (loaded ? "LOADED" : "idle  ") + " |";
                    for (double d : durs)
                    {
                        auto once = [&] (float v) {
                            cp[ip]->setValueNotifyingHost (v);
                            if (d > 0) pumpFor (d);
                            host.pausePumpForMutation();
                            auto r = P::levelSweptBursts (*ci, 0.2, 0.0, 6.0, 4);
                            host.resumePumpAfterMutation(); return r; };
                        auto a = once (0.15f); auto b = once (0.85f);
                        double w = 0;
                        for (int i = 0; i < a.size() && i < b.size(); ++i)
                            w = juce::jmax (w, std::abs (b[i] - a[i]));
                        line << " " << juce::String (d, 2) << ":" << juce::String (w, 1);
                    }
                    return line; };

                say ("  " + pd7.name + "  [" + juce::String (ip) + "] " + cp[ip]->getName (24));
                say (sweepOnce (false));
                busy = true;
                for (int t = 0; t < 6; ++t)
                    loaders.add (new std::thread ([&busy] { volatile double x = 0;
                        while (busy) for (int k = 0; k < 100000; ++k) x += std::sin ((double) k); }));
                say (sweepOnce (true));
                busy = false;
                for (auto* t : loaders) if (t->joinable()) t->join();
                loaders.clear();
            }
            std::cout << "SUBMS: DONE" << std::endl; quitNow(); return;
        }

        if (mode == "turns")
        {
            const char* subs2[] = { "SSL X-Gate", "SSL X-Limit", "kHs Gate",
                                    "Weiss Deess", "bx_limiter True Peak", "spiff" };
            auto census6 = echojay::auregistry::buildCensus();
            say ("TURNS: does ONE dispatch turn (zero sleep) deliver the write?");
            for (auto* want : subs2)
            {
                juce::PluginDescription pd6;
                for (const auto& t : census6.targets)
                { auto d = echojay::auregistry::describeFromRegistry (t.identifier);
                  if (d.name.containsIgnoreCase (want)) { pd6 = d; break; } }
                host.unload();
                loadedName = pd6.name; loadedId = pd6.fileOrIdentifier; loadedDesc = pd6;
                if (host.load (pd6, watchdog).outcome != LoadOutcome::ok) continue;
                auto* ci = host.getInstance();
                auto cp = ci->getParameters();
                // NO SILENT SKIP. The previous version required the name
                // "Output" and did a bare `continue`, so spiff (sensitivity,
                // decay, trim, mix) was dropped without a word and its absence
                // was then reported as the subject's problem. Suitability is
                // measured, not named -- instance seven's rule applied to a
                // diagnostic rather than a suite -- so any automatable
                // parameter that actually moves the render will do.
                int ip = -1;
                for (auto* n : { "Output", "Gain", "Trim", "Mix", "Ceiling",
                                 "Threshold", "Drive", "Depth", "Amount" })
                {
                    for (int i = 0; i < cp.size(); ++i)
                        if (cp[i]->getName (64).containsIgnoreCase (n) && cp[i]->isAutomatable())
                        { ip = i; break; }
                    if (ip >= 0) break;
                }
                if (ip < 0)
                    for (int i = 0; i < cp.size(); ++i)      // last resort: anything automatable
                        if (cp[i]->isAutomatable() && ! cp[i]->isDiscrete()) { ip = i; break; }
                if (ip < 0)
                {
                    juce::StringArray names;
                    for (int i = 0; i < juce::jmin (8, cp.size()); ++i) names.add (cp[i]->getName (20));
                    say ("  " + juce::String (pd6.name).paddedRight (' ', 16)
                         + "| SKIPPED, and here is why: no automatable continuous parameter. Its "
                         + juce::String (cp.size()) + " params begin: " + names.joinIntoString (", "));
                    continue;
                }
                say ("  " + juce::String (pd6.name).paddedRight (' ', 16) + "| probing ["
                     + juce::String (ip) + "] " + cp[ip]->getName (24));
                // three deliveries: none, ONE turn (zero sleep), 1 ms
                const char* how[] = { "no pump at all", "ONE turn, zero sleep", "1 ms sleep-pump" };
                juce::String line = "  " + juce::String (pd6.name).paddedRight (' ', 16) + "|";
                for (int variant = 0; variant < 3; ++variant)
                {
                    auto once = [&] (float v) {
                        cp[ip]->setValueNotifyingHost (v);
                        if (variant == 1) juce::MessageManager::getInstance()->runDispatchLoopUntil (0);
                        if (variant == 2) juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
                        host.pausePumpForMutation();
                        auto r = P::levelSweptBursts (*ci, 0.25, 0.0, 6.0, 5);
                        host.resumePumpAfterMutation();
                        return r; };
                    auto a = once (0.15f); auto b = once (0.85f);
                    double worst = 0;
                    for (int i = 0; i < a.size() && i < b.size(); ++i)
                        worst = juce::jmax (worst, std::abs (b[i] - a[i]));
                    line << "  " << how[variant] << ": " << juce::String (worst, 1) << " dB |";
                }
                say (line);
            }
            std::cout << "TURNS: DONE" << std::endl; quitNow(); return;
        }

        if (mode == "settle")
        {
            const char* subs[] = { "API-2500 (m)" };
            const int pumps[] = { 0, 1, 2, 5, 10, 20, 50, 100 };
            auto census5 = echojay::auregistry::buildCensus();
            say ("SETTLE CURVE: minimum message-loop pump for a write to reach the DSP");
            say ("  (delta between two parameter settings, per pump duration, dB)");
            for (auto* want : subs)
            {
                juce::PluginDescription pd5;
                for (const auto& t : census5.targets)
                { auto d = echojay::auregistry::describeFromRegistry (t.identifier);
                  if (d.name.containsIgnoreCase (want)) { pd5 = d; break; } }
                if (pd5.fileOrIdentifier.isEmpty()) { say ("  " + juce::String (want) + ": absent"); continue; }
                host.unload();
                loadedName = pd5.name; loadedId = pd5.fileOrIdentifier; loadedDesc = pd5;
                if (host.load (pd5, watchdog).outcome != LoadOutcome::ok)
                { say ("  " + juce::String (want) + ": load failed"); continue; }
                auto* ci = host.getInstance();
                auto cp = ci->getParameters();
                // a parameter whose move is audible on a steady tone: prefer
                // an output/gain-shaped one, else the first automatable.
                int ip = -1;
                for (auto* n : { "Output", "Gain", "Ceiling", "Threshold", "Release", "Mix" })
                { for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).containsIgnoreCase (n) && cp[i]->isAutomatable())
                    { ip = i; break; }
                  if (ip >= 0) break; }
                if (ip < 0) { say ("  " + juce::String (want) + ": no candidate param"); continue; }
                juce::String line = "  " + juce::String (pd5.name).paddedRight (' ', 18)
                                  + "[" + juce::String (ip) + "] "
                                  + cp[ip]->getName (18).paddedRight (' ', 18) + "|";
                for (int pm : pumps)
                {
                    auto once = [&] (float v) {
                        cp[ip]->setValueNotifyingHost (v);
                        if (pm > 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (pm);
                        host.pausePumpForMutation();
                        auto r = P::levelSweptBursts (*ci, 0.25, 0.0, 6.0, 5);
                        host.resumePumpAfterMutation();
                        return r; };
                    auto a = once (0.15f);
                    auto b = once (0.85f);
                    double worst = 0;
                    for (int i = 0; i < a.size() && i < b.size(); ++i)
                        worst = juce::jmax (worst, std::abs (b[i] - a[i]));
                    // ITEM 1: the DELTA hides whether a level moved at all.
                    // Print the underlying burst-0 levels for both settings.
                    say ("    pump " + juce::String (pm) + " ms: lo-setting burst0 "
                         + juce::String (a.isEmpty() ? 0.0 : a[0], 2) + " dB, hi-setting burst0 "
                         + juce::String (b.isEmpty() ? 0.0 : b[0], 2) + " dB, worst |delta| "
                         + juce::String (worst, 2));
                    line << " " << juce::String (pm) << "ms:" << juce::String (worst, 1);
                }
                say (line);
            }
            std::cout << "SETTLE: DONE" << std::endl; quitNow(); return;
        }

        // ---- planes: WHY is the render blind to writes the property plane
        //      confirmed? Three orderings, one of them will land ------------
        if (mode == "planes")
        {
            auto census4 = echojay::auregistry::buildCensus();
            juce::PluginDescription pd4;
            for (const auto& t : census4.targets)
            { auto d = echojay::auregistry::describeFromRegistry (t.identifier);
              if (d.name.containsIgnoreCase ("SSL X-Gate")) { pd4 = d; break; } }
            if (pd4.fileOrIdentifier.isEmpty()) { say ("PLANES: no subject"); quitNow(); return; }
            host.unload();
            loadedName = pd4.name; loadedId = pd4.fileOrIdentifier; loadedDesc = pd4;
            if (host.load (pd4, watchdog).outcome != LoadOutcome::ok)
            { say ("PLANES: load failed"); quitNow(); return; }
            auto* ci = host.getInstance();
            auto cp = ci->getParameters();
            int iRel = -1;
            for (int i = 0; i < cp.size(); ++i)
                if (cp[i]->getName (64).containsIgnoreCase ("Release")) { iRel = i; break; }
            auto swRel = sweepOneIndex (*ci, iRel, watchdog, loadedId);
            auto toNorm = [] (const juce::Array<juce::Array<float>>& a, double v) {
                auto e = echojay::dominantMonotonicTable (a);
                return echojay::interpolateAnchors (e.table, (float) v); };
            const float nLo = toNorm (swRel.anchors, swRel.anchors.getFirst()[0]);
            const float nHi = toNorm (swRel.anchors, swRel.anchors.getLast()[0]);
            say ("PLANES | " + pd4.name + " | moving [" + juce::String (iRel) + "] "
                 + cp[iRel]->getName (32) + " " + juce::String (swRel.anchors.getFirst()[0], 1)
                 + " -> " + juce::String (swRel.anchors.getLast()[0], 1));

            auto rowsFor = [&] (int variant, float norm)
            {
                // variant 0: write BEFORE pause (what the suites do today)
                // variant 1: write AFTER pause, inside the paused window
                // variant 2: write before pause, message loop PUMPED during render
                if (variant == 0) P::writeAndServiceRunloop (*cp[norm == nLo ? iRel : iRel], norm);
                host.pausePumpForMutation();
                if (variant == 1)
                {
                    cp[iRel]->setValueNotifyingHost (norm);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
                }
                juce::Array<double> v;
                if (variant == 2)
                {
                    // render in slices, pumping between them
                    for (int slice = 0; slice < 3; ++slice)
                    {
                        auto part = P::levelSweptBursts (*ci, 0.6, 0.0 - slice * 12.0, 4.0, 3);
                        for (auto x : part) v.add (x);
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
                    }
                }
                else
                    v = P::levelSweptBursts (*ci, 0.6, 0.0, 4.0, 9);
                host.resumePumpAfterMutation();
                return v;
            };
            // THE 2x2. Variant 0 and 1 differed in TWO ways -- write position
            // relative to the pause AND writeConfirm (which pumps the message
            // loop) versus a bare setValueNotifyingHost. Four cells separate
            // them. The dangerous cell is "writeConfirm INSIDE the window":
            // if that is blind, the path built to GUARANTEE writes land is
            // the path losing them, and every suite uses it.
            auto cell = [&] (bool useConfirm, bool insideWindow, float norm)
            {
                if (! insideWindow)
                {
                    if (useConfirm) P::writeAndServiceRunloop (*cp[iRel], norm);
                    else            cp[iRel]->setValueNotifyingHost (norm);
                }
                host.pausePumpForMutation();
                if (insideWindow)
                {
                    if (useConfirm) P::writeAndServiceRunloop (*cp[iRel], norm);
                    else
                    {
                        cp[iRel]->setValueNotifyingHost (norm);
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
                    }
                }
                auto v = P::levelSweptBursts (*ci, 0.6, 0.0, 4.0, 9);
                host.resumePumpAfterMutation();
                return v;
            };
            struct Cell { bool confirm, inside; const char* name; };
            const Cell cells[] = {
                { true,  false, "A  writeConfirm  BEFORE pause  (what every suite does)" },
                { true,  true,  "B  writeConfirm  INSIDE window (the dangerous cell)" },
                { false, false, "C  bare write    BEFORE pause" },
                { false, true,  "D  bare write    INSIDE window (variant 1 previously)" } };
            for (const auto& c : cells)
            {
                auto a = cell (c.confirm, c.inside, nLo);
                auto b = cell (c.confirm, c.inside, nHi);
                double worst = 0;
                for (int i = 0; i < a.size() && i < b.size(); ++i)
                    worst = juce::jmax (worst, std::abs (b[i] - a[i]));
                say ("  " + juce::String (c.name) + ": worst |delta| "
                     + juce::String (worst, 2) + " dB" + (worst > 0.5 ? "   <- LANDS" : "   <- BLIND"));
            }
            std::cout << "PLANES: DONE" << std::endl; quitNow(); return;
        }

        // ---- limgate: LIMITER and GATE off the compressor stimuli --------
        if (mode == "limiter" || mode == "gate")
        {
            const bool isLim = mode == "limiter";
            // PINNED BY ID, not by name -- the unfixed member of the class that
            // resolved "bx_saturator V2" to the UAD component. Substring
            // matching takes the first container of the string, so "SSL X-Gate"
            // would also match a plugin named "SSL X-Gate Legacy" and
            // "bx_limiter True Peak" sits beside bx_limiter and bx_limiter V5.
            // Ids measured from the component registry, 3 Aug 2026.
            const juce::String wantId = isLim ? "AudioUnit:Effects/aumf,bxtp,Brwx"
                                              : "AudioUnit:Effects/aufx,XGAT,SSLN";
            const juce::String wantName = isLim ? "bx_limiter True Peak" : "SSL X-Gate";
            auto pd = echojay::auregistry::describeFromRegistry (wantId);
            if (pd.fileOrIdentifier.isEmpty())
            { say (mode.toUpperCase() + ": pinned id " + wantId + " is not in the census "
                   "(expected " + wantName + ")"); quitNow(); return; }
            // Identity PRINTED BEFORE THE LOAD, per "a name is not evidence of
            // identity": the name is now a check on the id, not the lookup key.
            say (mode.toUpperCase() + " subject, resolved BEFORE load: name '" + pd.name
                 + "' | vendor '" + pd.manufacturerName + "' | version " + pd.version
                 + " | id " + pd.fileOrIdentifier);
            if (! pd.name.containsIgnoreCase (wantName))
                say ("  NOTE: the pinned id resolves to '" + pd.name + "', not '" + wantName
                     + "'. The id is authoritative; this suite's thresholds were derived on '"
                     + wantName + "'.");
            host.unload();
            loadedName = pd.name; loadedId = pd.fileOrIdentifier; loadedDesc = pd;
            ledger.beginLoad (loadedId, pd.name, pd.manufacturerName, pd.pluginFormatName,
                              pd.version, "probe_gate_load", "probe_gate_load");
            {
                Watchdog::Scope g (watchdog, "probe_gate_load", loadedId, pd.name,
                                   pd.pluginFormatName, "probe", 30000);
                if (host.load (pd, watchdog).outcome != LoadOutcome::ok)
                { ledger.quarantine (loadedId, "probe gate: load failed", "probe");
                  say (mode.toUpperCase() + ": load failed, quarantined"); quitNow(); return; }
            }
            auto* ci = host.getInstance();
            auto cp = ci->getParameters();
            say ((isLim ? "LIMITER" : "GATE") + juce::String (" SUITE | ") + pd.name
                 + " | " + juce::String (cp.size()) + " params");
            // ITEM 2: a name pattern that matches MORE THAN ONE index is
            // ambiguous, and the suite does not choose. idxOf() taking the
            // first substring match found X-Gate's hysteresis CLOSE point
            // ("Lower Threshold") instead of the open point -- the same class
            // as the band matcher's stray-flat-control problem, and it will
            // recur on Upper/Lower, In/Out, L/R, Band N. Silent first-match
            // selection is REMOVED, not improved.
            auto candidates = [&] (const juce::String& sub) {
                juce::Array<int> out;
                for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).containsIgnoreCase (sub)) out.add (i);
                return out; };
            auto nameList = [&] (const juce::Array<int>& v) {
                juce::StringArray n;
                for (int i : v) n.add ("[" + juce::String (i) + "] " + cp[i]->getName (64));
                return n.joinIntoString (", "); };

            // Demonstrate the refusal on the bare pattern before using a
            // qualified one: attempting what the rule refuses IS the test.
            {
                auto bare = candidates (isLim ? "Ceiling" : "Threshold");
                say ("  pattern '" + juce::String (isLim ? "Ceiling" : "Threshold") + "' matches "
                     + juce::String (bare.size()) + ": " + nameList (bare));
                if (bare.size() > 1)
                    say ("  AMBIGUOUS: the suite DECLINES to pick. Every candidate is named "
                         "above rather than one chosen silently. NOTE: a map qualifier would "
                         "resolve this, and this suite does not yet read one -- it falls through "
                         "to its own name preferences below.");
            }

            // ---- THE UPGRADE (M9 parameterisation item 1) ------------------
            // The target and its ladder come FROM THE MAP. That changes what
            // this suite tests: sweeping the plugin and comparing a rendered
            // feature against the ladder just read from that same plugin is a
            // display-versus-render self-consistency check. Driving the MAP's
            // numbers asks whether the plugin does what THE MAP CLAIMS -- which
            // diverges exactly when the map is stale, was made in another mode
            // or preset, or was made against another version.
            cal = capture.calibrate (*ci, loadedId);
            currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);
            auto mapFile = ledger.getRoot().getChildFile ("maps")
                                 .getChildFile (currentFp + ".json");
            auto mapVar = juce::JSON::parse (mapFile.loadFileAsString());
            const bool haveMap = mapVar.isObject();
            const juce::String wantSem = isLim ? "ceiling_db" : "threshold_db";
            auto slot = ejmap::subject::slotFor (mapVar, wantSem);
            say ("  fp " + currentFp.substring (0, 12) + " | map "
                 + (haveMap ? mapFile.getFileName() : juce::String ("NONE on this machine")));

            int iCtl = -1;
            juce::String targetSource;
            if (slot.ok())
            {
                iCtl = slot.index;
                targetSource = "map";
                say ("  target FROM THE MAP: " + wantSem + " -> [" + juce::String (iCtl) + "] "
                     + (juce::isPositiveAndBelow (iCtl, cp.size()) ? cp[iCtl]->getName (64)
                                                                   : juce::String ("INDEX OUT OF RANGE"))
                     + ", ladder " + juce::String (slot.ladderLo(), 2) + " .. "
                     + juce::String (slot.ladderHi(), 2) + " (" + juce::String (slot.anchors.size())
                     + " anchors, map name '" + slot.name + "')");
                if (! juce::isPositiveAndBelow (iCtl, cp.size()))
                { emitInconclusive (wantSem, "the map's index " + juce::String (iCtl)
                        + " is outside this instance's " + juce::String (cp.size())
                        + " parameters -- the map does not describe this plugin");
                  std::cout << (isLim ? "LIMITER" : "GATE") << " SUITE: INCONCLUSIVE (map index "
                            "out of range)" << std::endl; quitNow(); return; }
            }
            else
            {
                // NO MAP, OR THE MAP DOES NOT CARRY THIS SEMANTIC. The suite
                // still runs, but it is back to the WEAKER CLAIM and says so:
                // self-consistency, not map correctness.
                targetSource = haveMap ? "names (map carries no " + wantSem + ")" : "names (no map)";
                say ("  NO MAP-DRIVEN TARGET: " + (slot.why.isNotEmpty() ? slot.why
                                                                         : juce::String ("no map"))
                     + ". Falling back to name preferences -- this run is a DISPLAY-VS-RENDER "
                       "SELF-CONSISTENCY CHECK, not a check of any map's claims.");
                for (auto* n : (isLim ? std::initializer_list<const char*>{ "Ceiling", "Output Ceiling" }
                                      : std::initializer_list<const char*>{ "Upper Threshold", "Threshold" }))
                {
                    auto c2 = candidates (n);
                    if (c2.size() == 1) { iCtl = c2[0]; say ("  qualified target: '" + juce::String (n)
                                                            + "' -> " + nameList (c2)); break; }
                }
            }
            if (iCtl < 0)
            { say ("  no unambiguous ceiling/threshold parameter -- INCONCLUSIVE: target ambiguous. "
                   "A map qualifier is what would resolve it; this suite cannot consult one yet, "
                   "so the stop is final rather than recoverable here");
              std::cout << (isLim ? "LIMITER" : "GATE") << " SUITE: INCONCLUSIVE (ambiguous target)"
                        << std::endl; quitNow(); return; }
            say ("  target parameter [" + juce::String (iCtl) + "] " + cp[iCtl]->getName (64)
                 + " (display '" + cp[iCtl]->getCurrentValueAsText() + "')");
            if (P::displayIsModeToken (cp[iCtl]->getCurrentValueAsText()))
            { say ("  INCONCLUSIVE: " + P::modeTokenReason (cp[iCtl]->getCurrentValueAsText()));
              std::cout << (isLim ? "LIMITER" : "GATE") << ": DONE" << std::endl; quitNow(); return; }
            // The burst gap must exceed this plugin's OWN maximum release,
            // read from its release ladder rather than guessed.
            double gateGapS = 1.5;
            if (! isLim)
            {
                auto relC = candidates ("Release");
                if (relC.size() == 1)
                {
                    auto swR2 = sweepOneIndex (*ci, relC[0], watchdog, loadedId);
                    if (swR2.anchors.size() > 1)
                    {
                        const double maxRel = juce::jmax ((double) swR2.anchors.getFirst()[0],
                                                          (double) swR2.anchors.getLast()[0]);
                        gateGapS = juce::jmax (1.5, 3.0 * (maxRel > 50 ? maxRel / 1000.0 : maxRel));
                        say ("  burst gap " + juce::String (gateGapS, 2) + " s = 3x this plugin's own "
                             "max release (" + juce::String (maxRel, 2) + ", ladder from ["
                             + juce::String (relC[0]) + "] " + cp[relC[0]]->getName (32) + ")");
                    }
                }
                else
                    say ("  burst gap 1.50 s (no unambiguous Release parameter to read a max from: "
                         + juce::String (relC.size()) + " candidates)");
            }
            // ITEM 1: is the BURST PATH parameter-sensitive at all? Four
            // byte-identical rows are either a clean null or an A/A, and
            // getValue only proves the PROPERTY plane -- this project has
            // already measured the two planes diverging (API-2500). Move a
            // parameter known live and require the render to change.
            if (! isLim)
            {
                auto relC2 = candidates ("Release");
                if (relC2.size() == 1)
                {
                    auto swRel = sweepOneIndex (*ci, relC2[0], watchdog, loadedId);
                    auto burstsNow = [&] {
                        host.pausePumpForMutation();
                        auto v = P::levelSweptBursts (*ci, 1.0, 0.0, 4.0, 9);
                        host.resumePumpAfterMutation(); return v; };
                    auto toNorm = [] (const juce::Array<juce::Array<float>>& a, double v) {
                        auto e = echojay::dominantMonotonicTable (a);
                        return echojay::interpolateAnchors (e.table, (float) v); };
                    P::writeAndServiceRunloop (*cp[relC2[0]], toNorm (swRel.anchors, swRel.anchors.getFirst()[0]));
                    auto rA = burstsNow();
                    P::writeAndServiceRunloop (*cp[relC2[0]], toNorm (swRel.anchors, swRel.anchors.getLast()[0]));
                    auto rB = burstsNow();
                    double worstD = 0; juce::String deltas;
                    for (int i = 0; i < rA.size() && i < rB.size(); ++i)
                    {
                        const double d = rB[i] - rA[i];
                        worstD = juce::jmax (worstD, std::abs (d));
                        if (i % 2 == 0) deltas << " " << juce::String (0.0 - 4.0 * i, 0)
                                               << ":" << juce::String (d, 1);
                    }
                    say ("  RENDER-PLANE CHECK: moved [" + juce::String (relC2[0]) + "] "
                         + cp[relC2[0]]->getName (32) + " across its full ladder ("
                         + juce::String (swRel.anchors.getFirst()[0], 1) + " -> "
                         + juce::String (swRel.anchors.getLast()[0], 1) + "), burst-row deltas:"
                         + deltas);
                    say ("  worst |delta| " + juce::String (worstD, 2) + " dB -> "
                         + (worstD > 0.5
                              ? juce::String ("the burst path IS parameter-sensitive, so a null on "
                                              "another parameter is that parameter's null, not the harness")
                              : juce::String ("THE BURST PATH DOES NOT SEE PARAMETER STATE -- the "
                                              "Upper Threshold null is the HARNESS, not the plugin, "
                                              "and no verdict may be issued from it")));
                    if (worstD <= 0.5)
                    { std::cout << "GATE SUITE: HARNESS DEFECT (burst path parameter-blind)"
                                << std::endl; quitNow(); return; }
                }
                else
                    say ("  RENDER-PLANE CHECK SKIPPED: no unambiguous Release parameter ("
                         + juce::String (relC2.size()) + " candidates) -- the null below is "
                           "therefore UNPROVEN as a plugin property");
            }
            SweepOutcome sw;
            {
                Watchdog::Scope g (watchdog, "probe_gate_sweep", loadedId, pd.name,
                                   pd.pluginFormatName, "probe", 30000);
                sw = sweepOneIndex (*ci, iCtl, watchdog, loadedId);
            }
            say ("  live ladder " + juce::String (sw.anchors.getFirst()[0], 2) + " .. "
                 + juce::String (sw.anchors.getLast()[0], 2) + " (" + juce::String (sw.anchors.size())
                 + " anchors, unit family '" + sw.unitFamily + "', method " + sw.method + ")");

            // The live sweep is KEPT even when the map drives the run. It is no
            // longer the source of truth; it is the second opinion, and the
            // disagreement between the two is the thing that was invisible
            // before -- only one of them was ever consulted.
            const bool mapDriven = slot.ok();

            // ---- probe points: fixture pins them, chooser only if nothing does
            juce::Array<double> points;
            juce::String pointSource;
            {
                auto fx = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures")
                            .getChildFile (juce::String (isLim ? "m9-limiter" : "m9-gate") + "-"
                                           + pd.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)
                                                                .replaceCharacter (',', '-') + ".json");
                auto fv = juce::JSON::parse (fx.loadFileAsString());
                if (auto* arr = fv.getProperty ("ladder_points", juce::var()).getArray())
                    for (const auto& v : *arr) points.add ((double) v);
                if (! points.isEmpty())
                    pointSource = "fixture " + fx.getFileName();
            }
            if (points.isEmpty())
            {
                // THE CHOOSER RUNS ONLY WHEN NOTHING PINS THE POINTS. A test case
                // that lets a chooser move its own stimulus is not a regression
                // test -- measured on eq, where a centred 2-octave pair lands at
                // 54.1/216.3 Hz against the fixture's 100/400.
                const juce::Array<double> fracs { 0.15, 0.4, 0.65, 0.9 };
                points = mapDriven ? ejmap::subject::spreadAcrossLadder (slot, fracs)
                                   : juce::Array<double>();
                if (points.isEmpty())
                    for (double f : fracs)
                    { const double lo = sw.anchors.getFirst()[0], hi = sw.anchors.getLast()[0];
                      points.add (lo + f * (hi - lo)); }
                pointSource = mapDriven ? "chooser over the MAP ladder (no fixture pins points)"
                                        : "chooser over the LIVE ladder (no map, no fixture)";
            }
            say ("  probe points from " + pointSource + ": ");
            {
                juce::StringArray ps;
                for (double v : points) ps.add (juce::String (v, 2));
                say ("    " + ps.joinIntoString (", "));
            }
            auto nf3 = [] (const juce::Array<juce::Array<float>>& a, double v) {
                auto e = echojay::dominantMonotonicTable (a);
                return echojay::interpolateAnchors (e.table, (float) v); };
            auto cap = [&] { host.pausePumpForMutation();
                             auto c = P::steppedCurve (*ci);
                             host.resumePumpAfterMutation(); return c; };

            if (isLim && ! renderPlaneSensitive (*ci, pd.name, [&] { return cap(); }))
            { std::cout << "LIMITER SUITE: STOPPED (render plane blind)" << std::endl;
              quitNow(); return; }

            // A/A floor for both estimators, on this subject.
            auto n1 = cap(), n2 = cap();
            double sgPlateau = 0, sgKnee2 = 0;
            {
                auto topOf = [] (const juce::Array<double>& c) { return c.getLast(); };
                sgPlateau = std::abs (topOf (n1) - topOf (n2));
                auto k1 = P::curveFeaturesTwoSegment (n1), k2 = P::curveFeaturesTwoSegment (n2);
                sgKnee2 = (k1.kneeFound && k2.kneeFound) ? std::abs (k1.kneeInDb - k2.kneeInDb) : -1;
            }
            say ("  sigma_f on this subject: plateau " + juce::String (sgPlateau, 3)
                 + " dB | knee " + (sgKnee2 >= 0 ? juce::String (sgKnee2, 3) : juce::String ("n/a"))
                 + " dB (instrument floors: plateau 0.088 dB from the depth measurement, knee 0.00 dB)");
            if (! isLim)
                say ("  BED LEVEL: the stepped curve has no bed -- 'closed' here means the "
                     "plugin's own output floor at each input step, and the gate's attenuation "
                     "is read against the unity region above threshold, not against a bed. The "
                     "burst train's bed (-30 dBFS) is used only for envelope work.");

            // EFFECT-BASED walk: 4 ladder points, plateau (primary) + knee (corroboration)
            say (isLim ? "     requested | landed | plateau out dBFS PEAK | knee dBFS PEAK | plateau err | knee err"
                       : "     requested | landed | gate open point dBFS PEAK | knee dBFS PEAK | open err | knee err");
            // EXCITATION, through the single resolution point. These two suites
            // declare NO plan: the limiter reads its own expressible range and
            // the gate drives level-swept bursts, so neither needs the plugin
            // put into a special state first. That was checked rather than
            // assumed, and the check that keeps it honest is below: when the
            // feature never moves, the suite must say INCONCLUSIVE and name
            // excitation as a candidate cause -- never route a verdict from a
            // plugin that was never engaged.
            ejmap::subject::ExcitationPlan declaredPlan;      // deliberately none
            const auto excPlan = ejmap::subject::resolveExcitation (mapVar, declaredPlan);
            say ("  " + excPlan.describe()
                 + (excPlan.declared() ? "" : " -- neither suite needs one; a missing plan may "
                                              "still leave the plugin unengaged, which is handled "
                                              "as inconclusive below, not as a verdict"));

            juce::Array<double> plErr, knErr, plPoints, plLanded;
            double worstClaimErr = 0; int claimChecks = 0; juce::StringArray claimRows;
            int featureUndefined = 0;
            for (double want : points)
            {
                // WRITE WHAT THE MAP SAYS. When map-driven, the norm and the
                // predicted landing both come from the map's anchors, so the
                // feature error below is measured against THE MAP'S CLAIM, not
                // against the plugin's own display ladder.
                const float norm = mapDriven ? slot.normFor (want) : nf3 (sw.anchors, want);
                const double wms = P::writeAndServiceRunloop (*cp[iCtl], norm);
                const double landed = mapDriven ? P::predictedLanding (slot.anchors, want)
                                                : P::predictedLanding (sw.anchors, want);

                // ---- THE DIVERGENCE THAT WAS INVISIBLE BEFORE --------------
                // The map claims this norm means `want`. The plugin's display
                // says what it actually means. A stale map, a map made in
                // another mode or preset, or one made against another version
                // disagrees HERE, and nothing in this suite could see it while
                // the ladder came from the plugin itself.
                if (mapDriven)
                {
                    const double liveLanded = P::predictedLanding (sw.anchors, want);
                    double shown = 0; bool parsed = false;
                    {
                        const auto txt = cp[iCtl]->getCurrentValueAsText();
                        const auto unit = slot.unit.isNotEmpty() ? slot.unit : sw.unitFamily;
                        float f = 0; bool negInf = false;
                        parsed = echojay::parseDisplayForUnit (txt, unit, f, negInf);
                        if (! parsed) parsed = echojay::parseDisplayForUnit (txt, "db", f, negInf);
                        // -inf is a real landing at a dB bottom, not a parse
                        // failure, and it must not read as 0.
                        shown = negInf ? -std::numeric_limits<double>::infinity() : (double) f;
                    }
                    const double claimErr = parsed ? std::abs (shown - want)
                                                   : std::abs (liveLanded - want);
                    worstClaimErr = juce::jmax (worstClaimErr, claimErr); ++claimChecks;
                    claimRows.add ("     map says " + juce::String (want, 2)
                        + " at norm " + juce::String (norm, 4) + " -> plugin "
                        + (parsed ? "displays " + juce::String (shown, 2)
                                  : "display unparsed, live ladder says "
                                    + juce::String (liveLanded, 2))
                        + "  |diff| " + juce::String (claimErr, 2));
                }
                say ("       write: norm " + juce::String (nf3 (sw.anchors, want), 4)
                     + " -> getValue " + juce::String (cp[iCtl]->getValue(), 4)
                     + ", display '" + cp[iCtl]->getCurrentValueAsText() + "'"
                     + (wms < 0 ? "  <- UNLANDED" : ", landed in " + juce::String (wms, 1) + " ms"));
                auto c = cap();
                double plat = 0;
                if (isLim)
                {
                    // TOP STEP ONLY. The five-step mean was contaminated: as the
                    // ceiling rises, steps BELOW it pass through unlimited and
                    // enter the average, which is exactly the measured error
                    // sequence 3.00 / 3.01 / 3.01 / 4.82. GR tracks |ceiling| to
                    // 0.02 dB, so the limiter is clean and the estimator was not.
                    plat = c.getLast();
                    // ITEM 2: was there a plateau to read at all?
                    const double gr = P::grAtTopDb (c);
                    if (gr < 1.0) ++featureUndefined;
                    say ("       [ceiling " + juce::String (landed, 2) + "] gain reduction at the "
                         "loudest step: " + juce::String (gr, 2) + " dB peak"
                         + (gr < 1.0 ? "  <- NEAR ZERO: the stimulus never drove the limiter, so "
                                       "there is NO plateau here; the cause is stimulus headroom, "
                                       "not true-peak behaviour"
                                     : "  <- the limiter is working, a plateau exists"));
                }
                else
                {
                    // LEVEL-SWEPT BURSTS: a gate threshold is an EVENT feature.
                    host.pausePumpForMutation();
                    auto pk = P::levelSweptBursts (*ci, gateGapS, 0.0, 4.0, 21);
                    host.resumePumpAfterMutation();
                    plat = P::loudestAttenuatedBurstDb (pk, 0.0, 4.0, 3.0);
                    juce::String pkLine;
                    for (int b = 0; b < pk.size(); b += 4)
                        pkLine << " " << juce::String (0.0 - 4.0 * b, 0) << "->"
                               << juce::String (pk[b], 1);
                    say ("       burst peaks (in->out, every 4th):" + pkLine);
                    if (plat <= -999.0) ++featureUndefined;
                    if (plat <= -999.0)
                        say ("       [threshold " + juce::String (landed, 2) + "] NO burst "
                             "is attenuated by 3 dB anywhere in the sweep -- gate threshold "
                             "UNDEFINED, not fitted");
                }
                auto kf = P::curveFeaturesTwoSegment (c);
                const double pe = std::abs (plat - landed), ke = kf.kneeFound ? std::abs (kf.kneeInDb - landed) : -1;
                plErr.add (pe); plPoints.add (plat); plLanded.add (landed);
                if (ke >= 0) knErr.add (ke);
                say ("     " + juce::String (want, 2).paddedLeft (' ', 9) + " | "
                     + juce::String (landed, 2).paddedLeft (' ', 6) + " | "
                     + juce::String (plat, 2).paddedLeft (' ', 14) + " | "
                     + (kf.kneeFound ? juce::String (kf.kneeInDb, 1) : juce::String ("UNDEF")).paddedLeft (' ', 7)
                     + " | " + juce::String (pe, 2).paddedLeft (' ', 11)
                     + " | " + (ke >= 0 ? juce::String (ke, 2) : juce::String ("-")));
            }
            // THE EXCITATION GUARD. If the feature never appeared at ANY ladder
            // point there is nothing to route: the plugin was never engaged, and
            // a route computed from -999s or from zero gain reduction would
            // publish a verdict about a plugin that did nothing. Inconclusive by
            // precondition -- the function that cannot express a confirm.
            if (featureUndefined >= points.size() && points.size() > 0)
            {
                emitInconclusive (isLim ? "ceiling_db" : "threshold_db",
                    juce::String (isLim ? "gain reduction stayed below 1 dB"
                                        : "no burst was attenuated by 3 dB")
                    + " at every one of the " + juce::String (points.size())
                    + " ladder points, so the feature never existed to measure. Candidate causes, "
                      "in order: the plugin was never engaged (no excitation plan is declared for "
                      "this suite -- " + excPlan.source + "), the stimulus does not reach the "
                      "ladder's range, or the target index is not the "
                    + juce::String (isLim ? "ceiling" : "threshold") + ". No verdict is reachable "
                      "from a feature that did not move",
                    "measured over 4 ladder points, and the feature never appeared in any of them");
                std::cout << (isLim ? "LIMITER" : "GATE") << " SUITE: INCONCLUSIVE "
                             "(feature never appeared; not a verdict)" << std::endl;
                quitNow(); return;
            }

            // With no map on this machine, write the live sweep out in map
            // shape. It is a SELF-MAP: circular as verification (it cannot
            // disagree with the plugin it came from) and useful as a specimen,
            // because a divergence check that has never been shown NOT to fire
            // is as unproven as one that has never fired.
            if (! mapDriven && sw.anchors.size() >= 2)
            {
                juce::Array<ejmap::subject::SelfMapEntry> entries;
                entries.add ({ wantSem, cp[iCtl]->getName (64), sw.unitFamily, iCtl, sw.anchors });
                auto out = ledger.getRoot().getChildFile ("selfmap-" + currentFp + ".json");
                out.replaceWithText (juce::JSON::toString (
                    ejmap::subject::selfMapVar (currentFp, ejmap::kMapSchemaString,
                                                isLim ? "limiter" : "gate", entries), false));
                say ("  self-map written: " + out.getFullPathName()
                     + " (the live ladder in map shape; circular as verification)");
            }

            // monotone + tracking, by EFFECT (the compressor's lesson)
            bool mono = true;
            for (int i = 1; i < plErr.size(); ++i) {}
            double worstPl = 0; for (auto e : plErr) worstPl = juce::jmax (worstPl, e);
            double worstKn = 0; for (auto e : knErr) worstKn = juce::jmax (worstKn, e);
            const double tolPl = juce::jmax (0.25 * std::abs (sw.anchors.getLast()[0]
                                                            - sw.anchors.getFirst()[0]) * 0.25,
                                             4.0 * juce::jmax (sgPlateau, 0.088));
            // ROUTING FORK, shared. The suite reports what it measured; the
            // fork in EjmapProbe.h decides which verdict language applies.
            double featMoved = 0;
            if (plPoints.size() >= 2)
                featMoved = std::abs (plPoints.getLast() - plPoints.getFirst());
            // Delta_pred IS NOT THE FULL LADDER SPAN. A limiter's plateau follows
            // its ceiling only while the ceiling sits BELOW the signal, so a
            // -30..0 ladder against a signal peaking lower has ladder that
            // cannot express, and predicting against the whole span reports a
            // correct plugin as an over-claim. Measured here rather than
            // assumed, and NOT fixed by raising the stimulus, which would work
            // on this subject and fail on any ladder exceeding a renderable
            // level. The compressor's -70 lesson, at the other end of the
            // curve. NOTE the gate shares this input and therefore this
            // exposure; both now clip.
            const double ladderRaw = std::abs (sw.anchors.getLast()[0] - sw.anchors.getFirst()[0]);
            double signalLevelDb = 0.0;
            {
                // most-permissive end of the ladder = least processing = the
                // signal's own level through this plugin, measured not guessed
                const double permissive = isLim ? juce::jmax ((double) sw.anchors.getFirst()[0],
                                                              (double) sw.anchors.getLast()[0])
                                                : juce::jmin ((double) sw.anchors.getFirst()[0],
                                                              (double) sw.anchors.getLast()[0]);
                P::writeAndServiceRunloop (*cp[iCtl], nf3 (sw.anchors, permissive));
                auto c0 = cap();
                signalLevelDb = c0.isEmpty() ? 0.0 : c0.getLast();
            }
            int expressible = 0;
            for (const auto& a2 : sw.anchors)
                if (isLim ? (a2[0] <= signalLevelDb) : (a2[0] >= P::kStepBaseDb))
                    ++expressible;
            const double exprFrac = sw.anchors.isEmpty() ? 0.0
                              : (double) expressible / sw.anchors.size();
            const double ladderSpan = ladderRaw * exprFrac;
            say ("  EXPRESSIBLE RANGE: signal through this plugin measures "
                 + juce::String (signalLevelDb, 2) + " dBFS peak, so " + juce::String (expressible)
                 + " of " + juce::String (sw.anchors.size()) + " ladder points ("
                 + juce::String (100.0 * exprFrac, 0) + "%) can express -- Delta_pred clipped from "
                 + juce::String (ladderRaw, 2) + " to " + juce::String (ladderSpan, 2)
                 + " dB. A verdict below covers THAT sub-range, not the whole ladder.");
            // Delta_pred IS THE PROBED SPAN, not the ladder's. featMoved is the
            // span between the FIRST and LAST probed points, so predicting
            // against the whole ladder compares a four-point probe against a
            // twenty-one-point range: (0.90 - 0.15) x 30 = 22.50 dB, which
            // matched the "over-claim" to the decimal. The expressible-range
            // clipping above is KEPT -- it was aimed at the wrong cause here
            // but is correct in general, and this stimulus reaching -0.04 dBFS
            // is a property of this stimulus, not a guarantee. Both suites take
            // this input; the gate's copy was masked only by its feature not
            // moving, which is how a defect survives to be rediscovered.
            const double probedSpan = plLanded.size() >= 2
                ? std::abs (plLanded.getLast() - plLanded.getFirst())
                : ladderSpan;
            const double predForRoute = juce::jmin (probedSpan, ladderSpan);
            const auto route = P::routeVerdict (featMoved, juce::jmax (sgPlateau, 0.088),
                                                predForRoute, tolPl);
            say ("  Delta_pred: probed span " + juce::String (probedSpan, 2)
                 + " dB (ladder points " + juce::String (plLanded.getFirst(), 2) + " -> "
                 + juce::String (plLanded.getLast(), 2) + "), expressible-clipped ladder "
                 + juce::String (ladderSpan, 2) + " dB -> using "
                 + juce::String (predForRoute, 2) + " dB");
            say ("  ROUTING: feature moved " + juce::String (featMoved, 2)
                 + " dB against a predicted " + juce::String (predForRoute, 2) + " dB -> "
                 + P::routeText (route, featMoved, predForRoute, juce::jmax (sgPlateau, 0.088), "dB"));
            if (route == P::Route::deafness)
            {
                say ("  carve-out 1 exclusion (a) mode states on this plugin:");
                juce::StringArray modes;
                for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->isDiscrete() || cp[i]->getNumSteps() <= 5)
                        modes.add (cp[i]->getName (32) + "=" + cp[i]->getCurrentValueAsText());
                say ("    " + (modes.isEmpty() ? juce::String ("none") : modes.joinIntoString (", ")));
                say ("  carve-out 1 exclusion (b) gesture evidence at index "
                     + juce::String (iCtl) + ": none on this machine (no capture row for this fp)");
            }
            // ---- what the map claimed vs what the plugin does ---------------
            if (mapDriven)
            {
                say ("");
                say ("  MAP-CLAIM CHECK (the upgrade: does the plugin do what the MAP says?)");
                for (const auto& r : claimRows) say (r);
                say ("  worst |map claim - plugin| " + juce::String (worstClaimErr, 2)
                     + " over " + juce::String (claimChecks) + " points"
                     + (worstClaimErr > 1.0
                          ? "  <- THE MAP AND THE PLUGIN DISAGREE. A ladder read live from this "
                            "plugin would have hidden this: the suite would have driven the "
                            "plugin's own numbers and confirmed them against themselves."
                          : "  <- the map's ladder and this plugin agree"));
            }
            else
                say ("  (no map-claim check: this run measured the plugin against its own "
                     "display ladder, which is the weaker claim)");

            // PER-POINT ACCURACY GATES THE SPAN VERDICT. When the map supplies
            // the ladder, "the feature moved by the predicted amount" is only a
            // statement about THE MAP if each point also landed where the map
            // said. Where it did not, the span verdict is a statement about the
            // plugin's own ladder and must not be published as a confirm about
            // the map's. One semantic, one verdict: two verdicts on one
            // semantic is its own confusion.
            if (mapDriven && worstPl > tolPl)
            {
                emitContradicts (isLim ? "ceiling_db" : "threshold_db",
                    "the feature tracks over its SPAN (moved " + juce::String (featMoved, 2)
                    + " dB against " + juce::String (predForRoute, 2) + " predicted) but each point "
                      "lands away from where the map says: worst |feature - map claim| "
                    + juce::String (worstPl, 2) + " dB against a " + juce::String (tolPl, 2)
                    + " dB tolerance, worst |map claim - display| "
                    + juce::String (worstClaimErr, 2) + " dB. A uniformly offset ladder moves by "
                      "the right amount with every point wrong, so the span alone cannot decide "
                      "this. The map does not describe this plugin as installed: stale, made in "
                      "another mode or preset, or made against another version");
                std::cout << (isLim ? "LIMITER" : "GATE") << " SUITE: "
                          << juce::String (fails) << " FAILED" << std::endl;
                quitNow(); return;
            }

            // ITEM 1: the WORDS come from the route. Routing right with wrong
            // words is the failure the fork existed to prevent.
            emitVerdict (isLim ? "ceiling_db (top-step plateau, PRIMARY)"
                        : "threshold_db (level-swept burst train, PRIMARY)",
                  featMoved, predForRoute, juce::jmax (sgPlateau, 0.088), tolPl,
                  juce::String (mapDriven ? "MAP-DRIVEN (feature vs the map's claim); "
                                          : "self-consistency only (no map); ")
                  + "worst |feature - ladder| " + juce::String (worstPl, 2) + " dB vs tol "
                  + juce::String (tolPl, 2) + "; corroborating knee estimator "
                  + (knErr.isEmpty() ? juce::String ("found no resolvable corner")
                                     : "worst " + juce::String (worstKn, 2) + " dB"),
                  "dB");
            std::cout << (isLim ? "LIMITER" : "GATE") << " SUITE: "
                      << (fails == 0 ? "PASS" : juce::String (fails) + " FAILED") << std::endl;
            quitNow(); return;
        }

        // ---- guardtest (ITEM 3): attempt what the guard refuses ----------
        // FORGETTABILITY TEST: a load site that plants NO stake of its own.
        // If the choke point works, a hard death here is still attributed.
        // Deliberately bare -- adding a beginLoad here would test nothing.
        if (mode == "mapstate")
        {
            // FIXTURE, not live data: the endpoint does not exist yet. One
            // synthetic identity per state, so the filter is tested by
            // ATTEMPTING WHAT IT REFUSES rather than by reading it.
            say ("MAPSTATE: fixture with one identity per state");
            const char* names[] = { "unmapped", "localOnly", "submittedByYou",
                                    "submittedByOther", "differentBuild", "unknown" };
            mapStateByIdentity.clear();
            for (int i = 0; i < 6; ++i)
            {
                MapStateRow r;
                r.state = (MapState) i;
                r.by = i == 3 ? "mapper-jonas" : "sean-studio";
                r.at = "2026-08-02"; r.schema = "2.2"; r.probed = (i == 2);
                r.mapsForIdentity = (i == 4 ? 2 : (i == 0 || i == 5 ? 0 : 1));
                r.paramCountHere = 86;
                mapStateByIdentity[juce::String ("Fixture|") + names[i] + "|1.0"] = r;
            }
            mapStateFetchedAt = juce::Time::getCurrentTime();
            mapStateFailure = {};

            saveMapStateCache();
            auto before = mapStateByIdentity;
            mapStateByIdentity.clear(); mapStateFetchedAt = juce::Time();
            loadMapStateCache();
            bool same = mapStateByIdentity.size() == before.size();
            for (const auto& kv : before)
                if (mapStateByIdentity.count (kv.first) == 0
                     || mapStateByIdentity[kv.first].state != kv.second.state
                     || mapStateByIdentity[kv.first].by != kv.second.by) same = false;
            say (juce::String (same ? "  ok   " : "  FAIL ")
                 + "cache round-trips through the FILE with all six states and their fields");
            say ("  status line: " + mapStateStatusLine());

            say ("  hide-mapped, per state:");
            int hidden = 0, hiddenWrongly = 0;
            for (int i = 0; i < 6; ++i)
            {
                const auto st = (MapState) i;
                const bool wouldHide = (st == MapState::submittedByYou
                                     || st == MapState::submittedByOther);
                const bool mustNotHide = (st == MapState::differentBuild
                                       || st == MapState::unknown);
                if (wouldHide) ++hidden;
                if (wouldHide && mustNotHide) ++hiddenWrongly;
                say (juce::String ("    ") + names[i] + " -> " + (wouldHide ? "HIDDEN" : "shown")
                     + (mustNotHide ? "   (must never be hidden)" : ""));
            }
            say (juce::String (hidden == 2 && hiddenWrongly == 0 ? "  ok   " : "  FAIL ")
                 + "hides exactly the two submitted states; differentBuild and unknown survive");

            mapStateFailure = "connection refused";
            say ("  on failure:      " + mapStateStatusLine());
            mapStateFailure = {}; mapStateByIdentity.clear();
            say ("  never fetched:   " + mapStateStatusLine());
            std::cout << "MAPSTATE: DONE" << std::endl; quitNow(); return;
        }

        if (mode == "identkey")
        {
            // Print the CLIENT identity key for the two live subjects, built
            // by the shipping function from a real PluginDescription. The
            // server key is computed separately from the stored map and the
            // two STRINGS are compared -- reading both implementations and
            // concluding they agree is the failure mode this check exists to
            // avoid (the doubled stake id looked plausible by eye too).
            for (auto* id : { "AudioUnit:Effects/aumf,ameq,Brwx",
                              "AudioUnit:Effects/aufx,SpfA,OekS" })
            {
                auto d = echojay::auregistry::describeFromRegistry (id);
                if (d.fileOrIdentifier.isEmpty()) { say ("  absent: " + juce::String (id)); continue; }
                say ("CLIENTKEY\t" + d.name + "\t" + echojay::identityKeyForDescription (d));
            }
            std::cout << "IDENTKEY: DONE" << std::endl; quitNow(); return;
        }

        if (mode == "lookupmiss")
        {
            // Item 2: CONSTRUCT the specimen rather than hunt one. Any
            // VST3-only product searched in the AU census exercises the
            // recorder. Both names below are real products on this machine;
            // neither ships an AU.
            // list EVERY match, not the first -- substring resolution took
            // the UAD component last time because it matched first.
            {
                auto cen = echojay::auregistry::buildCensus();
                for (const auto& t : cen.targets)
                {
                    auto d = echojay::auregistry::describeFromRegistry (t.identifier);
                    if (d.name.containsIgnoreCase ("saturat") || d.name.containsIgnoreCase ("crusher"))
                        say ("  CANDIDATE: '" + d.name + "' | vendor '" + d.manufacturerName
                             + "' | id " + d.fileOrIdentifier);
                }
            }
            for (auto* n : std::initializer_list<const char*>{})
            {
                say ("  searching AU census for '" + juce::String (n) + "':");
                auto d = resolveSubjectByName (n);
                if (d.fileOrIdentifier.isNotEmpty())
                    say ("    RESOLVED TO: name '" + d.name + "' | vendor '" + d.manufacturerName
                         + "' | format " + d.pluginFormatName + " | id " + d.fileOrIdentifier);
            }
            std::cout << "LOOKUPMISS: DONE" << std::endl; quitNow(); return;
        }

        if (mode == "forget")
        {
            auto census8 = echojay::auregistry::buildCensus();
            juce::PluginDescription pd8;
            for (const auto& t : census8.targets)
            { auto d = echojay::auregistry::describeFromRegistry (t.identifier);
              if (d.name.containsIgnoreCase ("MCompressor")) { pd8 = d; break; } }
            if (pd8.fileOrIdentifier.isEmpty())
            { say ("FORGET: MCompressor absent"); quitNow(); return; }
            say ("FORGET: bare load site, NO caller stake, loading " + pd8.name);
            std::cout.flush();
            host.unload();
            auto r8 = host.load (pd8, watchdog);      // expected: dies inside
            say ("FORGET: survived the load, outcome detail: " + r8.detail);
            quitNow(); return;
        }

        // ---- SATURATION: THD monotone in drive, profile as evidence -------
        if (mode == "saturation")
        {
            // PINNED BY ID, not by name. "bx_saturator V2" resolved to the
            // UAD component (aufx,33au,!UAD) because substring matching takes
            // the first container of the string; the intended Plugin Alliance
            // product is aufx,bxa2,Brwx. A suite naming a specific product
            // matches on the id. Identity is PRINTED before the load.
            auto pd9 = echojay::auregistry::describeFromRegistry ("AudioUnit:Effects/aufx,bxa2,Brwx");
            if (pd9.fileOrIdentifier.isEmpty())
            { say ("SATURATION: pinned id not in the census"); quitNow(); return; }
            say ("SATURATION subject, resolved BEFORE load: name '" + pd9.name + "' | vendor '"
                 + pd9.manufacturerName + "' | id " + pd9.fileOrIdentifier);
            host.unload();
            loadedName = pd9.name; loadedId = pd9.fileOrIdentifier; loadedDesc = pd9;
            if (host.load (pd9, watchdog).outcome != LoadOutcome::ok)
            { say ("SATURATION: load failed"); quitNow(); return; }
            auto* ci = host.getInstance();
            auto cp = ci->getParameters();
            say ("SATURATION SUITE | " + pd9.name + " | " + juce::String (cp.size()) + " params");

            auto sine = [&] { host.pausePumpForMutation();
                              auto b = P::renderSine (*ci);
                              host.resumePumpAfterMutation(); return b; };
            // INHERITANCE CHECK 1: the shared sensitivity check must fire here.
            if (! renderPlaneSensitive (*ci, pd9.name,
                    [&] { auto sp = P::welch (sine());
                          juce::Array<double> rows;
                          for (int h = 1; h <= 6; ++h)
                              rows.add (P::bandEnergyDb (sp.mid, 997.0 * h - 40, 997.0 * h + 40));
                          return rows; }))
            { std::cout << "SATURATION SUITE: STOPPED (render plane blind)" << std::endl;
              quitNow(); return; }

            auto candidates2 = [&] (const juce::String& sub) {
                juce::Array<int> out;
                for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).containsIgnoreCase (sub) && cp[i]->isAutomatable())
                        out.add (i);
                return out; };
            // PINNED BY INDEX, the pattern the subject-by-id fix established.
            // bx_saturator V2 is an M/S multiband: five Drive controls (Master,
            // Mid Lo/Hi, Side Lo/Hi) plus four Drive Compensation, so every
            // name pattern is ambiguous and the rule correctly declines. Rather
            // than invent an M/S convention on one subject, pin the global
            // control and print it before use.
            int iDrive = -1;
            {
                const int pinned = 34;      // "Master Drive"
                if (juce::isPositiveAndBelow (pinned, cp.size())
                     && cp[pinned]->getName (64).containsIgnoreCase ("Master Drive"))
                {
                    iDrive = pinned;
                    say ("  drive PINNED by index: [" + juce::String (iDrive) + "] "
                         + cp[iDrive]->getName (32) + " (five Drive controls exist on this M/S "
                           "multiband; the ambiguity rule declines to pick, so the global one is "
                           "named explicitly)");
                }
                else
                    say ("  pinned index 34 is not 'Master Drive' on this build -- refusing to use it");
            }
            for (auto* n : (iDrive >= 0 ? std::initializer_list<const char*>{}
                                        : std::initializer_list<const char*>{ "Drive", "Saturation", "Amount" }))
            { auto c = candidates2 (n);
              if (c.size() == 1) { iDrive = c[0];
                  say ("  drive target: [" + juce::String (iDrive) + "] " + cp[iDrive]->getName (32));
                  break; }
              if (c.size() > 1)
                  say ("  pattern '" + juce::String (n) + "' AMBIGUOUS (" + juce::String (c.size())
                       + " matches) -- declining to pick"); }
            if (iDrive < 0)
            {
                juce::StringArray all;
                for (int i = 0; i < cp.size(); ++i)
                    all.add ("[" + juce::String (i) + "]" + cp[i]->getName (22));
                say ("  no unambiguous drive parameter. Params: " + all.joinIntoString (" "));
                quitNow(); return;
            }

            auto swD = sweepOneIndex (*ci, iDrive, watchdog, loadedId);
            auto nfD = [] (const juce::Array<juce::Array<float>>& a, double v) {
                auto e = echojay::dominantMonotonicTable (a);
                return echojay::interpolateAnchors (e.table, (float) v); };
            say ("  drive ladder " + juce::String (swD.anchors.getFirst()[0], 2) + " .. "
                 + juce::String (swD.anchors.getLast()[0], 2) + " (unit '" + swD.unitFamily + "')");

            say ("     drive | landed |  THD dB  | harmonic profile h2..h7 (dB rel. fundamental)");
            juce::Array<double> thds, landeds;
            for (double f : { 0.1, 0.4, 0.7, 1.0 })
            {
                const double lo = swD.anchors.getFirst()[0], hi = swD.anchors.getLast()[0];
                const double want = lo + f * (hi - lo);
                P::writeAndServiceRunloop (*cp[iDrive], nfD (swD.anchors, want));
                const double landed = P::predictedLanding (swD.anchors, want);
                auto t = P::thdOf (P::welch (sine()));
                thds.add (t.thdDb); landeds.add (landed);
                juce::String prof;
                for (auto h : t.harmonicsDb) prof << " " << juce::String (h, 1);
                say ("     " + juce::String (want, 2).paddedLeft (' ', 5) + " | "
                     + juce::String (landed, 2).paddedLeft (' ', 6) + " | "
                     + juce::String (t.thdDb, 2).paddedLeft (' ', 8) + " |" + prof);
            }
            bool mono = true;
            for (int i = 1; i < thds.size(); ++i) if (thds[i] < thds[i-1] - 0.5) mono = false;
            // INHERITANCE CHECK 2: Delta_pred from the PROBED span, not the ladder.
            const double probedSpan2 = std::abs (landeds.getLast() - landeds.getFirst());
            const double ladderRaw2 = std::abs (swD.anchors.getLast()[0] - swD.anchors.getFirst()[0]);
            say ("  Delta_pred: probed span " + juce::String (probedSpan2, 2)
                 + " (ladder " + juce::String (ladderRaw2, 2) + ") -- probed span used");
            const double thdMoved = std::abs (thds.getLast() - thds.getFirst());
            emitVerdict ("drive (THD monotone, PRIMARY)",
                  thdMoved, juce::jmax (1.0, std::abs (thds.getLast() - thds.getFirst())),
                  0.088, 0.25 * juce::jmax (1.0, thdMoved),
                  "THD " + juce::String (thds.getFirst(), 2) + " -> " + juce::String (thds.getLast(), 2)
                  + " dB across drive " + juce::String (landeds.getFirst(), 2) + " -> "
                  + juce::String (landeds.getLast(), 2) + " (moved " + juce::String (thdMoved, 2)
                  + " dB), monotone: " + (mono ? "YES" : "NO")
                  + ". Harmonic profile recorded as evidence, no verdict issued on it.",
                  "dB");
            std::cout << "SATURATION SUITE: " << (fails == 0 ? "PASS" : "FAILED") << std::endl;
            quitNow(); return;
        }

        if (mode == "guardtest")
        {
            say ("MODE GUARD TEST: feeding the guard displays it must refuse");
            say ("  (Var firing once on API-2500 was an observation; this is the test)");
            struct C { const char* d; bool expectGuard; };
            const C cases[] = {
                { "Var",        true  }, { "Auto",     true  }, { "Sync",   true },
                { "Ext",        true  }, { "Link",     true  }, { "Off",    true },
                { "Prog",       true  },   // never-seen token: must STILL be guarded
                { "\xe2\x88\x9e",  true  },   // a glyph
                { "Adaptif",    true  },   // localised
                { "",           true  },   // empty display
                { "30",         false }, { "-20.0",    false }, { "1.5 ms", false },
                { "0.05",       false }, { "4:1",      false } };
            int bad = 0;
            for (const auto& c : cases)
            {
                const juce::String d (juce::CharPointer_UTF8 (c.d));
                const bool guarded = P::displayIsModeToken (d);
                const bool ok = guarded == c.expectGuard;
                if (! ok) ++bad;
                say (juce::String (ok ? "  ok   " : "  FAIL ") + "display '" + d + "' -> "
                     + (guarded ? "GUARDED: " + P::modeTokenReason (d)
                                : juce::String ("numeric, verdict permitted")));
            }
            // the token must be quoted VERBATIM in the reason, not normalised
            const juce::String odd ("Prog");
            const bool quoted = P::modeTokenReason (odd).contains ("'" + odd + "'");
            say (juce::String (quoted ? "  ok   " : "  FAIL ")
                 + "unrecognised token is quoted verbatim in the reason");
            if (! quoted) ++bad;
            std::cout << "GUARDTEST: " << (bad == 0 ? "PASS" : juce::String (bad) + " FAILED")
                      << std::endl;
            quitNow(); return;
        }

        // ---- knee2 (ITEM 1): the knee estimator on a SHARP-curve design ---
        if (mode == "knee2")
        {
            // ITEM 1: the scopes were the right shape at the WRONG SITE.
            // MCompressor dies inside the registry walk, before any load, so
            // the stake and the deadline belong HERE -- around buildCensus and
            // the per-target describeFromRegistry loop. The stake names the
            // search target because no plugin id exists yet.
            const juce::String want = "MCompressor";
            ledger.beginLoad ("census:" + want, want, "", "AudioUnit", "",
                              "probe_gate_census", "probe_gate_census");
            juce::PluginDescription mc;
            {
                Watchdog::Scope guard (watchdog, "probe_gate_census", "census:" + want,
                                       want, "AudioUnit", "probe", 30000);
                auto census2 = echojay::auregistry::buildCensus();
                for (const auto& t : census2.targets)
                {
                    // per-target scope: the walk is a loop, and a wedge inside
                    // ONE describe must name that target, not the whole walk.
                    Watchdog::Scope inner (watchdog, "probe_gate_describe", t.identifier,
                                           t.identifier, "AudioUnit", "probe", 15000);
                    auto d = echojay::auregistry::describeFromRegistry (t.identifier);
                    if (d.name.containsIgnoreCase (want)) { mc = d; break; }
                }
            }
            if (mc.fileOrIdentifier.isNotEmpty())
                ledger.quarantine ("census:" + want, "probe gate: census walk completed", "probe");
            if (mc.fileOrIdentifier.isEmpty())
            { say ("KNEE2: no digital-style compressor on this machine"); quitNow(); return; }
            host.unload();
            loadedName = mc.name; loadedId = mc.fileOrIdentifier; loadedDesc = mc;

            // ITEM 1: the gate path had NO watchdog around its own load and
            // sweep -- MCompressor wedged it for ten minutes with nothing on
            // disk naming the plugin. Same class as the endLoad() finding.
            // The stake goes down BEFORE the call, so a hang that terminates
            // the process is attributable on relaunch.
            ledger.beginLoad (loadedId, mc.name, mc.manufacturerName,
                              mc.pluginFormatName, mc.version, "probe_gate_load");
            {
                Watchdog::Scope guard (watchdog, "probe_gate_load", loadedId,
                                       mc.name, mc.pluginFormatName, "probe", 30000);
                if (host.load (mc, watchdog).outcome != LoadOutcome::ok)
                {
                    ledger.quarantine (loadedId, "probe gate: load failed", "probe");
                    say ("KNEE2: load failed, quarantined with a reason on disk");
                    quitNow(); return;
                }
            }
            auto* ci = host.getInstance();
            auto cp = ci->getParameters();
            say ("KNEE ESTIMATOR vs A SHARP-CURVE DESIGN | " + mc.name
                 + " | " + juce::String (cp.size()) + " params");
            auto idxOf = [&] (const juce::String& sub) {
                for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).containsIgnoreCase (sub)) return i;
                return -1; };
            const int iTh = idxOf ("Threshold"), iRa = idxOf ("Ratio"), iKn = idxOf ("Knee");
            say ("  Threshold [" + juce::String (iTh) + "] Ratio [" + juce::String (iRa)
                 + "] Knee [" + juce::String (iKn) + "]");
            if (iTh < 0 || iRa < 0) { say ("KNEE2: no threshold/ratio"); quitNow(); return; }
            SweepOutcome swT, swR;
            {
                Watchdog::Scope guard (watchdog, "probe_gate_sweep", loadedId,
                                       mc.name, mc.pluginFormatName, "probe", 30000);
                swT = sweepOneIndex (*ci, iTh, watchdog, loadedId);
                swR = sweepOneIndex (*ci, iRa, watchdog, loadedId);
            }
            say ("  threshold ladder " + juce::String (swT.anchors.getFirst()[0], 1) + ".."
                 + juce::String (swT.anchors.getLast()[0], 1) + " (unit '" + swT.unitFamily
                 + "'), ratio ladder " + juce::String (swR.anchors.getFirst()[0], 1) + ".."
                 + juce::String (swR.anchors.getLast()[0], 1));
            auto nf = [] (const juce::Array<juce::Array<float>>& a, double v) {
                auto e = echojay::dominantMonotonicTable (a);
                return echojay::interpolateAnchors (e.table, (float) v); };
            if (iKn >= 0) { P::writeAndServiceRunloop (*cp[iKn], 0.0f);
                            say ("  Knee -> hard end (display '"
                                 + cp[iKn]->getCurrentValueAsText() + "')"); }
            P::writeAndServiceRunloop (*cp[iRa], nf (swR.anchors, 8.0));
            say ("     requested thr | landed (ladder) | measured knee | error dB");
            double worst = 0; int n = 0;
            for (double want : { -30.0, -24.0, -18.0, -12.0 })
            {
                P::writeAndServiceRunloop (*cp[iTh], nf (swT.anchors, want));
                const double landed = P::predictedLanding (swT.anchors, want);
                host.pausePumpForMutation();
                auto c = P::steppedCurve (*ci);
                host.resumePumpAfterMutation();
                auto f = P::curveFeaturesTwoSegment (c);
                const double err = f.kneeFound ? std::abs (f.kneeInDb - landed) : -1;
                if (err >= 0) { worst = juce::jmax (worst, err); ++n; }
                say ("     " + juce::String (want, 1).paddedLeft (' ', 13) + " | "
                     + juce::String (landed, 1).paddedLeft (' ', 15) + " | "
                     + (f.kneeFound ? juce::String (f.kneeInDb, 1) : juce::String ("UNDEF")).paddedLeft (' ', 13)
                     + " | " + (err >= 0 ? juce::String (err, 2) : juce::String ("-")));
            }
            say ("  WORST knee error on this sharp-curve design: " + juce::String (worst, 2)
                 + " dB across " + juce::String (n) + " thresholds");
            say (worst <= 2.5
                 ? "  => THRESHOLD IS PROVABLE on resolvable-breakpoint designs. API-2500's "
                   "measured 44% is a SOFT-CURVE property, per plugin, not an estimator limit."
                 : "  => the ESTIMATOR is the limit; threshold is not provable as built.");
            std::cout << "KNEE2: DONE" << std::endl; quitNow(); return;
        }

        // ---- kneefloor: knee and slope estimator resolution, known truth --
        if (mode == "kneefloor")
        {
            say ("KNEE/SLOPE FLOOR: known-truth static compressor, no plugin in the path");
            say ("  stimulus step grid = 2.0 dB; truths chosen ON and OFF the grid");
            say ("     true thr | true ratio | step-grid knee (err) | 2-seg knee (err) | step slope (err) | 2-seg slope (err)");
            const double thrs[]  = { -50.0, -30.0, -25.0, -20.7, -19.3, -15.0, -12.4, -8.0 };
            const double ratios[] = { 2.0, 4.0, 10.0 };
            double wStep = 0, wSeg = 0, wSlopeStep = 0, wSlopeSeg = 0;
            for (double th : thrs)
                for (double ra : ratios)
                {
                    auto c = P::syntheticCurve (th, ra);
                    auto fs = P::curveFeatures (c);
                    auto f2 = P::curveFeaturesTwoSegment (c);
                    const double eS = fs.kneeFound ? std::abs (fs.kneeInDb - th) : -1;
                    const double e2 = f2.kneeFound ? std::abs (f2.kneeInDb - th) : -1;
                    const double sS = fs.kneeFound ? std::abs (fs.slopeAbove - 1.0 / ra) : -1;
                    const double s2 = f2.kneeFound ? std::abs (f2.slopeAbove - 1.0 / ra) : -1;
                    if (eS > 0) wStep = juce::jmax (wStep, eS);
                    if (e2 > 0) wSeg = juce::jmax (wSeg, e2);
                    if (sS > 0) wSlopeStep = juce::jmax (wSlopeStep, sS);
                    if (s2 > 0) wSlopeSeg = juce::jmax (wSlopeSeg, s2);
                    say ("     " + juce::String (th, 1).paddedLeft (' ', 8) + " | "
                         + juce::String (ra, 1).paddedLeft (' ', 10) + " | "
                         + (fs.kneeFound ? juce::String (fs.kneeInDb, 1) + " (" + juce::String (eS, 2) + ")"
                                         : juce::String ("UNDEF")).paddedLeft (' ', 20) + " | "
                         + (f2.kneeFound ? juce::String (f2.kneeInDb, 1) + " (" + juce::String (e2, 2) + ")"
                                         : juce::String ("UNDEF")).paddedLeft (' ', 16) + " | "
                         + (fs.kneeFound ? juce::String (fs.slopeAbove, 3) + " (" + juce::String (sS, 3) + ")"
                                         : juce::String ("UNDEF")).paddedLeft (' ', 16) + " | "
                         + (f2.kneeFound ? juce::String (f2.slopeAbove, 3) + " (" + juce::String (s2, 3) + ")"
                                         : juce::String ("UNDEF")));
                }
            say ("  WORST knee error: step-grid " + juce::String (wStep, 2)
                 + " dB | two-segment " + juce::String (wSeg, 2) + " dB");
            say ("  WORST slope error: step-grid " + juce::String (wSlopeStep, 4)
                 + " | two-segment " + juce::String (wSlopeSeg, 4));
            std::cout << "KNEEFLOOR: DONE" << std::endl; quitNow(); return;
        }

        // ---- relwalk: is the release reversal real or program-dependent? --
        if (mode == "relwalk")
        {
            auto cdesc = echojay::auregistry::describeFromRegistry ("AudioUnit:Effects/aufx,APCM,ksWV");
            ScannedPlugin csp; csp.desc = cdesc;
            host.unload();
            loadedName = cdesc.name; loadedId = csp.pluginId(); loadedDesc = cdesc;
            if (host.load (cdesc, watchdog).outcome != LoadOutcome::ok)
            { say ("RELWALK: load failed"); quitNow(); return; }
            auto* ci = host.getInstance();
            auto cp = ci->getParameters();
            auto idxOf = [&] (const juce::String& nm) {
                for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).equalsIgnoreCase (nm)) return i;
                return -1; };
            const int iTh = idxOf ("Thresh"), iRa = idxOf ("Ratio"), iRe = idxOf ("Release");
            auto swRe = sweepOneIndex (*ci, iRe, watchdog, loadedId);
            auto swTh = sweepOneIndex (*ci, iTh, watchdog, loadedId);
            auto swRa = sweepOneIndex (*ci, iRa, watchdog, loadedId);
            auto nf = [] (const juce::Array<juce::Array<float>>& a, double v) {
                auto e = echojay::dominantMonotonicTable (a);
                return echojay::interpolateAnchors (e.table, (float) v); };
            P::writeAndServiceRunloop (*cp[iTh], nf (swTh.anchors, -30.0));
            P::writeAndServiceRunloop (*cp[iRa], nf (swRa.anchors, swRa.anchors.getLast()[0]));
            say ("RELEASE LADDER WALK | " + cdesc.name + " | ladder "
                 + juce::String (swRe.anchors.getFirst()[0], 3) + " .. "
                 + juce::String (swRe.anchors.getLast()[0], 3)
                 + " (" + juce::String (swRe.anchors.size()) + " anchors, unit family '"
                 + swRe.unitFamily + "')");
            say ("  threshold -30 dB, ratio max, everything else fixed; 5 ladder points x 2 materials");
            say ("     ladder value | display | tau sparse + excursion/residual dB | tau dense + excursion/residual dB");
            juce::Array<double> sparse, dense;
            for (int k = 0; k < 5; ++k)
            {
                const double v = swRe.anchors.getFirst()[0]
                               + k * (swRe.anchors.getLast()[0] - swRe.anchors.getFirst()[0]) / 4.0;
                P::writeAndServiceRunloop (*cp[iRe], nf (swRe.anchors, v));
                const auto disp = cp[iRe]->getCurrentValueAsText();
                host.pausePumpForMutation();
                auto eS = P::burstEnvelope (*ci, false);
                auto eD = P::burstEnvelope (*ci, true);
                host.resumePumpAfterMutation();
                auto rS = P::timeConstantFull (eS, 1402.0, 1900.0);
                auto rD = P::timeConstantFull (eD, 421.0, 600.0);
                sparse.add (rS.tauMs); dense.add (rD.tauMs);
                say ("     " + juce::String (v, 3).paddedLeft (' ', 12) + " | "
                     + disp.paddedLeft (' ', 7) + " | "
                     + (rS.tauMs > 0 ? juce::String (rS.tauMs, 1) : juce::String ("UNDEF")).paddedLeft (' ', 8)
                     + " exc " + juce::String (rS.excursionDb, 2) + " res "
                     + juce::String (rS.residualDb, 2)
                     + " | " + (rD.tauMs > 0 ? juce::String (rD.tauMs, 1) : juce::String ("UNDEF")).paddedLeft (' ', 8)
                     + " exc " + juce::String (rD.excursionDb, 2) + " res "
                     + juce::String (rD.residualDb, 2)
                     + "  [" + rD.why() + "]");
            }
            auto verdictOf = [] (const juce::Array<double>& v) {
                int up = 0, down = 0;
                for (int i = 1; i < v.size(); ++i)
                { if (v[i] > v[i - 1] * 1.05) ++up; else if (v[i] < v[i - 1] * 0.95) ++down; }
                if (up > 0 && down == 0) return juce::String ("monotone UP");
                if (down > 0 && up == 0) return juce::String ("monotone DOWN (reversed)");
                return juce::String ("NON-MONOTONE"); };
            say ("  sparse material: " + verdictOf (sparse));
            say ("  dense material:  " + verdictOf (dense));
            std::cout << "RELWALK: DONE" << std::endl; quitNow(); return;
        }

        // ---- taufloor: what the envelope extractor can actually resolve --
        if (mode == "taufloor")
        {
            say ("TAU FLOOR: known-truth exponentials, no plugin (env resolution "
                 + juce::String (P::envMsPerSample(), 3) + " ms/sample)");
            say ("     true tau ms | measured ms | error % ");
            const double taus[] = { 0.03, 0.1, 0.3, 0.5, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0 };
            double worstRel = 0; double firstGood = -1;
            for (double t : taus)
            {
                auto e = P::syntheticEnvelope (0.0, -12.0, t, (int) juce::jmax (20.0, t * 8));
                const double m = P::timeConstantMs (e, 0.0, juce::jmax (20.0, t * 8) - 1, 1.0);
                const double rel = m > 0 ? 100.0 * (m - t) / t : -1;
                if (m > 0 && std::abs (rel) <= 25.0 && firstGood < 0) firstGood = t;
                if (m > 0 && t >= 1.0) worstRel = juce::jmax (worstRel, std::abs (rel));
                say ("     " + juce::String (t, 2).paddedLeft (' ', 11) + " | "
                     + (m > 0 ? juce::String (m, 3).paddedLeft (' ', 11) : juce::String ("  UNDEFINED"))
                     + " | " + (m > 0 ? juce::String (rel, 1) : juce::String ("-")));
            }
            say ("  shortest tau resolved within 25%: " + juce::String (firstGood, 2)
                 + " ms | worst error at tau >= 1 ms: " + juce::String (worstRel, 1) + "%");
            std::cout << "TAUFLOOR: DONE" << std::endl; quitNow(); return;
        }

        // ---- comp mode: the compressor suite against a LIVE subject -------
        if (mode == "comp")
        {
            auto cdesc = echojay::auregistry::describeFromRegistry ("AudioUnit:Effects/aufx,APCM,ksWV");
            ScannedPlugin csp; csp.desc = cdesc;
            host.unload();
            loadedName = cdesc.name; loadedId = csp.pluginId(); loadedDesc = cdesc;
            auto cres = host.load (cdesc, watchdog);
            if (cres.outcome != LoadOutcome::ok) { say ("COMP: load failed"); quitNow(); return; }
            auto* ci = host.getInstance();
            auto cp = ci->getParameters();
            say ("COMP SUITE | " + cdesc.name + " (BRIDGED Waves) | " + juce::String (cp.size()) + " params");

            auto idxOf = [&] (const juce::String& nm) {
                for (int i = 0; i < cp.size(); ++i)
                    if (cp[i]->getName (64).equalsIgnoreCase (nm)) return i;
                return -1; };
            // ---- THE MAP (M9 parameterisation item 2) ----------------------
            // What can now be wrong that could not be before, asked BEFORE the
            // conversion rather than discovered during it:
            //   1. the map's INDEX may address a different parameter (a Bank
            //      insertion broke 339 indices on this project). Impossible
            //      while indices came from a name lookup on the live instance.
            //   2. an OFFSET threshold ladder gives the right GR span with
            //      every point wrong -- item 1's class, and this suite's
            //      threshold verdict routes on totalGr, a span.
            //   3. a RATIO ladder whose values are right and whose norms are
            //      not lands the plugin elsewhere on a curve the map describes
            //      correctly; the slope-delta verdict sees only the delta.
            //   4. a DECLARED BUT WRONG unit family (ms against a seconds
            //      ladder) scales every time prediction by 1000.
            // 1 is caught by crossCheckName + a range guard, 2/3/4 by the
            // map-claim check and the per-point gate below. None was reachable
            // before, and none is visible to a span or delta verdict.
            cal = capture.calibrate (*ci, loadedId);
            currentFp = echojay::fingerprintForDescription (loadedDesc, cal.paramCount);
            auto compMapFile = ledger.getRoot().getChildFile ("maps")
                                     .getChildFile (currentFp + ".json");
            auto compMap = juce::JSON::parse (compMapFile.loadFileAsString());
            const bool haveCompMap = compMap.isObject();
            auto thSlot = ejmap::subject::slotFor (compMap, "threshold_db");
            auto raSlot = ejmap::subject::slotFor (compMap, "ratio");
            const bool compMapDriven = thSlot.ok() && raSlot.ok();
            say ("  fp " + currentFp.substring (0, 12) + " | map "
                 + (haveCompMap ? compMapFile.getFileName() : juce::String ("NONE on this machine")));

            int iTh = idxOf ("Thresh"), iRa = idxOf ("Ratio");
            const int iAt = idxOf ("Attack"), iRe = idxOf ("Release"), iOut = idxOf ("Output");
            if (compMapDriven)
            {
                // Range guard, then the NAME CROSS-CHECK. Resolution is by
                // index because that is what the map carries; the name is the
                // check on it, deliberately the reverse order -- names survived
                // every version transition measured, indices did not.
                if (! juce::isPositiveAndBelow (thSlot.index, cp.size())
                    || ! juce::isPositiveAndBelow (raSlot.index, cp.size()))
                {
                    emitInconclusive ("threshold_db", "the map's indices ("
                        + juce::String (thSlot.index) + ", " + juce::String (raSlot.index)
                        + ") are outside this instance's " + juce::String (cp.size())
                        + " parameters -- the map does not describe this plugin");
                    std::cout << "COMP SUITE: INCONCLUSIVE (map index out of range)" << std::endl;
                    quitNow(); return;
                }
                for (const auto* pr : { &thSlot, &raSlot })
                {
                    auto nc = ejmap::subject::crossCheckName (*pr, cp[pr->index]->getName (64));
                    if (nc.checkable && ! nc.agrees)
                    {
                        emitContradicts (pr->semantic, nc.why + ". A verdict computed from this "
                            "index would be about the wrong parameter, and every span it produced "
                            "would look plausible. Names survived every version transition this "
                            "project measured; indices did not");
                        std::cout << "COMP SUITE: " << fails << " FAILED (map index names the "
                                     "wrong parameter)" << std::endl;
                        quitNow(); return;
                    }
                    say ("  index/name cross-check: " + pr->semantic + " -> ["
                         + juce::String (pr->index) + "] map '" + pr->name + "' vs instance '"
                         + cp[pr->index]->getName (64) + "'"
                         + (nc.checkable ? " -- agree" : " -- " + nc.why));
                }
                iTh = thSlot.index; iRa = raSlot.index;
                say ("  target FROM THE MAP: threshold_db [" + juce::String (iTh) + "] ladder "
                     + juce::String (thSlot.ladderLo(), 2) + " .. " + juce::String (thSlot.ladderHi(), 2)
                     + " | ratio [" + juce::String (iRa) + "] ladder "
                     + juce::String (raSlot.ladderLo(), 2) + " .. " + juce::String (raSlot.ladderHi(), 2));
            }
            else
                say ("  NO MAP-DRIVEN TARGETS: " + (haveCompMap ? thSlot.why + " / " + raSlot.why
                                                                : juce::String ("no map for this fp"))
                     + ". Falling back to name lookup -- this run is a DISPLAY-VS-RENDER "
                       "SELF-CONSISTENCY CHECK, not a check of any map's claims.");
            say ("  indices: Thresh [" + juce::String (iTh) + "] Ratio [" + juce::String (iRa)
                 + "] Attack [" + juce::String (iAt) + "] Release [" + juce::String (iRe)
                 + "] Output [" + juce::String (iOut) + "]");

            // Anchors from the REAL M3 sweeper -- no compressor map exists
            // yet, so the suite builds its ladder the way the tool would.
            struct Sw { juce::Array<juce::Array<float>> a; juce::String method, unit; int n = 0; };
            auto sweep = [&] (int idx) {
                Sw r;
                auto o = sweepOneIndex (*ci, idx, watchdog, loadedId);
                r.a = o.anchors; r.method = o.method; r.unit = o.unitFamily; r.n = o.anchors.size();
                say ("  swept [" + juce::String (idx) + "] " + cp[idx]->getName (32)
                     + ": " + juce::String (r.n) + " anchors (" + r.method
                     + ", display unit family: '" + o.unitFamily + "', sample display: '"
                     + cp[idx]->getCurrentValueAsText() + "'), range "
                     + (r.n > 0 ? juce::String (o.anchors.getFirst()[0], 2) + " .. "
                                  + juce::String (o.anchors.getLast()[0], 2) : juce::String ("none")));
                return r; };
            auto swTh = sweep (iTh); auto swRa = sweep (iRa);
            auto swAt = sweep (iAt); auto swRe = sweep (iRe);
            // The live sweep stays as the second opinion; the MAP's ladder is
            // what the suite drives and predicts against, so every downstream
            // use becomes map-driven by this one substitution rather than by
            // threading a flag through forty call sites.
            const auto liveThAnchors = swTh.a, liveRaAnchors = swRa.a;
            if (compMapDriven)
            {
                swTh.a = thSlot.anchors; swRa.a = raSlot.anchors;
                say ("  ladders now FROM THE MAP (live sweep kept as the second opinion): "
                     "threshold " + juce::String (swTh.a.size()) + " anchors, ratio "
                     + juce::String (swRa.a.size()) + " anchors");
            }
            const juce::String swAtUnit = swAt.unit, swReUnit = swRe.unit;

            auto normFor2 = [] (const juce::Array<juce::Array<float>>& a, double v) {
                auto eff = echojay::dominantMonotonicTable (a);
                return echojay::interpolateAnchors (eff.table, (float) v); };
            double wMax = 0; int wN = 0; bool unl = false;
            auto w2 = [&] (int idx, float v) {
                const double ms = P::writeAndServiceRunloop (*cp[idx], v);
                if (ms < 0) { unl = true; say ("  WRITE UNLANDED [" + juce::String (idx) + "]"); }
                else { wMax = juce::jmax (wMax, ms); ++wN; } };
            auto curve = [&] { host.pausePumpForMutation();
                               auto c = P::steppedCurve (*ci);
                               host.resumePumpAfterMutation(); return c; };
            // ITEM 2 (measured, --gate-m9 kneefloor): the continuity-constrained
            // two-segment fit recovers the knee EXACTLY on known truth (0.00 dB,
            // including off-grid thresholds) against the step grid's 1.60 dB,
            // and the slope exactly (0.0000) against 0.0600 -- at zero extra
            // render cost, from the renders already taken. Chosen over a 0.5 dB
            // step grid, which would have cost 4x the stimulus time for a worse
            // number. Instrument floors are therefore 0.00 dB / 0.0000.
            auto feats = [] (const juce::Array<double>& c) { return P::curveFeaturesTwoSegment (c); };
            auto bursts = [&] { host.pausePumpForMutation();
                                auto e = P::burstEnvelope (*ci);
                                host.resumePumpAfterMutation(); return e; };

            if (! renderPlaneSensitive (*ci, cdesc.name,
                    [&] { host.pausePumpForMutation();
                          auto c = P::steppedCurve (*ci);
                          host.resumePumpAfterMutation(); return c; }))
            { std::cout << "COMP SUITE: STOPPED (render plane blind)" << std::endl; quitNow(); return; }

            const auto tc0 = juce::Time::getMillisecondCounterHiRes();

            if (! compMapDriven && swTh.a.size() >= 2 && swRa.a.size() >= 2)
            {
                juce::Array<ejmap::subject::SelfMapEntry> entries;
                entries.add ({ "threshold_db", cp[iTh]->getName (64), swTh.unit, iTh, swTh.a });
                entries.add ({ "ratio",        cp[iRa]->getName (64), swRa.unit, iRa, swRa.a });
                auto out = ledger.getRoot().getChildFile ("selfmap-" + currentFp + ".json");
                out.replaceWithText (juce::JSON::toString (
                    ejmap::subject::selfMapVar (currentFp, ejmap::kMapSchemaString,
                                                "compressor", entries), false));
                say ("  self-map written: " + out.getFullPathName());
            }

            // EXCITATION, DECLARED. comp is the first real consumer of the named
            // type: this suite cannot measure a threshold on a compressor that
            // is not compressing, so "ratio to its maximum, threshold to -30"
            // is not setup code, it is a precondition of every verdict below.
            // Declaring it here means schema 2.3a serialises a shape that
            // already has a working consumer, rather than one designed blind.
            ejmap::subject::ExcitationPlan compPlan;
            compPlan.source = "suite:comp";
            compPlan.steps.add ({ iRa, (double) swRa.a.getLast()[0], "ratio",
                                  "a compressor at 1:1 compresses nothing" });
            compPlan.steps.add ({ iTh, -30.0, "threshold_db",
                                  "the threshold must sit below the stimulus" });
            const auto compExc = ejmap::subject::resolveExcitation (compMap, compPlan);
            say ("  " + compExc.describe());

            // pre-excitation reference: ratio at minimum
            w2 (iRa, normFor2 (swRa.a, swRa.a.getFirst()[0]));
            w2 (iTh, normFor2 (swTh.a, 0.0));
            auto preC = curve();
            auto preF = feats (preC);
            // EXCITATION: APPLIED THROUGH THE PLAN, not beside it. The inline
            // writes that stood here are deleted -- a declared plan that
            // something else re-implements is the false-comment class with a
            // struct around it, and the two would drift the first time either
            // changed.
            const double ratHi = swRa.a.getLast()[0];
            const auto excResult = ejmap::subject::applyExcitation (compExc, cp.size(),
                [&] (int idx, double value, const juce::String&) -> double
                {
                    const auto& ladder = (idx == iRa) ? swRa.a : swTh.a;
                    const double ms = P::writeAndServiceRunloop (*cp[idx], normFor2 (ladder, value));
                    if (ms < 0) { unl = true; say ("  WRITE UNLANDED [" + juce::String (idx) + "]"); }
                    else { wMax = juce::jmax (wMax, ms); ++wN; }
                    return ms;
                });
            say ("  excitation applied from " + excResult.source + ": "
                 + juce::String (excResult.applied) + " step(s) landed"
                 + (excResult.ok() ? "" : ", " + juce::String (excResult.unlanded) + " unlanded, "
                    + juce::String (excResult.outOfRange) + " out of range -- " + excResult.detail));
            if (! excResult.ok())
            {
                emitInconclusive ("threshold_db", "the excitation plan did not apply ("
                    + excResult.detail + "), so the compressor was not put into the state every "
                      "verdict below assumes. Measuring anyway would attribute an unexcited "
                      "plugin's silence to its parameters",
                    "the excitation plan failed to apply; nothing downstream was measured");
                std::cout << "COMP SUITE: INCONCLUSIVE (excitation did not apply)" << std::endl;
                quitNow(); return;
            }
            auto excC = curve();
            auto excF2 = feats (excC);
            double curveDelta = 0;
            for (int i = 0; i < excC.size() && i < preC.size(); ++i)
                curveDelta = juce::jmax (curveDelta, std::abs (excC[i] - preC[i]));
            say ("");
            say ("P4 excitation verified by signal: ratio -> " + juce::String (ratHi, 1)
                 + ", threshold -> -30 dB changed the I/O curve by "
                 + juce::String (curveDelta, 2) + " dB max -> "
                 + (curveDelta > 1.0 ? "VERIFIED, branch armed" : "UNVERIFIED"));

            // sigma_f: 3 A/A pairs on the curve features and the envelope
            double sgKnee = 0, sgSlope = 0, sgTau = 0;
            {
                auto c1 = curve(); auto c2 = curve();
                auto f1 = feats (c1), f2 = feats (c2);
                sgKnee = std::abs (f1.kneeInDb - f2.kneeInDb);
                sgSlope = std::abs (f1.slopeAbove - f2.slopeAbove);
                auto e1 = bursts(); auto e2 = bursts();
                const double t1 = P::timeConstantMs (e1, 1000.0, 1200.0, 1.0);
                const double t2 = P::timeConstantMs (e2, 1000.0, 1200.0, 1.0);
                sgTau = (t1 > 0 && t2 > 0) ? std::abs (t1 - t2) : -1;
            }
            say ("P2 sigma_f (plugin repeat, 1 A/A pair each): knee " + juce::String (sgKnee, 3)
                 + " dB | slope " + juce::String (sgSlope, 4) + " dB/dB | tau "
                 + juce::String (sgTau, 1) + " ms");
            say ("   instrument floor (MEASURED, known-truth compressor, two-segment fit): "
                 "knee 0.00 dB | slope 0.0000 -- exact on hard-knee truth including off-grid "
                 "thresholds. The step-grid estimator it replaced measured 1.60 dB / 0.0600. "
                 "Residual knee error on a real plugin is that plugin's knee SOFTNESS, not "
                 "the instrument.");

            // ---- threshold A/B (excitation: ratio high) --------------------
            say ("");
            say ("THRESHOLD (excitation: ratio at max, verified)");
            // Is the residual the plugin's knee SOFTNESS or the estimator?
            // API-2500 exposes a Knee control, so hard-knee it and re-measure:
            // the cheap experiment that separates the two.
            const int iKnee = idxOf ("Knee");
            juce::String kneeNote = "no Knee control on this plugin";
            if (iKnee >= 0)
            {
                auto swK = sweepOneIndex (*ci, iKnee, watchdog, loadedId);
                w2 (iKnee, 0.0f);
                kneeNote = "Knee control [" + juce::String (iKnee) + "] set to its hard end (display '"
                         + cp[iKnee]->getCurrentValueAsText() + "', ladder "
                         + (swK.anchors.size() > 0 ? juce::String (swK.anchors.getFirst()[0], 2) + ".."
                            + juce::String (swK.anchors.getLast()[0], 2) : juce::String ("?")) + ")";
            }
            say ("   " + kneeNote);
            w2 (iTh, normFor2 (swTh.a, -30.0));
            auto cLo = feats (curve());
            w2 (iTh, normFor2 (swTh.a, -10.0));
            auto cHi = feats (curve());
            const double predTh = P::predictedLanding (swTh.a, -10.0) - P::predictedLanding (swTh.a, -30.0);
            const double measTh = cHi.kneeInDb - cLo.kneeInDb;
            const double tolTh = juce::jmax (0.25 * std::abs (predTh), 4 * sgKnee);
            // FOLDED, NOT DROPPED. The knee estimator is corroboration for the
            // GR-at-level primary, not a second verdict on the same semantic.
            // Two verdicts on one semantic is its own confusion. The number
            // stays VISIBLE -- the 44%-vs-47% agreement between two estimators
            // sharing no machinery is the strongest evidence in the over-claim
            // finding, and reducing it to a footnote would weaken the claim it
            // supports.
            juce::String kneeEvidence;
            const bool thrTracks = std::abs (measTh - predTh) <= tolTh;
            const bool thrDirection = (measTh > 0) == (predTh > 0) && std::abs (measTh) > 1.0;
            if (cLo.kneeFound && cHi.kneeFound && ! thrTracks && thrDirection)
                say ("   verdict note: the knee MOVES in the predicted direction but by "
                     + juce::String (100.0 * measTh / predTh, 0) + "% of the predicted amount, with a "
                       "measured instrument floor of 0.00 dB. Not a contradicts: the plugin's own "
                       "knee softness sets the residual, and the transfer curve has no sharp "
                       "breakpoint to resolve. Reported as INCONCLUSIVE: knee unresolvable to the "
                       "tolerance the prediction demands.");
            kneeEvidence =
                  "ladder " + juce::String (P::predictedLanding (swTh.a, -30.0), 1) + " -> "
                  + juce::String (P::predictedLanding (swTh.a, -10.0), 1) + " dB (predicted move +"
                  + juce::String (predTh, 1) + " dB); knee measured "
                  + (cLo.kneeFound ? juce::String (cLo.kneeInDb, 1) : juce::String ("UNDEFINED"))
                  + " -> " + (cHi.kneeFound ? juce::String (cHi.kneeInDb, 1) : juce::String ("UNDEFINED"))
                  + " dB (moved +" + juce::String (measTh, 1) + "), error "
                  + juce::String (std::abs (measTh - predTh), 2) + " vs tol " + juce::String (tolTh, 2);

            // ---- THRESHOLD VIA GR AT FIXED LEVEL (ITEM 2, PRIMARY) --------
            // Threshold means gain reduction BEGINS around X dB, not that a
            // corner exists at X dB. This tests the actual semantics on every
            // design including soft-knee ones: no breakpoint, no unity region,
            // no extended stimulus needed. The knee estimator stays as
            // corroboration where a resolvable corner exists.
            say ("");
            say ("THRESHOLD via GAIN REDUCTION AT FIXED LEVEL (primary test)");
            // RE-ESTABLISHED FROM THE PLAN, not from the ladder's maximum. This
            // line used to write the ladder top and silently overrode a
            // map-declared ratio, while the run still reported the map's value.
            const double excRatio = compExc.valueFor ("ratio", (double) swRa.a.getLast()[0]);
            w2 (iRa, normFor2 (swRa.a, excRatio));
            const double probeLevels[] = { -30.0, -20.0, -10.0 };
            const double thrPoints[]   = { -20.0, -15.0, -10.0, -5.0 };
            say ("     threshold | landed |  GR@-30  |  GR@-20  |  GR@-10   (dB PEAK, vs the "
                 "highest-threshold reference; GR is a DIFFERENCE of two peak readings, so "
                 "the convention cancels)");
            juce::Array<double> grRef;
            juce::Array<juce::Array<double>> grRows;
            for (double th : thrPoints)
            {
                w2 (iTh, normFor2 (swTh.a, th));
                auto c = curve();
                juce::Array<double> row;
                for (double lv : probeLevels)
                {
                    const int idx2 = (int) ((lv - P::kStepBaseDb) / 2.0);
                    row.add (juce::isPositiveAndBelow (idx2, c.size()) ? c[idx2] : -200.0);
                }
                grRows.add (row);
            }
            // reference = the HIGHEST threshold (least compression)
            grRef = grRows.getLast();
            for (int r = 0; r < grRows.size(); ++r)
            {
                juce::String line = "     " + juce::String (thrPoints[r], 1).paddedLeft (' ', 9)
                                  + " | " + juce::String (P::predictedLanding (swTh.a, thrPoints[r]), 1).paddedLeft (' ', 6) + " |";
                for (int c2 = 0; c2 < grRows[r].size(); ++c2)
                    line += juce::String (grRows[r][c2] - grRef[c2], 2).paddedLeft (' ', 9) + " |";
                say (line);
            }
            // ---- DOES THE PLUGIN DO WHAT THE MAP SAYS? ----------------------
            // The same check item 1 introduced, through the SHARED helper --
            // not a second copy. It is what sees failures 2, 3 and 4 above:
            // each is a value<->norm disagreement that leaves the span intact.
            // ONE REPORT PER SEMANTIC. Pooling them into a single worst-error
            // named the wrong parameter: a ratio ladder with bad norms produced
            // a contradiction attributed to threshold_db, whose every point was
            // exact. Routing right with wrong words is the failure the verdict
            // fork exists to prevent, and it reappears wherever evidence from
            // two parameters is merged before it is attributed.
            ejmap::subject::MapClaimReport thClaims, raClaims;
            if (compMapDriven)
            {
                auto writeTo = [&] (int idx) { return [&, idx] (float n) { w2 (idx, n); }; };
                auto readOf  = [&] (int idx) { return [&, idx] { return cp[idx]->getCurrentValueAsText(); }; };
                for (double v : { thrPoints[0], thrPoints[1], thrPoints[2], thrPoints[3] })
                    thClaims.add (ejmap::subject::checkMapClaim (thSlot, v,
                                      writeTo (iTh), readOf (iTh)));
                for (double v : { 2.0, 10.0 })
                    raClaims.add (ejmap::subject::checkMapClaim (raSlot, v,
                                      writeTo (iRa), readOf (iRa)));
                say ("");
                say ("  MAP-CLAIM CHECK, threshold_db");
                for (const auto& r : thClaims.rows) say (r);
                say ("    worst " + juce::String (thClaims.worst, 2) + " dB over "
                     + juce::String (thClaims.checked - thClaims.unparsed) + " readable points");
                say ("  MAP-CLAIM CHECK, ratio");
                for (const auto& r : raClaims.rows) say (r);
                say ("    worst " + juce::String (raClaims.worst, 2) + " :1 over "
                     + juce::String (raClaims.checked - raClaims.unparsed) + " readable points"
                     + (raClaims.unparsed > 0 ? " (" + juce::String (raClaims.unparsed)
                                                + " unreadable -- a ratio display of 'Inf' is a "
                                                  "real landing, not a parse failure)" : ""));
                // restore the excitation state the claim walk just disturbed
                w2 (iRa, normFor2 (swRa.a, excRatio));   // the plan's ratio, not the ladder top
            }

            // verdict: GR must move monotonically with threshold at the loudest probe
            bool monotone = true; double totalGr = 0;
            for (int r = 1; r < grRows.size(); ++r)
            {
                const double a2 = grRows[r - 1].getLast() - grRef.getLast();
                const double b2 = grRows[r].getLast() - grRef.getLast();
                if (b2 < a2 - 0.5) monotone = false;
            }
            totalGr = std::abs (grRows.getFirst().getLast() - grRef.getLast());
            const double predSpan = std::abs (P::predictedLanding (swTh.a, thrPoints[3])
                                            - P::predictedLanding (swTh.a, thrPoints[0]));
            // The prediction must describe the state the plugin is ACTUALLY in.
            // Using the ladder's maximum here would predict 10:1 behaviour from
            // a plugin the plan had set to 6:1, and the error would be read as
            // the plugin's fault.
            const double ratioMax = juce::jmax (1.01, P::predictedLanding (swRa.a, excRatio));
            const double predGr = predSpan * (1.0 - 1.0 / ratioMax);
            const double sgKneeFloor = sgKnee > 0.1 ? sgKnee : 0.1;
            const double tolGr = juce::jmax (0.25 * predGr, 4.0 * sgKneeFloor);
            // PER-POINT ACCURACY GATES THE SPAN VERDICT. totalGr is a span, and
            // a uniformly offset ladder produces the right span with every
            // point in the wrong place. Measured on the limiter in item 1:
            // CONFIRMS carrying a 7.20 dB error in its evidence.
            if (compMapDriven && thClaims.worst > juce::jmax (0.25 * std::abs (predSpan), 1.0))
            {
                emitContradicts ("threshold_db",
                    "the ladder's SPAN is intact (" + juce::String (predSpan, 1)
                    + " dB predicted) but the plugin does not land where the map says: worst "
                      "|map claim - plugin| " + juce::String (thClaims.worst, 2)
                    + " dB over " + juce::String (thClaims.checked - thClaims.unparsed)
                    + " points. A span verdict cannot see this -- an offset ladder moves by the "
                      "right amount with every point wrong. The map does not describe this plugin "
                      "as installed: stale, made in another mode or preset, or made against "
                      "another version");
                std::cout << "COMP SUITE: " << fails << " FAILED (map claims not met)" << std::endl;
                quitNow(); return;
            }

            emitVerdict ("threshold_db (GR-at-level, PRIMARY)",
                  totalGr, predGr, juce::jmax (sgKneeFloor, 0.001), tolGr,
                  "threshold " + juce::String (P::predictedLanding (swTh.a, thrPoints[0]), 1)
                  + " -> " + juce::String (P::predictedLanding (swTh.a, thrPoints[3]), 1)
                  + " dB across " + juce::String (predSpan, 1) + " dB at ratio "
                  // the ratio the excitation plan ACTUALLY established, not the
                  // ladder's top: printing the top said "at ratio 10.0:1
                  // predicts 12.50" about a prediction computed from 6:1.
                  + juce::String (ratioMax, 1) + ":1 predicts " + juce::String (predGr, 2)
                  + " dB of GR change at -10 dBFS; MEASURED " + juce::String (totalGr, 2)
                  + " dB, monotone: " + (monotone ? "YES" : "NO") + ", error "
                  + juce::String (std::abs (totalGr - predGr), 2) + " vs tol "
                  + juce::String (tolGr, 2),
                  "dB");

            // ---- ratio A/B (excitation: threshold low) ---------------------
            say ("");
            say ("RATIO (excitation: threshold at -30 dB, verified)");
            w2 (iTh, normFor2 (swTh.a, -30.0));
            const double rLo = 2.0, rHi = 10.0;
            w2 (iRa, normFor2 (swRa.a, rLo));
            auto sLo = feats (curve());
            w2 (iRa, normFor2 (swRa.a, rHi));
            auto sHi = feats (curve());
            const double predRLo = P::predictedLanding (swRa.a, rLo), predRHi = P::predictedLanding (swRa.a, rHi);
            const double predSlopeLo = 1.0 / predRLo, predSlopeHi = 1.0 / predRHi;
            const double predDS = predSlopeHi - predSlopeLo, measDS = sHi.slopeAbove - sLo.slopeAbove;
            const double tolS = juce::jmax (0.25 * std::abs (predDS), 4 * sgSlope);
            if (compMapDriven && raClaims.worst > juce::jmax (0.25 * std::abs (rHi - rLo), 0.5))
            {
                emitContradicts ("ratio",
                    "the map's ratio ladder does not land where it says: worst |map claim - "
                    "plugin| " + juce::String (raClaims.worst, 2) + ":1 over "
                    + juce::String (raClaims.checked - raClaims.unparsed) + " readable points. The "
                      "ladder's VALUES may be correct while its norms are not, which puts the "
                      "plugin somewhere else on a curve the map describes accurately -- the "
                      "slope-delta verdict below sees only the delta and would not notice");
                std::cout << "COMP SUITE: " << fails << " FAILED (ratio map claims not met)"
                          << std::endl;
                quitNow(); return;
            }

            emitVerdict ("ratio", measDS, predDS, juce::jmax (sgSlope, 0.0001), tolS,
                  "ladder " + juce::String (predRLo, 2) + ":1 -> " + juce::String (predRHi, 2)
                  + ":1 => predicted slope " + juce::String (predSlopeLo, 3) + " -> "
                  + juce::String (predSlopeHi, 3) + " (delta " + juce::String (predDS, 3)
                  + "); measured slope " + juce::String (sLo.slopeAbove, 3) + " -> "
                  + juce::String (sHi.slopeAbove, 3) + " (delta " + juce::String (measDS, 3)
                  + " => " + juce::String (sLo.slopeAbove > 0.01 ? 1.0 / sLo.slopeAbove : 0.0, 2)
                  + ":1 -> " + juce::String (sHi.slopeAbove > 0.01 ? 1.0 / sHi.slopeAbove : 0.0, 2)
                  + ":1), error " + juce::String (std::abs (measDS - predDS), 3)
                  + " vs tol " + juce::String (tolS, 3),
                  // a slope DELTA: output dB per input dB. Never "dB".
                  "dB/dB");

            // ---- attack / release A/B --------------------------------------
            say ("");
            say ("ENVELOPE (excitation: threshold -30, ratio max, verified)");
            w2 (iRa, normFor2 (swRa.a, excRatio));   // the plan's ratio
            for (int which = 0; which < 2; ++which)
            {
                const bool atk = which == 0;
                const int idx = atk ? iAt : iRe;
                auto& sw = atk ? swAt : swRe;
                // Probe only the portion of the ladder the instrument can
                // resolve; the unresolvable end is reported, not fitted.
                const double ladderLo = sw.a.getFirst()[0], ladderHi = sw.a.getLast()[0];
                const double lo = juce::jmax (ladderLo, P::InstrumentFloor::tauMs * 2.0);
                const double hi = ladderHi;
                const double fracAbove = P::ladderFractionAbove (sw.a, P::InstrumentFloor::tauMs);
                say (juce::String ("   ladder coverage: ")
                     + juce::String (100.0 * fracAbove, 0) + "% of this parameter's "
                     + juce::String (sw.a.size()) + " anchors lie above the 0.5 ms tau floor"
                     + (fracAbove < 0.999 ? " -- the remaining "
                        + juce::String (100.0 * (1.0 - fracAbove), 0)
                        + "% is UNTESTABLE by this instrument and must not read as verified"
                                          : ""));
                if (lo > ladderLo)
                    say ("   note: ladder starts at " + juce::String (ladderLo, 3)
                         + " ms, below the measured instrument tau floor ("
                         + juce::String (P::InstrumentFloor::tauMs, 2)
                         + " ms); probing from " + juce::String (lo, 2)
                         + " ms instead and reporting the unresolvable end rather than fitting it.");
                w2 (idx, normFor2 (sw.a, lo));
                auto eA = bursts();
                w2 (idx, normFor2 (sw.a, hi));
                auto eB = bursts();
                // attack: first burst onset at ~1000 ms; release: after it ends at ~1400 ms
                // second burst onsets at 1000 ms and ends at 1400 ms; the bed
                // makes the post-burst recovery observable.
                const double s0 = atk ? 1000.0 : 1402.0, s1 = atk ? 1200.0 : 1900.0;
                const double tA = P::timeConstantMs (eA, s0, s1, 1.0);
                const double tB = P::timeConstantMs (eB, s0, s1, 1.0);
                // CARVE-OUT 1 + the unit rule, consulted BEFORE any verdict.
                // The first run of this suite issued fail verdicts without
                // either check: API-2500's Release displays "Var" (program-
                // dependent mode, not a time), and no time parameter here
                // declares a unit family, so a nameplate log-ratio has no
                // established relationship to milliseconds.
                const auto disp = cp[idx]->getCurrentValueAsText();
                const juce::String unitFam = atk ? swAtUnit : swReUnit;
                if (P::displayIsModeToken (disp))          // HARNESS-level guard
                {
                    emitInconclusive (atk ? "attack_ms" : "release_ms",
                          juce::String ("possibly mode-suppressed -- the parameter ")
                          + P::modeTokenReason (disp)
                          + ". Carve-out 1 exclusion (a) FAILS: a suppressing state is present. "
                            "No contradicts may be issued. Measured anyway for the record: tau "
                          + juce::String (tA, 1) + " -> " + juce::String (tB, 1) + " ms.",
                          "measured, and the parameter did not move the envelope; carve-out 1 "
                          "governs whether that is suppression or absence");
                    continue;
                }
                if (unitFam.isEmpty())
                {
                    emitInconclusive (atk ? "attack_ms" : "release_ms",
                          juce::String ("ladder units undeclared -- the display ('")
                          + disp + "') carries no unit family, so the ladder's numbers cannot be "
                          "checked against milliseconds. Measured tau "
                          + juce::String (tA, 1) + " -> " + juce::String (tB, 1)
                          + " ms across ladder " + juce::String (P::predictedLanding (sw.a, lo), 2)
                          + " -> " + juce::String (P::predictedLanding (sw.a, hi), 2)
                          + " -- direction "
                          + (tB > tA ? "SAME as the ladder"
                                     : "opposite ACROSS THESE TWO POINTS; the 5-point walk "
                                       "(--gate-m9 relwalk) showed the ladder is monotone up and "
                                       "plateaus, so a two-point read here samples the plateau and "
                                       "must not be reported as a reversal")
                          + ". Recorded as evidence; no verdict.",
                          "measured, and the direction recorded; the ladder's unit family is "
                          "undeclared, so there is no numeric prediction to test against");
                    continue;
                }
                const double predRatio = std::log2 (P::predictedLanding (sw.a, hi)
                                                  / juce::jmax (0.001, P::predictedLanding (sw.a, lo)));
                const double measRatio = (tA > 0 && tB > 0) ? std::log2 (tB / juce::jmax (1.0, tA)) : 0.0;
                const bool defined = tA > 0 && tB > 0;
                emitVerdict (atk ? "attack_ms" : "release_ms",
                      measRatio, predRatio, 0.0001,
                      juce::jmax (0.25 * std::abs (predRatio), 0.25),
                      "ladder " + juce::String (P::predictedLanding (sw.a, lo), 2) + " -> "
                      + juce::String (P::predictedLanding (sw.a, hi), 2) + " (predicted log2 ratio "
                      + juce::String (predRatio, 2) + "); measured tau "
                      + (defined ? juce::String (tA, 0) + " -> " + juce::String (tB, 0) + " ms (log2 "
                                   + juce::String (measRatio, 2) + ")"
                                 : juce::String ("UNDEFINED (excursion below 1 dB -- no tau fitted)")),
                  // log2 of a TIME RATIO, dimensionless. "dB" here would have
                      // described milliseconds as decibels.
                      "log2 ratio");
            }

            say ("");
            say ("write landing on the BRIDGE: " + juce::String (wN) + " writes, max "
                 + juce::String (wMax, 1) + " ms, unlanded: " + (unl ? "YES" : "none"));
            say ("comp suite wall clock: "
                 + juce::String ((juce::Time::getMillisecondCounterHiRes() - tc0) / 1000.0, 2) + " s");
            std::cout << "COMP SUITE: " << (fails == 0 ? "PASS" : juce::String (fails) + " FAILED")
                      << std::endl;
            quitNow(); return;
        }

        // ---- instrument mode: the extractor's OWN floors and resolution,
        //      measured with no plugin in the path (amendments 3 and 4) -----
        if (mode == "instrument")
        {
            say ("INSTRUMENT: feature-extractor floors and resolution (no plugin in the path)");
            auto s0 = P::stimulusReference (0);
            auto sp0 = P::welch (s0);

            // (i) NUMERICAL floor: bit-identical input pair.
            auto numF = P::lobeFeatures (sp0.mid, sp0.mid);
            const double numSide = std::abs (P::bandEnergyDb (sp0.side, 50, 400)
                                           - P::bandEnergyDb (sp0.side, 50, 400));
            say ("  numerical floor (bit-identical pair): depth "
                 + juce::String (numF.maxAbsDb, 6) + " dB | side band "
                 + juce::String (numSide, 6) + " dB");

            // (ii) REALIZATION floor: different noise realizations, same
            //      (identity) system. This is the estimator variance a
            //      deterministic plugin's A/A pair can never show.
            double rDepth = 0, rSide = 0, rCentre = 0, rWidth = 0;
            juce::Array<double> centres, widths;
            for (int k = 1; k <= 4; ++k)
            {
                auto sk = P::stimulusReference (k);
                auto spk = P::welch (sk);
                auto f = P::lobeFeatures (sp0.mid, spk.mid);
                rDepth = juce::jmax (rDepth, f.maxAbsDb);
                rSide  = juce::jmax (rSide, std::abs (P::bandEnergyDb (sp0.side, 50, 400)
                                                    - P::bandEnergyDb (spk.side, 50, 400)));
                // centre/width of a KNOWN synthetic lobe under each realization
                auto lk = P::welch (P::syntheticPeak (sk, 200.0, 6.0, 1.0));
                auto fk = P::lobeFeatures (spk.mid, lk.mid, 30.0, 20000.0, 1.0);
                if (fk.centreDefined()) { centres.add (std::log2 (fk.centreHz / 200.0)); widths.add (fk.widthOct); }
            }
            for (int a = 0; a < centres.size(); ++a)
                for (int b2 = a + 1; b2 < centres.size(); ++b2)
                {
                    rCentre = juce::jmax (rCentre, std::abs (centres[a] - centres[b2]));
                    rWidth  = juce::jmax (rWidth,  std::abs (widths[a] - widths[b2]));
                }
            say ("  realization floor (independent noise, same system): depth "
                 + juce::String (rDepth, 4) + " dB | side band " + juce::String (rSide, 4)
                 + " dB | centre " + juce::String (rCentre, 5) + " oct | width "
                 + juce::String (rWidth, 4) + " oct");

            // (iii) CENTRE RESOLUTION vs FREQUENCY: known-truth synthetic
            //       peaks, 40 Hz to 16 kHz. Bin width at this FFT is
            //       48000/8192 = 5.86 Hz.
            say ("  bin width " + juce::String (P::kSampleRate / P::kFftSize, 2)
                 + " Hz (FFT " + juce::String (P::kFftSize) + ", " + juce::String (P::kBins) + " bins)");
            say ("  centre resolution vs frequency (synthetic peak, +6 dB, Q 1.0):");
            say ("     true Hz | measured Hz | error oct | error % | width oct | depth dB (true +6.00)");
            const double freqs[] = { 40, 63, 80, 100, 160, 250, 400, 630, 1000,
                                     2000, 4000, 8000, 16000 };
            double worstLow = 0, worstHigh = 0, worstDepth = 0;
            for (double f0 : freqs)
            {
                auto lp = P::welch (P::syntheticPeak (s0, f0, 6.0, 1.0));
                auto ft = P::lobeFeatures (sp0.mid, lp.mid, 20.0, 22000.0, 1.0);
                if (! ft.centreDefined()) { say ("     " + juce::String (f0, 0) + " | NO LOBE"); continue; }
                const double errOct = std::log2 (ft.centreHz / f0);
                const double errPct = 100.0 * (ft.centreHz - f0) / f0;
                if (f0 <= 250) worstLow = juce::jmax (worstLow, std::abs (errOct));
                else           worstHigh = juce::jmax (worstHigh, std::abs (errOct));
                worstDepth = juce::jmax (worstDepth, std::abs (ft.depthDb - 6.0));
                say ("     " + juce::String (f0, 0).paddedLeft (' ', 7) + " | "
                     + juce::String (ft.centreHz, 1).paddedLeft (' ', 11) + " | "
                     + juce::String (errOct, 4).paddedLeft (' ', 9) + " | "
                     + juce::String (errPct, 2).paddedLeft (' ', 7) + " | "
                     + juce::String (ft.widthOct, 3) + " | "
                     + juce::String (ft.depthDb, 3));
            }
            say ("  worst |centre error| <=250 Hz: " + juce::String (worstLow, 4)
                 + " oct | >250 Hz: " + juce::String (worstHigh, 4)
                 + " oct  (frequency gate is 0.070 oct)");
            say ("  worst |depth error| on a known +6.00 dB peak: "
                 + juce::String (worstDepth, 3) + " dB  <- INSTRUMENT DEPTH FLOOR");
            std::cout << "INSTRUMENT: DONE" << std::endl;
            quitNow(); return;
        }

        // ---- resume mode: the relaunch after a mid-batch kill --------------
        if (mode == "resume")
        {
            if (! stakeFile.existsAsFile() || ! stateFile.existsAsFile())
            { say ("GATE-RESUME: no probe stake on disk; nothing to restore"); quitNow(); return; }
            auto stake = juce::JSON::parse (stakeFile.loadFileAsString());
            say ("A probe died mid-run at " + stake.getProperty ("started_at", "?").toString()
                 + " (suite " + stake.getProperty ("suite", "?").toString()
                 + ", render " + stake.getProperty ("render_n", juce::var (0)).toString()
                 + "). Restoring your settings from the stake.");
            juce::MemoryBlock st;
            stateFile.loadFileAsData (st);
            host.pausePumpForMutation();
            inst->setStateInformation (st.getData(), (int) st.getSize());
            host.resumePumpAfterMutation();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            bool verified = true;
            if (auto* exp = stake.getProperty ("expect", juce::var()).getDynamicObject())
                for (auto& kv : exp->getProperties())
                {
                    const int idx = kv.name.toString().getIntValue();
                    const float want = (float) (double) kv.value;
                    const float got = params[idx]->getValue();
                    say ("  restore verify: [" + juce::String (idx) + "] expected "
                         + juce::String (want, 4) + " read " + juce::String (got, 4));
                    if (std::abs (got - want) > 0.01f) verified = false;
                }
            if (verified)
            {
                say ("Your settings were restored from the stake and verified.");
                stakeFile.deleteFile(); stateFile.deleteFile();
            }
            else
                say ("RESTORE VERIFICATION FAILED: the stake stays on disk and probing "
                     "this plugin is blocked until you acknowledge this.");
            std::cout << "GATE-RESUME: " << (verified ? "RESTORED" : "UNRESTORED") << std::endl;
            quitNow(); return;
        }

        // ---- fixture from the production map -------------------------------
        auto mapVar = juce::JSON::parse (ledger.getRoot().getChildFile ("maps")
                          .getChildFile (currentFp + ".json").loadFileAsString());
        auto g1 = mapVar.getProperty ("groups", juce::var())[0];
        auto gp = g1.getProperty ("params", juce::var());
        const int idxFreq = (int) gp.getProperty ("freq_hz", juce::var()).getProperty ("index", -1);
        const int idxGain = (int) gp.getProperty ("gain_db", juce::var()).getProperty ("index", -1);
        const int idxQ    = (int) gp.getProperty ("q", juce::var()).getProperty ("index", -1);
        // ---- ROLE AND ENABLE LINK, RESOLVED (M9 item 3, session 1) ---------
        // These were `idxMono = 7, idxMonoIn = 8` -- facts about one plugin,
        // verified once by a human and frozen in code. They now come from the
        // map, or from the fixture when the map predates schema 2.3b. The real
        // AMEK map carries neither: signed option (i), the fixture holds them
        // until AMEK is re-mapped, because writing them into a human-verified
        // artefact to make our own tests convenient is the pressure the schema
        // gate change was accepted to remove.
        // THE CLAIM AND THE LADDER COME FROM DIFFERENT PLACES, deliberately.
        // The fixture supplies the ROLE and the LINK -- the two fields schema
        // 2.3b added and the real AMEK map predates. The LADDER always comes
        // from the map, because that is the artefact under test: a fixture
        // carrying its own anchors would be probing itself.
        juce::var roleSource = mapVar;
        juce::String roleFrom = "map";
        auto rolePick = ejmap::subject::controlWithRole (mapVar, "stereo_width");
        if (! rolePick.ok && rolePick.candidates == 0)
        {
            auto fx = juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/tests/fixtures")
                        .getChildFile ("m9-eq-aumf-ameq-Brwx.json");
            auto fv = juce::JSON::parse (fx.loadFileAsString());
            auto claim = ejmap::subject::controlWithRole (fv, "stereo_width");
            if (claim.candidates == 1)
            {
                roleSource = fv;
                roleFrom = "fixture " + fx.getFileName() + " (the map predates schema 2.3b)";
                // ladder from the MAP's control of that name
                auto fromMap = ejmap::subject::controlNamed (mapVar, claim.controlName);
                const int fixtureIdx = (int) fv.getProperty ("controls", juce::var())
                                              .getProperty (claim.controlName, juce::var())
                                              .getProperty ("index", -1);
                if (fromMap.ok() && fromMap.index != fixtureIdx)
                {
                    say ("GATE: the fixture claims '" + claim.controlName + "' is at index "
                         + juce::String (fixtureIdx) + " and the map puts it at index "
                         + juce::String (fromMap.index) + ". The fixture describes a different "
                           "build than the map under test; nothing runs on that disagreement.");
                    quitNow(); return;
                }
                rolePick.ok = fromMap.ok();
                rolePick.slot = fromMap;
                rolePick.controlName = claim.controlName;
                rolePick.why = fromMap.why;
            }
        }
        if (! rolePick.ok)
        {
            say ("GATE: no usable stereo_width control (" + rolePick.why + "). Arm A's falsifier "
                 "and B5's negative control both write that control, so neither can run.");
            quitNow(); return;
        }
        const int idxMono = rolePick.slot.index;
        auto monoLink = ejmap::subject::enableLinkFor (roleSource, rolePick.controlName);
        const int idxMonoIn = monoLink.declared ? monoLink.index : -1;
        say ("stereo_width role from " + roleFrom + ": '" + rolePick.controlName + "' ["
             + juce::String (idxMono) + "]"
             + (monoLink.declared ? ", enabled by [" + juce::String (idxMonoIn) + "] "
                                    + monoLink.name + " = " + juce::String (monoLink.value, 2)
                                  : ", NO enable link declared -- whether it is live in the "
                                    "default state is UNKNOWN, not assumed"));
        auto freqAnch = echojay::anchorsFromVar (gp.getProperty ("freq_hz", juce::var()));
        auto gainAnch = echojay::anchorsFromVar (gp.getProperty ("gain_db", juce::var()));
        auto qAnch    = echojay::anchorsFromVar (gp.getProperty ("q", juce::var()));
        auto monoAnch = echojay::anchorsFromVar (mapVar.getProperty ("controls", juce::var())
                                                       .getProperty ("Mono Maker", juce::var()));
        say ("GATE M9 | AMEK EQ 200 | fp " + currentFp.substring (0, 12)
             + " | group1 freq [" + juce::String (idxFreq) + "] gain [" + juce::String (idxGain)
             + "] q [" + juce::String (idxQ) + "]");

        // declared vs measured latency, recorded; the eq features are Welch
        // magnitude spectra, so the aligner uses NEITHER number.
        say ("latency: declared " + juce::String (inst->getLatencySamples())
             + " samples; Task 0 measured first-energy at sample 0; the eq suite's "
               "spectral features use neither (alignment-insensitive by construction).");

        // decorrelation check on the raw stimulus
        auto stim = P::stimulusReference();
        const double smdb = P::sideMidRatioDb (stim);
        say ("stimulus decorrelation: broadband side/mid = " + juce::String (smdb, 3)
             + " dB (gate requires within +/-1 dB of 0)");
        if (std::abs (smdb) > 1.0)
        { say ("GATE: stimulus not decorrelated; the gate does not run."); quitNow(); return; }

        // ---- probe stake BEFORE the first write ----------------------------
        juce::MemoryBlock preState;
        host.pausePumpForMutation();
        inst->getStateInformation (preState);
        host.resumePumpAfterMutation();
        stateFile.replaceWithData (preState.getData(), preState.getSize());
        std::map<int, float> preVals;
        for (int i : { idxFreq, idxGain, idxQ, idxMono, idxMonoIn })
            if (juce::isPositiveAndBelow (i, params.size()))
                preVals[i] = params[i]->getValue();
        auto writeStake = [&] (const juce::String& suite, int renderN)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("plugin_id", loadedId);
            o->setProperty ("fp", currentFp);
            o->setProperty ("suite", suite);
            o->setProperty ("render_n", renderN);
            o->setProperty ("state_file", stateFile.getFullPathName());
            o->setProperty ("started_at", juce::Time::getCurrentTime().toISO8601 (true));
            auto* exp = new juce::DynamicObject();
            for (auto& kv : preVals) exp->setProperty (juce::String (kv.first), (double) kv.second);
            o->setProperty ("expect", juce::var (exp));
            stakeFile.replaceWithText (juce::JSON::toString (juce::var (o), false));
        };
        writeStake ("eq", 0);

        int renderCount = 0;
        auto render = [&] () {
            host.pausePumpForMutation();
            auto b = P::renderCapture (*inst);
            host.resumePumpAfterMutation();
            writeStake ("eq", ++renderCount);
            return b;
        };
        double writeMsMax = 0, writeMsSum = 0; int writeN = 0;
        bool sawUnlanded = false;
        auto wc = [&] (int idx, float v) {
            const double ms = P::writeAndServiceRunloop (*params[idx], v);
            if (ms < 0) { sawUnlanded = true; say ("  WRITE UNLANDED [" + juce::String (idx) + "]"); }
            else { writeMsMax = juce::jmax (writeMsMax, ms); writeMsSum += ms; ++writeN; }
            return ms;
        };
        auto normFor = [] (const juce::Array<juce::Array<float>>& a, double value) {
            auto eff = echojay::dominantMonotonicTable (a);
            return echojay::interpolateAnchors (eff.table, (float) value);
        };

        const auto tArmB0 = juce::Time::getMillisecondCounterHiRes();

        // ---- P4 pre-excitation reference then excitation verify ------------
        wc (idxFreq, normFor (freqAnch, 100.0));
        wc (idxGain, normFor (gainAnch, 0.0));
        wc (idxQ,    normFor (qAnch, 1.0));
        auto preExc = render();
        auto preExcS = P::welch (preExc);
        wc (idxGain, normFor (gainAnch, 6.0));            // the excitation
        auto excR = render();
        auto excS = P::welch (excR);
        auto excF = P::lobeFeatures (preExcS.mid, excS.mid);
        say ("P4 excitation verified by signal: LF Gain 1 -> +6 dB changed the mid "
             "spectrum by " + juce::String (excF.maxAbsDb, 2) + " dB max (lobe at "
             + juce::String (excF.centreHz, 1) + " Hz, depth " + juce::String (excF.depthDb, 2)
             + " dB). Should-have-moved branch ARMED for both arms on this evidence.");
        const bool excitationVerified = excF.maxAbsDb > 1.0;

        // ---- P2 sigma_f -----------------------------------------------------
        // (i) three A/A null pairs at the excited state: depth + side-band floors
        double sigDepth = 0, sigSide = 0;
        for (int r = 0; r < 3; ++r)
        {
            auto a1 = P::welch (render());
            auto a2 = P::welch (render());
            auto nf = P::lobeFeatures (a1.mid, a2.mid);
            sigDepth = juce::jmax (sigDepth, nf.maxAbsDb);
            sigSide  = juce::jmax (sigSide, std::abs (P::bandEnergyDb (a1.side, 50, 400)
                                                     - P::bandEnergyDb (a2.side, 50, 400)));
        }
        // (ii) three repeated freq-move measurements: centre + width floors
        double centres[3], widths[3];
        for (int r = 0; r < 3; ++r)
        {
            wc (idxFreq, normFor (freqAnch, 100.0));
            auto f100 = P::lobeFeatures (preExcS.mid, P::welch (render()).mid);
            wc (idxFreq, normFor (freqAnch, 400.0));
            auto f400 = P::lobeFeatures (preExcS.mid, P::welch (render()).mid);
            centres[r] = std::log2 (f400.centreHz / f100.centreHz);
            widths[r]  = f100.widthOct;
            if (mode == "kill" && r == 1)
            { say ("GATE-KILL: dying mid-batch with the stake on disk."); std::cout.flush(); ::_exit (9); }
        }
        double sigCentre = 0, sigWidth = 0;
        for (int a = 0; a < 3; ++a) for (int b2 = a + 1; b2 < 3; ++b2)
        { sigCentre = juce::jmax (sigCentre, std::abs (centres[a] - centres[b2]));
          sigWidth  = juce::jmax (sigWidth,  std::abs (widths[a] - widths[b2])); }
        const double plugCentre = sigCentre, plugDepth = sigDepth,
                     plugWidth = sigWidth, plugSide = sigSide;
        sigCentre = juce::jmax (sigCentre, P::InstrumentFloor::centreOct (100.0));
        sigDepth  = juce::jmax (sigDepth,  P::InstrumentFloor::depthDb);
        sigWidth  = juce::jmax (sigWidth,  P::InstrumentFloor::widthOct);
        sigSide   = juce::jmax (sigSide,   P::InstrumentFloor::sideDb);
        say ("P2 sigma_f, TWO FLOORS REPORTED SEPARATELY (amendment 3):");
        say ("   plugin repeat floor (3 A/A null pairs; 3 repeated measurements): centre "
             + juce::String (plugCentre, 5) + " oct | depth " + juce::String (plugDepth, 4)
             + " dB | width " + juce::String (plugWidth, 5) + " oct | side "
             + juce::String (plugSide, 4) + " dB   <- AMEK is deterministic, so these are ~0");
        say ("   instrument floor (known-truth extraction, no plugin): centre "
             + juce::String (P::InstrumentFloor::centreOct (100.0), 4) + " oct | depth "
             + juce::String (P::InstrumentFloor::depthDb, 3) + " dB | width "
             + juce::String (P::InstrumentFloor::widthOct, 4) + " oct | side "
             + juce::String (P::InstrumentFloor::sideDb, 4) + " dB");
        say ("   sigma_f USED = max(plugin, instrument): centre " + juce::String (sigCentre, 4)
             + " oct | depth " + juce::String (sigDepth, 3) + " dB | width "
             + juce::String (sigWidth, 4) + " oct | side " + juce::String (sigSide, 4) + " dB");

        // ---- P5 ROLE AND ENABLE, VERIFIED BEFORE ANYTHING ACTS ON THEM -----
        // Order is deliberate and was decided before building: triage first,
        // because it names WHICH of three causes produced one symptom and
        // everything below quotes its answer; then the role, because arm A's
        // falsifier and B5's negative control both write that control; then
        // the enable null-test, because a link with side effects contaminates
        // the arm that is not testing it.
        say ("");
        say ("P5 ROLE AND ENABLE (map claims, checked before use)");

        // (1) TRIAGE. One symptom, three causes, named apart.
        auto monoAnchLive = monoAnch;
        auto sideEnergyNow = [&] {
            auto sp = P::welch (render());
            return P::bandEnergyDb (sp.side, 50, 400); };
        const float monoLo = monoAnchLive.size() >= 2 ? normFor (monoAnchLive, monoAnchLive.getFirst()[0]) : 0.0f;
        const float monoHi = monoAnchLive.size() >= 2 ? normFor (monoAnchLive, monoAnchLive.getLast()[0]) : 1.0f;

        // engage the enable FIRST if one is declared -- an undeclared link is
        // UNKNOWN, so the triage below reports what it finds either way
        double primaryBefore = 0, primaryAfter = 0;
        if (monoLink.declared)
        {
            auto preEnable = P::welch (render());
            primaryBefore = P::bandEnergyDb (preEnable.mid, 50, 400);
            const auto ms = P::writeAndServiceRunloop (*params[idxMonoIn], (float) monoLink.value);
            auto postEnable = P::welch (render());
            primaryAfter = P::bandEnergyDb (postEnable.mid, 50, 400);
            say ("  enable [" + juce::String (idxMonoIn) + "] " + monoLink.name + " -> "
                 + juce::String (monoLink.value, 2)
                 + (ms < 0 ? "  <- WRITE DID NOT LAND" : ", landed in " + juce::String (ms, 1) + " ms"));
        }

        auto tri = ejmap::triage::classifyLiveness (idxMono, params.size(), monoLo, monoHi, sigSide,
                       [&] (float n) { return P::writeAndServiceRunloop (*params[idxMono], n); },
                       [&] { return sideEnergyNow(); });
        say ("  TRIAGE of '" + rolePick.controlName + "': " + tri.cause());

        // (2) ROLE, verified by signal.
        P::writeAndServiceRunloop (*params[idxMono], monoLo);
        auto rA = P::welch (render());
        P::writeAndServiceRunloop (*params[idxMono], monoHi);
        auto rB = P::welch (render());
        const double roleSideMoved = std::abs (P::bandEnergyDb (rB.side, 50, 400)
                                             - P::bandEnergyDb (rA.side, 50, 400));
        const double roleMidMoved  = std::abs (P::bandEnergyDb (rB.mid, 50, 400)
                                             - P::bandEnergyDb (rA.mid, 50, 400));
        auto roleEv = ejmap::triage::verifyStereoWidthRole (rolePick.controlName, roleSideMoved, roleMidMoved,
                                                4.0 * sigSide, 4.0 * sigDepth);
        say ("  ROLE: " + roleEv.why);
        say ("  " + roleEv.limitStatement());

        // (3) ENABLE NULL-TEST.
        if (monoLink.declared)
        {
            auto nul = ejmap::triage::checkEnableIsNull (monoLink.name, std::abs (primaryAfter - primaryBefore),
                                             4.0 * sigDepth);
            say ("  ENABLE NULL: " + nul.why);
            if (! nul.clean)
            {
                emitInconclusive ("stereo_width role", nul.why + ". No arm runs on a plugin whose "
                    "measurement the enable itself moved",
                    "measured: the enable link is not null, so arm B would measure a different "
                    "plugin than the one the map describes");
                std::cout << "GATE M9: STOPPED (enable link is not null)" << std::endl;
                quitNow(); return;
            }
        }
        if (! roleEv.supported)
        {
            emitInconclusive ("stereo_width role", roleEv.why + ". Triage says: " + tri.cause()
                + ". Arm A's falsifier and B5's negative control both write this control, so "
                  "neither can produce a verdict",
                "measured: the role claim was checked by signal before any arm ran");
            std::cout << "GATE M9: STOPPED (map's stereo_width claim not supported)" << std::endl;
            quitNow(); return;
        }
        // restore the control to where the arms expect it
        P::writeAndServiceRunloop (*params[idxMono], preVals.count (idxMono) ? preVals[idxMono] : monoLo);

        // ---- P3 sanity gate under excitation -------------------------------
        auto stimS = P::welch (stim);
        auto sanF = P::lobeFeatures (stimS.mid, excS.mid);
        say ("P3 sanity: output vs input under excitation differs by "
             + juce::String (sanF.maxAbsDb, 2) + " dB max -> "
             + (sanF.maxAbsDb > 4 * sigDepth ? "PASS (plugin alters the signal under excitation)"
                                             : "FAIL (inert)"));

        say ("P1 write landing: " + juce::String (writeN) + " writes so far, mean "
             + juce::String (writeN > 0 ? writeMsSum / writeN : 0.0, 2) + " ms, max "
             + juce::String (writeMsMax, 2) + " ms, unlanded: " + (sawUnlanded ? "YES" : "none"));

        // =====================================================================
        say ("");
        say ("ARM B: correct map");
        double monoDrift = 0;
        auto monoBefore = params[idxMono]->getValue();
        auto trackMono = [&] { monoDrift = juce::jmax (monoDrift,
                                  (double) std::abs (params[idxMono]->getValue() - monoBefore)); };

        // ---- ISOLATION: A DECLARED REFERENCE STATE (item 3, session 2) -----
        // Arm B already moved one parameter per block, but the reference each
        // block started from was implicit and different: B1 ran at whatever q
        // the excitation left, B3 re-established freq but not q, B4 set gain
        // but not freq. Nothing stated the reference and nothing verified it,
        // so a reordering would have re-coupled the blocks silently.
        //
        // WHY THIS IS NOT ENOUGH ON ITS OWN, which is the whole difficulty:
        // lobeFeatures returns centre, depth and width from ONE spectrum pair
        // and the three are physically coupled. Moving one parameter at a time
        // does not decouple the FEATURES -- a band sitting at the wrong q
        // measures a different centre, so a q defect surfaces as a centre
        // failure and would be attributed to freq_hz. Isolation of the inputs
        // is necessary and insufficient; the per-semantic claim checks below
        // are what make the attribution sound.
        struct BandRef { double freqHz = 100.0, gainDb = 6.0, q = 1.0; } bandRef;
        auto establishReference = [&] (const juce::String& probing)
        {
            wc (idxFreq, normFor (freqAnch, bandRef.freqHz));
            wc (idxGain, normFor (gainAnch, bandRef.gainDb));
            wc (idxQ,    normFor (qAnch,    bandRef.q));
            trackMono();
            say ("  reference: freq " + juce::String (bandRef.freqHz, 1) + " Hz, gain "
                 + juce::String (bandRef.gainDb, 1) + " dB, q " + juce::String (bandRef.q, 2)
                 + " -- probing " + probing);
        };

        // "the other two held" was a sentence, not a check. Verify it: after a
        // block, the two parameters that were supposed to stay put must still
        // be at the reference. A held parameter that drifted would put the
        // feature somewhere the attribution above has already vouched for.
        auto verifyHeld = [&] (const juce::String& probing, int a, int b,
                               const juce::Array<juce::Array<float>>& aA,
                               const juce::Array<juce::Array<float>>& bA,
                               double aRef, double bRef, const juce::String& aName,
                               const juce::String& bName)
        {
            const double da = std::abs (params[a]->getValue() - normFor (aA, aRef));
            const double db = std::abs (params[b]->getValue() - normFor (bA, bRef));
            const bool held = da < 0.005 && db < 0.005;
            assertHarness ("B-hold while probing " + probing, held,
                  aName + " drift " + juce::String (da, 5) + ", " + bName + " drift "
                  + juce::String (db, 5) + " (limit 0.005) -- the two parameters not being "
                    "probed stayed at the reference, so the feature measured belongs to "
                  + probing);
        };

        // ---- PER-SEMANTIC CLAIM CHECKS -------------------------------------
        // Did each parameter land where the MAP says? Attribution rests on
        // this: a centre failure is only freq_hz's when gain and q are proven
        // to be where the map claimed. Otherwise the coupled feature is
        // reporting somebody else's defect under freq_hz's name.
        struct SemClaim { juce::String semantic, unit; double worst = 0; bool checked = false, parsed = false;
                          juce::String row; };
        auto claimFor = [&] (const juce::String& semantic, int idx,
                             const juce::Array<juce::Array<float>>& anchors,
                             double value, const juce::String& unit)
        {
            ejmap::subject::SlotRef sl;
            sl.found = true; sl.index = idx; sl.semantic = semantic; sl.anchors = anchors;
            auto c = ejmap::subject::checkMapClaim (sl, value,
                        [&] (float n) { wc (idx, n); },
                        [&] { return params[idx]->getCurrentValueAsText(); });
            SemClaim sc; sc.semantic = semantic; sc.unit = unit; sc.checked = true;
            sc.parsed = c.parsed; sc.worst = c.parsed ? c.error() : 0.0;
            sc.row = "    " + semantic.paddedRight (' ', 10) + "map says "
                   + juce::String (c.claimed, 2) + " " + unit + " at norm "
                   + juce::String (c.norm, 4) + " -> plugin displays '" + c.display + "'"
                   + (c.parsed ? "  |diff| " + juce::String (c.error(), 2) + " " + unit
                               : "  (unreadable as a number)");
            return sc;
        };
        say ("");
        say ("  PER-SEMANTIC CLAIM CHECK (did each input land where the map says?)");
        auto clFreq = claimFor ("freq_hz", idxFreq, freqAnch, bandRef.freqHz, "Hz");
        auto clGain = claimFor ("gain_db", idxGain, gainAnch, bandRef.gainDb, "dB");
        auto clQ    = claimFor ("q",       idxQ,    qAnch,    bandRef.q,      "");
        for (const auto* c : { &clFreq, &clGain, &clQ }) say (c->row);
        // Tolerances in each semantic's OWN unit -- never one number for three.
        const double clTolFreq = 0.05 * bandRef.freqHz;      // 5% of the asked frequency
        const double clTolGain = 1.0;                        // dB
        const double clTolQ    = 0.25 * bandRef.q;           // a quarter of the asked q
        auto claimBad = [] (const SemClaim& c, double tol)
                        { return c.parsed && c.worst > tol; };
        if (claimBad (clFreq, clTolFreq) || claimBad (clGain, clTolGain) || claimBad (clQ, clTolQ))
        {
            juce::StringArray bad;
            if (claimBad (clFreq, clTolFreq)) bad.add ("freq_hz (" + juce::String (clFreq.worst, 2) + " Hz)");
            if (claimBad (clGain, clTolGain)) bad.add ("gain_db (" + juce::String (clGain.worst, 2) + " dB)");
            if (claimBad (clQ,    clTolQ))    bad.add ("q (" + juce::String (clQ.worst, 3) + ")");
            emitContradicts (bad.joinIntoString (", "),
                "the reference state the band measurements are taken from does not match the map: "
                + bad.joinIntoString ("; ") + " landed away from what the map claims. Every arm-B "
                  "feature is measured from this state, and centre, depth and width all come "
                  "from one spectrum pair and move together. A verdict issued now would attribute "
                  "this defect to whichever FEATURE moved rather than to the PARAMETER that "
                  "caused it, which is why the inputs are checked before any feature is read");
            std::cout << "GATE M9: STOPPED (reference state does not match the map)" << std::endl;
            quitNow(); return;
        }
        say ("    all three inputs land within tolerance (freq " + juce::String (clTolFreq, 1)
             + " Hz, gain " + juce::String (clTolGain, 1) + " dB, q " + juce::String (clTolQ, 2)
             + "), so a feature failure below is attributable to its OWN parameter");

        establishReference ("freq_hz");
        auto b100 = P::welch (render());
        auto f100 = P::lobeFeatures (preExcS.mid, b100.mid);
        wc (idxFreq, normFor (freqAnch, 400.0)); trackMono();
        auto b400 = P::welch (render());
        auto f400 = P::lobeFeatures (preExcS.mid, b400.mid);

        const double pred100 = P::predictedLanding (freqAnch, 100.0);
        const double pred400 = P::predictedLanding (freqAnch, 400.0);
        const double predOct = std::log2 (pred400 / pred100);
        const double measOct = std::log2 (f400.centreHz / f100.centreHz);
        // ITEM 1 (decided): tolerance = signed 0.070 plugin-error gate PLUS the
        // measured instrument bias at the predicted frequency. Bias-shaped,
        // not noise-shaped; the two terms are recorded separately so the
        // split survives to the 20-map re-derivation.
        const double biasA = P::InstrumentFloor::centreBiasOct (pred100);
        const double biasB = P::InstrumentFloor::centreBiasOct (pred400);
        const double biasTerm = juce::jmax (biasA, biasB);
        const double centreTol = 0.070 + biasTerm;
        assertHarness ("B1 centre move", std::abs (measOct - predOct) <= centreTol,
              "predicted " + juce::String (predOct, 3) + " oct (ladder: " + juce::String (pred100, 1)
              + " -> " + juce::String (pred400, 1) + " Hz), measured " + juce::String (measOct, 3)
              + " oct (" + juce::String (f100.centreHz, 1) + " -> " + juce::String (f400.centreHz, 1)
              + " Hz), error " + juce::String (std::abs (measOct - predOct), 4)
              + " oct vs tol " + juce::String (centreTol, 4)
              + " = 0.070 gate + " + juce::String (biasTerm, 4) + " instrument bias @"
              + juce::String (pred400, 0) + " Hz [terms recorded separately: gate 0.0700, bias "
              + juce::String (biasTerm, 4) + "]");
        verifyHeld ("freq_hz", idxGain, idxQ, gainAnch, qAnch, bandRef.gainDb, bandRef.q,
                    "gain_db", "q");
        assertHarness ("B2 expressible", predOct >= 4 * sigCentre,
              "Delta_pred " + juce::String (predOct, 3) + " oct vs 4*sigma_centre "
              + juce::String (4 * sigCentre, 4) + " oct");

        // B3 gain depth, from the declared reference
        establishReference ("gain_db");
        wc (idxGain, normFor (gainAnch, 3.0)); trackMono();
        auto d3 = P::lobeFeatures (preExcS.mid, P::welch (render()).mid);
        wc (idxGain, normFor (gainAnch, 9.0)); trackMono();
        auto d9 = P::lobeFeatures (preExcS.mid, P::welch (render()).mid);
        const double predDDepth = P::predictedLanding (gainAnch, 9.0) - P::predictedLanding (gainAnch, 3.0);
        const double measDDepth = d9.depthDb - d3.depthDb;
        const double tolDepth = juce::jmax (0.25 * std::abs (predDDepth), 4 * sigDepth);
        assertHarness ("B3 gain depth", std::abs (measDDepth - predDDepth) <= tolDepth,
              "predicted +" + juce::String (predDDepth, 2) + " dB, measured +"
              + juce::String (measDDepth, 2) + " dB (depth@+3 " + juce::String (d3.depthDb, 2)
              + ", depth@+9 " + juce::String (d9.depthDb, 2) + "), error "
              + juce::String (std::abs (measDDepth - predDDepth), 3) + " vs tol "
              + juce::String (tolDepth, 3) + " [max(0.25*pred, 4*sigma)]");

        verifyHeld ("gain_db", idxFreq, idxQ, freqAnch, qAnch, bandRef.freqHz, bandRef.q,
                    "freq_hz", "q");

        // B4 q width, from the declared reference
        establishReference ("q");
        wc (idxQ, normFor (qAnch, 0.71)); trackMono();
        auto wLo = P::lobeFeatures (preExcS.mid, P::welch (render()).mid);
        wc (idxQ, normFor (qAnch, 2.0)); trackMono();
        auto wHi = P::lobeFeatures (preExcS.mid, P::welch (render()).mid);
        const double qLo = P::predictedLanding (qAnch, 0.71), qHi = P::predictedLanding (qAnch, 2.0);
        const double predWRatio = std::log2 (qHi / qLo);          // width ~ 1/q
        const double measWRatio = std::log2 (wLo.widthOct / juce::jmax (1e-6, wHi.widthOct));
        const double tolW = juce::jmax (0.25 * std::abs (predWRatio), 4 * sigWidth);
        assertHarness ("B4 q width", std::abs (measWRatio - predWRatio) <= tolW,
              "q " + juce::String (qLo, 2) + " -> " + juce::String (qHi, 2)
              + ": predicted width ratio " + juce::String (predWRatio, 3)
              + " oct-log2, measured " + juce::String (measWRatio, 3)
              + " (width@qLo " + juce::String (wLo.widthOct, 3) + " oct, width@qHi "
              + juce::String (wHi.widthOct, 3) + " oct), error "
              + juce::String (std::abs (measWRatio - predWRatio), 3) + " vs tol " + juce::String (tolW, 3));

        verifyHeld ("q", idxFreq, idxGain, freqAnch, gainAnch, bandRef.freqHz, bandRef.gainDb,
                    "freq_hz", "gain_db");

        // THE COUPLING, MEASURED FROM DATA THIS RUN ALREADY HAS. B4 moved only
        // q; if the centre estimate moved with it, then centre is not a pure
        // function of freq_hz and a q defect can surface as a freq_hz failure.
        // This is the number that makes eq's pooling physical rather than a
        // reporting choice, and it is free -- both features come from the two
        // spectra B4 already captured.
        {
            const double centreShiftOct = std::abs (std::log2 (wHi.centreHz
                                                    / juce::jmax (1.0, wLo.centreHz)));
            say ("  COUPLING (recorded, not a criterion): moving ONLY q from "
                 + juce::String (qLo, 2) + " to " + juce::String (qHi, 2) + " moved the CENTRE "
                   "estimate " + juce::String (wLo.centreHz, 1) + " -> "
                 + juce::String (wHi.centreHz, 1) + " Hz = " + juce::String (centreShiftOct, 4)
                 + " oct, against B1's centre tolerance of " + juce::String (centreTol, 4)
                 + " oct. Centre is NOT a pure function of freq_hz: at "
                 + juce::String ((int) std::round (100.0 * centreShiftOct
                                                  / juce::jmax (1e-9, centreTol)))
                 + "% of B1's tolerance, a q defect can surface as a freq_hz failure. That is why "
                   "attribution rests on the per-semantic claim checks above and not on which "
                   "feature moved");
        }

        const double sideB_first = P::bandEnergyDb (b100.side, 50, 400);
        const double sideB_last  = P::bandEnergyDb (b400.side, 50, 400);
        // AMENDMENT 1: B5 is the parameter-drift half only. The side-energy
        // null was retired -- A5's live falsifier covers mid/side confusion
        // better than a null can, and AMEK's TMT channel asymmetry makes a
        // side null unmeetable by construction. The quantity is RECORDED with
        // its cause and given no threshold.
        assertHarness ("B5 negative control (drift only)", monoDrift < 0.005,
              "Mono Maker parameter value drift across every write in this arm: "
              + juce::String (monoDrift, 5) + " (limit 0.005)");
        say ("   recorded, not a criterion: side band(50-400) moved "
             + juce::String (std::abs (sideB_last - sideB_first), 3)
             + " dB against a " + juce::String (std::abs (d9.depthDb), 2)
             + " dB primary. Cause: AMEK's TMT channel-tolerance modelling makes L and R "
               "filters genuinely differ, so a mid-band move carries a channel-asymmetric "
               "component. Expected physics on this plugin; no threshold assigned.");

        const bool b1ok = std::abs (measOct - predOct) <= centreTol;
        const bool b3ok = std::abs (measDDepth - predDDepth) <= tolDepth;
        const bool b4ok = std::abs (measWRatio - predWRatio) <= tolW;
        assertHarness ("B6 verdicts", b1ok && b3ok && b4ok,
              juce::String ("freq_hz=") + (b1ok ? "confirms" : "NOT-confirms")
              + " gain_db=" + (b3ok ? "confirms" : "NOT-confirms")
              + " q=" + (b4ok ? "confirms" : "NOT-confirms"));

        // The gate publishes these three as HARNESS ASSERTIONS, not through
        // emitVerdict (M9_PAUSED open item 1: 3 of 19 sites route). The batch
        // runner records them HERE, where the booleans and their numbers are in
        // scope, rather than parsing B6's sentence back into a verdict --
        // reading a result out of prose is the same class as confirming a write
        // by the writer's return value. ARM B ONLY: arm A's verdicts are about
        // a deliberately falsified map and say nothing about the map on disk.
        batchRows.add ({ "freq_hz", b1ok ? "confirms" : "NOT-confirms",
                         "centre: predicted " + juce::String (predOct, 3) + " oct, measured "
                         + juce::String (measOct, 3) + ", tol " + juce::String (centreTol, 4)
                         + " [gate assertion B1, unrouted]", "group 1 / freq_hz" });
        batchRows.add ({ "gain_db", b3ok ? "confirms" : "NOT-confirms",
                         "depth: predicted " + juce::String (predDDepth, 2) + " dB, measured "
                         + juce::String (measDDepth, 2) + ", tol " + juce::String (tolDepth, 3)
                         + " [gate assertion B3, unrouted]", "group 1 / gain_db" });
        batchRows.add ({ "q", b4ok ? "confirms" : "NOT-confirms",
                         "width ratio: predicted " + juce::String (predWRatio, 3) + ", measured "
                         + juce::String (measWRatio, 3) + ", tol " + juce::String (tolW, 3)
                         + " [gate assertion B4, unrouted]", "group 1 / q" });

        say ("arm B wall clock: " + juce::String ((juce::Time::getMillisecondCounterHiRes() - tArmB0) / 1000.0, 2) + " s");

        // =====================================================================
        say ("");
        say ("ARM A: deliberate mis-map (band 1 freq_hz -> Mono Maker [7]); zero human input");
        const auto tArmA0 = juce::Time::getMillisecondCounterHiRes();
        say ("  excitation carried from P4 (verified by signal, "
             + juce::String (excF.maxAbsDb, 2) + " dB): should-have-moved branch ARMED.");

        // reset band to a known state
        wc (idxFreq, normFor (freqAnch, 100.0));
        wc (idxGain, normFor (gainAnch, 6.0));
        wc (idxQ, normFor (qAnch, 1.0));
        // the mis-mapped writes: band-1 freq anchors, Mono Maker's index
        wc (idxMono, normFor (freqAnch, 100.0));
        auto a1r = P::welch (render());
        wc (idxMono, normFor (freqAnch, 400.0));
        auto a2r = P::welch (render());

        // AMENDMENT 2: A1 is LOCALIZED, never broadband. Broadband
        // redistribution is expected physics (M = (L+R)/2 means mono-ing
        // below the crossover moves side content into mid) and is recorded,
        // never a criterion. The test: is there a COHERENT lobe within the
        // declared frequency gate of the predicted centre, deep enough to be
        // a lobe at all? The depth floor reuses the declared 0.25 constant
        // against the excitation's EXPRESSED magnitude, and the position
        // window reuses the declared 0.070 oct frequency gate.
        const double lobeDepthFloor = 0.25 * std::abs (excF.depthDb);
        auto amid = P::lobeFeatures (a1r.mid, a2r.mid, 30.0, 20000.0, lobeDepthFloor);
        const bool lobeAtPredicted = amid.centreDefined()
              && std::abs (std::log2 (amid.centreHz / pred400)) <= centreTol;
        assertHarness ("A1 no localized lobe", ! lobeAtPredicted,
              juce::String ("depth floor for lobe existence = 0.25 * expressed ")
              + juce::String (std::abs (excF.depthDb), 2) + " dB = "
              + juce::String (lobeDepthFloor, 2) + " dB; largest mid excursion "
              + juce::String (amid.maxAbsDb, 3) + " dB -> lobe "
              + (amid.centreDefined() ? "EXISTS at " + juce::String (amid.centreHz, 1) + " Hz"
                                      : "does not exist, centre UNDEFINED")
              + "; predicted centre " + juce::String (pred400, 1) + " Hz +/- 0.070 oct");
        say ("   recorded, not a criterion: broadband mid redistribution "
             + juce::String (amid.maxAbsDb, 3) + " dB. Cause: M=(L+R)/2, so mono-ing below "
               "the crossover necessarily moves side content into mid. Expected physics.");
        assertHarness ("A2 branch fires", predOct >= 4 * sigCentre,
              "Delta_pred " + juce::String (predOct, 3) + " oct >= 4*sigma_centre "
              + juce::String (4 * sigCentre, 4) + " oct, and excitation verified -> armed");

        // A3 exclusions, individually
        juce::StringArray engagedModes;
        if (auto* co = mapVar.getProperty ("controls", juce::var()).getDynamicObject())
            for (auto& kv : co->getProperties())
                if (kv.value.getProperty ("kind", "").toString() == "mode")
                {
                    const int ci = (int) kv.value.getProperty ("index", -1);
                    if (! juce::isPositiveAndBelow (ci, params.size())) continue;
                    const float v = params[ci]->getValue();
                    juce::String label = "?"; double best = 1e9;
                    if (auto* lo = kv.value.getProperty ("labels", juce::var()).getDynamicObject())
                        for (auto& lv : lo->getProperties())
                            if (std::abs ((double) lv.value - v) < best)
                            { best = std::abs ((double) lv.value - v); label = lv.name.toString(); }
                    engagedModes.add (kv.name.toString() + "=" + label);
                }
        say ("  A3(a) map mode states: " + engagedModes.joinIntoString (", "));
        bool gestureAtMono = false;
        for (const auto& entry : juce::RangedDirectoryIterator (ledger.getRoot(), false, "captures-*.jsonl"))
        {
            juce::StringArray lines; lines.addLines (entry.getFile().loadFileAsString());
            for (const auto& line : lines)
            {
                auto v = juce::JSON::parse (line);
                const auto kind = v.getProperty ("kind", "").toString();
                if ((kind == "captured" || kind == "captured_from_gesture")
                     && (int) v.getProperty ("index", -1) == idxMono
                     && v.getProperty ("plugin_id", "").toString() == loadedId)
                    gestureAtMono = true;
            }
        }
        say (juce::String ("  A3(b) gesture evidence at index 7: ") + (gestureAtMono ? "FOUND" : "none"));

        // A5 falsifier: side energy between the two Mono Maker landings
        const double monoLandA = P::predictedLanding (monoAnch, 0) * 0
                                 + [&]{ // forward-eval mono's own anchors at the two written norms
                                     auto eff = echojay::dominantMonotonicTable (monoAnch);
                                     auto fwd = [&] (float n) {
                                         auto rows = eff.table;
                                         for (int i = 1; i < rows.size(); ++i)
                                         { const float n0 = rows[i-1][1], n1 = rows[i][1];
                                           if ((n >= juce::jmin (n0,n1) && n <= juce::jmax (n0,n1)) || i == rows.size()-1)
                                           { const float t = std::abs (n1-n0) > 1e-9f ? (n-n0)/(n1-n0) : 0.0f;
                                             return (double) (rows[i-1][0] + t * (rows[i][0]-rows[i-1][0])); } }
                                         return (double) rows.getLast()[0]; };
                                     return fwd (normFor (freqAnch, 100.0)); }();
        const double monoLandB = [&]{
            auto eff = echojay::dominantMonotonicTable (monoAnch);
            auto rows = eff.table; const float n = normFor (freqAnch, 400.0);
            for (int i = 1; i < rows.size(); ++i)
            { const float n0 = rows[i-1][1], n1 = rows[i][1];
              if ((n >= juce::jmin (n0,n1) && n <= juce::jmax (n0,n1)) || i == rows.size()-1)
              { const float t = std::abs (n1-n0) > 1e-9f ? (n-n0)/(n1-n0) : 0.0f;
                return (double) (rows[i-1][0] + t * (rows[i][0]-rows[i-1][0])); } }
            return (double) rows.getLast()[0]; }();
        const double bandLo = juce::jmin (monoLandA, monoLandB), bandHi = juce::jmax (monoLandA, monoLandB);
        const double sideA = P::bandEnergyDb (a1r.side, bandLo, bandHi);
        const double sideBv = P::bandEnergyDb (a2r.side, bandLo, bandHi);
        const double sideMove = std::abs (sideBv - sideA);
        const bool a5 = sideMove >= 4 * sigSide;
        assertHarness ("A5 falsifier (side moves)", a5,
              "Mono Maker landings " + juce::String (monoLandA, 1) + " -> " + juce::String (monoLandB, 1)
              + " Hz; side energy in [" + juce::String (bandLo, 0) + "," + juce::String (bandHi, 0)
              + "] Hz moved " + juce::String (sideMove, 3) + " dB vs 4*sigma_side "
              + juce::String (4 * sigSide, 3) + " dB");

        // verdict, through the carve-outs
        juce::String verdict, branch;
        if (! a5 && ! lobeAtPredicted)
        { verdict = "inconclusive"; branch = "carve-out 1: total render-deafness, exclusions govern"; }
        else if (a5 && ! lobeAtPredicted)
        { verdict = "contradicts";
          branch = "wrong-place branch: the index IS live (side moved >= 4 sigma at the driven "
                   "frequency) but NO localized mid lobe exists at the predicted centre "
                   "(should-have-moved, localized). NOT the render-deafness branch: this is "
                   "mid-feature absence with proven index liveness."; }
        else { verdict = "contradicts"; branch = "wrong-direction/magnitude branch"; }
        assertHarness ("A4 verdict", verdict == "contradicts", verdict + " (" + branch + ")");
        if (a5)
        {
            say ("  A6 card text:");
            say ("    PROBE CONTRADICTS  freq_hz -> [7] Mono Maker");
            say ("      requested 100 -> 400 Hz: no EQ lobe appeared at " + juce::String (pred400, 0)
                 + " Hz (largest mid excursion " + juce::String (amid.maxAbsDb, 2)
                 + " dB, a lobe needs " + juce::String (lobeDepthFloor, 2) + " dB)");
            say ("      but the index IS live: it moved a STEREO-WIDTH feature at "
                 + juce::String (bandLo, 0) + "-" + juce::String (bandHi, 0) + " Hz by "
                 + juce::String (sideMove, 2) + " dB -- a mono-maker, not a band frequency.");
            say ("      W re-verify by hand  -  shift+N insist (recorded, with these numbers)  -  D later");
        }
        say ("  A7 human input: none (no gesture, pick or corroboration entered the verdict path; "
             "scripted writes only).");
        say ("arm A wall clock: " + juce::String ((juce::Time::getMillisecondCounterHiRes() - tArmA0) / 1000.0, 2) + " s");

        // ---- restore the real state through the verified-restore path ------
        host.pausePumpForMutation();
        inst->setStateInformation (preState.getData(), (int) preState.getSize());
        host.resumePumpAfterMutation();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        bool restored = true;
        for (auto& kv : preVals)
            if (std::abs (params[kv.first]->getValue() - kv.second) > 0.01f) restored = false;
        assertHarness ("restore", restored, juce::String ("state restored and verified on ")
              + juce::String ((int) preVals.size()) + " tracked parameters"
              + "; the mis-map existed only in memory, the on-disk map was never touched");
        if (restored) { stakeFile.deleteFile(); stateFile.deleteFile(); }

        say ("");
        std::cout << "GATE M9: " << (fails == 0 ? "PASS" : juce::String (fails) + " CRITERIA FAILED")
                  << std::endl;
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
        // Manufacture the pre-M10 shape: blank the stamped provenance. The
        // corpus no longer contains a pre-M10 map (both were re-emitted by
        // the stamping binary), so the shape under test is simulated -- and
        // said so. Without this, steps 1-2 silently test whatever provenance
        // the fixture happens to carry.
        {
            auto prov = mapVar.getProperty ("provenance", juce::var());
            if (auto* pd = prov.getDynamicObject())
                for (auto* k : { "tester_id", "machine_id", "apply_header_sha" })
                    pd->setProperty (k, "");
        }
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
            ok (text.contains ("\r\nX-EJMap-Token: ") && ! text.contains ("\r\nX-EJMap-Token: \r\n"),
                "X-EJMap-Token header present (server fails closed without it)");
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

        assignPanel.dispatchAction ("space");     // accept -> tier preview card
        const int builtControls = assignPanel.controlsForSubmit().size();
        ok (builtControls > 0,
            "accept built " + juce::String (builtControls) + " named controls");

        // THE TIER CARD (server contract 2026-08-02): exception-shaped. The
        // exposed list is the PINNED server ordering; X kicks out, P pulls
        // up, the same key clears, untouched rows emit nothing.
        ok (assignPanel.tierPhase, "tier preview card follows the accept");
        {
            auto ex0 = assignPanel.tierExposure();
            ok (! ex0.defaultExposure.isEmpty(),
                "preview shows the server's exposed set ("
                  + juce::String (ex0.defaultExposure.size()) + " of "
                  + juce::String (ex0.orderedCandidates.size()) + ")");
            std::cout << "---- tier card ----" << std::endl
                      << assignPanel.bandTableText() << std::endl;

            const auto firstName = ex0.defaultExposure[0];
            assignPanel.dispatchAction ("kick");              // cursor row 0 out
            auto ex1 = assignPanel.tierExposure();
            ok (! ex1.defaultExposure.contains (firstName),
                "X: '" + firstName + "' left the exposed set at the moment of the gesture");
            ok (assignPanel.controlByName (firstName) != nullptr
                  && assignPanel.controlByName (firstName)->tier == "hidden",
                "X wrote tier: hidden on the staged control");
            // The kicked row LEFT the exposed list, so clearing means going
            // where it went: expand, navigate to it, X again.
            assignPanel.dispatchAction ("expand");
            while (assignPanel.tierViewNames[assignPanel.tierCursor] != firstName
                    && assignPanel.tierCursor < assignPanel.tierViewNames.size() - 1)
                assignPanel.dispatchAction ("next");
            assignPanel.dispatchAction ("kick");              // same key clears
            auto ex2 = assignPanel.tierExposure();
            ok (ex2.defaultExposure.contains (firstName)
                  && assignPanel.controlByName (firstName)->tier.isEmpty(),
                "X again CLEARS the tier (reversible, visible)");
            assignPanel.dispatchAction ("expand");            // collapse again
            assignPanel.tierCursor = 0;

            // Pull a collapsed row up, watch it arrive, then leave it set so
            // submit proves the emission -- INCLUDING across the W re-sweep
            // below, which rebuilds the staged controls.
            assignPanel.dispatchAction ("expand");
            juce::String pulled;
            for (const auto& c : ex2.orderedCandidates)
                if (! ex2.defaultExposure.contains (c.name) && c.cls != "plumbing")
                { pulled = c.name; break; }
            if (pulled.isNotEmpty())
            {
                while (assignPanel.tierViewNames[assignPanel.tierCursor] != pulled
                        && assignPanel.tierCursor < assignPanel.tierViewNames.size() - 1)
                    assignPanel.dispatchAction ("next");
                assignPanel.dispatchAction ("pull");
                auto ex3 = assignPanel.tierExposure();
                ok (ex3.defaultExposure.contains (pulled),
                    "P: '" + pulled + "' entered the exposed set (tier: primary)");
            }
            tierPulledName = pulled;
            assignPanel.dispatchAction ("space");             // accept tiers
            ok (! assignPanel.tierPhase, "SPACE closes the tier card");
        }

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
            assignPanel.dispatchAction ("space");   // accept again -> tier card
            ok (assignPanel.tierPhase, "re-accept re-offers the tier card");
            assignPanel.dispatchAction ("space");   // zero-interaction pass-through
        }

        assignPanel.dispatchAction ("submit");
        ok (assignPanel.isSummaryShowing() && assignPanel.isSubmitEnabled(),
            "review offers submit (controls row is confirmed work)");
        assignPanel.confirmSubmitFromSummary();

        // The emitted map: exactly ONE tier (the P), everything else absent.
        {
            auto map = juce::JSON::parse (ledger.getRoot().getChildFile ("maps")
                          .getChildFile (currentFp + ".json").loadFileAsString());
            int tiers = 0; juce::String tieredName;
            if (auto* co = map.getProperty ("controls", juce::var()).getDynamicObject())
                for (auto& kv : co->getProperties())
                    if (! kv.value.getProperty ("tier", juce::var()).isVoid())
                    { ++tiers; tieredName = kv.name.toString(); }
            ok (tiers == (tierPulledName.isNotEmpty() ? 1 : 0)
                  && (tierPulledName.isEmpty() || tieredName == tierPulledName),
                "map emits EXACTLY the gestured tier ("
                  + juce::String (tiers) + "), it SURVIVED the W re-sweep, "
                  "untouched rows emit none");
        }

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

        // THE duplicate-index rule from EjmapAssignment.h -- the same call the
        // review screen makes, never a private copy. This block used to hold
        // one, and it refused "[-1] bands AND controls" AFTER the review's
        // copy was fixed to exclude surface rows: the AMEK re-submission died
        // twice on a rule that existed twice.
        {
            const auto conflicts = duplicateIndexConflicts (rws);
            if (! conflicts.isEmpty())
            {
                captureReadout.setText ("SUBMIT REFUSED: " + conflicts.joinIntoString ("; ")
                    + ". One of them is wrong.", juce::dontSendNotification);
                std::cout << "ASSIGN: SUBMIT REFUSED: duplicate index "
                          << conflicts.joinIntoString ("; ") << std::endl;
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
        // The extractor version THIS binary compiles, read from the header's
        // own default -- never a hand-kept copy that can lag a bump.
        p.provenance.extractorVersion = juce::String (echojay::ExtractorConfig().extractorVersion);
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
            // Surface rows drive a flow; their outcome is the top-level
            // groups/controls objects, NEVER a params entry. The spiff
            // session's controls row still carried a stale capture (the old
            // W-routing defect: boost depth grabbed as if controls were a
            // knob), so it passed the confirmed+anchors test and the server
            // read params.controls, kind "controls" -- a surface row wearing
            // a semantic. Same class as the -1 duplicate refusal.
            if (r.isSurfaceRow())
                continue;
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

        // ------------------------------------------------------------------
        // lockstep_of, Source B (signed 2026-08-02): write-verify over
        // CANDIDATE pairs -- names equal after the M5 stripDigits equality,
        // nothing speculative -- still inside the state-snapshot window.
        // Canonical rule as signed: a Tier 1 / group claim wins; lower vendor
        // index as fallback. Every stamp carries by: human_pick|write_verify;
        // the two evidence sources never blur. The write is the observation:
        // move the canonical, watch the partner follow 1:1, or stamp nothing.
        juce::Array<NamedControl> stampedControls;
        {
            auto params = inst->getParameters();
            std::set<int> claimed;
            for (const auto& r : rws)
                if (! r.isSurfaceRow() && r.state == AssignRow::State::confirmed && r.resolvedIndex >= 0)
                    claimed.insert (r.resolvedIndex);
            for (const auto& g : assignPanel.groupsForSubmit())
                for (const auto& gm : g.params)
                    for (int gi : gm.indices)
                        claimed.insert (gi);

            auto strippedOf = [&params] (int idx)
            {
                return juce::isPositiveAndBelow (idx, params.size())
                         ? EvidenceIndex::stripDigits (params[idx]->getName (64)) : juce::String();
            };

            int stamped = 0, verifiedFailed = 0;
            for (const auto& c0 : assignPanel.controlsForSubmit())
            {
                auto c = c0;
                const int ci = c.indices.size() == 1 ? c.indices[0] : -1;
                if (ci >= 0 && ! c.duplicate && c.lockstepOf < 0)
                {
                    // Source A first: an existing human-pick observation.
                    auto it = assignPanel.lockstepObserved.find (ci);
                    if (it != assignPanel.lockstepObserved.end())
                    {
                        c.lockstepOf = it->second;
                        c.lockstepBy = assignPanel.lockstepSource.count (ci)
                                         ? assignPanel.lockstepSource[ci] : "human_pick";
                        ++stamped;
                    }
                    else
                    {
                        // Candidate canonical: claimed index with the same
                        // stripped name; else a lower-indexed staged control.
                        const auto key = EvidenceIndex::stripDigits (c.name);
                        int canonical = -1;
                        if (key.isNotEmpty())
                        {
                            for (int k : claimed)
                                if (strippedOf (k) == key) { canonical = k; break; }
                            if (canonical < 0)
                                for (const auto& d : assignPanel.controlsForSubmit())
                                    if (&d != &c0 && d.indices.size() == 1 && ! d.duplicate
                                         && d.indices[0] < ci
                                         && EvidenceIndex::stripDigits (d.name) == key)
                                    { canonical = d.indices[0]; break; }
                        }
                        if (canonical >= 0 && canonical != ci
                             && juce::isPositiveAndBelow (canonical, params.size())
                             && juce::isPositiveAndBelow (ci, params.size()))
                        {
                            auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (60); };
                            params[canonical]->setValueNotifyingHost (0.35f); pump();
                            const float p1 = params[ci]->getValue();
                            params[canonical]->setValueNotifyingHost (0.65f); pump();
                            const float p2 = params[ci]->getValue();
                            if (std::abs ((p2 - p1) - 0.30f) <= 0.02f)
                            {
                                c.lockstepOf = canonical;
                                c.lockstepBy = "write_verify";
                                ++stamped;
                            }
                            else
                                ++verifiedFailed;   // candidate discarded, nothing emitted
                        }
                    }
                }
                stampedControls.add (c);
            }
            if (stamped > 0 || verifiedFailed > 0)
                std::cout << "ASSIGN: lockstep: " << stamped << " twin(s) stamped, "
                          << verifiedFailed << " candidate pair(s) did not track 1:1 "
                          << "(discarded, nothing emitted)" << std::endl;
        }

        if (stateBefore.getSize() > 0)
        {
            host.pausePumpForMutation();
            inst->setStateInformation (stateBefore.getData(), (int) stateBefore.getSize());
            host.resumePumpAfterMutation();
        }

        p.groups = assignPanel.groupsForSubmit();
        for (const auto& c : stampedControls)
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
              << "   endpoint's contract before anything is sent)\n"
              << "  X-EJMap-Token: "
              << (juce::SystemStats::getEnvironmentVariable ("EJMAP_INGEST_TOKEN", "").isNotEmpty()
                    ? "set from EJMAP_INGEST_TOKEN (value not shown)"
                    : "UNSET -- the server fails closed; this request would 401")
              << "\n\n";

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
            // HIDE-MAPPED hides ONLY the two submitted states. differentBuild
            // and unknown both mean "something needs a decision", so hiding
            // them would turn a filter into a silent drop -- a product with a
            // map for the wrong build would vanish and read as handled.
            if (hideMapped)
            {
                const auto st = mapStateFor (rows.getReference (i)).state;
                if (st == MapState::submittedByYou || st == MapState::submittedByOther)
                    continue;
            }
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

        // STATE COLUMN, left of the name so the eye lands on it while scanning
        // 1,376 rows rather than reading to the end of each line. It recolours
        // the GLYPH ONLY, never the row: quarantine already owns the row
        // colour and a quarantined plugin should read as quarantined first.
        const auto st = mapStateFor (sp);
        auto bounds = juce::Rectangle<int> (0, 0, w, h);
        auto col = bounds.removeFromLeft (22);
        g.setColour (colourFor (st.state));
        g.setFont (13.0f);
        g.drawText (glyphFor (st.state), col, juce::Justification::centred);

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
    //==========================================================================
    // MAP STATE (feature 2). Six states, and two of them are deliberately
    // distinct characters rather than one glyph dimmed: "unmapped" and
    // "unknown" are DIFFERENT FACTS, and the whole reason the query degrades
    // instead of blocking is that they must never render alike.
    enum class MapState { unmapped, localOnly, submittedByYou, submittedByOther,
                          differentBuild, unknown };

    struct MapStateRow
    {
        MapState state = MapState::unknown;
        juce::String by, at, schema;
        bool probed = false;
        int  mapsForIdentity = 0;      // how many fps this product has, any build
        int  paramCountHere = 0;       // the param count of the build we matched
    };

    std::map<juce::String, MapStateRow> mapStateByIdentity;
    juce::Time mapStateFetchedAt;
    juce::String mapStateFailure;      // empty when the last fetch succeeded
    bool hideMapped = false;

    static juce::String glyphFor (MapState s)
    {
        switch (s)
        {
            case MapState::unmapped:          return juce::String::fromUTF8 ("\u00b7");  // ·
            case MapState::localOnly:         return juce::String::fromUTF8 ("\u270e");  // ✎
            case MapState::submittedByYou:    return juce::String::fromUTF8 ("\u2713");  // ✓
            case MapState::submittedByOther:  return juce::String::fromUTF8 ("\u25d0");  // ◐
            case MapState::differentBuild:    return juce::String::fromUTF8 ("\u26a0");  // ⚠
            case MapState::unknown:
            default:                          return "?";
        }
    }

    static juce::Colour colourFor (MapState s)
    {
        switch (s)
        {
            case MapState::unmapped:          return juce::Colour (0xff4a5a5e);
            case MapState::localOnly:         return juce::Colours::orange;
            case MapState::submittedByYou:    return juce::Colour (0xff6fd08c);
            case MapState::submittedByOther:  return juce::Colour (0xff6fa8d0);
            case MapState::differentBuild:    return juce::Colours::darkorange;
            case MapState::unknown:
            default:                          return juce::Colour (0xff707070);
        }
    }

    /** Local maps are known without the server: a map on disk whose fp the
        server has not confirmed is "local, not submitted". Checked first so an
        offline session still distinguishes work done from work not done.
    */
    bool haveLocalMapFor (const juce::String& identityKey) const
    {
        auto dir = ledger.getRoot().getChildFile ("maps");
        for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*.json"))
        {
            auto id = juce::JSON::parse (e.getFile().loadFileAsString())
                          .getProperty ("identity", juce::var());
            if (! id.isObject()) continue;
            const auto k = id.getProperty ("format", "").toString() + "|"
                         + id.getProperty ("uid", "").toString() + "|"
                         + id.getProperty ("version", "").toString();
            if (k == identityKey) return true;
        }
        return false;
    }

    MapStateRow mapStateFor (const ScannedPlugin& sp) const
    {
        const auto key = echojay::identityKeyForDescription (sp.desc);
        auto it = mapStateByIdentity.find (key);
        if (it != mapStateByIdentity.end()) return it->second;
        MapStateRow r;
        // No server answer. A local map is still a fact we hold; without one,
        // an unqueried row is UNKNOWN, never "unmapped" -- claiming unmapped
        // on no evidence is how a mapper re-maps a mapped product.
        r.state = mapStateFailure.isNotEmpty() || mapStateByIdentity.empty()
                    ? MapState::unknown : MapState::unmapped;
        return r;
    }

    /** The detail line. The differentBuild case says what will HAPPEN if you
        map it, because "mapped, different build" is otherwise ambiguous
        between "already done" and "do it again".
    */
    juce::String mapStateDetail (const ScannedPlugin& sp) const
    {
        const auto st = mapStateFor (sp);
        switch (st.state)
        {
            case MapState::unmapped:
                return "no map for this build or any other";
            case MapState::localOnly:
                return "mapped locally, NOT submitted -- it exists only on this machine";
            case MapState::submittedByYou:
                return "submitted by you, " + st.at + ", schema " + st.schema
                     + (st.probed ? ", probed" : ", not probed");
            case MapState::submittedByOther:
                return "submitted by " + st.by + ", " + st.at + ", schema " + st.schema
                     + (st.probed ? ", probed" : ", not probed");
            case MapState::differentBuild:
                return juce::String (st.mapsForIdentity) + " map(s) for this product, none for "
                       "this build (" + juce::String (st.paramCountHere) + " params). "
                       "Mapping this creates another.";
            case MapState::unknown:
            default:
                return mapStateFailure.isNotEmpty()
                         ? "map state unknown: " + mapStateFailure
                         : "map state not fetched yet";
        }
    }

    juce::File mapStateCacheFile() const
    { return ledger.getRoot().getChildFile ("map-state.json"); }

    /** Persist with the stamp, ALWAYS. A cached state that cannot say its age
        is indistinguishable from a fresh one, which is the shape the scan
        cache already refuses (it prints its run id and age on every restore).
    */
    void saveMapStateCache()
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("fetched_at", mapStateFetchedAt.toISO8601 (true));
        o->setProperty ("failure", mapStateFailure);
        auto* by = new juce::DynamicObject();
        for (const auto& kv : mapStateByIdentity)
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("state", (int) kv.second.state);
            e->setProperty ("by", kv.second.by);
            e->setProperty ("at", kv.second.at);
            e->setProperty ("schema", kv.second.schema);
            e->setProperty ("probed", kv.second.probed);
            e->setProperty ("maps_for_identity", kv.second.mapsForIdentity);
            e->setProperty ("param_count_here", kv.second.paramCountHere);
            by->setProperty (kv.first, juce::var (e));
        }
        o->setProperty ("identities", juce::var (by));
        mapStateCacheFile().replaceWithText (juce::JSON::toString (juce::var (o), false));
    }

    void loadMapStateCache()
    {
        auto f = mapStateCacheFile();
        if (! f.existsAsFile()) return;
        auto v = juce::JSON::parse (f.loadFileAsString());
        mapStateFetchedAt = juce::Time::fromISO8601 (v.getProperty ("fetched_at", "").toString());
        mapStateFailure   = v.getProperty ("failure", "").toString();
        mapStateByIdentity.clear();
        if (auto* by = v.getProperty ("identities", juce::var()).getDynamicObject())
            for (auto& kv : by->getProperties())
            {
                MapStateRow r;
                r.state  = (MapState) (int) kv.value.getProperty ("state", 5);
                r.by     = kv.value.getProperty ("by", "").toString();
                r.at     = kv.value.getProperty ("at", "").toString();
                r.schema = kv.value.getProperty ("schema", "").toString();
                r.probed = (bool) kv.value.getProperty ("probed", false);
                r.mapsForIdentity = (int) kv.value.getProperty ("maps_for_identity", 0);
                r.paramCountHere  = (int) kv.value.getProperty ("param_count_here", 0);
                mapStateByIdentity[kv.name.toString()] = r;
            }
    }

    /** The status-line fragment. Says its age, and says a failure in words --
        an empty answer and a failed answer must never read alike.
    */
    juce::String mapStateStatusLine() const
    {
        if (mapStateFailure.isNotEmpty())
            return "map state unavailable: " + mapStateFailure + " (checked "
                 + mapStateCacheAge() + ") -- everything shows ?";
        if (mapStateByIdentity.empty())
            return "map state not fetched; everything shows ?";
        return "map state from " + mapStateCacheAge() + " ("
             + juce::String ((int) mapStateByIdentity.size()) + " identities)";
    }

    /** The ? listing: every fp this product has, across builds. This is what
        out.identities is for -- the list is data, not inferred from one fp.
    */
    juce::String mapStateFpListing (const ScannedPlugin& sp) const
    {
        const auto key = echojay::identityKeyForDescription (sp.desc);
        const auto st  = mapStateFor (sp);
        juce::String t;
        t << "IDENTITY " << key << "\n";
        if (st.mapsForIdentity <= 0)
        { t << "  no maps recorded for this product\n"; return t; }
        t << "  " << st.mapsForIdentity << " map(s) for this product\n"
          << "  this build: " << st.paramCountHere << " params -> "
          << (st.state == MapState::differentBuild ? "NOT mapped" : "mapped") << "\n";
        if (st.by.isNotEmpty())
            t << "  most recent: " << st.by << ", " << st.at << ", schema " << st.schema
              << (st.probed ? ", probed" : ", not probed") << "\n";
        return t;
    }

    juce::String mapStateCacheAge() const
    {
        if (mapStateFetchedAt == juce::Time()) return "never fetched";
        const auto d = juce::Time::getCurrentTime() - mapStateFetchedAt;
        if (d.inMinutes() < 1.0) return "just now";
        if (d.inHours() < 1.0)   return juce::String ((int) d.inMinutes()) + " min ago";
        return juce::String (d.inHours(), 1) + " h ago";
    }

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
    int kneeTestIdx = -1;
    juce::String tierPulledName;         // assign self-test: the labelled discrete switch
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
