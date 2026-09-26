# ============================================================================================================
#  EchoJay guard suite — ONE CMake test tree (21q, 23 Sep 2026)
# ============================================================================================================
#  WHY. Until now every guard was its own build_and_run.sh calling harness_build.py, which re-derived the compile
#  line from compile_commands.json and recompiled the harness from scratch EVERY time, even when neither the guard
#  nor the library had changed. Thirty-eight guards, one after another, cost ~35 minutes of wall clock and could not
#  use more than one core. Here each guard is a TARGET in one tree: CMake skips what is unchanged, ccache skips what
#  it has compiled before (a rebuilt-but-identical lib, another worktree, a RED snapshot), one precompiled JuceHeader
#  is shared by every guard, and ctest runs them in parallel.
#
#  WHAT IS NOT CHANGED, deliberately: no guard's assertions, no guard's source, the summary shape, and the own_diff
#  check. The isolation rule is unchanged too - every guard still runs with a private HOME / state root / TMPDIR,
#  now in ONE place (tools/tests/run_guard.sh) instead of thirty-eight copies.
#
#  LABELS. "fast" is everything that runs in-process against mocks and built-ins: it runs on every edit.
#  "plugins" is the guard that loads third-party plugins out of process through EchoJayProbe - it needs the user's
#  real plugin folder and a signed probe, so it runs once before packaging, not on every edit.
#
#  Build:  cmake -S . -B build-guards -DEJ_BUILD_GUARDS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-guards -j 8
#  Run:    ctest --test-dir build-guards -L fast -j 4 --output-on-failure
# ============================================================================================================
include(CTest)

find_program(EJ_CCACHE ccache)
if(EJ_CCACHE)
    # BOTH scopes. Set only in PARENT_SCOPE, the variable is invisible HERE, where the guard targets are created -
    # which is how the first cut of this tree ran with no ccache at all.
    set(CMAKE_CXX_COMPILER_LAUNCHER "${EJ_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${EJ_CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${EJ_CCACHE}" PARENT_SCOPE)
    set(CMAKE_C_COMPILER_LAUNCHER   "${EJ_CCACHE}" PARENT_SCOPE)
    message(STATUS "EchoJay guards: ccache at ${EJ_CCACHE}")
else()
    message(STATUS "EchoJay guards: no ccache found - CMake's own up-to-date check still skips unchanged targets")
endif()

# ------------------------------------------------------------------------------------------------------------
# EJ_GUARDS_NO_LTO (default OFF, i.e. the guards are built EXACTLY as the shipping plugin is).
# The one real cost in this tree is link-time optimisation: the shipping archive is bitcode, so every guard's link
# re-runs whole-program codegen - about 140 s per guard, which is what a one-line edit costs. Turning LTO off in
# THIS build directory only (the shipping build-release is untouched) removes that. It is a deviation from the
# flags harness_build.py copied, so it is opt-in and reported, never silent.
# ------------------------------------------------------------------------------------------------------------
option(EJ_GUARDS_NO_LTO "Build the guard tree (and its copy of the plugin library) without LTO" OFF)
if(EJ_GUARDS_NO_LTO)
    foreach(T EchoJay EchoJayLink)
        if(TARGET ${T})
            target_compile_options(${T} PRIVATE -fno-lto)
            target_link_options(${T} PRIVATE -fno-lto)
        endif()
    endforeach()
endif()

set(EJ_GUARD_PCH_OWNER "" CACHE INTERNAL "the first V2-side guard; every other V2 guard reuses its precompiled header")
set(EJ_GUARD_PCH_OWNER_LINK "" CACHE INTERNAL "the first Link-side guard; every other Link guard reuses its precompiled header")

# ej_add_guard(<name> SOURCE <file> LABEL fast|plugins [SCRIBBLE] [EXTRA <files...>])
# The framework set harness_build.py linked every guard with since the first one. Unchanged.
set(EJ_GUARD_FRAMEWORKS "")
foreach(F CoreAudioKit DiscRecording CoreAudio CoreMIDI AudioToolbox Accelerate WebKit Metal MetalKit QuartzCore
          Cocoa Foundation IOKit Security OpenGL UniformTypeIdentifiers AVFoundation CoreMedia AVKit)
    list(APPEND EJ_GUARD_FRAMEWORKS "-framework ${F}")
