cmake_minimum_required(VERSION 3.22)
project(EchoJay VERSION 1.0.9)

# Path to your JUCE installation — update this to match your setup
# Download JUCE from https://juce.com/download/
set(JUCE_PATH "${CMAKE_SOURCE_DIR}/../JUCE" CACHE PATH "Path to JUCE framework")

# Path to AAX SDK — download from developer.avid.com
set(AAX_SDK_PATH "$ENV{HOME}/AAX_SDK" CACHE PATH "Path to AAX SDK")

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
    set(PLUGIN_FORMATS AU VST3 AAX Standalone)
else()
    set(PLUGIN_FORMATS AU VST3 Standalone)
endif()

juce_add_plugin(EchoJay
    COMPANY_NAME "EchoJay"
    BUNDLE_ID "com.echojay.plugin"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Ecjy
    PLUGIN_CODE EcJy
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "EchoJay"
    AAX_IDENTIFIER "com.echojay.plugin"
)

juce_generate_juce_header(EchoJay)

target_sources(EchoJay PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/MeterEngine.cpp
    Source/MeterEngine.h
    Source/PluginScanner.cpp
    Source/PluginScanner.h
    Source/EchoJayAPI.cpp
    Source/EchoJayAPI.h
    Source/ReferenceAnalyser.cpp
    Source/ReferenceAnalyser.h
    Source/WaveformRecorder.cpp
    Source/WaveformRecorder.h
    Source/EchoJayLookAndFeel.h
)

target_compile_definitions(EchoJay PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(EchoJay
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
