cmake_minimum_required(VERSION 3.22)
# Universal binary: Apple Silicon + Intel
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")
# Minimum macOS: Big Sur (11.0). Covers anything 4+ years old.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# 2.25.0: the built-in device suite (feat/surgical-eq) merges into the dashboard
# mainline. Minor bump, not a patch — 20 self-registering devices, the viz
# library and the ParamSchema dialability contract all arrive at once.
project(EchoJay VERSION 2.25.23)

# Path to your JUCE installation — update this to match your setup
# Download JUCE from https://juce.com/download/
set(JUCE_PATH "${CMAKE_SOURCE_DIR}/../JUCE" CACHE PATH "Path to JUCE framework")

# Path to AAX SDK — download from developer.avid.com
set(AAX_SDK_PATH "$ENV{HOME}/AAX_SDK" CACHE PATH "Path to AAX SDK")

# ---------------------------------------------------------------------------
# Development transport (Session B). OFF, and it must stay off for anything
# that ships.
#
# The V2 API is only reachable on a Vercel PREVIEW deployment, which needs a
# base URL that changes on every deploy plus an x-vercel-protection-bypass
# secret. That secret grants access to every protected deployment on the
# project, so it must never be in a release binary.
#
# A grep in the build script is not enough: a grep only catches what someone
# remembered to look for. This makes it structural instead. With the flag off,
# the code that reads ~/.echojay/dev.json, the code that sets the header, and
# the strings themselves are NOT COMPILED, so a shipped plugin can only ever
# talk to https://www.echojay.ai and there is nothing in the binary to extract.
#
# Debug builds get it automatically; Release only if someone asks for it by
# name, which build-installer.sh never does. Verify the artefact, not the
# source (see the strings check in RELEASE.md).
option(ECHOJAY_DEV_TRANSPORT "Compile the dev transport: preview base URL + Vercel bypass header. NEVER for a release build." OFF)
set(ECHOJAY_DEV_TRANSPORT_DEFS
    "$<$<OR:$<CONFIG:Debug>,$<BOOL:${ECHOJAY_DEV_TRANSPORT}>>:ECHOJAY_DEV_TRANSPORT=1>")

add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE)

# Tell JUCE where the AAX SDK lives
if(EXISTS "${AAX_SDK_PATH}/Interfaces/AAX.h" OR EXISTS "${AAX_SDK_PATH}/Interfaces/ACF/AAX.h")
    juce_set_aax_sdk_path(${AAX_SDK_PATH})
    set(HAS_AAX TRUE)
    message(STATUS "AAX SDK found at ${AAX_SDK_PATH}")
else()
    set(HAS_AAX FALSE)
    message(STATUS "AAX SDK not found — building without AAX")
endif()

# Build formats — include AAX if SDK is available
if(HAS_AAX)
    set(PLUGIN_FORMATS AU VST3 AAX)
else()
    set(PLUGIN_FORMATS AU VST3)
endif()

# Platform-conditional native sources, shared by both plugin targets.
#   NativeClip.mm   — Obj-C++/Cocoa: NSView clipping container for inline-hosted
#                     plugin editors. NativeClipWin.cpp supplies the same
#                     extern "C" surface off macOS (real logger, stubbed clip).
#   AUEnumerator.mm — Obj-C++/CoreAudio: AU registry scan. Needs no off-Mac
#                     counterpart — its only call site (ChainHost.cpp) is
#                     already inside #if JUCE_MAC, so nothing references it.
# Both are Objective-C++ and can never compile under MSVC.
if(APPLE)
    set(ECHOJAY_NATIVE_SOURCES
        Source/NativeClip.mm
        Source/AUEnumerator.mm
    )
else()
    set(ECHOJAY_NATIVE_SOURCES
        Source/NativeClipWin.cpp
    )
endif()

# Platform-conditional app icon — JUCE needs .ico on Windows, .icns on macOS
# We only have .icns, so only set the icon on macOS
if(APPLE)
    set(ECHOJAY_ICON_ARG ICON_BIG "${CMAKE_SOURCE_DIR}/EchoJay.icns")
else()
    set(ECHOJAY_ICON_ARG "")
endif()

juce_add_plugin(EchoJay
    ${ECHOJAY_ICON_ARG}
    COMPANY_NAME "EchoJay"
    BUNDLE_ID "com.echojay.plugin.v2"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Ecjy
    PLUGIN_CODE EcJ2
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "EchoJay V2"
    AAX_IDENTIFIER "com.echojay.plugin.v2"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES "Fx" "Analyzer"
    AAX_CATEGORY "AAX_ePlugInCategory_None"
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS "com.apple.security.network.client"
)

