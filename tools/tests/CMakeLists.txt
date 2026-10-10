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
        # 10 Oct 2026, at the integration merge: the V2 archive now REFERENCES integration's binary-data library
        # (juce_add_binary_data(EchoJayPlaybackArt), the Playback grid's pictures), so every guard that links the
        # archive needs it too or the link fails on EJPlaybackArt::club_floor_jpg and five siblings. The plugin
        # target links it at CMakeLists.txt:441; this is the same dependency, stated once for the guard tree.
        # Only for the V2 side - the Link archive does not reference the art.
        $<$<STREQUAL:${EJ_PLUGIN_TARGET},EchoJay>:EchoJayPlaybackArt>
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
    # 8 Oct 2026: A BIGGER MAIN-THREAD STACK, NOT A SIDE THREAD. Some guards hold several EchoJayProcessors as
    # locals in one frame, and an EchoJayProcessor is large: ui_guard tipped over the 8 MB default the moment
    # 06d item 6/10 added two clocks and a std::function to LoudnessLoop (a processor member). The crash is three
    # frames deep - start -> main -> ___chkstk_darwin, SIGSEGV at a stack address - which is a stack overflow and
    # not a product fault. Moving the work to a pthread with room DOES fix the overflow and breaks four GUI legs,
    # because a JUCE GUI guard's message thread has to be the thread it was initialised on. So the stack itself is
    # made bigger at link time and everything stays exactly where it was.
    target_link_options(${NAME} PRIVATE -Wl,-stack_size,0x20000000)   # 512 MB
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
# 21t-m (29 Sep 2026 ruling): THE END-TO-END LEVEL HARNESS. Real ChainHost, real tallies, real EJCalibLoop,
# generated audio - the hold had been fixed six times from Pro Tools logs, one symptom a round, and this is where
# its RED lives from now on. Sits beside level_slot_guard because they share the rig's shape.
ej_add_guard(level_loop_guard         SOURCE tools/level_loop_guard/harness.cpp               LABEL fast SCRIBBLE)
# COMP_PROFILE_SPEC_v1 section 5: the level the plugin sends with a build. Pure arithmetic on a synthetic signal,
# so it needs no product sources beyond the header under test.
ej_add_guard(track_level_guard        SOURCE tools/track_level_guard/harness.cpp              LABEL fast SCRIBBLE)
# COMP_PROFILE_SPEC_v1 items 2 and 3: the profile on the slot, and the one check. Hosts a REAL plugin, because a
# profile joins by map fingerprint and a built-in has none.
ej_add_guard(comp_profile_guard       SOURCE tools/comp_profile_guard/harness.cpp             LABEL fast SCRIBBLE)
# The plugin against the SERVER'S OWN generated output (~/Desktop/ej_contract). Passes with a note when the drop is
# absent: a missing contract is not a plugin fault, and inventing a fixture would defeat the point.
ej_add_guard(comp_contract_guard      SOURCE tools/comp_contract_guard/harness.cpp            LABEL fast SCRIBBLE)
# COMP_PROFILE_SPEC_v1 section 8: the acceptance check. A TOOL, not a test - it loads a real AU and renders audio
# through it, so it is built and run by hand (or by EJ Maps) and carries the label "none" so no gate runs it.
ej_add_guard(comp_render_check        SOURCE tools/comp_render_check/main.cpp                 LABEL none SCRIBBLE)
# ...and its PRE-FIX reproducer, registered so it BUILDS with the tree (a RED that does not compile is not a RED)
# but with NO label, so no gate runs it: it is the same rig cut down to what exists before this round's fix, and
# it is run by hand against a stashed Source/ to show the wind-up happening. Its output is quoted in MERGE.
ej_add_guard(level_loop_red_prefix    SOURCE tools/level_loop_guard/red_prefix.cpp             LABEL none SCRIBBLE)
ej_add_guard(pair_dial_guard          SOURCE tools/pair_dial_guard/harness.cpp                LABEL fast SCRIBBLE)
ej_add_guard(deltas_guard             SOURCE tools/deltas_guard/harness.cpp                   LABEL fast SCRIBBLE)
ej_add_guard(bubble_parity_guard      SOURCE tools/bubble_parity_guard/harness.cpp            LABEL fast SCRIBBLE)
ej_add_guard(substitute_guard         SOURCE tools/substitute_guard/harness.cpp               LABEL fast SCRIBBLE)
ej_add_guard(dialinfo_keep_guard      SOURCE tools/dialinfo_keep_guard/harness.cpp            LABEL fast SCRIBBLE)
# 06d item 1 (7 Oct 2026): the network census - no worker outlives the last processor. SCRIBBLE because the
# crash it closes was only ever seen under MallocScribble, so this guard runs the same way the evidence did.
ej_add_guard(net_quiet_guard          SOURCE tools/net_quiet_guard/harness.cpp               LABEL fast SCRIBBLE)
# 06d test 5 (7 Oct 2026): the staged proposal - never printed, kept on the wire, applied on a yes.
ej_add_guard(proposal_guard           SOURCE tools/proposal_guard/harness.cpp                LABEL fast)
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
# 8 Oct 2026: linksync_test JOINS THE GATE, which is how it comes to be trusted at all. It had its own
# build_and_run.sh, and that script (a) linked build/EchoJayLink_artefacts - a 30 SEPTEMBER archive, so it was
# asserting against object code eight days old - and (b) never exported EJ_STATE_TEST_HOME, so the harness's own
# isolation precondition refused it: "app-data resolves to /nonexistent/Library". A test that cannot run, against a
# library that is not the one being changed, is not evidence. In the tree it links the Link archive by path like
# every other LINKSIDE guard and takes its private state root from run_guard.sh.
ej_add_guard(linksync_test            SOURCE tools/linksync_test/linksync_test.cpp           LABEL fast LINKSIDE)
ej_add_guard(slotwet_knobpath         SOURCE tools/slotwet_knobpath/harness.cpp               LABEL fast)
# 21t-i re-cut (27 Sep 2026): the tuner's WIRE LADDER (natural/balanced/hard/snap, "tuned" an alias) is ruled
# behaviour, so its test joins the gate. pitch_mode_test has lived outside it since it was written - it also renders
# editor snapshots under EJ_EDITOR_SNAP - and outside the gate is where a ruled behaviour goes unguarded between
# rounds. It runs here WITHOUT that variable, so no snapshot is written and the mode/scale assertions are the test.
ej_add_guard(pitch_mode_test           SOURCE tools/pitch_mode_test/main.cpp                   LABEL fast)
# 21t-j: landing a dB target by readback on a control sampled only at -inf / -24 / 0 (B's contract note 3).
ej_add_guard(readback_search_guard     SOURCE tools/readback_search_guard/harness.cpp          LABEL fast)
ej_add_guard(slotwet_nomutation       SOURCE tools/slotwet_nomutation/slotwet_nomutation.cpp  LABEL fast)
ej_add_guard(overlay_gate_harness     SOURCE tools/overlay_gate_harness/overlay_gate_harness.cpp LABEL fast)
ej_add_guard(key_gate_harness         SOURCE tools/key_gate_harness/key_gate_harness.cpp      LABEL fast)
ej_add_guard(detected_key_text        SOURCE tools/detected_key_text/harness.cpp              LABEL fast)
ej_add_guard(attach_order             SOURCE tools/attach_order/attach_order.cpp              LABEL fast)
# 4 Oct 2026 (Kathy): a hosted plugin's SIDECHAIN bus must never be fed silence. A keyed mock that only compresses
# off a non-silent key, on BOTH host modes, because rebuildGraph is shared. The bug it holds fixed made real Waves
# compressors (C1, RComp, SSLComp, VComp, dbx-160) do nothing at all, with no error anywhere.
ej_add_guard(sidechain_guard          SOURCE tools/sidechain_guard/harness.cpp               LABEL fast SCRIBBLE)
# 5 Oct 2026: a hosted plugin is disposed at RELEASE and NEVER inside ~ChainHost/AP_Close, which is where the
# Softube CL 1B's ACFShutdown aborts the AU host service. Asserts BOTH directions, because leaking everything would
# also "pass" the teardown half.
ej_add_guard(teardown_dispose_guard   SOURCE tools/teardown_dispose_guard/harness.cpp        LABEL fast SCRIBBLE)
ej_add_guard(borrowkept_harness       SOURCE tools/borrowkept_harness/borrowkept_harness.cpp  LABEL fast)
ej_add_guard(global_undo_guard        SOURCE tools/global_undo_guard/harness.cpp              LABEL fast SCRIBBLE)
ej_add_guard(groups_guard             SOURCE tools/groups_guard/harness.cpp                   LABEL fast SCRIBBLE)
ej_add_guard(ui_guard                 SOURCE tools/ui_round_guard/ui_guard.cpp                LABEL fast SCRIBBLE)
# Session A2's agent client + panel legs (docs/AGENT_UI_HOOKS.md section 5): a recorder transport and a fake
# executor, so it meets no server. First compiled in the 9 Oct merge round.
ej_add_guard(agent_client_guard       SOURCE tools/agent_client_guard/harness.cpp             LABEL fast)
# A2's read-only executor (look / check / checkpoint / the playback window), first compiled 10 Oct
ej_add_guard(agent_executor_read_guard SOURCE tools/agent_executor_read_guard/harness.cpp      LABEL fast)
ej_add_guard(history_guard            SOURCE tools/ui_round_guard/history_guard.cpp           LABEL fast)
ej_add_guard(classify_body_guard      SOURCE tools/classify_body_guard/harness.cpp            LABEL fast)
ej_add_guard(shm_layout_guard         SOURCE tools/shm_layout_guard/harness.cpp               LABEL fast)
ej_add_guard(wire_tokens_guard        SOURCE tools/wire_tokens_guard/harness.cpp              LABEL fast)
# 21t-h (27 Sep 2026): linkmixer_test holds the stripSelected truth table and was NOT in the gate at all - it had
# its own build_and_run.sh and nothing ran it. A pure predicate with a truth table nobody runs is a document. It is
# registered here through its own script (it lifts the real compile flags and links the shipped SharedCode, which
# ej_add_guard's single-TU shape cannot do).
add_test(NAME linkmixer_test COMMAND bash ${CMAKE_SOURCE_DIR}/tools/linkmixer_test/build_and_run.sh)
set_tests_properties(linkmixer_test PROPERTIES LABELS "fast" TIMEOUT 600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
ej_add_guard(latch_harness            SOURCE tools/latch_harness/harness.cpp                  LABEL fast)
ej_add_guard(eq_readback              SOURCE tools/eq_readback/harness.cpp                    LABEL fast)
ej_add_guard(pregain_readback         SOURCE tools/pregain_readback/harness.cpp               LABEL fast)
# own_diff's verdict is the sha256 of the own-channel text it writes, not its exit code, so it runs through a
# wrapper that keeps the suite script's check (shasum -a 256 /tmp/own_out.txt against the baseline).
#
# RE-BASELINED 29 Sep 2026 BY RULING (21t-k), and only after the diff was read and reported RED overnight: 65
# lines ADDED, none removed, 4,953 -> 14,469 bytes. Item 2(b) put EchoJay's own devices in the recommendable feed
# on every send, and the own_diff fixture is a machine with NO scanned plugins - so where the feed used to be
# empty and the injection omitted them entirely, it now carries [AVAILABLE PLUGINS], the CHAIN BLOCK RULE and the
# AVAILABLE BUILTINS descriptions. That is the ruled behaviour, not a regression: the product already treated the
# built-ins as the right feed for a machine with nothing recommendable (the feed-split fallback), and the ruling
# generalised it. The text is OWN_CHANNEL_BASELINE_c7b142a.txt so the next move can be diffed rather than guessed.
#
# PREVIOUSLY: re-baselined 27 Sep 2026 by ruling, from 5f47897e..., for the ruled short90 token on the
# [CHAIN LEVELS] header (OWN_CHANNEL_BASELINE_efdec55.txt).
#
# RE-BASELINED 10 OCT 2026 BY SEAN'S RULING ("own_diff: approved. Pin e16eb2d8 and record the three reasons"),
# after the diff was read, measured and REPORTED RED first, and after the one line that was NOT covered by the
# standing authorisation was put to Sean rather than waved through.
#
#   pin  4bf16c07 (OWN_CHANNEL_BASELINE_6d42a67.txt, 14,530 bytes)
#   ->   e16eb2d8 (OWN_CHANNEL_BASELINE_87600ab.txt, 15,586 bytes)
#
# THE THREE REASONS, each a ruled product change, diffed three ways (against 4bf16c07, the match pin, and against
# 300ee864 = OWN_CHANNEL_BASELINE_70f5407.txt, limiter v2's own pin) so that no fourth change could hide:
#
#   1. THE LIMITER'S SCHEMA (limiter v2). The mode list becomes
#      transparent|punchy|clip|modern|allround; NEW attack_ms 275, link_pct 75, release_link_pct 100; and the
#      Pro-L 2 defaults for ceiling_db (0.0), release_ms (400), lookahead_ms (0..5, 0.18) and true_peak (on).
#      Sean pre-ruled this one: "own_diff will move by the limiter's mode-list line only ... record the new sha".
#
#   2. THE "match" OPTION. loudness_option gains |match - the CHANNEL/BUS aim from CONTRACT_LEVEL_PARAMS, a
#      chain that must not change the level. Ruled 9 Oct; already in the 4bf16c07 pin, and listed here because
#      it is one of the three changes the current text carries against 300ee864.
#
#   3. THE SETTINGS PARAGRAPH: TWO TOGGLES BECOME THREE. "Only use EchoJay's own devices (no third-party
#      plugins)" joins Dial-only and never-dial, and the paragraph now spells out which of the three is about
#      WHICH PLUGINS ARE OFFERED, which about WHETHER THEY MUST BE DIALABLE, and which about WHETHER VALUES ARE
#      WRITTEN. This is Sean's 10 Oct EchoJay-only ruling ("keep it as its OWN button, separate from Dial only")
#      reaching the own-channel injection.
#      THIS LINE WAS NOT COVERED by the standing authorisation, which said "limiter and match lines only". It is
#      a consequence of a ruling Sean had given, but it was not the ruling he gave ABOUT THIS GUARD, so the pin
#      was left alone and the question was asked. He approved it explicitly. Recorded at length because the
#      next person to see a third change in this text should ask too, not reason from this precedent.
#
# NOTHING ELSE MOVED: 6 lines removed, 9 added, and every one of them belongs to a reason above.
#
# The two rulings this supersedes are kept verbatim below, because each was a separate decision and the record of
# WHY a baseline moved is the only thing that makes the next move diffable.
#
# [A, 9 Oct] RE-BASELINED 9 OCT 2026 BY RULING (Sean), and only after the diff was read and reported RED first. ONE LINE
# [A, 9 Oct] changed, 14,469 -> 14,530 bytes, and it is the 08c item F2 level contract showing up in the own-channel
# [A, 9 Oct] injection because that text prints the built-in schemas:
#
# [A, 9 Oct]   -  loudness_option (commercial|pushed|dynamic|keep, default commercial) - which loudness the target came from
# [A, 9 Oct]   +  loudness_option (commercial|pushed|dynamic|keep|match, default commercial) - ...; "match" means the aim is
# [A, 9 Oct]      this chain's own input level
#
# [A, 9 Oct] "match" is the CHANNEL/BUS aim from CONTRACT_LEVEL_PARAMS: a chain that must not change the level. It is a ruled
# [A, 9 Oct] product change, not a regression. The text is OWN_CHANNEL_BASELINE_6d42a67.txt so the next move can be diffed
# [A, 9 Oct] rather than guessed.
#
# [A, 9 Oct] PROVEN BOTH DIRECTIONS, on the same text and two baselines: against 46da20e4 (the old one) this text is RED,
# [A, 9 Oct] which is the run that reported it; against 4bf16c07 it is GREEN. A baseline that only ever passes is as useless
# [A, 9 Oct] as one that only ever fails.
#
# [A, 9 Oct] PREVIOUSLY: re-baselined 29 Sep 2026 from the 21t-k feed change (OWN_CHANNEL_BASELINE_c7b142a.txt), and 27 Sep
# [A, 9 Oct] from the short90 token (OWN_CHANNEL_BASELINE_efdec55.txt).
#
# [L, 9 Oct] RE-BASELINED 9 Oct 2026 BY SEAN'S RULING (limiter v2 gate round c), after session G read and reported the diff:
# [L, 9 Oct] 11 lines changed against c7b142a, every one the EchoJay Limiter's schema text (ceiling_db default 0, release_ms 400,
# [L, 9 Oct] NEW attack_ms 275, lookahead_ms 0..5 default 0.18, NEW link_pct 75, NEW release_link_pct 100, true_peak on), zero
# [L, 9 Oct] non-limiter lines; 14,469 -> 15,028 bytes. The text is OWN_CHANNEL_BASELINE_70f5407.txt (the gated commit).
# [L, 9 Oct] "own_diff re-baselined by Sean's ruling, limiter schema only."
#
# THE STANDING RULE IS UNCHANGED for every future move: if this goes RED, STOP AND REPORT - never re-baseline.
# 10 Oct 2026: the user-data WRITE policy, executed in the gate. Source/EJUserDataWrite.h was made a pure
# predicate precisely so a suite could RUN the rule instead of grepping EchoJayAPI.cpp for it (open list 217) -
# and the only thing running it was tools/mapfps_test, which is NOT REGISTERED HERE. A predicate nobody executes
# in the gate is a text pin with extra steps. See the guard's own opening comment for both defects it covers.
ej_add_guard(userdata_write_guard     SOURCE tools/userdata_write_guard/harness.cpp          LABEL fast SCRIBBLE)
# 10 Oct 2026, remote control stage 2: the four VALUE ops (slot_in, slot_out, master_wet, pre_gain) that make
# the V2's own controls work on a rack it does not own. They were written straight onto the host before, which
# is why a remote rack's IN/OUT readouts were read-only. Stages 3-5 add to this guard (the ring, open_editor,
# undo over the transport), which is why it is its own file rather than legs bolted onto level_slot_guard.
ej_add_guard(remote_ops_guard         SOURCE tools/remote_ops_guard/harness.cpp              LABEL fast SCRIBBLE)
ej_add_guard(mapfps_sync_guard        SOURCE tools/mapfps_sync_guard/harness.cpp              LABEL fast SCRIBBLE)
ej_add_guard(own_diff                 SOURCE tools/own_diff/harness.cpp                       LABEL fast
             WRAPPER run_own_diff.sh WRAPPER_ARG e16eb2d8c4e59156ad912967b611fc8364452980c6fbf0aa083d5f02c1937fc7)


# 21t-m (29 Sep 2026): builtin_registry_test JOINS THE GATE. It is built by the ROOT CMakeLists as an
# EXCLUDE_FROM_ALL console app (EchoJayBuiltinRegistryTest) and had no ctest label at all, which breaks the
# standing rule that every test in the repo is in the gate or carries a written exclusion. The cost of not being
# in it: its hard-coded device count said 22 while the product registers 23 (EchoJay Level was added to every
# target's source list and to this test's, but not to its count), and the only binary of it on the machine was
# dated 21 August - so the count had been wrong, unnoticed, for five weeks. It builds its own target first.
add_test(NAME builtin_registry_test COMMAND bash ${CMAKE_SOURCE_DIR}/tools/builtin_registry_guard/build_and_run.sh)
set_tests_properties(builtin_registry_test PROPERTIES LABELS "fast" TIMEOUT 1800 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ---- the guards that are not a single in-process binary -----------------------------------------------------
# Kept as their own scripts on purpose: each is a two-process orchestration (a real LinkProcessor against the Link
# archive plus a real EchoJayProcessor against the V2 one), a grep over the source tree, or a python driver. They
# gain the label and the parallel run; their internals are untouched.
add_test(NAME probe_plist_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/probe_plist_guard/build_and_run.sh)
set_tests_properties(probe_plist_guard PROPERTIES LABELS "fast" TIMEOUT 600 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME temp_var_grep_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/temp_var_grep_guard/build_and_run.sh)
set_tests_properties(temp_var_grep_guard PROPERTIES LABELS "fast" TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# 21t-m (29 Sep 2026): the role question never touches a string on the AUDIO thread. A grep guard because the
# defect it pins is a grep-able shape and the gate should not need a two-process rig to catch it again.
add_test(NAME rt_role_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/rt_role_guard/build_and_run.sh)
set_tests_properties(rt_role_guard PROPERTIES LABELS "fast" TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME state_path_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/state_path_guard/build_and_run.sh)
set_tests_properties(state_path_guard PROPERTIES LABELS "fast" TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# 21t-d: one process, but the same shape - a real LinkProcessor against the Link archive, so it lives here.
add_test(NAME calib_link_guard COMMAND bash ${CMAKE_SOURCE_DIR}/tools/calib_link_guard/build_and_run.sh)
set_tests_properties(calib_link_guard PROPERTIES LABELS "fast" TIMEOUT 900 RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# 21t-j (28 Sep 2026): the Apply path in two processes - three real Links and a real V2. Ruled to be the guard for
# test 4 ("a card with three deltas, Apply, trims read back moved, card text counts them"), because a registry
# fixture cannot refuse, ack or move a trim.
# 21t-j (28 Sep 2026): role_snapshot_guard joins them - registration is a full snapshot, proven with a real Link
# handed another's saved state (a copied insert) and a real V2 reading the row.
foreach(TWO alias_mirror_guard lease_id_guard level_match_guard role_snapshot_guard)
    add_test(NAME ${TWO} COMMAND bash ${CMAKE_SOURCE_DIR}/tools/${TWO}/build_and_run.sh)
    set_tests_properties(${TWO} PROPERTIES LABELS "fast" TIMEOUT 900 RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
add_test(NAME linkmeter_harness COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/linkmeter_harness/build_and_run.py)
set_tests_properties(linkmeter_harness PROPERTIES LABELS "fast" TIMEOUT 900 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "EJ_GUARD_HOME_BASE=${CMAKE_BINARY_DIR}/guard-homes")