endforeach()

# ------------------------------------------------------------------------------------------------------------
# JUCE usage requirements WITHOUT JUCE's sources.
# A juce_add_module target hands its consumer both its usage requirements AND its sources (target_sources INTERFACE).
# A guard must have the first and must not have the second - the library it links already contains those objects.
# So walk the module graph once here and keep only the include paths, defines and flags.
# ------------------------------------------------------------------------------------------------------------
set(EJ_JUCE_INCS "")
set(EJ_JUCE_DEFS "")
set(EJ_JUCE_OPTS "")
set(_ej_pending juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp
                juce::juce_gui_extra juce::juce_cryptography juce::juce_opengl)
set(_ej_seen "")
while(_ej_pending)
    list(POP_FRONT _ej_pending _t)
    if(NOT TARGET ${_t})
        continue()
    endif()
    get_target_property(_alias ${_t} ALIASED_TARGET)
    if(_alias)
        set(_t ${_alias})
    endif()
    if(_t IN_LIST _ej_seen)
        continue()
    endif()
    list(APPEND _ej_seen ${_t})
    get_target_property(_v ${_t} INTERFACE_INCLUDE_DIRECTORIES)
    if(_v)
        list(APPEND EJ_JUCE_INCS ${_v})
    endif()
    get_target_property(_v ${_t} INTERFACE_COMPILE_DEFINITIONS)
    if(_v)
        list(APPEND EJ_JUCE_DEFS ${_v})
    endif()
    get_target_property(_v ${_t} INTERFACE_COMPILE_OPTIONS)
    if(_v)
        list(APPEND EJ_JUCE_OPTS ${_v})
    endif()
    get_target_property(_v ${_t} INTERFACE_LINK_LIBRARIES)
    if(_v)
        foreach(_dep IN LISTS _v)
            if(TARGET ${_dep})
                list(APPEND _ej_pending ${_dep})
            endif()
        endforeach()
    endif()
endwhile()
list(REMOVE_DUPLICATES EJ_JUCE_INCS)
list(REMOVE_DUPLICATES EJ_JUCE_DEFS)
list(REMOVE_DUPLICATES EJ_JUCE_OPTS)

