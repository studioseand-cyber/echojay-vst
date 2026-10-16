/*
    EedDeviceProcessor.h  —  the base every EchoJay built-in device is built on
    (BUILTIN_SUITE_PLAN.md §3).

    It carries the two things that are identical for all 19 devices, so that a new
    device only writes its DSP and its knobs:

    1. The universal dialable apply path. `settings_structured` arrives from the
       server in one of two shapes; this base owns the flat one:

           settings_structured: { "params": { "level_db": -3, "pan": 0.5 } }

       Ids are canonical and values are in real-world units. MERGE SEMANTICS: a
       param that is absent is left alone, which is what lets "make it punchier"
       send only attack_ms/release_ms without restating the rest of the device.

       Structured devices (the EQ's eq_bands, a future 4-band comp's comp_bands)
       override applyStructured to add their array form and delegate here for
       `params` — so BOTH shapes reach a device through one funnel and ChainHost
       never needs to know which kind of device it is holding.

    2. State. get/setStateInformation are written once here in terms of the
       schema, so a device gets save/restore for free and cannot forget a knob:
       whatever it publishes is what persists. Restore is a FULL REPLACE (every
       param reset to its default first) so a state file written before a param
       existed loads as that param's default rather than inheriting whatever the
       previous instance happened to have.

    A device therefore implements: paramSchema(), setParamValue/getParamValue,
    prepareToPlay/processBlock, getName() and createEditor(). Nothing else.
*/

#pragma once

#include <JuceHeader.h>
#include "EedParamSchema.h"

class EedDeviceProcessor : public juce::AudioProcessor
{
public:
    // ---- who is writing ---------------------------------------------------
    // THE INTENT BEHIND A WRITE, carried explicitly because ONE function
    // serves three of them. applyParams is the only place a built-in device's
    // values change, so the do-not-dial guard has to live there; but session
    // restore and state migration reach that same function through
    // setStateInformation and are not EchoJay dialling. A guard that could not
    // tell them apart reset every built-in to its schema defaults on reload,
    // because setStateInformation resets to defaults BEFORE it applies and
    // only the apply was blocked.
    //
    // NO DEFAULT ARGUMENT, deliberately. A call site added without thinking
    // must fail to COMPILE rather than silently pick a meaning. That is the
    // same lesson WetSource taught by defaulting to Assistant: a defaulted
    // source does not announce itself, it just refuses at runtime.
    enum class ParamSource
    {
        Assistant,   // EchoJay dialling a value. Blocked under do-not-dial.
        User,        // the hand, from a device's own editor. Never blocked.
        Restore,     // the user's own saved session coming back. Never blocked.
        Migration    // state carried between processors. Never blocked.
    };

    EedDeviceProcessor();
    ~EedDeviceProcessor() override = default;

    // ---- the dialable contract (each device publishes its own) ------------
    virtual const echojay::ParamSchema& paramSchema() const = 0;

    // Map one canonical id onto the device's knob. The value arrives ALREADY
    // clamped to the schema's range, so an implementation never re-clamps.
    // Return false for an id the device publishes but does not (yet) implement —
    // that is reported as skipped rather than silently swallowed.
    virtual bool   setParamValue (const juce::String& id, double value) = 0;
    virtual double getParamValue (const juce::String& id) const = 0;

    // ---- AI apply ---------------------------------------------------------
    // THE entry point for a move. Returns a human-readable summary, or an EMPTY
    // string when the value carried nothing this device understands — that is how
    // a caller distinguishes "applied nothing on purpose" from "this wasn't for
    // me". appliedOut/skippedOut report what actually landed.
    virtual juce::String applyStructured (const juce::var& structured,
                                          ParamSource src,
                                          int* appliedOut = nullptr,
                                          int* skippedOut = nullptr);

    // The flat `params` map itself. Public so a structured device's override can
    // delegate to it after handling its own array form.
    juce::String applyParams (const juce::var& paramsObject,
                              ParamSource src,
                              int* appliedOut = nullptr,
                              int* skippedOut = nullptr);

    // Every param at its schema default. Used by restore (full replace) and by
    // a device wanting a genuine "reset".
    // Virtual so a device can guard side effects that only make sense for a
    // USER move. EchoJay Pitch flips key_source to manual when key_root or
    // scale is set by hand; a defaults write is not a hand.
    virtual void resetParamsToDefaults();

