/*
    EedDeviceProcessor.cpp  —  see EedDeviceProcessor.h.
*/

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
                                                  int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    // A bare ARRAY belongs to a structured device (eq_bands). A flat device has
    // no meaning for one, and guessing would be worse than declining.
    if (! structured.isObject()) return {};
    if (! structured.hasProperty ("params")) return {};

    return applyParams (structured.getProperty ("params", juce::var()),
                        appliedOut, skippedOut);
}

juce::String EedDeviceProcessor::applyParams (const juce::var& paramsObject,
                                              int* appliedOut, int* skippedOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;

    auto* obj = paramsObject.getDynamicObject();
    if (obj == nullptr) return {};

    const auto& schema = paramSchema();

    juce::StringArray notes;      // what landed, for the chat log
    juce::StringArray unknown;    // ids this device does not publish
    int applied = 0, skipped = 0;

    for (const auto& prop : obj->getProperties())
    {
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
        if (! spec->choices.empty() && prop.value.isString())
        {
            const int idx = spec->indexOfChoice (prop.value.toString().toStdString());
            if (idx >= 0) { raw = (double) idx; have = true; }
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
        if (! spec->choices.empty())
            note += juce::String (spec->choiceLabel (v));
        else if (spec->boolean)
            note += (v >= 0.5 ? "on" : "off");
        else
            note += juce::String (v, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                  + (spec->unit.empty() ? juce::String()
                                        : " " + juce::String (spec->unit));
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
    resetParamsToDefaults();
    applyParams (parsed.getProperty ("params", juce::var()));
}
