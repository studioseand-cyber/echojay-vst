#include "EJAgentPanel.h"
#include "EchoJayLookAndFeel.h"   // Colours + EchoJayChrome::kFieldCorner

using namespace echojay::agentui;
using Colours = EchoJayLookAndFeel::Colours;
using Step = EJAgentClient::Step;
using State = EJAgentClient::State;

// =============================================================================
EJAgentPanel::EJAgentPanel (EJAgentClient& client) : client_ (client)
{
    viewport_.setViewedComponent (&content_, false);
    viewport_.setScrollBarsShown (true, false);
    viewport_.setScrollBarThickness (Metrics::scrollbarW);
    addAndMakeVisible (viewport_);

    styleDanger (stopBtn_);
    stylePrimary (approveAllBtn_);
    stylePrimary (applyBtn_);
    styleSecondary (declineBtn_);
    stylePrimary (gotItBtn_);
    styleSecondary (undoAllBtn_);
    stylePrimary (retryBtn_);
    styleSecondary (closeBtn_);
    for (auto* b : { &stopBtn_, &approveAllBtn_, &applyBtn_, &declineBtn_, &gotItBtn_, &undoAllBtn_, &retryBtn_, &closeBtn_ })
    {
        addChildComponent (b);
        b->setWantsKeyboardFocus (false);
    }
    stopBtn_.onClick       = [this] { client_.stop(); };
    approveAllBtn_.onClick = [this] { client_.approveAll(); };
    applyBtn_.onClick      = [this] { client_.submitPlan(); };
    declineBtn_.onClick    = [this] { client_.declineAll(); };
    gotItBtn_.onClick      = [this] { client_.playbackGotIt(); };
    undoAllBtn_.onClick    = [this] { client_.undoAll(); };
    retryBtn_.onClick      = [this] { client_.retry(); };
    closeBtn_.onClick      = [this] { client_.dismiss(); };
    stopBtn_.setTooltip ("Stop the agent now. Nothing further runs; what landed stays until you undo it.");
    undoAllBtn_.setTooltip ("Put the rack back to how it was when this session started.");

    client_.addListener (this);
    lastShown_ = shouldShow();
}

EJAgentPanel::~EJAgentPanel()
{
    client_.removeListener (this);
    stopTimer();
}

void EJAgentPanel::styleSecondary (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, Colours::bg4);
    b.setColour (juce::TextButton::textColourOffId, Colours::text2);
    b.setColour (juce::TextButton::textColourOnId, Colours::text);
}
void EJAgentPanel::stylePrimary (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, Colours::blue);   // the LnF reads this as the teal glow button
    b.setColour (juce::TextButton::textColourOffId, Colours::blue2);
    b.setColour (juce::TextButton::textColourOnId, Colours::text);
}
void EJAgentPanel::styleDanger (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a1216));
    b.setColour (juce::TextButton::textColourOffId, Colours::red);
    b.setColour (juce::TextButton::textColourOnId, Colours::text);
}

