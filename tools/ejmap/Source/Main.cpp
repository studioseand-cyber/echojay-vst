/*
  Main.cpp

  The title bar carries the version and the git short hash, because the adopted
  convention on this project is that the on-screen version is the only proof of
  what is loaded. Version numbers do not indicate lineage: each branch counts up
  from its own base.
*/

#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "EjmapSchema.h"

// Generated on every build by cmake/StampBuildInfo.cmake. Carries the git short
// hash of the commit actually compiled, plus "-dirty" when tracked files were
// modified. Deliberately has no #ifndef fallback: a build with no stamp should
// fail to compile rather than quietly claim a version it cannot know.
#include "EjmapBuildInfo.h"

class EjmapApplication  : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "ejmap"; }
    const juce::String getApplicationVersion() override { return EJMAP_VERSION; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // --ledger-root DIR         write the ledger somewhere throwaway
        // --selftest-reentry ID     scripted double-click proof, then quit
        // preserveQuotedStrings keeps the quotes IN the token, and JUCE quotes
        // any argument containing a space when it rebuilds the command line. A
        // VST3 path like "TDR SlickEQ M.vst3" therefore arrives wrapped in
        // literal quote characters and matches nothing in the quarantine.
        auto args = juce::StringArray::fromTokens (commandLine, true);
        for (auto& a : args)
            a = a.unquoted();
        juce::File ledgerRoot;
        juce::String selfTestId;
        bool cacheTest = false;
        juce::String releaseId, quarantineId, quarantineReason;

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--ledger-root" && i + 1 < args.size())
                ledgerRoot = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
            else if (args[i] == "--selftest-reentry" && i + 1 < args.size())
                selfTestId = args[++i];
            else if (args[i] == "--selftest-cache")
                cacheTest = true;
            else if (args[i] == "--release-quarantine" && i + 1 < args.size())
                releaseId = args[++i];
            else if (args[i] == "--quarantine" && i + 2 < args.size())
                { quarantineId = args[i + 1]; quarantineReason = args[i + 2]; i += 2; }
        }

        // Headless release, so a quarantine can be undone through the same code
        // path the button uses rather than by hand-editing quarantine.json.
        if (releaseId.isNotEmpty())
        {
            ejmap::Ledger l (ledgerRoot);
            const bool was = l.isQuarantined (releaseId);
            l.releaseFromQuarantine (releaseId);
            std::cout << "release-quarantine: " << releaseId
                      << (was ? " -> released" : " -> was not quarantined") << std::endl;
            quit();
            return;
        }

        if (quarantineId.isNotEmpty())
        {
            ejmap::Ledger l (ledgerRoot);
            l.quarantine (quarantineId, quarantineReason);
            std::cout << "quarantine: " << quarantineId << " -> " << quarantineReason << std::endl;
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (buildTitle(), ledgerRoot);

        // Read back off the constructed window, not off buildTitle(). The title
        // bar is the only proof of what is running, and on a machine where the
        // shell cannot screenshot (Screen Recording denied) this line is the
        // only way to assert it against the live object rather than the source.
        std::cout << "ejmap window title: " << mainWindow->getName() << std::endl;

        if (cacheTest && mainWindow->getMain() != nullptr)
            mainWindow->getMain()->selfTestScanCache();
        else if (selfTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
            mainWindow->getMain()->selfTestReentry (selfTestId);
    }

    void shutdown() override { mainWindow = nullptr; }

    void systemRequestedQuit() override { quit(); }

private:
    static juce::String buildTitle()
    {
        return juce::String ("ejmap ") + EJMAP_VERSION
             + "  (" + EJMAP_GIT_HASH + ")"
             + "  schema " + ejmap::kMapSchemaString;
    }

    class MainWindow  : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, juce::File ledgerRoot)
            : DocumentWindow (name,
                              juce::Colour (0xff10141c),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            main = new ejmap::MainComponent (ledgerRoot);
            setContentOwned (main, true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

        ejmap::MainComponent* getMain() const noexcept { return main; }

    private:
        ejmap::MainComponent* main = nullptr;   // owned by the window's content
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (EjmapApplication)
