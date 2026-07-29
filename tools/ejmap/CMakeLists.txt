# ejmap: standalone plugin mapping tool
#
# Add from the repo root with:   add_subdirectory(tools/ejmap)
#
# Build in a DEDICATED directory. Both plugin worktrees use build/, and two
# cmake --build runs sharing a directory have already collided once on this
# project:
#
#     cmake -B build-ejmap -DCMAKE_BUILD_TYPE=RelWithDebInfo
#     cmake --build build-ejmap --target ejmap -j8
#
# ejmap does not install to the AU component path. It is an app, so it cannot
# overwrite a plugin build and a plugin build cannot overwrite it.

cmake_minimum_required(VERSION 3.22)

set(EJMAP_VERSION "0.1.0")

# Git short hash into the title bar. The on-screen version is the only proof of
# what is loaded.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE EJMAP_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT EJMAP_GIT_HASH)
    set(EJMAP_GIT_HASH "nogit")
endif()

juce_add_gui_app(ejmap
    PRODUCT_NAME            "ejmap"
    COMPANY_NAME            "EchoJay"
    BUNDLE_ID               "ai.echojay.ejmap"
    VERSION                 ${EJMAP_VERSION}
    MICROPHONE_PERMISSION_ENABLED FALSE
    HARDENED_RUNTIME_ENABLED TRUE)

target_sources(ejmap PRIVATE
    Source/Main.cpp
    Source/PluginScanner.cpp
    Source/PluginHost.cpp)

target_compile_definitions(ejmap PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ejmap,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ejmap,JUCE_VERSION>"
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    EJMAP_VERSION="${EJMAP_VERSION}"
    EJMAP_GIT_HASH="${EJMAP_GIT_HASH}")

# SHARED CODE, NOT COPIES.
#
# ejmap and EchoJay must compile the same headers. Two implementations of map
# semantics drift within a week, exactly as the dial predicate did. These are
# include paths, never file copies, and the round-trip test below enforces it.
target_include_directories(ejmap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source      # EchoJayParamApply.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ejextract)     # EchoJayParamExtractor.h

target_link_libraries(ejmap PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    target_link_libraries(ejmap PRIVATE "-framework AudioToolbox" "-framework CoreAudioKit")
endif()

#==============================================================================
# Round-trip test: the drift guard.
#
# A map produced by ejmap must round-trip through EchoJay's applySettings and
# produce the identical writes the mapper verified. If the shared header changes
# and the two paths diverge, this fails before the commit lands.
#
# Wire into the pre-commit hook on feat/ejmap.

juce_add_console_app(ejmap-roundtrip-test
    PRODUCT_NAME "ejmap-roundtrip-test")

target_sources(ejmap-roundtrip-test PRIVATE
    tests/RoundTripTest.cpp)

target_compile_definitions(ejmap-roundtrip-test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1)

target_include_directories(ejmap-roundtrip-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../ejextract)

target_link_libraries(ejmap-roundtrip-test PRIVATE
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# JUCE_PLUGINHOST_AU=1 above compiles the AU hosting path into
# juce_audio_processors, which references AUGenericView. Same framework
# ejextract and the ejmap app already link for the same reason.
if(APPLE)
    target_link_libraries(ejmap-roundtrip-test PRIVATE "-framework CoreAudioKit")
endif()

#==============================================================================
# Registry conformance test: does the description we derive WITHOUT loading a
# plugin match the one JUCE fills in from a live instance?
#
# describeFromRegistry is a hand-written mirror of JUCE's
# fillInPluginDescription, and mirrors drift. This is the only thing that would
# notice.
#
# NOT part of a normal build loop: it instantiates real plugins. Bounded to 10,
# spread across vendors, never a full scan. Run it when the shared header or
# JUCE changes.

juce_add_console_app(ejmap-registry-conformance-test
    PRODUCT_NAME "ejmap-registry-conformance-test")

target_sources(ejmap-registry-conformance-test PRIVATE
    tests/RegistryConformanceTest.cpp
    Source/PluginScanner.cpp)

target_compile_definitions(ejmap-registry-conformance-test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1)

target_include_directories(ejmap-registry-conformance-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../ejextract)

target_link_libraries(ejmap-registry-conformance-test PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    target_link_libraries(ejmap-registry-conformance-test PRIVATE
        "-framework AudioToolbox" "-framework CoreAudioKit")
endif()