// =============================================================================
//  layout
// =============================================================================
EJAgentPanel::Layout EJAgentPanel::computeLayout (int width, int maxHeight) const
{
    Layout L;
    L.width = juce::jmax (0, width);
    const auto st = client_.state();
    const bool active = client_.isActive();

    // ---- header -----------------------------------------------------------
    L.header = { 0, 0, L.width, Metrics::headerH };
    if (active)
        L.stop = { L.width - Metrics::pad - Metrics::stopW, (Metrics::headerH - Metrics::btnH) / 2, Metrics::stopW, Metrics::btnH };

    // ---- footer: the context buttons ---------------------------------------
    L.footerLabels.clear();
    switch (st)
    {
        case State::AwaitingApproval: L.footerLabels = { "Apply all", "Apply", "Skip all" }; break;
        case State::Listening:        L.footerLabels = { "Got it, you can stop" }; break;
        case State::Done: case State::Stopped: case State::Failed:
            if (client_.canUndoAll()) L.footerLabels.add ("Undo all");
            if (st == State::Failed && client_.errorRetryable()) L.footerLabels.add ("Retry");
            L.footerLabels.add ("Close");
            break;
        default: break;
    }
    const int footerH = L.footerLabels.isEmpty() ? 0 : Metrics::footerH;

    // ---- body content, measured at a width; done twice when it scrolls -------
    auto measure = [&] (int contentWidth, Layout& out)
    {
        out.contentWidth = contentWidth;
        out.rows.clear(); out.rowRects.clear(); out.rowIds.clear(); out.rowButtonKind.clear();
        out.chipLabels.clear(); out.chipRects.clear();
        const int x0 = Metrics::pad;
        int y = Metrics::pad;

        const auto goalText = client_.goal().isNotEmpty() ? "Goal: " + client_.goal() : juce::String();
        const int goalH = wrappedTextHeight (goalText, detailFont(), contentWidth);
        out.goal = goalH > 0 ? juce::Rectangle<int> (x0, y, contentWidth, goalH) : juce::Rectangle<int>();
        if (goalH > 0) y += goalH + Metrics::rowGap;

        const auto live = client_.liveText().trim();
        const int textH = wrappedTextHeight (live, bodyFont(), contentWidth);
        out.text = textH > 0 ? juce::Rectangle<int> (x0, y, contentWidth, textH) : juce::Rectangle<int>();
        if (textH > 0) y += textH + Metrics::rowGap;

        const auto notice = client_.notice();
        const int noticeH = wrappedTextHeight (notice, detailFont(), contentWidth);
        out.notice = noticeH > 0 ? juce::Rectangle<int> (x0, y, contentWidth, noticeH) : juce::Rectangle<int>();
        if (noticeH > 0) y += noticeH + Metrics::rowGap;

        const auto heading = st == State::AwaitingApproval ? client_.planHeading() : juce::String();
        const int headingH = wrappedTextHeight (heading, bodyFont(), contentWidth);
        out.planHeading = headingH > 0 ? juce::Rectangle<int> (x0, y, contentWidth, headingH) : juce::Rectangle<int>();
        if (headingH > 0) y += headingH + Metrics::rowGap;

        const bool undoNow = client_.undoAllowedNow();
        for (const auto& s : client_.steps())
        {
            RowSpec r;
            r.label  = s.label.isNotEmpty() ? s.label : juce::String ("step");
            r.detail = s.detail;
            int kind = 0;
            if (s.status == Step::Status::AwaitingApproval && st == State::AwaitingApproval) { r.rightButtonW = Metrics::toggleW; kind = 2; }
            else if (undoNow && s.canUndo())                                                  { r.rightButtonW = Metrics::undoW;   kind = 1; }
            out.rows.push_back (r);
            out.rowIds.push_back (s.id);
            out.rowButtonKind.push_back (kind);
        }
        if (! out.rows.empty())
            y += layoutRows (out.rows, x0, y, contentWidth, out.rowRects) + Metrics::rowGap;

        if (client_.askPending())
        {
            const auto ask = client_.pendingAsk();
            const int qH = wrappedTextHeight (ask.question, bodyFont(), contentWidth);
            out.askQuestion = qH > 0 ? juce::Rectangle<int> (x0, y, contentWidth, qH) : juce::Rectangle<int>();
            if (qH > 0) y += qH + 4;
            out.chipLabels = ask.labels;
            if (! out.chipLabels.isEmpty())
                y += layoutChips (out.chipLabels, x0, y, contentWidth, out.chipRects) + 2;
        }
        else
            out.askQuestion = {};

        if (client_.playback().active)
        {
            const int lineW = juce::jmax (1, contentWidth - Metrics::indicatorW - Metrics::glyphGap);
            const int lineH = juce::jmax (Metrics::indicatorW, wrappedTextHeight (client_.playback().line, bodyFont(), lineW));
            out.playbackRing = { x0, y, Metrics::indicatorW, Metrics::indicatorW };
            out.playbackLine = { x0 + Metrics::indicatorW + Metrics::glyphGap, y, lineW, lineH };
            y += lineH + Metrics::rowGap;
        }
        else { out.playbackRing = {}; out.playbackLine = {}; }

        out.contentHeight = y + Metrics::pad - Metrics::rowGap;
        if (out.contentHeight < Metrics::pad * 2) out.contentHeight = Metrics::pad * 2;
    };

    const int fullW = juce::jmax (1, L.width - 2 * Metrics::pad);
    measure (fullW, L);
    const int roomForBody = juce::jmax (0, maxHeight - Metrics::headerH - footerH);
    L.scrolls = L.contentHeight > roomForBody;
    if (L.scrolls)
        measure (juce::jmax (1, fullW - Metrics::scrollbarW), L);   // the scrollbar takes its width from the text, not the text from the card
    const int bodyH = L.scrolls ? roomForBody : L.contentHeight;
    L.body   = { 0, Metrics::headerH, L.width, bodyH };
    L.footer = { 0, Metrics::headerH + bodyH, L.width, footerH };
    L.totalHeight = Metrics::headerH + bodyH + footerH;

    // footer buttons: natural widths, right-aligned, wrapping is not needed because they are few and short; at a
    // width where they would not fit they shrink evenly rather than leave the card (no truncation of the card).
    L.footerButtons.clear();
    if (footerH > 0)
    {
        const auto f = chipFont();
        std::vector<int> widths;
        int total = 0;
        for (const auto& lab : L.footerLabels) { const int w = juce::jmax (Metrics::chipMinW, f.getStringWidth (lab) + 24); widths.push_back (w); total += w; }
        total += Metrics::btnGap * (int) (widths.size() - 1);
        const int avail = L.width - 2 * Metrics::pad;
        if (total > avail && ! widths.empty())
        {
            const int each = juce::jmax (Metrics::chipMinW, (avail - Metrics::btnGap * ((int) widths.size() - 1)) / (int) widths.size());
            for (auto& w : widths) w = each;
        }
        int x = L.width - Metrics::pad;
        const int y = L.footer.getY() + (footerH - Metrics::btnH) / 2;
        for (int i = (int) widths.size() - 1; i >= 0; --i)
        {
            x -= widths[(size_t) i];
            L.footerButtons.insert (L.footerButtons.begin(), juce::Rectangle<int> (x, y, widths[(size_t) i], Metrics::btnH));
            x -= Metrics::btnGap;
        }
    }
    return L;
}

