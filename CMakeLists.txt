cmake_minimum_required(VERSION 3.22)

project(MeridianMasteringSuite
    VERSION 1.0.0
    DESCRIPTION "Meridian Mastering Suite - production mastering plugin"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()

option(MERIDIAN_ENABLE_AVX2  "Compile with AVX2/FMA instructions (x64 only)" OFF)
option(MERIDIAN_BUILD_TESTS  "Build the unit/integration test executables"   ON)
option(MERIDIAN_COPY_PLUGIN  "Copy built plugins to the system plugin folder" OFF)

# ---------------------------------------------------------------------------
# JUCE 8 via FetchContent
#   To use a local checkout instead of downloading, configure with
#   -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
set(MERIDIAN_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND MERIDIAN_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(MeridianMasteringSuite
    PRODUCT_NAME                 "Meridian Mastering Suite"
    PLUGIN_NAME                  "Meridian Mastering Suite"
    COMPANY_NAME                 "Meridian Audio"
    COMPANY_WEBSITE              "https://github.com/mwbtrue/meridian-mastering-suite"
    BUNDLE_ID                    "com.meridianaudio.masteringsuite"
    PLUGIN_MANUFACTURER_CODE     Mrid
    PLUGIN_CODE                  MeMS
    FORMATS                      ${MERIDIAN_PLUGIN_FORMATS}
    IS_SYNTH                     FALSE
    NEEDS_MIDI_INPUT             FALSE
    NEEDS_MIDI_OUTPUT            FALSE
    IS_MIDI_EFFECT               FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS  TRUE
    COPY_PLUGIN_AFTER_BUILD      ${MERIDIAN_COPY_PLUGIN}
    VST3_CATEGORIES              Fx Mastering Dynamics EQ
    AU_MAIN_TYPE                 kAudioUnitType_Effect
    DESCRIPTION                  "Mastering suite: EQ, dynamics, saturation, imaging, true-peak limiting"
    VERSION                      ${PROJECT_VERSION})

target_compile_definitions(MeridianMasteringSuite
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_SILENCE_XCODE_15_LINKER_WARNING=1
        MERIDIAN_VERSION_STRING="${PROJECT_VERSION}")

target_include_directories(MeridianMasteringSuite
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/common
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/analysis
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/assistant
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ui)

target_link_libraries(MeridianMasteringSuite
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(MeridianMasteringSuite PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/Zc:__cplusplus;/Zc:preprocessor>"
        /MP /fp:precise /bigobj
        /wd4100   # unreferenced formal parameter (JUCE callbacks)
        /wd4127)  # conditional expression is constant
    if(MERIDIAN_ENABLE_AVX2)
        target_compile_options(MeridianMasteringSuite PRIVATE /arch:AVX2)
    endif()
    target_compile_definitions(MeridianMasteringSuite PRIVATE _USE_MATH_DEFINES NOMINMAX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    target_compile_options(MeridianMasteringSuite PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wshadow-field;-Wnon-virtual-dtor;-Wno-unused-parameter>"
        -fno-fast-math)
    if(MERIDIAN_ENABLE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(MeridianMasteringSuite PRIVATE -mavx2 -mfma)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(MeridianMasteringSuite PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wnon-virtual-dtor;-Wno-unused-parameter>"
        -fno-fast-math)
    if(MERIDIAN_ENABLE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(MeridianMasteringSuite PRIVATE -mavx2 -mfma)
    endif()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
add_subdirectory(Source)

# ---------------------------------------------------------------------------
# Testing (CTest + optional pluginval)
# ---------------------------------------------------------------------------
include(CTest)

if(MERIDIAN_BUILD_TESTS AND BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

find_program(PLUGINVAL_EXECUTABLE
    NAMES pluginval pluginval.exe
    HINTS ENV PLUGINVAL_DIR
    DOC "Path to the Tracktion pluginval executable (optional)")

if(BUILD_TESTING AND PLUGINVAL_EXECUTABLE)
    set(MERIDIAN_PLUGINVAL_STRICTNESS 10 CACHE STRING "pluginval strictness level (1-10)")
    add_test(
        NAME    pluginval_vst3
        COMMAND "${PLUGINVAL_EXECUTABLE}"
                --strictness-level ${MERIDIAN_PLUGINVAL_STRICTNESS}
                --validate-in-process
                --skip-gui-tests
                --timeout-ms 600000
                --validate "$<TARGET_PROPERTY:MeridianMasteringSuite_VST3,JUCE_PLUGIN_ARTEFACT_FILE>")
    set_tests_properties(pluginval_vst3 PROPERTIES LABELS "pluginval;integration" TIMEOUT 900)
    message(STATUS "pluginval found: ${PLUGINVAL_EXECUTABLE} (ctest -L pluginval)")
else()
    message(STATUS "pluginval not found - pluginval CTest target disabled (set PLUGINVAL_EXECUTABLE to enable)")
endif()
