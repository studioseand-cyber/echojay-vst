/*
  EchoJayEventLog.h — local structured client events (26 Jul 2026).

  Append-only JSONL at ~/Library/EchoJay/events.jsonl. LOCAL-ONLY today, but
  the schema is upload-ready by design so the format never needs a
  migration when an upload path lands:

    {"v":1,"t":<epoch ms>,"machine_id":"<uuid>","event":"<name>", ...fields}

  - "v" is the schema version; bump it on any breaking field change.
  - "machine_id" is a RANDOM UUID minted once into ~/Library/EchoJay/
    machine_id. Deliberately NOT the login deviceId (that one is
    SHA256(computerName|userName|osName): re-derivable from the hostname and
    silently changes when the user renames their Mac). Not hardware UUID,
    serial, or hostname.
  - Field names are snake_case and stable.

  Cap: 1 MB. When an append would cross it, the OLDEST HALF of the file is
  dropped on line boundaries and the rest rewritten. Message thread only.
*/

#pragma once
#include <juce_core/juce_core.h>
#include "EJStateRoot.h"
#if JUCE_MAC || JUCE_LINUX
 #include <sys/stat.h>   // ::chmod — machine_id is 0600, owner-only
#endif

namespace echojay
{

inline juce::File eventLogDir()
{
    return echojay::userStateHome()
        .getChildFile ("Library").getChildFile ("EchoJay");
}

inline juce::String machineId()
{
    static juce::String cached;
    if (cached.isNotEmpty()) return cached;
    auto f = eventLogDir().getChildFile ("machine_id");
    if (f.existsAsFile())
        cached = f.loadFileAsString().trim();
    if (cached.length() != 36)   // missing or mangled: mint once
    {
        cached = juce::Uuid().toDashedString();
        eventLogDir().createDirectory();
        f.replaceWithText (cached + "\n");
       #if JUCE_MAC || JUCE_LINUX
        ::chmod (f.getFullPathName().toRawUTF8(), 0600);
       #endif
    }
    return cached;
}

// Append one event. `fields` is a var wrapping a DynamicObject of extra
// event-specific fields (may be void). Failures are swallowed: logging must
// never break the feature it observes.
inline void logClientEvent (const juce::String& event, const juce::var& fields = {})
{
    constexpr juce::int64 kMaxBytes = 1024 * 1024;
    auto dir = eventLogDir();
    dir.createDirectory();
    auto f = dir.getChildFile ("events.jsonl");

    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 1);
    o->setProperty ("t", juce::Time::currentTimeMillis());
    o->setProperty ("machine_id", machineId());
    o->setProperty ("event", event);
    if (auto* extra = fields.getDynamicObject())
        for (auto& kv : extra->getProperties())
            o->setProperty (kv.name, kv.value);
    const auto line = juce::JSON::toString (juce::var (o), true) + "\n";

    if (f.existsAsFile() && f.getSize() + (juce::int64) line.getNumBytesAsUTF8() > kMaxBytes)
    {
        // Drop the oldest half on a line boundary, atomic rewrite.
        auto text = f.loadFileAsString();
        int cut = text.indexOfChar ((int) text.length() / 2, '\n');
        if (cut >= 0)
            f.replaceWithText (text.substring (cut + 1));
    }
    f.appendText (line, false, false, "\n");
}

} // namespace echojay
