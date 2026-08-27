#pragma once
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "ChainWetKnob.h"
#include "NativeClip.h"

// EchoJay Link editor — resizable window hosting the received chain inline.
//
// The hosting pattern is a port of the main plugin's ChainListPanel (see
// PluginEditor.h): dedicated clip-container NSView locked to the display
// rect (NativeClip, per-target class/log tag), JUCE-side InlineHolder clip,
// native-frame polling then a 400ms maintenance re-assert, ONE hosted editor
// at a time with sequential close-before-open, pop-out fallback. Layout
// policy identical: native size, no scaling, centred with equal side trim,
// top edge always visible, bottom-only trim, dark navy container.
class LinkEditor : public juce::AudioProcessorEditor,
                   private juce::Timer
{
public:
    explicit LinkEditor(LinkProcessor&);
    ~LinkEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    // Low-rate poll for the audio-thread mono fold-down note; repaints ONLY
    // when the note text changes, so idle playback never flickers the hosted
    // native editor.
    void timerCallback() override;
    juce::String lastMonoNote_;

    // Shared tooltip window — makes the chain strip / card button tooltips
    // live (700ms hover, styled by the editor's LookAndFeel)
    juce::TooltipWindow tooltipWindow_ { this, 700 };

    // ---- Mini view (top-right minimise icon) ----
    // EXPLICIT compact layout, not the full layout with parts hidden:
    // header row (name, Active toggle, restore icon) + a status body (chain
    // summary + status line). No chain strip / display area / settings box.
    // Entering mini closes any hosted editor cleanly; restore re-opens the
    // selected slot inline via the normal rebuild path.
    bool miniMode = false;
    int  miniSavedW_ = 0, miniSavedH_ = 0;
    void toggleMiniMode();

    // ========================================================================
    struct LinkChainPanel : juce::Component, juce::Timer
    {
        LinkProcessor& proc;

        static constexpr int kNameRowH  = 28;   // selected plugin name + popout
        static constexpr int kSettingsH = 72;   // SUGGESTED SETTINGS card
        static constexpr int kCardGap   = 8;
        static constexpr int kStatusH   = 18;   // build results line
        static constexpr int kStripH    = 76;
        static constexpr int kBlockW    = 118;
        static constexpr int kBlockH    = 64;   // room for the wet/dry knob row
        static constexpr int kBlockGap  = 26;
        static constexpr int kMasterW   = 62;   // fixed master MIX knob area
        static constexpr int kPreGainW  = 56;   // fixed pre-gain knob area, HEAD of strip

        // Pop-out fallback window (native size, always on top, close returns
        // the editor inline)
        struct PopoutWindow : juce::DocumentWindow
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor;
            std::function<void()> onCloseRequest;
            PopoutWindow(const juce::String& name, juce::AudioProcessorEditor* e)
                : juce::DocumentWindow(name, juce::Colour(0xff0A0C18),
                                       juce::DocumentWindow::closeButton),
                  editor(e)
            {
                setUsingNativeTitleBar(true);
                setContentNonOwned(editor.get(), true);
                setResizable(false, false);
                centreWithSize(editor->getWidth(), editor->getHeight());
                setAlwaysOnTop(true);
                setVisible(true);
            }
            void closeButtonPressed() override
            {
                if (onCloseRequest) onCloseRequest();
                else setVisible(false);
            }
        };

        struct Block : juce::Component
        {
            juce::String name;
            int  modelIdx = 0;
            bool bypassed = false;
            bool selected = false;
            bool missing  = false;
            bool popoutOnly = false;   // editor opens in a floating window

            juce::TextButton bypassBtn { "B" };
            juce::TextButton removeBtn { "X" };
            juce::TextButton prevBtn   { "<" };
            juce::TextButton nextBtn   { ">" };
            ChainWetKnob     wetKnob;   // per-slot wet/dry (hidden for bypassed/missing)

            std::function<void()>      onSelect;
            std::function<void()>      onBypass;
            std::function<void()>      onRemove;
            std::function<void(int)>   onMove;
            std::function<void(float)> onWet;
            std::function<void()>      onWetEnd;

            Block()
            {
                auto style = [](juce::TextButton& b, juce::Colour fg) {
                    b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xcc0E1020));
                    b.setColour(juce::TextButton::textColourOffId, fg);
                };
                style(bypassBtn, juce::Colour(0xffa0a0b8));
                style(removeBtn, juce::Colour(0xffef4444));
                style(prevBtn,   juce::Colour(0xffa0a0b8));
                style(nextBtn,   juce::Colour(0xffa0a0b8));
                bypassBtn.setTooltip("Bypass this plugin");
                removeBtn.setTooltip("Remove from chain");
                prevBtn.setTooltip("Move earlier in the chain");
                nextBtn.setTooltip("Move later in the chain");
                for (auto* b : { &bypassBtn, &removeBtn, &prevBtn, &nextBtn })
                    addAndMakeVisible(*b);
                bypassBtn.onClick = [this] { if (onBypass) onBypass(); };
                removeBtn.onClick = [this] { if (onRemove) onRemove(); };
                prevBtn.onClick   = [this] { if (onMove)   onMove(-1); };
                nextBtn.onClick   = [this] { if (onMove)   onMove(+1); };
                addAndMakeVisible(wetKnob);
                wetKnob.onChange     = [this](float v) { if (onWet) onWet(v); };
                wetKnob.onGestureEnd = [this] { if (onWetEnd) onWetEnd(); };
            }

            void mouseDown(const juce::MouseEvent&) override
            { if (onSelect) onSelect(); }

            void paint(juce::Graphics& g) override
            {
                const auto coral = juce::Colour(0xffff6d5a);
                auto r = getLocalBounds().toFloat().reduced(0.5f);
                g.setColour(missing ? coral.withAlpha(0.12f)
                          : selected ? juce::Colour(0xff11293a)
                                     : juce::Colour(0xff0E1020));
                g.fillRoundedRectangle(r, 8.0f);
                g.setColour(missing  ? coral.withAlpha(0.6f)
                          : selected ? juce::Colour(0xff22d3ee)
                                     : juce::Colour::fromFloatRGBA(1, 1, 1, 0.08f));
                g.drawRoundedRectangle(r, 8.0f, selected ? 1.5f : 1.0f);

                g.setColour(missing  ? coral
                          : bypassed ? juce::Colour(0xff606078)
                                     : juce::Colour(0xfff0f0f5));
                g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
                g.drawText(name, 6, 3, getWidth() - 12, 18,
                           juce::Justification::centred, true);
                if (missing || bypassed)
                {
                    g.setColour(missing ? coral.withAlpha(0.8f)
                                        : juce::Colour(0xfff59e0b));
                    g.setFont(juce::Font(juce::FontOptions(7.0f, juce::Font::bold)));
                    g.drawText(missing ? "MISSING" : "BYPASSED", 6, 19,
                               getWidth() - 12, 9, juce::Justification::centred);
                }
                if (popoutOnly)
                {
                    // Pop-out glyph — this plugin's editor opens in a
                    // floating window (out-of-process view, can't inline)
                    g.setColour(juce::Colour(0xff22d3ee).withAlpha(0.85f));
                    g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
                    g.drawText(juce::String::fromUTF8("\xe2\x86\x97"),
                               getWidth() - 15, 2, 12, 11, juce::Justification::centred);
                }
            }

            void resized() override
            {
                int bw = 18, bh = 15, m = 4;
                int by = getHeight() - bh - m;
                bypassBtn.setBounds(m, by, bw, bh);
                removeBtn.setBounds(m + bw + 2, by, bw, bh);
                nextBtn.setBounds(getWidth() - m - bw, by, bw, bh);
                prevBtn.setBounds(getWidth() - m - bw * 2 - 2, by, bw, bh);
                // Wet/dry knob — centred between name row and button row
                // (same geometry as the main plugin's Chain tab block)
                wetKnob.setBounds((getWidth() - 22) / 2, 20, 22, 22);
            }
        };

        struct StripContent : juce::Component
        {
            int lineY = 0, lineEndX = 0;
            void paint(juce::Graphics& g) override
            {
                if (lineEndX > 12)
                {
                    g.setColour(juce::Colour(0xff22d3ee).withAlpha(0.35f));
                    g.drawLine(12.0f, (float)lineY, (float)lineEndX, (float)lineY, 1.5f);
                }
            }
        };

        struct InlineHolder : juce::Component
        {
            std::function<void()> onChildBounds;
            void childBoundsChanged(juce::Component*) override
            { if (onChildBounds) onChildBounds(); }
        };

        // ---- members ----
        juce::Viewport   stripView;
        StripContent     stripContent;
        ChainWetKnob     masterKnob;   // whole-chain wet/dry, fixed right of strip
        PreGainKnob      preGainKnob;  // pre-chain headroom gain, HEAD of strip (the Link's own rack)
        std::vector<std::unique_ptr<Block>> blocks;
        int selectedIdx = -1;

        // Manual add — "+" block after the last slot, identical to the main
        // plugin's; hidden at kMaxChainSlots
        static constexpr int kAddW = 40;
        juce::TextButton addBlock { "+" };
        std::function<void()> onAddClick;

        InlineHolder inlineHolder;
        std::unique_ptr<juce::AudioProcessorEditor> inlineEditor;
        int  inlineModelIdx = -1;
        int  realW = 0, realH = 0;
        int  framePolls  = 0;
        bool settled     = false;
        bool inlineIsBuiltin = false;   // plain JUCE editor: no native view
                                        // to attach, measure, clip or poll
        bool layoutGuard = false;

        std::unique_ptr<PopoutWindow> popout;
        int popoutModelIdx = -1;
        juce::TextButton popBtn { juce::String::fromUTF8("\xe2\x86\x97") };

        // SUGGESTED SETTINGS content — wraps, scrolls on overflow (same card
        // as the main plugin's Chain tab)
        juce::TextEditor settingsBox;

        juce::String statusText;   // build results line

        // ---- lifecycle ----
        explicit LinkChainPanel(LinkProcessor& p) : proc(p)
        {
            addChildComponent(inlineHolder);   // back of z-order
            inlineHolder.setInterceptsMouseClicks(false, true);
            inlineHolder.onChildBounds = [this]
            {
                if (layoutGuard || inlineEditor == nullptr) return;
                int w = 0, h = 0;
                if (NativeClip::getPluginViewSize(this, w, h) && w > 100 && h > 60)
                { realW = w; realH = h; }
                layoutInline();
                attachNative(false);
            };

            addAndMakeVisible(stripView);
            stripView.setViewedComponent(&stripContent, false);
            stripView.setScrollBarsShown(false, true, false, true);
            stripView.setScrollBarThickness(8);

            addBlock.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff141626));
            addBlock.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
            addBlock.onClick = [this] { if (onAddClick) onAddClick(); };
            stripContent.addAndMakeVisible(addBlock);

            // Master chain wet/dry — fixed right of the strip, same placement
            // and shared knob component as the main plugin's Chain tab
            masterKnob.caption = "MIX";
            masterKnob.setValue(proc.getChainMasterWet());
            masterKnob.onChange     = [this](float v) { proc.setChainMasterWet(v); };
            masterKnob.onGestureEnd = [this] { proc.commitChainWetChange(); };
            addAndMakeVisible(masterKnob);

            // Pre-gain knob at the HEAD of the Link's OWN rack strip: the
            // control the main plugin has, now on the Link's own editor. It
            // drives this Link's chain host directly; a drag is a hand set.
            preGainKnob.caption = "PRE";
            preGainKnob.onChange = [this](float v01)
            { proc.getChainHost().setPreGainDb(PreGainKnob::dbFromV(v01), true);
              proc.commitChainWetChange(); };
            preGainKnob.onResetAuto = [this]
            { proc.getChainHost().resetPreGainToAuto(); proc.commitChainWetChange();
              syncPreGain(); };   // rebuild is not per-tick here: repaint the knob now
            addAndMakeVisible(preGainKnob);

            popBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xcc0E1020));
            popBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff22d3ee));
            popBtn.onClick = [this] { openPopoutForSelected(); };
            addChildComponent(popBtn);

            settingsBox.setMultiLine(true, true);
            settingsBox.setReadOnly(true);
            settingsBox.setScrollbarsShown(true);
            settingsBox.setCaretVisible(false);
            settingsBox.setColour(juce::TextEditor::backgroundColourId,
                                  juce::Colours::transparentBlack);
            settingsBox.setColour(juce::TextEditor::outlineColourId,
                                  juce::Colours::transparentBlack);
            settingsBox.setColour(juce::TextEditor::focusedOutlineColourId,
                                  juce::Colours::transparentBlack);
            settingsBox.setColour(juce::TextEditor::textColourId, juce::Colour(0xff22d3ee));
            settingsBox.setFont(juce::Font(juce::FontOptions(11.5f)));
            addChildComponent(settingsBox);
        }

        ~LinkChainPanel() override { closeAllEditors(); }

        juce::Rectangle<int> displayArea() const
        {
            // Space is always reserved for the settings card so the clip
            // container rect stays stable across selection changes
            return { 12, kNameRowH,
                     juce::jmax(50, getWidth() - 24),
                     juce::jmax(50, getHeight() - kNameRowH - kSettingsH - kCardGap
                                    - kStatusH - kStripH - 8) };
        }

        juce::Rectangle<int> settingsBoxRect() const
        {
            auto area = displayArea();
            return { area.getX(), area.getBottom() + kCardGap,
                     area.getWidth(), kSettingsH };
        }

        // Sync the settings card with the current selection
        void updateSettingsCard()
        {
            auto& model = proc.getChainModel();
            bool sel = selectedIdx >= 0 && selectedIdx < (int)model.size();
            settingsBox.setVisible(sel);
            if (!sel) return;
            const auto& s = model[(size_t)selectedIdx];
            bool hasGuidance = !s.missing && s.settings.isNotEmpty();
            settingsBox.setColour(juce::TextEditor::textColourId,
                                  hasGuidance ? juce::Colour(0xff22d3ee)
                                              : juce::Colour(0xff606078));
            juce::String txt = hasGuidance
                ? s.settings
                : juce::String("No suggested settings for this plugin");
            if (settingsBox.getText() != txt)
                settingsBox.setText(txt, false);
        }

        // ---- editor lifecycle (ONE at a time, close-before-open) ----
        void closeInline()
        {
            stopTimer();
            if (inlineEditor)
            {
                inlineHolder.removeChildComponent(inlineEditor.get());
                inlineEditor.reset();
                if (!inlineIsBuiltin) NativeClip::detach(this);
            }
            inlineHolder.setVisible(false);
            inlineModelIdx = -1;
            realW = realH = 0;
            settled = false;
            inlineIsBuiltin = false;
        }

        void closePopout()
        {
            if (popout) { popout->setVisible(false); popout.reset(); }
            popoutModelIdx = -1;
        }

        void closeAllEditors() { closeInline(); closePopout(); }

        void attachNative(bool log)
        {
            if (inlineIsBuiltin) return;   // no foreign view to clip
            if (inlineEditor)
                NativeClip::attach(this, displayArea(), log,
                                   inlineEditor->getWidth(), inlineEditor->getHeight());
        }

        // Slot's hosted format ("AudioUnit"/"VST3") — popout-only is per-format
        juce::String slotFormat(int modelIdx) const
        {
            auto& model = proc.getChainModel();
            if (modelIdx < 0 || modelIdx >= (int)model.size()) return {};
            int h = model[(size_t)modelIdx].hostIdx;
            return h >= 0 ? proc.getChainHost().getSlotDescription(h).pluginFormatName
                          : juce::String();
        }

        void layoutInline()
        {
            if (!inlineEditor) return;
            auto area = displayArea();
            inlineHolder.setBounds(area);
            int pw = realW > 8 ? realW : inlineEditor->getWidth();
            int ph = realH > 8 ? realH : inlineEditor->getHeight();
            pw = juce::jmax(pw, 40);
            ph = juce::jmax(ph, 30);
            layoutGuard = true;
            inlineEditor->setBounds((area.getWidth() - pw) / 2, 0, pw, ph);
            layoutGuard = false;
        }

        void showInline(int modelIdx)
        {
            closeAllEditors();
            auto& model = proc.getChainModel();
            if (modelIdx < 0 || modelIdx >= (int)model.size()) return;
            int hostIdx = model[(size_t)modelIdx].hostIdx;
            if (model[(size_t)modelIdx].missing || hostIdx < 0) { repaint(); return; }

            // THE ONE PLACEMENT DECISION (31 Aug 2026): shared with the
            // main's panel via ChainHost::editorPlacement — the Link used
            // to consult only the popout list and had no builtin arm, so
            // the native-size poll (blind to a plain JUCE editor) marked
            // EchoJay's own devices popout-only and floated them forever
            // under a "plugin limitation" caption that was false as
            // written. Builtins embed inline with NO native machinery;
            // genuine out-of-process editors keep floating, label intact.
            switch (ChainHost::editorPlacement(model[(size_t)modelIdx].name,
                                               slotFormat(modelIdx)))
            {
                case ChainHost::EditorPlacement::InlineJuce:
                {
                    juce::AudioProcessorEditor* edB = nullptr;
                    try { edB = proc.getChainHost().createEditorForSlot(hostIdx); }
                    catch (...) {}
                    if (!edB)
                    { statusText = "Failed: could not open editor"; repaint(); return; }
                    statusText.clear();
                    inlineEditor.reset(edB);
                    inlineModelIdx  = modelIdx;
                    inlineIsBuiltin = true;
                    settled         = true;   // nothing to poll or wait for
                    inlineHolder.setVisible(true);
                    inlineHolder.addAndMakeVisible(*inlineEditor);
                    layoutInline();
                    repaint();
                    return;                   // no timer: nothing native runs
                }
                case ChainHost::EditorPlacement::Float:
                    statusText = "Opens in a floating window (plugin limitation)";
                    openPopoutForSelected();
                    repaint();
                    return;
                case ChainHost::EditorPlacement::InlineNative:
                    break;                    // the native path below
            }

           #if ! JUCE_MAC
            // Off macOS there is no NativeClip container — see the matching
            // guard in PluginEditor.h ChainListPanel::showInline for the full
            // reasoning. Decide the floating window up front rather than letting
            // the poll below stall ~5s and then persist a popout_only mark that
            // blames the plugin for a platform gap.
            statusText = "Opens in a floating window";
            openPopoutForSelected();
            repaint();
            return;
           #endif

            juce::AudioProcessorEditor* ed = nullptr;
            try { ed = proc.getChainHost().createEditorForSlot(hostIdx); } catch (...) {}
            if (!ed) { statusText = "Failed: could not open editor"; repaint(); return; }

            inlineEditor.reset(ed);
            inlineModelIdx = modelIdx;
            inlineHolder.setVisible(true);
            inlineHolder.addAndMakeVisible(*inlineEditor);
            layoutInline();
            attachNative(true);
            framePolls = 0;
            settled = false;
            startTimer(100);
            repaint();
        }

        void timerCallback() override
        {
            if (!inlineEditor) { stopTimer(); return; }
            if (!settled)
            {
                ++framePolls;
                int w = 0, h = 0;
                bool got = NativeClip::getPluginViewSize(this, w, h)
                        && w > 100 && h > 60;
                if (got) { realW = w; realH = h; }
                if (got || framePolls >= 50)
                {
                    settled = true;
                    if (!got)
                    {
                        // Never grew past a placeholder: out-of-process
                        // editors (WaveShell etc.) render in a view service
                        // and won't size inline, but work in their own
                        // window. Remember the plugin and open the pop-out
                        // automatically — first-class, not an error.
                        auto& model = proc.getChainModel();
                        if (inlineModelIdx >= 0 && inlineModelIdx < (int)model.size())
                        {
                            ChainHost::markPopoutOnly(model[(size_t)inlineModelIdx].name,
                                                      slotFormat(inlineModelIdx));
                            for (auto& bl : blocks)
                                if (bl->modelIdx == inlineModelIdx)
                                { bl->popoutOnly = true; bl->repaint(); }
                        }
                        statusText = "Opens in a floating window (plugin limitation)";
                        openPopoutForSelected();   // destroys the inline editor first
                        repaint();
                        startTimer(400);
                        return;
                    }
                    layoutInline();
                    attachNative(true);
                    repaint();
                    startTimer(400);
                }
                return;
            }
            int w = 0, h = 0;
            if (NativeClip::getPluginViewSize(this, w, h) && w > 100 && h > 60
                && (w != realW || h != realH))
            {
                realW = w; realH = h;
                if (statusText.startsWith("Editor didn't")) statusText.clear();
                layoutInline();
                repaint();
            }
            attachNative(false);
        }

        void selectSlot(int i)
        {
            selectedIdx = i;
            for (auto& bl : blocks)
            { bl->selected = (bl->modelIdx == i); bl->repaint(); }
            if (inlineModelIdx != i || inlineEditor == nullptr)
                showInline(i);
            popBtn.setVisible(canPopOut());
            updateSettingsCard();
            repaint();
        }

        /** Rack (host) index to model index. The wire and the sidecar both
            speak RACK slots; the panel is indexed by MODEL slots, which also
            contain unresolved entries with hostIdx = -1. Returns -1 when no
            live slot answers to that rack index, which is the honest answer
            for a plugin that failed to load on this Link. */
        int modelIdxForHostIdx(int hostIdx) const
        {
            if (hostIdx < 0) return -1;
            const auto& model = proc.getChainModel();
            for (int i = 0; i < (int)model.size(); ++i)
                if (model[(size_t)i].hostIdx == hostIdx) return i;
            return -1;
        }

        /** Bring whichever editor is currently up to the front. Inline
            editors ride the Link window (already raised by the caller), so
            only a pop-out needs its own raise. */
        void raiseOpenEditor()
        {
            if (popout != nullptr) popout->toFront(true);
        }

        bool canPopOut() const
        {
            auto& model = proc.getChainModel();
            return selectedIdx >= 0 && selectedIdx < (int)model.size()
                && !model[(size_t)selectedIdx].missing;
        }

        void openPopoutForSelected()
        {
            auto& model = proc.getChainModel();
            if (!canPopOut()) return;
            int i = selectedIdx;
            int hostIdx = model[(size_t)i].hostIdx;
            closeAllEditors();
            juce::AudioProcessorEditor* ed = nullptr;
            try { ed = proc.getChainHost().createEditorForSlot(hostIdx); } catch (...) {}
            if (!ed) return;
            popout = std::make_unique<PopoutWindow>(model[(size_t)i].name, ed);
            popoutModelIdx = i;
            popout->onCloseRequest = [safe = juce::Component::SafePointer<LinkChainPanel>(this)]
            {
                juce::MessageManager::callAsync([safe]
                {
                    if (safe == nullptr) return;
                    int s = safe->popoutModelIdx;
                    safe->closePopout();
                    // Popout-only plugins must NOT bounce back inline (that
                    // would immediately reopen the window they just closed)
                    auto& mdl = safe->proc.getChainModel();
                    if (s >= 0 && s == safe->selectedIdx
                        && s < (int)mdl.size()
                        && !ChainHost::isPopoutOnly(mdl[(size_t)s].name, safe->slotFormat(s)))
                        safe->showInline(s);
                    safe->repaint();
                });
            };
            popout->toFront(true);
            repaint();
        }

        // ---- strip / model sync ----
        void rebuild()
        {
            auto& model = proc.getChainModel();
            selectedIdx = juce::jlimit(-1, (int)model.size() - 1, selectedIdx);
            // Rack lock (21 Aug 2026): rack-wide read-only while a main's
            // Chain tab shows this rack. STRUCTURE AND MIX ONLY — selection
            // and hosted editors stay live (locking parameter writes is a
            // claim the code cannot keep). pollRackLock calls
            // notifyChainModel on every transition, so this rebuild runs on
            // lock and on release, exactly like the lease's engage path.
            const juce::String lockOwner = proc.rackLockOwner();
            // Two lock states, two truths: a live selection instructs a
            // deselect; a failure-held session (lease editPending) says an
            // edit is still pending and what actually releases it — never
            // an instruction the user already followed.
            const juce::String lockWhy = lockOwner.isNotEmpty()
                ? (proc.rackEditPendingHeld()
                    ? "An edit from \"" + lockOwner + "\" is still pending - "
                      "retry or end the session there to release this rack."
                    : "Selected in the rack on \"" + lockOwner
                        + "\" - deselect there to edit here.")
                : juce::String();

            for (auto& bl : blocks) stripContent.removeChildComponent(bl.get());
            blocks.clear();
            for (int i = 0; i < (int)model.size(); ++i)
            {
                auto bl = std::make_unique<Block>();
                bl->name     = model[(size_t)i].name;
                bl->popoutOnly = ChainHost::isPopoutOnly(bl->name, slotFormat(i));
                bl->modelIdx = i;
                bl->bypassed = model[(size_t)i].bypassed;
                bl->missing  = model[(size_t)i].missing;
                bl->selected = (i == selectedIdx);
                int ci = i;
                bl->onSelect = [this, ci] { selectSlot(ci); };
                bl->onBypass = [this, ci] { proc.toggleChainSlotBypass(ci); };
                bl->onRemove = [this, ci]
                {
                    // proc closes our editors first via onChainAboutToChange
                    if (selectedIdx == ci) selectedIdx = -1;
                    proc.removeChainSlot(ci);
                };
                bl->onMove   = [this, ci](int dir)
                {
                    if (selectedIdx == ci) selectedIdx = ci + dir;
                    else if (selectedIdx == ci + dir) selectedIdx = ci;
                    proc.moveChainSlot(ci, dir);
                };
                bl->wetKnob.setValue(model[(size_t)i].wet);
                bl->wetKnob.setVisible(!model[(size_t)i].bypassed
                                       && !model[(size_t)i].missing);
                bl->onWet    = [this, ci](float v) { proc.setChainSlotWet(ci, v); };
                bl->onWetEnd = [this] { proc.commitChainWetChange(); };
                bl->prevBtn.setEnabled(i > 0);
                bl->nextBtn.setEnabled(i < (int)model.size() - 1);
                // Stage 1: a slot under an edit lease is CONTROLLED from the
                // main plugin. Its controls go dead here (the lease owns the
                // bypass, and structure is refused rack-wide while held) and
                // the tooltip says where control lives. The lease actions
                // call notifyChainModel, so this rebuild runs on every
                // engage and release.
                if (model[(size_t)i].hostIdx >= 0
                    && model[(size_t)i].hostIdx == proc.controlledSlot0())
                {
                    const juce::String why =
                        "Being edited from the main EchoJay plugin - "
                        "controls return when it releases";
                    for (auto* b : { &bl->bypassBtn, &bl->removeBtn,
                                     &bl->prevBtn, &bl->nextBtn })
                    { b->setEnabled(false); b->setTooltip(why); }
                    bl->wetKnob.setVisible(false);
                    bl->setAlpha(0.55f);
                }
                // Rack lock: the whole rack's structure/mix controls go dead,
                // named tooltip on each. onSelect stays wired — selection and
                // opening editors are deliberately NOT locked.
                if (lockOwner.isNotEmpty())
                {
                    for (auto* b : { &bl->bypassBtn, &bl->removeBtn,
                                     &bl->prevBtn, &bl->nextBtn })
                    { b->setEnabled(false); b->setTooltip(lockWhy); }
                    bl->wetKnob.setVisible(false);
                    bl->setAlpha(0.55f);
                }
                stripContent.addAndMakeVisible(*bl);
                blocks.push_back(std::move(bl));
            }
            layoutStrip();
            // Rack lock: master MIX and "+" are mix/structure writes. The
            // processor guard is the backstop; this stops the gesture ever
            // starting (a knob that moves then snaps back reads as a bug).
            addBlock.setEnabled(lockOwner.isEmpty());
            addBlock.setTooltip(lockWhy);
            masterKnob.setInterceptsMouseClicks(lockOwner.isEmpty(), lockOwner.isEmpty());
            masterKnob.setAlpha(lockOwner.isEmpty() ? 1.0f : 0.55f);
            masterKnob.setValue(proc.getChainMasterWet());
            {
                auto& ch = proc.getChainHost();
                preGainKnob.userSet = ch.isPreGainUserSet();
                preGainKnob.inputKnown = ch.getChainInLevels().known;
                preGainKnob.setDb(ch.getPreGainDb(), false);
            }
            popBtn.setVisible(canPopOut());

            // Keep the inline editor in sync: the model index it was opened
            // for may have moved or vanished — reopen/close as needed
            if (selectedIdx < 0)
                closeAllEditors();
            else if ((inlineModelIdx != selectedIdx || inlineEditor == nullptr)
                     && !(popout != nullptr && popoutModelIdx == selectedIdx))
                showInline(selectedIdx);
            resized();
            repaint();
        }

        void layoutStrip()
        {
            const int contentH = kStripH - 10;
            int x = 12;
            int y = (contentH - kBlockH) / 2;
            for (auto& bl : blocks)
            {
                bl->setBounds(x, y, kBlockW, kBlockH);
                x += kBlockW + kBlockGap;
            }
            stripContent.lineY    = y + kBlockH / 2;
            stripContent.lineEndX = blocks.empty() ? 0 : x - kBlockGap;

            // "+" block after the last slot (same as the main plugin's strip)
            bool canAdd = (int)blocks.size() < LinkProcessor::kMaxChainSlots;
            addBlock.setVisible(canAdd);
            if (canAdd)
            {
                addBlock.setBounds(x, y + (kBlockH - kAddW) / 2, kAddW, kAddW);
                x += kAddW + 12;
            }

            stripContent.setSize(juce::jmax(x, stripView.getWidth()), contentH);
            stripContent.repaint();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff0A0C18));

            auto area = displayArea();
            auto& model = proc.getChainModel();
            bool haveSel = selectedIdx >= 0 && selectedIdx < (int)model.size();

            g.setColour(juce::Colour(0xff080A12));
            g.fillRoundedRectangle(area.toFloat(), 8.0f);
            g.setColour(juce::Colour(0xff22d3ee).withAlpha(0.3f));
            g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 8.0f, 1.0f);

            // Rack lock overlay banner: names the owner and says how to
            // release. Paint only — it must never block clicks, because
            // selection and hosted editors stay live under the lock.
            {
                const juce::String lockOwner = proc.rackLockOwner();
                if (lockOwner.isNotEmpty())
                {
                    g.setColour(juce::Colour(0xffFFB020));
                    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
                    g.drawText(proc.rackEditPendingHeld()
                                   ? "An edit from \"" + lockOwner
                                       + "\" is still pending - retry or end "
                                       "the session there to release this rack."
                                   : "Selected in the rack on \"" + lockOwner
                                       + "\" - deselect there to edit here.",
                               8, kNameRowH + 2, getWidth() - 16, 16,
                               juce::Justification::centred, true);
                    // Phase 3: while a structure plan's journal is active,
                    // the overlay says the shape flicker underneath is
                    // deliberate, not corruption.
                    if (proc.structPlanJournalPresent())
                        g.drawText(lockOwner + " is restructuring this rack...",
                                   8, kNameRowH + 18, getWidth() - 16, 14,
                                   juce::Justification::centred, true);
                }
            }

            // Name row — selected plugin, centred
            if (haveSel)
            {
                const auto& s = model[(size_t)selectedIdx];
                g.setColour(s.missing  ? juce::Colour(0xffff6d5a)
                          : s.bypassed ? juce::Colour(0xff606078)
                                       : juce::Colour(0xfff0f0f5));
                g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
                juce::String title = s.name
                    + (s.missing ? "  (missing)" : s.bypassed ? "  (bypassed)" : "");
                g.drawText(title, 48, 3, getWidth() - 96, kNameRowH - 4,
                           juce::Justification::centred, true);
            }

            // Display-area messages
            if (model.empty())
            {
                g.setColour(juce::Colour(0xffa0a0b8));
                g.setFont(juce::Font(juce::FontOptions(13.0f)));
                if (proc.isChainBuilding())
                {
                    g.drawText("Building chain...", area, juce::Justification::centred);
                }
                else
                {
                    // Same two-line empty state as the main plugin's Chain tab
                    int cy = area.getCentreY() - 20;
                    g.drawText("Press + to add a plugin, or send a chain",
                               area.getX(), cy, area.getWidth(), 20,
                               juce::Justification::centred);
                    g.drawText("from EchoJay.",
                               area.getX(), cy + 20, area.getWidth(), 20,
                               juce::Justification::centred);
                }
            }
            else if (haveSel && model[(size_t)selectedIdx].missing)
            {
                g.setColour(juce::Colour(0xffff6d5a).withAlpha(0.8f));
                g.setFont(juce::Font(juce::FontOptions(12.0f)));
                g.drawText("This plugin could not be loaded on this machine.",
                           area, juce::Justification::centred);
            }
            else if (popout != nullptr && popoutModelIdx == selectedIdx
                     && inlineEditor == nullptr)
            {
                bool po = selectedIdx < (int)model.size()
                       && ChainHost::isPopoutOnly(model[(size_t)selectedIdx].name,
                                                  slotFormat(selectedIdx));
                g.setColour(juce::Colour(0xffa0a0b8));
                g.setFont(juce::Font(juce::FontOptions(12.0f)));
                g.drawText(po ? "This plugin opens in a floating window (plugin limitation)."
                              : "Editor is open in a floating window - click the plugin block to bring it back.",
                           area.reduced(16), juce::Justification::centred, true);
            }

            // SUGGESTED SETTINGS card — same styling as the main plugin
            if (haveSel)
            {
                auto sb = settingsBoxRect().toFloat();
                g.setColour(juce::Colour(0xff080A12));
                g.fillRoundedRectangle(sb, 8.0f);
                g.setColour(juce::Colour(0xff22d3ee).withAlpha(0.3f));
                g.drawRoundedRectangle(sb.reduced(0.5f), 8.0f, 1.0f);
                g.setColour(juce::Colour(0xffa0a0b8));
                g.setFont(juce::Font(juce::FontOptions(8.5f, juce::Font::bold)));
                g.drawText("SUGGESTED SETTINGS", (int)sb.getX() + 10, (int)sb.getY() + 5,
                           200, 12, juce::Justification::centredLeft);
            }

            // Status line
            {
                int sy = getHeight() - kStripH - kStatusH;
                g.setColour(juce::Colour(0xff8B949E).withAlpha(0.85f));
                g.setFont(juce::Font(juce::FontOptions(9.5f)));
                juce::String line = statusText;
                if (line.isEmpty() && !model.empty())
                {
                    int missing = 0;
                    for (auto& s : model) if (s.missing) ++missing;
                    line = juce::String((int)model.size()) + " slot(s)";
                    if (missing > 0) line += ", " + juce::String(missing) + " missing";
                }
                if (!proc.chainLayoutSupported())
                    line += (line.isEmpty() ? "" : "  |  ")
                          + juce::String("unsupported channel layout (chain bypassed)");
                if (auto note = proc.getMonoFoldNote(); note.isNotEmpty())
                    line += (line.isEmpty() ? "" : "  |  ") + note;
                g.drawText(line, 12, sy, getWidth() - 24, kStatusH,
                           juce::Justification::centredLeft);
            }
        }

        // Refresh the pre-gain knob from this Link's chain host. Skipped while
        // dragging so it never fights the gesture. Called per tick (this panel
        // has no per-tick rebuild) and after a reset.
        void syncPreGain()
        {
            if (preGainKnob.isMouseButtonDown()) return;
            auto& ch = proc.getChainHost();
            preGainKnob.userSet = ch.isPreGainUserSet();
            preGainKnob.inputKnown = ch.getChainInLevels().known;
            preGainKnob.setDb(ch.getPreGainDb(), false);
        }

        void resized() override
        {
            popBtn.setBounds(getWidth() - 40, 3, 26, 22);
            auto sb = settingsBoxRect();
            settingsBox.setBounds(sb.getX() + 8, sb.getY() + 18,
                                  sb.getWidth() - 16, sb.getHeight() - 24);
            updateSettingsCard();
            preGainKnob.setBounds(6, getHeight() - kStripH + 6, 44, 54);
            stripView.setBounds(kPreGainW, getHeight() - kStripH,
                                juce::jmax(50, getWidth() - kMasterW - kPreGainW), kStripH);
            masterKnob.setBounds(getWidth() - kMasterW + 9,
                                 getHeight() - kStripH + 6, 44, 54);
            layoutStrip();
            layoutInline();
            attachNative(false);   // re-assert the clip rect at the new size
        }

        void moved() override { attachNative(false); }
    };
    // ========================================================================

