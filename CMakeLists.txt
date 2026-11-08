cmake_minimum_required(VERSION 3.25)

# Version lives in one place so plugin metadata, state schema notes and installers agree.
project(STRAT3 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Universal binary on macOS: users run both arm64 hosts and Rosetta hosts.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")
endif()

option(STRAT3_BUILD_PLUGIN "Build plugin formats (VST3/AU/CLAP/Standalone)" ON)
option(STRAT3_BUILD_TESTS  "Build Catch2 DSP unit tests"                    ON)
option(STRAT3_WITH_WEBUI   "Embed the React WebView panels (UI option C)"   ON)

# ---------------------------------------------------------------- dependencies
include(cmake/CPM.cmake)

# Pinned exact tags: a floating branch would change DSP/UI behaviour under us between CI runs.
CPMAddPackage(NAME JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.17-security
    GIT_SHALLOW ON
    SYSTEM YES)   # treat JUCE headers as system so our strict warnings only report our code

if(STRAT3_BUILD_PLUGIN)
    # Pinned to a main commit, not a tag: the last release (0.26.0) includes
    # juce_LegacyAudioParameter.cpp, which JUCE 8.0.11+ moved to juce_audio_processors_headless.
    # Move back to a tag once one ships with that fix.
    CPMAddPackage(NAME clap-juce-extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG 7adee3a1bd4684d4caa5601100e364abccff4b4d
        GIT_SHALLOW OFF)   # a shallow fetch needs a branch or tag name
endif()

# ---------------------------------------------------------------- warnings
# Applied per source file, not per target: JUCE module .cpp files are compiled inside our
# targets, and target-level -Wdouble-promotion etc. would bury our warnings under JUCE's.
if(MSVC)
    set(STRAT3_WARNING_FLAGS /W4 /permissive-)
else()
    # -Wdouble-promotion: silent float->double promotion in DSP inner loops costs CPU.
    set(STRAT3_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wdouble-promotion)
endif()

# ---------------------------------------------------------------- DSP core
# INTERFACE library: no plugin-wrapper dependency, testable on its own.
# Each final binary (tests, plugin) compiles JUCE modules exactly once itself; a STATIC DSP
# lib that also carried JUCE objects would duplicate juce_core in the plugin with possibly
# different JUCE_* config macros (ODR violation that links fine and fails at runtime).
file(GLOB_RECURSE STRAT3_DSP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/dsp/*.cpp)
add_library(strat3_dsp INTERFACE)
target_sources(strat3_dsp INTERFACE ${STRAT3_DSP_SOURCES})
target_include_directories(strat3_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(strat3_dsp INTERFACE juce::juce_dsp juce::juce_audio_basics)
set_source_files_properties(${STRAT3_DSP_SOURCES} PROPERTIES COMPILE_OPTIONS "${STRAT3_WARNING_FLAGS}")

# ---------------------------------------------------------------- parameters + state
# Separate from the plugin so tests cover the exact layout the host will see.
file(GLOB_RECURSE STRAT3_PARAM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/params/*.cpp)
add_library(strat3_params INTERFACE)
target_sources(strat3_params INTERFACE ${STRAT3_PARAM_SOURCES})
target_include_directories(strat3_params INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(strat3_params INTERFACE juce::juce_audio_processors juce::juce_data_structures)
set_source_files_properties(${STRAT3_PARAM_SOURCES} PROPERTIES COMPILE_OPTIONS "${STRAT3_WARNING_FLAGS}")

# ---------------------------------------------------------------- processor
# The AudioProcessor itself (APVTS -> engine, state, analysis thread) and its native editor.
# Outside the plugin target so the tests drive the exact classes the host will load.
file(GLOB_RECURSE STRAT3_PROCESSOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/processor/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/ui/*.cpp)
add_library(strat3_processor INTERFACE)
target_sources(strat3_processor INTERFACE ${STRAT3_PROCESSOR_SOURCES})
target_link_libraries(strat3_processor INTERFACE strat3_dsp strat3_params juce::juce_audio_processors)
set_source_files_properties(${STRAT3_PROCESSOR_SOURCES} PROPERTIES COMPILE_OPTIONS "${STRAT3_WARNING_FLAGS}")

# ---------------------------------------------------------------- plugin
if(STRAT3_BUILD_PLUGIN)
    juce_add_plugin(STRAT3
        COMPANY_NAME             "XYP3RN"
        BUNDLE_ID                "com.xyp3rn.strat3"
        PLUGIN_MANUFACTURER_CODE Xyp3
        PLUGIN_CODE              Str3    # never change after release: hosts key saved sessions on it
        FORMATS                  VST3 AU Standalone
        PRODUCT_NAME             "STRAT3"
        IS_SYNTH                 FALSE
        NEEDS_MIDI_INPUT         FALSE
        NEEDS_MIDI_OUTPUT        FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE   # keyboard layer navigation (Phase 4 a11y)
        COPY_PLUGIN_AFTER_BUILD  FALSE      # CI and pluginval read from the build dir
        NEEDS_WEB_BROWSER        ${STRAT3_WITH_WEBUI}
        NEEDS_WEBVIEW2           ${STRAT3_WITH_WEBUI}
        VST3_CATEGORIES          Fx Mastering Dynamics EQ
        AU_MAIN_TYPE             kAudioUnitType_Effect)

    file(GLOB_RECURSE STRAT3_PLUGIN_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/source/assistant/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/Plugin*.cpp)   # source/ui comes with strat3_processor
    target_sources(STRAT3 PRIVATE ${STRAT3_PLUGIN_SOURCES})
    set_source_files_properties(${STRAT3_PLUGIN_SOURCES} PROPERTIES COMPILE_OPTIONS "${STRAT3_WARNING_FLAGS}")

    target_compile_definitions(STRAT3 PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0     # no VST2 SDK; also avoids ID collisions with nothing
        JUCE_DISPLAY_SPLASH_SCREEN=0     # requires a JUCE commercial/indie licence: confirm before release
        JUCE_REPORT_APP_USAGE=0
        JUCE_WEB_BROWSER=$<BOOL:${STRAT3_WITH_WEBUI}>
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=$<BOOL:${STRAT3_WITH_WEBUI}>
        STRAT3_WITH_WEBUI=$<BOOL:${STRAT3_WITH_WEBUI}>)

    target_link_libraries(STRAT3
        PRIVATE strat3_dsp
                strat3_params
                strat3_processor
                juce::juce_audio_utils
                juce::juce_gui_extra
        PUBLIC  juce::juce_recommended_config_flags
                juce::juce_recommended_lto_flags)

    # React panels (webui/) are built by Vite before CMake runs; embedding them means the
    # plugin never loads UI from the network, and the UI version always matches the binary.
    if(STRAT3_WITH_WEBUI)
        set(STRAT3_WEBUI_DIST ${CMAKE_CURRENT_SOURCE_DIR}/webui/dist)
        file(GLOB_RECURSE STRAT3_WEBUI_FILES CONFIGURE_DEPENDS ${STRAT3_WEBUI_DIST}/*)
        if(NOT STRAT3_WEBUI_FILES)
            message(FATAL_ERROR
                "webui/dist is empty. Run `npm ci && npm run build` in webui/, "
                "or configure with -DSTRAT3_WITH_WEBUI=OFF for a native-only build.")
        endif()
        juce_add_binary_data(strat3_webui SOURCES ${STRAT3_WEBUI_FILES})
        target_link_libraries(STRAT3 PRIVATE strat3_webui)
    endif()

    # CLAP via clap-juce-extensions: separate target wrapping the same shared code.
    clap_juce_extensions_plugin(TARGET STRAT3
        CLAP_ID "com.xyp3rn.strat3"
        CLAP_FEATURES audio-effect mastering equalizer compressor limiter stereo)
endif()

# ---------------------------------------------------------------- tests
if(STRAT3_BUILD_TESTS)
    CPMAddPackage(NAME Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.1
        GIT_SHALLOW ON)
    enable_testing()

    file(GLOB_RECURSE STRAT3_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_executable(strat3_tests ${STRAT3_TEST_SOURCES})
    set_source_files_properties(${STRAT3_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${STRAT3_WARNING_FLAGS}")
    target_include_directories(strat3_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(strat3_tests PRIVATE
        strat3_dsp
        strat3_params
        strat3_processor
        Catch2::Catch2WithMain
        juce::juce_recommended_config_flags)
    target_compile_definitions(strat3_tests PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STANDALONE_APPLICATION=1
        STRAT3_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        STRAT3_CANDIDATE_DIR="${CMAKE_CURRENT_BINARY_DIR}")

    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(strat3_tests)
endif()
