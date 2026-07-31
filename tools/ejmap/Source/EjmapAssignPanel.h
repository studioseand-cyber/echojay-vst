/*
  EjmapAssignPanel.h

  M4 UI: the keyboard-driven loop. Fifty runs of this loop is the product, so
  every action is a single keypress, every keypress writes its evidence
  immediately (session file plus captures rows), and nothing advances
  silently.

  Keys (signed map): SPACE accept (corroborated only, see EjmapAssignment.h),
  W wiggle-verify, N/A/D the three skips (Shift+ for a custom reason),
  R recapture, T typed anchors, I bulk-accept eligible ignores (two-press,
  count shown first), 1-9 pick a gesture candidate, arrows navigate,
  ? evidence for the current row, S skip plugin, cmd+return submit.

  Actions live in action*() methods and keys call them, so the self-test
  drives the same code path a keypress does.
*/

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "EjmapAssignment.h"
#include "EjmapCapture.h"

namespace ejmap
{

class AssignPanel : public juce::Component,
                    private juce::ListBoxModel,
                    private juce::Timer
{
public:
    struct Hooks
    {
        std::function<void()>                          armForRow;      // W: arm; result via captureArrived
        std::function<SweepOutcome (int)>              sweepIndex;     // synchronous, pump-paused
        std::function<void (int)>                      startTyped;     // T; completion via typedCompleted
        std::function<juce::String (int)>              paramName;
        std::function<int()>                           paramCount;
        std::function<void (const juce::String&)>      status;         // one-line readout
        std::function<void (const juce::var&)>         writeRow;       // captures jsonl writer
        std::function<void (const juce::var&)>         writeMisclassified;
        std::function<void (juce::Array<AssignRow>&, const juce::String& category,
                            const juce::String& sessionMode)> submit;
        std::function<void()>                          exitPanel;
    } hooks;

    bool deepMode = false;

    AssignPanel()
    {
        addAndMakeVisible (progress);
        progress.setColour (juce::Label::textColourId, juce::Colour (0xff9fd8e0));
        progress.setFont (juce::FontOptions (12.0f));

        // THE QUESTION STRIP. A row must state its question: what is being
        // confirmed, in mix-engineer words, and what each answer means. The
        // 90-second gate's first failed run cost 55 seconds on one row
        // because the row did not say whether to hunt for a control or
        // record its absence.
        addAndMakeVisible (question);
        question.setColour (juce::Label::textColourId, juce::Colour (0xffd8d0a0));
        question.setFont (juce::FontOptions (13.0f));
        question.setJustificationType (juce::Justification::topLeft);
        question.setMinimumHorizontalScale (1.0f);

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (20);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff10141c));