    // Current values as a `params` object — the round-trip counterpart of
    // applyParams, for state and for showing the AI what the device is set to.
    juce::var currentParamsVar() const;

    // ---- bypass -----------------------------------------------------------
    // Deliberately NOT a schema param: every device has it, the host chain
    // already exposes it per slot, and advertising it per device would invite
    // the model to bypass a device instead of dialling it.
    //
    // Virtual because a device whose engine carries its own bypass (the EQ) has
    // to keep the two in step.
    virtual void setBypassed (bool b) { bypassed_.store (b); }
    bool isBypassed() const { return bypassed_.load(); }

    // ---- state (written once, in terms of the schema) ---------------------
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // True while setStateInformation is applying a loaded state's params.
    // A device that treats a LIVE param write as a user action (e.g. the
    // pitch reference flipping to manual) must not treat a state load the
    // same way - a load replays values, it is nobody touching a control.
    bool applyingState() const noexcept { return applyingState_; }

protected:
    // Called once after a loaded state's params have all been applied, so a
    // device can run migrations that need the COMPLETE state (e.g. the
    // 29 Aug 2026 laundered-reference revert). Default: nothing.
    virtual void onStateApplied() {}

    // ---- WIRE LITERALS THAT ARE NOT SCHEMA CHOICES (21t-i re-cut, 27 Sep 2026 ruling) -----------------------
    // The server's vocabulary for a control can be a LADDER whose rungs are not one-to-one with a device's own
    // choice list: the tuner ladder is natural -> balanced -> hard -> snap, with "tuned" kept as an accepted alias
    // and "snap" meaning "the hardest this device's map allows". A device that owns such a ladder resolves the
    // literal here; -1 means "not mine", and the caller then reports the miss exactly as it always has.
    //
    // whyOut is for a literal that was ACCEPTED BY FALLBACK (an unknown rung built as the hardest one): the note
    // travels to the chat log, because a wire value that silently became a different setting is the class of
    // failure this whole file exists to prevent.
    virtual int aliasChoiceIndex (const juce::String& /*canonicalId*/, const juce::String& /*label*/,
                                  juce::String& /*whyOut*/) const { return -1; }

    // ---- 21t-j (28 Sep 2026 ruling): A MODE WRITES OTHER CONTROLS, SO IT GOES FIRST ------------------------
    // A device whose mode selection writes its other params names that key here. Everything else in the payload is
    // applied AFTER it, so a turn carrying correction_mode together with flat flex/humanize keys lands the flats on
    // top of the mode rather than under it - which is what "even harder" did: the mode wrote flex 25 / humanize 30
    // and the flats' 0 / 0 arrived first, so the reported summary and the knobs disagreed.
    virtual juce::String modeKeyId() const { return {}; }

    /** THE LANDED STATE, in the device's own words, read back from the device. The chain line and the settings card
        report THIS, never the prose a mode wrote when it was selected: after an edit dials a mode, the derived
        values are what the knobs hold and the wording of the build is a fortnight out of date. Default: every
        dialable id and its value. */
    virtual juce::String readbackSummary() const;

public:
    /** The readback, for a HOST that has to report the slot's landed state (ChainHost writes it into the slot's
        settings text). Public because the caller is not the device and not a subclass: it is the rack. */
    juce::String readbackSummaryForHost() const { return readbackSummary(); }

