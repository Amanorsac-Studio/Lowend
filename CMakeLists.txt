# Amanorsac Low End — bass amp, cab, pedalboard, looper, and the Learn tab.
# Configure:  cmake -S apps/lowend -B apps/lowend/build
# Build:      cmake --build apps/lowend/build --config Release --parallel
# Test:       ctest --test-dir apps/lowend/build -C Release
#
# The UI is resources/ui/index.html hosted in JUCE's WebBrowserComponent
# (WebView2 on Windows, WKWebView on macOS), wired to the engine through
# src/plugin/LowEndEditor.cpp. On Windows the WebView2 SDK NuGet package is
# needed at configure time:
#     nuget install Microsoft.Web.WebView2 -Version 1.0.2903.40 -OutputDirectory <dir>
# and the block below stages it out of the NuGet cache automatically.
#
# Reuses (read-only) the JUCE checkout that lives next to JM Signature. Nothing
# here writes into apps/signature or apps/sanctuary.
cmake_minimum_required(VERSION 3.24)
project(LowEnd VERSION 1.0.0 LANGUAGES C CXX)

# The C++ runtime is linked STATICALLY into our own binaries. A plug-in is
# loaded into somebody else's process on a machine that may never have had the
# Visual C++ redistributable installed; depending on it being there is how a
# plug-in fails to load on exactly the clean machine a buyer owns. (ONNX
# Runtime is a prebuilt DLL and still needs the runtime: the installer places
# those four DLLs beside it - see installer/LowEnd.iss.) Must be set before JUCE.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# ------------------------------------------------------------------ JUCE 8
set(LOWEND_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../signature/external/JUCE" CACHE PATH "JUCE checkout")
if(WIN32)
    file(GLOB _wv2_headers "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/*/build/native/include/WebView2.h")
    if(_wv2_headers)
        list(SORT _wv2_headers)
        list(GET _wv2_headers -1 _wv2_header)
        get_filename_component(_wv2_version_dir "${_wv2_header}/../../../.." ABSOLUTE)
        set(_wv2_staged "${CMAKE_BINARY_DIR}/webview2/Microsoft.Web.WebView2")
        if(NOT EXISTS "${_wv2_staged}/build/native/include/WebView2.h")
            file(COPY "${_wv2_version_dir}/build" DESTINATION "${_wv2_staged}")
        endif()
        set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${CMAKE_BINARY_DIR}/webview2" CACHE PATH "Folder containing Microsoft.Web.WebView2" FORCE)
        message(STATUS "Low End: WebView2 SDK staged from ${_wv2_version_dir}")
    endif()
endif()

if(EXISTS "${LOWEND_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${LOWEND_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.15
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# TEST BUILD. Adds a key check that silences the audio until a valid, unexpired
# test key is entered (src/plugin/TesterGate.h). Off for a release: a release
# contains none of it. It is not the studio's licensing (see TesterGate.h) and
# carries a second signing key, so a build made with this ON must never be sold.
option(LOWEND_TESTER_BUILD "Build a key-gated test build" OFF)
if(LOWEND_TESTER_BUILD)
    message(STATUS "Low End: TEST BUILD - key required, not for release")
endif()

set(LOWEND_AUDIO_DEFS JUCE_WASAPI=1 JUCE_DIRECTSOUND=1)
set(LOWEND_JUCE_ROOT "${JUCE_SOURCE_DIR}")
if(NOT LOWEND_JUCE_ROOT)
    set(LOWEND_JUCE_ROOT "${juce_SOURCE_DIR}")
endif()
if(WIN32 AND EXISTS "${LOWEND_JUCE_ROOT}/modules/juce_audio_devices/native/asio/iasiodrv.h")
    list(APPEND LOWEND_AUDIO_DEFS JUCE_ASIO=1)
    message(STATUS "Low End: ASIO enabled (JUCE-bundled headers)")
endif()

set(LOWEND_WEB_DEFS JUCE_WEB_BROWSER=1 JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)

# ------------------------------------------------------------------ Stem separation
# The separation network (HT-Demucs) runs on ONNX Runtime — Microsoft's official
# prebuilt CPU package, MIT licensed, fetched at configure time. Only the header
# and the import library are used here; onnxruntime.dll is copied next to the
# binaries below and ships with the app.
#
# This replaced demucs.cpp (a C++/Eigen port of the same model), which was
# correct but ran ~30x slower than real time under MSVC — see Separator.cpp.
# Consequences of the switch: no Eigen, no AVX2-only translation unit, and no
# CPU feature check, because ONNX Runtime dispatches on the instruction sets it
# finds at run time. The weights are not part of the build either: the
# INSTALLER places them (installer/), and the product only ever reads them.
set(LOWEND_ORT_VERSION 1.22.0)
if(WIN32)
    set(LOWEND_ORT_PKG "onnxruntime-win-x64-${LOWEND_ORT_VERSION}.zip")
elseif(APPLE)
    set(LOWEND_ORT_PKG "onnxruntime-osx-universal2-${LOWEND_ORT_VERSION}.tgz")
else()
    set(LOWEND_ORT_PKG "onnxruntime-linux-x64-${LOWEND_ORT_VERSION}.tgz")
endif()
include(FetchContent)
FetchContent_Declare(onnxruntime
    URL "https://github.com/microsoft/onnxruntime/releases/download/v${LOWEND_ORT_VERSION}/${LOWEND_ORT_PKG}")
FetchContent_MakeAvailable(onnxruntime)

add_library(lowend_ort INTERFACE)
target_include_directories(lowend_ort INTERFACE "${onnxruntime_SOURCE_DIR}/include")
if(WIN32)
    target_link_libraries(lowend_ort INTERFACE "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib")
    set(LOWEND_ORT_RUNTIME "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
elseif(APPLE)
    target_link_libraries(lowend_ort INTERFACE "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.dylib")
    set(LOWEND_ORT_RUNTIME "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.${LOWEND_ORT_VERSION}.dylib")
else()
    target_link_libraries(lowend_ort INTERFACE "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so")
    set(LOWEND_ORT_RUNTIME "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so.${LOWEND_ORT_VERSION}")
endif()

# Every target that separates needs the runtime library with it, and needs to
# find THAT copy rather than any other:
#
#  * Windows: the import is DELAY-LOADED and Separator.cpp loads our DLL by full
#    path first. A plug-in's folder is not on the DLL search path (the host's
#    is), and System32 carries an older onnxruntime.dll of its own that a search
#    by name would find instead.
#  * macOS: the dylib goes into the bundle's Frameworks folder and is found
#    through @rpath. It is signed IN THIS SAME build phase, immediately after
#    being copied in - see the comment on the codesign line below for why.
function(lowend_stage_onnxruntime target)
    if(NOT TARGET ${target})
        return()
    endif()
    if(WIN32)
        target_link_options(${target} PRIVATE "/DELAYLOAD:onnxruntime.dll")
        target_link_libraries(${target} PRIVATE delayimp)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LOWEND_ORT_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    elseif(APPLE)
        get_target_property(_is_bundle ${target} BUNDLE)
        get_target_property(_is_macos_bundle ${target} MACOSX_BUNDLE)
        if(_is_bundle OR _is_macos_bundle)
            set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path/../Frameworks"
                                                        INSTALL_RPATH "@loader_path/../Frameworks")
            get_filename_component(_ort_filename "${LOWEND_ORT_RUNTIME}" NAME)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LOWEND_ORT_RUNTIME}" "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks/"
                # Apple rejects an app whose embedded code isn't signed BEFORE the
                # app itself is - "code object is not signed at all, in
                # subcomponent: .../libonnxruntime.dylib" is exactly what Xcode's
                # own automatic CodeSign phase says if this dylib is left as a
                # bare copy. That phase runs as the LAST step for a signed
                # target, after this POST_BUILD command, so signing the dylib
                # right here - in the same phase that copies it in - satisfies
                # Apple's ordering rule. Xcode exports CODE_SIGN_IDENTITY to
                # every Run Script phase, so this only fires on a signed (CI or
                # release) configure; a local unsigned dev build leaves the
                # dylib as a plain copy, exactly as before.
                COMMAND sh -c "if [ -n \"$CODE_SIGN_IDENTITY\" ] && [ \"$CODE_SIGNING_ALLOWED\" != \"NO\" ]; then codesign --force --options runtime --timestamp --sign \"$CODE_SIGN_IDENTITY\" \"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks/${_ort_filename}\"; fi"
                VERBATIM)
        else()
            set_target_properties(${target} PROPERTIES BUILD_RPATH "${onnxruntime_SOURCE_DIR}/lib")
        endif()
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LOWEND_ORT_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

