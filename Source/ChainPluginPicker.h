#pragma once
// ---------------------------------------------------------------------------
// ChainPluginPicker (P13, 17 Aug 2026): the searchable "add plugin to chain"
// picker, shared by the main plugin's "+" and the Link's "+". Replaces the
// 1400-row PopupMenu that had no way in but scrolling.
//
//   type to filter: every space-separated token must match the name, the
//   vendor, the format tag ("au", "vst3", "ej"), or, for built-ins, the
//   category and the description ("reverb" reaches EchoJay Reverb,
//   "echojay" reaches all of them)
//   keyboard first: focus lands in the field on open, up/down move the
//   selection (wrapping), Return racks the selection, Escape closes.
//   Mouse: click selects, double-click racks.
//
// Shown in a CallOutBox pointing at the target; the box owns the picker and
// dismisses itself. onPick fires at most once, after the box is gone.
// ---------------------------------------------------------------------------
#include <JuceHeader.h>
#include "ChainHost.h"
#include "NativeClip.h"   // EchoJay_NSLog — instant-dismiss diagnosis (21 Aug 2026)

class ChainPluginPicker : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::TextEditor::Listener
{
    // A single-line TextEditor consumes up/down (caret to start/end of line)
    // before any KeyListener sees them, so navigation keys are taken here
    // first and everything else goes to the editor.
    struct SearchField : juce::TextEditor
    {
        std::function<bool(const juce::KeyPress&)> onNavKey;
        bool keyPressed(const juce::KeyPress& k) override
        {
            if (onNavKey && onNavKey(k)) return true;
            return juce::TextEditor::keyPressed(k);
        }
    };
public:
    using PickFn = std::function<void(const juce::PluginDescription&)>;

    ChainPluginPicker(juce::Array<juce::PluginDescription> items, PickFn onPick)
        : all_(std::move(items)), onPick_(std::move(onPick))
    {
        // Instant-dismiss diagnosis (21 Aug 2026): the CallOutBox's OWN
        // dismissal (outside click, focus fight) never passes through
        // dismiss() below — the DESTRUCTOR is the one witness every death
        // path shares. Interleave these EJPicker lines with EJPanel:
        // rebuild in the unified log to name the actor.
        EchoJay_NSLog("EJPicker: shown");
        setSize(440, 500);
        search_.setTextToShowWhenEmpty("Search plugins: name, vendor, AU / VST3, or what a built-in does", juce::Colour(0xff6b7280));
        search_.setFont(juce::Font(juce::FontOptions(13.0f)));
        search_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0e1020));
        search_.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        search_.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2a4d7a));
        search_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff22d3ee));
        search_.setSelectAllWhenFocused(true);
        search_.addListener(this);
        search_.onNavKey = [this](const juce::KeyPress& k) { return navKey(k); };
        addAndMakeVisible(search_);

        list_.setModel(this);
        list_.setRowHeight(24);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff141626));
        list_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff2a4d7a));
        list_.setOutlineThickness(1);
        list_.setWantsKeyboardFocus(false);   // keys stay in the field
        addAndMakeVisible(list_);

        refilter();
    }

    // Focus lands in the field once the box has put us on screen
    void parentHierarchyChanged() override
    {
        if (isShowing() && !focused_)
        {
            focused_ = true;
            juce::MessageManager::callAsync([sp = juce::Component::SafePointer<ChainPluginPicker>(this)]
            {
                if (sp != nullptr) sp->search_.grabKeyboardFocus();
            });
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8);
        search_.setBounds(r.removeFromTop(28));
        r.removeFromTop(6);
        hint_ = r.removeFromBottom(16);
        r.removeFromBottom(4);
        list_.setBounds(r);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0b0d18));
        g.setColour(juce::Colour(0xff8a94a6));
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        const juce::String left = juce::String(shown_.size()) + " of " + juce::String(all_.size())
                                + (shown_.size() == all_.size() ? " plugins" : " match");
        g.drawText(left, hint_, juce::Justification::centredLeft);
        g.drawText("up/down select   Return add   Esc close", hint_, juce::Justification::centredRight);
    }

    // Show in a CallOutBox pointing at `target`, EMBEDDED in `parent` (the
    // plugin editor). PARENTED ON PURPOSE (21 Aug 2026): a desktop-parented
    // CallOutBox inside the AU hosting XPC process is killed ~200ms after
    // opening by JUCE's foreground watchdog (CallOutBoxCallback::
    // timerCallback dismisses when isForegroundOrEmbeddedProcess is false,
    // and a background XPC process is never the foreground app). An embedded
    // box passes that check. Fits: the box (~470x540 with chrome) is inside
    // both editors' 900x580 minimum. Never pass nullptr here again.
    static void show(juce::Component& target, juce::Component& parent,
                     juce::Array<juce::PluginDescription> items, PickFn onPick)
    {
        auto picker = std::make_unique<ChainPluginPicker>(std::move(items), std::move(onPick));
        auto* raw = picker.get();
        auto& box = juce::CallOutBox::launchAsynchronously(
            std::move(picker),
            parent.getLocalArea(&target, target.getLocalBounds()),
            &parent);
        box.setDismissalMouseClicksAreAlwaysConsumed(true);
        raw->box_ = &box;
    }