int EJAgentPanel::preferredHeight (int width, int maxHeight) const
{
    return computeLayout (width, maxHeight).totalHeight;
}

juce::String EJAgentPanel::checkLayout (const Layout& l)
{
    if (l.width <= 0) return "no width";
    const int x0 = Metrics::pad, w = l.contentWidth;
    const auto within = [&] (juce::Rectangle<int> r) { return r.isEmpty() || (r.getX() >= x0 && r.getRight() <= x0 + w); };
    if (! within (l.goal))        return "goal leaves the column";
    if (! within (l.text))        return "text leaves the column";
    if (! within (l.notice))      return "notice leaves the column";
    if (! within (l.planHeading)) return "plan heading leaves the column";
    if (! within (l.askQuestion)) return "ask question leaves the column";
    if (! within (l.playbackLine)) return "playback line leaves the column";
    const auto rows = checkRows (l.rows, l.rowRects, x0, w);
    if (rows.isNotEmpty()) return rows;
    for (size_t i = 0; i < l.chipRects.size(); ++i)
    {
        const auto& c = l.chipRects[i];
        if (c.getX() < x0 || c.getRight() > x0 + w) return "chip " + juce::String ((int) i) + " leaves the column";
        if (c.getWidth() < Metrics::chipMinW) return "chip " + juce::String ((int) i) + " narrower than the minimum";
        for (size_t j = 0; j < i; ++j) if (l.chipRects[j].intersects (c)) return "chips overlap";
    }
    if (! l.stop.isEmpty() && l.stop.getRight() > l.width) return "Stop leaves the card";
    for (size_t i = 0; i < l.footerButtons.size(); ++i)
    {
        const auto& b = l.footerButtons[i];
        if (b.getX() < 0 || b.getRight() > l.width) return "footer button " + juce::String ((int) i) + " leaves the card";
        for (size_t j = 0; j < i; ++j) if (l.footerButtons[j].intersects (b)) return "footer buttons overlap";
    }
    if (l.body.getBottom() > l.totalHeight || l.footer.getBottom() > l.totalHeight) return "body/footer leave the card";
    if (! l.scrolls && l.body.getHeight() < l.contentHeight) return "content taller than the body without a scrollbar";
    return {};
}

void EJAgentPanel::relayout()
{
    layout_ = computeLayout (getWidth(), getHeight());
    viewport_.setBounds (layout_.body);
    content_.setSize (layout_.contentWidth + 2 * Metrics::pad, layout_.contentHeight);
    placeButtons();
    ensureTimer();
    repaint();
}