# ------------------------------------------------------------------ UI resource
juce_add_binary_data(LowEndUi HEADER_NAME LowEndUi.h NAMESPACE LowEndUi SOURCES
    resources/ui/index.html
    resources/ui/analyzer.js
    resources/ui/stemplayer.js
    # Fonts are shipped inside the product and never fetched (Master Standard §5).
    resources/ui/fonts/Inter.woff2
    resources/ui/fonts/JetBrainsMono.woff2
    resources/ui/fonts/BarlowSemiCondensed-600.woff2
    resources/ui/fonts/BarlowSemiCondensed-700.woff2
    resources/ui/about-lockup.png
    THIRD_PARTY_NOTICES.txt)

# ------------------------------------------------------------------ Plugin
juce_add_plugin(LowEnd
    COMPANY_NAME              "Amanorsac Studio"
    PLUGIN_MANUFACTURER_CODE  Amsc
    PLUGIN_CODE               LwEd
    PRODUCT_NAME              "Low End"
    BUNDLE_ID                 com.amanorsac.lowend
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          TRUE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    NEEDS_WEB_BROWSER         TRUE
    NEEDS_WEBVIEW2            TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    ICON_BIG                  "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-1024.png"
    ICON_SMALL                "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-256.png"
    # macOS: the hardened runtime is a notarisation requirement, and an amp that
    # cannot open the audio input is not an amp.
    HARDENED_RUNTIME_ENABLED  TRUE
    HARDENED_RUNTIME_OPTIONS  "com.apple.security.device.audio-input"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Low End needs the audio input to hear your bass."
    FORMATS                   VST3 AU Standalone
    VST3_CATEGORIES           Fx Distortion)

