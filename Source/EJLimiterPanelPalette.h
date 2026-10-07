#pragma once
// EJLimiterPanelPalette.h (session L, 8 Oct 2026): the EchoJay palette for EedLimiterPanelV2, usable without the plugin.
// Inside the plugin EchoJayLookAndFeel.h is the one source; the preview app (tools/limiter_preview) has no JuceHeader and
// no BinaryData, so it compiles the mirror below. The values are EchoJayLookAndFeel::Colours verbatim (8 Oct 2026); a
// drift is caught by limiter_preview's palette check, which compares the two when both headers are present.
#if __has_include(<JuceHeader.h>) && ! defined(EJ_LIMITER_PREVIEW)
 #include "EchoJayLookAndFeel.h"
 namespace echojay::limv2 { using PanelColours = EchoJayLookAndFeel::Colours; }
#else
 #include <juce_gui_basics/juce_gui_basics.h>
 namespace echojay::limv2 {
 struct PanelColours {
     static inline const juce::Colour bg       { 0xff080A12 };
     static inline const juce::Colour bg2      { 0xff0A0C18 };
     static inline const juce::Colour bg3      { 0xff0E1020 };
     static inline const juce::Colour bg4      { 0xff141626 };
     static inline const juce::Colour text      { 0xfff0f0f5 };
     static inline const juce::Colour text2     { 0xffa0a0b8 };
     static inline const juce::Colour text3     { 0xff606078 };
     static inline const juce::Colour blue      { 0xff06b6d4 };
     static inline const juce::Colour blue2     { 0xff22d3ee };
     static inline const juce::Colour purple    { 0xff0891b2 };
     static inline const juce::Colour green     { 0xff4ade80 };
     static inline const juce::Colour red       { 0xffef4444 };
     static inline const juce::Colour amber     { 0xfff59e0b };
     static inline const juce::Colour border    { juce::Colour::fromFloatRGBA (1, 1, 1, 0.05f) };
     static inline const juce::Colour border2   { juce::Colour::fromFloatRGBA (1, 1, 1, 0.1f) };
 };
 }
#endif
