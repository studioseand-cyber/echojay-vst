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
        auto args = juce::StringArray::fromTokens (commandLine, true);
        juce::File ledgerRoot;
        juce::String selfTestId;
        bool cacheTest = false;

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--ledger-root" && i + 1 < args.size())
                ledgerRoot = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
            else if (args[i] == "--selftest-reentry" && i + 1 < args.size())
                selfTestId = args[++i];
            else if (args[i] == "--selftest-cache")
                cacheTest = true;
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