target_sources(LowEnd PRIVATE
    src/plugin/LowEndProcessor.cpp
    src/plugin/LowEndEditor.cpp
    src/learn/StemSeparator.cpp
    src/learn/Separator.cpp)

target_include_directories(LowEnd PRIVATE src)

target_compile_definitions(LowEnd PUBLIC
    LOWEND_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${LOWEND_TESTER_BUILD}>:LOWEND_TESTER_BUILD=1>
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    ${LOWEND_WEB_DEFS}
    ${LOWEND_AUDIO_DEFS})

if(TARGET LowEnd_Standalone)
    target_sources(LowEnd_Standalone PRIVATE src/plugin/StandaloneApp.cpp)
    target_include_directories(LowEnd_Standalone PRIVATE src
        "${CMAKE_CURRENT_BINARY_DIR}/juce_binarydata_LowEndUi/JuceLibraryCode")
endif()

target_link_libraries(LowEnd PRIVATE
    LowEndUi
    lowend_ort
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ------------------------------------------------------------------ Tests
juce_add_console_app(LowEndTests PRODUCT_NAME "LowEndTests")
target_sources(LowEndTests PRIVATE tests/LowEndTests.cpp)
target_include_directories(LowEndTests PRIVATE src)
target_compile_definitions(LowEndTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(LowEndTests PRIVATE
    juce::juce_audio_formats
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Processor-level soak test: audio runs while presets change, blocks are
# reordered and the looper is driven underneath it. Compiles the processor only
# (the editor is web-hosted and not needed here).
juce_add_console_app(LowEndSoak PRODUCT_NAME "LowEndSoak")
target_sources(LowEndSoak PRIVATE
    tests/SoakTest.cpp
    src/plugin/LowEndProcessor.cpp
    src/learn/StemSeparator.cpp
    src/learn/Separator.cpp)
target_include_directories(LowEndSoak PRIVATE src)
target_compile_definitions(LowEndSoak PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    LOWEND_HEADLESS=1)
target_link_libraries(LowEndSoak PRIVATE
    lowend_ort
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The test-build gate, checked in the real processor. Always compiled with the gate
# ON (LOWEND_TESTER_BUILD=1) whatever the rest of the build is doing, because what it
# proves - that a locked build is silent - is only true of a build that has one.
juce_add_console_app(LowEndGateTest PRODUCT_NAME "LowEndGateTest")
target_sources(LowEndGateTest PRIVATE
    tests/GateTest.cpp
    src/plugin/LowEndProcessor.cpp
    src/learn/StemSeparator.cpp
    src/learn/Separator.cpp)
target_include_directories(LowEndGateTest PRIVATE src)
target_compile_definitions(LowEndGateTest PRIVATE
    LOWEND_TESTER_BUILD=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    LOWEND_HEADLESS=1)
target_link_libraries(LowEndGateTest PRIVATE
    lowend_ort
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Stem separation on a real file, timed:
#   LowEndStems <song.wav> <out-dir> [best]
# Uses the weights in Documents/Amanorsac Studio/Low End/Models.
juce_add_console_app(LowEndStems PRODUCT_NAME "LowEndStems")
target_sources(LowEndStems PRIVATE tests/StemTest.cpp src/learn/StemSeparator.cpp src/learn/Separator.cpp)
target_include_directories(LowEndStems PRIVATE src)
target_compile_definitions(LowEndStems PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(LowEndStems PRIVATE
    lowend_ort
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags)

# onnxruntime.dll beside each binary that separates.
lowend_stage_onnxruntime(LowEnd_Standalone)
lowend_stage_onnxruntime(LowEnd_VST3)
lowend_stage_onnxruntime(LowEnd_AU)
lowend_stage_onnxruntime(LowEndStems)
lowend_stage_onnxruntime(LowEndSoak)
lowend_stage_onnxruntime(LowEndGateTest)

enable_testing()
add_test(NAME LowEndUnit COMMAND LowEndTests)
add_test(NAME LowEndTesterGate COMMAND LowEndGateTest)
set_tests_properties(LowEndTesterGate PROPERTIES TIMEOUT 120)
add_test(NAME LowEndSoakShort COMMAND LowEndSoak --seconds 30)
set_tests_properties(LowEndSoakShort PROPERTIES TIMEOUT 600)

# The Learn tab's song analyser is JavaScript (it runs in a Web Worker in the
# UI); its tests run under Node when Node is installed.
find_program(LOWEND_NODE node)
if(LOWEND_NODE)
    add_test(NAME LowEndLearnAnalyzer COMMAND ${LOWEND_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/learn/analyzer.test.js)
    set_tests_properties(LowEndLearnAnalyzer PROPERTIES TIMEOUT 120)
    # The stem player is an AudioWorklet; the test stubs the three globals a
    # worklet is given and drives the real process() in 128-sample blocks.
    add_test(NAME LowEndStemPlayer COMMAND ${LOWEND_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/learn/stemplayer.test.js)
    set_tests_properties(LowEndStemPlayer PROPERTIES TIMEOUT 60)
endif()
