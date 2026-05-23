cmake_minimum_required(VERSION 3.22)
project(DRUM_SAT_76 VERSION 1.0.1)

add_subdirectory(JUCE)

juce_add_plugin(DRUM_SAT_76
    COMPANY_NAME "Alphastudio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE "Manu"
    PLUGIN_CODE "Abi1"
    FORMATS VST3 Standalone
    PRODUCT_NAME "Alphastudio DRUM SAT 76"
)

juce_generate_juce_header(DRUM_SAT_76)

juce_add_binary_data(DRUM_SAT_76_Data
    HEADER_NAME "BinaryData.h"
    NAMESPACE "BinaryData"
    SOURCES
    "Resources/background.png"
)

target_sources(DRUM_SAT_76
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(DRUM_SAT_76
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(DRUM_SAT_76
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        DRUM_SAT_76_Data
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