void EJAgentPanel::resized()
{
    relayout();
}

juce::TextButton& EJAgentPanel::rowButton (size_t i)
{
    while (rowBtns_.size() <= i)
    {
        auto b = std::make_unique<juce::TextButton>();
        b->setWantsKeyboardFocus (false);
        content_.addChildComponent (*b);
        rowBtns_.push_back (std::move (b));
    }
    return *rowBtns_[i];
}
juce::TextButton& EJAgentPanel::chipButton (size_t i)
{
    while (chipBtns_.size() <= i)
    {
        auto b = std::make_unique<juce::TextButton>();
        b->setWantsKeyboardFocus (false);
        styleSecondary (*b);
        content_.addChildComponent (*b);
        chipBtns_.push_back (std::move (b));
    }
    return *chipBtns_[i];
}

void EJAgentPanel::placeButtons()
{
    const auto& L = layout_;
    // Stop
    stopBtn_.setVisible (! L.stop.isEmpty());
    if (! L.stop.isEmpty()) stopBtn_.setBounds (L.stop);

    // footer
    for (auto* b : { &approveAllBtn_, &applyBtn_, &declineBtn_, &gotItBtn_, &undoAllBtn_, &retryBtn_, &closeBtn_ }) b->setVisible (false);
    for (int i = 0; i < L.footerLabels.size() && i < (int) L.footerButtons.size(); ++i)
    {
        const auto& lab = L.footerLabels[i];
        juce::TextButton* b = nullptr;
        if      (lab == "Apply all")            b = &approveAllBtn_;
        else if (lab == "Apply")                b = &applyBtn_;
        else if (lab == "Skip all")             b = &declineBtn_;
        else if (lab == "Got it, you can stop") b = &gotItBtn_;
        else if (lab == "Undo all")             b = &undoAllBtn_;
        else if (lab == "Retry")                b = &retryBtn_;
        else if (lab == "Close")                b = &closeBtn_;
        if (b == nullptr) continue;
        b->setBounds (L.footerButtons[(size_t) i]);
        b->setVisible (true);
        b->toFront (false);
    }

    // rows
    size_t used = 0;
    for (size_t i = 0; i < L.rowRects.size(); ++i)
    {
        const int kind = L.rowButtonKind[i];
        const auto rect = L.rowRects[i].button;
        if (kind == 0 || rect.isEmpty()) continue;
        auto& b = rowButton (used++);
        const juce::String id = L.rowIds[i];
        b.setBounds (rect);
        if (kind == 1)
        {
            styleSecondary (b);
            b.setButtonText ("Undo");
            b.setTooltip ("Undo this step only");
            b.onClick = [this, id] { client_.undoStep (id); };
        }
        else
        {
            bool approved = true;
            for (const auto& s : client_.steps()) if (s.id == id) { approved = s.approved; break; }
            styleSecondary (b);
            b.setColour (juce::TextButton::textColourOffId, approved ? Colours::green : Colours::text3);
            b.setButtonText (approved ? "Apply" : "Skip");           // the line's decision, as the contract names it
            b.setTooltip (approved ? "This line will be applied - tap to skip it" : "This line is skipped - tap to apply it");
            b.onClick = [this, id, approved] { client_.setLineApproved (id, ! approved); };
        }
        b.setVisible (true);
    }
    for (size_t i = used; i < rowBtns_.size(); ++i) rowBtns_[i]->setVisible (false);

    // chips
    for (size_t i = 0; i < L.chipRects.size() && i < (size_t) L.chipLabels.size(); ++i)
    {
        auto& b = chipButton (i);
        const juce::String label = L.chipLabels[(int) i];
        b.setBounds (L.chipRects[i]);
        b.setButtonText (label);
        b.onClick = [this, label] { client_.answerAsk (label); };
        b.setVisible (true);
    }
    for (size_t i = L.chipRects.size(); i < chipBtns_.size(); ++i) chipBtns_[i]->setVisible (false);
}

