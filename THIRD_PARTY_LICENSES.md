# Third-party licences

## libvorbis / libogg (via JUCE)
The Codec Player's Ogg Vorbis presets use the libvorbis and libogg
implementations BUNDLED WITH JUCE (juce_audio_formats). No separate
dependency is added. Xiph.Org BSD-style licence; full text ships in the
JUCE tree at modules/juce_audio_formats/codecs/oggvorbis/ and applies to
redistribution of this plugin.

## AAC (macOS)
AAC encode/decode uses Apple's AudioToolbox system framework (OS-provided,
no bundled encoder). AAC presets are hidden on non-macOS builds.
