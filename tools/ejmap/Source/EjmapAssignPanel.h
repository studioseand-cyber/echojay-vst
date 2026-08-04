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
#include "EjmapExposure.h"

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
        std::function<juce::Array<int>()>              maskIndices;    // masked = not a control
        std::function<juce::var()>                     loadBreadcrumbs;// tier2-candidates rows, deduped
        std::function<juce::String (int, double)>      spotCheck;      // set norm, read display, restore
        std::function<SweepOutcome (int)>              sweepIndexSetread; // the honest retry
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

        for (auto* b : { &prevBtn, &nextBtn, &evidBtn, &bulkBtn, &skipPluginBtn, &parkBtn, &reviewBtn })
            addAndMakeVisible (b);
        prevBtn.setButtonText ("< prev");            prevBtn.onClick = [this] { dispatchAction ("prev"); grabKeyboardFocus(); };
        nextBtn.setButtonText ("> next");            nextBtn.onClick = [this] { dispatchAction ("next"); grabKeyboardFocus(); };
        evidBtn.setButtonText ("? evidence");        evidBtn.onClick = [this] { dispatchAction ("evidence"); grabKeyboardFocus(); };
        bulkBtn.setButtonText ("I bulk ignores");    bulkBtn.onClick = [this] { dispatchAction ("bulk"); grabKeyboardFocus(); };
        skipPluginBtn.setButtonText ("S dismiss"); skipPluginBtn.onClick = [this] { dispatchAction ("skipplugin"); grabKeyboardFocus(); };
        parkBtn.setButtonText ("ESC park"); parkBtn.onClick = [this] { dispatchAction ("park"); grabKeyboardFocus(); };
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
        reasonEntry.onReturnKey = [this] { commitEntry(); };
        reasonEntry.onEscapeKey = [this]
        {
            pendingSkip = {}; pendingEntry = nullptr;
            reasonEntry.setVisible (false);
            say ("Cancelled.");
            grabKeyboardFocus();
        };

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
        else if (restoredCategory().isNotEmpty())
        {
            // A PARKED SESSION ALREADY ANSWERED THIS. Asking again is not
            // caution, it is amnesia: the category is in the session file, it
            // was chosen by the same human, and until it is answered the panel
            // has NO ROWS -- so a park-and-return looked like a lost session.
            // Caught by the park round-trip test, which compared 5 rows before
            // against 0 after.
            //
            // This is not the silent default the rule above forbids: the
            // category is read from what the mapper chose, and the header
            // shows it, and it stays changeable.
            category = restoredCategory();
        }
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

    /** The category this fp was last mapped under, from the session file.
        Empty when there is no session, which is when the human is asked.
    */
    juce::String restoredCategory() const
    {
        auto f = sessionFile();
        if (! f.existsAsFile()) return {};
        auto v = juce::JSON::parse (f.loadFileAsString());
        if (v.getProperty ("fp", "").toString() != fp) return {};
        return v.getProperty ("category", "").toString();
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

        // 3b. The Tier 2 controls row, every category: the full surface,
        //     named. What the AI can dial BY NAME once mapped.
        {
            AssignRow r;
            r.semantic = "controls";
            r.kind = "controls";
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
                if (r.slotKey() == pr.slotKey() && pr.semantic.isNotEmpty())
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
                return r.kind == "bands"    ? "BANDS: " + r.skipReason
                     : r.kind == "controls" ? "CONTROLS: " + r.skipReason
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
    /** THE ROUTING TARGET FOLLOWS THE CURSOR. It used to persist independently:
        W on a control set controlsWiggleTarget, the wizard advanced to a Tier 1
        row, and nothing cleared it -- so every later touch was checked against
        the control armed earlier while the card asked about the parameter, and
        SPACE stayed locked because no evidence ever reached the row on screen.
        Two sources for one act, which is the attribution-lost-on-merge class in
        the interaction layer.

        Cleared HERE because selectRow is the choke point every cursor move goes
        through, rather than at each call site -- a guard at the sites is the one
        that gets forgotten by the next path added. */
    void selectRow (int i)
    {
        const int want = juce::jlimit (0, juce::jmax (0, rowCount() - 1), i);
        if (want != selected) clearCaptureTargets();
        selected = want;
        list.selectRow (selected); list.scrollToEnsureRowIsOnscreen (selected);
        list.updateContent(); updateQuestion();
    }

    /** Every field that says "a capture arriving now belongs to X". If a new
        one is ever added it belongs in this list, and the list is why there is
        a function rather than three assignments. */
    void clearCaptureTargets()
    {
        if (controlsWiggleTarget >= 0 || bandWiggleTarget >= 0 || awaitingCaptureRow >= 0)
            say ("(capture target cleared: the row moved, so a touch now belongs "
                 "to the row on screen)");
        controlsWiggleTarget = -1;
        bandWiggleTarget = -1;
        controlsTypedEntry = -1;
        awaitingCaptureRow = -1;
    }

    //==========================================================================
    /** ONE validity function for every surface. The question strip, the key
        handler, the legend's highlighting and the legend's clicks all ask
        this, so they cannot disagree about what a key does on this row.
    */
    bool keyValid (const juce::String& id)
    {
        if (tierPhase)
            return id == "space" || id == "expand" || id == "prev" || id == "next"
                || id == "kick" || id == "pull" || id == "defer";
        if (rowCount() == 0) return id == "skipplugin";
        auto& r = rowAt (selected);

        if (id == "typed" && r.kind == "bands" && ! r.isResolved())
            return true;                               // T enters bands by hand
        if (id == "space" && r.kind == "bands" && ! r.isResolved())
            return true;                               // begins the band flow (or accepts manual)
        if (id == "space" && r.kind == "controls" && ! r.isResolved())
            return true;                               // begins the controls sweep
        if (id == "space" && r.conflictWith.isNotEmpty() && ! r.isResolved() && r.sweep.ok)
            return true;                               // the insist on a shared control
        if (id == "space")
            return ! deepMode && ! r.isResolved() && r.semantic.isNotEmpty()
                && r.semantic != "mode"                    // mode never confirms via anchors
                && ! r.sweep.nonNumeric                    // a labelled switch can never confirm
                && r.proposedIndex >= 0
                && evidence.corroborationFor (r.proposedIndex, r.proposedName).isNotEmpty();
        if (id == "wiggle")      return r.kind != "ignore";
        // A fixed-frequency band is the one row that could not be answered.
        // N here means "this band has no frequency CONTROL", not "no bands".
        if (id == "notpresent" && r.isBandMemberRow() && r.semantic == "freq_hz")
            return r.state != AssignRow::State::confirmed;
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
            if (bandPickPending())
            {
                if (id == "wiggle")     // none of these: discard the pick, re-arm
                {
                    bandGesturePending = {};
                    say ("Pick discarded. " + bandCardPrompt());
                    if (hooks.armForRow) hooks.armForRow();
                    updateQuestion();
                    return;
                }
                if (id != "defer")
                {
                    say ("A lockstep pick is open: answer it with the numbered buttons "
                         "(or R if none of these was your touch).");
                    return;
                }
            }
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
        if (tierPhase)
        {
            if (id == "space")  { tierAccept(); return; }
            if (id == "expand") { tierExpanded = ! tierExpanded; showTierCard(); return; }
            if (id == "prev")   { tierCursor = juce::jmax (0, tierCursor - 1); showTierCard(); return; }
            if (id == "next")   { ++tierCursor; showTierCard(); return; }
            if (id == "kick")   { tierKick(); return; }
            if (id == "pull")   { tierPull(); return; }
            if (id == "defer")  { tierDefer(); return; }
            say ("Exposure preview: X kicks the selected row out, P pulls it up, "
                 "V shows the rest, SPACE accepts, D reverts this visit.");
            return;
        }
        if (controlsPhase)
        {
            if (id == "space")      { actionControlsAccept(); return; }
            if (id == "expand")     { controlsExpanded = ! controlsExpanded; showControlsTable(); return; }
            if (id == "prev")       { controlsCursor = juce::jmax (0, controlsCursor - 1); showControlsTable(); return; }
            if (id == "next")       { controlsCursor = controlsCursor + 1; showControlsTable(); return; }
            if (id == "notpresent")
            {
                if (auto* e = cursorControl())
                {
                    e->excluded = true;
                    controlsExcluded.add (e->index);
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("kind", "control_excluded");
                    o->setProperty ("index", e->index);
                    o->setProperty ("name", e->name);
                    o->setProperty ("reason", "excluded by mapper");
                    if (hooks.writeRow) hooks.writeRow (juce::var (o));
                    say ("Excluded " + e->name + " (recorded).");
                    showControlsTable();
                }
                return;
            }
            if (id == "wiggle")
            {
                if (auto* e = cursorControl())
                {
                    controlsWiggleTarget = e->index;
                    say ("Touch '" + e->name + "' to verify it - the capture is live.");
                    awaitingCaptureRow = controlsRowIndex;
                    if (hooks.armForRow) hooks.armForRow();
                }
                return;
            }
            if (id == "typed")
            {
                if (auto* e = cursorControl())
                {
                    controlsTypedEntry = e->index;
                    if (hooks.startTyped) hooks.startTyped (e->index);
                }
                return;
            }
            if (id == "defer")
            {
                controlsPhase = false;
                summaryText.setVisible (false);
                rowAt (controlsRowIndex).state = AssignRow::State::proposed;
                say ("Controls left for later; nothing accepted.");
                updateQuestion(); list.updateContent(); resized();
                return;
            }
            say ("Controls table: SPACE accepts, F expands, arrows pick, W/N/T act on the row, D leaves.");
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
        if (id == "typed" && rowCount() > 0 && rowAt (selected).kind == "bands"
             && ! rowAt (selected).isResolved())
        { actionManualBandsBegin(); return; }
        if (id == "notpresent" && rowCount() > 0 && rowAt (selected).isBandMemberRow()
             && rowAt (selected).semantic == "freq_hz"
             && rowAt (selected).state != AssignRow::State::confirmed)
        { actionBandFreqFixed(); return; }

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
        else if (id == "park")       actionParkPlugin();
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
            // SPACE means "accept what I entered" once manual rows exist, and
            // "start the guided captures" before that. Same key, and the card
            // says which it is.
            if (manualBands && anyManualBandRows()) actionManualBandsAccept();
            else                                    actionBandsBegin();
            return;
        }

        if (r.kind == "controls" && ! r.isResolved())
        {
            actionControlsBegin();
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

        // The surface rows are not parameters. W on the controls row armed a
        // capture five times on the spiff run, timed out twice, and once
        // captured [10] boost depth as if "controls" were a knob. W means
        // "redo this row's own flow": re-sweep the surface, restart the
        // bands.
        if (r.kind == "controls") { actionControlsBegin(); return; }
        if (r.kind == "bands")    { actionBandsBegin(); return; }

        // RE-OPENING WITHDRAWS THE PREVIOUS OUTCOME, AND ITS REASON WITH IT.
        // W is the only route from a resolved row back to an unresolved one,
        // so it is the choke point where a stale reason must die. Measured on
        // API-2500 (m): a row superseded at 11:53:01 was re-opened and
        // confirmed again at 11:54:30 on 31 Jul, and reached disk CONFIRMED
        // while still carrying "superseded: [3] confirmed for release_ms" --
        // a record of an outcome that no longer applied. actionSkip already
        // overwrites the reason when the row is skipped again; the confirm
        // paths never did, and a reason nobody clears outlives its verdict.
        // Cleared here rather than at each confirm site, because there are
        // four of those and one of this.
        r.conflictWith = {};
        r.skipReason = {};
        r.state = AssignRow::State::armed;
        awaitingCaptureRow = selected;
        say ("ARMED for " + (r.semantic.isNotEmpty() ? r.semantic : juce::String ("unsure row"))
               + " - move the control on the plugin");
        if (hooks.armForRow) hooks.armForRow();
        // THE WITHDRAWAL IS DURABLE. W used to change the row on screen and
        // leave the session file saying "confirmed" until some later action
        // wrote -- so the disk and the screen disagreed about a verdict, which
        // is the class this project keeps filing, and the session file is what
        // a parked plugin is restored from. Decided 4 Aug 2026: a crash here
        // costs one confirmation and seconds to redo; a session file that
        // vouches for a withdrawn verdict costs trust in every parked plugin.
        persistSession();
        list.updateContent();
        // THE CARD MUST FOLLOW THE ROW. W changes what the row is and what its
        // keys mean -- a confirmed row offers "> and W", a re-opened one
        // offers the answers again -- and without this the strip kept showing
        // the OLD row's answers until some later action refreshed it. Found by
        // a self-test asserting the re-opened frequency row offers "N - no
        // frequency control": N worked, and the card had not said so.
        updateQuestion();
    }

    /** Routed from the capture engine via MainComponent. */
    void captureArrived (const CaptureEngine::Result& res)
    {
        if (bandStep == BandStep::capFreq1 || bandStep == BandStep::capGain1
             || bandStep == BandStep::capQ1 || bandStep == BandStep::capFreqLast)
        { bandCaptureArrived (res); return; }

        // BOTH conditions, not either: the target must be live AND the capture
        // must belong to the controls row. The row check is what stops a stale
        // target from claiming a capture meant for a Tier 1 parameter.
        if (controlsPhase && controlsWiggleTarget >= 0
             && awaitingCaptureRow == controlsRowIndex)
        {
            const int idx = res.primaryIndex >= 0 ? res.primaryIndex
                            : (res.indices.size() == 1 ? res.indices[0] : -1);
            for (auto& e : controlEntries)
                if (e.index == controlsWiggleTarget)
                {
                    if (idx == e.index)
                    { e.trust = "human-verified"; say (e.name + " verified by touch."); }
                    else
                        say ("You touched [" + juce::String (idx) + "] "
                               + (hooks.paramName ? hooks.paramName (idx) : juce::String())
                               + ", not '" + e.name + "' - trust unchanged.");
                }
            controlsWiggleTarget = -1;
            showControlsTable();
            return;
        }

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
    /** ONE text entry, two callers. The skip-reason box and every numeric
        prompt manual band entry needs are the same widget with the same Enter
        and Esc; a second entry field would be a second set of keys to get
        wrong. pendingEntry, when set, owns Return.
    */
    std::function<void (const juce::String&)> pendingEntry;

    void askText (const juce::String& prompt, std::function<void (const juce::String&)> onCommit)
    {
        pendingEntry = std::move (onCommit);
        reasonEntry.setVisible (true);
        reasonEntry.setText ({}, juce::dontSendNotification);
        reasonEntry.grabKeyboardFocus();
        resized();
        say (prompt);
    }

    void commitEntry()
    {
        if (pendingEntry != nullptr)
        {
            const auto text = reasonEntry.getText().trim();
            auto fn = pendingEntry;
            pendingEntry = nullptr;
            reasonEntry.setVisible (false);
            resized();
            grabKeyboardFocus();
            fn (text);
            return;
        }
        commitCustomReason();
    }

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

    /** What the sweeper OBSERVED about this parameter, in the words of the
        flags it set. Never "the mapper preferred to type": that is the one
        answer this must not be able to give, because it is the answer a
        bypass would want.

        An empty string means the sweep left no refusal behind -- typing over a
        sweep that WORKED is a legitimate act (a mapper correcting a bad table)
        and it is recorded as exactly that, unearned, so a gate can tell the
        two apart.
    */
    static juce::String describeTypedReason (const SweepOutcome& s)
    {
        juce::StringArray why;
        if (s.points.isEmpty())      why.add ("no sweep was taken");
        if (s.flat)                  why.add ("display flat through both paths (text liar)");
        if (s.nonNumeric)            why.add ("display is labels, not numbers");
        if (s.identityDisplay)       why.add ("identity display: every value equalled its own norm");
        if (! s.ok && ! s.points.isEmpty() && s.reason.isNotEmpty())
            why.add ("sweep refused: " + s.reason.substring (0, 80));
        if (why.isEmpty())
            return "typed over a usable sweep (no refusal recorded): NOT earned";
        return why.joinIntoString ("; ");
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
        if (controlsPhase && controlsTypedEntry >= 0)
        {
            for (auto& e : controlEntries)
                if (e.index == controlsTypedEntry)
                {
                    e.sweep = sw;
                    e.unbuildable = ! sw.ok;
                    e.kind = "anchored";
                    e.trust = sw.ok ? "human-verified" : e.trust;
                    e.note = sw.ok ? juce::String (sw.anchors.size()) + " anchors (human-typed)"
                                   : "typed table refused: " + sw.reason.substring (0, 60);
                }
            controlsTypedEntry = -1;
            showControlsTable();
            return;
        }
        if (typedRow < 0 || typedRow >= rowCount()) return;
        auto& r = rowAt (typedRow);

        // READ THE REFUSING SWEEP BEFORE OVERWRITING IT. The sweep that failed
        // is the evidence for why typing was necessary, and the next line
        // replaces it with the typed table. Ordered deliberately: after the
        // assignment there is nothing left to read.
        r.typedReason = describeTypedReason (r.sweep);

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

    /** ESC: PARK. Return to the list without deciding anything.

        S already returned to the list, and that is the whole of what it shares
        with this: it writes `deferred` and "plugin skipped by mapper" onto
        every unresolved row first, which is a VERDICT about rows the mapper
        never considered. Coming back then means re-opening rows that claim
        they were decided.

        Parking writes NO row state. The session is already on disk -- persist
        happens on every accept and, since 4 Aug, on every W -- so this call
        catches only the gap between the last action and the keypress. It is
        one line of machinery and a sentence of wording, which is why it came
        before the two features it sits beside.

        The plugin stays loaded, because exitPanel only hides the panel. That
        is deliberate for ESC: the commonest reason to park is to look
        something up, and reloading a plugin costs 0.5-1.6 s and is where
        crashes happen.
    */
    void actionParkPlugin()
    {
        if (hooks.cancelArm) hooks.cancelArm();
        awaitingCaptureRow = -1;

        int confirmed = 0, open = 0;
        for (const auto& r : rows)
        {
            if (r.state == AssignRow::State::confirmed) ++confirmed;
            else if (! r.isResolved()) ++open;
        }

        persistSession();
        say ("PARKED: " + juce::String (confirmed) + " confirmed, " + juce::String (open)
               + " still open, saved to " + sessionFile().getFileName()
               + ". Load it again and everything comes back.");
        if (hooks.exitPanel) hooks.exitPanel();
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
        // A live card owns the surface (the lockstep-pick rule). On the AMEK
        // re-run the band table stayed rendered over a waiting card, buttons
        // and all -- so arming hides the table, always.
        summaryText.setVisible (false);
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
        summaryText.setVisible (false);
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
                // Filter the candidates BEFORE raising a pick. A previous
                // touch settles across the card boundary (caught live on
                // AMEK: the freq pair's tail arrived on the GAIN card as a
                // multi), so: already-captured members are residue, and so is
                // anything that also moved in the PREVIOUS arm. What survives
                // is what the hand could actually have touched now.
                juce::Array<int> fresh;
                juce::StringArray freshNames;
                for (int i = 0; i < res.indices.size(); ++i)
                {
                    const int ci = res.indices[i];
                    bool residue = false;
                    for (const auto& cm : bandCaptured)
                        if (cm.index == ci) residue = true;
                    if (! residue && ! bandPlan.arms.isEmpty())
                        residue = bandPlan.arms.getReference (bandPlan.arms.size() - 1)
                                    .moved.contains (ci);
                    if (! residue)
                    {
                        fresh.add (ci);
                        freshNames.add (i < res.names.size() ? res.names[i] : juce::String());
                    }
                }

                if (fresh.isEmpty())
                {
                    // NAME the filtered movers. The AMEK re-run burned eight
                    // re-arms on the Q card under this message while the
                    // measured control tail is 6-7 ms -- "still settling" was
                    // a guess wearing a diagnosis. The row written here makes
                    // the next storm attributable from disk.
                    juce::StringArray culprits;
                    for (int i = 0; i < res.indices.size(); ++i)
                        culprits.add ("[" + juce::String (res.indices[i]) + "] "
                                        + (i < res.names.size() ? res.names[i] : juce::String()));
                    say ("Only residue moved (" + culprits.joinIntoString (", ")
                           + ") - re-armed: " + bandCardPrompt());
                    if (hooks.writeRow)
                    {
                        auto* o = new juce::DynamicObject();
                        o->setProperty ("kind", "band_rearm_residue");
                        juce::Array<juce::var> iv;
                        for (int i : res.indices) iv.add (i);
                        o->setProperty ("indices", juce::var (iv));
                        o->setProperty ("names", culprits.joinIntoString (", "));
                        hooks.writeRow (juce::var (o));
                    }
                    // Re-arm rebuilds the surface, never inherits it: this is
                    // where the band table sat over the waiting card with its
                    // buttons still live (AMEK re-run, capFreqLast).
                    summaryText.setVisible (false);
                    if (hooks.armForRow) hooks.armForRow();
                    updateQuestion();
                    return;
                }
                if (fresh.size() == 1)
                {
                    // One genuinely new mover among the residue: the touch.
                    processBandIndex (fresh[0], res);
                    return;
                }

                // A REAL lockstep pair (AMEK's Param Link mirroring LF Freq 1
                // onto LF Freq 2: the first confirmed linked pair this
                // project has seen live). The plugin cannot say which was
                // touched; the human picks. THE PICK OWNS THE CARD: its own
                // headline, the candidates as the only buttons, no next-step
                // text until answered.
                bandGesturePending = res;
                bandGesturePending.indices = fresh;
                bandGesturePending.names = freshNames;
                say ("Lockstep pair: pick the control you touched.");
                updateQuestion();
                return;
            }
            say ("Capture unusable (" + res.reason + ") - re-arming. R also re-arms.");
            summaryText.setVisible (false);
            if (hooks.armForRow) hooks.armForRow();
            updateQuestion();
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
        const int picked = res.indices[oneBased - 1];
        // Source A of lockstep_of (signed 2026-08-02): the tool has just
        // watched every candidate move together; the human named the touched
        // one. The unpicked partner(s) are OBSERVED 1:1 movers of the picked
        // address -- recorded here as cargo, or the observation dies with the
        // pick (the spiff lesson: the session carries the cargo).
        for (int i : res.indices)
            if (i != picked)
            { lockstepObserved[i] = picked; lockstepSource[i] = "human_pick"; }
        persistSession();
        processBandIndex (picked, res);
        updateQuestion();
    }

    /** Test seams for the text prompt: answering it IS typing the text and
        pressing Enter, so the self-test drives the same call the key does.
    */
    bool isAskingText() const { return pendingEntry != nullptr; }
    bool answerEntry (const juce::String& text)
    {
        if (pendingEntry == nullptr) return false;
        reasonEntry.setText (text, juce::dontSendNotification);
        commitEntry();
        return true;
    }

    /** Row lookup by band slot, for the self-test and for anything that needs
        to point at "band 2's gain" without knowing the checklist's order.
    */
    int bandRowIndex (int ordinal, const juce::String& semantic) const
    {
        for (int i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows.getReference (i);
            if (r.isBandMemberRow() && r.bandOrdinal == ordinal && r.semantic == semantic)
                return i;
        }
        return -1;
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

    //==========================================================================
    // MANUAL BAND ENTRY (M5b, 4 Aug 2026). The typed-anchors equivalent for
    // bands: when inference cannot group a band, the mapper says which indices
    // form it.
    //
    // NO PICKER. One count prompt, then synthesised rows -- "band 1 frequency",
    // "band 1 gain" -- each resolved through the flow the mapper already knows
    // (touch, W, confirm). The panel renders and resolves rows generically, so
    // mismatch warnings, readback and skip reasons all arrive free, and there
    // is no new widget and no new mental model.
    //
    // ORDER IS NEVER ENTRY ORDER. The mapper supplies the LABEL; accept sorts
    // the bands by frequency and refuses when the two disagree. On a swept
    // band the frequency is measured; on a FIXED band (a graphic EQ: 31, 63,
    // 125...) it is transcribed off the panel, and sorting transcribed
    // constants is still derivation. What is never allowed to become the claim
    // is the order the mapper happened to type.

    bool manualBands = false;
    int  manualBandCount = 0;

    bool anyManualBandRows() const
    {
        for (const auto& r : rows) if (r.isBandMemberRow()) return true;
        return false;
    }

    /** T on the bands row, or T at the band table when inference produced
        nothing usable.
    */
    void actionManualBandsBegin()
    {
        if (bandsRowIndex < 0) bandsRowIndex = selected;
        if (hooks.cancelArm) hooks.cancelArm();
        awaitingCaptureRow = -1;
        bandStep = BandStep::none;
        summaryText.setVisible (false);

        askText ("How many bands does this EQ have? (a number, then Enter. Esc cancels)",
                 [this] (const juce::String& text)
        {
            const int n = text.getIntValue();
            if (n < 1 || n > 32)
            {
                say ("Band count must be 1-32; '" + text + "' is not usable. T to try again.");
                return;
            }
            synthesiseBandRows (n);
        });
    }

    /** THE COUNT PROMPT AND ROW SYNTHESIS COME BEFORE ANY GROUP EXISTS. A
        group only materialises at accept, once its frequency rows resolve --
        because the order is derived from those frequencies, and a group
        formed earlier would have to be renumbered by the thing that decides
        the numbering.
    */
    void synthesiseBandRows (int n)
    {
        // Restart-safe: T again rebuilds the slots rather than doubling them.
        for (int i = rows.size(); --i >= 0;)
            if (rows.getReference (i).isBandMemberRow())
                rows.remove (i);

        manualBands = true;
        manualBandCount = n;

        int at = juce::jmax (0, bandsRowIndex + 1);
        for (int b = 1; b <= n; ++b)
            for (auto* sem : { "freq_hz", "gain_db", "q", "enable" })
            {
                AssignRow r;
                r.kind = "band_member";
                r.semantic = sem;
                r.proposalSource = "none";
                r.bandOrdinal = b;
                r.bandLabel = juce::String (b);
                rows.insert (at++, r);
            }

        auto& br = rowAt (bandsRowIndex);
        br.state = AssignRow::State::proposed;
        br.skipReason = {};

        selectRow (bandsRowIndex + 1);
        say (juce::String (n) + " bands: " + juce::String (n * 4) + " rows added. Resolve each "
             "frequency and gain (W to touch it); Q and enable are D-able without penalty. "
             "SPACE on the bands row accepts when the frequencies are in.");
        persistSession();
        list.updateContent();
        updateProgress();
        updateQuestion();
    }

    /** N on a band frequency row: this band has no frequency CONTROL. That is
        the graphic-EQ answer the card could not previously give -- every other
        row can be answered "not present" and this one could not, so the only
        honest response was to abandon the whole bands row.
        The frequency is then transcribed, not measured, and says so.
    */
    void actionBandFreqFixed()
    {
        auto& r = rowAt (selected);
        // STAND THE CAPTURE DOWN, as actionSkip does for an armed row. N here
        // is reached from an ARMED row whenever the mapper re-opened it with W
        // first, and an arm left live would deliver a later touch into a row
        // that has already answered.
        if (r.state == AssignRow::State::armed)
        {
            if (hooks.cancelArm) hooks.cancelArm();
            awaitingCaptureRow = -1;
        }
        askText ("Band " + r.bandLabel + " has no frequency control. What frequency is it "
                 "FIXED at, as printed on the plugin? (Hz, then Enter)",
                 [this] (const juce::String& text)
        {
            auto& rr = rowAt (selected);
            const double hz = text.retainCharacters ("0123456789.").getDoubleValue();
            if (hz <= 0.0 || hz > 30000.0)
            { say ("'" + text + "' is not a frequency in Hz. N to try again."); return; }

            rr.typedFreqHz = hz;
            rr.freqSource = "typed_fixed";
            rr.resolvedIndex = -1;            // there is no parameter: not a claim, an absence
            rr.state = AssignRow::State::confirmed;
            rr.trust = "human-verified";
            rr.mode = deepMode ? "deep" : "fast";
            rr.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
            recordResolution (rr, "band_freq_fixed");
            say ("Band " + rr.bandLabel + " fixed at " + juce::String (hz, 1)
                   + " Hz (transcribed from the panel, not measured).");
            advance();
            persistSession(); list.updateContent(); updateProgress(); updateQuestion();
        });
    }

    /** Every band's frequency, in Hz, however it was established. Empty
        optional semantics: 0 means "this band has no frequency yet".
    */
    double bandFreqOf (int ordinal) const
    {
        for (const auto& r : rows)
        {
            if (! r.isBandMemberRow() || r.bandOrdinal != ordinal || r.semantic != "freq_hz")
                continue;
            if (r.freqSource == "typed_fixed") return r.typedFreqHz;
            if (r.state == AssignRow::State::confirmed && ! r.sweep.anchors.isEmpty())
            {
                // The measured MIDPOINT of the swept range, which is what
                // orders a parametric band: its lo and hi overlap its
                // neighbours' and its centre does not.
                float lo = r.sweep.anchors.getFirst()[0], hi = lo;
                for (const auto& a : r.sweep.anchors)
                { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                return 0.5 * ((double) lo + (double) hi);
            }
        }
        return 0.0;
    }

    /** SPACE on the bands row while manual entry is live. Builds the groups,
        derives the order from the frequencies, and refuses rather than guess.
    */
    void actionManualBandsAccept()
    {
        juce::SortedSet<int> ordinals;
        for (const auto& r : rows)
            if (r.isBandMemberRow()) ordinals.add (r.bandOrdinal);

        // 1. UNIT SANITY FIRST, and the order of these checks is the point.
        //    It ran second until a self-test caught the consequence: a
        //    frequency slot pointed at a dB control has no frequency, so the
        //    "still unresolved" check fired first and reported a MISSING
        //    frequency for a row that was confirmed and simply wrong. The
        //    coarser refusal masked the precise one -- the same shape as a
        //    suite failing for one reason being unable to report a second.
        //    Ask the specific question before the general one. A slot claimed
        //    as frequency that sweeps to a dB family is the commonest slip --
        //    picking the gain when you meant the freq.
        juce::StringArray unitWrong;
        for (const auto& r : rows)
        {
            if (! r.isBandMemberRow() || r.state != AssignRow::State::confirmed) continue;
            const auto fam = r.sweep.unitFamily;
            if (fam.isEmpty()) continue;                    // undeclared: no claim to check
            if (r.semantic == "freq_hz" && fam != "hz")
                unitWrong.add ("band " + r.bandLabel + " frequency [" + juce::String (r.resolvedIndex)
                                 + "] sweeps in '" + fam + "', not Hz");
            if (r.semantic == "gain_db" && fam != "db")
                unitWrong.add ("band " + r.bandLabel + " gain [" + juce::String (r.resolvedIndex)
                                 + "] sweeps in '" + fam + "', not dB");
        }
        if (! unitWrong.isEmpty())
        {
            say ("REFUSED - " + unitWrong.joinIntoString ("; ")
                   + ". W re-captures the right control.");
            return;
        }

        // 2. Every band needs a frequency AND a gain. A band the consumer
        //    cannot place or cannot move is not dialable, so it is refused
        //    here rather than shipped as half a band.
        juce::StringArray missing;
        for (int i = 0; i < ordinals.size(); ++i)
        {
            const int b = ordinals[i];
            if (bandFreqOf (b) <= 0.0) missing.add ("band " + juce::String (b) + " frequency");
            bool gain = false;
            for (const auto& r : rows)
                if (r.isBandMemberRow() && r.bandOrdinal == b && r.semantic == "gain_db"
                     && r.state == AssignRow::State::confirmed && r.resolvedIndex >= 0)
                    gain = true;
            if (! gain) missing.add ("band " + juce::String (b) + " gain");
        }
        if (! missing.isEmpty())
        {
            say ("Not yet: " + missing.joinIntoString (", ")
                   + " still unresolved. W to touch it, or N on a frequency whose band has no "
                     "frequency control.");
            return;
        }

        // 3. ORDER IS DERIVED FROM FREQUENCY, and disagreement with the
        //    mapper's own labels STOPS. Either a mislabel or a mis-picked
        //    index; both are worth stopping for.
        struct Slot { int ordinal; double hz; };
        juce::Array<Slot> slots;
        for (int i = 0; i < ordinals.size(); ++i)
            slots.add ({ ordinals[i], bandFreqOf (ordinals[i]) });
        auto byHz = slots;
        std::stable_sort (byHz.begin(), byHz.end(),
                          [] (const Slot& a, const Slot& b) { return a.hz < b.hz; });

        for (int i = 0; i < byHz.size(); ++i)
            if (byHz[i].ordinal != slots[i].ordinal)
            {
                juce::String said, measured;
                for (int j = 0; j < slots.size(); ++j)
                    said << (j ? ", " : "") << "band " << slots[j].ordinal
                         << " " << juce::String (slots[j].hz, 1) << " Hz";
                for (int j = 0; j < byHz.size(); ++j)
                    measured << (j ? ", " : "") << "band " << byHz[j].ordinal;
                say ("REFUSED - your band order and the frequencies disagree. You entered "
                     + said + ", which puts them in the order " + measured
                     + ". Either a label is wrong or an index is. Fix the row, or re-enter "
                       "the bands with T.");
                return;
            }

        // 4. DISTINCTNESS, flagged not refused: two bands with the same
        //    frequency suggest one parameter picked twice, or a channel bank
        //    rather than distinct bands.
        for (int i = 1; i < byHz.size(); ++i)
            if (byHz[i].hz > 0.0 && std::abs (byHz[i].hz - byHz[i - 1].hz) < 0.01)
                say ("NOTE: bands " + juce::String (byHz[i - 1].ordinal) + " and "
                       + juce::String (byHz[i].ordinal) + " sit at the same frequency - the same "
                         "control picked twice, or a channel bank rather than two bands.");

        // 5. Groups, numbered by the DERIVED order.
        juce::String freqSourceOverall;
        for (int i = 0; i < byHz.size(); ++i)
        {
            const int b = byHz[i].ordinal;
            GroupSpec g;
            g.n = i + 1;                          // derived, not the entry ordinal
            g.primary = (i == 0);
            g.groupingSource = "mapper";
            g.orderingSource = "derived";

            juce::String fs = "swept";
            for (const auto& r : rows)
            {
                if (! r.isBandMemberRow() || r.bandOrdinal != b) continue;
                if (r.state != AssignRow::State::confirmed) continue;
                if (r.semantic == "freq_hz" && r.freqSource == "typed_fixed")
                {
                    // A fixed band has no frequency PARAMETER, so it emits no
                    // freq_hz mapping. The number lives in the group's range,
                    // where the band matcher's reach test reads it, and
                    // applyBands then declines a freq request on this band
                    // with a reason instead of writing something.
                    fs = "typed_fixed";
                    const double hz = r.typedFreqHz;
                    // A fixed band is not a point: it covers a slice. Half an
                    // octave either side is the honest default for a
                    // graphic-EQ slider, and it is what makes "cut 250 Hz"
                    // reach the 250 slider at all.
                    g.freqLo = hz / std::sqrt (2.0);
                    g.freqHi = hz * std::sqrt (2.0);
                    continue;
                }
                if (r.resolvedIndex < 0 || r.sweep.anchors.size() < 2) continue;
                ParamMapping pm;
                pm.semantic = r.semantic;
                pm.kind = r.semantic;
                pm.paramName = hooks.paramName ? hooks.paramName (r.resolvedIndex) : juce::String();
                pm.indices.add (r.resolvedIndex);
                for (const auto& a : r.sweep.anchors)
                    pm.anchors.add ({ (double) a[1], (double) a[0] });
                pm.anchorsReversed = r.sweep.anchorsReversed;
                pm.trust  = Trust::humanVerified;      // the mapper touched it
                pm.method = r.sweep.method == "setread" ? AnchorMethod::setread
                          : r.sweep.method == "human-typed" ? AnchorMethod::humanTyped
                                                            : AnchorMethod::gettext;
                if (r.semantic == "freq_hz")
                {
                    float lo = r.sweep.anchors.getFirst()[0], hi = lo;
                    for (const auto& a : r.sweep.anchors)
                    { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                    g.freqLo = lo; g.freqHi = hi;
                }
                g.params.add (pm);
            }
            g.freqSource = fs;
            if (freqSourceOverall.isEmpty()) freqSourceOverall = fs;
            else if (freqSourceOverall != fs)  freqSourceOverall = "mixed";
            if (! g.params.isEmpty())
                acceptedGroups.add (g);
        }

        auto& row = rowAt (bandsRowIndex);
        row.state = AssignRow::State::confirmed;
        row.trust = "human-verified";
        row.mode = deepMode ? "deep" : "fast";
        row.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
        row.skipReason = juce::String (acceptedGroups.size())
                       + " band group(s) accepted (entered by hand, " + freqSourceOverall + ")";
        recordResolution (row, "manual_bands_accept");

        auto* o = new juce::DynamicObject();
        o->setProperty ("kind", "manual_bands_accepted");
        o->setProperty ("groups", acceptedGroups.size());
        o->setProperty ("grouping_source", "mapper");
        o->setProperty ("ordering_source", "derived");
        o->setProperty ("freq_source", freqSourceOverall);
        juce::Array<juce::var> hz;
        for (int i = 0; i < byHz.size(); ++i) hz.add (byHz[i].hz);
        o->setProperty ("derived_order_hz", juce::var (hz));
        if (hooks.writeRow) hooks.writeRow (juce::var (o));

        manualBands = false;
        say ("BANDS: " + row.skipReason);
        persistSession();
        advance();
        list.updateContent();
        updateProgress();
        updateQuestion();
    }

    //==========================================================================
    // The Tier 2 controls phase (M6): full-surface sweep + breadcrumbs ->
    // named controls. The table shows ONLY rows needing a decision; the clean
    // majority is a count line, because a SPACE over 217 rows of text is a
    // rubber stamp that reads as review, and what actually happens to a
    // machine-swept control is that a machine swept it.

    struct ControlEntry
    {
        juce::String name;
        int index = -1;
        juce::String kind { "anchored" };      // or "mode"
        SweepOutcome sweep;
        juce::StringArray labelTexts;
        juce::Array<double> labelNorms;
        juce::String provenance { "sweep" };   // or "breadcrumb"
        juce::String trust { "setread" };
        bool duplicate = false, excluded = false, unbuildable = false;
        juce::String note;

        bool needsDecision() const
        {
            return duplicate || excluded || unbuildable
                || kind == "mode" || sweep.identityDisplay;
        }
    };

    juce::Array<ControlEntry> controlEntries;
    juce::Array<NamedControl> pendingControls;

    /** Lockstep observations: unpicked/partner index -> canonical index, with
        the evidence source per record ("human_pick" | "write_verify"). Session
        cargo; submit stamps matching staged controls with lockstep_of.
    */
    std::map<int, int> lockstepObserved;
    std::map<int, juce::String> lockstepSource;

    //==========================================================================
    // The tier preview: EXCEPTION-SHAPED. After the controls table accepts,
    // the card shows the EXACT twelve the server would expose (the pinned
    // re-implementation, spec/controls-exposure-fixture.json) with the rest
    // collapsed. Two gestures: X kicks one out (hidden), P pulls one up
    // (primary). Every gesture re-runs the real ordering so the consequence
    // is watched, not imagined. Untouched controls emit no tier; a capture
    // where the heuristic was right costs zero interactions.
    bool tierPhase = false;
    int tierCursor = 0;
    bool tierExpanded = false;
    juce::StringArray tierSnapshot;                 // tiers at entry, for D
    juce::StringArray tierViewNames;                // visible line -> control name

    ejmap::Exposure::Result tierExposure()
    {
        auto* co = new juce::DynamicObject();
        for (const auto& c : pendingControls)
            co->setProperty (c.name, c.toVar());
        juce::Array<juce::var> gv;
        for (const auto& g : acceptedGroups) gv.add (g.toVar());
        return ejmap::Exposure::build (juce::var (co), juce::var (gv));
    }

    NamedControl* controlByName (const juce::String& nm)
    {
        for (auto& c : pendingControls)
            if (c.name == nm) return &c;
        return nullptr;
    }

    void beginTierPhase()
    {
        tierSnapshot.clear();
        for (const auto& c : pendingControls) tierSnapshot.add (c.tier);
        tierPhase = true; tierCursor = 0; tierExpanded = false;
        showTierCard();
    }

    void showTierCard()
    {
        auto ex = tierExposure();
        tierViewNames.clear();

        juce::String t;
        t << "THE SERVER WOULD EXPOSE THESE " << ex.defaultExposure.size()
          << ", IN THIS ORDER"+juce::String("\n\n");
        auto detailFor = [&ex] (const juce::String& nm) -> juce::String
        {
            for (const auto& c : ex.orderedCandidates)
                if (c.name == nm)
                    return "(" + c.cls + ", " + (c.trust.isEmpty() ? "?" : c.trust) + ", " + c.kind
                         + (c.tier.isNotEmpty() ? ", tier:" + c.tier : "") + ")";
            return {};
        };
        for (const auto& nm : ex.defaultExposure) tierViewNames.add (nm);
        juce::StringArray rest;
        for (const auto& c : ex.orderedCandidates)
            if (! ex.defaultExposure.contains (c.name)) rest.add (c.name);

        tierCursor = juce::jlimit (0, juce::jmax (0,
            (tierExpanded ? tierViewNames.size() + rest.size() : tierViewNames.size()) - 1), tierCursor);

        for (int i = 0; i < tierViewNames.size(); ++i)
            t << (i == tierCursor ? " > " : "   ") << juce::String (i + 1).paddedLeft (' ', 2)
              << "  " << tierViewNames[i] << "  " << detailFor (tierViewNames[i]) << "\n";

        if (! tierExpanded)
            t << "\n --- " << rest.size() << " more stay collapsed (hidden by the heuristic"
              << (ex.excludedNames.isEmpty() ? "" : "; "
                    + juce::String (ex.excludedNames.size()) + " excluded outright") << ") - V to view ---\n";
        else
        {
            t << "\n --- THE REST (P pulls one up) ---\n";
            for (int i = 0; i < rest.size(); ++i)
            {
                const int line = tierViewNames.size() + i;
                t << (line == tierCursor ? " > " : "   ") << "    " << rest[i]
                  << "  " << detailFor (rest[i]) << "\n";
            }
            for (const auto& nm : rest) tierViewNames.add (nm);
            if (! ex.excludedNames.isEmpty())
            {
                t << "\n --- EXCLUDED OUTRIGHT (no tier can reach these) ---\n";
                for (int i = 0; i < ex.excludedNames.size(); ++i)
                    t << "       " << ex.excludedNames[i] << "  <- " << ex.excludedReasons[i] << "\n";
            }
        }

        summaryText.setText (t, juce::dontSendNotification);
        summaryText.setVisible (true);
        promptTitle.setText ("Exposure preview: correct the server, or cost nothing",
                             juce::dontSendNotification);
        question.setText ("SPACE accept as shown (untouched rows emit no tier) - X kick out - "
                          "P pull up - V " + juce::String (tierExpanded ? "collapse" : "view")
                          + " the rest - D revert this visit", juce::dontSendNotification);
        rebuildTierAnswers();
        resized();
    }

    void rebuildTierAnswers()
    {
        answerButtons.clear();
        auto add = [this] (const juce::String& id, const juce::String& text)
        {
            auto* b = answerButtons.add (new juce::TextButton (text));
            addAndMakeVisible (b);
            b->onClick = [this, id] { dispatchAction (id); grabKeyboardFocus(); };
        };
        add ("space",  "SPACE - accept as shown");
        add ("kick",   "X - kick out (tier: hidden)");
        add ("pull",   "P - pull up (tier: primary)");
        add ("expand", juce::String ("V - ") + (tierExpanded ? "collapse" : "view the rest"));
        add ("defer",  "D - revert this visit");
        resized();
    }

    void tierKick()
    {
        if (! juce::isPositiveAndBelow (tierCursor, tierViewNames.size())) return;
        auto* c = controlByName (tierViewNames[tierCursor]);
        if (c == nullptr) return;
        if (c->tier == "hidden")
        { c->tier.clear(); say ("Tier cleared on '" + c->name + "': the heuristic decides again."); }
        else
        { c->tier = "hidden"; say ("'" + c->name + "' kicked out (tier: hidden). Watch what replaces it."); }
        persistSession();
        showTierCard();
    }

    void tierPull()
    {
        if (! juce::isPositiveAndBelow (tierCursor, tierViewNames.size())) return;
        auto* c = controlByName (tierViewNames[tierCursor]);
        if (c == nullptr) return;
        if (ejmap::Exposure::classifyControl (c->name) == "plumbing")
        { say ("'" + c->name + "' is plumbing: the server never exposes it; a tier would be "
               "recorded and ignored. Refusing to write a field that cannot act."); return; }
        if (c->tier == "primary")
        { c->tier.clear(); say ("Tier cleared on '" + c->name + "': the heuristic decides again."); }
        else
        { c->tier = "primary"; say ("'" + c->name + "' pulled up (tier: primary)."); }
        persistSession();
        showTierCard();
    }

    void tierAccept()
    {
        int tiers = 0;
        for (const auto& c : pendingControls) tiers += c.tier.isNotEmpty();
        tierPhase = false;
        summaryText.setVisible (false);
        persistSession();
        say (tiers == 0 ? juce::String ("Exposure accepted as the heuristic built it: no tiers emitted.")
                        : juce::String (tiers) + " tier(s) will be emitted; the rest stay heuristic.");
        advance();
        list.updateContent();
        updateQuestion();
        resized();
    }

    void tierDefer()
    {
        for (int i = 0; i < pendingControls.size() && i < tierSnapshot.size(); ++i)
            pendingControls.getReference (i).tier = tierSnapshot[i];
        tierPhase = false;
        summaryText.setVisible (false);
        persistSession();
        say ("Tier changes from this visit reverted; the heuristic decides. "
             "W on the controls row re-opens the flow.");
        advance();
        list.updateContent();
        updateQuestion();
        resized();
    }
    /** Clear every piece of in-progress state so the wizard can start again on
        the same loaded plugin. Written for Restart, and deliberately explicit
        rather than a memset: the stale-state bug this exists to recover from
        was controlsWiggleTarget surviving a stage change, so a reset that
        quietly missed a field would be the same defect wearing a fix. */
    void resetAll()
    {
        rows.clear();
        controlEntries.clear();
        controlsExcluded.clear();
        controlsPhase = false;
        controlsExpanded = false;
        controlsCursor = 0;
        controlsRowIndex = -1;
        controlsWiggleTarget = -1;      // the field that caused the interleave
        controlsTypedEntry = -1;
        controlsSkippedClaimed = 0;
        controlsSkippedMasked = 0;
        selected = 0;
        tierCursor = 0;
        awaitingCaptureRow = -1;
        bandStep = BandStep::none;
        bandCursor = 0;
        bandWiggleTarget = -1;          // the band-stage twin of the same field
        bandsRowIndex = -1;
        ignoreRows.clear();
        summaryText.setVisible (false);
        list.updateContent();
        updateQuestion();
        resized();
    }

    bool controlsPhase = false;
    bool controlsExpanded = false;
    int  controlsCursor = 0;
    int  controlsRowIndex = -1;
    int  controlsWiggleTarget = -1;
    int  controlsTypedEntry = -1;
    int  controlsSkippedClaimed = 0, controlsSkippedMasked = 0;
    juce::SortedSet<int> controlsExcluded;   // human decisions, survive re-sweeps

    const juce::Array<NamedControl>& controlsForSubmit() const { return pendingControls; }

    void actionControlsBegin()
    {
        controlsRowIndex = selected;
        controlEntries.clear();
        controlsExpanded = false;
        controlsCursor = 0;
        controlsSkippedClaimed = controlsSkippedMasked = 0;

        // What is already spoken for: Tier 1 rows, group members, the mask.
        juce::SortedSet<int> claimed;
        for (const auto& r : rows)
            if (r.state == AssignRow::State::confirmed && r.resolvedIndex >= 0)
                claimed.add (r.resolvedIndex);
        for (const auto& g : acceptedGroups)
            for (const auto& m : g.params)
                claimed.add (m.indices[0]);
        juce::SortedSet<int> masked;
        if (hooks.maskIndices)
            for (int i : hooks.maskIndices()) masked.add (i);

        // Breadcrumbs first: mode findings from THIS machine's earlier
        // sessions, deduped by index upstream. They own their indices.
        juce::SortedSet<int> crumbIdx;
        if (hooks.loadBreadcrumbs)
            if (auto* arr = hooks.loadBreadcrumbs().getArray())
                for (auto& c : *arr)
                {
                    ControlEntry e;
                    e.index = (int) c.getProperty ("index", -1);
                    e.name  = c.getProperty ("param_name", "").toString();
                    e.kind  = "mode";
                    e.provenance = "breadcrumb";
                    if (auto* pts = c.getProperty ("points", juce::var()).getArray())
                        for (auto& pt : *pts)
                        {
                            const auto t = pt.getProperty ("t", "").toString();
                            if (! e.labelTexts.contains (t))
                            {
                                e.labelTexts.add (t);
                                e.labelNorms.add ((double) pt.getProperty ("n", 0.0));
                            }
                        }
                    e.note = "mode: " + e.labelTexts.joinIntoString (" / ").substring (0, 60)
                           + "  (breadcrumb)";
                    if (e.index >= 0 && ! e.labelTexts.isEmpty())
                    { controlEntries.add (e); crumbIdx.add (e.index); }
                }

        // The full-surface sweep. Synchronous and cheap by measurement
        // (0-2 ms per sweep); progress every 32 so a big surface says so.
        const int n = hooks.paramCount ? hooks.paramCount() : 0;
        for (int i = 0; i < n; ++i)
        {
            if (crumbIdx.contains (i)) continue;
            if (claimed.contains (i))  { ++controlsSkippedClaimed; continue; }
            if (masked.contains (i))   { ++controlsSkippedMasked; continue; }
            if ((i & 31) == 0) say ("Sweeping the surface: " + juce::String (i) + "/" + juce::String (n));

            ControlEntry e;
            e.excluded = controlsExcluded.contains (i);   // decisions survive a re-sweep
            e.index = i;
            e.name  = hooks.paramName ? hooks.paramName (i) : juce::String();
            e.sweep = hooks.sweepIndex ? hooks.sweepIndex (i) : SweepOutcome();
            if (e.sweep.ok)
                e.note = juce::String (e.sweep.anchors.size()) + " anchors ("
                       + e.sweep.method
                       + (e.sweep.unitFamily.isNotEmpty() ? ", " + e.sweep.unitFamily : juce::String())
                       + ")" + (e.sweep.identityDisplay ? "  IDENTITY DISPLAY - dials by raw number"
                                                        : juce::String());
            else if (e.sweep.nonNumeric && ! e.sweep.points.isEmpty())
            {
                // A labelled switch discovered by the sweep itself: mode.
                e.kind = "mode";
                for (const auto& pt : e.sweep.points)
                    if (! e.labelTexts.contains (pt.t))
                    { e.labelTexts.add (pt.t); e.labelNorms.add (pt.n); }
                e.note = "mode: " + e.labelTexts.joinIntoString (" / ").substring (0, 60);
            }
            else
            {
                e.unbuildable = true;
                e.note = e.sweep.flat ? "flat both ways - unbuildable (T types anchors)"
                                      : "unbuildable: " + e.sweep.reason.substring (0, 60);
            }
            controlEntries.add (e);
        }

        // Exact-case duplicates: every entry wearing a shared name is flagged.
        for (int i = 0; i < controlEntries.size(); ++i)
            for (int j = i + 1; j < controlEntries.size(); ++j)
                if (controlEntries.getReference (i).name == controlEntries.getReference (j).name)
                {
                    controlEntries.getReference (i).duplicate = true;
                    controlEntries.getReference (j).duplicate = true;
                }

        // VERIFY BEFORE SHOWING. getText anchors and breadcrumb labels are
        // gettext-derived, and the API-2500 run proved getText can lie about
        // set positions while passing every flatness check. One set-then-read
        // spot check per anchored entry; a mismatch earns the honest setread
        // re-sweep. Mode labels are ALWAYS re-read by setting, because a
        // label map that dials the wrong position is worse than none.
        say ("Verifying by set-then-read...");
        for (auto& e : controlEntries)
        {
            if (e.excluded || e.unbuildable) continue;

            if (e.kind == "mode")
            {
                juce::StringArray vTexts;
                juce::Array<double> vNorms;
                for (int i = 0; i < e.labelNorms.size(); ++i)
                {
                    const auto t = hooks.spotCheck ? hooks.spotCheck (e.index, e.labelNorms[i])
                                                   : juce::String();
                    if (t.isNotEmpty() && ! vTexts.contains (t))
                    { vTexts.add (t); vNorms.add (e.labelNorms[i]); }
                }
                if (vTexts.size() >= 2)
                {
                    if (vTexts.strings != e.labelTexts.strings)
                        e.note = "mode: " + vTexts.joinIntoString (" / ").substring (0, 50)
                               + "  (labels re-read by setting: getText lied)";
                    e.labelTexts = vTexts;
                    e.labelNorms = vNorms;
                }
                else
                {
                    e.unbuildable = true;
                    e.note = "labels collapse under set-then-read: not a dialable switch";
                }
                continue;
            }

            if (e.sweep.ok && e.sweep.method == "gettext" && e.sweep.anchors.size() >= 3
                 && hooks.spotCheck)
            {
                const int mid = e.sweep.anchors.size() / 2;
                const float vExpect = e.sweep.anchors[mid][0];
                const auto landed = hooks.spotCheck (e.index, e.sweep.anchors[mid][1]);
                float vGot = 0.0f; bool negInf = false; bool parsed;
                if (e.sweep.unitFamily.isNotEmpty())
                    parsed = echojay::parseDisplayForUnit (landed, e.sweep.unitFamily, vGot, negInf);
                else
                { double d = 0.0; parsed = echojay::parseLeadingFloat (landed, d); vGot = (float) d; }

                float lo = e.sweep.anchors.getFirst()[0], hi = lo;
                for (const auto& a : e.sweep.anchors)
                { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                const float gap = mid + 1 < e.sweep.anchors.size()
                                    ? std::abs (e.sweep.anchors[mid + 1][0] - vExpect)
                                    : std::abs (vExpect - e.sweep.anchors[mid - 1][0]);
                const float tol = juce::jmax (0.02f * (hi - lo), 0.6f * gap);

                if (! parsed || std::abs (vGot - vExpect) > tol)
                {
                    auto redo = hooks.sweepIndexSetread ? hooks.sweepIndexSetread (e.index)
                                                        : SweepOutcome();
                    if (redo.ok)
                    {
                        e.sweep = redo;
                        e.note = juce::String (redo.anchors.size())
                               + " anchors (setread: getText lied at spot check)";
                    }
                    else
                    {
                        e.unbuildable = true;
                        e.note = "getText lied at spot check and setread refused: "
                               + redo.reason.substring (0, 50);
                    }
                }
            }
        }

        controlsPhase = true;
        rowAt (controlsRowIndex).state = AssignRow::State::captured;
        showControlsTable();
    }

    juce::Array<int> visibleControlIndices() const
    {
        juce::Array<int> out;
        for (int i = 0; i < controlEntries.size(); ++i)
            if (controlsExpanded || controlEntries.getReference (i).needsDecision())
                out.add (i);
        return out;
    }

    void showControlsTable()
    {
        const auto vis = visibleControlIndices();
        controlsCursor = juce::jlimit (0, juce::jmax (0, vis.size() - 1), controlsCursor);

        int clean = 0, mode = 0, dup = 0, unb = 0, excl = 0, ident = 0;
        for (const auto& e : controlEntries)
        {
            if (e.excluded) { ++excl; continue; }
            if (e.duplicate) ++dup;
            if (e.unbuildable) ++unb;
            if (e.kind == "mode") ++mode;
            if (e.sweep.identityDisplay) ++ident;
            if (! e.needsDecision()) ++clean;
        }

        juce::String t;
        t << "CONTROLS: " << controlEntries.size() << " candidates ("
          << controlsSkippedClaimed << " claimed by Tier 1/groups, "
          << controlsSkippedMasked << " masked, not controls)\n\n";

        for (int v = 0; v < vis.size(); ++v)
        {
            const auto& e = controlEntries.getReference (vis[v]);
            t << (v == controlsCursor ? " > " : "   ");
            t << e.name << "  [" << e.index << "]";
            if (e.duplicate) t << "  DUPLICATE: unresolvable by name";
            if (e.excluded)  t << "  excluded by mapper";
            t << "  " << e.note;
            if (e.trust != "setread") t << "  " << e.trust;
            t << "\n";
        }

        // The clean majority is a COUNT, not a wall. An empty flagged view
        // says so explicitly, same rule as the exclusion footer.
        if (! controlsExpanded)
        {
            if (vis.isEmpty())
                t << "   (no rows need a decision)\n";
            t << "\n" << clean << " others swept clean, recorded as setread. F shows them.\n";
        }
        else
            t << "\n(full list shown; F returns to flagged-only)\n";

        summaryText.setText (t, juce::dontSendNotification);
        summaryText.setVisible (true);
        promptTitle.setText (juce::String (controlEntries.size()) + " controls named ("
                               + juce::String (mode) + " mode, " + juce::String (dup)
                               + " duplicate, " + juce::String (unb) + " unbuildable, "
                               + juce::String (ident) + " identity)",
                             juce::dontSendNotification);
        question.setText ("SPACE accept - F " + juce::String (controlsExpanded ? "flagged only"
                                                                               : "show all")
                            + " - arrows pick - W verify by touch - N exclude - "
                              "T type anchors - D later",
                          juce::dontSendNotification);
        rebuildControlsAnswers();
        resized();
    }

    void rebuildControlsAnswers()
    {
        answerButtons.clear();
        auto add = [this] (const juce::String& id, const juce::String& text)
        {
            auto* b = answerButtons.add (new juce::TextButton (text));
            addAndMakeVisible (b);
            b->onClick = [this, id] { dispatchAction (id); grabKeyboardFocus(); };
        };
        add ("space",      "SPACE - accept");
        add ("expand",     controlsExpanded ? "F - flagged only" : "F - show all clean");
        add ("wiggle",     "W - verify by touch");
        add ("notpresent", "N - exclude");
        add ("typed",      "T - type anchors");
        add ("defer",      "D - later");
        resized();
    }

    ControlEntry* cursorControl()
    {
        const auto vis = visibleControlIndices();
        return juce::isPositiveAndBelow (controlsCursor, vis.size())
                 ? &controlEntries.getReference (vis[controlsCursor]) : nullptr;
    }

    /** Test seam, and nothing else uses it: cursor to a control by name. */
    bool selectControlByName (const juce::String& nm)
    {
        controlsExpanded = true;
        const auto vis = visibleControlIndices();
        for (int v = 0; v < vis.size(); ++v)
            if (controlEntries.getReference (vis[v]).name == nm)
            { controlsCursor = v; showControlsTable(); return true; }
        return false;
    }

    void actionControlsAccept()
    {
        // A re-sweep rebuilds the staged controls; the human's tiers must
        // SURVIVE it by name (the controlsExcluded precedent -- found live:
        // W after tiering silently emitted zero tiers). A control that no
        // longer exists after the re-sweep takes its tier with it, honestly.
        std::map<juce::String, juce::String> tierByName;
        for (const auto& c : pendingControls)
            if (c.tier.isNotEmpty()) tierByName[c.name] = c.tier;

        pendingControls.clear();
        juce::StringArray dupDone;
        int shipped = 0, modeN = 0;

        for (const auto& e : controlEntries)
        {
            if (e.excluded || e.unbuildable) continue;

            if (e.duplicate)
            {
                if (dupDone.contains (e.name)) continue;
                dupDone.add (e.name);
                NamedControl c;
                c.name = e.name;
                c.duplicate = true;
                for (const auto& o : controlEntries)
                    if (o.name == e.name && ! o.excluded) c.indices.add (o.index);
                pendingControls.add (c);
                continue;
            }

            NamedControl c;
            c.name = e.name;
            c.indices.add (e.index);
            c.trust = e.trust == "human-verified" ? Trust::humanVerified : Trust::setread;
            if (e.kind == "mode")
            {
                c.kind = "mode";
                for (int i = 0; i < e.labelTexts.size(); ++i)
                    c.labels.add ({ e.labelTexts[i], e.labelNorms[i] });
                ++modeN;
            }
            else
            {
                for (const auto& a : e.sweep.anchors)
                    c.anchors.add ({ (double) a[1], (double) a[0] });
                if (! e.sweep.anchors.isEmpty())
                {
                    float lo = e.sweep.anchors.getFirst()[0], hi = lo;
                    for (const auto& a : e.sweep.anchors)
                    { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                    c.rangeLo = lo; c.rangeHi = hi;
                }
                c.unit = e.sweep.unitFamily;
                c.identityDisplay = e.sweep.identityDisplay;
            }
            if (auto it = tierByName.find (c.name); it != tierByName.end())
                c.tier = it->second;
            pendingControls.add (c);
            ++shipped;
        }

        auto& row = rowAt (controlsRowIndex);
        row.state = AssignRow::State::confirmed;
        row.trust = "setread";
        row.mode = deepMode ? "deep" : "fast";
        row.resolvedAt = juce::Time::getCurrentTime().toISO8601 (true);
        row.skipReason = juce::String (shipped) + " named control(s) ("
                       + juce::String (modeN) + " mode, "
                       + juce::String (dupDone.size()) + " duplicate name(s) recorded unresolvable)";
        recordResolution (row, "controls_accept");

        controlsPhase = false;
        say ("CONTROLS: " + row.skipReason);
        persistSession();
        // The tier preview rides the same accept: exception-shaped, SPACE
        // straight through when the heuristic was right.
        beginTierPhase();
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
        // Delegates to THE rule in EjmapAssignment.h. This method and the
        // submit path each had a private copy; the copies drifted (the
        // review's learned the surface-row exclusion, the submit's did not)
        // and the AMEK re-run refused twice. Never reimplement it here.
        return duplicateIndexConflicts (rows);
    }

    bool isSummaryShowing() const { return summaryShowing; }
    bool isAwaitingCategory() const { return awaitingCategory; }
    juce::String progressText() const { return progress.getText(); }
    int sessionSeconds() const { return (int) ((juce::Time::getMillisecondCounter() - startedAt) / 1000); }
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

        auto conflicts = duplicateConflicts();

        // A row claiming N>0 with nothing staged CONTRADICTS itself: refuse
        // with the count, never submit an empty claim.
        //
        // STATE THE OBSERVATION, NEVER THE CAUSE. This block used to say
        // "(session restore lost them)" -- a cause it had not checked. On
        // Cenozoix Compressor (99 params, 0 nameable controls) the truth was
        // agreement, not loss, and the asserted cause was believed: a ledger
        // was backed up and the row re-swept twice on the strength of it. A
        // message that states a diagnosis gets acted on; one that states what
        // it saw can be checked. Every refusal here reports the claim and the
        // count, and stops.
        for (const auto& r : rows)
        {
            if (r.state != AssignRow::State::confirmed) continue;
            // ONLY A CONTRADICTION IS A CONFLICT. An empty store alone is not:
            // a plugin whose surface carries no nameable controls sweeps to
            // `shipped == 0` and the row honestly claims "0 named control(s)".
            // Refusing that refused a correct map forever with no way through,
            // and blamed a restore bug it had not checked for -- measured on
            // Cenozoix Compressor (99 params, 0 nameable), 4 Aug 2026.
            //
            // The claim's own leading count is the discriminator: claims N>0
            // with nothing staged is cargo lost, claims 0 with nothing staged
            // is agreement. Read from skipReason, which is built ~70 lines
            // above from `shipped`; if that text changes, this must change with
            // it.
            if (r.kind == "controls" && pendingControls.isEmpty() && r.skipReason.getIntValue() > 0)
                conflicts.add ("controls row claims '" + r.skipReason
                                 + "' but 0 controls are staged. W re-opens the row to rebuild");
            // BANDS: claim built at ~1664 from acceptedGroups.size(), the same
            // store this reads -- verified, not assumed. Same leading-count
            // discriminator, so the same rule: a claim of 0 with 0 staged is
            // agreement, not a conflict.
            if (r.kind == "bands" && acceptedGroups.isEmpty() && r.skipReason.getIntValue() > 0)
                conflicts.add ("bands row claims '" + r.skipReason
                                 + "' but 0 groups are staged. W re-opens the row");
        }

        juce::String t;
        t << "REVIEW: what submit will write\n\n";
        for (auto& r : rows)
            if (r.state == AssignRow::State::confirmed)
                t << "  " << displayLabel (r) << "  <- [" << r.resolvedIndex << "] "
                  << (hooks.paramName ? hooks.paramName (r.resolvedIndex) : juce::String())
                  << "  (" << r.trust << ", " << r.sweep.method << ")\n";
        if (! pendingControls.isEmpty())
        {
            int dups = 0; for (const auto& c : pendingControls) dups += c.duplicate;
            t << "\n" << pendingControls.size() << " named control(s) will be written"
              << (dups > 0 ? " (" + juce::String (dups) + " duplicate name(s), unresolvable, recorded)"
                           : juce::String()) << "\n";
        }
        if (! acceptedGroups.isEmpty())
            t << "\n" << acceptedGroups.size() << " band group(s) will be written "
                 "(a 250 Hz-class request can only land inside them)\n";
        t << "\n" << modePos << " mode/position finding(s), " << skips << " skip(s) with reasons, "
          << open << " unresolved row(s)" << (open > 0 ? " -> will be recorded as deferred" : "")
          << (ignoresOpen > 0 ? ", " + juce::String (ignoresOpen) + " unreviewed ignore(s)" : juce::String())
          << "\n";

        if (! conflicts.isEmpty())
        {
            t << "\nSUBMIT REFUSED:\n";
            for (const auto& c : conflicts) t << "  " << c << "\n";
            t << "Fix the above (W re-captures or re-opens, D defers), then review again.\n";
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
        for (auto* b : { &prevBtn, &nextBtn, &evidBtn, &bulkBtn, &skipPluginBtn, &parkBtn, &reviewBtn })
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

        if (k == juce::KeyPress::escapeKey)               { dispatchAction ("park"); return true; }
        if (k == juce::KeyPress::spaceKey)                { dispatchAction ("space"); return true; }
        if (c == 'w' || c == 'W' || c == 'r' || c == 'R') { dispatchAction ("wiggle"); return true; }
        if (c == 'n' || c == 'N')                         { dispatchAction ("notpresent", shift); return true; }
        if (c == 'a' || c == 'A')                         { dispatchAction ("noparam", shift); return true; }
        if (c == 'd' || c == 'D')                         { dispatchAction ("defer", shift); return true; }
        if (c == 't' || c == 'T')                         { dispatchAction ("typed"); return true; }
        if (c == 'm' || c == 'M')                         { dispatchAction ("modematerial"); return true; }
        if (c == 'f' || c == 'F')                         { dispatchAction ("expand"); return true; }
        if (c == 'i' || c == 'I')                         { dispatchAction ("bulk"); return true; }
        if (c == 'x' || c == 'X')                         { dispatchAction ("kick"); return true; }
        if (c == 'p' || c == 'P')                         { dispatchAction ("pull"); return true; }
        if (c == 'v' || c == 'V')                         { dispatchAction ("expand"); return true; }
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
        parkBtn.setBounds (strip.removeFromLeft (gw).reduced (1));
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
        if (r.isBandMemberRow())
        {
            // "band LF frequency". The mapper's own label rides here so the
            // row they are answering is the row they meant; the ORDER those
            // labels imply is never trusted (accept derives it from the
            // frequencies).
            juce::String slot = r.semantic == "freq_hz" ? "frequency"
                              : r.semantic == "gain_db" ? "gain"
                              : r.semantic == "q"       ? "Q"
                              : r.semantic == "enable"  ? "enable"
                                                        : r.semantic;
            juce::String t = "band " + r.bandLabel + " " + slot;
            if (r.freqSource == "typed_fixed" && r.typedFreqHz > 0.0)
                t << "  = " << juce::String (r.typedFreqHz, 1) << " Hz fixed";
            return t;
        }
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
        // A CARD WITH NO BACKING ROW MUST NOT DRAW AS IF IT HAS ONE. It kept
        // the previous row's text after the rows were cleared -- "Record mode
        // switch Analog?" with nothing behind it -- which asks the operator to
        // answer a question about a row that does not exist, and every key then
        // refuses because there is nothing to act on. An empty wizard should
        // say it is empty.
        if (rowCount() == 0)
        {
            question.setText ("No rows. The wizard has nothing to ask about: either the "
                              "category produced no dial set for this plugin, or a restart "
                              "cleared the rows without rebuilding them.",
                              juce::dontSendNotification);
            return;
        }

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

        if (r.kind == "bands" && bandPickPending())
        {
            q << "Two controls moved together in lockstep (a linked pair). The plugin "
                 "cannot say which one your hand was on.\n"
              << "Pick it by number - the other is kept as co-moved evidence.";
        }
        else if (r.kind == "bands" && bandStep != BandStep::none && bandStep != BandStep::table)
        {
            q << bandCardPrompt() << " - the card is LIVE.\n"
              << "R discard a wrong grab and re-arm"
              << (bandStep == BandStep::capQ1 ? " - N this band has no Q" : juce::String())
              << " - D leave bands for later";
        }
        else if (r.kind == "controls" && ! r.isResolved() && ! controlsPhase)
        {
            const int n = hooks.paramCount ? hooks.paramCount() : 0;
            q << juce::String (n) << " parameters on this surface. SPACE sweeps every one "
                 "not already claimed and names it as a Tier 2 control the AI can dial "
                 "BY NAME.\n"
              << "Mode findings from earlier sessions join automatically. "
                 "Only rows needing a decision are shown; the clean majority is a count.";
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
        if (bandPickPending()) return "Which control did you touch?";
        auto& r = rowAt (selected);
        if (r.isResolved())
            return displayLabel (r) + ": done (" + r.stateString() + ")";
        if (r.kind == "controls")
            return controlsPhase ? "The controls table"
                 : r.isResolved() ? "Named controls: done"
                                  : "Name the rest of the surface?";
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
            add ("wiggle", r.kind == "controls" ? "W - re-sweep the surface"
                         : r.kind == "bands"    ? "W - redo the bands"
                                                : "W - re-open (re-capture)");
            resized(); return;
        }

        if (bandPickPending())
        {
            for (int i = 0; i < juce::jmin (9, bandGesturePending.indices.size()); ++i)
            {
                const int oneBased = i + 1;
                auto* b = answerButtons.add (new juce::TextButton (
                    juce::String (oneBased) + " - " + bandGesturePending.names[i]
                      + "  [" + juce::String (bandGesturePending.indices[i]) + "]"));
                addAndMakeVisible (b);
                b->onClick = [this, oneBased] { bandPickCandidate (oneBased); grabKeyboardFocus(); };
            }
            resized(); return;
        }
        if (r.kind == "controls" && ! r.isResolved() && ! controlsPhase)
        {
            add ("space", "SPACE - sweep and review");
            add ("defer", "D - later");
            resized(); return;
        }
        if (r.kind == "bands" && ! r.isResolved() && bandStep == BandStep::none)
        {
            if (manualBands && anyManualBandRows())
                add ("space", "SPACE - accept the bands you entered");
            else
                add ("space", "SPACE - begin mapping bands");
            add ("typed", manualBands && anyManualBandRows() ? "T - re-enter by hand"
                                                             : "T - enter bands by hand");
            add ("defer", "D - later");
            resized(); return;
        }
        if (r.isBandMemberRow() && ! r.isResolved())
        {
            add ("wiggle", "W - touch it on the GUI");
            if (r.semantic == "freq_hz")
                add ("notpresent", "N - no frequency control (fixed band)");
            else
                add ("notpresent", "N - this band has no " + r.semantic);
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
            if (&r != &winner && r.slotKey() == winner.slotKey() && ! r.isResolved())
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

        // THE CARGO, not just the claims. A confirmed controls row that
        // restores without its controls submits controls:{} while the console
        // says "35 named" -- the spiff gate failure. Same for band groups.
        juce::Array<juce::var> cv;
        for (const auto& c : pendingControls) cv.add (c.toVar());
        o->setProperty ("pending_controls", juce::var (cv));
        {
            auto* lo = new juce::DynamicObject();
            for (auto& kv : lockstepObserved)
            {
                auto* e = new juce::DynamicObject();
                e->setProperty ("of", kv.second);
                auto it = lockstepSource.find (kv.first);
                e->setProperty ("by", it != lockstepSource.end() ? it->second : "human_pick");
                lo->setProperty (juce::String (kv.first), juce::var (e));
            }
            o->setProperty ("lockstep_observed", juce::var (lo));
        }
        juce::Array<juce::var> gv;
        for (const auto& g : acceptedGroups) gv.add (g.toVar());
        o->setProperty ("accepted_groups", juce::var (gv));

        // THE BAND DIAGNOSTIC (queue item 1). "0 bands inferred from your 3
        // touches" reached the screen and nothing else: the session file held
        // accepted_groups: 0, which is indistinguishable from never having
        // attempted the inference. strideNote already names the parameter the
        // inference failed on; it simply was not written down. Recorded
        // whenever an inference has been ATTEMPTED, so that an absent block
        // means not attempted and a present one with zero bands means
        // attempted and found nothing -- which are different facts.
        if (bandPlan.strideNote.isNotEmpty() || ! bandCaptured.isEmpty()
             || ! bandPlan.bands.isEmpty())
        {
            auto* bd = new juce::DynamicObject();
            bd->setProperty ("stride_note", bandPlan.strideNote);
            bd->setProperty ("axis", bandPlan.axis);
            bd->setProperty ("family", bandPlan.family);
            bd->setProperty ("bands_inferred", bandPlan.bands.size());
            bd->setProperty ("stride_verified",
                             bandPlan.strideNote.contains ("verifying the rest"));
            juce::Array<juce::var> touched;
            for (const auto& m : bandCaptured)
            {
                auto* t = new juce::DynamicObject();
                t->setProperty ("semantic", m.semantic);
                t->setProperty ("index", m.index);
                // The name AS THE PLUGIN REPORTS IT: the whole point of the
                // record is to answer the naming question by reading rather
                // than by recalling the product.
                t->setProperty ("param_name", m.name);
                touched.add (juce::var (t));
            }
            if (bandSecondFreqIdx >= 0)
            {
                auto* t = new juce::DynamicObject();
                t->setProperty ("semantic", "freq_hz");
                t->setProperty ("index", bandSecondFreqIdx);
                t->setProperty ("param_name", hooks.paramName ? hooks.paramName (bandSecondFreqIdx)
                                                              : juce::String());
                t->setProperty ("role", "highest_band_freq");
                touched.add (juce::var (t));
            }
            bd->setProperty ("touched", juce::var (touched));
            o->setProperty ("band_diagnostic", juce::var (bd));
        }
        juce::Array<juce::var> xv;
        for (int i = 0; i < controlsExcluded.size(); ++i) xv.add (controlsExcluded[i]);
        o->setProperty ("controls_excluded", juce::var (xv));

        sessionFile().replaceWithText (juce::JSON::toString (juce::var (o), false));
    }

    static ParamMapping paramMappingFromVar (const juce::String& semantic, const juce::var& v)
    {
        ParamMapping m;
        m.semantic = semantic;
        m.kind = v.getProperty ("kind", semantic).toString();
        m.paramName = v.getProperty ("name", "").toString();
        if (auto* ia = v.getProperty ("indices", juce::var()).getArray())
            for (auto& i : *ia) m.indices.add ((int) i);
        else
            m.indices.add ((int) v.getProperty ("index", -1));
        if (auto* aa = v.getProperty ("anchors", juce::var()).getArray())
            for (auto& pv : *aa)
                if (auto* pr = pv.getArray(); pr != nullptr && pr->size() >= 2)
                    m.anchors.add ({ (double) (*pr)[1], (double) (*pr)[0] });   // [value, norm] on disk
        m.anchorsReversed = (bool) v.getProperty ("anchors_reversed", false);
        const auto tr = v.getProperty ("trust", "").toString();
        m.trust = tr == "human-verified" ? Trust::humanVerified
                : tr == "llm-classified" ? Trust::llmClassified : Trust::setread;
        m.method = v.getProperty ("method", "").toString() == "setread" ? AnchorMethod::setread
                 : v.getProperty ("method", "").toString() == "human-typed" ? AnchorMethod::humanTyped
                 : AnchorMethod::gettext;
        return m;
    }

    static NamedControl namedControlFromVar (const juce::var& v)
    {
        NamedControl c;
        c.name = v.getProperty ("name", "").toString();
        if (auto* ia = v.getProperty ("indices", juce::var()).getArray())
            for (auto& i : *ia) c.indices.add ((int) i);
        else
            c.indices.add ((int) v.getProperty ("index", -1));
        c.kind = v.getProperty ("kind", "anchored").toString();
        if (auto* ra = v.getProperty ("range", juce::var()).getArray(); ra != nullptr && ra->size() >= 2)
        { c.rangeLo = (double) (*ra)[0]; c.rangeHi = (double) (*ra)[1]; }
        c.unit = v.getProperty ("unit", juce::var()).toString();
        if (auto* lo = v.getProperty ("labels", juce::var()).getDynamicObject())
            for (auto& kv : lo->getProperties())
                c.labels.add ({ kv.name.toString(), (double) kv.value });
        if (auto* aa = v.getProperty ("anchors", juce::var()).getArray())
            for (auto& pv : *aa)
                if (auto* pr = pv.getArray(); pr != nullptr && pr->size() >= 2)
                    c.anchors.add ({ (double) (*pr)[1], (double) (*pr)[0] });
        c.identityDisplay = (bool) v.getProperty ("identity_display", false);
        c.trust = v.getProperty ("trust", "").toString() == "human-verified"
                    ? Trust::humanVerified : Trust::setread;
        c.duplicate = (bool) v.getProperty ("duplicate", false);
        if (! v.getProperty ("lockstep_of", juce::var()).isVoid())
        {
            c.lockstepOf = (int) v.getProperty ("lockstep_of", -1);
            c.lockstepBy = v.getProperty ("lockstep_by", "").toString();
        }
        c.tier = v.getProperty ("tier", "").toString();
        return c;
    }

    void restoreSession()
    {
        auto f = sessionFile();
        if (! f.existsAsFile()) return;
        auto v = juce::JSON::parse (f.loadFileAsString());
        if (v.getProperty ("fp", "").toString() != fp) return;

        // A PARKED MANUAL-BAND SESSION HAS ROWS THE CHECKLIST DOES NOT BUILD.
        // Every other row exists before restore runs, so restore only had to
        // fill one in; band-member rows are synthesised by the mapper's count
        // answer and exist nowhere else. Recreate them from the file FIRST,
        // or parking a hand-entered EQ silently discards every band.
        if (auto* fileRows = v.getProperty ("rows", juce::var()).getArray())
        {
            int at = -1;
            for (int i = 0; i < rows.size(); ++i)
                if (rows.getReference (i).kind == "bands") { at = i + 1; break; }
            if (at < 0) at = rows.size();
            for (auto& rv : *fileRows)
            {
                const int ord = (int) rv.getProperty ("band_ordinal", -1);
                if (ord <= 0) continue;
                const auto sem = rv.getProperty ("semantic", "").toString();
                bool have = false;
                for (const auto& r : rows)
                    if (r.isBandMemberRow() && r.bandOrdinal == ord && r.semantic == sem)
                        have = true;
                if (have) continue;
                AssignRow nr;
                nr.kind = "band_member";
                nr.semantic = sem;
                nr.proposalSource = "none";
                nr.bandOrdinal = ord;
                nr.bandLabel = rv.getProperty ("band_label", juce::String (ord)).toString();
                rows.insert (juce::jmin (at++, rows.size()), nr);
                manualBands = true;
            }
        }

        auto restore = [] (juce::Array<AssignRow>& dst, const juce::var& src)
        {
            auto* arr = src.getArray();
            if (arr == nullptr) return;
            for (auto& rv : *arr)
            {
                const auto sem  = rv.getProperty ("semantic", "").toString();
                const int  pidx = (int) rv.getProperty ("proposed_index", -1);
                const int  ord  = (int) rv.getProperty ("band_ordinal", -1);
                const auto key  = ord > 0 ? "band" + juce::String (ord) + ":" + sem : sem;
                for (auto& r : dst)
                {
                    // Band rows identify by band+semantic; everything else by
                    // semantic and proposal, as before. Four bands all carry
                    // freq_hz and they are four different questions.
                    if (r.slotKey() != key) continue;
                    if (ord <= 0 && r.proposedIndex != pidx) continue;
                    const auto st = rv.getProperty ("state", "").toString();
                    if (st == "confirmed")            r.state = AssignRow::State::confirmed;
                    else if (st == "not_present")     r.state = AssignRow::State::skipNotPresent;
                    else if (st == "not_automatable") r.state = AssignRow::State::skipNotAutomatable;
                    else if (st == "deferred")        r.state = AssignRow::State::skipDeferred;
                    // mode_material IS a resolution -- the control exists, is
                    // discrete, and was recorded with its labels for Tier 2.
                    // It was missing from this ladder, so every restore hit the
                    // `break` below and silently dropped the row back to
                    // unresolved. Measured 4 Aug 2026: three of eighteen maps
                    // refused re-submission with "unresolved rows need the
                    // wizard", and each had exactly one mode_material row --
                    // API-2500 (s), Aphex Vintage Exciter (m), DPR-402 (m) --
                    // while their siblings without one restored cleanly.
                    //
                    // It costs more than a re-submission: PARKING a plugin
                    // whose knee was recorded as mode/position material and
                    // coming back to it loses that finding, and the row then
                    // claims to be unresolved rather than saying anything was
                    // dropped.
                    else if (st == "mode_material")   r.state = AssignRow::State::modeMaterial;
                    else break;
                    r.resolvedIndex   = (int) rv.getProperty ("resolved_index", -1);
                    r.proposalMismatch = (bool) rv.getProperty ("proposal_mismatch", false);
                    r.corroboration   = rv.getProperty ("corroboration", "").toString();
                    r.mode            = rv.getProperty ("mode", "").toString();
                    r.trust           = rv.getProperty ("trust", "").toString();
                    r.skipReason      = rv.getProperty ("skip_reason", "").toString();
                    r.resolvedAt      = rv.getProperty ("resolved_at", "").toString();
                    r.typedReason     = rv.getProperty ("typed_reason", "").toString();
                    r.freqSource      = rv.getProperty ("freq_source", "").toString();
                    r.typedFreqHz     = (double) rv.getProperty ("typed_freq_hz", 0.0);
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

        pendingControls.clear();
        if (auto* lo = v.getProperty ("lockstep_observed", juce::var()).getDynamicObject())
            for (auto& kv : lo->getProperties())
            {
                lockstepObserved[kv.name.toString().getIntValue()] = (int) kv.value.getProperty ("of", -1);
                lockstepSource[kv.name.toString().getIntValue()]
                    = kv.value.getProperty ("by", "human_pick").toString();
            }
        if (auto* ca = v.getProperty ("pending_controls", juce::var()).getArray())
            for (auto& cvr : *ca)
                pendingControls.add (namedControlFromVar (cvr));
        controlsExcluded.clear();
        if (auto* xa = v.getProperty ("controls_excluded", juce::var()).getArray())
            for (auto& xi : *xa) controlsExcluded.add ((int) xi);
        acceptedGroups.clear();
        if (auto* ga = v.getProperty ("accepted_groups", juce::var()).getArray())
            for (auto& gvr : *ga)
            {
                GroupSpec g;
                g.family  = gvr.getProperty ("family", "").toString();
                g.n       = (int) gvr.getProperty ("n", 0);
                g.primary = (bool) gvr.getProperty ("primary", false);
                if (auto* fr = gvr.getProperty ("freq_range", juce::var()).getArray();
                    fr != nullptr && fr->size() >= 2)
                { g.freqLo = (double) (*fr)[0]; g.freqHi = (double) (*fr)[1]; }
                if (auto* po = gvr.getProperty ("params", juce::var()).getDynamicObject())
                    for (auto& kv : po->getProperties())
                        g.params.add (paramMappingFromVar (kv.name.toString(), kv.value));
                acceptedGroups.add (g);
            }

        say ("Restored assignment session from " + f.getFileName()
               + (pendingControls.isEmpty() ? juce::String()
                    : " (" + juce::String (pendingControls.size()) + " controls)")
               + (acceptedGroups.isEmpty() ? juce::String()
                    : " (" + juce::String (acceptedGroups.size()) + " groups)"));
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
    juce::TextButton prevBtn, nextBtn, evidBtn, bulkBtn, skipPluginBtn, parkBtn, reviewBtn;
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
