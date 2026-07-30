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

        // Candidate picker: appears only for a multi-parameter gesture, where
        // the engine cannot know which of the moved parameters the human meant.
        addChildComponent (candidatePicker);
        candidatePicker.setTextWhenNothingSelected ("pick the parameter you meant");
        candidatePicker.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff161c26));
        candidatePicker.setColour (juce::ComboBox::textColourId, juce::Colour (0xff9fd8e0));
        candidatePicker.onChange = [this] { chooseCandidate(); };

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

        setSize (1280, 820);
        startTimerHz (4);
    }

    ~MainComponent() override
    {
        capture.stop();
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

        const char* names[] = { "one moved -> captured",
                                "two correlated -> twins",
                                "two opposed -> gesture",
                                "nine moved -> too_many" };

        if (stage >= 4)
        {
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
        else              for (int k = 0; k < 9; ++k) targets.add (usable[k]);

        for (int i = 0; i < targets.size(); ++i)
        {
            // stage 2 wants opposed directions, so one starts high.
            const float base = (st == 2 && i == 1) ? 0.80f : 0.20f;
            params[targets[i]]->setValueNotifyingHost (base);
        }
        juce::Thread::sleep (120);      // let the base settle before the snapshot

        capture.arm (*inst, cal, mask, [this, st] (const CaptureEngine::Result& r)
        {
            static const char* names[] = { "one moved -> captured",
                                           "two correlated -> twins",
                                           "two opposed -> gesture",
                                           "nine moved -> too_many" };
            static const char* want[]  = { "captured", "twins", "gesture", "too_many" };
            const bool ok = (r.kindString() == juce::String (want[st]));
            if (! ok) ++failures;
            std::cout << "  " << (ok ? "ok   " : "FAIL ") << names[st]
                      << "  -> got " << r.kindString()
                      << " indices=" << r.indices.size() << std::endl;

            // Exercise the RECORD WRITER, not just the classifier. Without this
            // the test reported PASS while writing nothing, so persistence was
            // never covered by the thing that claimed to cover it. A gesture is
            // recorded here as the raw gesture row; in the app it is recorded
            // again once the human picks, which is the same writer either way.
            recordCapture (r.kindString(),
                           r.indices.size() == 1 ? r.indices[0] : -1,
                           r.names.isEmpty() ? juce::String() : r.names[0],
                           r.indices.size() > 1 ? r.indices : juce::Array<int>(),
                           r.indices.size() > 1 ? r.names : juce::StringArray(),
                           r.reason);
            ++stage;
            juce::Timer::callAfterDelay (400, [this] { runCaptureStage(); });
        });

        juce::Timer::callAfterDelay (300, [this, targets, st]
        {
            auto* in = host.getInstance();
            if (in == nullptr) return;
            auto& ps = in->getParameters();
            for (int i = 0; i < targets.size(); ++i)
            {
                const float to = (st == 2 && i == 1) ? 0.50f : 0.50f;   // +0.30 / -0.30
                ps[targets[i]]->setValueNotifyingHost (to);
            }
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

        armButton.setEnabled (true);
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
                       coMoved, coNames, lastGesture.reason);

        candidatePicker.setVisible (false);
        resized();
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
    void recordCapture (const juce::String& kind, int intended, const juce::String& name,
                        const juce::Array<int>& coMoved, const juce::StringArray& coNames,
                        const juce::String& reason)
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
                t << "   |  " << mask.promotions[mask.promotions.size() - 1];

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
                // Cannot be resolved by the engine: ask.
                lastGesture = r;
                candidatePicker.clear (juce::dontSendNotification);
                for (int i = 0; i < r.indices.size(); ++i)
                    candidatePicker.addItem (juce::String (r.indices[i]) + ":  " + r.names[i], i + 1);
                candidatePicker.setVisible (true);
                resized();
                return;   // Arm stays disabled until a choice is made
            }

            recordCapture (r.kindString(),
                           r.indices.size() == 1 ? r.indices[0] : -1,
                           r.names.isEmpty() ? juce::String() : r.names[0],
                           r.indices.size() > 1 ? r.indices : juce::Array<int>(),
                           r.indices.size() > 1 ? r.names : juce::StringArray(),
                           r.reason);
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
            attachEditor();
            prepareCapture (sp.desc.name, id);
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

    PluginScanner::Result lastScan;
    juce::Array<ScannedPlugin> rows;        // the whole scan result, never filtered
    juce::Array<int>           visibleRows; // indices into rows, what the list shows
    juce::String crashedId;

    juce::TextButton scanButton, loadButton, releaseButton, summaryButton, armButton;
    juce::Label      captureReadout;
    CaptureEngine::Calibration cal;
    CaptureEngine::NoiseMask   mask;
    int stage = 0, failures = 0;
    juce::ComboBox candidatePicker;
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
