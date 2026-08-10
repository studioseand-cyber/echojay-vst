/*
    EedDeviceRegistry.cpp  —  see EedDeviceRegistry.h.
*/

#include "EedDeviceRegistry.h"
#include "EedDeviceProcessor.h"

#include <algorithm>
#include <iterator>     // std::size

// ---------------------------------------------------------------------------
// The canonical add-menu order. A device whose category is not listed sorts
// after all of these (alphabetically among themselves), so an unrecognised
// category is a cosmetic surprise rather than a device that vanishes.
// ---------------------------------------------------------------------------
namespace
{
    const char* const kCategoryOrder[] = {
        "EQ", "Dynamics", "Utility", "Stereo", "Modulation", "Harmonic", "Time",
        "Pitch", "Analysis"
    };
}

BuiltinDeviceRegistry& BuiltinDeviceRegistry::instance()
{
    // Function-local static: constructed on first use, which is what makes it
    // safe to call from another translation unit's static initialiser. A plain
    // namespace-scope object would be a static-init-order race.
    static BuiltinDeviceRegistry reg;
    return reg;
}

int BuiltinDeviceRegistry::categoryRank (const juce::String& category)
{
    for (int i = 0; i < (int) std::size (kCategoryOrder); ++i)
        if (category.equalsIgnoreCase (kCategoryOrder[i])) return i;
    return (int) std::size (kCategoryOrder);      // unknown: after everything known
}

juce::String BuiltinDeviceRegistry::normalize (const juce::String& raw)
{
    juce::String out;
    for (auto c : raw)
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            out << juce::CharacterFunctions::toLowerCase (c);
    return out;
}

void BuiltinDeviceRegistry::add (BuiltinDevice d)
{
    // A duplicate name or uid would make resolution ambiguous and, worse, make
    // it depend on static-init order. Refusing the second registration keeps the
    // first one working rather than corrupting both.
    jassert (d.name.isNotEmpty() && d.create != nullptr);
    if (d.name.isEmpty() || d.create == nullptr) return;

    for (const auto& existing : devices_)
    {
        if (normalize (existing.name) == normalize (d.name) || existing.uid == d.uid)
        {
            jassertfalse;   // two devices claiming the same identity
            return;
        }
    }

    // EVERY device is constructed at its ADVERTISED defaults.
    //
    // The advertisement is what the model reasons against, so a device whose
    // members initialise differently from its ParamSpec defaults makes the
    // model's picture of a fresh insert wrong - and the error hides itself the
    // moment any state is restored, because restore writes the schema defaults.
    // Only the very first instance is ever wrong, which is the hardest kind of
    // bug to notice.
    //
    // Measured before this wrapper: seven mismatches across Auto Pan, Chorus,
    // Phaser and Tremolo, all sharing an LFO core that constructs at its own
    // generic 1 Hz / 50% while each device advertises its own rate and depth.
    //
    // Fixed HERE rather than in four sets of member initialisers, because the
    // funnel closes the class permanently: device 23 cannot reintroduce it.
    if (d.create)
    {
        d.create = [inner = std::move (d.create)]() -> std::unique_ptr<juce::AudioProcessor>
        {
            auto p = inner();
            if (auto* dev = dynamic_cast<EedDeviceProcessor*> (p.get()))
                dev->resetParamsToDefaults();
            return p;
        };
    }

    devices_.push_back (std::move (d));

    // Sorted on every insert rather than lazily, so `all()` is always in the
    // published order no matter which registrar ran first.
    std::sort (devices_.begin(), devices_.end(),
               [] (const BuiltinDevice& a, const BuiltinDevice& b)
               {
                   const int ra = categoryRank (a.category), rb = categoryRank (b.category);
                   if (ra != rb) return ra < rb;
                   const int byCat = a.category.compareIgnoreCase (b.category);
                   if (byCat != 0) return byCat < 0;
                   return a.name.compareIgnoreCase (b.name) < 0;
               });
}

// ---------------------------------------------------------------------------
// lookup
// ---------------------------------------------------------------------------
const BuiltinDevice* BuiltinDeviceRegistry::findByName (const juce::String& rawName) const
{
    const auto key = normalize (rawName);
    if (key.isEmpty()) return nullptr;

    for (const auto& d : devices_)
    {
        if (normalize (d.name) == key) return &d;
        for (const auto& a : d.aliases)
            if (normalize (a) == key) return &d;
    }
    return nullptr;
}

const BuiltinDevice* BuiltinDeviceRegistry::findByUid (int uid) const
{
    if (uid == 0) return nullptr;
    for (const auto& d : devices_)
        if (d.uid == uid) return &d;
    return nullptr;
}

const BuiltinDevice* BuiltinDeviceRegistry::findByIdentifier (const juce::String& identifier) const
{
    if (identifier.isEmpty()) return nullptr;
    for (const auto& d : devices_)
        if (d.identifier == identifier) return &d;
    return nullptr;
}

const BuiltinDevice* BuiltinDeviceRegistry::findForDescription (const juce::PluginDescription& d) const
{
    // Identifier first: it is the one field that survives an XML round-trip
    // intact and is unique by construction. Uid next. Name last, because it is
    // the field most likely to have been retyped by a model.
    if (auto* byId = findByIdentifier (d.fileOrIdentifier)) return byId;
    if (auto* byUid = findByUid (d.uniqueId))               return byUid;
    if (auto* byUid = findByUid (d.deprecatedUid))          return byUid;
    return findByName (d.name);
}

// ---------------------------------------------------------------------------
// published views
// ---------------------------------------------------------------------------
juce::StringArray BuiltinDeviceRegistry::names() const
{
    juce::StringArray out;
    for (const auto& d : devices_) out.add (d.name);
    return out;
}

juce::PluginDescription BuiltinDeviceRegistry::descriptionFor (const BuiltinDevice& d)
{
    juce::PluginDescription pd;
    pd.name              = d.name;
    pd.descriptiveName   = d.descriptiveName;
    pd.pluginFormatName  = kEchoJayBuiltinFormat;
    pd.category          = d.category;
    pd.manufacturerName  = "EchoJay";
    pd.version           = "1.0.0";
    pd.fileOrIdentifier  = d.identifier;
    pd.uniqueId          = d.uid;
    pd.deprecatedUid     = d.uid;
    pd.isInstrument      = false;
    pd.numInputChannels  = 2;
    pd.numOutputChannels = 2;
    return pd;
}

juce::Array<juce::PluginDescription> BuiltinDeviceRegistry::descriptions() const
{
    juce::Array<juce::PluginDescription> out;
    for (const auto& d : devices_) out.add (descriptionFor (d));
    return out;
}

juce::StringArray BuiltinDeviceRegistry::categories() const
{
    juce::StringArray out;
    for (const auto& d : devices_)          // devices_ is already in category order
        if (! out.contains (d.category)) out.add (d.category);
    return out;
}

bool BuiltinDeviceRegistry::isBuiltinDescription (const juce::PluginDescription& d) noexcept
{
    return d.pluginFormatName == kEchoJayBuiltinFormat
        || d.fileOrIdentifier.startsWith ("echojay:builtin:");
}
