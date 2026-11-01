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
meridian_add_test(fft_test           fft_test.cpp)
meridian_add_test(fastmath_test      fastmath_test.cpp)
meridian_add_test(oversampling_test  oversampling_test.cpp "${MERIDIAN_SRC}/common/OversamplingEngine.cpp")
meridian_add_test(crossover_sum_test crossover_sum_test.cpp "${MERIDIAN_SRC}/common/LinearPhaseCrossover.cpp"
                                     "${MERIDIAN_SRC}/common/LinearPhaseFIR.cpp")
# Tests that instantiate the complete processor link the plugin's SharedCode
# library (which already contains the compiled JUCE modules).
function(meridian_add_plugin_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        $<TARGET_PROPERTY:MeridianMasteringSuite,INCLUDE_DIRECTORIES>)
    target_compile_definitions(${name} PRIVATE
        $<TARGET_PROPERTY:MeridianMasteringSuite,COMPILE_DEFINITIONS>)
    target_link_libraries(${name} PRIVATE MeridianMasteringSuite)
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endfunction()

meridian_add_plugin_test(chain_null_test chain_null_test.cpp)
add_test(NAME chain_null_test COMMAND chain_null_test)
set_tests_properties(chain_null_test PROPERTIES LABELS "integration" TIMEOUT 600)

meridian_add_plugin_test(lm_undo_test lm_undo_test.cpp)
add_test(NAME lm_undo_test COMMAND lm_undo_test)
set_tests_properties(lm_undo_test PROPERTIES LABELS "integration" TIMEOUT 300)

meridian_add_plugin_test(cpu_bench cpu_bench.cpp)
add_test(NAME cpu_bench COMMAND cpu_bench --fail-over=30)
set_tests_properties(cpu_bench PROPERTIES LABELS "perf" TIMEOUT 600)

meridian_add_test(ebu_loudness_test  ebu_loudness_test.cpp "${MERIDIAN_SRC}/analysis/MeteringEngine.cpp"
                                     "${MERIDIAN_SRC}/common/TruePeakDetector.cpp" "${MERIDIAN_SRC}/common/Biquad.cpp")