    // ---- FIX 3 (Sean, 14 Oct): GAIN REDUCTION, FOR THE HOST --------------------------------------------
    // check(gr) gated on ChainHost's per-slot SlotPicture, which was READ IN THREE PLACES AND WRITTEN IN
    // NONE - so it could never succeed, and the refusal blamed the audio: "No audio has been heard for gain
    // reduction on slot 1 yet", two seconds after the plugin's own playback wait had returned heard=8.0s.
    // Sean spent nine rounds on it.
    //
    // The figure existed all along - EedCompressorProcessor::gainReductionDb() and its siblings, which the
    // device EDITORS have been painting for months - it simply had no route to the rack, because it is not
    // on this shared base. These two are that route. The default is "does not report", not "reports 0 dB":
    // a device with no dynamics stage must read as ABSENT, since 0.0 dB of reduction is a real measurement
    // and an EQ claiming it would be a lie in the same family as the one being fixed.
    /** ---- SEAN'S #4, 16 Oct 2026: AN UNKNOWN PARAMETER IS NOT A ZERO ---------------------------
        `getParamValue` returns a double and has no way to say "I have never heard of that id", so a
        device that did not know a name returned 0.0 - and the agent's read-back then compared 0.0 with
        what it asked for and reported "not applied". A silent 0.0 is the -200 sentinel in a new place.

        DEFAULTS TO TRUE so every existing device behaves exactly as it did; a device that can answer
        honestly overrides it. The EQ does, because its bands were the ones being misread. */
    virtual bool  knowsParam (const juce::String& id) const { juce::ignoreUnused (id); return true; }
    /** ---- SEAN'S #4 (16 Oct 2026): AN ID THIS DEVICE OWNS BUT ITS SCHEMA DOES NOT PUBLISH --------
        The schema is the gate on every write, and it is right to be: a device silently absorbing
        "thrshold_db" would look like it worked and sound like it did nothing. But the EQ's BANDS are
        not in its schema - six bands times eleven leaves would be sixty-six entries in the parameter
        list the add-menu reads - and they are precisely what the agent dials. `band1.gain_db` resolved
        no spec, counted as unknown, and never reached setParamValue: row E1 read the bands back at
        1000 Hz / 0.0 dB after a turn that asked for 240 Hz / -2.5 dB.

        DEFAULTS TO FALSE, DELIBERATELY, and it is NOT knowsParam. knowsParam answers "can I READ this
        id", and it defaults to TRUE so every existing device's read-back behaves exactly as it did.
        This answers a different and far more dangerous question - "may a write bypass my schema?" - so
        it defaults to no, and only a device that has a vocabulary outside its schema says yes. One
        shared virtual for both would have opened every device's write path on the strength of a
        default chosen for reads. */
    virtual bool  ownsUnpublishedParam (const juce::String& id) const { juce::ignoreUnused (id); return false; }
    virtual bool  reportsGainReduction() const { return false; }
    virtual float gainReductionDbForHost() const { return 0.0f; }
    // ---- THE LISTEN WINDOW (Sean, 15 Oct 2026) -------------------------------------------------
    //  A momentary sample is not a measurement of a time-varying quantity, and the agent was reading
    //  exactly one, at the worst possible instant. These three carry what the window actually saw.
    //  `grWindowFramesForHost() == 0` means NOTHING WAS MEASURED - which a caller must report as such
    //  and never as 0 dB of reduction, for the same reason the default above is "does not report".
    /** THE LOUDEST HITS (95th percentile of the active frames) - the figure to report, by Sean's
        ruling of 15 Oct 2026. The absolute max below is ONE transient instant and is kept for spike
        warnings only; quoting it reads as a number nobody can see on a meter. */
    /** THE HELD FIGURE the GR meter draws (deepest of the last ~2 s). Sean's ruling of 15 Oct 2026:
        the slot tile, the device's own meter and the agent all read this one value, so they cannot
        disagree by sampling a moving number at different instants (his tile read 0.4 while the meter
        read 1.2). */
    virtual float gainReductionHeldDbForHost() const { return 0.0f; }
    virtual float gainReductionLoudestDbForHost() const { return 0.0f; }
    virtual float gainReductionPeakDbForHost()    const { return 0.0f; }
    virtual float gainReductionTypicalDbForHost() const { return 0.0f; }
    virtual juce::uint64 grWindowFramesForHost()  const { return 0; }
    /** Start a new listen window. Called when the agent begins a listen, and by the tally reset. */
    virtual void  resetGrWindowForHost() {}

private:
    bool applyingState_ = false;

public:

    // ---- AudioProcessor boilerplate shared by every built-in --------------
    bool hasEditor() const override                   { return true; }
    bool acceptsMidi() const override                 { return false; }
    bool producesMidi() const override                { return false; }
    bool isMidiEffect() const override                { return false; }
    double getTailLengthSeconds() const override      { return 0.0; }

    int  getNumPrograms() override                    { return 1; }
    int  getCurrentProgram() override                 { return 0; }
    void setCurrentProgram (int) override             {}
    const juce::String getProgramName (int) override  { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void releaseResources() override                  {}

    // Mono or stereo, in == out. The chain runs everything at 2x2, but a device
    // that refuses mono cannot be auditioned standalone.
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

protected:
    // Read a juce::var as a number, tolerating the shapes JSON actually arrives
    // in. Returns false when the value is not usable as a number at all.
    static bool numberFromVar (const juce::var& v, double& out);

    std::atomic<bool> bypassed_ { false };

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDeviceProcessor)
};