private:
    LinkProcessor& proc;

    static constexpr int kHeaderH = 40;

    juce::TextEditor   nameField;
    juce::String lastPlaceholderHostName_;   // Phase N placeholder change detection
    // Rounds the name field's corners to match the main plugin's Project name
    // box (same shared radius). Applied to nameField only; colours/fill/size
    // are unchanged (it reads the editor's own colours).
    std::unique_ptr<juce::LookAndFeel_V4> nameFieldLnF;
    juce::ToggleButton toggleBtn { "Active" };
    juce::Rectangle<float> lightBounds;

    // Built-in gain: compact horizontal slider + numeric readout ("+2.5 dB").
    // Lives in the header row (full) and the mini body (mini). Double-click
    // returns to 0. onValueChange drives the processor; the processor's
    // onLinkStateChanged pushes remote/restore changes back to the slider.
    juce::Slider gainSlider { juce::Slider::LinearHorizontal,
                              juce::Slider::TextBoxRight };
    juce::Label  gainCaption;   // small "GAIN" cap to the left of the slider

    // ---- Placement declaration -------------------------------------------
    // Header control (click → choose bus/insert) + a one-time prompt shown
    // on first open while placement is unset. Link measures at its insert
    // point; for level work it belongs on a bus. See LinkProcessor::Placement.
    // Compact header selector (dropdown): "Bus" / "Channel", or a dim
    // "Placement" when unset. No modal prompt — an unset Link is never nagged;
    // it simply behaves as a Channel (conservative). A small "?" button beside
    // it opens a non-modal popover explaining the two options.
    juce::TextButton placementBtn;                 // "Placement" / "Bus" / "Channel"

    // Subtle inline "?" glyph beside the placement value (dim, brightens on
    // hover) — not a bordered button.
    struct HelpGlyph : juce::Component, juce::SettableTooltipClient
    {
        HelpGlyph();
        std::function<void()> onClick;
        bool hover = false;
        void paint(juce::Graphics&) override;
        void mouseEnter(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
    };
    HelpGlyph placementHelp;

    void showPlacementChooser();                   // dropdown: Bus / Channel
    void showPlacementHelp();                       // "?" popover (CallOutBox)
    void updatePlacementBtn();
    void applyPlacement(int p);                    // set + refresh

    LinkChainPanel chainPanel;

    // "+" block popup — same searchable picker as the main plugin's Chain
    // tab, filtered by plugin_disabled.json
    void showChainPluginPicker();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinkEditor)
};
