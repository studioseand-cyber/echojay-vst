// ui_observe — round C Step 0 (17 Sep 2026): observations on the real editor.
// (1)(2) Build pressed with the CHAT tab current: which tab is current before
// and after, whether the chain panel / inline holder is visible, and every
// visible component whose bounds intersect the chat transcript.
// (4) the brief card laid out at the chat box width: does any child extend
// past the card's right edge?
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EedDeviceRegistry.h"
#include <cstdio>

struct EchoJayTabStripTestAccess
{
    static void toChat (EchoJayEditor& e)   { e.switchToTab (EchoJayEditor::Tab::Chat, true); }
    static void toChain (EchoJayEditor& e)  { e.switchToTab (EchoJayEditor::Tab::Chain, true); }
    static int  tab (EchoJayEditor& e)      { return (int) e.currentTab; }
    static juce::Component& panel (EchoJayEditor& e) { return e.chainListPanel; }
    static juce::Component& inlineHolder (EchoJayEditor& e) { return e.chainListPanel.inlineHolder; }
    static juce::Component& chatScroll (EchoJayEditor& e) { return e.chatScroll; }
    static void build (EchoJayEditor& e, const juce::String& json) { e.loadChainFromJson (json, true); }
    static juce::Rectangle<int> chatBox (EchoJayEditor& e) { return e.chatBoxRect_; }
    // the brief card is a private type: build + lay it out from inside the friend
    static juce::String cardProbe (EchoJayEditor& e, int w, int& outOverflow, int& outMaxRight, int& outH)
    {
        auto& card = e.briefCard_;
        std::vector<EchoJayEditor::BriefCard::Q> qs;
        for (int q = 0; q < 3; ++q) { EchoJayEditor::BriefCard::Q qq; qq.axis = "q" + juce::String (q); qq.text = "Question " + juce::String (q + 1) + " with a reasonably long prompt text here?"; for (int o = 0; o < 4; ++o) { qq.labels.add ("Option " + juce::String (o + 1) + ": a fairly long option label that takes space"); qq.intents.add ("i" + juce::String (o)); qq.vars.add (juce::var (o)); } qs.push_back (qq); }
        card.reset (std::move (qs), "Lead Vocal", true);
        outH = card.preferredHeight();
        card.setBounds (e.chatBoxRect_.getX(), e.chatBoxRect_.getY() - outH, w, outH); card.setVisible (true);
        outOverflow = 0; outMaxRight = 0;
        std::function<void(juce::Component&, int)> walk = [&] (juce::Component& c, int offX) { for (auto* k : c.getChildren()) { if (! k->isVisible()) continue; const int r = offX + k->getRight(); outMaxRight = juce::jmax (outMaxRight, r); if (r > w) ++outOverflow; walk (*k, offX + k->getX()); } };
        walk (card, 0);
        return "children=" + juce::String (card.getNumChildComponents()) + " bounds=" + card.getBounds().toString();
    }
};
namespace
{
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
const char* tabName (int t) { switch (t) { case 0: return "Dashboard"; case 1: return "Visual"; case 2: return "Meters"; case 3: return "Chat"; case 4: return "Compare"; case 5: return "Link"; case 6: return "Chain"; case 7: return "Settings"; } return "?"; }
void listOverlapping (juce::Component& root, juce::Rectangle<int> area, int depth, juce::Component* skip)
{
    for (auto* c : root.getChildren())
    {
        if (c == skip || ! c->isVisible()) continue;
        const auto b = root.getLocalArea (c, c->getLocalBounds()); // child bounds in root coords? use screen-relative via getBoundsInParent chain
        juce::ignoreUnused (b);
        const auto bp = c->getBounds();
        if (depth == 0 && bp.intersects (area))
            std::printf ("     visible over transcript: %-28s bounds=%d,%d %dx%d\n", c->getName().isNotEmpty() ? c->getName().toRawUTF8() : typeid (*c).name(), bp.getX(), bp.getY(), bp.getWidth(), bp.getHeight());
    }
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_uiobs_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    // LOGGED IN so the editor constructs on the MAIN screen (real geometry), never the login screen:
    // a fake auth.json in the isolated state root (no network call can succeed; the token is never sent anywhere real).
    { auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json"); af.getParentDirectory().createDirectory();
      af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\",\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,\"credits\":999,\"displayName\":\"UI\",\"messagesUsedToday\":0,\"usageDate\":\"2026-09-17\",\"autoDialMode\":false,\"dialWritesBlocked\":false}"); }
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    ed->setSize (2000, 1100);
    using A = EchoJayTabStripTestAccess;
    A::toChat (*ed); pumpMs (80);
    std::printf ("== (1)(2) Build with the CHAT tab current ==\n");
    std::printf ("  before: tab=%s panel.visible=%d inlineHolder.visible=%d chatScroll.visible=%d chatScroll.bounds=%s\n", tabName (A::tab (*ed)), (int) A::panel (*ed).isVisible(), (int) A::inlineHolder (*ed).isVisible(), (int) A::chatScroll (*ed).isVisible(), A::chatScroll (*ed).getBounds().toString().toRawUTF8());
    const juce::String json = "{\"chain\":[{\"name\":\"EchoJay EQ\",\"role\":\"eq\",\"settings\":\"\"}]}";
    A::build (*ed, json);
    std::printf ("  right after loadChainFromJson: tab=%s panel.visible=%d\n", tabName (A::tab (*ed)), (int) A::panel (*ed).isVisible());
    pumpMs (600);
    std::printf ("  600 ms later: tab=%s panel.visible=%d panel.bounds=%s inlineHolder.visible=%d inlineHolder.bounds=%s slots=%d\n", tabName (A::tab (*ed)), (int) A::panel (*ed).isVisible(), A::panel (*ed).getBounds().toString().toRawUTF8(), (int) A::inlineHolder (*ed).isVisible(), A::inlineHolder (*ed).getBounds().toString().toRawUTF8(), proc.getChainHost().getNumSlots());
    listOverlapping (*ed, A::chatScroll (*ed).getBounds(), 0, &A::chatScroll (*ed));
    std::printf ("== (4) BriefCard at the chat box width ==\n");
    A::toChat (*ed); pumpMs (50);
    const auto box = A::chatBox (*ed);
    std::printf ("  editor=%dx%d chatBoxRect_=%s chatScroll=%s\n", ed->getWidth(), ed->getHeight(), box.toString().toRawUTF8(), A::chatScroll (*ed).getBounds().toString().toRawUTF8());
    int ovf = 0, maxR = 0, h = 0; const int w = box.getWidth();
    const auto info = A::cardProbe (*ed, w, ovf, maxR, h);
    std::printf ("  card width=%d preferredHeight=%d %s children overflowing right edge=%d maxChildRight=%d (card-relative)\n", w, h, info.toRawUTF8(), ovf, maxR);
    return 0;
}
