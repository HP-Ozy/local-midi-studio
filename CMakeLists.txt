cmake_minimum_required(VERSION 3.22)
project(LocalMIDIStudio VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

# MinGW: link the runtime statically so the exe runs without the toolchain on PATH
if(MINGW)
    add_link_options(-static)
endif()

add_subdirectory(JUCE)

juce_add_gui_app(LocalMIDIStudio PRODUCT_NAME "Local MIDI Studio")
target_sources(LocalMIDIStudio PRIVATE src/Main.cpp)
target_compile_definitions(LocalMIDIStudio PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1) # drop downloaded .mp3 files
target_link_libraries(LocalMIDIStudio PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)

juce_add_console_app(RenderTest)
target_sources(RenderTest PRIVATE tests/RenderTest.cpp)
target_include_directories(RenderTest PRIVATE src)
target_compile_definitions(RenderTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
target_link_libraries(RenderTest PRIVATE juce::juce_audio_devices juce::juce_audio_formats juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)

if(WIN32)
    target_link_libraries(RenderTest PRIVATE psapi) # process memory for the stress test
endif()

enable_testing()
add_test(NAME render COMMAND RenderTest)
