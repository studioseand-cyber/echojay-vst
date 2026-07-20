/*
  EchoJayParamMaps.h

  Fingerprint + display helpers for EchoJay auto-parameter-mapping.

  The fingerprint MUST match EchoJayParamExtractor.h makeFingerprint exactly
  (format|uidHex|version|param_count, SHA-256 hex): the server's seeded maps
  (plugin:{fp}:map, GET /api/params/maps?fps=...) are keyed by it. param_count
  requires a LOADED instance, so fingerprints are computed at slot-load time,
  and ChainHost persists identity(format|uid|version) -> fp so scan-time
  prefetch can batch-fetch maps for every plugin seen at least once.

  House style: no em-dashes.

  Requires JUCE modules: juce_audio_processors, juce_core, juce_cryptography.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace echojay
{

// Keep byte-identical to the extractor: order matters and must be stable.
inline juce::String fingerprintForDescription (const juce::PluginDescription& desc, int paramCount)
{
    juce::String basis;
    basis << desc.pluginFormatName << "|"
          << juce::String::toHexString (desc.uniqueId) << "|"
          << desc.version << "|"
          << juce::String (paramCount);

    juce::SHA256 sha (basis.toRawUTF8(), basis.getNumBytesAsUTF8());
    return sha.toHexString();
}

// Identity WITHOUT param_count: what a PluginDescription alone can provide.
// Used as the persistent index key identity -> fp once a load has revealed
// the parameter count.
inline juce::String identityKeyForDescription (const juce::PluginDescription& desc)
{
    return desc.pluginFormatName + "|" + juce::String::toHexString (desc.uniqueId) + "|" + desc.version;
}

// Compact human display of one applied semantic setting:
//   ratio "4:1" -> "ratio 4:1", attack_ms 40 -> "attack 40ms",
//   threshold_db -18 -> "threshold -18dB", freq_hz 1200 -> "freq 1200Hz",
//   mix_pct 25 -> "mix 25%", reverb_decay_s 2 -> "reverb decay 2s".
inline juce::String formatSemanticSetting (const juce::String& key, const juce::var& value)
{
    const juce::String v = value.toString();
    if (key == "ratio")       return "ratio " + v;
    if (key.endsWith ("_db"))  return key.dropLastCharacters (3).replaceCharacter ('_', ' ') + " " + v + "dB";
    if (key.endsWith ("_ms"))  return key.dropLastCharacters (3).replaceCharacter ('_', ' ') + " " + v + "ms";
    if (key.endsWith ("_hz"))  return key.dropLastCharacters (3).replaceCharacter ('_', ' ') + " " + v + "Hz";
    if (key.endsWith ("_pct")) return key.dropLastCharacters (4).replaceCharacter ('_', ' ') + " " + v + "%";
    if (key.endsWith ("_s"))   return key.dropLastCharacters (2).replaceCharacter ('_', ' ') + " " + v + "s";
    return key + " " + v;
}

} // namespace echojay
