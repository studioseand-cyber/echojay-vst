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
                                          int* appliedOut = nullptr,
                                          int* skippedOut = nullptr);

    // The flat `params` map itself. Public so a structured device's override can
    // delegate to it after handling its own array form.
    juce::String applyParams (const juce::var& paramsObject,
                              int* appliedOut = nullptr,
                              int* skippedOut = nullptr);

    // Every param at its schema default. Used by restore (full replace) and by
    // a device wanting a genuine "reset".
    void resetParamsToDefaults();

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