private:
    // ---- filtering ----
    static juce::String tagFor(const juce::PluginDescription& d)
    {
        if (ChainHost::isBuiltinDescription(d)) return "EJ";
        return d.pluginFormatName == "AudioUnit" ? "AU" : "VST3";
    }
    static juce::String haystack(const juce::PluginDescription& d)
    {
        // name, vendor, tag, and for built-ins the category and description
        juce::String h = d.name + " " + d.manufacturerName + " " + tagFor(d);
        if (ChainHost::isBuiltinDescription(d))
            h << " " << d.category << " " << d.descriptiveName << " echojay built-in";
        return h.toLowerCase();
    }
    void refilter()
    {
        const juce::String q = search_.getText().trim().toLowerCase();
        juce::StringArray tokens;
        tokens.addTokens(q, " ", "");
        tokens.removeEmptyStrings();
        shown_.clear();
        for (int i = 0; i < all_.size(); ++i)
        {
            if (tokens.isEmpty()) { shown_.add(i); continue; }
            const auto h = haystack(all_.getReference(i));
            bool ok = true;
            for (const auto& t : tokens) if (!h.contains(t)) { ok = false; break; }
            if (ok) shown_.add(i);
        }
        list_.updateContent();
        if (!shown_.isEmpty()) list_.selectRow(0);
        else list_.deselectAllRows();
        list_.repaint();
        repaint();
    }

    // ---- ListBoxModel ----
    int getNumRows() override { return shown_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (row < 0 || row >= shown_.size()) return;
        const auto& d = all_.getReference(shown_[row]);
        if (sel) g.fillAll(juce::Colour(0xff2a4d7a));
        const bool isBuiltin = ChainHost::isBuiltinDescription(d);
        const bool isAU = d.pluginFormatName == "AudioUnit";
        const juce::Colour tagCol = isBuiltin ? juce::Colour(0xff06b6d4) : isAU ? juce::Colour(0xff3a7a3a) : juce::Colour(0xff2a4d7a);
        const int tagW = 36;
        g.setColour(tagCol.withAlpha(0.7f));
        g.fillRoundedRectangle((float)(w - tagW - 6), (float)(h / 2 - 8), (float) tagW, 16.0f, 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
        g.drawText(tagFor(d), w - tagW - 6, h / 2 - 8, tagW, 16, juce::Justification::centred);
        g.setColour(sel ? juce::Colours::white : juce::Colour(0xffd6d9e0));
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        const int nameW = juce::jmin(w - tagW - 20, 260);
        g.drawText(d.name, 8, 0, nameW, h, juce::Justification::centredLeft, true);
        g.setColour(sel ? juce::Colour(0xffcfe3ff) : juce::Colour(0xff8a94a6));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        const juce::String sub = isBuiltin ? d.category : d.manufacturerName;
        g.drawText(sub, 8 + nameW + 8, 0, w - tagW - 20 - nameW - 16, h, juce::Justification::centredLeft, true);
    }
    void listBoxItemClicked(int, const juce::MouseEvent&) override { search_.grabKeyboardFocus(); }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { pickRow(row); }
    void returnKeyPressed(int row) override { pickRow(row); }

    // ---- keys, from the field ----
    bool navKey(const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            const int n = shown_.size();
            if (n > 0)
            {
                int cur = list_.getSelectedRow();
                if (cur < 0) cur = 0;
                cur = (cur + (k == juce::KeyPress::downKey ? 1 : n - 1)) % n;
                list_.selectRow(cur);
            }
            return true;
        }
        if (k == juce::KeyPress::returnKey) { pickRow(list_.getSelectedRow()); return true; }
        if (k == juce::KeyPress::escapeKey) { dismiss(); return true; }
        return false;
    }
    void textEditorTextChanged(juce::TextEditor&) override { refilter(); }

    void pickRow(int row)
    {
        if (row < 0 || row >= shown_.size() || !onPick_) return;
        const auto d = all_.getReference(shown_[row]);
        auto fn = std::move(onPick_);
        onPick_ = nullptr;
        dismiss();   // this may destroy us: nothing below touches members
        fn(d);
    }
public:
    ~ChainPluginPicker() override { EchoJay_NSLog("EJPicker: destroyed"); }
private:
    void dismiss()
    {
        EchoJay_NSLog("EJPicker: self-dismiss (Esc or pick)");
        if (box_ != nullptr) { auto* b = box_; box_ = nullptr; b->dismiss(); }
    }

    juce::Array<juce::PluginDescription> all_;
    juce::Array<int> shown_;
    PickFn onPick_;
    SearchField search_;
    juce::ListBox list_;
    juce::Rectangle<int> hint_;
    juce::CallOutBox* box_ = nullptr;
    bool focused_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainPluginPicker)
};