juce_generate_juce_header(EchoJay)

target_sources(EchoJay PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/ChainHost.cpp
    Source/ChainHost.h
    ${ECHOJAY_NATIVE_SOURCES}
    Source/AUEnumerator.h
    Source/NativeClip.h
    Source/MeterEngine.cpp
    Source/MeterEngine.h
    Source/CodecRender.cpp
    Source/CodecRender.h
    Source/PluginScanner.cpp
    Source/PluginScanner.h
    Source/PluginCatalog.h
    Source/PluginChecklist.cpp
    Source/PluginChecklist.h
    Source/EchoJayAPI.cpp
    Source/EchoJayAPI.h
    Source/EchoJayWorkspace.cpp
    Source/EchoJayWorkspace.h
    Source/ReferenceAnalyser.cpp
    Source/ReferenceAnalyser.h
    Source/WaveformRecorder.cpp
    Source/WaveformRecorder.h
    Source/EchoJayLookAndFeel.h
    Source/ParticleVisual.h
    Source/ParticleVisual.cpp
    Source/LinkShm.h
    # Procedural project art for the Dashboard tab. MIRRORS
    # public/js/art-core.js in echojay-saas: the ORDER of PRNG calls IS the
    # algorithm, so a change on either side needs the same change on the other
    # in the same commit. tools/art_parity_test holds both sides to it against
    # a fixture generated by the web repo.
    Source/ProjectArt.cpp
    Source/ProjectArt.h
    # Session C: the process-wide community poller. One timer and one
    # request per 20s for the whole host, shared by every EchoJay
    # instance, because the unread counts are per account.
    Source/DashPoll.cpp
    Source/DashPoll.h
    # Session C: the Dashboard tab. The SECOND renderer of the one dashboard
    # payload (the web app is the first). Parses the payload, draws it, and
    # owns every rect it draws, so nothing on it can leak onto another tab.
    Source/DashboardTab.cpp
    Source/DashboardTab.h

    # --- built-in surgical EQ (feat/surgical-eq) ---
    Source/EqEngine.cpp
    Source/EqEngine.h
    Source/EqFft.h
    Source/EqMove.h
    Source/EqNote.h
    Source/EqPresets.h
    Source/EqResonanceHunt.h
    Source/SurgicalEqProcessor.cpp
    Source/SurgicalEqProcessor.h
    Source/SurgicalEqEditor.cpp
    Source/SurgicalEqEditor.h

    # --- Built-in device framework + devices (BUILTIN_SUITE_PLAN.md) ---------
    # ALPHABETISED. This is the one shared file a new device has to touch, so
    # keeping it sorted means two Wave 1 sessions adding two devices land in
    # different places in the list and merge without conflicting.
    Source/DeviceEditorBase.cpp
    Source/DeviceEditorBase.h
    Source/EchoJayDeviceLookAndFeel.cpp
    Source/EchoJayDeviceLookAndFeel.h
    Source/EedAutoPanEditor.cpp
    Source/EedAutoPanEditor.h
    Source/EedAutoPanEngine.h
    Source/EedAutoPanProcessor.cpp
    Source/EedAutoPanProcessor.h
    Source/EedChorusEditor.cpp
    Source/EedChorusEditor.h
    Source/EedChorusEngine.h
    Source/EedChorusProcessor.cpp
    Source/EedChorusProcessor.h
    Source/EedDelayCore.h
    Source/EedDelayEditor.cpp
    Source/EedDelayEditor.h
    Source/EedDelayEngine.h
    Source/EedDelayProcessor.cpp
    Source/EedDelayProcessor.h
    Source/EedCompressorEditor.cpp
    Source/EedCompressorEditor.h
    Source/EedCompressorProcessor.cpp
    Source/EedCompressorProcessor.h
    Source/EedDeEsserEditor.cpp
    Source/EedDeEsserEditor.h
    Source/EedDeEsserProcessor.cpp
    Source/EedDeEsserProcessor.h
    Source/EedDeviceProcessor.cpp
    Source/EedDeviceProcessor.h
    Source/EedDeviceRegistry.cpp
    Source/EedDeviceRegistry.h
    Source/EedExciterEditor.cpp
    Source/EedExciterEditor.h
    Source/EedExciterEngine.h
    Source/EedExciterProcessor.cpp
    Source/EedExciterProcessor.h
    Source/EedDynamicsCore.h
    Source/EedDynamicsEditorSupport.h
    Source/EedDynamicsFaceEditor.cpp
    Source/EedDynamicsFaceEditor.h
    Source/EedExpanderEditor.cpp
    Source/EedExpanderEditor.h
    Source/EedExpanderProcessor.cpp
    Source/EedExpanderProcessor.h
    Source/EedGainEditor.cpp
    Source/EedGainEditor.h
    Source/EedGainEngine.h
    Source/EedGainProcessor.cpp
    Source/EedGainProcessor.h
    Source/EedHarmonicAnalysis.cpp
    Source/EedHarmonicAnalysis.h
    Source/EedHarmonicCore.cpp
    Source/EedHarmonicCore.h
    Source/EedKeyDetectorEditor.cpp
    Source/EedKeyDetectorEditor.h
    Source/EedKeyDetectorProcessor.cpp
    Source/EedKeyDetectorProcessor.h
    Source/EedKeyEngine.cpp
    Source/EedKeyEngine.h
    Source/EedKeyWorker.h
    Source/EedLfoCore.h
    Source/EedModEditorBase.cpp
    Source/EedModEditorBase.h
    Source/EedModTempo.h
    Source/EedGateEditor.cpp
    Source/EedGateEditor.h
    Source/EedGateProcessor.cpp
    Source/EedGateProcessor.h
    Source/EedLimiterEditor.cpp
    Source/EedLimiterEditor.h
    Source/EedLimiterProcessor.cpp
    Source/EedLimiterProcessor.h
    Source/EedMultibandEditor.cpp
    Source/EedMultibandEditor.h
    Source/EedMultibandProcessor.cpp
    Source/EedMultibandProcessor.h
    Source/EedParamSchema.h
    Source/EedPhaseInvertEditor.cpp
    Source/EedPhaseInvertEditor.h
    Source/EedPhaseInvertProcessor.cpp
    Source/EedPhaseInvertProcessor.h
    Source/EedPhaserEditor.cpp
    Source/EedPhaserEditor.h
    Source/EedPhaserEngine.h
    Source/EedPhaserProcessor.cpp
    Source/EedPhaserProcessor.h
    Source/EedPitchEditor.cpp
    Source/EedPitchEditor.h
    Source/EedPitchEngine.h
    Source/EedPitchProcessor.cpp
    Source/EedPitchProcessor.h
    Source/EedReverbEditor.cpp
    Source/EedReverbEditor.h
    Source/EedReverbEngine.h
    Source/EedReverbProcessor.cpp
    Source/EedReverbProcessor.h
    Source/EedSaturationEditor.cpp
    Source/EedSaturationEditor.h
    Source/EedSaturationProcessor.cpp
    Source/EedSaturationProcessor.h
    Source/EedStereoEngine.h
    Source/EedStereoWidthEditor.cpp
    Source/EedStereoWidthEditor.h
    Source/EedStereoWidthProcessor.cpp
    Source/EedStereoWidthProcessor.h
    Source/EedStereoizerEditor.cpp
    Source/EedStereoizerEditor.h
    Source/EedStereoizerProcessor.cpp
    Source/EedStereoizerProcessor.h
    Source/EedTapeEditor.cpp
    Source/EedTapeEditor.h
    Source/EedTapeEngine.h
    Source/EedTapeMotionView.cpp
    Source/EedTapeMotionView.h
    Source/EedTapeProcessor.cpp
    Source/EedTapeProcessor.h
    Source/EedTempoClock.h
    Source/EedTremoloEditor.cpp
    Source/EedTremoloEditor.h
    Source/EedTremoloEngine.h
    Source/EedTremoloProcessor.cpp
    Source/EedTremoloProcessor.h

    # --- Shared visualisation library (VISUALS_PLAN.md Phase V0) -------------
    # ALPHABETISED, same reason as the device block above: this is a shared file
    # that every V1 category session will add a view to, and a sorted list is
    # what makes those additions land in different places and merge clean.
    Source/viz/DecayView.cpp
    Source/viz/DecayView.h
    Source/viz/DeEsserBandView.cpp
    Source/viz/DeEsserBandView.h
    Source/viz/DelayTapsView.cpp
    Source/viz/DelayTapsView.h
    Source/viz/EedGrMeter.h
    Source/viz/Goniometer.cpp
    Source/viz/Goniometer.h
    Source/viz/HarmonicBars.cpp
    Source/viz/HarmonicBars.h
    Source/viz/LevelMeter.cpp
    Source/viz/LevelMeter.h
    Source/viz/LfoScopeView.cpp
    Source/viz/LfoScopeView.h
    Source/viz/PanPositionView.cpp
    Source/viz/PanPositionView.h
    Source/viz/SweepView.cpp
    Source/viz/SweepView.h
    Source/viz/DwellGlow.cpp
    Source/viz/TransferCurveView.cpp
    Source/viz/TransferCurveView.h
    Source/viz/VizTap.h
    Source/viz/VizView.cpp
    Source/viz/VizView.h
    Source/viz/WaveshaperView.cpp
    Source/viz/WaveshaperView.h
)

