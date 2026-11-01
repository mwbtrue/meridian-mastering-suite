# ---------------------------------------------------------------------------
# Meridian Mastering Suite - source manifest
#
# This is the complete manifest of the repository's planned source tree.
# The repository is generated in batches; each entry is added to the target
# only once the file exists on disk (see meridian_add_sources below), so the
# project configures cleanly at every stage of generation. Once all batches
# are committed, every file listed here is present and compiled.
# ---------------------------------------------------------------------------

set(MERIDIAN_SOURCE_FILES
    # --- Core ---------------------------------------------------------------
    ParameterIDs.h
    PluginProcessor.h
    PluginProcessor.cpp
    PluginEditor.h
    PluginEditor.cpp

    # --- Common utilities -----------------------------------------------------
    common/LockFreeQueue.h
    common/SmoothParam.h
    common/MidSideMatrix.h
    common/DelayLine.h
    common/DotProduct.h
    common/RealFFT.h
    common/FastMath.h
    common/AnalysisTypes.h
    common/LinearPhaseFIR.h
    common/LinearPhaseFIR.cpp
    common/OversamplingEngine.h
    common/OversamplingEngine.cpp
    common/Biquad.h
    common/Biquad.cpp
    common/LinearPhaseCrossover.h
    common/LinearPhaseCrossover.cpp
    common/LinkwitzRileyBank.h
    common/LinkwitzRileyBank.cpp
    common/TruePeakDetector.h
    common/TruePeakDetector.cpp

    # --- Signal chain modules -------------------------------------------------
    dsp/ModuleBase.h
    dsp/DSPModuleBase.h
    dsp/InputModule.h
    dsp/InputModule.cpp
    dsp/CorrectiveEQ.h
    dsp/CorrectiveEQ.cpp
    dsp/LowEndControl.h
    dsp/LowEndControl.cpp
    dsp/DynamicEQ.h
    dsp/DynamicEQ.cpp
    dsp/GlueCompressor.h
    dsp/GlueCompressor.cpp
    dsp/MultibandDynamics.h
    dsp/MultibandDynamics.cpp
    dsp/ToneEQ.h
    dsp/ToneEQ.cpp
    dsp/Saturation.h
    dsp/Saturation.cpp
    dsp/StereoImager.h
    dsp/StereoImager.cpp
    dsp/Clipper.h
    dsp/Clipper.cpp
    dsp/TruePeakLimiter.h
    dsp/TruePeakLimiter.cpp
    dsp/Dither.h
    dsp/Dither.cpp

    # --- Analysis -------------------------------------------------------------
    analysis/MeteringEngine.h
    analysis/MeteringEngine.cpp
    analysis/SpectrumAnalyzer.h
    analysis/SpectrumAnalyzer.cpp
    analysis/CodecPreview.h
    analysis/CodecPreview.cpp
    analysis/ReferencePanel.h
    analysis/ReferencePanel.cpp

    # --- Intelligent assistant ------------------------------------------------
    assistant/IntelligentAssistant.h
    assistant/IntelligentAssistant.cpp
    assistant/TargetProfile.h
    assistant/TargetProfile.cpp

    # --- User interface -------------------------------------------------------
    ui/UITheme.h
    ui/UITheme.cpp
    ui/ModuleRack.h
    ui/ModuleRack.cpp
    ui/SpectrumDisplay.h
    ui/SpectrumDisplay.cpp
    ui/MeterColumn.h
    ui/MeterColumn.cpp
    ui/PresetBrowser.h
    ui/PresetBrowser.cpp
    ui/AssistantPanel.h
    ui/AssistantPanel.cpp)

set(MERIDIAN_PRESENT_SOURCES "")
set(MERIDIAN_PENDING_SOURCES "")

foreach(src IN LISTS MERIDIAN_SOURCE_FILES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
        list(APPEND MERIDIAN_PRESENT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
    else()
        list(APPEND MERIDIAN_PENDING_SOURCES "${src}")
    endif()
endforeach()

target_sources(MeridianMasteringSuite PRIVATE ${MERIDIAN_PRESENT_SOURCES})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source" FILES ${MERIDIAN_PRESENT_SOURCES})

list(LENGTH MERIDIAN_PRESENT_SOURCES MERIDIAN_PRESENT_COUNT)
list(LENGTH MERIDIAN_PENDING_SOURCES MERIDIAN_PENDING_COUNT)
message(STATUS "Meridian: ${MERIDIAN_PRESENT_COUNT} source files present, ${MERIDIAN_PENDING_COUNT} not yet generated")
if(MERIDIAN_PENDING_COUNT GREATER 0)
    string(REPLACE ";" ", " MERIDIAN_PENDING_PRETTY "${MERIDIAN_PENDING_SOURCES}")
    message(STATUS "Meridian: pending files: ${MERIDIAN_PENDING_PRETTY}")
endif()
