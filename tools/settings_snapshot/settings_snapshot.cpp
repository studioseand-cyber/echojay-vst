/*  settings_snapshot (round 60): renders the MAIN editor's Settings view
    offline to PNG - the standing rule ("a layout's correctness is a claim
    until a render shows it") applied to the plugin editor, not only the
    pitch device's. Two sizes; the settings viewport scrolled to the top and
    to the region below YOUR PLUGINS. Usage: settings_snapshot <outdir> <tag>
*/
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>

struct EchoJayTabStripTestAccess   // the friend PluginEditor.h already declares
{
    static void toSettings (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Settings, true); }
    static juce::Viewport& viewport (EchoJayEditor& e) { return e.settingsViewport_; }
    // The prompt overlays (onboarding, plugin review, update) sit on top of the
    // whole window in a fresh session; hidden here so the panel itself is seen.
    static void hideOverlays (EchoJayEditor& e) { e.onboardingOverlay_.setVisible (false); e.reviewOverlay.setVisible (false); e.updateOverlay.setVisible (false); }
};

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf ("usage: settings_snapshot <outdir> <tag>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File out (argv[1]); out.createDirectory();
    const juce::String tag (argv[2]);
    const int sizes[][2] = { { 1100, 720 }, { 900, 600 } };
    for (const auto& sz : sizes)
    {
        EchoJayProcessor p;
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* e = dynamic_cast<EchoJayEditor*> (ed.get());
        if (e == nullptr) { std::printf ("no editor\n"); return 1; }
        ed->setSize (sz[0], sz[1]);
        EchoJayTabStripTestAccess::toSettings (*e);
        EchoJayTabStripTestAccess::hideOverlays (*e);
        ed->resized();
        auto& vp = EchoJayTabStripTestAccess::viewport (*e);
        const int scrolls[] = { 0, 260, 520 };
        for (int sc : scrolls)
        {
            vp.setViewPosition (0, juce::jmin (sc, juce::jmax (0, vp.getViewedComponent() != nullptr ? vp.getViewedComponent()->getHeight() - vp.getHeight() : 0)));
            juce::Image img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
            juce::File f = out.getChildFile (tag + "_settings_" + juce::String (sz[0]) + "x" + juce::String (sz[1]) + "_scroll" + juce::String (vp.getViewPositionY()) + ".png");
            f.deleteFile(); juce::FileOutputStream os (f); juce::PNGImageFormat png; png.writeImageToStream (img, os);
            std::printf ("  wrote %s (%dx%d, content height %d)\n", f.getFileName().toRawUTF8(), img.getWidth(), img.getHeight(), vp.getViewedComponent() != nullptr ? vp.getViewedComponent()->getHeight() : -1);
        }
    }
    return 0;
}