target_compile_definitions(EchoJay PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_OPENGL=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    ${ECHOJAY_DEV_TRANSPORT_DEFS}
)

target_link_libraries(EchoJay
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_cryptography
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# --- Built-in device self-registration vs. the static linker -----------------
# Built-in devices (Source/Eed*.cpp, SurgicalEq*) add themselves to
# BuiltinDeviceRegistry from a file-scope static in their OWN .cpp. That is what
# makes a device self-contained: no central list, so two Wave 1 sessions adding
# two devices never touch the same line.
#
# JUCE compiles these into a STATIC library (libEchoJay V2_SharedCode.a) that the
# AU/VST3 targets link. A static-library member is only pulled in when a symbol
# in it is ALREADY needed — and a device's only outside-visible effect is its
# registrar's constructor, which nothing references by name. Left alone the
# linker discards the entire device and it silently disappears from the add-menu
# and the AI advertisement, with no build error to notice.
#
# This is not theoretical — it was measured. Relinking with the call below
# commented out drops BOTH Wave 0 devices: the binary keeps only
# "echojay:builtin:eq" (which survives because other code still names the EQ),
# while Gain and Phase Invert vanish from the add-menu and the AI advertisement
# with a clean, successful build and no diagnostic anywhere.
#
# Force-loading the archive keeps every registrar. This is the ONE place that has
# to know the problem exists; adding a device later only appends to the source
# list above.
function(echojay_force_load_shared_code shared_target)
    foreach(fmt AU VST3 AAX Standalone)
        if(TARGET ${shared_target}_${fmt})
            if(APPLE)
                target_link_options(${shared_target}_${fmt} PRIVATE
                    "-Wl,-force_load,$<TARGET_FILE:${shared_target}>")
            elseif(MSVC)
                target_link_options(${shared_target}_${fmt} PRIVATE
                    "/WHOLEARCHIVE:$<TARGET_FILE:${shared_target}>")
            else()
                target_link_options(${shared_target}_${fmt} PRIVATE
                    "-Wl,--whole-archive" "$<TARGET_FILE:${shared_target}>"
                    "-Wl,--no-whole-archive")
            endif()
        endif()
    endforeach()
endfunction()

echojay_force_load_shared_code(EchoJay)

# --- Force-replace JUCE-generated placeholder Icon.icns with the real EchoJay.icns ---
# JUCE sometimes embeds an 8-byte placeholder instead of using ICON_BIG.
# This post-build copy guarantees the real icon ends up in every bundle.
if(APPLE)
    foreach(target_name EchoJay_VST3 EchoJay_AU EchoJay_AAX)
        if(TARGET ${target_name})
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_SOURCE_DIR}/EchoJay.icns"
                    "$<TARGET_BUNDLE_CONTENT_DIR:${target_name}>/Resources/Icon.icns"
                COMMENT "Copying real EchoJay.icns to ${target_name} Resources"
                VERBATIM
            )
        endif()
    endforeach()
