# ============================================================================
# EchoJayProbe on feat/ejmap-cert: the signed out-of-process instrument EJ Map
# certification drives. One process per product, the real home, _Exit.
#
# The target block is merge/kathy-2026-09-06's (root CMakeLists.txt:735-777 at
# 971e4c1) with ONE deliberate difference: there is no loop copying the probe
# into the six plugin bundles. On this branch the probe is a standalone binary;
# whether a harness-bearing probe ever ships inside a plugin bundle is the merge
# decision, and it stays open. Everything else - definitions, modules, both
# architectures, the linked Info.plist - is theirs unchanged.
#
# Add from the repo root with:   add_subdirectory(tools/au_instantiate_probe)
# (after add_subdirectory(JUCE), so juce_add_console_app is defined).
#
# THE ARTEFACT PATH IS PINNED to <build>/EchoJayProbe_artefacts/<config>/, where
# the root-level target put it on the kathy line. JUCE would otherwise nest it
# under tools/au_instantiate_probe/, and probe_plist_guard, which arrived
# unmodified as a subtree, looks at build-release/EchoJayProbe_artefacts/.
#
# NOT THE SIGNED ARTEFACT. A build produces an unsigned probe, and PACE refuses
# an unsigned one ("fatal wrapper bootstrap error", 21 Sep, df407ab). Sign it
# before pointing it at any plugin:
#   codesign -f -s "Developer ID Application: Sean Donoghue (8BT5F9B887)" \
#     --timestamp --options runtime \
#     --entitlements tools/au_instantiate_probe/EchoJayProbe.entitlements <binary>
# ============================================================================
juce_add_console_app(EchoJayProbe PRODUCT_NAME "EchoJayProbe")
target_sources(EchoJayProbe PRIVATE au_instantiate_probe.cpp)
juce_generate_juce_header(EchoJayProbe)
target_compile_definitions(EchoJayProbe PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(EchoJayProbe PRIVATE
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_recommended_config_flags)
set_target_properties(EchoJayProbe PROPERTIES
    OSX_ARCHITECTURES "arm64;x86_64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/EchoJayProbe_artefacts/$<CONFIG>")
if(APPLE)
    # 21t-e: a BARE executable has no bundle, so its Info.plist is linked into
    # the binary. LSBackgroundOnly keeps a sampling pass out of the Dock.
    target_link_options(EchoJayProbe PRIVATE
        "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/EchoJayProbe-Info.plist")
    target_link_libraries(EchoJayProbe PRIVATE "-framework CoreAudioKit")
endif()
