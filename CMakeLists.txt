cmake_minimum_required(VERSION 3.22)
# Universal binary: Apple Silicon + Intel
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures")
# Minimum macOS: Big Sur (11.0). Covers anything 4+ years old.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(EchoJay VERSION 2.23.56)

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

set(LINK_VERSION "0.8.4")

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
)

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
