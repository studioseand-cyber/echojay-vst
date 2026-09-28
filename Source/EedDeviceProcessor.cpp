/*
    EedDeviceProcessor.cpp  —  see EedDeviceProcessor.h.
*/

#include "EJDialWrites.h"
#include "EedDeviceProcessor.h"

EedDeviceProcessor::EedDeviceProcessor()
    : juce::AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

bool EedDeviceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;   // in must match out
}

// ---------------------------------------------------------------------------
// value coercion
// ---------------------------------------------------------------------------
bool EedDeviceProcessor::numberFromVar (const juce::var& v, double& out)
{
    if (v.isDouble() || v.isInt() || v.isInt64())
    {
        out = (double) v;
        return true;
    }

    // A JSON boolean is the natural way to send a switch, so accept it as 1/0
    // rather than making every boolean param arrive as a number.
    if (v.isBool())
    {
        out = ((bool) v) ? 1.0 : 0.0;
        return true;
    }

    // Strings happen: a model emitting "-3" or "-3 dB" or "on" is expressing a
    // perfectly clear intent, and rejecting it would turn a working move into a
    // silent no-op for a formatting reason the user cannot see.
    if (v.isString())
    {
        const auto s = v.toString().trim().toLowerCase();
        if (s.isEmpty()) return false;

        if (s == "on"  || s == "true"  || s == "yes") { out = 1.0; return true; }
        if (s == "off" || s == "false" || s == "no")  { out = 0.0; return true; }

        // Keep only what can form a number, so "-3 dB" reads as -3. A string
        // with no digits at all ("auto") is NOT a number and must be rejected
        // rather than silently becoming 0.
        const auto digits = s.retainCharacters ("0123456789");
        if (digits.isEmpty()) return false;

        out = s.retainCharacters ("0123456789.-+eE").getDoubleValue();
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------
juce::String EedDeviceProcessor::applyStructured (const juce::var& structured,
                                                  ParamSource src,
                                                  int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    // A bare ARRAY belongs to a structured device (eq_bands). A flat device has
    // no meaning for one, and guessing would be worse than declining.
    if (! structured.isObject()) return {};
    if (! structured.hasProperty ("params")) return {};

    return applyParams (structured.getProperty ("params", juce::var()),
                        src, appliedOut, skippedOut);
}

juce::String EedDeviceProcessor::applyParams (const juce::var& paramsObject,
                                              ParamSource src,
                                              int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    // ===== DO NOT DIAL (5 Sep 2026) =====
    // THE PATH A SINGLE GUARD MISSES. A built-in device writes its own state
    // through setParamValue and never touches a juce::AudioProcessorParameter,
    // so the guard in EchoJayParamApply.h does not cover it. Guarding only
    // there would leave every third-party plugin correctly refusing to move
    // while EchoJay's own EQ dialled itself, which is a worse product than the
    // mode not existing.
    //
    // Returns empty, which the caller (ChainHost::applyStructuredIfReady)
    // already treats as "the device placed nothing", so the settings still
    // reach the card by the normal route and nothing here has to know about
    // cards.
    //
    // ASSISTANT ONLY (6 Sep 2026). This function is not the assistant's path,
    // it is EVERY path: setStateInformation restores a saved session through
    // it and a same-plugin replace migrates state through it. Guarding it
    // without asking who was writing did not merely block a reload, it
    // DESTROYED it, because setStateInformation resets every param to its
    // schema default first and only the apply that puts the saved values back
    // was refused. The device came back at defaults and the state blob that
    // held the user's values had already been consumed. See ParamSource.
    if (src == ParamSource::Assistant && echojay::dialWritesBlocked()) return {};

    auto* obj = paramsObject.getDynamicObject();
    if (obj == nullptr) return {};

    const auto& schema = paramSchema();

    juce::StringArray notes;      // what landed, for the chat log
    juce::StringArray unknown;    // ids this device does not publish
    int applied = 0, skipped = 0;

    // 21t-j: THE MODE FIRST. Two passes over the same object: the key the device names as its mode (if the payload
    // carries it), then everything else. One ordering, so a mode and the flats it writes cannot race by JSON order.
    juce::Array<juce::Identifier> order;
    {
        const auto modeId = juce::String (paramSchema().find (modeKeyId().toStdString()) != nullptr
                                              ? modeKeyId() : juce::String());
        if (modeId.isNotEmpty())
            for (const auto& prop : obj->getProperties())
                if (echojay::ParamSchema::normalizeId (prop.name.toString().toStdString())
                    == echojay::ParamSchema::normalizeId (modeId.toStdString()))
                    order.add (prop.name);
        for (const auto& prop : obj->getProperties())
            if (! order.contains (prop.name)) order.add (prop.name);
    }
    for (const auto& propName : order)
    {
        struct { juce::Identifier name; juce::var value; } prop { propName, obj->getProperty (propName) };
        const juce::String id = prop.name.toString();

        // The schema is the gate. An id outside it is reported, never guessed
        // at: a device silently absorbing "thrshold_db" would look like it
        // worked and sound like it did nothing.
        const auto* spec = schema.find (id.toStdString());
        if (spec == nullptr)
        {
            ++skipped;
            unknown.add (id);
            continue;
        }

        double raw = 0.0;
        bool   have = false;

        // A choice param is dialled BY NAME. "tube" carries no digits, so
        // numberFromVar would reject it and the move would vanish; resolving the
        // label first is what makes a selector as settable as a dial. A numeric
        // index still works, because this falls through when the label misses.
        juce::String aliasWhy;
        if (! spec->choices.empty() && prop.value.isString())
        {
            // 21t-i re-cut: A LADDER THE DEVICE OWNS TAKES PRECEDENCE OVER ITS OWN CHOICE LIST - but only on an
            // ASSISTANT write, which is the wire. The tuner ladder is the case: "snap" is a rung the choice list
            // does not spell at all, and "tuned" IS in the list as a softer row while the wire's "tuned" is now an
            // accepted alias for hard. Consulting the list first resolved "tuned" to that softer row and the alias
            // never ran. A human picking "tuned" in the editor still gets the row: the editor writes the index.
            if (src == ParamSource::Assistant)
            {
                const int a = aliasChoiceIndex (juce::String (spec->id), prop.value.toString(), aliasWhy);
                if (a >= 0) { raw = (double) a; have = true; }
            }
            if (! have)
            {
                const int idx = spec->indexOfChoice (prop.value.toString().toStdString());
                if (idx >= 0) { raw = (double) idx; have = true; }
            }
            if (! have)
            {
                // ...and a rung the list cannot spell still resolves on any other source, so a state or a user
                // write carrying "snap" is not silently skipped either.
                const int a = aliasChoiceIndex (juce::String (spec->id), prop.value.toString(), aliasWhy);
                if (a >= 0) { raw = (double) a; have = true; }
            }
        }

        if (! have && ! numberFromVar (prop.value, raw))
        {
            ++skipped;
            unknown.add (id + " (not a number)");
            continue;
        }

        const double v = spec->clamp (raw);

        // Set through the CANONICAL id, never the raw spelling. ParamSchema::find
        // is deliberately tolerant — "centerFreq", "center-freq" and "center_freq"
        // are one param — but a device's setParamValue compares against its own
        // literal id constants. Passing the raw spelling through would resolve the
        // spec and then fail to set it: the move lands in the schema, reports
        // "not implemented", and the knob never turns. That is exactly the silent
        // no-op the tolerant matching exists to prevent, so the tolerance has to
        // survive all the way to the device.
        const juce::String canonicalId (spec->id);

        if (! setParamValue (canonicalId, v))
        {
            ++skipped;
            unknown.add (id + " (not implemented)");
            continue;
        }

        ++applied;

        // Report the value that LANDED, not the one that was asked for, so a
        // clamped move reads honestly in the chat log.
        juce::String note = spec->id + " ";
        // THE CHAIN OF THREE IS ONE CHAIN. The first cut of the alias note spliced an `if` between the choices
        // branch and its `else if`, which detached the number branch and appended the raw value after the LABEL:
        // "correction_mode balanced1" went out on [CURRENT CHAIN], where the server reads it. The alias note is
        // appended AFTER the chain, which is where an annotation belongs.
        if (! spec->choices.empty())
            note += juce::String (spec->choiceLabel (v));
        else if (spec->boolean)
            note += (v >= 0.5 ? "on" : "off");
        else
            note += juce::String (v, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                  + (spec->unit.empty() ? juce::String()
                                        : " " + juce::String (spec->unit));
        if (aliasWhy.isNotEmpty()) note += " [" + aliasWhy + "]";
        notes.add (note);
    }

    if (appliedOut != nullptr) *appliedOut = applied;
    if (skippedOut != nullptr) *skippedOut = skipped;

    if (applied == 0 && skipped == 0) return {};   // an empty params object

    juce::String summary;
    if (applied > 0) summary = notes.joinIntoString (", ");

    if (! unknown.isEmpty())
    {
        if (summary.isNotEmpty()) summary += "; ";
        summary += "ignored " + unknown.joinIntoString (", ");
    }

    return summary;
}

// 21t-j: the default readback - every dialable id and the value it holds, choices by label.
juce::String EedDeviceProcessor::readbackSummary() const
{
    juce::StringArray parts;
    for (const auto& p : paramSchema().params())
    {
        const double v = getParamValue (juce::String (p.id));
        juce::String one (p.id); one << " ";
        if (! p.choices.empty())      one << juce::String (p.choiceLabel (v));
        else if (p.boolean)           one << (v >= 0.5 ? "on" : "off");
        else                          one << juce::String (v, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                                          << (p.unit.empty() ? juce::String() : " " + juce::String (p.unit));
        parts.add (one);
    }
    return parts.joinIntoString (", ");
}

void EedDeviceProcessor::resetParamsToDefaults()
{
    for (const auto& p : paramSchema().params())
        setParamValue (juce::String (p.id), p.def);
}

juce::var EedDeviceProcessor::currentParamsVar() const
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    for (const auto& p : paramSchema().params())
        o->setProperty (juce::Identifier (juce::String (p.id)),
                        getParamValue (juce::String (p.id)));
    return juce::var (o.get());
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
void EedDeviceProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("v", 1);
    root->setProperty ("bypassed", bypassed_.load());
    root->setProperty ("params", currentParamsVar());

    const juce::String json = juce::JSON::toString (juce::var (root.get()), true);
    juce::MemoryOutputStream mos (dest, false);
    mos.writeText (json, false, false, nullptr);
}

void EedDeviceProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0) return;

    const juce::String json = juce::String::createStringFromData (data, sizeInBytes);
    const juce::var parsed = juce::JSON::parse (json);
    if (! parsed.isObject()) return;

    setBypassed ((bool) parsed.getProperty ("bypassed", false));

    // FULL REPLACE, not a merge. A saved state is the whole device, and a file
    // written before a param existed must load as that param's DEFAULT rather
    // than inheriting whatever this instance was last set to.
    applyingState_ = true;
    resetParamsToDefaults();
    applyParams (parsed.getProperty ("params", juce::var()), ParamSource::Restore);
    applyingState_ = false;
    onStateApplied();
}
