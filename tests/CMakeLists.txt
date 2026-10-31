# Standalone verification programs (console apps; JUCE is used only for the
# APVTS the modules read their parameters from, and for juce::dsp::FFT).
set(MERIDIAN_SRC "${CMAKE_SOURCE_DIR}/Source")

function(meridian_add_test name)
    juce_add_console_app(${name} PRODUCT_NAME ${name})
    target_sources(${name} PRIVATE ${ARGN})
    target_include_directories(${name} PRIVATE "${MERIDIAN_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${name} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(${name} PRIVATE juce::juce_audio_processors juce::juce_dsp juce::juce_recommended_config_flags)
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "unit" TIMEOUT 120)
endfunction()

set(LIMITER_SRC "${MERIDIAN_SRC}/dsp/TruePeakLimiter.cpp" "${MERIDIAN_SRC}/common/TruePeakDetector.cpp" "${MERIDIAN_SRC}/common/Biquad.cpp")

meridian_add_test(null_test          null_test.cpp ${LIMITER_SRC})
meridian_add_test(limiter_tp_test    limiter_tp_test.cpp ${LIMITER_SRC})
meridian_add_test(lufs_test          lufs_test.cpp "${MERIDIAN_SRC}/analysis/MeteringEngine.cpp"
                                     "${MERIDIAN_SRC}/common/TruePeakDetector.cpp" "${MERIDIAN_SRC}/common/Biquad.cpp")
meridian_add_test(crossover_sum_test crossover_sum_test.cpp "${MERIDIAN_SRC}/common/LinearPhaseCrossover.cpp"
                                     "${MERIDIAN_SRC}/common/LinearPhaseFIR.cpp")