        addChildComponent (reasonEntry);
        reasonEntry.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff161c26));
        reasonEntry.onReturnKey = [this] { commitCustomReason(); };
        reasonEntry.onEscapeKey = [this] { pendingSkip = {}; reasonEntry.setVisible (false); grabKeyboardFocus(); };

        setWantsKeyboardFocus (true);
        startTimer (1000);
    }

    //==========================================================================
    void begin (const juce::File& rootIn, const juce::String& fpIn,
                const juce::String& pluginIdIn, const ProposalSet& proposals,
                const EvidenceIndex& evidenceIn)
    {
        root = rootIn; fp = fpIn; pluginId = pluginIdIn;
        evidence = evidenceIn;
        category = proposals.present ? proposals.category : "compressor";
        startedAt = juce::Time::getMillisecondCounter();
        rows.clear();
        ignoreRows.clear();
        bulkArmedAt = 0;

        // 1. Classifier proposals with semantic kinds.
        juce::StringArray coveredSemantics;
        if (proposals.present)
            for (const auto& e : proposals.entries)
            {
                if (e.kind == "ignore" || e.kind == "unsure")
                    continue;
                AssignRow r;
                r.semantic = e.kind;
                r.kind = e.kind;
                r.proposedIndex = e.index;
                r.proposedName = hooks.paramName ? hooks.paramName (e.index) : juce::String();
                r.proposalSource = "classifier";
                r.proposalConfidence = e.confidence;
                r.proposalReason = e.reason;
                r.proposalChannel = e.channel;
                rows.add (r);
                coveredSemantics.addIfNotAlreadyThere (e.kind);
            }

        // 2. Unsure proposals: shown first, skippable, never SPACE-able. The
        //    dial-set rows are where a human resolves what these point at.
        if (proposals.present)
            for (const auto& e : proposals.entries)
                if (e.kind == "unsure")
                {
                    AssignRow r;
                    r.semantic = {};
                    r.kind = "unsure";
                    r.proposedIndex = e.index;
                    r.proposedName = hooks.paramName ? hooks.paramName (e.index) : juce::String();
                    r.proposalSource = "classifier";
                    r.proposalConfidence = e.confidence;
                    r.proposalReason = e.reason;
                    rows.add (r);
                }

        // 3. Dial-set semantics with no proposal.
        for (const auto& s : DialSets::forCategory (category))
            if (! coveredSemantics.contains (s))
            {
                AssignRow r;
                r.semantic = s;
                r.kind = s;
                r.proposalSource = "none";
                rows.add (r);
            }

        // 4. Ignores, bottom of the list, bulk-acceptable under the floor.
        if (proposals.present)
            for (const auto& e : proposals.entries)
                if (e.kind == "ignore")
                {
                    AssignRow r;
                    r.semantic = {};
                    r.kind = "ignore";
                    r.proposedIndex = e.index;
                    r.proposedName = hooks.paramName ? hooks.paramName (e.index) : juce::String();
                    r.proposalSource = "classifier";
                    r.proposalConfidence = e.confidence;
                    r.proposalReason = e.reason;
                    ignoreRows.add (r);
                }

        sortRows();
        restoreSession();
        selected = firstUnresolved();
        list.selectRow (selected);
        list.updateContent();
        updateProgress();
        updateQuestion();
        say ("Assign: " + juce::String (rows.size()) + " rows, "
               + juce::String (ignoreRows.size()) + " classifier ignores. "
               + (proposals.present ? "Proposals loaded." : "NO proposals for this fp: dial-set rows only."));
    }

    int rowCount() const { return rows.size() + ignoreRows.size(); }
    AssignRow& rowAt (int i) { return i < rows.size() ? rows.getReference (i)
                                                      : ignoreRows.getReference (i - rows.size()); }
    int selectedRow() const { return selected; }
    juce::String currentQuestionText() const { return question.getText(); }
    void selectRow (int i) { selected = juce::jlimit (0, juce::jmax (0, rowCount() - 1), i);
                             list.selectRow (selected); list.updateContent(); updateQuestion(); }

    //==========================================================================
    // Actions. Keys call these; the self-test calls these.

    /** SPACE. Fast lane only, and only on corroborated proposals. */
    void actionSpace()
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);

        if (r.semantic.isEmpty() || r.proposedIndex < 0)
        { say ("SPACE needs a proposal with a semantic. W to capture, or skip."); return; }
        if (r.isResolved())
        { say (r.semantic + " already " + r.stateString()); return; }
        if (deepMode)
        { say ("Deep mode: SPACE is disabled, W to wiggle-verify."); return; }

        const auto cor = evidence.corroborationFor (r.proposedIndex, r.proposedName);
        if (cor.isEmpty())
        {
            say ("UNCORROBORATED: " + r.semantic + " -> [" + juce::String (r.proposedIndex)
                   + "] " + r.proposedName + " has no capture, no co-move, no stride on disk. "
                   "W to verify. The fastest path must not produce the least evidence.");
            return;
        }

        auto sw = hooks.sweepIndex ? hooks.sweepIndex (r.proposedIndex) : SweepOutcome();
        r.sweep = sw;
        r.resolvedIndex = r.proposedIndex;
        r.corroboration = cor;
        r.mode = "fast";

        if (sw.ok)
        {
            r.state = AssignRow::State::confirmed;
            r.trust = "llm-classified";
            r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
            recordResolution (r, "space_accept");
            supersedeSiblings (r);
            say ("ACCEPTED " + r.semantic + " -> [" + juce::String (r.resolvedIndex) + "] "
                   + r.proposedName + " (corroborated by " + cor + ", " + sw.reason + ")");
            advance();
        }
        else
        {
            r.state = AssignRow::State::swept;
            say (r.semantic + " swept but not confirmable: " + sw.reason
                   + (sw.flat ? "  T for typed anchors." : ""));
        }
        persistSession();
        list.updateContent();
        updateProgress();
    }

    /** W / R. Arm; the human touches the control; captureArrived finishes. */
    void actionWiggle()
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);
        if (r.kind == "ignore") { say ("Ignore rows are skipped, not captured."); return; }
        r.state = AssignRow::State::armed;
        awaitingCaptureRow = selected;
        say ("ARMED for " + (r.semantic.isNotEmpty() ? r.semantic : juce::String ("unsure row"))
               + " - move the control on the plugin");
        if (hooks.armForRow) hooks.armForRow();
        list.updateContent();
    }

    /** Routed from the capture engine via MainComponent. */
    void captureArrived (const CaptureEngine::Result& res)
    {
        if (awaitingCaptureRow < 0 || awaitingCaptureRow >= rowCount())
            return;
        auto& r = rowAt (awaitingCaptureRow);

        if (res.kind == CaptureEngine::Result::Kind::notAutomatable)
        {
            r.state = AssignRow::State::proposed;
            pendingAutoSkipReason = res.reason;
            say ("Nothing moved: " + res.reason + "  Press A to record not_automatable.");
            list.updateContent();
            return;
        }

        if (res.kind == CaptureEngine::Result::Kind::gesture && res.primaryIndex < 0)
        {
            lastGesture = res;
            r.state = AssignRow::State::captured;
            juce::String cands;
            for (int i = 0; i < juce::jmin (9, res.indices.size()); ++i)
                cands << (i ? "  " : "") << juce::String (i + 1) << ":" << res.names[i];
            say ("Multi-move, pick with a digit: " + cands);
            list.updateContent();
            return;
        }

        const int idx = res.primaryIndex >= 0 ? res.primaryIndex
                        : (res.indices.size() == 1 ? res.indices[0] : -1);
        if (idx < 0) { say ("Capture unusable: " + res.reason); return; }

        finishCaptureWith (idx, res);
    }

    /** Digit keys resolve a pending multi-move. */
    void actionPickCandidate (int oneBased)
    {
        if (awaitingCaptureRow < 0 || ! juce::isPositiveAndBelow (oneBased - 1, lastGesture.indices.size()))
            return;
        finishCaptureWith (lastGesture.indices[oneBased - 1], lastGesture);
    }

    /** The three skips. Canned reason unless customReason is non-empty. */
    void actionSkip (AssignRow::State outcome, const juce::String& customReason = {})
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);
        if (r.state == AssignRow::State::confirmed) { say ("Row already confirmed; navigate elsewhere."); return; }

        juce::String canned;
        if (outcome == AssignRow::State::skipNotPresent)
            canned = r.kind == "ignore" ? "classifier: " + r.proposalReason
                                        : "mapper: no such control on this plugin";
        else if (outcome == AssignRow::State::skipNotAutomatable)
            canned = pendingAutoSkipReason.isNotEmpty()
                       ? pendingAutoSkipReason
                       : "mapper: control exists without an automatable parameter";
        else
            canned = "deferred by mapper";

        r.state = outcome;
        r.skipReason = customReason.isNotEmpty() ? customReason : canned;
        r.mode = deepMode ? "deep" : "fast";
        r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
        pendingAutoSkipReason = {};
        recordResolution (r, "skip");
        persistSession();
        say ((r.semantic.isNotEmpty() ? r.semantic : r.proposedName) + " -> "
               + r.stateString() + " (" + r.skipReason + ")");
        advance();
        list.updateContent();
        updateProgress();
    }

    /** Shift+skip: same outcome, human reason. */
    void beginCustomReason (AssignRow::State outcome)
    {
        pendingSkip = outcome;
        reasonEntry.setVisible (true);
        reasonEntry.setText ({}, juce::dontSendNotification);
        reasonEntry.grabKeyboardFocus();
        resized();
        say ("Reason, then Enter (Esc cancels):");
    }

    /** I, twice. The floor (signed): only ignores whose index has NO capture,
        NO co-movement and NO dial-set name match. Confidence is not a filter:
        1,276 high and zero unsure, measured, so it filters nothing.
    */
    void actionBulkIgnores()
    {
        juce::Array<int> eligible;
        for (int i = 0; i < ignoreRows.size(); ++i)
        {
            const auto& r = ignoreRows.getReference (i);
            if (r.isResolved()) continue;
            if (evidence.captured.contains (r.proposedIndex)) continue;
            if (evidence.coMoved.contains (r.proposedIndex)) continue;
            if (DialSets::nameSuggestsDialSet (r.proposedName)) continue;
            eligible.add (i);
        }

        const int withheld = ignoreRows.size() - eligible.size();
        const auto now = juce::Time::getMillisecondCounter();

        if (bulkArmedAt == 0 || (int) (now - bulkArmedAt) > 6000)
        {
            bulkArmedAt = now;
            say ("I will bulk-accept " + juce::String (eligible.size()) + " of "
                   + juce::String (ignoreRows.size()) + " ignores ("
                   + juce::String (withheld) + " withheld: evidence or dial-set name). "
                   "Press I again to confirm.");
            return;
        }
        bulkArmedAt = 0;

        for (int i : eligible)
        {
            auto& r = ignoreRows.getReference (i);
            r.state = AssignRow::State::skipNotPresent;
            r.skipReason = "classifier-ignore, bulk-accepted: " + r.proposalReason;
            r.mode = deepMode ? "deep" : "fast";
            r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
            recordResolution (r, "bulk_ignore");
        }
        persistSession();
        say ("Bulk-accepted " + juce::String (eligible.size()) + " ignores; "
               + juce::String (withheld) + " withheld for individual review.");
        list.updateContent();
        updateProgress();
    }

    /** ?: the evidence the current row rests on. */
    void actionEvidence()
    {
        if (rowCount() == 0) return;
        const auto& r = rowAt (selected);
        juce::String t;
        t << (r.semantic.isNotEmpty() ? r.semantic : r.kind) << ": ";
        t << "proposal " << (r.proposalSource == "classifier"
                               ? "[" + juce::String (r.proposedIndex) + "] " + r.proposedName
                                   + " (" + r.proposalConfidence + ": " + r.proposalReason + ")"
                               : juce::String ("none"));
        const auto cor = r.proposedIndex >= 0
                           ? evidence.corroborationFor (r.proposedIndex, r.proposedName)
                           : juce::String();
        t << " | corroboration " << (cor.isEmpty() ? "NONE" : cor);
        if (r.resolvedIndex >= 0 && r.resolvedIndex != r.proposedIndex)
            t << " | RE-POINTED to [" << r.resolvedIndex << "]";
        if (r.proposalMismatch) t << " (proposal mismatch, recorded)";
        t << " | sweep " << (r.sweep.method.isEmpty() ? "none"
                               : r.sweep.method + ", " + juce::String (r.sweep.anchors.size()) + " anchors"
                                   + (r.sweep.identityDisplay ? ", IDENTITY DISPLAY" : ""));
        t << " | trust " << (r.trust.isEmpty() ? "unresolved" : r.trust);
        t << " | mode " << (r.mode.isEmpty() ? (deepMode ? "(deep lane)" : "(fast lane)") : r.mode);
        say (t);
    }

    void actionTyped()
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);
        const int idx = r.resolvedIndex >= 0 ? r.resolvedIndex : r.proposedIndex;
        if (idx < 0) { say ("No index to type against: W first."); return; }
        typedRow = selected;
        if (hooks.startTyped) hooks.startTyped (idx);
    }

    /** Routed from the typed flow's completion. */
    void typedCompleted (const SweepOutcome& sw)
    {
        if (typedRow < 0 || typedRow >= rowCount()) return;
        auto& r = rowAt (typedRow);
        r.sweep = sw;
        if (sw.ok && r.semantic.isNotEmpty())
        {
            r.state = AssignRow::State::confirmed;
            r.resolvedIndex = r.resolvedIndex >= 0 ? r.resolvedIndex : r.proposedIndex;
            r.trust = "human-verified";
            r.mode = deepMode ? "deep" : "fast";
            r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
            recordResolution (r, "typed");
            supersedeSiblings (r);
            say ("TYPED " + r.semantic + " confirmed: " + sw.reason);
        }
        else
            say ("Typed flow finished without a usable table: " + sw.reason);
        typedRow = -1;
        persistSession();
        list.updateContent();
        updateProgress();
    }

    /** S: every unresolved semantic row becomes deferred, recorded, exit. */
    void actionSkipPlugin()
    {
        for (auto& r : rows)
            if (! r.isResolved())
            {
                r.state = AssignRow::State::skipDeferred;
                r.skipReason = "plugin skipped by mapper";
                r.mode = deepMode ? "deep" : "fast";
                r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
                recordResolution (r, "skip_plugin");
            }
        persistSession();
        say ("Plugin skipped: every unresolved row recorded as deferred.");
        if (hooks.exitPanel) hooks.exitPanel();
    }

    /** cmd+return. */
    void actionSubmit()
    {
        int confirmed = 0;
        for (const auto& r : rows) confirmed += r.state == AssignRow::State::confirmed;
        if (confirmed == 0) { say ("Nothing confirmed: refusing to submit an empty map."); return; }

        // Unresolved rows become deferred AT submit, recorded: a map is a
        // statement about every row, including the ones nobody finished.
        for (auto& r : rows)
            if (! r.isResolved())
            {
                r.state = AssignRow::State::skipDeferred;
                r.skipReason = "unresolved at submit";
                r.mode = deepMode ? "deep" : "fast";
                r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
                recordResolution (r, "submit_defer");
            }
        persistSession();
        if (hooks.submit) hooks.submit (rows, category, deepMode ? "deep" : "fast");
    }

    void advance()
    {
        const int next = firstUnresolvedFrom (selected + 1);
        selectRow (next >= 0 ? next : selected);
    }

    juce::String currentCategory() const { return category; }
    void setCategory (const juce::String& c) { category = c; }

    //==========================================================================
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (reasonEntry.isVisible()) return false;   // text entry owns the keys

        const auto c = k.getTextCharacter();
        const bool shift = k.getModifiers().isShiftDown();

        if (k == juce::KeyPress::spaceKey)                        { actionSpace(); return true; }
        if (c == 'w' || c == 'W' || c == 'r' || c == 'R')         { actionWiggle(); return true; }
        if (c == 'n' || c == 'N')
        { shift ? beginCustomReason (AssignRow::State::skipNotPresent)
                : actionSkip (AssignRow::State::skipNotPresent); return true; }
        if (c == 'a' || c == 'A')
        { shift ? beginCustomReason (AssignRow::State::skipNotAutomatable)
                : actionSkip (AssignRow::State::skipNotAutomatable); return true; }
        if (c == 'd' || c == 'D')
        { shift ? beginCustomReason (AssignRow::State::skipDeferred)
                : actionSkip (AssignRow::State::skipDeferred); return true; }
        if (c == 't' || c == 'T')                                 { actionTyped(); return true; }
        if (c == 'i' || c == 'I')                                 { actionBulkIgnores(); return true; }
        if (c == '?')                                             { actionEvidence(); return true; }
        if (c == 's' || c == 'S')                                 { actionSkipPlugin(); return true; }
        if (c >= '1' && c <= '9')                                 { actionPickCandidate (c - '0'); return true; }
        if (k == juce::KeyPress::leftKey  || k == juce::KeyPress::upKey)   { selectRow (selected - 1); return true; }
        if (k == juce::KeyPress::rightKey || k == juce::KeyPress::downKey) { selectRow (selected + 1); return true; }
        if (k == juce::KeyPress::returnKey && k.getModifiers().isCommandDown()) { actionSubmit(); return true; }
        return false;
    }

    //==========================================================================
    void resized() override
    {
        auto r = getLocalBounds();
        progress.setBounds (r.removeFromTop (18));
        question.setBounds (r.removeFromTop (52));
        if (reasonEntry.isVisible())
            reasonEntry.setBounds (r.removeFromTop (22));
        list.setBounds (r);
    }

    void selectedRowsChanged (int row) override
    {
        if (row >= 0) selected = row;
        updateQuestion();
    }

    /** Mix-engineer words: the SHARED semanticLabel plus the unit, and mode
        rows carry their parameter name because four rows reading "mode" are
        four different switches wearing one uniform.
    */
    juce::String displayLabel (const AssignRow& r) const
    {
        if (r.kind == "ignore")  return "ignore: " + r.proposedName;
        if (r.kind == "unsure")  return "unsure: " + r.proposedName;
        auto label = echojay::semanticLabel (r.semantic);
        auto unit = echojay::semanticUnit (r.semantic);
        if (unit == "db")  unit = "dB";
        else if (unit == "hz")  unit = "Hz";
        else if (unit == "pct") unit = "%";
        if (unit.isNotEmpty()) label << " (" << unit << ")";
        if (r.semantic == "mode" && r.proposedName.isNotEmpty())
            label << " - " << r.proposedName;
        return label;
    }

    /** The question the current row is asking, with the answers and their
        cost spelled out. This is what a human reads fifty times, so it says
        exactly what a keypress will do.
    */
    void updateQuestion()
    {
        if (rowCount() == 0) { question.setText ({}, juce::dontSendNotification); return; }
        auto& r = rowAt (selected);
        const auto label = displayLabel (r);
        juce::String q;

        if (r.isResolved())
        {
            q << label << ": " << r.stateString()
              << (r.skipReason.isNotEmpty() ? " (" + r.skipReason + ")" : juce::String())
              << ". Arrow on; W re-opens it.";
        }
        else if (r.kind == "ignore")
        {
            q << "Classifier says IGNORE [" << r.proposedIndex << "] " << r.proposedName
              << ": " << r.proposalReason << "\n"
              << "N agree (one key) - W dispute by touching it - I bulk-accepts all eligible ignores";
        }
        else if (r.kind == "unsure")
        {
            q << "Classifier is UNSURE about [" << r.proposedIndex << "] " << r.proposedName
              << ": " << r.proposalReason << "\n"
              << "Nothing to confirm HERE: if it belongs to a semantic, W on that row. "
              << "D defers this note (one key).";
        }
        else if (r.proposedIndex >= 0)
        {
            const auto cor = evidence.corroborationFor (r.proposedIndex, r.proposedName);
            q << "Is [" << r.proposedIndex << "] '" << r.proposedName
              << "' this plugin's " << label << "?\n";
            if (deepMode)
                q << "W touch it to verify (Deep: SPACE disabled)";
            else if (cor.isNotEmpty())
                q << "SPACE yes (evidence on disk: " << cor << ") - W touch it to verify";
            else
                q << "W touch it to verify (SPACE locked: no evidence for this index yet)";
            q << " - N no such control - D later";
        }
        else
        {
            q << "Does this plugin have a " << label << " control? NO index proposed.\n"
              << "W touch it on the GUI - N it does not exist (ONE KEY, four seconds) - D later";
        }
        question.setText (q, juce::dontSendNotification);
    }

    int getNumRows() override { return rowCount(); }

    void paintListBoxItem (int i, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (i >= rowCount()) return;
        auto& r = const_cast<AssignPanel*> (this)->rowAt (i);

        if (sel) g.fillAll (juce::Colour (0xff223040));
        juce::Colour col = juce::Colour (0xff9fd8e0);
        if (r.state == AssignRow::State::confirmed) col = juce::Colour (0xff6ad86a);
        else if (r.isSkipped())                     col = juce::Colour (0xff8090a0);
        else if (r.kind == "unsure")                col = juce::Colour (0xffd8b06a);
        else if (r.kind == "ignore")                col = juce::Colour (0xff607080);
        g.setColour (col);
        g.setFont (13.0f);

        juce::String t;
        t << (r.state == AssignRow::State::confirmed ? juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93 "))
              : r.isSkipped() ? "- " : "  ");
        t << displayLabel (r);
        if (r.kind != "ignore" && r.kind != "unsure")
        {
            if (r.proposedIndex >= 0)
                t << "  <- [" << (r.resolvedIndex >= 0 ? r.resolvedIndex : r.proposedIndex) << "] "
                  << r.proposedName;
            else if (! r.isResolved())
                t << "  (unmapped: N if absent)";
        }
        if (r.proposalMismatch) t << "  re-pointed";
        if (r.trust.isNotEmpty()) t << "  " << r.trust;
        g.drawText (t, 4, 0, w - 8, h, juce::Justification::centredLeft);
    }

    void listBoxItemClicked (int i, const juce::MouseEvent&) override { selectRow (i); grabKeyboardFocus(); }

    juce::Array<AssignRow> rows, ignoreRows;