set(EJ_JUCE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/ej_guard_pch.h")   # CoreFoundation then JuceHeader; see that file

function(ej_add_guard NAME)
    # LINKSIDE: the handful of guards that assert on the LINK plugin (LinkProcessor), not on V2. They link the
    # Link's own archive and compile against the Link target's defines, exactly as compile_link_harness.sh did.
    cmake_parse_arguments(G "SCRIBBLE;LINKSIDE" "SOURCE;LABEL;WRAPPER;WRAPPER_ARG" "EXTRA" ${ARGN})
    set(EJ_PLUGIN_TARGET EchoJay)
    set(EJ_ARTEFACTS EchoJay_artefacts)
    if(G_LINKSIDE)
        set(EJ_PLUGIN_TARGET EchoJayLink)
        set(EJ_ARTEFACTS EchoJayLink_artefacts)
    endif()
    add_executable(${NAME} ${CMAKE_SOURCE_DIR}/${G_SOURCE} ${G_EXTRA})
    # The guard compiles against the SAME target the plugin's own sources do, so its flags, defines and include
    # paths cannot drift from the library it links - that drift is what harness_build.py existed to prevent.
    # the SAME module set the plugin links, plus the plugin's own static library. juce_audio_plugin_client comes
    # with the plugin target and is what JuceHeader.h expects to find on the include path.
    # LINK LIKE harness_build.py DID, FOR THE SAME REASON. The guard links the plugin's own static library by path
    # ($<TARGET_FILE:EchoJay>, which is "libEchoJay V2_SharedCode.a") and NOT the juce:: module targets, because a
    # JUCE module target carries its sources as an INTERFACE requirement: linking one makes THIS target compile all
    # of JUCE again (that is where the "Objective-C was disabled in PCH file" wall came from, 34 times over).
    # $<COMPILE_ONLY:...> takes each module's include paths, defines and flags WITHOUT its sources and WITHOUT
    # linking it, so the guard compiles exactly as the plugin's own sources do and links the objects already built.
    # LINK LIKE harness_build.py DID, FOR THE SAME REASON. The guard links the plugin's own static library by path
    # ($<TARGET_FILE:EchoJay> == "libEchoJay V2_SharedCode.a") and does NOT link the juce:: module targets, because a
    # JUCE module target carries its sources as an INTERFACE requirement: linking one makes THIS target compile the
    # whole of JUCE again (34 times over). $<COMPILE_ONLY:> does not help - it suppresses the link, not the sources.
    # So the module targets' USAGE REQUIREMENTS are harvested once above (EJ_JUCE_INCS/DEFS/OPTS) and applied here.
    target_link_libraries(${NAME} PRIVATE
        juce::juce_recommended_config_flags
        $<$<NOT:$<BOOL:${EJ_GUARDS_NO_LTO}>>:juce::juce_recommended_lto_flags>
        $<TARGET_FILE:${EJ_PLUGIN_TARGET}>
        ${EJ_GUARD_FRAMEWORKS}
        "-lcurl")
    target_include_directories(${NAME} PRIVATE ${EJ_JUCE_INCS})
    target_compile_definitions(${NAME} PRIVATE ${EJ_JUCE_DEFS})
    target_compile_options(${NAME} PRIVATE ${EJ_JUCE_OPTS})
    if(EJ_GUARDS_NO_LTO)
        target_compile_options(${NAME} PRIVATE -fno-lto)
        target_link_options(${NAME} PRIVATE -fno-lto)
    endif()
    set_target_properties(${NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_dependencies(${NAME} ${EJ_PLUGIN_TARGET})
    # The guard must see EXACTLY what the plugin's own sources see. Rather than restate the flags (which is how a
    # harness drifts from the library it links), read them off the plugin target itself: its private include
    # directories and its compile definitions, which is what harness_build.py copied out of compile_commands.json.
    get_target_property(EJ_TGT_INCS ${EJ_PLUGIN_TARGET} INCLUDE_DIRECTORIES)
    get_target_property(EJ_TGT_DEFS ${EJ_PLUGIN_TARGET} COMPILE_DEFINITIONS)
    if(EJ_TGT_INCS)
        target_include_directories(${NAME} PRIVATE ${EJ_TGT_INCS})
    endif()
    if(EJ_TGT_DEFS)
        target_compile_definitions(${NAME} PRIVATE ${EJ_TGT_DEFS})
    endif()
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/Source
        ${CMAKE_SOURCE_DIR}/tools/guard_common
        ${CMAKE_BINARY_DIR}/${EJ_ARTEFACTS}/JuceLibraryCode)
    set_target_properties(${NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/guards)
    # ONE precompiled JuceHeader per plugin side: the first guard compiles it, every other guard on that side
    # reuses it. Two, not one, because clang rejects a reused PCH whose JucePlugin_* defines differ - and V2's
    # and the Link's do.
    set(EJ_PCH_SLOT EJ_GUARD_PCH_OWNER)
    if(G_LINKSIDE)
        set(EJ_PCH_SLOT EJ_GUARD_PCH_OWNER_LINK)
    endif()
    if("${${EJ_PCH_SLOT}}" STREQUAL "")
        # CXX ONLY. Without the language guard CMake also generates a C precompiled header and compiles it with cc,
        # which fails on <algorithm> - the first thing this tree taught us.
        target_precompile_headers(${NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${EJ_JUCE_HEADER}>")
        set(${EJ_PCH_SLOT} ${NAME} CACHE INTERNAL "" FORCE)
    else()
        target_precompile_headers(${NAME} REUSE_FROM ${${EJ_PCH_SLOT}})
    endif()
    set(ARGS ${CMAKE_BINARY_DIR}/guards/${NAME})
    if(G_SCRIBBLE)
        list(APPEND ARGS --scribble)
    endif()
    # Almost every guard runs through run_guard.sh. WRAPPER is for the one whose verdict is not its exit code.
    set(EJ_RUNNER ${CMAKE_SOURCE_DIR}/tools/tests/run_guard.sh)
    set(EJ_RUNNER_PRE "")
    if(G_WRAPPER)
        set(EJ_RUNNER ${CMAKE_SOURCE_DIR}/tools/tests/${G_WRAPPER})
        set(EJ_RUNNER_PRE ${G_WRAPPER_ARG})
    endif()
    add_test(NAME ${NAME} COMMAND ${EJ_RUNNER} ${EJ_RUNNER_PRE} ${ARGS})
    # WORKING_DIRECTORY is the repo root because that is the cwd harness_build.py ran every guard with, and
    # several guards read the checkout from it (own_diff reads Source/, the grep guards read tools/).
    set_tests_properties(${NAME} PROPERTIES LABELS "${G_LABEL}" TIMEOUT 900 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "EJ_GUARD_HOME_BASE=${CMAKE_BINARY_DIR}/guard-homes")
endfunction()

# ---- the guards, in the order the old suite ran them -------------------------------------------------------
ej_add_guard(land_by_name_guard       SOURCE tools/land_by_name_guard/harness.cpp             LABEL fast SCRIBBLE)
ej_add_guard(stepped_position_guard   SOURCE tools/stepped_position_guard/harness.cpp         LABEL fast SCRIBBLE)
ej_add_guard(limiter_wall_guard       SOURCE tools/limiter_wall_guard/harness.cpp             LABEL fast SCRIBBLE)
ej_add_guard(loudness_loop_guard      SOURCE tools/loudness_loop_guard/harness.cpp            LABEL fast SCRIBBLE)
ej_add_guard(level_slot_guard         SOURCE tools/level_slot_guard/harness.cpp               LABEL fast SCRIBBLE)
ej_add_guard(pair_dial_guard          SOURCE tools/pair_dial_guard/harness.cpp                LABEL fast SCRIBBLE)
ej_add_guard(deltas_guard             SOURCE tools/deltas_guard/harness.cpp                   LABEL fast SCRIBBLE)
ej_add_guard(bubble_parity_guard      SOURCE tools/bubble_parity_guard/harness.cpp            LABEL fast SCRIBBLE)
ej_add_guard(substitute_guard         SOURCE tools/substitute_guard/harness.cpp               LABEL fast SCRIBBLE)
ej_add_guard(dialinfo_keep_guard      SOURCE tools/dialinfo_keep_guard/harness.cpp            LABEL fast SCRIBBLE)
ej_add_guard(near_map_guard           SOURCE tools/near_map_guard/harness.cpp                 LABEL fast SCRIBBLE)
ej_add_guard(dial_wait_guard          SOURCE tools/dial_wait_guard/harness.cpp                LABEL fast SCRIBBLE)
ej_add_guard(preflight_guard          SOURCE tools/preflight_guard/harness.cpp                LABEL plugins SCRIBBLE)
ej_add_guard(checklist_reason_guard   SOURCE tools/checklist_reason_guard/harness.cpp         LABEL fast)
ej_add_guard(scrollbar_contrast_guard SOURCE tools/scrollbar_contrast_guard/harness.cpp       LABEL fast)
ej_add_guard(stream_reroute_guard     SOURCE tools/stream_reroute_guard/harness.cpp           LABEL fast)
ej_add_guard(borrow_budget_guard      SOURCE tools/borrow_budget_guard/harness.cpp            LABEL fast)
ej_add_guard(overlay_liveness_guard   SOURCE tools/overlay_liveness_guard/harness.cpp         LABEL fast)
ej_add_guard(wet_rebuild_guard        SOURCE tools/wet_rebuild_guard/harness.cpp              LABEL fast)
ej_add_guard(slotwet_readback         SOURCE tools/slotwet_readback/slotwet_readback.cpp      LABEL fast LINKSIDE)
ej_add_guard(slotwet_knobpath         SOURCE tools/slotwet_knobpath/harness.cpp               LABEL fast)
ej_add_guard(slotwet_nomutation       SOURCE tools/slotwet_nomutation/slotwet_nomutation.cpp  LABEL fast)
ej_add_guard(overlay_gate_harness     SOURCE tools/overlay_gate_harness/overlay_gate_harness.cpp LABEL fast)
ej_add_guard(key_gate_harness         SOURCE tools/key_gate_harness/key_gate_harness.cpp      LABEL fast)
ej_add_guard(detected_key_text        SOURCE tools/detected_key_text/harness.cpp              LABEL fast)
ej_add_guard(attach_order             SOURCE tools/attach_order/attach_order.cpp              LABEL fast)
ej_add_guard(borrowkept_harness       SOURCE tools/borrowkept_harness/borrowkept_harness.cpp  LABEL fast)
ej_add_guard(global_undo_guard        SOURCE tools/global_undo_guard/harness.cpp              LABEL fast SCRIBBLE)
ej_add_guard(groups_guard             SOURCE tools/groups_guard/harness.cpp                   LABEL fast SCRIBBLE)
ej_add_guard(ui_guard                 SOURCE tools/ui_round_guard/ui_guard.cpp                LABEL fast SCRIBBLE)
ej_add_guard(history_guard            SOURCE tools/ui_round_guard/history_guard.cpp           LABEL fast)
ej_add_guard(classify_body_guard      SOURCE tools/classify_body_guard/harness.cpp            LABEL fast)
ej_add_guard(latch_harness            SOURCE tools/latch_harness/harness.cpp                  LABEL fast)
ej_add_guard(eq_readback              SOURCE tools/eq_readback/harness.cpp                    LABEL fast)
ej_add_guard(pregain_readback         SOURCE tools/pregain_readback/harness.cpp               LABEL fast)
# own_diff's verdict is the sha256 of the own-channel text it writes, not its exit code, so it runs through a
# wrapper that keeps the suite script's check (shasum -a 256 /tmp/own_out.txt against the 19 Sep baseline).
ej_add_guard(mapfps_sync_guard        SOURCE tools/mapfps_sync_guard/harness.cpp              LABEL fast SCRIBBLE)
ej_add_guard(own_diff                 SOURCE tools/own_diff/harness.cpp                       LABEL fast
             WRAPPER run_own_diff.sh WRAPPER_ARG 5f47897eebef7116c74c35d35ab8b461cfb2333c3014269e339612d5244b25ce)


# ---- the guards that are not a single in-process binary -----------------------------------------------------
# Kept as their own scripts on purpose: each is a two-process orchestration (a real LinkProcessor against the Link
# archive plus a real EchoJayProcessor against the V2 one), a grep over the source tree, or a python driver. They
# gain the label and the parallel run; their internals are untouched.
add_test(NAME probe_plist_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/probe_plist_guard/build_and_run.sh)
set_tests_properties(probe_plist_guard PROPERTIES LABELS "fast" TIMEOUT 600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME temp_var_grep_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/temp_var_grep_guard/build_and_run.sh)
set_tests_properties(temp_var_grep_guard PROPERTIES LABELS "fast" TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME state_path_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/state_path_guard/build_and_run.sh)
set_tests_properties(state_path_guard PROPERTIES LABELS "fast" TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# 21t-d: one process, but the same shape - a real LinkProcessor against the Link archive, so it lives here.
add_test(NAME calib_link_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/calib_link_guard/build_and_run.sh)
set_tests_properties(calib_link_guard PROPERTIES LABELS "fast" TIMEOUT 900 RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
foreach(TWO alias_mirror_guard lease_id_guard)
    add_test(NAME ${TWO} COMMAND bash ${CMAKE_SOURCE_DIR}/tools/${TWO}/build_and_run.sh)
    set_tests_properties(${TWO} PROPERTIES LABELS "fast" TIMEOUT 900 RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
add_test(NAME linkmeter_harness COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/linkmeter_harness/build_and_run.py)
set_tests_properties(linkmeter_harness PROPERTIES LABELS "fast" TIMEOUT 900 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "EJ_GUARD_HOME_BASE=${CMAKE_BINARY_DIR}/guard-homes")