endif()

# =============================================================================
# EchoJay Link — separate plugin target, version 0.1.0
# A lightweight insert that identifies a channel by name and passes audio
# through completely unchanged. No audio analysis yet.
# =============================================================================

set(LINK_VERSION "0.8.5")

# Link ships AU + VST3 + AAX (AAX only when the SDK is present, matching the
# main plugin's gate). No Standalone.
if(HAS_AAX)
    set(LINK_FORMATS AU VST3 AAX)
else()
    set(LINK_FORMATS AU VST3)
endif()

juce_add_plugin(EchoJayLink
    COMPANY_NAME "EchoJay"
    BUNDLE_ID "com.echojay.link"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Ecjy
    PLUGIN_CODE EjLk
    FORMATS ${LINK_FORMATS}
    PRODUCT_NAME "EchoJay Link"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES "Fx"
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS "com.apple.security.network.client"
    VERSION ${LINK_VERSION}
)

juce_generate_juce_header(EchoJayLink)

target_sources(EchoJayLink PRIVATE
    Source/LinkProcessor.cpp
    Source/LinkProcessor.h
    Source/LinkEditor.cpp
    Source/LinkEditor.h
    Source/EchoJayLogo.h
    Source/LinkShm.h
    # Per-Link meter frames (LINK tab mini strips) — full engine, shared source
    Source/MeterEngine.cpp
    Source/MeterEngine.h
    # Chain hosting — shared with the main plugin (phase 1)
    Source/ChainHost.cpp
    Source/ChainHost.h
    ${ECHOJAY_NATIVE_SOURCES}
    Source/AUEnumerator.h
    Source/PluginScanner.h
    Source/NativeClip.h
    # --- built-in surgical EQ (feat/surgical-eq) ---
    # Shared with the main plugin: the EQ has to run in the Link insert too, so
    # the DSP core, the AudioProcessor wrapper and its editor all build here.
    Source/EqEngine.cpp
    Source/EqEngine.h
    Source/EqFft.h
    Source/EqMove.h
    Source/EqNote.h
    Source/EqPresets.h
    Source/EqResonanceHunt.h
    Source/SurgicalEqProcessor.cpp
    Source/SurgicalEqProcessor.h
    Source/SurgicalEqEditor.cpp
    Source/SurgicalEqEditor.h

    # --- Built-in device framework + devices (BUILTIN_SUITE_PLAN.md) ---------
    # Same set as the main plugin, and for the same reason the EQ is here: a
    # built-in has to be hostable in the Link insert too. ALPHABETISED — see the
    # note on the main plugin's copy of this block.
    Source/DeviceEditorBase.cpp
    Source/DeviceEditorBase.h
    Source/EchoJayDeviceLookAndFeel.cpp
    Source/EchoJayDeviceLookAndFeel.h
    Source/EedAutoPanEditor.cpp
    Source/EedAutoPanEditor.h
    Source/EedAutoPanEngine.h
    Source/EedAutoPanProcessor.cpp
    Source/EedAutoPanProcessor.h
    Source/EedChorusEditor.cpp
    Source/EedChorusEditor.h
    Source/EedChorusEngine.h
    Source/EedChorusProcessor.cpp
    Source/EedChorusProcessor.h
    Source/EedDelayCore.h
    Source/EedDelayEditor.cpp
    Source/EedDelayEditor.h
    Source/EedDelayEngine.h
    Source/EedDelayProcessor.cpp
    Source/EedDelayProcessor.h
    Source/EedCompressorEditor.cpp
    Source/EedCompressorEditor.h
    Source/EedCompressorProcessor.cpp
    Source/EedCompressorProcessor.h
    Source/EedDeEsserEditor.cpp
    Source/EedDeEsserEditor.h
    Source/EedDeEsserProcessor.cpp
    Source/EedDeEsserProcessor.h
    Source/EedDeviceProcessor.cpp
    Source/EedDeviceProcessor.h
    Source/EedDeviceRegistry.cpp
    Source/EedDeviceRegistry.h
    Source/EedExciterEditor.cpp
    Source/EedExciterEditor.h
    Source/EedExciterEngine.h
    Source/EedExciterProcessor.cpp
    Source/EedExciterProcessor.h
    Source/EedDynamicsCore.h
    Source/EedDynamicsEditorSupport.h
    Source/EedDynamicsFaceEditor.cpp
    Source/EedDynamicsFaceEditor.h
    Source/EedExpanderEditor.cpp
    Source/EedExpanderEditor.h
    Source/EedExpanderProcessor.cpp
    Source/EedExpanderProcessor.h
    Source/EedGainEditor.cpp
    Source/EedGainEditor.h
    Source/EedGainEngine.h
    Source/EedGainProcessor.cpp
    Source/EedGainProcessor.h
    Source/EedHarmonicAnalysis.cpp
    Source/EedHarmonicAnalysis.h
    Source/EedHarmonicCore.cpp
    Source/EedHarmonicCore.h
    Source/EedKeyDetectorEditor.cpp
    Source/EedKeyDetectorEditor.h
    Source/EedKeyDetectorProcessor.cpp
    Source/EedKeyDetectorProcessor.h
    Source/EedKeyEngine.cpp
    Source/EedKeyEngine.h
    Source/EedKeyWorker.h
    Source/EedLfoCore.h
    Source/EedModEditorBase.cpp
    Source/EedModEditorBase.h
    Source/EedModTempo.h
    Source/EedGateEditor.cpp
    Source/EedGateEditor.h
    Source/EedGateProcessor.cpp
    Source/EedGateProcessor.h
    Source/EedLimiterEditor.cpp
    Source/EedLimiterEditor.h
    Source/EedLimiterProcessor.cpp
    Source/EedLimiterProcessor.h
    Source/EedMultibandEditor.cpp
    Source/EedMultibandEditor.h
    Source/EedMultibandProcessor.cpp
    Source/EedMultibandProcessor.h
    Source/EedParamSchema.h
    Source/EedPhaseInvertEditor.cpp
    Source/EedPhaseInvertEditor.h
    Source/EedPhaseInvertProcessor.cpp
    Source/EedPhaseInvertProcessor.h
    Source/EedPhaserEditor.cpp
    Source/EedPhaserEditor.h
    Source/EedPhaserEngine.h
    Source/EedPhaserProcessor.cpp
    Source/EedPhaserProcessor.h
    Source/EedPitchEditor.cpp
    Source/EedPitchEditor.h
    Source/EedPitchEngine.h
    Source/EedPitchProcessor.cpp
    Source/EedPitchProcessor.h
    Source/EedReverbEditor.cpp
    Source/EedReverbEditor.h
    Source/EedReverbEngine.h
    Source/EedReverbProcessor.cpp
    Source/EedReverbProcessor.h
    Source/EedSaturationEditor.cpp
    Source/EedSaturationEditor.h
    Source/EedSaturationProcessor.cpp
    Source/EedSaturationProcessor.h
    Source/EedStereoEngine.h
    Source/EedStereoWidthEditor.cpp
    Source/EedStereoWidthEditor.h
    Source/EedStereoWidthProcessor.cpp
    Source/EedStereoWidthProcessor.h
    Source/EedStereoizerEditor.cpp
    Source/EedStereoizerEditor.h
    Source/EedStereoizerProcessor.cpp
    Source/EedStereoizerProcessor.h
    Source/EedTapeEditor.cpp
    Source/EedTapeEditor.h
    Source/EedTapeEngine.h
    Source/EedTapeMotionView.cpp
    Source/EedTapeMotionView.h
    Source/EedTapeProcessor.cpp
    Source/EedTapeProcessor.h
    Source/EedTempoClock.h
    Source/EedTremoloEditor.cpp
    Source/EedTremoloEditor.h
    Source/EedTremoloEngine.h
    Source/EedTremoloProcessor.cpp
    Source/EedTremoloProcessor.h

    # --- Shared visualisation library (VISUALS_PLAN.md Phase V0) -------------
    # ALPHABETISED, same reason as the device block above: this is a shared file
    # that every V1 category session will add a view to, and a sorted list is
    # what makes those additions land in different places and merge clean.
    Source/viz/DecayView.cpp
    Source/viz/DecayView.h
    Source/viz/DeEsserBandView.cpp
    Source/viz/DeEsserBandView.h
    Source/viz/DelayTapsView.cpp
    Source/viz/DelayTapsView.h
    Source/viz/EedGrMeter.h
    Source/viz/Goniometer.cpp
    Source/viz/Goniometer.h
    Source/viz/HarmonicBars.cpp
    Source/viz/HarmonicBars.h
    Source/viz/LevelMeter.cpp
    Source/viz/LevelMeter.h
    Source/viz/LfoScopeView.cpp
    Source/viz/LfoScopeView.h
    Source/viz/PanPositionView.cpp
    Source/viz/PanPositionView.h
    Source/viz/SweepView.cpp
    Source/viz/SweepView.h
    Source/viz/DwellGlow.cpp
    Source/viz/TransferCurveView.cpp
    Source/viz/TransferCurveView.h
    Source/viz/VizTap.h
    Source/viz/VizView.cpp
    Source/viz/VizView.h
    Source/viz/WaveshaperView.cpp
    Source/viz/WaveshaperView.h
)

