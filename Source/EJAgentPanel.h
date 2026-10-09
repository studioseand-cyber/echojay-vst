#pragma once
// =============================================================================
//  EJAgentPanel — the agent session's CARD, docked on the composer like the ask
//  shelf (AskShelfLayout.h): one component the editor places with ONE height it
//  asks the panel for (preferredHeight), inside the chat column, never past it.
//
//  What it shows, top to bottom:
//    header   a status dot + the client's status line, and Stop while the loop runs
//    body     (scrolls when taller than the room it is given - nothing is ever cut)
//             the goal; the streamed talk of the current round; a notice line;
//             THE CHECKLIST - one row per tool call with a glyph (pending / running
//             spinner / tick / cross / skipped / undone / stopped), the server's
//             summary, the landed line or error underneath, and a right-hand
//             button: "Approved"/"Skipped" toggles on the round's ask_first lines
//             while the plan awaits a decision (the plan card IS the checklist),
//             Undo on a landed change once undo is allowed;
//             the ask (question + choice pills) when the model asked;
//             the playback row (listening ring + the one sentence) while waiting
//    footer   the context buttons: Approve all / Apply / Decline; Got it, you can
//             stop; Undo all / Retry / Close
//
//  Palette and components are EchoJay's own (EchoJayLookAndFeel::Colours, the
//  chat's 26 px pills, kFieldCorner corners); every rect comes from
//  EJAgentPanelLayout.h so paint and buttons cannot disagree and the guard can
//  assert "no truncation at any editor size" on the same arithmetic.
// =============================================================================
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>
#include "EJAgentClient.h"
#include "EJAgentPanelLayout.h"

class EJAgentPanel : public juce::Component,
                     private EJAgentClient::Listener,
                     private juce::Timer
{
public:
    explicit EJAgentPanel (EJAgentClient& client);
    ~EJAgentPanel() override;

    // The editor docks the panel only while this is true (hook E3).
    bool shouldShow() const noexcept { return client_.hasSession(); }
    // THE ONE HEIGHT AUTHORITY. The natural height of everything at `width`,
    // capped at maxHeight; beyond the cap the body scrolls, so the rule
    // "no truncation at any editor size" holds at every width and height.
    int preferredHeight (int width, int maxHeight) const;

    // Hooks the editor wires (E3 / E4). onLayoutNeeded fires when the panel's
    // natural height or its visibility changed - the editor re-runs its chat
    // layout. onTranscript carries lines for the chat history.
    std::function<void()> onLayoutNeeded;
    std::function<void (const juce::String& role, const juce::String& text)> onTranscript;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // ---- the laid-out geometry (one author; the guard reads it) ----
    struct Layout
    {
        int width = 0, totalHeight = 0, contentWidth = 0, contentHeight = 0;
        bool scrolls = false;
        juce::Rectangle<int> header, stop, body, footer;
        juce::Rectangle<int> goal, text, notice, askQuestion, playbackRing, playbackLine;
        std::vector<echojay::agentui::RowSpec>  rows;
        std::vector<echojay::agentui::RowRects> rowRects;
        std::vector<juce::String> rowIds;
        std::vector<int> rowButtonKind;          // 0 none, 1 Undo, 2 Approved/Skipped toggle
        juce::StringArray chipLabels;
        std::vector<juce::Rectangle<int>> chipRects;
        std::vector<juce::Rectangle<int>> footerButtons;
        juce::StringArray footerLabels;
    };
    const Layout& layout() const noexcept { return layout_; }
    Layout computeLayout (int width, int maxHeight) const;     // pure over the client's model
    // The invariants: "" when every rect fits, else the first violation.
    static juce::String checkLayout (const Layout& l);

private:
    struct Content : juce::Component
    {
        explicit Content (EJAgentPanel& p) : panel (p) { setInterceptsMouseClicks (false, true); }
        void paint (juce::Graphics& g) override { panel.paintContent (g); }
        EJAgentPanel& panel;
    };

    void paintContent (juce::Graphics& g);
    void paintGlyph (juce::Graphics& g, juce::Rectangle<int> r, EJAgentClient::Step::Status st) const;
    void relayout();
    void placeButtons();
    juce::TextButton& rowButton (size_t i);
    juce::TextButton& chipButton (size_t i);
    static void styleSecondary (juce::TextButton& b);
    static void stylePrimary (juce::TextButton& b);
    static void styleDanger (juce::TextButton& b);
    void ensureTimer();

    // EJAgentClient::Listener
    void agentChanged() override;
    void agentTranscript (const juce::String& role, const juce::String& text) override;
    void timerCallback() override;

    EJAgentClient& client_;
    juce::Viewport viewport_;
    Content content_ { *this };
    juce::TextButton stopBtn_ { "Stop" }, approveAllBtn_ { "Approve all" }, applyBtn_ { "Apply" }, declineBtn_ { "Decline" },
                     gotItBtn_ { "Got it, you can stop" }, undoAllBtn_ { "Undo all" }, retryBtn_ { "Retry" }, closeBtn_ { "Close" };
    std::vector<std::unique_ptr<juce::TextButton>> rowBtns_, chipBtns_;
    Layout layout_;
    int   lastNaturalH_ = -1;
    bool  lastShown_ = false;
    float phase_ = 0.0f;
};
