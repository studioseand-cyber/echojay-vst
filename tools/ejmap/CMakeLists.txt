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

# Git short hash into the title bar, stamped at BUILD time.
#
# This used to be an execute_process here, at configure time. That is wrong for
# the one job it has: after a commit, a plain rebuild kept the old hash, so the
# title bar could name a commit the binary predated. It also could not say the
# tree was dirty. StampBuildInfo.cmake regenerates the header on every build and
# appends "-dirty" when tracked files are modified.
set(EJMAP_BUILD_INFO_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(EJMAP_BUILD_INFO_H   ${EJMAP_BUILD_INFO_DIR}/EjmapBuildInfo.h)
file(MAKE_DIRECTORY ${EJMAP_BUILD_INFO_DIR})

add_custom_target(ejmap-build-stamp ALL
    COMMAND ${CMAKE_COMMAND}
            -D SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -D OUT_FILE=${EJMAP_BUILD_INFO_H}
            -D VERSION=${EJMAP_VERSION}
            -D APPLY_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/../../Source/EchoJayParamApply.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StampBuildInfo.cmake
    BYPRODUCTS ${EJMAP_BUILD_INFO_H}
    COMMENT "Stamping ejmap build info (git hash + dirty flag)"
    VERBATIM)

# Generate it once now so the first configure leaves a compilable tree.
execute_process(COMMAND ${CMAKE_COMMAND}
    -D SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -D OUT_FILE=${EJMAP_BUILD_INFO_H}
    -D VERSION=${EJMAP_VERSION}
        -D APPLY_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/../../Source/EchoJayParamApply.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StampBuildInfo.cmake)

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
    Source/PluginHost.cpp
    Source/EjmapSend.mm
    Source/EjmapViewLayer.mm)

target_compile_definitions(ejmap PRIVATE
    EJMAP_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ejmap,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ejmap,JUCE_VERSION>"
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    # The editor-ready wait must pump the message loop while it waits: a bridged
    # AU editor gets its real size over XPC ON the message thread, so blocking
    # that thread would guarantee the timeout the wait exists to avoid. JUCE
    # gates runDispatchLoopUntil behind this flag, which is off by default
    # because a plugin must never run a modal loop inside its host. ejmap is a
    # standalone app, not a plugin, so the reason for the default does not apply.
    JUCE_MODAL_LOOPS_PERMITTED=1)
# EJMAP_VERSION / EJMAP_GIT_HASH now come from the generated EjmapBuildInfo.h,
# never from a configure-time define.

# SHARED CODE, NOT COPIES.
#
# ejmap and EchoJay must compile the same headers. Two implementations of map
# semantics drift within a week, exactly as the dial predicate did. These are
# include paths, never file copies, and the round-trip test below enforces it.
add_dependencies(ejmap ejmap-build-stamp)

target_include_directories(ejmap PRIVATE
    ${EJMAP_BUILD_INFO_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source      # EchoJayParamApply.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ejextract)     # EchoJayParamExtractor.h

target_link_libraries(ejmap PRIVATE
    juce::juce_cryptography    # fingerprintForDescription (SHA256), M4 fp lookup
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    target_link_libraries(ejmap PRIVATE "-framework AudioToolbox" "-framework CoreAudioKit"
                                        "-framework Network")   # Option A: the TLS stream
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
    EJMAP_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    # Matches the ejmap target. The gate now tests rules that live in
    # EjmapAssignment.h, which pulls in EjmapSweeper.h, whose settle hook calls
    # runDispatchLoopUntil -- gated behind this flag. Without it the TEST
    # target fails to compile while the app still builds, so the gate silently
    # runs a stale binary and reports a pass. Nothing here runs a modal loop;
    # this only makes the header compilable.
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_include_directories(ejmap-roundtrip-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../ejextract)

target_link_libraries(ejmap-roundtrip-test PRIVATE
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    juce::juce_cryptography)

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

#==============================================================================
# Watchdog test: does the deadline actually fire?
#
# It ends in _Exit, so it cannot assert on itself. The test IS the hang; the
# shell around it checks exit code 87 and the rows left on disk. Writes to a
# throwaway directory, never the real ledger.

juce_add_console_app(ejmap-watchdog-test PRODUCT_NAME "ejmap-watchdog-test")

target_sources(ejmap-watchdog-test PRIVATE tests/WatchdogTest.cpp)

target_compile_definitions(ejmap-watchdog-test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_include_directories(ejmap-watchdog-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source
    ${CMAKE_CURRENT_SOURCE_DIR}/../ejextract)

target_link_libraries(ejmap-watchdog-test PRIVATE
    juce::juce_core
    juce::juce_events
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    target_link_libraries(ejmap-watchdog-test PRIVATE "-framework CoreAudioKit")
endif()

#==============================================================================
# Arch probe: why does the scan get no description from the Waves VST3 shells?
#
# A probe, not pipeline code. It answers one question with a number and is meant
# to be run BOTH natively and under `arch -x86_64`, because the answer is the
# pairing of the bundle's architectures with the running process's.

juce_add_console_app(ejmap-arch-probe PRODUCT_NAME "ejmap-arch-probe")
target_sources(ejmap-arch-probe PRIVATE tests/ArchProbe.cpp Source/EjmapViewLayer.mm)
target_compile_definitions(ejmap-arch-probe PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    # runDispatchLoopUntil is only declared with a modal loop available, and the
    # probe has to let a panel settle before judging it empty.
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_include_directories(ejmap-arch-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(ejmap-arch-probe PRIVATE
    juce::juce_core juce::juce_events juce::juce_gui_basics juce::juce_audio_processors
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(APPLE)
    target_link_libraries(ejmap-arch-probe PRIVATE "-framework CoreAudioKit")
endif()