private:
    void finishCaptureWith (int idx, const CaptureEngine::Result& res)
    {
        auto& r = rowAt (awaitingCaptureRow);
        awaitingCaptureRow = -1;

        r.resolvedIndex = idx;
        for (int i = 0; i < res.indices.size(); ++i)
            if (res.indices[i] != idx) r.coMoved.add (res.indices[i]);

        // Mismatch: the classifier's ground truth arriving free. Re-point the
        // row, keep the wrong proposal in evidence, write the labelled row.
        if (r.proposalSource == "classifier" && r.proposedIndex >= 0 && idx != r.proposedIndex)
        {
            r.proposalMismatch = true;
            auto* o = new juce::DynamicObject();
            o->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
            o->setProperty ("fp", fp);
            o->setProperty ("plugin_id", pluginId);
            o->setProperty ("semantic", r.semantic);
            o->setProperty ("proposed_index", r.proposedIndex);
            o->setProperty ("proposed_name", r.proposedName);
            o->setProperty ("captured_index", idx);
            o->setProperty ("captured_name", hooks.paramName ? hooks.paramName (idx) : juce::String());
            o->setProperty ("classifier_reason", r.proposalReason);
            o->setProperty ("classifier_confidence", r.proposalConfidence);
            if (hooks.writeMisclassified) hooks.writeMisclassified (juce::var (o));
        }

        r.state = AssignRow::State::captured;
        auto sw = hooks.sweepIndex ? hooks.sweepIndex (idx) : SweepOutcome();
        r.sweep = sw;

        if (r.semantic.isEmpty())
        {
            say ("Captured [" + juce::String (idx) + "] "
                   + (hooks.paramName ? hooks.paramName (idx) : juce::String())
                   + " on an unsure row: navigate to the dial-set row it belongs to and W there, "
                     "or skip this row.");
            r.state = AssignRow::State::proposed;
            persistSession();
            list.updateContent();
            return;
        }

        if (sw.ok)
        {
            r.state = AssignRow::State::confirmed;
            r.trust = "human-verified";
            r.mode = deepMode ? "deep" : "fast";
            r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
            recordResolution (r, r.proposalMismatch ? "wiggle_repoint" : "wiggle_verify");
            supersedeSiblings (r);
            say ("VERIFIED " + r.semantic + " -> [" + juce::String (idx) + "] "
                   + (hooks.paramName ? hooks.paramName (idx) : juce::String())
                   + (r.proposalMismatch ? "  (proposal was wrong, recorded)" : "")
                   + "  " + sw.reason);
            advance();
        }
        else
        {
            r.state = AssignRow::State::swept;
            say (r.semantic + " captured but sweep not confirmable: " + sw.reason
                   + (sw.flat ? "  T for typed anchors." : ""));
        }

        // The capture is now on-disk evidence for later rows in this session.
        evidence.captured.add (idx);
        for (int i : r.coMoved) evidence.coMoved.add (i);
        EvidenceIndex::Cap c; c.index = idx;
        c.name = hooks.paramName ? hooks.paramName (idx) : juce::String();
        evidence.captures.add (c);

        persistSession();
        list.updateContent();
        updateProgress();
    }

    /** Confirming a semantic auto-defers sibling rows proposing the same
        semantic: one index per key, and the losers are recorded, not dropped.
    */
    void supersedeSiblings (const AssignRow& winner)
    {
        for (auto& r : rows)
            if (&r != &winner && r.semantic == winner.semantic && ! r.isResolved())
            {
                r.state = AssignRow::State::skipDeferred;
                r.skipReason = "superseded: [" + juce::String (winner.resolvedIndex)
                             + "] confirmed for " + winner.semantic;
                r.mode = deepMode ? "deep" : "fast";
                r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
                recordResolution (r, "superseded");
            }
    }

    void recordResolution (const AssignRow& r, const juce::String& action)
    {
        auto v = r.toVar();
        if (auto* o = v.getDynamicObject())
        {
            o->setProperty ("kind", "assign_" + juce::String (r.isSkipped() ? "skip" : "resolve"));
            o->setProperty ("action", action);
            o->setProperty ("fp", fp);
        }
        if (hooks.writeRow) hooks.writeRow (v);
    }

    void commitCustomReason()
    {
        const auto reason = reasonEntry.getText().trim();
        reasonEntry.setVisible (false);
        resized();
        grabKeyboardFocus();
        if (pendingSkip.hasValue() && reason.isNotEmpty())
            actionSkip (*pendingSkip, "mapper: " + reason);
        pendingSkip = {};
    }

    void sortRows()
    {
        // unsure and low confidence first, then unmapped dial set, then the
        // rest; confirmed sink on repaint via advance(), not by resorting
        // (rows must not move under the selection).
        std::stable_sort (rows.begin(), rows.end(),
            [] (const AssignRow& a, const AssignRow& b)
            {
                auto rank = [] (const AssignRow& r)
                {
                    if (r.kind == "unsure") return 0;
                    if (r.proposalSource == "classifier" && r.proposalConfidence != "high") return 1;
                    if (r.proposalSource == "none") return 2;
                    return 3;
                };
                return rank (a) < rank (b);
            });
    }

    int firstUnresolved() const { return firstUnresolvedFrom (0); }
    int firstUnresolvedFrom (int start) const
    {
        for (int i = start; i < rows.size(); ++i)
            if (! rows.getReference (i).isResolved()) return i;
        for (int i = 0; i < rows.size(); ++i)
            if (! rows.getReference (i).isResolved()) return i;
        return juce::jmax (0, juce::jmin (start, rowCount() - 1));
    }

    //==========================================================================
    juce::File sessionFile() const { return root.getChildFile ("assign-" + fp + ".json"); }

    void persistSession()
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("fp", fp);
        o->setProperty ("plugin_id", pluginId);
        o->setProperty ("category", category);
        juce::Array<juce::var> rv, iv;
        for (const auto& r : rows) rv.add (r.toVar());
        for (const auto& r : ignoreRows) iv.add (r.toVar());
        o->setProperty ("rows", juce::var (rv));
        o->setProperty ("ignore_rows", juce::var (iv));
        sessionFile().replaceWithText (juce::JSON::toString (juce::var (o), false));
    }

    void restoreSession()
    {
        auto f = sessionFile();
        if (! f.existsAsFile()) return;
        auto v = juce::JSON::parse (f.loadFileAsString());
        if (v.getProperty ("fp", "").toString() != fp) return;

        auto restore = [] (juce::Array<AssignRow>& dst, const juce::var& src)
        {
            auto* arr = src.getArray();
            if (arr == nullptr) return;
            for (auto& rv : *arr)
            {
                const auto sem  = rv.getProperty ("semantic", "").toString();
                const int  pidx = (int) rv.getProperty ("proposed_index", -1);
                for (auto& r : dst)
                {
                    if (r.semantic != sem || r.proposedIndex != pidx) continue;
                    const auto st = rv.getProperty ("state", "").toString();
                    if (st == "confirmed")            r.state = AssignRow::State::confirmed;
                    else if (st == "not_present")     r.state = AssignRow::State::skipNotPresent;
                    else if (st == "not_automatable") r.state = AssignRow::State::skipNotAutomatable;
                    else if (st == "deferred")        r.state = AssignRow::State::skipDeferred;
                    else break;
                    r.resolvedIndex   = (int) rv.getProperty ("resolved_index", -1);
                    r.proposalMismatch = (bool) rv.getProperty ("proposal_mismatch", false);
                    r.corroboration   = rv.getProperty ("corroboration", "").toString();
                    r.mode            = rv.getProperty ("mode", "").toString();
                    r.trust           = rv.getProperty ("trust", "").toString();
                    r.skipReason      = rv.getProperty ("skip_reason", "").toString();
                    r.resolvedAt      = rv.getProperty ("resolved_at", "").toString();
                    r.sweep.anchorsReversed = (bool) rv.getProperty ("anchors_reversed", false);
                    r.sweep.method    = rv.getProperty ("sweep_method", "").toString();
                    r.sweep.identityDisplay = (bool) rv.getProperty ("identity_display", false);
                    r.sweep.ok = false;
                    r.sweep.anchors.clear();
                    if (auto* an = rv.getProperty ("anchors", juce::var()).getArray())
                        for (auto& pv : *an)
                            if (auto* p = pv.getArray(); p != nullptr && p->size() >= 2)
                            {
                                juce::Array<float> a;
                                a.add ((float) (double) (*p)[0]);
                                a.add ((float) (double) (*p)[1]);
                                r.sweep.anchors.add (a);
                            }
                    r.sweep.ok = r.sweep.anchors.size() >= 2;
                    break;
                }
            }
        };
        restore (rows, v.getProperty ("rows", juce::var()));
        restore (ignoreRows, v.getProperty ("ignore_rows", juce::var()));
        say ("Restored assignment session from " + f.getFileName());
    }

    void updateProgress()
    {
        int done = 0;
        for (const auto& r : rows) done += r.isResolved();
        const int total = rows.size();
        const auto secs = (int) ((juce::Time::getMillisecondCounter() - startedAt) / 1000);
        juce::String t;
        t << done << "/" << total << " rows";
        if (done > 0 && done < total)
        {
            const double per = (double) secs / (double) done;
            t << "  ~" << (int) std::ceil (per * (total - done)) << "s left (measured "
              << juce::String (per, 1) << "s/row)";
        }
        t << "  " << secs << "s elapsed" << (deepMode ? "  DEEP" : "  fast");
        progress.setText (t, juce::dontSendNotification);
    }

    void timerCallback() override { if (isVisible()) updateProgress(); }

    void say (const juce::String& s) { if (hooks.status) hooks.status (s); }

    juce::File root;
    juce::String fp, pluginId, category;
    EvidenceIndex evidence;
    juce::ListBox list;
    juce::Label progress;
    juce::Label question;
    juce::TextEditor reasonEntry;
    juce::Optional<AssignRow::State> pendingSkip;
    juce::String pendingAutoSkipReason;
    CaptureEngine::Result lastGesture;
    int selected = 0;
    int awaitingCaptureRow = -1;
    int typedRow = -1;
    juce::uint32 startedAt = 0, bulkArmedAt = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AssignPanel)
};

} // namespace ejmap