echojay_force_load_shared_code(EchoJayLink)

target_compile_definitions(EchoJayLink PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    # NativeClip per-target identity — unique ObjC class + log tag so both
    # plugins can host inline in the same process without colliding
    ECHOJAY_CLIP_CLASS=EchoJayClipContainerLink
    NATIVECLIP_LOG_TAG="NativeClipLink"
    # The Link consumes no spectrum: LinkEditor never touches meterEngine_
    # and LinkMeterFrame carries no bins, so MeterEngine's 4096-point
    # VISUAL FFT (~43/s, per Link, up to 16 of them) was computed and
    # thrown away. Compile-time because the decision is fixed at build
    # time; a runtime flag would cost a test per block forever to answer a
    # question that can never change. THIS TARGET ONLY: the main plugin
    # reads the visual spectrum on Visualisation, the spectrum and
    # spectrogram views and the Compare page, and its own
    # target_compile_definitions does not carry this.
    ECHOJAY_NO_VISUAL_FFT=1
)

target_link_libraries(EchoJayLink
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_dsp
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Print built bundle path after AU build
if(APPLE)
    add_custom_command(TARGET EchoJayLink_AU POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo
            "EchoJay Link AU: $<TARGET_BUNDLE_DIR:EchoJayLink_AU>"
        COMMENT "EchoJay Link AU path"
        VERBATIM
    )
endif()

# ============================================================================
# ejextract: the auto-parameter-mapping harness (worker + --bootstrap driver).
# Universal2 so the driver can relaunch itself under Rosetta for Intel-only
# plugins. Bundled by build-installer.sh into the opt-in background-mapping
# installer choice; NEVER loaded inside a DAW.
# ============================================================================
juce_add_console_app(ejextract PRODUCT_NAME "ejextract")

target_sources(ejextract PRIVATE tools/ejextract/main.cpp)
target_include_directories(ejextract PRIVATE tools/ejextract)

target_compile_definitions(ejextract PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(ejextract PRIVATE
    juce::juce_audio_processors
    juce::juce_cryptography
    juce::juce_gui_basics
    juce::juce_recommended_config_flags)

set_target_properties(ejextract PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")

if(APPLE)
    target_link_libraries(ejextract PRIVATE "-framework CoreAudioKit")
endif()

# ============================================================================
# Built-in device framework test (BUILTIN_SUITE_PLAN.md Wave 0).
#
# ParamSchema and the device engines are JUCE-free and tested under plain g++ in
# test/. The registry, the device processors and the applyStructured funnel are
# not — they need JUCE — so they are exercised here instead, through the same
# static registrars, the same factory and the same apply path the plugin uses.
#
# EXCLUDE_FROM_ALL: a normal plugin build should not pay for it. Run it with
#   cmake --build build --target EchoJayBuiltinRegistryTest
# ============================================================================
juce_add_console_app(EchoJayBuiltinRegistryTest PRODUCT_NAME "EchoJayBuiltinRegistryTest")
juce_generate_juce_header(EchoJayBuiltinRegistryTest)

target_sources(EchoJayBuiltinRegistryTest PRIVATE
    tools/builtin_registry_test.cpp
    # The devices under test, plus what they pull in. Alphabetised, same as the
    # plugin targets' built-in device block.
    Source/DeviceEditorBase.cpp
    Source/EchoJayDeviceLookAndFeel.cpp
    Source/EedAutoPanEditor.cpp
    Source/EedAutoPanProcessor.cpp
    Source/EedChorusEditor.cpp
    Source/EedChorusProcessor.cpp
    Source/EedDelayEditor.cpp
    Source/EedDelayProcessor.cpp
    Source/EedDeviceProcessor.cpp
    Source/EedDeviceRegistry.cpp
    Source/EedExciterEditor.cpp
    Source/EedExciterProcessor.cpp
    Source/EedGainEditor.cpp
    Source/EedGainProcessor.cpp
    Source/EedHarmonicAnalysis.cpp
    Source/EedHarmonicCore.cpp
    Source/EedKeyDetectorEditor.cpp
    Source/EedKeyDetectorProcessor.cpp
    Source/EedKeyEngine.cpp
    Source/EedModEditorBase.cpp
    Source/EedCompressorEditor.cpp
    Source/EedCompressorProcessor.cpp
    Source/EedDeEsserEditor.cpp
    Source/EedDeEsserProcessor.cpp
    Source/EedDeviceProcessor.cpp
    Source/EedDeviceRegistry.cpp
    Source/EedDynamicsFaceEditor.cpp
    Source/EedExpanderEditor.cpp
    Source/EedExpanderProcessor.cpp
    Source/EedGainEditor.cpp
    Source/EedGainProcessor.cpp
    Source/EedGateEditor.cpp
    Source/EedGateProcessor.cpp
    Source/EedLimiterEditor.cpp
    Source/EedLimiterProcessor.cpp
    Source/EedMultibandEditor.cpp
    Source/EedMultibandProcessor.cpp
    Source/EedPhaseInvertEditor.cpp
    Source/EedPhaseInvertProcessor.cpp
    Source/EedPhaserEditor.cpp
    Source/EedPhaserProcessor.cpp
    Source/EedPitchEditor.cpp
    Source/EedPitchProcessor.cpp
    Source/EedReverbEditor.cpp
    Source/EedReverbProcessor.cpp
    Source/EedSaturationEditor.cpp
    Source/EedSaturationProcessor.cpp
    Source/EedStereoWidthEditor.cpp
    Source/EedStereoWidthProcessor.cpp
    Source/EedStereoizerEditor.cpp
    Source/EedStereoizerProcessor.cpp
    Source/EedTapeEditor.cpp
    Source/EedTapeMotionView.cpp
    Source/EedTapeProcessor.cpp
    Source/EedTremoloEditor.cpp
    Source/EedTremoloProcessor.cpp
    Source/EqEngine.cpp
    Source/SurgicalEqEditor.cpp
    Source/SurgicalEqProcessor.cpp
    Source/viz/DecayView.cpp
    Source/viz/DeEsserBandView.cpp
    Source/viz/DelayTapsView.cpp
    Source/viz/Goniometer.cpp
    Source/viz/HarmonicBars.cpp
    Source/viz/LevelMeter.cpp
    Source/viz/LfoScopeView.cpp
    Source/viz/PanPositionView.cpp
    Source/viz/SweepView.cpp
    Source/viz/DwellGlow.cpp
    Source/viz/TransferCurveView.cpp
    Source/viz/VizView.cpp
    Source/viz/WaveshaperView.cpp
)
target_include_directories(EchoJayBuiltinRegistryTest PRIVATE Source)

target_compile_definitions(EchoJayBuiltinRegistryTest PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(EchoJayBuiltinRegistryTest PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags)

set_target_properties(EchoJayBuiltinRegistryTest PROPERTIES EXCLUDE_FROM_ALL TRUE)

# ============================================================================
# pitch_probe — offline validation harness for the P0 pitch detector
# (PITCH_CORRECTION_SPEC.md §2.1 / §9).
#
# Runs EedPitchEngine over a real audio file and reports the number the spec
# gates P0 on: octave-guard fires as a percentage of voiced hops. The g++ suite
# proves the estimator on SYNTHETIC material; this is what proves it on
# singing, which has consonants, breath, creak and a room under it.
#
# Its own target, EXCLUDE_FROM_ALL, and deliberately NOT linked into or
# depended on by either plugin target — a validation tool must not be able to
# break or slow a shipping build. The engine is header-only, so this needs no
# device sources at all.
#
#   cmake --build build --target EchoJayPitchProbe
#   ./build/EchoJayPitchProbe_artefacts/Debug/EchoJayPitchProbe vocal.wav --all
# ============================================================================
juce_add_console_app(EchoJayPitchProbe PRODUCT_NAME "EchoJayPitchProbe")
juce_generate_juce_header(EchoJayPitchProbe)

target_sources(EchoJayPitchProbe PRIVATE tools/pitch_probe/main.cpp)
target_include_directories(EchoJayPitchProbe PRIVATE Source)
target_compile_features(EchoJayPitchProbe PRIVATE cxx_std_17)

target_compile_definitions(EchoJayPitchProbe PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(EchoJayPitchProbe PRIVATE
    juce::juce_audio_formats
    juce::juce_recommended_config_flags)

set_target_properties(EchoJayPitchProbe PROPERTIES EXCLUDE_FROM_ALL TRUE)

# ============ Streaming parser test runner (spec step 3) ============
# Console app, juce_core only — runs the read-back suite that holds the
# incremental block parser byte-identical to the whole-reply strip
# (EJReplyBlocks). Never part of a plugin or release build.
option(ECHOJAY_BUILD_STREAM_TESTS "Build EJStreamTests, the streaming parser read-back runner" OFF)
if(ECHOJAY_BUILD_STREAM_TESTS)
    juce_add_console_app(EJStreamTests PRODUCT_NAME "EJStreamTests")
    target_sources(EJStreamTests PRIVATE Tests/test_stream_block_parser.cpp)
    target_compile_features(EJStreamTests PRIVATE cxx_std_17)
    target_link_libraries(EJStreamTests PRIVATE
        juce::juce_core
        juce::juce_recommended_config_flags)
endif()
