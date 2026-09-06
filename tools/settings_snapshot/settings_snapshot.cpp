/*  settings_snapshot (round 60): the MAIN editor's Settings view, offline.
      - THE CHAIN-LIST COUNT: scanner rows, the chain feed's entry count, and
        how many scanned rows the SHIPPING withhold predicate excludes - the
        number that must not move when a Settings section is removed.
      - RENDERS at every supported size (setResizeLimits 900x580 .. 1800x1200),
        overlays hidden, plus a BOUNDS AUDIT of every control below the removed
        block down to the bottom row: visible, inside the content, reachable.
    Usage: settings_snapshot <outdir> <tag>
*/
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <set>

struct EchoJayTabStripTestAccess   // the friend PluginEditor.h already declares
{
    static void toSettings (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Settings, true); }
    static juce::Viewport& viewport (EchoJayEditor& e) { return e.settingsViewport_; }
    static void hideOverlays (EchoJayEditor& e) { e.onboardingOverlay_.setVisible (false); e.reviewOverlay.setVisible (false); e.updateOverlay.setVisible (false); }
    struct Named { const char* name; juce::Component* c; };
    static std::vector<Named> below (EchoJayEditor& e)
    {
        return { { "UI SCALE combo", &e.uiScaleCombo }, { "CHAIN SUGGESTIONS toggle", &e.autoDialToggle },
                 { "YOUR PLUGINS field / Scan", &e.settingsScanBtn }, { "View all", &e.viewAllPluginsBtn },
                 { "Save", &e.saveSettingsBtn }, { "Manual", &e.settingsManualBtn }, { "saved label", &e.settingsSavedLabel },
                 { "Help & Support", &e.settingsHelpBtn }, { "Log Out", &e.logoutBtn } };
    }
    static juce::Component& content (EchoJayEditor& e) { return e.settingsContent_; }
};

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf ("usage: settings_snapshot <outdir> <tag>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File out (argv[1]); out.createDirectory();
    const juce::String tag (argv[2]);
    int fails = 0;
    {
        EchoJayProcessor p;
        auto& ch = p.getChainHost();
        // the scanner loads its cache on a thread (PluginProcessor.cpp:378): wait for the rows
        std::vector<ScannedPlugin> rows;
        for (int i = 0; i < 100 && rows.empty(); ++i) { juce::Thread::sleep (100); rows = p.getPluginScanner().getPlugins(); }
        // The removed panel's core test, by NAME over the enabled scanner rows: a
        // name that resolves in NEITHER format is "cannot be used in this host".
        std::set<juce::String> names, unusable;
        for (const auto& sp : rows) if (sp.enabled) names.insert (sp.name);
        for (const auto& n : names)
        {
            auto why = ChainHost::WithholdReason::None;
            const bool au = ch.resolveByName (n, "AudioUnit", nullptr, &why).name.isNotEmpty();
            const bool v3 = ch.resolveByName (n, "VST3", nullptr, &why).name.isNotEmpty();
            if (! au && ! v3) unusable.insert (n);
        }
        std::printf ("CHAIN-LIST COUNT [%s]: scanner rows %d (enabled names %d) | chain feed entries (getNumPlugins) %d | AU-host collapsed view %d | enabled names resolving in NEITHER format (excluded from the chain list) %d\n",
                     tag.toRawUTF8(), (int) rows.size(), (int) names.size(), ch.getNumPlugins(), ch.getFilteredPlugins ({}, "AudioUnit", true).size(), (int) unusable.size());
    }
    const int sizes[][2] = { { 900, 580 }, { 1000, 650 }, { 1100, 720 }, { 1400, 900 }, { 1800, 1200 } };
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
        auto& content = EchoJayTabStripTestAccess::content (*e);
        const int contentH = content.getHeight(), viewH = vp.getHeight();
        std::printf ("\n== %s %dx%d: settings content %d px tall, viewport %d px -> %s ==\n", tag.toRawUTF8(), sz[0], sz[1], contentH, viewH, contentH > viewH ? "SCROLLS" : "fits without scrolling");
        for (const auto& n : EchoJayTabStripTestAccess::below (*e))
        {
            const auto b = n.c->getBounds();
            const bool vis = n.c->isVisible(), inside = content.getLocalBounds().contains (b), reachable = b.getBottom() <= contentH;
            const bool ok = vis && inside && reachable && ! b.isEmpty();
            std::printf ("  [%s] %-28s bounds %4d,%4d %4dx%-3d %s%s%s\n", ok ? "ok" : "FAIL", n.name, b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                         vis ? "visible" : "NOT VISIBLE", inside ? "" : " OUTSIDE-CONTENT", reachable ? "" : " UNREACHABLE");
            if (! ok) ++fails;
        }
        const int scrolls[] = { 0, juce::jmax (0, contentH - viewH) };
        for (int sc : scrolls)
        {
            vp.setViewPosition (0, sc);
            if (sc == 0 || vp.getViewPositionY() > 0)
            {
                juce::Image img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f);
                juce::File f = out.getChildFile (tag + "_settings_" + juce::String (sz[0]) + "x" + juce::String (sz[1]) + "_scroll" + juce::String (vp.getViewPositionY()) + ".png");
                f.deleteFile(); juce::FileOutputStream os (f); juce::PNGImageFormat png; png.writeImageToStream (img, os);
                std::printf ("  wrote %s\n", f.getFileName().toRawUTF8());
            }
        }
    }
    std::printf ("\n%s: %d control%s failed the bounds audit\n", fails == 0 ? "ALL CONTROLS PRESENT" : "AUDIT FAILURES", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
