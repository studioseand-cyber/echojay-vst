# limiter_preview (session L, 8 Oct 2026): the panel + the v2 engine, offline, no plugin. Reads the renders, feeds the
# engine and the meter tap at file speed, paints EedLimiterPanelV2 offscreen at two sizes and writes PNGs to
# ~/echojay-limiter-renders/ui/. JUCE modules only (gui_basics, graphics, events, core); no audio modules, no BinaryData,
# no JuceHeader.h - the palette mirror in EJLimiterPanelPalette.h covers it.
#   cmake -S tools/limiter_preview -B build-limiter-preview -DCMAKE_BUILD_TYPE=Release && cmake --build build-limiter-preview -j 2
cmake_minimum_required(VERSION 3.22)
project(limiter_preview CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
juce_add_console_app(limiter_preview PRODUCT_NAME "limiter_preview")
target_sources(limiter_preview PRIVATE main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/EedLimiterPanelV2.cpp)
target_include_directories(limiter_preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../Source ${CMAKE_CURRENT_SOURCE_DIR}/../limiter_ab_guard)
target_compile_definitions(limiter_preview PRIVATE EJ_LIMITER_PREVIEW=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(limiter_preview PRIVATE juce::juce_gui_basics juce::juce_graphics juce::juce_events juce::juce_core juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
