/*
  EjmapAssignPanel.h

  M4 UI: a prompt-driven WIZARD, not a list with a caption (fourth usability
  finding). One row at a time, front and centre: the question in mix-engineer
  words, the proposal, and the answers as large labelled buttons wearing
  their shortcut keys. The full list survives as a progress sidebar below --
  where you look, not where you work. Resolution auto-advances. Submit is a
  REVIEW SCREEN with a summary of what will be written and a Submit button;
  that screen is also where refusals (duplicate index assignments, nothing
  confirmed) are shown instead of failing silently.

  Keys (signed map, all also on buttons): SPACE accept, W wiggle-verify,
  N/A/D the three skips (Shift+ custom reason; N over a live proposal is
  two-press, because "absent" against evidence is the falsehood class),
  M mode/position, T typed anchors, I bulk ignores (two-press), 1-9 pick a
  candidate, arrows navigate, ? evidence, S skip plugin, cmd+return review.

  Actions live in action*() methods; keys, buttons and the self-test all
  drive dispatchAction, so every input surface is one code path.
*/

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "EjmapAssignment.h"
#include "EjmapCapture.h"
#include "EjmapBands.h"

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
        std::function<void()>                          cancelArm;      // stand a pending arm down
        std::function<SweepOutcome (int)>              sweepIndex;     // synchronous, pump-paused
        std::function<void (int)>                      startTyped;     // T; completion via typedCompleted
        std::function<juce::String (int)>              paramName;
        std::function<int()>                           paramCount;
        std::function<int()>                           maskCount;      // per-arm watched = params - mask
        std::function<void (const juce::String&)>      status;         // one-line readout
        std::function<void (const juce::var&)>         writeRow;       // captures jsonl writer
        std::function<void (const juce::var&)>         writeMisclassified;
        std::function<void (const juce::var&)>         writeTier2Crumb;
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

        addAndMakeVisible (notice);
        notice.setFont (juce::FontOptions (12.0f));
        notice.setColour (juce::Label::textColourId, juce::Colour (0xffe0a060));

        addAndMakeVisible (categoryBox);
        for (int i = 0; i < DialSets::categories().size(); ++i)
            categoryBox.addItem (DialSets::categories()[i], i + 1);
        categoryBox.setTextWhenNothingSelected ("category?");
        categoryBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff161c26));
        categoryBox.onChange = [this]
        {
            const auto cat = categoryBox.getText();
            if (cat.isNotEmpty()) pickCategory (cat);
            grabKeyboardFocus();
        };

        addAndMakeVisible (promptTitle);
        promptTitle.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        promptTitle.setColour (juce::Label::textColourId, juce::Colour (0xffe8e0b0));

        for (auto* b : { &prevBtn, &nextBtn, &evidBtn, &bulkBtn, &skipPluginBtn, &reviewBtn })
            addAndMakeVisible (b);
        prevBtn.setButtonText ("< prev");            prevBtn.onClick = [this] { dispatchAction ("prev"); grabKeyboardFocus(); };
        nextBtn.setButtonText ("> next");            nextBtn.onClick = [this] { dispatchAction ("next"); grabKeyboardFocus(); };
        evidBtn.setButtonText ("? evidence");        evidBtn.onClick = [this] { dispatchAction ("evidence"); grabKeyboardFocus(); };
        bulkBtn.setButtonText ("I bulk ignores");    bulkBtn.onClick = [this] { dispatchAction ("bulk"); grabKeyboardFocus(); };
        skipPluginBtn.setButtonText ("S skip plugin"); skipPluginBtn.onClick = [this] { dispatchAction ("skipplugin"); grabKeyboardFocus(); };
        reviewBtn.setButtonText ("Review & submit (cmd-return)");
        reviewBtn.onClick = [this] { dispatchAction ("submit"); grabKeyboardFocus(); };

        addChildComponent (summaryText);
        summaryText.setMultiLine (true);
        summaryText.setReadOnly (true);
        summaryText.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff161c26));
        summaryText.setColour (juce::TextEditor::textColourId, juce::Colour (0xff9fd8e0));
        addChildComponent (submitBtn);
        submitBtn.setButtonText ("SUBMIT MAP");
        submitBtn.onClick = [this] { confirmSubmitFromSummary(); };
        addChildComponent (backBtn);
        backBtn.setButtonText ("< back to rows");
        backBtn.onClick = [this] { closeSummary(); grabKeyboardFocus(); };

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
                const EvidenceIndex& evidenceIn,
                const juce::String& categoryOverride = {})
    {
        root = rootIn; fp = fpIn; pluginId = pluginIdIn;
        evidence = evidenceIn;
        beginProposals = proposals;
        startedAt = juce::Time::getMillisecondCounter();
        rows.clear();
        ignoreRows.clear();
        bulkArmedAt = 0;

        // THE CATEGORY IS CHOSEN, NEVER DEFAULTED. A silent default built a
        // compressor checklist on AMEK EQ 200 -- nine wrong rows and no bands
        // flow on the plugin the M5 gate is built around. With a classifier
        // verdict the verdict chooses (visible, changeable); with an explicit
        // override the caller chooses; with NEITHER, the human is ASKED
        // before any row exists.
        if (categoryOverride.isNotEmpty())      category = categoryOverride;
        else if (proposals.present)             category = proposals.category;
        else
        {
            awaitingCategory = true;
            category = {};
            syncCategoryBox();
            selected = 0;
            list.updateContent();
            updateProgress();
            updateQuestion();
            say ("No classifier verdict for this fp: choose the category first. "
                 "It decides the dial set, and on an EQ it is what routes you into the bands flow.");
            return;
        }
        buildRows (proposals);
    }

    void buildRows (const ProposalSet& proposals)
    {
        awaitingCategory = false;

        // Category changes never discard work: a confirmed row is evidence
        // about the PLUGIN (captured index, sweep anchors), none of it
        // category-dependent. Resolved rows survive; only the unresolved
        // checklist rebuilds.
        juce::Array<AssignRow> preserved;
        for (const auto& r : rows)
            if (r.isResolved()) preserved.add (r);
        rows.clear();
        ignoreRows.clear();

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

        // 3. Dial-set semantics with no proposal. For eq, the band-class
        //    semantics (freq_hz/gain_db/q) fold into ONE bands-flow row: a
        //    flat freq_hz on a banded EQ is the bx_digital failure shape, so
        //    the wizard never offers to build one.
        const bool banded = category.trim().toLowerCase() == "eq";
        if (banded)
        {
            AssignRow r;
            r.semantic = "bands";
            r.kind = "bands";
            r.proposalSource = "none";
            rows.add (r);
        }
        for (const auto& s : DialSets::forCategory (category))
        {
            if (coveredSemantics.contains (s)) continue;
            if (banded && (s == "freq_hz" || s == "gain_db" || s == "q")) continue;
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

        // Merge the survivors: same-semantic fresh rows are REPLACED by the
        // resolved original; resolved rows outside the new checklist (input_db
        // confirmed under compressor, category corrected to eq) are appended
        // -- confirmed extras, shipped in the map, never re-done.
        for (auto& pr : preserved)
        {
            bool merged = false;
            for (auto& r : rows)
                if (r.semantic == pr.semantic && pr.semantic.isNotEmpty())
                { r = pr; merged = true; break; }
            if (! merged)
                rows.add (pr);
        }

        sortRows();
        restoreSession();
        syncCategoryBox();
        selected = firstUnresolved();
        list.selectRow (selected);
        list.updateContent();
        updateProgress();
        updateQuestion();
        say ("Assign (" + category + "): " + juce::String (rows.size()) + " rows, "
               + juce::String (ignoreRows.size()) + " classifier ignores. "
               + (proposals.present ? "Proposals loaded." : "No proposals: dial-set rows."));
    }

    void pickCategory (const juce::String& cat)
    {
        if (awaitingCategory)
        {
            category = cat;
            buildRows (beginProposals);
            return;
        }
        if (cat == category)
            return;

        // Mid-flow change: rebuild the checklist around the surviving work,
        // and record the correction as evidence.
        int kept = 0;
        for (const auto& r : rows) kept += r.isResolved();
        auto* o = new juce::DynamicObject();
        o->setProperty ("kind", "category_changed");
        o->setProperty ("from", category);
        o->setProperty ("to", cat);
        o->setProperty ("resolved_rows_kept", kept);
        if (hooks.writeRow) hooks.writeRow (juce::var (o));

        const auto from = category;
        category = cat;
        buildRows (beginProposals);
        say ("Category " + from + " -> " + cat + ": " + juce::String (kept)
               + " resolved row(s) kept, checklist rebuilt.");
    }

    /** The state, in words. Paint, the text mirror and the self-test all use
        this: what a human sees on screen and what the transcript prints are
        the same string by construction.
    */
    juce::String stateWord (AssignRow& r)
    {
        switch (r.state)
        {
            case AssignRow::State::confirmed:
                return r.kind == "bands"
                         ? "BANDS: " + r.skipReason
                         : "CONFIRMED [" + juce::String (r.resolvedIndex) + "] " + r.trust;
            case AssignRow::State::modeMaterial:      return "MODE/POS recorded";
            case AssignRow::State::skipNotPresent:    return "ABSENT";
            case AssignRow::State::skipNotAutomatable:return "NO PARAM";
            case AssignRow::State::skipDeferred:      return "LATER";
            case AssignRow::State::armed:             return "ARMED - touch the control";
            case AssignRow::State::captured:
                return r.conflictWith.isNotEmpty()
                         ? "INDEX CONFLICT with " + r.conflictWith + " - decide in the card"
                         : "captured...";
            case AssignRow::State::swept:
                return r.sweep.nonNumeric ? "needs M (labelled switch)"
                     : r.sweep.flat       ? "needs T (text liar)"
                                          : "swept, not confirmable";
            case AssignRow::State::proposed:          break;
        }
        return {};
    }

    /** The whole panel as text: current-row marker, every row's label and
        state word, the question strip, the last status line. The self-test
        prints this after every action so the loop can be judged from the
        transcript alone.
    */
    juce::String textRender()
    {
        juce::String t;
        t << progress.getText() << "\n";
        if (summaryShowing)
        {
            t << "== REVIEW SCREEN ==\n" << summaryText.getText()
              << "[buttons: SUBMIT MAP" << (submitBtn.isEnabled() ? "" : " (disabled)")
              << " | back]\nlast: " << lastStatus << "\n";
            return t;
        }
        t << "PROMPT: " << promptTitle.getText() << "\n";
        t << "   " << question.getText().replace ("\n", "\n   ") << "\n";
        t << "ANSWERS: ";
        for (auto* b : answerButtons)
            t << "[" << b->getButtonText() << (b->isEnabled() ? "" : " (greyed)") << "] ";
        t << "\n";
        for (int i = 0; i < rowCount(); ++i)
        {
            auto& r = rowAt (i);
            t << (i == selected ? " > " : "   ");
            t << displayLabel (r);
            if (r.kind != "ignore" && r.kind != "unsure" && r.proposedIndex >= 0
                 && r.state == AssignRow::State::proposed)
                t << " <- [" << r.proposedIndex << "] " << r.proposedName;
            const auto w = stateWord (r);
            if (w.isNotEmpty()) t << "   | " << w;
            else if (r.proposedIndex < 0 && ! r.isResolved() && r.kind != "ignore" && r.kind != "unsure")
                t << "   | unmapped (N if absent)";
            t << "\n";
        }
        t << "notice (in card): " << lastStatus << "\n";
        return t;
    }

    int rowCount() const { return rows.size() + ignoreRows.size(); }
    AssignRow& rowAt (int i) { return i < rows.size() ? rows.getReference (i)
                                                      : ignoreRows.getReference (i - rows.size()); }
    int selectedRow() const { return selected; }
    juce::String currentQuestionText() const { return question.getText(); }
    void selectRow (int i) { selected = juce::jlimit (0, juce::jmax (0, rowCount() - 1), i);
                             list.selectRow (selected); list.scrollToEnsureRowIsOnscreen (selected);
                             list.updateContent(); updateQuestion(); }

    //==========================================================================
    /** ONE validity function for every surface. The question strip, the key
        handler, the legend's highlighting and the legend's clicks all ask
        this, so they cannot disagree about what a key does on this row.
    */
    bool keyValid (const juce::String& id)
    {
        if (rowCount() == 0) return id == "skipplugin";
        auto& r = rowAt (selected);

        if (id == "space" && r.kind == "bands" && ! r.isResolved())
            return true;                               // begins the band flow
        if (id == "space" && r.conflictWith.isNotEmpty() && ! r.isResolved() && r.sweep.ok)
            return true;                               // the insist on a shared control
        if (id == "space")
            return ! deepMode && ! r.isResolved() && r.semantic.isNotEmpty()
                && r.semantic != "mode"                    // mode never confirms via anchors
                && ! r.sweep.nonNumeric                    // a labelled switch can never confirm
                && r.proposedIndex >= 0
                && evidence.corroborationFor (r.proposedIndex, r.proposedName).isNotEmpty();
        if (id == "wiggle")      return r.kind != "ignore";
        if (id == "notpresent" || id == "noparam" || id == "defer")
            return r.state != AssignRow::State::confirmed;
        if (id == "typed")       return r.resolvedIndex >= 0 || r.proposedIndex >= 0;
        if (id == "modematerial")
            return ! r.isResolved()
                && ( (! r.sweep.points.isEmpty() && r.sweep.nonNumeric)
                   || (r.semantic == "mode" && (r.proposedIndex >= 0 || r.resolvedIndex >= 0)) );
        if (id == "bulk")
        {
            for (const auto& ir : ignoreRows) if (! ir.isResolved()) return true;
            return false;
        }
        if (id == "pick")        return awaitingCaptureRow >= 0 && lastGesture.indices.size() > 1;
        if (id == "submit")      return true;   // always opens the REVIEW screen,
                                                // which shows any refusal in words
        return true;   // evidence, move, skipplugin
    }

    /** Every input surface lands here: keys, legend clicks, self-test. An
        INVALID action refuses OUT LOUD with the reason -- half the confusion
        in the third stopwatch run was not knowing whether a key was ignored
        or had worked invisibly.
    */
    void dispatchAction (const juce::String& id, bool shift = false)
    {
        if (awaitingCategory)
        {
            say ("Choose the category first: buttons, digits, or the box above.");
            return;
        }

        if (summaryShowing)
        {
            if (id == "submit") { confirmSubmitFromSummary(); return; }
            if (id == "prev")   { closeSummary(); return; }
            // Any other action returns to the rows first, then applies.
            closeSummary();
        }

        if (id == "submit") { openSummary(); return; }

        // The bands flow owns its keys while active.
        if (bandStep == BandStep::capFreq1 || bandStep == BandStep::capGain1
             || bandStep == BandStep::capQ1 || bandStep == BandStep::capFreqLast)
        {
            if (id == "wiggle")     { actionBandRearm(); return; }          // R and W both re-arm
            if (id == "notpresent" && bandStep == BandStep::capQ1)
            {
                say ("No Q on this band, recorded. " );
                bandStep = BandStep::capFreqLast;
                armBandCard();
                return;
            }
            if (id == "defer")
            {
                if (hooks.cancelArm) hooks.cancelArm();
                awaitingCaptureRow = -1;
                bandStep = BandStep::none;
                rowAt (bandsRowIndex).state = AssignRow::State::proposed;
                say ("Bands left for later; nothing recorded.");
                updateQuestion(); list.updateContent();
                return;
            }
            say ("Band card is waiting for a touch. R re-arms, D leaves bands for later.");
            return;
        }
        if (bandStep == BandStep::table)
        {
            if (id == "space")      { actionBandTableAccept(); return; }
            if (id == "prev")       { bandCursor = juce::jmax (0, bandCursor - 1); showBandTable(); return; }
            if (id == "next")       { bandCursor = juce::jmin (bandPlan.bands.size() - 1, bandCursor + 1); showBandTable(); return; }
            if (id == "wiggle")
            {
                if (juce::isPositiveAndBelow (bandCursor, bandPlan.bands.size()))
                {
                    bandWiggleTarget = bandCursor;
                    say ("Touch band " + bandPlan.bands.getReference (bandCursor).label
                           + "'s FREQUENCY control");
                    awaitingCaptureRow = bandsRowIndex;
                    if (hooks.armForRow) hooks.armForRow();
                }
                return;
            }
            if (id == "notpresent")
            {
                if (juce::isPositiveAndBelow (bandCursor, bandPlan.bands.size()))
                {
                    auto label = bandPlan.bands.getReference (bandCursor).label;
                    bandPlan.bands.remove (bandCursor);
                    bandCursor = juce::jmax (0, bandCursor - 1);
                    say ("Band " + label + " dropped (recorded).");
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("kind", "band_dropped");
                    o->setProperty ("band", label);
                    if (hooks.writeRow) hooks.writeRow (juce::var (o));
                    showBandTable();
                }
                return;
            }
            if (id == "defer")
            {
                bandStep = BandStep::none;
                summaryText.setVisible (false);
                rowAt (bandsRowIndex).state = AssignRow::State::proposed;
                say ("Band table left for later; nothing accepted.");
                updateQuestion(); list.updateContent(); resized();
                return;
            }
            say ("Band table: SPACE accepts, arrows pick, W wiggles, N drops, D leaves.");
            return;
        }

        if (! keyValid (id))
        {
            say ("KEY REFUSED (" + keyCapFor (id) + "): " + refusalFor (id));
            return;
        }
        if (id == "space")           actionSpace();
        else if (id == "wiggle")     actionWiggle();
        else if (id == "notpresent") shift ? beginCustomReason (AssignRow::State::skipNotPresent)
                                           : actionSkip (AssignRow::State::skipNotPresent);
        else if (id == "noparam")    shift ? beginCustomReason (AssignRow::State::skipNotAutomatable)
                                           : actionSkip (AssignRow::State::skipNotAutomatable);
        else if (id == "defer")      shift ? beginCustomReason (AssignRow::State::skipDeferred)
                                           : actionSkip (AssignRow::State::skipDeferred);
        else if (id == "typed")      actionTyped();
        else if (id == "modematerial") actionModeMaterial();
        else if (id == "bulk")       actionBulkIgnores();
        else if (id == "evidence")   actionEvidence();
        else if (id == "skipplugin") actionSkipPlugin();
        else if (id == "prev")       selectRow (selected - 1);
        else if (id == "next")       selectRow (selected + 1);
    }

    juce::String keyCapFor (const juce::String& id) const
    {
        if (id == "space") return "SPACE";
        if (id == "wiggle") return "W";
        if (id == "notpresent") return "N";
        if (id == "noparam") return "A";
        if (id == "defer") return "D";
        if (id == "typed") return "T";
        if (id == "modematerial") return "M";
        if (id == "bulk") return "I";
        if (id == "submit") return "cmd-return";
        return id;
    }

    juce::String refusalFor (const juce::String& id)
    {
        if (rowCount() == 0) return "no rows";
        auto& r = rowAt (selected);

        // A resolved row's refusals must READ as resolved, not as a failure
        // of the key: "M needs a labelled switch" on an already-skipped row
        // pointed the human at the wrong problem.
        if (r.isResolved() && (id == "space" || id == "modematerial" || id == "typed"))
            return "row already resolved (" + r.stateString() + "); W re-opens it";
        if (id == "space")
        {
            if (deepMode)               return "Deep mode: W to verify by touching";
            if (r.isResolved())         return "row already " + r.stateString();
            if (r.semantic == "mode")   return "mode never confirms via anchors: M records it";
            if (r.sweep.nonNumeric)     return "labelled switch, cannot confirm as a value: M records it";
            if (r.proposedIndex < 0)    return "no index proposed: W to point at the control";
            return "no evidence on disk for this index: W to verify";
        }
        if (id == "wiggle")             return "ignore rows are skipped, not captured";
        if (id == "notpresent" || id == "noparam" || id == "defer")
                                        return "row already confirmed: arrow away";
        if (id == "typed")              return "no index to type against: W first";
        if (id == "modematerial")       return "M needs a labelled switch (swept nonNumeric) or a mode row with an index";
        if (id == "bulk")               return "no unresolved ignore rows";
        if (id == "submit")             return "nothing confirmed yet";
        return "not available here";
    }

    //==========================================================================
    // Actions. Keys call these; the self-test calls these.

    /** SPACE. Fast lane only, and only on corroborated proposals. On a row
        held at an index conflict, SPACE is the INSIST. On the bands row it
        begins the band flow.
    */
    void actionSpace()
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);

        if (r.kind == "bands" && ! r.isResolved())
        {
            actionBandsBegin();
            return;
        }

        if (r.conflictWith.isNotEmpty() && ! r.isResolved() && r.sweep.ok)
        {
            r.state = AssignRow::State::confirmed;
            r.sharedInsisted = true;
            r.trust = "human-verified";
            r.mode = deepMode ? "deep" : "fast";
            r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
            recordResolution (r, "shared_index_insist");
            supersedeSiblings (r);
            say ("INSISTED: [" + juce::String (r.resolvedIndex) + "] serves both "
                   + r.conflictWith + " and " + r.semantic + ", recorded as a shared control.");
            r.conflictWith = {};
            advance();
            persistSession(); list.updateContent(); updateProgress();
            return;
        }

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
            const auto holder = confirmedHolderOf (r.proposedIndex, &r);
            if (holder.isNotEmpty())
            {
                r.state = AssignRow::State::captured;
                r.conflictWith = holder;
                say ("[" + juce::String (r.proposedIndex) + "] " + r.proposedName
                       + " is already assigned to " + holder + ". Decide in the card.");
                persistSession(); list.updateContent(); updateQuestion();
                return;
            }
        }

        // A mode semantic never confirms through the anchor path: the map's
        // mode entries carry labels, and an anchored mode entry is a broken
        // map. The M outcome is its resolution.
        if (r.semantic == "mode")
        {
            r.state = AssignRow::State::swept;
            say ("Mode switch swept: M records it as mode/position material (anchors "
                 "cannot express a mode entry).");
            persistSession(); list.updateContent(); updateQuestion();
            return;
        }

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
        r.conflictWith = {};
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
        if (bandStep == BandStep::capFreq1 || bandStep == BandStep::capGain1
             || bandStep == BandStep::capQ1 || bandStep == BandStep::capFreqLast)
        { bandCaptureArrived (res); return; }

        if (bandStep == BandStep::table && bandWiggleTarget >= 0)
        {
            const int idx = res.primaryIndex >= 0 ? res.primaryIndex
                            : (res.indices.size() == 1 ? res.indices[0] : -1);
            if (idx >= 0 && juce::isPositiveAndBelow (bandWiggleTarget, bandPlan.bands.size()))
            {
                auto& b = bandPlan.bands.getReference (bandWiggleTarget);
                for (auto& m : b.members)
                    if (m.semantic == "freq_hz")
                    {
                        if (m.index != idx)
                        {
                            say ("Band " + b.label + " freq is [" + juce::String (idx) + "] "
                                   + (hooks.paramName ? hooks.paramName (idx) : juce::String())
                                   + ", not [" + juce::String (m.index) + "] as the name pattern said "
                                     "(recorded).");
                            auto* o = new juce::DynamicObject();
                            o->setProperty ("kind", "band_repointed");
                            o->setProperty ("band", b.label);
                            o->setProperty ("was", m.index);
                            o->setProperty ("now", idx);
                            if (hooks.writeRow) hooks.writeRow (juce::var (o));
                            m.index = idx;
                            m.name = hooks.paramName ? hooks.paramName (idx) : juce::String();
                            m.sweep = hooks.sweepIndex ? hooks.sweepIndex (idx) : SweepOutcome();
                        }
                        else
                            say ("Band " + b.label + " verified by touch.");
                        m.captured = true;
                        b.flag = {};
                        b.strideAgrees = true;
                        b.strideUnverified = false;
                    }
                BandInference::ArmRecord arm;
                arm.watched = (hooks.paramCount ? hooks.paramCount() : 0)
                            - (hooks.maskCount ? hooks.maskCount() : 0);
                arm.moved = res.indices;
                bandPlan.arms.add (arm);
            }
            bandWiggleTarget = -1;
            showBandTable();
            return;
        }

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

    /** The three skips. Canned reason unless customReason is non-empty.

        N over a live classifier proposal takes TWO presses. mode - Type
        landed ABSENT in a submitted map while index 6 sat proposed and the
        switch sat on the GUI: one keystroke wrote a falsehood the evidence
        contradicted. The insist is cheap (press N again); the accident is
        not.
    */
    void actionSkip (AssignRow::State outcome, const juce::String& customReason = {})
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);
        if (r.state == AssignRow::State::confirmed) { say ("Row already confirmed; navigate elsewhere."); return; }

        if (outcome == AssignRow::State::skipNotPresent
             && r.proposalSource == "classifier" && r.proposedIndex >= 0
             && r.kind != "ignore" && customReason.isEmpty())
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (insistRow != selected || (int) (now - insistAt) > 6000)
            {
                insistRow = selected; insistAt = now;
                say ("N claims NO such control exists, but the classifier proposed ["
                       + juce::String (r.proposedIndex) + "] " + r.proposedName
                       + (r.semantic == "mode" ? ". If it is a switch, M records it truthfully."
                                               : ".")
                       + " Press N again to insist.");
                return;
            }
            insistRow = -1;
        }

        // Skipping an ARMED row must also stand the capture down, or the
        // engine later delivers into a row that moved on.
        if (r.state == AssignRow::State::armed)
        {
            if (hooks.cancelArm) hooks.cancelArm();
            awaitingCaptureRow = -1;
        }

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

        r.conflictWith = {};
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

    /** M: the outcome the evidence demands when a sweep proves the control
        exists but is discrete. Records the captured index, the sweep result
        and the labels; resolves the row as its own thing; and drops the
        Tier 2 breadcrumb where M6's named controls will pick it up -- a
        three-position knee switch belongs there, not in an anchor table and
        not in a skip that claims it does not exist.
    */
    void actionModeMaterial()
    {
        if (rowCount() == 0) return;
        auto& r = rowAt (selected);
        if (! keyValid ("modematerial"))
        { say ("M needs a labelled switch or a mode row with an index."); return; }

        const int idx = r.resolvedIndex >= 0 ? r.resolvedIndex : r.proposedIndex;

        // A mode row is M without a wiggle: six of fifteen rows on the third
        // stopwatch run were this case. The sweep runs here, automatically,
        // because the labels are the record and the plugin supplies them.
        const bool wiggled = r.resolvedIndex >= 0;
        if (r.sweep.points.isEmpty() && hooks.sweepIndex)
        {
            r.sweep = hooks.sweepIndex (idx);
            say ("Auto-swept [" + juce::String (idx) + "] for its labels: " + r.sweep.reason);
        }

        juce::StringArray labels;
        for (const auto& pt : r.sweep.points)
            labels.addIfNotAlreadyThere (pt.t);

        r.state = AssignRow::State::modeMaterial;
        r.resolvedIndex = idx;
        r.trust = wiggled ? "human-verified" : "llm-classified";   // trust reflects the hand
        r.mode = deepMode ? "deep" : "fast";
        r.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
        r.skipReason = "exists as a " + juce::String (labels.size()) + "-position control ["
                     + juce::String (idx) + "] "
                     + (hooks.paramName ? hooks.paramName (idx) : juce::String())
                     + " (" + labels.joinIntoString (" / ").substring (0, 120)
                     + "); Tier 2 breadcrumb written";
        recordResolution (r, "mode_material");

        auto* o = new juce::DynamicObject();
        o->setProperty ("at", r.resolvedAt);
        o->setProperty ("fp", fp);
        o->setProperty ("plugin_id", pluginId);
        o->setProperty ("semantic_hint", r.semantic);
        o->setProperty ("index", idx);
        o->setProperty ("param_name", hooks.paramName ? hooks.paramName (idx) : juce::String());
        o->setProperty ("positions", labels.size());
        juce::Array<juce::var> lv;
        juce::Array<juce::var> pv;
        for (const auto& pt : r.sweep.points)
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("n", pt.n);
          pp->setProperty ("t", pt.t); pv.add (juce::var (pp)); }
        for (const auto& l : labels) lv.add (l);
        o->setProperty ("labels", juce::var (lv));
        o->setProperty ("points", juce::var (pv));
        o->setProperty ("sweep_method", r.sweep.method);
        o->setProperty ("reason", r.sweep.reason);
        if (hooks.writeTier2Crumb) hooks.writeTier2Crumb (juce::var (o));

        // Mode rows do NOT supersede each other: they are not competing for
        // one map slot (mode_material lands in skips and breadcrumbs, plural
        // welcome). Six switches on one plugin are six findings, and the
        // first transcript review caught this auto-deferring five of them.
        if (r.semantic != "mode")
            supersedeSiblings (r);
        persistSession();
        say ("MODE/POSITION: " + displayLabel (r) + " -> " + r.skipReason);
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
            selectRow (typedRow);
            advance();
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

    //==========================================================================
    // The bands flow (M5): guided captures -> inference -> table -> groups.

    enum class BandStep { none, capFreq1, capGain1, capQ1, capFreqLast, table };
    BandStep bandStep = BandStep::none;
    juce::Array<BandInference::Member> bandCaptured;
    int bandSecondFreqIdx = -1;
    BandInference bandPlan;
    CaptureEngine::Result bandGesturePending;
    juce::Array<GroupSpec> acceptedGroups;
    int bandCursor = 0;
    int bandWiggleTarget = -1;         // table-phase wiggle: which band ordinal
    int bandsRowIndex = -1;

    const juce::Array<GroupSpec>& groupsForSubmit() const { return acceptedGroups; }

    void actionBandsBegin()
    {
        bandsRowIndex = selected;
        bandCaptured.clear();
        bandSecondFreqIdx = -1;
        acceptedGroups.clear();
        bandStep = BandStep::capFreq1;
        armBandCard();
    }

    juce::String bandCardPrompt() const
    {
        switch (bandStep)
        {
            case BandStep::capFreq1:    return "Touch BAND 1's FREQUENCY control";
            case BandStep::capGain1:    return "Touch BAND 1's GAIN control";
            case BandStep::capQ1:       return "Touch BAND 1's Q control";
            case BandStep::capFreqLast: return "Touch the HIGHEST band's FREQUENCY";
            default: break;
        }
        return {};
    }

    void armBandCard()
    {
        rowAt (bandsRowIndex).state = AssignRow::State::armed;
        say (bandCardPrompt() + " - the card is LIVE (R discards a wrong grab and re-arms)");
        if (hooks.armForRow) hooks.armForRow();
        awaitingCaptureRow = bandsRowIndex;
        list.updateContent();
        updateQuestion();
    }

    /** R on a live band card: the grab was a brush or the wrong control.
        Discard whatever the engine holds and arm again. (Amendment 2: every
        other card waits for you; a live card needs a way to say "that wasn't
        me".)
    */
    void actionBandRearm()
    {
        if (hooks.cancelArm) hooks.cancelArm();
        say ("Discarded. " + bandCardPrompt());
        if (hooks.armForRow) hooks.armForRow();
    }

    /** Band-flow capture: collect the member, record the ARM (watched set and
        moved set, this arm only), advance the card.
    */
    void bandCaptureArrived (const CaptureEngine::Result& res)
    {
        const int idx = res.primaryIndex >= 0 ? res.primaryIndex
                        : (res.indices.size() == 1 ? res.indices[0] : -1);
        if (idx < 0)
        {
            if (res.indices.size() > 1)
            {
                // A LOCKSTEP PAIR on a band card: AMEK's Param Link mirrors
                // LF Freq 1 onto LF Freq 2, the first confirmed linked pair
                // this project has seen live. The plugin cannot say which is
                // the touched one, so the human picks, same as the main loop;
                // the other stays in the arm record as co-moved evidence.
                bandGesturePending = res;
                juce::String cands;
                for (int i = 0; i < juce::jmin (9, res.indices.size()); ++i)
                    cands << (i ? "  " : "") << juce::String (i + 1) << ":" << res.names[i];
                say ("Two controls moved in lockstep (a linked pair). Pick the one you touched: "
                       + cands);
                rebuildBandAnswers();
                updateQuestion();
                return;
            }
            say ("Capture unusable (" + res.reason + ") - re-arming. R also re-arms.");
            if (hooks.armForRow) hooks.armForRow();
            return;
        }
        processBandIndex (idx, res);
    }

    void bandPickCandidate (int oneBased)
    {
        if (bandGesturePending.indices.isEmpty()
             || ! juce::isPositiveAndBelow (oneBased - 1, bandGesturePending.indices.size()))
            return;
        auto res = bandGesturePending;
        bandGesturePending = {};
        processBandIndex (res.indices[oneBased - 1], res);
    }

    /** Test seam: pick a pending lockstep candidate by its parameter index,
        which is what a human does by reading the names.
    */
    bool bandPickByParamIndex (int paramIdx)
    {
        for (int i = 0; i < bandGesturePending.indices.size(); ++i)
            if (bandGesturePending.indices[i] == paramIdx)
            { bandPickCandidate (i + 1); return true; }
        return false;
    }

    bool bandPickPending() const { return ! bandGesturePending.indices.isEmpty(); }

    void processBandIndex (int idx, const CaptureEngine::Result& res)
    {

        BandInference::ArmRecord arm;
        arm.watched = (hooks.paramCount ? hooks.paramCount() : 0)
                    - (hooks.maskCount ? hooks.maskCount() : 0);
        arm.moved = res.indices;
        bandPlan.arms.add (arm);

        auto nameOf = [this] (int i) { return hooks.paramName ? hooks.paramName (i) : juce::String(); };

        // RESIDUE GUARD. The card auto-arms the instant the previous capture
        // lands, and a hand can still be settling on the previous control --
        // its last jitter would be captured as the NEXT member. An index this
        // flow already captured is residue, not an answer: say so and re-arm.
        for (const auto& cm : bandCaptured)
            if (cm.index == idx)
            {
                bandPlan.arms.removeLast();      // that arm was residue too
                say ("That was [" + juce::String (idx) + "] " + cm.name
                       + " again (the previous control settling). Re-armed: "
                       + bandCardPrompt());
                if (hooks.armForRow) hooks.armForRow();
                return;
            }

        if (bandStep == BandStep::capFreq1 || bandStep == BandStep::capGain1
             || bandStep == BandStep::capQ1)
        {
            BandInference::Member m;
            m.semantic = bandStep == BandStep::capFreq1 ? "freq_hz"
                       : bandStep == BandStep::capGain1 ? "gain_db" : "q";
            m.index = idx; m.name = nameOf (idx); m.captured = true;
            bandCaptured.add (m);
            say ("Captured " + m.semantic + " -> [" + juce::String (idx) + "] " + m.name);
            bandStep = bandStep == BandStep::capFreq1 ? BandStep::capGain1
                     : bandStep == BandStep::capGain1 ? BandStep::capQ1
                                                      : BandStep::capFreqLast;
            armBandCard();
            return;
        }

        if (bandStep == BandStep::capFreqLast)
        {
            bandSecondFreqIdx = idx;
            if (hooks.cancelArm) hooks.cancelArm();
            awaitingCaptureRow = -1;
            buildBandTable();
        }
    }

    void buildBandTable()
    {
        juce::StringArray names;
        const int n = hooks.paramCount ? hooks.paramCount() : 0;
        for (int i = 0; i < n; ++i) names.add (hooks.paramName (i));

        auto arms = bandPlan.arms;                       // keep the arm records
        bandPlan = BandInference::infer (bandCaptured, bandSecondFreqIdx, names);
        bandPlan.arms = arms;

        // Every member is swept: real anchors, real ranges, and a refusal
        // flags its band. Machine verification of a name hypothesis.
        for (auto& b : bandPlan.bands)
            for (auto& m : b.members)
            {
                m.sweep = hooks.sweepIndex ? hooks.sweepIndex (m.index) : SweepOutcome();
                if (! m.sweep.ok && b.flag.isEmpty())
                    b.flag = m.semantic + " sweep refused: " + m.sweep.reason.substring (0, 60);
            }

        bandStep = BandStep::table;
        bandCursor = 0;
        rowAt (bandsRowIndex).state = AssignRow::State::captured;
        showBandTable();
    }

    void showBandTable()
    {
        juce::String t;
        t << "BANDS (" << bandPlan.axis << " axis"
          << (bandPlan.family.isNotEmpty() ? ", family '" + bandPlan.family + "'" : juce::String())
          << ")  " << bandPlan.strideNote << "\n\n";
        for (int i = 0; i < bandPlan.bands.size(); ++i)
        {
            const auto& b = bandPlan.bands.getReference (i);
            t << (i == bandCursor ? " > " : "   ") << "band " << b.label << ":  ";
            for (const auto& m : b.members)
            {
                t << m.semantic << " [" << m.index << "]"
                  << (m.captured ? " CAPTURED" : m.sweep.ok ? "" : " !sweep") << "  ";
            }
            if (b.flag.isNotEmpty())            t << "  ! " << b.flag;
            else if (b.strideUnverified)        t << "  ~ stride unverified here";
            else if (b.strideAgrees)            t << "  ok";
            t << "\n";
        }

        // THE EXCLUSION FOOTER. The most important line in the module and the
        // easiest to make vacuous: when there are no imposters it SAYS SO with
        // the watched denominator, because an empty footer reads identically
        // to "nothing was watched".
        const auto excl = bandPlan.exclusionList ([this] {
            juce::StringArray ns;
            const int n = hooks.paramCount ? hooks.paramCount() : 0;
            for (int i = 0; i < n; ++i) ns.add (hooks.paramName (i));
            return ns; }());
        int watched = 0;
        for (const auto& a : bandPlan.arms) watched = juce::jmax (watched, a.watched);
        t << "\nWATCHED & UNMOVED during your " << bandPlan.arms.size()
          << " gestures, OUTSIDE the group:\n";
        if (excl.isEmpty())
            t << "   (none: among the " << watched << " watched parameters, every "
                 "band-look-alike name is a group member or moved with one)\n";
        else
            for (const auto& e : excl) t << "   " << e << "\n";
        if (! bandPlan.otherFamilies.isEmpty())
            t << "\nANOTHER band-shaped family exists: "
              << bandPlan.otherFamilies.joinIntoString (", ")
              << " - you will be asked after accepting.\n";

        summaryText.setText (t, juce::dontSendNotification);
        summaryText.setVisible (true);
        promptTitle.setText (juce::String (bandPlan.bands.size()) + " bands inferred from your "
                               + juce::String (bandCaptured.size() + 1) + " touches",
                             juce::dontSendNotification);
        question.setText ("SPACE accept clean bands - up/down pick a band - W wiggle it - "
                          "N drop it - D leave bands for later", juce::dontSendNotification);
        rebuildBandAnswers();
        resized();
    }

    void rebuildBandAnswers()
    {
        answerButtons.clear();
        auto add = [this] (const juce::String& id, const juce::String& text)
        {
            auto* b = answerButtons.add (new juce::TextButton (text));
            addAndMakeVisible (b);
            b->onClick = [this, id] { dispatchAction (id); grabKeyboardFocus(); };
        };

        if (bandPickPending())
        {
            for (int i = 0; i < juce::jmin (9, bandGesturePending.indices.size()); ++i)
            {
                const int oneBased = i + 1;
                auto* b = answerButtons.add (new juce::TextButton (
                    juce::String (oneBased) + " - " + bandGesturePending.names[i]));
                addAndMakeVisible (b);
                b->onClick = [this, oneBased] { bandPickCandidate (oneBased); grabKeyboardFocus(); };
            }
            resized(); return;
        }
        add ("space",  "SPACE - accept clean bands");
        add ("wiggle", "W - wiggle band " + (bandCursor < bandPlan.bands.size()
                          ? bandPlan.bands.getReference (bandCursor).label : juce::String()));
        add ("notpresent", "N - drop that band");
        add ("defer",  "D - later");
        resized();
    }

    void actionBandTableAccept()
    {
        acceptedGroups.addArray (bandPlan.toGroups());
        int flagged = 0;
        for (const auto& b : bandPlan.bands) flagged += b.flag.isNotEmpty();

        auto& row = rowAt (bandsRowIndex);
        row.state = AssignRow::State::confirmed;
        row.trust = "human-verified";
        row.mode = deepMode ? "deep" : "fast";
        row.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
        row.skipReason = juce::String (acceptedGroups.size()) + " band group(s) accepted"
                       + (flagged > 0 ? ", " + juce::String (flagged) + " flagged band(s) EXCLUDED" : "");
        recordResolution (row, "bands_accept");

        // The evidence row: axis, stride verdict, per-arm records, exclusions.
        auto* o = new juce::DynamicObject();
        o->setProperty ("kind", "bands_accepted");
        o->setProperty ("axis", bandPlan.axis);
        o->setProperty ("family", bandPlan.family);
        o->setProperty ("groups", acceptedGroups.size());
        o->setProperty ("stride_note", bandPlan.strideNote);
        juce::Array<juce::var> armsV;
        for (const auto& a : bandPlan.arms)
        {
            auto* av = new juce::DynamicObject();
            av->setProperty ("watched", a.watched);
            juce::Array<juce::var> mv; for (int i : a.moved) mv.add (i);
            av->setProperty ("moved", juce::var (mv));
            armsV.add (juce::var (av));
        }
        o->setProperty ("arms", juce::var (armsV));
        if (hooks.writeRow) hooks.writeRow (juce::var (o));

        summaryText.setVisible (false);
        bandStep = BandStep::none;
        say ("BANDS: " + row.skipReason);

        if (! bandPlan.otherFamilies.isEmpty())
        {
            promptTitle.setText ("Another band family: " + bandPlan.otherFamilies[0],
                                 juce::dontSendNotification);
            question.setText ("You touched '" + bandPlan.family + "'. The family '"
                                + bandPlan.otherFamilies[0] + "' also looks band-shaped.\n"
                                "W map it too (touch ITS band 1 freq) - N not bands - D later",
                              juce::dontSendNotification);
            bandStep = BandStep::capFreq1;      // W restarts the flow for it
            bandCaptured.clear();
            // The prompt waits: W arms, N/D resolve it via the normal keys on
            // this row, which is already confirmed for the first family.
            bandStep = BandStep::none;
            persistSession(); list.updateContent();
            return;
        }

        persistSession();
        advance();
        list.updateContent();
        updateProgress();
        updateQuestion();
    }

    /** The semantic already holding this index, if any. Asked at CAPTURE
        time; the review's duplicate check stays as defence in depth (it can
        still fire via a restored session).
    */
    juce::String confirmedHolderOf (int idx, const AssignRow* except)
    {
        for (auto& r : rows)
            if (&r != except && r.state == AssignRow::State::confirmed && r.resolvedIndex == idx)
                return r.semantic;
        return {};
    }

    /** Two confirmed semantics on one index is a defect the map must not
        carry: makeup (dB) and output (dB) both CONFIRMED [8] on API-2500 is
        how one wrong accept poisons two keys. Computed here, shown on the
        review screen, and submit refuses while any exist.
    */
    juce::StringArray duplicateConflicts()
    {
        juce::StringArray out;
        std::map<int, juce::StringArray> byIndex;
        for (auto& r : rows)
            if (r.state == AssignRow::State::confirmed && ! r.sharedInsisted)
                byIndex[r.resolvedIndex].add (r.semantic);
        for (auto& kv : byIndex)
            if (kv.second.size() > 1)
                out.add ("[" + juce::String (kv.first) + "] "
                           + (hooks.paramName ? hooks.paramName (kv.first) : juce::String())
                           + " claimed by: " + kv.second.joinIntoString (" AND "));
        return out;
    }

    bool isSummaryShowing() const { return summaryShowing; }
    bool isAwaitingCategory() const { return awaitingCategory; }
    juce::String progressText() const { return progress.getText(); }
    juce::String bandTableText() const { return summaryText.getText(); }
    BandStep currentBandStep() const { return bandStep; }
    bool isSubmitEnabled() const  { return submitBtn.isEnabled(); }

    void openSummary()
    {
        int confirmed = 0, modePos = 0, skips = 0, open = 0;
        for (auto& r : rows)
        {
            if (r.state == AssignRow::State::confirmed) ++confirmed;
            else if (r.state == AssignRow::State::modeMaterial) ++modePos;
            else if (r.isSkipped()) ++skips;
            else ++open;
        }
        int ignoresOpen = 0;
        for (auto& r : ignoreRows) ignoresOpen += ! r.isResolved();

        const auto conflicts = duplicateConflicts();

        juce::String t;
        t << "REVIEW: what submit will write\n\n";
        for (auto& r : rows)
            if (r.state == AssignRow::State::confirmed)
                t << "  " << displayLabel (r) << "  <- [" << r.resolvedIndex << "] "
                  << (hooks.paramName ? hooks.paramName (r.resolvedIndex) : juce::String())
                  << "  (" << r.trust << ", " << r.sweep.method << ")\n";
        if (! acceptedGroups.isEmpty())
            t << "\n" << acceptedGroups.size() << " band group(s) will be written "
                 "(a 250 Hz-class request can only land inside them)\n";
        t << "\n" << modePos << " mode/position finding(s), " << skips << " skip(s) with reasons, "
          << open << " unresolved row(s)" << (open > 0 ? " -> will be recorded as deferred" : "")
          << (ignoresOpen > 0 ? ", " + juce::String (ignoresOpen) + " unreviewed ignore(s)" : juce::String())
          << "\n";

        if (! conflicts.isEmpty())
        {
            t << "\nSUBMIT REFUSED - one index, two semantics:\n";
            for (const auto& c : conflicts) t << "  " << c << "\n";
            t << "One of each pair is wrong. Back, W re-captures the wrong one "
                 "(or D defers it), then review again.\n";
        }
        else if (confirmed == 0)
            t << "\nSUBMIT REFUSED - nothing confirmed yet.\n";
        else
            t << "\nReady: SUBMIT writes maps/" << fp.substring (0, 12) << "....json\n";

        summaryText.setText (t, juce::dontSendNotification);
        submitBtn.setEnabled (conflicts.isEmpty() && confirmed > 0);

        summaryShowing = true;
        summaryText.setVisible (true);
        submitBtn.setVisible (true);
        backBtn.setVisible (true);
        setWizardVisible (false);
        resized();
        say (conflicts.isEmpty() ? "Review open: SUBMIT writes the map, back returns to the rows."
                                 : "Review open: submit refused, " + conflicts[0]);
    }

    bool confirmSubmitFromSummary()
    {
        if (! summaryShowing) return false;
        if (! submitBtn.isEnabled())
        {
            say ("SUBMIT still refused: fix the conflicts listed above, or confirm at least one row.");
            return false;
        }
        closeSummary();
        actionSubmit();
        return true;
    }

    void closeSummary()
    {
        summaryShowing = false;
        summaryText.setVisible (false);
        submitBtn.setVisible (false);
        backBtn.setVisible (false);
        setWizardVisible (true);
        resized();
        updateQuestion();
    }

    void syncCategoryBox()
    {
        const int id = DialSets::categories().indexOf (category) + 1;
        categoryBox.setSelectedId (id, juce::dontSendNotification);
    }

    void setWizardVisible (bool v)
    {
        promptTitle.setVisible (v);
        categoryBox.setVisible (v);
        question.setVisible (v);
        notice.setVisible (v);
        list.setVisible (v);
        for (auto* b : answerButtons) b->setVisible (v);
        for (auto* b : { &prevBtn, &nextBtn, &evidBtn, &bulkBtn, &skipPluginBtn, &reviewBtn })
            b->setVisible (v);
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

        if (k == juce::KeyPress::spaceKey)                { dispatchAction ("space"); return true; }
        if (c == 'w' || c == 'W' || c == 'r' || c == 'R') { dispatchAction ("wiggle"); return true; }
        if (c == 'n' || c == 'N')                         { dispatchAction ("notpresent", shift); return true; }
        if (c == 'a' || c == 'A')                         { dispatchAction ("noparam", shift); return true; }
        if (c == 'd' || c == 'D')                         { dispatchAction ("defer", shift); return true; }
        if (c == 't' || c == 'T')                         { dispatchAction ("typed"); return true; }
        if (c == 'm' || c == 'M')                         { dispatchAction ("modematerial"); return true; }
        if (c == 'i' || c == 'I')                         { dispatchAction ("bulk"); return true; }
        if (c == '?')                                     { dispatchAction ("evidence"); return true; }
        if (c == 's' || c == 'S')                         { dispatchAction ("skipplugin"); return true; }
        if (c >= '1' && c <= '9')
        {
            if (awaitingCategory)
            {
                const auto cats = DialSets::categories();
                if (c - '1' < cats.size()) pickCategory (cats[c - '1']);
            }
            else if (bandPickPending()) bandPickCandidate (c - '0');
            else                        actionPickCandidate (c - '0');
            return true;
        }
        if (k == juce::KeyPress::leftKey  || k == juce::KeyPress::upKey)   { dispatchAction ("prev"); return true; }
        if (k == juce::KeyPress::rightKey || k == juce::KeyPress::downKey) { dispatchAction ("next"); return true; }
        if (k == juce::KeyPress::returnKey && k.getModifiers().isCommandDown()) { dispatchAction ("submit"); return true; }
        if (c >= 'a' && c <= 'z')
        { say ("KEY '" + juce::String::charToString (c) + "' is not mapped; the legend below is the map."); return true; }
        return false;
    }

    //==========================================================================
    void resized() override
    {
        auto r = getLocalBounds();
        {
            auto head = r.removeFromTop (20);
            categoryBox.setBounds (head.removeFromLeft (130));
            head.removeFromLeft (6);
            progress.setBounds (head);
        }

        if (summaryShowing)
        {
            auto foot = r.removeFromBottom (34);
            backBtn.setBounds (foot.removeFromLeft (140).reduced (2));
            submitBtn.setBounds (foot.reduced (2));
            summaryText.setBounds (r.reduced (2));
            return;
        }

        promptTitle.setBounds (r.removeFromTop (30));
        question.setBounds (r.removeFromTop (58));

        // Answer buttons: up to two rows of large targets, keys on their faces.
        const int btnRows = answerButtons.size() > 8 ? 3 : answerButtons.size() > 3 ? 2 : 1;
        auto btns = r.removeFromTop (btnRows * 34 + 4);
        const int perRow = juce::jmax (1, (answerButtons.size() + btnRows - 1) / btnRows);
        int i = 0;
        for (int rowN = 0; rowN < btnRows && i < answerButtons.size(); ++rowN)
        {
            auto rowArea = btns.removeFromTop (34);
            const int n = juce::jmin (perRow, answerButtons.size() - i);
            const int wEach = rowArea.getWidth() / juce::jmax (1, n);
            for (int k = 0; k < n; ++k, ++i)
                answerButtons[i]->setBounds (rowArea.removeFromLeft (wEach).reduced (2));
        }

        notice.setBounds (r.removeFromTop (18));
        auto strip = r.removeFromTop (26);
        const int gw = strip.getWidth() / 6;
        prevBtn.setBounds (strip.removeFromLeft (gw).reduced (1));
        nextBtn.setBounds (strip.removeFromLeft (gw).reduced (1));
        evidBtn.setBounds (strip.removeFromLeft (gw).reduced (1));
        bulkBtn.setBounds (strip.removeFromLeft (gw).reduced (1));
        skipPluginBtn.setBounds (strip.removeFromLeft (gw).reduced (1));
        reviewBtn.setBounds (strip.reduced (1));

        if (reasonEntry.isVisible())
            reasonEntry.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        list.setBounds (r);      // the progress sidebar: where you look
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
        if (unit.isNotEmpty() && unit != label) label << " (" << unit << ")";
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
        if (awaitingCategory)
        {
            question.setText ("No classifier verdict exists for this fp, so nothing may guess. "
                              "The category decides the dial set; on an EQ it routes into the "
                              "bands flow.\nPick with the buttons, the digits, or the box above.",
                              juce::dontSendNotification);
            promptTitle.setText (promptHeadline(), juce::dontSendNotification);
            rebuildAnswers();
            return;
        }
        if (rowCount() == 0) { question.setText ({}, juce::dontSendNotification); return; }
        auto& r = rowAt (selected);
        const auto label = displayLabel (r);
        juce::String q;

        if (r.kind == "bands" && bandStep != BandStep::none && bandStep != BandStep::table)
        {
            q << bandCardPrompt() << " - the card is LIVE.\n"
              << "R discard a wrong grab and re-arm"
              << (bandStep == BandStep::capQ1 ? " - N this band has no Q" : juce::String())
              << " - D leave bands for later";
        }
        else if (r.kind == "bands" && ! r.isResolved())
        {
            q << "The band controls (freq/gain/q) are mapped as GROUPS, so a 250 Hz "
                 "request can never land on a look-alike outside them.\n"
              << "SPACE begin: touch band 1's controls, then the highest band's frequency "
                 "(2 wiggles buy a verified pattern)";
        }
        else if (r.conflictWith.isNotEmpty() && ! r.isResolved())
        {
            const int ci = r.resolvedIndex;
            q << "[" << ci << "] '" << (hooks.paramName ? hooks.paramName (ci) : juce::String())
              << "' is already assigned to " << r.conflictWith << ".\n"
              << "Is it also the right control for " << label << "? "
              << "SPACE yes, shared control - W re-capture - N no such control - D later";
        }
        else if (r.state == AssignRow::State::modeMaterial)
        {
            q << label << ": recorded as mode/position material. " << r.skipReason;
        }
        else if (r.isResolved())
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
        else if (! r.sweep.points.isEmpty() && r.sweep.nonNumeric)
        {
            juce::StringArray labels;
            for (const auto& pt : r.sweep.points) labels.addIfNotAlreadyThere (pt.t);
            const int idx = r.resolvedIndex >= 0 ? r.resolvedIndex : r.proposedIndex;
            q << "The sweep PROVED [" << idx << "] exists with " << labels.size()
              << " positions (" << labels.joinIntoString (" / ").substring (0, 60)
              << ") - it is not a " << label << " value.\n"
              << "M record as mode/position material (Tier 2 breadcrumb) - D later. "
              << "N would be a falsehood: the control exists.";
        }
        else if (r.semantic == "mode" && (r.proposedIndex >= 0 || r.resolvedIndex >= 0))
        {
            const int mi = r.resolvedIndex >= 0 ? r.resolvedIndex : r.proposedIndex;
            q << "Mode switch [" << mi << "] '"
              << (hooks.paramName ? hooks.paramName (mi) : juce::String())
              << "'. Map-level mode entries arrive with Tier 2.\n"
              << "M record it with its labels (auto-sweeps, NO wiggle needed) - "
              << "W verify by touching - D later";
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
        promptTitle.setText (promptHeadline(), juce::dontSendNotification);
        rebuildAnswers();
    }

    juce::String promptHeadline()
    {
        if (awaitingCategory) return "What is this plugin?";
        if (rowCount() == 0) return "No rows";
        auto& r = rowAt (selected);
        if (r.isResolved())
            return displayLabel (r) + ": done (" + r.stateString() + ")";
        if (r.kind == "bands")
            return bandStep == BandStep::table ? "The band table"
                 : bandStep != BandStep::none  ? bandCardPrompt()
                 : "Map the bands as groups";
        if (r.conflictWith.isNotEmpty())
            return "Index already assigned to " + r.conflictWith;
        if (r.kind == "ignore") return "Agree to ignore " + r.proposedName + "?";
        if (r.kind == "unsure") return "Classifier note: " + r.proposedName;
        if (! r.sweep.points.isEmpty() && r.sweep.nonNumeric)
            return displayLabel (r) + ": it is a labelled switch";
        if (r.semantic == "mode")
            return "Record mode switch " + r.proposedName + "?";
        if (r.proposedIndex >= 0)
            return "Is [" + juce::String (r.proposedIndex) + "] " + r.proposedName
                 + " the " + displayLabel (r) + "?";
        return "Does this plugin have " + displayLabel (r) + "?";
    }

    /** The answers, as buttons wearing their keys. Built per row from the
        SAME keyValid the keys use; a greyed button is a refusal you can read
        before pressing anything.
    */
    void rebuildAnswers()
    {
        answerButtons.clear();

        if (awaitingCategory)
        {
            const auto cats = DialSets::categories();
            for (int i = 0; i < cats.size(); ++i)
            {
                auto* b = answerButtons.add (new juce::TextButton (
                    (i < 9 ? juce::String (i + 1) + " - " : juce::String()) + cats[i]));
                addAndMakeVisible (b);
                const auto cat = cats[i];
                b->onClick = [this, cat] { pickCategory (cat); grabKeyboardFocus(); };
            }
            resized(); return;
        }

        if (rowCount() == 0) { resized(); return; }
        auto& r = rowAt (selected);

        auto add = [this] (const juce::String& id, const juce::String& text)
        {
            auto* b = answerButtons.add (new juce::TextButton (text));
            addAndMakeVisible (b);
            b->setEnabled (keyValid (id));
            b->onClick = [this, id] { dispatchAction (id); grabKeyboardFocus(); };
        };

        if (awaitingCaptureRow >= 0 && lastGesture.indices.size() > 1)
        {
            for (int i = 0; i < juce::jmin (9, lastGesture.indices.size()); ++i)
            {
                const int oneBased = i + 1;
                auto* b = answerButtons.add (new juce::TextButton (
                    juce::String (oneBased) + " - " + lastGesture.names[i]));
                addAndMakeVisible (b);
                b->onClick = [this, oneBased] { actionPickCandidate (oneBased); grabKeyboardFocus(); };
            }
            resized(); return;
        }

        if (r.isResolved())
        {
            add ("next", "> - next row");
            add ("wiggle", "W - re-open (re-capture)");
            resized(); return;
        }

        if (r.kind == "bands" && ! r.isResolved() && bandStep == BandStep::none)
        {
            add ("space", "SPACE - begin mapping bands");
            add ("defer", "D - later");
            resized(); return;
        }
        if (r.kind == "bands" && bandStep != BandStep::none && bandStep != BandStep::table)
        {
            add ("wiggle", "R - discard and re-arm");
            if (bandStep == BandStep::capQ1) add ("notpresent", "N - no Q on this band");
            add ("defer",  "D - leave bands for later");
            resized(); return;
        }
        if (r.conflictWith.isNotEmpty())
        {
            add ("space",      "SPACE - yes, shared control");
            add ("wiggle",     "W - re-capture");
            add ("notpresent", "N - no such control");
            add ("defer",      "D - later");
            resized(); return;
        }

        if (r.kind == "ignore")
        {
            add ("notpresent", "N - agree, ignore it");
            add ("wiggle",     "W - dispute: touch it");
            add ("defer",      "D - later");
        }
        else if (r.kind == "unsure")
        {
            add ("defer",  "D - defer this note");
            add ("wiggle", "W - capture what it means");
        }
        else if (r.semantic == "mode")
        {
            add ("modematerial", "M - record with labels");
            add ("wiggle",       "W - verify by touching");
            add ("notpresent",   "N - not present");
            add ("defer",        "D - later");
        }
        else if (! r.sweep.points.isEmpty() && (r.sweep.nonNumeric || r.sweep.flat))
        {
            if (r.sweep.nonNumeric) add ("modematerial", "M - record as mode/position");
            if (r.sweep.flat)       add ("typed",        "T - type anchors");
            add ("defer", "D - later");
        }
        else if (r.proposedIndex >= 0)
        {
            add ("space",      "SPACE - accept proposal");
            add ("wiggle",     "W - touch the control");
            add ("notpresent", "N - no such control");
            add ("defer",      "D - later");
        }
        else
        {
            add ("wiggle",     "W - touch it on the GUI");
            add ("notpresent", "N - it does not exist");
            add ("defer",      "D - later");
        }
        resized();
    }

    int getNumRows() override { return rowCount(); }

    void paintListBoxItem (int i, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (i >= rowCount()) return;
        auto& r = const_cast<AssignPanel*> (this)->rowAt (i);

        // THE CURRENT ROW IS UNMISTAKABLE: a bright fill and an edge bar, not
        // a faint highlight. And a resolved row changes on the spot: tinted
        // background, tick or dash, the outcome IN WORDS. A glance answers
        // both "where am I" and "did that keypress do anything".
        if (sel)
        {
            g.fillAll (juce::Colour (0xff2c4a5c));
            g.setColour (juce::Colour (0xff6ad8e0));
            g.fillRect (0, 0, 4, h);
        }
        else if (r.state == AssignRow::State::confirmed)
            g.fillAll (juce::Colour (0xff15281a));
        else if (r.state == AssignRow::State::modeMaterial)
            g.fillAll (juce::Colour (0xff2a2416));
        else if (r.isSkipped())
            g.fillAll (juce::Colour (0xff14181f));

        juce::Colour col = juce::Colour (0xff9fd8e0);
        if (r.state == AssignRow::State::confirmed)         col = juce::Colour (0xff6ad86a);
        else if (r.state == AssignRow::State::modeMaterial) col = juce::Colour (0xffd8b06a);
        else if (r.state == AssignRow::State::swept
              || r.state == AssignRow::State::armed)        col = juce::Colour (0xffe0c060);
        else if (r.isSkipped())                             col = juce::Colour (0xff70798a);
        else if (r.kind == "unsure")                        col = juce::Colour (0xffd8b06a);
        else if (r.kind == "ignore")                        col = juce::Colour (0xff607080);
        if (sel) col = col.brighter (0.3f);
        g.setColour (col);
        g.setFont (juce::FontOptions (13.0f, sel ? juce::Font::bold : juce::Font::plain));

        juce::String t;
        t << (sel ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 "))
              : r.state == AssignRow::State::confirmed
                  ? juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93 "))
              : r.state == AssignRow::State::modeMaterial
                  ? juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x86 "))
              : r.isSkipped() ? "- " : "  ");
        t << displayLabel (r);
        const auto word = stateWord (r);
        if (word.isNotEmpty())
            t << "   " << word;
        else if (r.kind != "ignore" && r.kind != "unsure")
        {
            if (r.proposedIndex >= 0)
                t << "  <- [" << r.proposedIndex << "] " << r.proposedName;
            else
                t << "  (unmapped: N if absent)";
        }
        if (r.proposalMismatch) t << "  re-pointed";
        g.drawText (t, 8, 0, w - 12, h, juce::Justification::centredLeft);
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

        if (sw.ok && r.semantic == "mode")
        {
            // Same guard as SPACE: mode never confirms via anchors.
            r.state = AssignRow::State::swept;
            say ("Mode switch captured: M records it as mode/position material.");
            persistSession(); list.updateContent(); updateQuestion();
            return;
        }

        if (sw.ok)
        {
            // The tool knows NOW whether another semantic holds this index.
            // Ask now, in the card, not minutes later at review.
            const auto holder = confirmedHolderOf (idx, &r);
            if (holder.isNotEmpty())
            {
                r.state = AssignRow::State::captured;
                r.conflictWith = holder;
                say ("[" + juce::String (idx) + "] "
                       + (hooks.paramName ? hooks.paramName (idx) : juce::String())
                       + " is already assigned to " + holder + ". Decide in the card.");
                persistSession(); list.updateContent(); updateQuestion();
                return;
            }

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
                   + (sw.flat ? "  T for typed anchors." : "")
                   + (sw.nonNumeric ? "  M records it as mode/position material." : ""));
            updateQuestion();     // the strip offers the key AT the refusal
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
        if (category.isNotEmpty()) t << category << "  ";
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

    void say (const juce::String& s)
    {
        lastStatus = s;
        notice.setText (s, juce::dontSendNotification);   // in the card, not elsewhere
        if (hooks.status) hooks.status (s);
    }
    juce::String lastStatus;

    juce::File root;
    juce::String fp, pluginId, category;
    EvidenceIndex evidence;
    juce::ListBox list;
    juce::Label progress;
    juce::Label question;
    juce::Label promptTitle;
    juce::ComboBox categoryBox;
    bool awaitingCategory = false;
    ProposalSet beginProposals;
    juce::Label notice;
    juce::OwnedArray<juce::TextButton> answerButtons;
    juce::TextButton prevBtn, nextBtn, evidBtn, bulkBtn, skipPluginBtn, reviewBtn;
    juce::TextEditor summaryText;
    juce::TextButton submitBtn, backBtn;
    bool summaryShowing = false;
    juce::TextEditor reasonEntry;
    juce::Optional<AssignRow::State> pendingSkip;
    juce::String pendingAutoSkipReason;
    CaptureEngine::Result lastGesture;
    int selected = 0;
    int awaitingCaptureRow = -1;
    int insistRow = -1;
    juce::uint32 insistAt = 0;
    int typedRow = -1;
    juce::uint32 startedAt = 0, bulkArmedAt = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AssignPanel)
};

} // namespace ejmap