// =============================================================================
//  paint
// =============================================================================
void EJAgentPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Colours::bg3);
    g.fillRoundedRectangle (r, EchoJayChrome::kFieldCorner);
    g.setColour (Colours::border2);
    g.drawRoundedRectangle (r, EchoJayChrome::kFieldCorner, 1.0f);

    // header: dot + status line
    const auto st = client_.state();
    juce::Colour dot = Colours::text3;
    switch (st)
    {
        case State::Connecting: case State::Streaming: case State::Executing: dot = Colours::blue2; break;
        case State::AwaitingApproval: case State::AwaitingAsk: case State::Listening: dot = Colours::amber; break;
        case State::Done:    dot = Colours::green; break;
        case State::Failed:  dot = Colours::red; break;
        case State::Stopped: dot = Colours::amber.withAlpha (0.7f); break;
        default: break;
    }
    if (st == State::Connecting || st == State::Streaming || st == State::Executing || st == State::Listening)
        dot = dot.withAlpha (0.55f + 0.45f * (0.5f + 0.5f * std::sin (phase_ * 2.0f)));
    const auto H = layout_.header;
    g.setColour (dot);
    g.fillEllipse ((float) H.getX() + Metrics::pad, (float) H.getCentreY() - 4.0f, 8.0f, 8.0f);
    const int textX = H.getX() + Metrics::pad + 14;
    const int textR = layout_.stop.isEmpty() ? H.getRight() - Metrics::pad : layout_.stop.getX() - Metrics::btnGap;
    g.setColour (Colours::text);
    g.setFont (headerFont());
    g.drawText ("Agent" + (client_.statusLine().isNotEmpty() ? juce::String::fromUTF8 (" \xc2\xb7 ") + client_.statusLine() : juce::String()),
                textX, H.getY(), juce::jmax (1, textR - textX), H.getHeight(), juce::Justification::centredLeft, true);
    // a hairline under the header and over the footer so the scrolling body reads as its own region
    g.setColour (Colours::border);
    g.drawHorizontalLine (H.getBottom(), (float) Metrics::pad, (float) getWidth() - Metrics::pad);
    if (! layout_.footer.isEmpty())
        g.drawHorizontalLine (layout_.footer.getY(), (float) Metrics::pad, (float) getWidth() - Metrics::pad);
}

void EJAgentPanel::paintGlyph (juce::Graphics& g, juce::Rectangle<int> r, Step::Status st) const
{
    const auto c = r.toFloat().withSizeKeepingCentre (12.0f, 12.0f);
    switch (st)
    {
        case Step::Status::Pending:
            g.setColour (Colours::text3); g.drawEllipse (c, 1.2f); break;
        case Step::Status::AwaitingApproval:
            g.setColour (Colours::amber); g.drawEllipse (c, 1.2f); break;
        case Step::Status::Running:
        {
            juce::Path p;
            p.addCentredArc (c.getCentreX(), c.getCentreY(), 6.0f, 6.0f, 0.0f, phase_, phase_ + juce::MathConstants<float>::pi * 1.3f, true);
            g.setColour (Colours::blue2);
            g.strokePath (p, juce::PathStrokeType (1.6f));
            break;
        }
        case Step::Status::Done:
        {
            juce::Path p;
            p.startNewSubPath (c.getX() + 1.5f, c.getCentreY() + 0.5f);
            p.lineTo (c.getX() + 5.0f, c.getBottom() - 1.5f);
            p.lineTo (c.getRight() - 1.0f, c.getY() + 1.5f);
            g.setColour (Colours::green);
            g.strokePath (p, juce::PathStrokeType (1.8f));
            break;
        }
        case Step::Status::Failed:
        {
            g.setColour (Colours::red);
            g.drawLine (c.getX() + 2.0f, c.getY() + 2.0f, c.getRight() - 2.0f, c.getBottom() - 2.0f, 1.6f);
            g.drawLine (c.getRight() - 2.0f, c.getY() + 2.0f, c.getX() + 2.0f, c.getBottom() - 2.0f, 1.6f);
            break;
        }
        case Step::Status::Declined: case Step::Status::NotRun:
            g.setColour (Colours::text3);
            g.drawLine (c.getX() + 2.0f, c.getCentreY(), c.getRight() - 2.0f, c.getCentreY(), 1.6f);
            break;
        case Step::Status::Undone:
            g.setColour (Colours::amber);
            g.drawEllipse (c, 1.2f);
            g.drawLine (c.getX() + 3.0f, c.getCentreY(), c.getRight() - 3.0f, c.getCentreY(), 1.2f);
            break;
        case Step::Status::Stopped:
            g.setColour (Colours::text3);
            g.fillRect (c.reduced (2.5f));
            break;
    }
}

