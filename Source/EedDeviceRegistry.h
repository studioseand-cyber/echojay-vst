/*
    EedDeviceRegistry.h  —  the self-registering built-in device registry
    (BUILTIN_SUITE_PLAN.md §1).

    The chain used to know about exactly one built-in by name: kBuiltinEqName,
    builtinEqDescription(), isBuiltinEqName(), isBuiltinEqSlot(), one hard-coded
    factory. Adding a second device that way means editing all of those, plus the
    add-menu and the AI advertisement — five shared files, which is five merge
    conflicts every time two Wave 1 sessions add two devices in parallel.

    Here instead a device is SELF-CONTAINED. Its own .cpp ends with:

        static BuiltinDeviceRegistrar reg { makeGainDevice() };

    and that is the entire integration. builtinDeviceNames(), the synthetic
    PluginDescription, isBuiltinSlot(), the load dispatch, the add-menu and the
    [AVAILABLE BUILTINS] advertisement all read from the registry, so a new device
    appears in every one of them without touching a shared list. The EQ is a
    registry entry like any other — not a special case.

    ORDER. Registrars run at static-init time and their relative order ACROSS
    translation units is unspecified by the standard, so registration order is not
    reproducible. Everything user- or model-visible is therefore sorted (category
    rank, then name) before it is handed out — otherwise the add-menu and the AI
    feed would quietly reshuffle between builds.

    LINKING. These registrars live in a static library. See the force-load block
    in CMakeLists.txt for why that needs help, and what happens without it.
*/

#pragma once

#include <JuceHeader.h>
#include "EedParamSchema.h"

#include <functional>
#include <memory>
#include <vector>

// The synthetic format name that marks a description as one of ours. ChainHost's
// load path branches on this instead of calling the format manager. It is written
// into saved chain XML, so it must never change.
inline constexpr const char* kEchoJayBuiltinFormat = "EchoJayBuiltin";

// ---------------------------------------------------------------------------
// One built-in device.
// ---------------------------------------------------------------------------
struct BuiltinDevice
{
    juce::String name;              // EXACT, as advertised + matched: "EchoJay Gain"
    juce::String category;          // add-menu grouping: "EQ", "Utility", "Dynamics", …
    juce::String descriptiveName;   // one line for the plugin description
    juce::String summary;           // one line for the AI feed: when to reach for it

    // Stable identity. Both are written into saved chain XML and matched on
    // restore, so once a device ships these are frozen forever.
    juce::String identifier;        // "echojay:builtin:gain"
    int          uid = 0;

    // Extra spellings accepted when resolving a name the model supplied. The
    // exact `name` is always accepted and does not need to be repeated here.
    juce::StringArray aliases;

    // The dialable contract. Empty only for a device with no parameters at all.
    echojay::ParamSchema schema;

    std::function<std::unique_ptr<juce::AudioProcessor>()> create;
};

// ---------------------------------------------------------------------------
class BuiltinDeviceRegistry
{
public:
    static BuiltinDeviceRegistry& instance();

    void add (BuiltinDevice d);

    // Sorted (category rank, then name) — stable across builds.
    const std::vector<BuiltinDevice>& all() const noexcept { return devices_; }

    // Tolerant of case, separators and punctuation, and honours each device's
    // aliases. Nullptr when the name is not one of ours.
    const BuiltinDevice* findByName       (const juce::String& rawName) const;
    const BuiltinDevice* findByUid        (int uid) const;
    const BuiltinDevice* findByIdentifier (const juce::String& identifier) const;

    // Whatever a restored (possibly partial) description carries, resolved in the
    // order it can be trusted: identifier, then uid, then name.
    const BuiltinDevice* findForDescription (const juce::PluginDescription& d) const;

    juce::StringArray                    names() const;
    juce::Array<juce::PluginDescription> descriptions() const;

    // The categories present, in canonical order — the add-menu's grouping.
    juce::StringArray categories() const;

    // Canonical synthetic description for a device. Stable across machines and
    // sessions: this is what gets written into saved chain XML.
    static juce::PluginDescription descriptionFor (const BuiltinDevice& d);

    // True for a description that came from this registry. Format name is the
    // primary key; the identifier prefix is a belt-and-braces fallback for a
    // description rebuilt from restore XML with fields missing.
    static bool isBuiltinDescription (const juce::PluginDescription& d) noexcept;

    // Shared name normaliser (lowercase, alphanumerics only).
    static juce::String normalize (const juce::String& raw);

private:
    BuiltinDeviceRegistry() = default;

    // Devices sort by their category's canonical rank first so the add-menu reads
    // in a deliberate order (EQ, Dynamics, …) rather than alphabetically.
    static int categoryRank (const juce::String& category);

    std::vector<BuiltinDevice> devices_;
};

// ---------------------------------------------------------------------------
// Drop one of these at file scope in a device's .cpp and it is integrated.
// ---------------------------------------------------------------------------
struct BuiltinDeviceRegistrar
{
    explicit BuiltinDeviceRegistrar (BuiltinDevice d)
    {
        BuiltinDeviceRegistry::instance().add (std::move (d));
    }
};
