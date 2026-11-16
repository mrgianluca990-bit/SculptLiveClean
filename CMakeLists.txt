cmake_minimum_required(VERSION 3.22)

if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SculptLiveClean VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SCULPT_JUCE_SOURCE_DIR "" CACHE PATH "Optional local JUCE 8.0.4 directory")
if(SCULPT_JUCE_SOURCE_DIR)
  add_subdirectory("${SCULPT_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(SculptLiveClean
  COMPANY_NAME "Sculpt"
  BUNDLE_ID "com.sculpt.liveclean"
  PRODUCT_NAME "Sculpt Live Clean"
  PLUGIN_MANUFACTURER_CODE Scpt
  PLUGIN_CODE Slcl
  FORMATS VST3
  VST3_CATEGORIES Fx Restoration
  IS_SYNTH FALSE
  IS_MIDI_EFFECT FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SculptLiveClean PRIVATE
  Source/LiveCleanEngine.h
  Source/SourceExtractor.h
  Source/FeedbackSuppressor.h
  Source/PluginProcessor.h
  Source/PluginProcessor.cpp
  Source/PluginEditor.h
  Source/PluginEditor.cpp)

target_compile_definitions(SculptLiveClean PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SculptLiveClean
  PRIVATE juce::juce_audio_processors
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

option(SCULPT_BUILD_STATE_TESTS "Build the JUCE state and host-bypass checks" OFF)
if(SCULPT_BUILD_STATE_TESTS)
  enable_testing()
  juce_add_console_app(SculptLiveCleanStateTests PRODUCT_NAME "SculptLiveCleanStateTests")
  target_sources(SculptLiveCleanStateTests PRIVATE Tests/plugin_state_test.cpp)
  target_include_directories(SculptLiveCleanStateTests PRIVATE Source)
  target_link_libraries(SculptLiveCleanStateTests PRIVATE
    SculptLiveClean juce::juce_audio_processors)
  add_test(NAME plugin-state COMMAND $<TARGET_FILE:SculptLiveCleanStateTests>)
endif()