void EJAgentPanel::paintContent (juce::Graphics& g)
{
    const auto& L = layout_;
    if (! L.goal.isEmpty())   drawWrapped (g, "Goal: " + client_.goal(), detailFont(), Colours::text2, L.goal);
    if (! L.text.isEmpty())   drawWrapped (g, client_.liveText().trim(), bodyFont(), Colours::text, L.text);
    if (! L.notice.isEmpty()) drawWrapped (g, client_.notice(), detailFont(), Colours::amber, L.notice);
    if (! L.planHeading.isEmpty()) drawWrapped (g, client_.planHeading(), bodyFont(), Colours::text, L.planHeading);

    const auto& steps = client_.steps();
    for (size_t i = 0; i < L.rowRects.size() && i < steps.size(); ++i)
    {
        const auto& rr = L.rowRects[i];
        const auto& s  = steps[i];
        paintGlyph (g, rr.glyph, s.status);
        const bool dim = s.status == Step::Status::Declined || s.status == Step::Status::NotRun || s.status == Step::Status::Stopped;
        drawWrapped (g, L.rows[i].label, rowFont(), dim ? Colours::text3 : Colours::text, rr.label);
        if (! rr.detail.isEmpty())
        {
            const auto dc = s.status == Step::Status::Failed ? Colours::red.withAlpha (0.9f)
                          : s.status == Step::Status::Undone ? Colours::amber
                          : Colours::text3;
            drawWrapped (g, L.rows[i].detail, detailFont(), dc, rr.detail);
        }
    }

    if (! L.askQuestion.isEmpty())
        drawWrapped (g, client_.pendingAsk().question, bodyFont(), Colours::text, L.askQuestion);

    if (! L.playbackRing.isEmpty())
    {
        // the listening indicator: a ring that fills as the heard seconds approach the minimum, pulsing while empty
        const auto pb = client_.playback();
        const auto c = L.playbackRing.toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        g.setColour (Colours::border2);
        g.drawEllipse (c, 1.5f);
        const float frac = pb.minSeconds > 0.0 ? juce::jlimit (0.0f, 1.0f, (float) (pb.heardSeconds / pb.minSeconds)) : 0.0f;
        juce::Path p;
        const float a0 = -juce::MathConstants<float>::halfPi;
        if (frac > 0.0f)
            p.addCentredArc (c.getCentreX(), c.getCentreY(), 7.0f, 7.0f, 0.0f, a0, a0 + frac * juce::MathConstants<float>::twoPi, true);
        else
            p.addCentredArc (c.getCentreX(), c.getCentreY(), 7.0f, 7.0f, 0.0f, phase_, phase_ + 1.2f, true);
        g.setColour (pb.reached ? Colours::green : Colours::blue2);
        g.strokePath (p, juce::PathStrokeType (1.8f));
        drawWrapped (g, pb.line, bodyFont(), Colours::text, L.playbackLine);
    }
}

// =============================================================================
//  listener + animation
// =============================================================================
void EJAgentPanel::ensureTimer()
{
    bool animate = client_.playback().active
                || client_.state() == State::Connecting || client_.state() == State::Streaming || client_.state() == State::Executing;
    for (const auto& s : client_.steps()) if (s.status == Step::Status::Running) { animate = true; break; }
    if (animate) { if (! isTimerRunning()) startTimerHz (15); }
    else if (isTimerRunning()) stopTimer();
}

void EJAgentPanel::timerCallback()
{
    phase_ += 0.18f;
    if (phase_ > juce::MathConstants<float>::twoPi * 100.0f) phase_ -= juce::MathConstants<float>::twoPi * 100.0f;
    repaint (layout_.header);
    content_.repaint();
}

void EJAgentPanel::agentChanged()
{
    const bool shown = shouldShow();
    const int natural = shown ? computeLayout (juce::jmax (getWidth(), 1), 1 << 20).totalHeight : 0;
    const bool geometryChanged = (shown != lastShown_) || (natural != lastNaturalH_);
    lastShown_ = shown;
    lastNaturalH_ = natural;
    relayout();
    if (geometryChanged && onLayoutNeeded) onLayoutNeeded();   // the editor re-docks the card at the new height
}

void EJAgentPanel::agentTranscript (const juce::String& role, const juce::String& text)
{
    if (onTranscript) onTranscript (role, text);
}
