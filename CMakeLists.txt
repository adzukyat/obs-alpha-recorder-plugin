cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ALPHA_RECORDER_PROJECT_VERSION)
string(STRIP "${ALPHA_RECORDER_PROJECT_VERSION}" ALPHA_RECORDER_PROJECT_VERSION)
if(NOT ALPHA_RECORDER_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-.+][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as X.Y.Z; got '${ALPHA_RECORDER_PROJECT_VERSION}'")
endif()

project(alpha_recorder_plugin
    VERSION "${ALPHA_RECORDER_PROJECT_VERSION}"
    DESCRIPTION "OBS native plugin scaffold for pair-based RGBA recording"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/cmake/finders")

file(GLOB _alpha_recorder_obs_deps_roots LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/.deps/obs-deps-*")
list(SORT _alpha_recorder_obs_deps_roots)
if(_alpha_recorder_obs_deps_roots)
    list(POP_FRONT _alpha_recorder_obs_deps_roots _alpha_recorder_obs_deps_root)
    list(APPEND CMAKE_PREFIX_PATH "${_alpha_recorder_obs_deps_root}")
endif()

include(ProjectOptions)
include(Warnings)
include(FetchDeps)
include(GNUInstallDirs)

alpha_recorder_set_project_options()
alpha_recorder_fetch_test_deps()

include(CTest)

set(ALPHA_RECORDER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/alpha_recorder/version.hpp.in"
    "${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}/alpha_recorder/version.hpp"
    @ONLY
)

include("${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-root.cmake" OPTIONAL)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/obs")
if(APPLE)
    set(_alpha_recorder_generated_obs_data_path "Resources")
    set(_alpha_recorder_generated_obs_plugin_path "PlugIns")
    set(_alpha_recorder_generated_obs_plugin_destination "PlugIns")
elseif(WIN32)
    set(_alpha_recorder_generated_obs_data_path "data")
    set(_alpha_recorder_generated_obs_plugin_path "obs-plugins/64bit")
    set(_alpha_recorder_generated_obs_plugin_destination "obs-plugins/64bit")
else()
    set(_alpha_recorder_generated_obs_data_path "share/obs/obs-studio")
    set(_alpha_recorder_generated_obs_plugin_path "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    set(_alpha_recorder_generated_obs_plugin_destination "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obs/obsconfig.h"
"#pragma once

#define OBS_DATA_PATH \"${_alpha_recorder_generated_obs_data_path}\"
#define OBS_PLUGIN_PATH \"${_alpha_recorder_generated_obs_plugin_path}\"
#define OBS_PLUGIN_DESTINATION \"${_alpha_recorder_generated_obs_plugin_destination}\"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
")

add_library(alpha_recorder_e2e_scenario STATIC
    src/core/e2e_scenario.cpp
)
alpha_recorder_setup_target(alpha_recorder_e2e_scenario)
alpha_recorder_apply_warnings(alpha_recorder_e2e_scenario)
target_include_directories(alpha_recorder_e2e_scenario
    PUBLIC
        $<BUILD_INTERFACE:${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(alpha_recorder_e2e_scenario PROPERTIES
    OUTPUT_NAME "alpha_recorder_e2e_scenario"
    FOLDER "alpha_recorder/core"
    POSITION_INDEPENDENT_CODE ON
)

set(_alpha_recorder_build_full_targets OFF)
if(ALPHA_RECORDER_BUILD_OBS_PLUGIN OR ALPHA_RECORDER_BUILD_E2E_TESTS OR ALPHA_RECORDER_BUILD_CORE_UNIT_TESTS)
    set(_alpha_recorder_build_full_targets ON)
endif()

if(_alpha_recorder_build_full_targets)
find_package(OBS REQUIRED MODULE)
find_package(jansson REQUIRED)
find_package(FFmpeg REQUIRED avcodec avformat avutil swscale)

set(_alpha_recorder_ffmpeg_include_dirs
    "${FFmpeg_avcodec_INCLUDE_DIR}"
    "${FFmpeg_avformat_INCLUDE_DIR}"
    "${FFmpeg_avutil_INCLUDE_DIR}"
    "${FFmpeg_swscale_INCLUDE_DIR}"
)
list(REMOVE_DUPLICATES _alpha_recorder_ffmpeg_include_dirs)

if(APPLE)
    if(EXISTS "${OBS_ROOT}/Frameworks")
        set(alpha_recorder_obs_runtime_bin_dir "${OBS_ROOT}/Frameworks")
    else()
        set(alpha_recorder_obs_runtime_bin_dir "${OBS_ROOT}/bin")
    endif()

    if(EXISTS "${OBS_ROOT}/PlugIns")
        set(alpha_recorder_obs_plugin_dir "${OBS_ROOT}/PlugIns")
    else()
        set(alpha_recorder_obs_plugin_dir "${OBS_ROOT}/obs-plugins")
    endif()
elseif(WIN32)
    set(alpha_recorder_obs_runtime_bin_dir "${OBS_ROOT}/bin/64bit")
    set(alpha_recorder_obs_plugin_dir "${OBS_ROOT}/obs-plugins/64bit")
else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/ObsHelpers.cmake")
    alpha_recorder_linux_runtime_layout("${OBS_ROOT}" alpha_recorder_obs_runtime_bin_dir alpha_recorder_obs_plugin_dir alpha_recorder_obs_data_dir alpha_recorder_obs_runtime_lib_dirs)
endif()

if(APPLE)
    list(APPEND CMAKE_BUILD_RPATH "${alpha_recorder_obs_runtime_bin_dir}")
elseif(UNIX)
    list(APPEND CMAKE_BUILD_RPATH ${alpha_recorder_obs_runtime_lib_dirs})
endif()

add_library(alpha_recorder_core STATIC
    src/core/pair_gate.cpp
    src/core/manifest_writer.cpp
    src/core/export_worker.cpp
    src/core/sidecar_reader.cpp
    src/core/sidecar_writer.cpp
    src/core/settings.cpp
)
alpha_recorder_setup_target(alpha_recorder_core)
alpha_recorder_apply_warnings(alpha_recorder_core)
target_link_libraries(alpha_recorder_core PRIVATE OBS::libobs)
target_link_libraries(alpha_recorder_core PUBLIC alpha_recorder_e2e_scenario jansson::jansson FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale)
target_include_directories(alpha_recorder_core
    BEFORE PRIVATE
        ${_alpha_recorder_ffmpeg_include_dirs}
)
target_include_directories(alpha_recorder_core
    PUBLIC
        $<BUILD_INTERFACE:${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(alpha_recorder_core PROPERTIES
    OUTPUT_NAME "alpha_recorder_core"
    FOLDER "alpha_recorder/core"
    POSITION_INDEPENDENT_CODE ON
)

set(_alpha_recorder_obs_outputs_dir "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/plugins/obs-outputs")
set(_alpha_recorder_private_obs_outputs_dir "${CMAKE_CURRENT_BINARY_DIR}/alpha_recorder_private_obs_outputs")

file(MAKE_DIRECTORY "${_alpha_recorder_private_obs_outputs_dir}")

file(READ "${_alpha_recorder_obs_outputs_dir}/mp4-mux.h" _alpha_recorder_private_mp4_mux_header)
if(NOT _alpha_recorder_private_mp4_mux_header MATCHES "MP4_VIDEO_ONLY_TRACKS")
    string(REPLACE
        "\tMP4_USE_NEGATIVE_CTS = 1 << 3,\n"
        "\tMP4_USE_NEGATIVE_CTS = 1 << 3,\n\t/* Alpha Recorder: construct only video tracks even if the output has an audio encoder for timing */\n\tMP4_VIDEO_ONLY_TRACKS = 1 << 4,\n"
        _alpha_recorder_private_mp4_mux_header
        "${_alpha_recorder_private_mp4_mux_header}"
    )
endif()
if(NOT _alpha_recorder_private_mp4_mux_header MATCHES "mp4_mux_set_video_edit")
    string(REPLACE
        "bool mp4_mux_add_chapter(struct mp4_mux *mux, int64_t dts_usec, const char *name);\nbool mp4_mux_finalise(struct mp4_mux *mux);"
        "bool mp4_mux_add_chapter(struct mp4_mux *mux, int64_t dts_usec, const char *name);\nvoid mp4_mux_set_video_edit(struct mp4_mux *mux, int64_t media_time, int64_t duration);\nbool mp4_mux_finalise(struct mp4_mux *mux);"
        _alpha_recorder_private_mp4_mux_header
        "${_alpha_recorder_private_mp4_mux_header}"
    )
endif()
file(WRITE "${_alpha_recorder_private_obs_outputs_dir}/mp4-mux.h" "${_alpha_recorder_private_mp4_mux_header}")

file(READ "${_alpha_recorder_obs_outputs_dir}/mp4-mux-internal.h" _alpha_recorder_private_mp4_mux_internal)
if(NOT _alpha_recorder_private_mp4_mux_internal MATCHES "has_edit_range")
    string(REPLACE
        "\t/* Highest PTS this track has seen (in usec) */\n\tint64_t last_pts_usec;\n"
        "\t/* Highest PTS this track has seen (in usec) */\n\tint64_t last_pts_usec;\n\t/* Optional edit list range in encoder packet timebase units. */\n\tbool has_edit_range;\n\tint64_t edit_media_time;\n\tint64_t edit_duration;\n"
        _alpha_recorder_private_mp4_mux_internal
        "${_alpha_recorder_private_mp4_mux_internal}"
    )
endif()
file(WRITE "${_alpha_recorder_private_obs_outputs_dir}/mp4-mux-internal.h" "${_alpha_recorder_private_mp4_mux_internal}")

file(READ "${_alpha_recorder_obs_outputs_dir}/mp4-mux.c" _alpha_recorder_private_mp4_mux_source)
if(NOT _alpha_recorder_private_mp4_mux_source MATCHES "has_edit_range")
    string(REPLACE
        "\t\tdelay = util_mul_div64(dts_offset, track->timescale, track->timebase_den);\n\t} else if (track->type == TRACK_AUDIO && track->first_pts < 0) {"
        "\t\tdelay = util_mul_div64(dts_offset, track->timescale, track->timebase_den);\n\t\tif (track->has_edit_range) {\n\t\t\tduration = util_mul_div64(track->edit_duration, 1000, track->timebase_den);\n\t\t\tdelay += util_mul_div64(track->edit_media_time, track->timescale, track->timebase_den);\n\t\t}\n\t} else if (track->type == TRACK_AUDIO && track->first_pts < 0) {"
        _alpha_recorder_private_mp4_mux_source
        "${_alpha_recorder_private_mp4_mux_source}"
    )
    string(REPLACE
        "\t\t/* Subtract priming delay from total duration */\n\t\tduration -= util_mul_div64(delay, 1000, track->timescale);\n\t}\n"
        "\t\t/* Subtract priming delay from total duration */\n\t\tduration -= util_mul_div64(delay, 1000, track->timescale);\n\t} else if (track->has_edit_range) {\n\t\tduration = util_mul_div64(track->edit_duration, 1000, track->timebase_den);\n\t\tdelay = util_mul_div64(track->edit_media_time, track->timescale, track->timebase_den);\n\t}\n"
        _alpha_recorder_private_mp4_mux_source
        "${_alpha_recorder_private_mp4_mux_source}"
    )
endif()
if(NOT _alpha_recorder_private_mp4_mux_source MATCHES "MP4_VIDEO_ONLY_TRACKS")
    string(REPLACE
        "\tfor (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {\n\t\tobs_encoder_t *enc = obs_output_get_audio_encoder(output, i);\n\t\tif (!enc)\n\t\t\tcontinue;\n\t\tadd_track(mux, enc);\n\t}\n"
        "\tif ((flags & MP4_VIDEO_ONLY_TRACKS) == 0) {\n\t\tfor (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {\n\t\t\tobs_encoder_t *enc = obs_output_get_audio_encoder(output, i);\n\t\t\tif (!enc)\n\t\t\t\tcontinue;\n\t\t\tadd_track(mux, enc);\n\t\t}\n\t}\n"
        _alpha_recorder_private_mp4_mux_source
        "${_alpha_recorder_private_mp4_mux_source}"
    )
endif()
if(NOT _alpha_recorder_private_mp4_mux_source MATCHES "mp4_mux_set_video_edit")
    string(REPLACE
        "\nbool mp4_mux_finalise(struct mp4_mux *mux)\n"
        "\nvoid mp4_mux_set_video_edit(struct mp4_mux *mux, int64_t media_time, int64_t duration)\n{\n\tif (!mux || media_time < 0 || duration <= 0)\n\t\treturn;\n\n\tfor (size_t i = 0; i < mux->tracks.num; i++) {\n\t\tstruct mp4_track *track = &mux->tracks.array[i];\n\t\tif (track->type != TRACK_VIDEO)\n\t\t\tcontinue;\n\n\t\ttrack->has_edit_range = true;\n\t\ttrack->edit_media_time = media_time;\n\t\ttrack->edit_duration = duration;\n\t}\n}\n\nbool mp4_mux_finalise(struct mp4_mux *mux)\n"
        _alpha_recorder_private_mp4_mux_source
        "${_alpha_recorder_private_mp4_mux_source}"
    )
endif()
file(WRITE "${_alpha_recorder_private_obs_outputs_dir}/mp4-mux.c" "${_alpha_recorder_private_mp4_mux_source}")

set(_alpha_recorder_direct_mp4_sources
    src/obs/direct_mp4_muxer.cpp
    src/obs/gpu_texture_alpha_output_sink.cpp
    "${_alpha_recorder_private_obs_outputs_dir}/mp4-mux.c"
    deps/obs/obs-studio/plugins/obs-outputs/rtmp-av1.c
    deps/obs/obs-studio/plugins/obs-outputs/rtmp-hevc.c
)
set(_alpha_recorder_direct_mp4_private_symbol_definitions
    mp4_mux_create=alpha_recorder_private_mp4_mux_create
    mp4_mux_destroy=alpha_recorder_private_mp4_mux_destroy
    mp4_mux_submit_packet=alpha_recorder_private_mp4_mux_submit_packet
    mp4_mux_add_chapter=alpha_recorder_private_mp4_mux_add_chapter
    mp4_mux_set_video_edit=alpha_recorder_private_mp4_mux_set_video_edit
    mp4_mux_finalise=alpha_recorder_private_mp4_mux_finalise
)

function(alpha_recorder_configure_direct_mp4_target target)
    target_compile_definitions(${target} PRIVATE
        ALPHA_RECORDER_HAS_OBS_MP4_MUX_VIDEO_EDIT=1
        ${_alpha_recorder_direct_mp4_private_symbol_definitions}
    )
    target_include_directories(${target} PRIVATE
        $<BUILD_INTERFACE:${_alpha_recorder_private_obs_outputs_dir}>
        $<BUILD_INTERFACE:${_alpha_recorder_obs_outputs_dir}>
    )
endfunction()
endif()

if(_alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_OBS_PLUGIN)
    add_library(alpha_recorder_plugin MODULE
        src/obs/alpha_alignment_engine.cpp
        src/obs/alpha_plane_extractor.cpp
        ${_alpha_recorder_direct_mp4_sources}
        src/obs/diagnostic_log.cpp
        src/obs/gpu_texture_recording_output.cpp
        src/obs/plugin.cpp
        src/obs/recording_session_controller.cpp
        src/obs/settings_ui.cpp
        src/obs/websocket_vendor_api.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_plugin)
    alpha_recorder_apply_warnings(alpha_recorder_plugin)
    target_compile_definitions(alpha_recorder_plugin PRIVATE QT_NO_VERSION_TAGGING)
    target_link_libraries(alpha_recorder_plugin PRIVATE alpha_recorder_core OBS::libobs OBS::frontend-api)
    target_include_directories(alpha_recorder_plugin PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/plugins/obs-websocket/lib>
    )
    alpha_recorder_configure_direct_mp4_target(alpha_recorder_plugin)

    set_target_properties(alpha_recorder_plugin PROPERTIES
        OUTPUT_NAME "alpha_recorder"
        FOLDER "alpha_recorder/obs"
    )

    if(APPLE)
        set_target_properties(alpha_recorder_plugin PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION plugin
        )
        add_custom_command(TARGET alpha_recorder_plugin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${alpha_recorder_obs_plugin_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<TARGET_BUNDLE_DIR:alpha_recorder_plugin>"
                    "${alpha_recorder_obs_plugin_dir}/alpha_recorder.plugin"
            COMMAND codesign --force --sign - "${alpha_recorder_obs_plugin_dir}/alpha_recorder.plugin"
            VERBATIM
        )
    else()
        add_custom_command(TARGET alpha_recorder_plugin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${alpha_recorder_obs_plugin_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:alpha_recorder_plugin>"
                    "${alpha_recorder_obs_plugin_dir}/$<TARGET_FILE_NAME:alpha_recorder_plugin>"
            VERBATIM
        )
    endif()

    if(TARGET OBS::frontend-api)
        set(_alpha_recorder_obs_qt6_cache "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-build/CMakeCache.txt")
        if(EXISTS "${_alpha_recorder_obs_qt6_cache}")
            file(STRINGS "${_alpha_recorder_obs_qt6_cache}" _alpha_recorder_qt6_dir_lines REGEX "^Qt6[A-Za-z0-9_]*_DIR:PATH=")
            foreach(_alpha_recorder_qt6_dir_line IN LISTS _alpha_recorder_qt6_dir_lines)
                string(REGEX REPLACE "^([^:]+):PATH=(.*)$" "\\1;\\2" _alpha_recorder_qt6_dir_pair "${_alpha_recorder_qt6_dir_line}")
                list(GET _alpha_recorder_qt6_dir_pair 0 _alpha_recorder_qt6_dir_var)
                list(GET _alpha_recorder_qt6_dir_pair 1 _alpha_recorder_qt6_dir_value)
                set(${_alpha_recorder_qt6_dir_var} "${_alpha_recorder_qt6_dir_value}" CACHE PATH "Qt6 package location from the pinned OBS build tree" FORCE)
            endforeach()
            if(Qt6_DIR)
                cmake_path(GET Qt6_DIR PARENT_PATH _alpha_recorder_qt6_cmake_dir)
                cmake_path(GET _alpha_recorder_qt6_cmake_dir PARENT_PATH _alpha_recorder_qt6_lib_dir)
                list(PREPEND CMAKE_PREFIX_PATH "${_alpha_recorder_qt6_lib_dir}")
            endif()
        endif()

        find_package(Qt6 REQUIRED COMPONENTS Widgets)
        find_package(CURL REQUIRED)

        if(APPLE AND EXISTS "${OBS_ROOT}/Frameworks/QtWidgets.framework")
            foreach(_alpha_recorder_qt_component IN ITEMS Core Gui Widgets)
                if(TARGET Qt6::${_alpha_recorder_qt_component})
                    set_target_properties(Qt6::${_alpha_recorder_qt_component} PROPERTIES
                        IMPORTED_LOCATION "${OBS_ROOT}/Frameworks/Qt${_alpha_recorder_qt_component}.framework/Versions/A/Qt${_alpha_recorder_qt_component}"
                        IMPORTED_LOCATION_RELEASE "${OBS_ROOT}/Frameworks/Qt${_alpha_recorder_qt_component}.framework/Versions/A/Qt${_alpha_recorder_qt_component}"
                        IMPORTED_LOCATION_RELWITHDEBINFO "${OBS_ROOT}/Frameworks/Qt${_alpha_recorder_qt_component}.framework/Versions/A/Qt${_alpha_recorder_qt_component}"
                    )
                endif()
            endforeach()
        endif()

        target_link_libraries(alpha_recorder_plugin PRIVATE Qt6::Widgets CURL::libcurl)
    endif()
endif()

if(BUILD_TESTING AND _alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_E2E_TESTS)
    add_library(alpha_recorder_e2e_output MODULE
        src/obs/plugin.cpp
        src/obs/obs_output_bridge.cpp
        src/obs/e2e_moving_alpha_source.cpp
        ${_alpha_recorder_direct_mp4_sources}
    )
    alpha_recorder_setup_target(alpha_recorder_e2e_output)
    alpha_recorder_apply_warnings(alpha_recorder_e2e_output)
    target_compile_definitions(alpha_recorder_e2e_output PRIVATE ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
    target_link_libraries(alpha_recorder_e2e_output PRIVATE alpha_recorder_core OBS::libobs)
    target_include_directories(alpha_recorder_e2e_output PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    alpha_recorder_configure_direct_mp4_target(alpha_recorder_e2e_output)
    set_target_properties(alpha_recorder_e2e_output PROPERTIES
        OUTPUT_NAME "alpha_recorder_e2e"
        FOLDER "alpha_recorder/tests/e2e"
    )

    if(APPLE)
        set_target_properties(alpha_recorder_e2e_output PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION plugin
        )
        add_custom_command(TARGET alpha_recorder_e2e_output POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${alpha_recorder_obs_plugin_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<TARGET_BUNDLE_DIR:alpha_recorder_e2e_output>"
                    "${alpha_recorder_obs_plugin_dir}/alpha_recorder_e2e.plugin"
            COMMAND codesign --force --sign - "${alpha_recorder_obs_plugin_dir}/alpha_recorder_e2e.plugin"
            VERBATIM
        )
    else()
        add_custom_command(TARGET alpha_recorder_e2e_output POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${alpha_recorder_obs_plugin_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:alpha_recorder_e2e_output>"
                    "${alpha_recorder_obs_plugin_dir}/$<TARGET_FILE_NAME:alpha_recorder_e2e_output>"
            VERBATIM
        )
    endif()
endif()

if(BUILD_TESTING AND ALPHA_RECORDER_BUILD_UNIT_TESTS)
    add_executable(alpha_recorder_unit_e2e_scenario
        tests/unit/e2e_scenario_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_e2e_scenario)
    alpha_recorder_apply_warnings(alpha_recorder_unit_e2e_scenario)
    target_link_libraries(alpha_recorder_unit_e2e_scenario PRIVATE alpha_recorder_e2e_scenario)
    set_target_properties(alpha_recorder_unit_e2e_scenario PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.e2e_scenario COMMAND alpha_recorder_unit_e2e_scenario)
    set_tests_properties(alpha_recorder.unit.e2e_scenario PROPERTIES LABELS "unit")
endif()

if(BUILD_TESTING AND _alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_UNIT_TESTS AND ALPHA_RECORDER_BUILD_CORE_UNIT_TESTS)
    add_executable(alpha_recorder_unit_pair_gate
        tests/unit/pair_gate_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_pair_gate)
    alpha_recorder_apply_warnings(alpha_recorder_unit_pair_gate)
    target_link_libraries(alpha_recorder_unit_pair_gate PRIVATE alpha_recorder_core)
    set_target_properties(alpha_recorder_unit_pair_gate PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.pair_gate COMMAND alpha_recorder_unit_pair_gate)
    set_tests_properties(alpha_recorder.unit.pair_gate PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_frame_matcher
        tests/unit/frame_matcher_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_frame_matcher)
    alpha_recorder_apply_warnings(alpha_recorder_unit_frame_matcher)
    target_link_libraries(alpha_recorder_unit_frame_matcher PRIVATE alpha_recorder_core)
    set_target_properties(alpha_recorder_unit_frame_matcher PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.frame_matcher COMMAND alpha_recorder_unit_frame_matcher)
    set_tests_properties(alpha_recorder.unit.frame_matcher PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_sidecar_writer
        tests/unit/sidecar_writer_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_sidecar_writer)
    alpha_recorder_apply_warnings(alpha_recorder_unit_sidecar_writer)
    target_link_libraries(alpha_recorder_unit_sidecar_writer PRIVATE alpha_recorder_core)
    set_target_properties(alpha_recorder_unit_sidecar_writer PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.sidecar_writer COMMAND alpha_recorder_unit_sidecar_writer)
    set_tests_properties(alpha_recorder.unit.sidecar_writer PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_recording_session_pause
        tests/unit/recording_session_controller_pause_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_recording_session_pause)
    alpha_recorder_apply_warnings(alpha_recorder_unit_recording_session_pause)
    target_include_directories(alpha_recorder_unit_recording_session_pause PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/obs)
    set_target_properties(alpha_recorder_unit_recording_session_pause PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.recording_session_pause COMMAND alpha_recorder_unit_recording_session_pause)
    set_tests_properties(alpha_recorder.unit.recording_session_pause PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_recording_session_cadence
        tests/unit/recording_session_controller_cadence_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_recording_session_cadence)
    alpha_recorder_apply_warnings(alpha_recorder_unit_recording_session_cadence)
    target_include_directories(alpha_recorder_unit_recording_session_cadence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/obs)
    set_target_properties(alpha_recorder_unit_recording_session_cadence PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.recording_session_cadence COMMAND alpha_recorder_unit_recording_session_cadence)
    set_tests_properties(alpha_recorder.unit.recording_session_cadence PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_alpha_alignment_engine
        tests/unit/alpha_alignment_engine_test.cpp
        src/obs/alpha_alignment_engine.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_alpha_alignment_engine)
    alpha_recorder_apply_warnings(alpha_recorder_unit_alpha_alignment_engine)
    target_include_directories(alpha_recorder_unit_alpha_alignment_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/obs)
    set_target_properties(alpha_recorder_unit_alpha_alignment_engine PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.alpha_alignment_engine COMMAND alpha_recorder_unit_alpha_alignment_engine)
    set_tests_properties(alpha_recorder.unit.alpha_alignment_engine PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_settings
        tests/unit/settings_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_settings)
    alpha_recorder_apply_warnings(alpha_recorder_unit_settings)
    target_link_libraries(alpha_recorder_unit_settings PRIVATE alpha_recorder_core OBS::libobs)
    set_target_properties(alpha_recorder_unit_settings PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.settings COMMAND alpha_recorder_unit_settings)
    set_tests_properties(alpha_recorder.unit.settings PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${alpha_recorder_obs_runtime_bin_dir};PATH=path_list_prepend:${_alpha_recorder_obs_deps_root}/bin"
    )
endif()

if(BUILD_TESTING AND _alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_E2E_TESTS)
    add_executable(alpha_recorder_e2e_host
        tests/e2e/host/main.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_e2e_host)
    alpha_recorder_apply_warnings(alpha_recorder_e2e_host)
    target_link_libraries(alpha_recorder_e2e_host PRIVATE alpha_recorder_core OBS::libobs)
    set_target_properties(alpha_recorder_e2e_host PROPERTIES
        FOLDER "alpha_recorder/tests/e2e"
    )
    add_executable(alpha_recorder_test_encoder
        tests/e2e/test_encoder/main.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_test_encoder)
    alpha_recorder_apply_warnings(alpha_recorder_test_encoder)
    target_link_libraries(alpha_recorder_test_encoder PRIVATE alpha_recorder_core)
    set_target_properties(alpha_recorder_test_encoder PROPERTIES
        FOLDER "alpha_recorder/tests/e2e"
    )

    set(alpha_recorder_e2e_artifact_root "${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/alpha_recorder_e2e")
    set(alpha_recorder_e2e_host_environment_modification
        "ALPHA_RECORDER_STAGE_DIR=set:${OBS_ROOT}"
        "PATH=path_list_prepend:${alpha_recorder_obs_runtime_bin_dir}"
    )
    if(UNIX AND NOT APPLE)
        foreach(alpha_recorder_obs_runtime_lib_dir IN LISTS alpha_recorder_obs_runtime_lib_dirs)
            list(APPEND alpha_recorder_e2e_host_environment_modification
                "LD_LIBRARY_PATH=path_list_prepend:${alpha_recorder_obs_runtime_lib_dir}"
            )
        endforeach()
    endif()

    function(alpha_recorder_add_e2e_scenario scenario_name scenario_path)
        set(host_test_name "alpha_recorder.e2e.${scenario_name}.host")
        set(encoder_test_name "alpha_recorder.e2e.${scenario_name}.test_encoder")

        add_test(NAME ${host_test_name}
            COMMAND alpha_recorder_e2e_host
                --scenario "${scenario_path}"
                --artifact-root "${alpha_recorder_e2e_artifact_root}"
        )
        set_tests_properties(${host_test_name} PROPERTIES
            LABELS "e2e"
            ENVIRONMENT_MODIFICATION "${alpha_recorder_e2e_host_environment_modification}"
        )

        add_test(NAME ${encoder_test_name}
            COMMAND alpha_recorder_test_encoder
                --scenario "${scenario_path}"
                --artifact-root "${alpha_recorder_e2e_artifact_root}"
        )
        set_tests_properties(${encoder_test_name} PROPERTIES
            LABELS "e2e"
            DEPENDS ${host_test_name}
            ENVIRONMENT_MODIFICATION "ALPHA_RECORDER_STAGE_DIR=set:${OBS_ROOT}"
        )
    endfunction()

    alpha_recorder_add_e2e_scenario(happy_path "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/scenarios/basic_pair.scenario")
    alpha_recorder_add_e2e_scenario(drop_backpressure "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/scenarios/drop_backpressure.scenario")
    alpha_recorder_add_e2e_scenario(split_rotation "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/scenarios/split_rotation.scenario")
endif()

if(_alpha_recorder_build_full_targets AND TARGET alpha_recorder_plugin)
    set(alpha_recorder_stage_dir "${CMAKE_CURRENT_SOURCE_DIR}/out/stage/obs" CACHE PATH "Portable OBS staging directory")
    add_custom_target(alpha_recorder_stage_obs_tree
        COMMAND ${CMAKE_COMMAND}
            "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DSTAGE_DIR=${alpha_recorder_stage_dir}"
            "-DOBS_ROOT=${OBS_ROOT}"
            "-DCONFIGURATION=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/StageObsTree.cmake"
        DEPENDS alpha_recorder_plugin
        VERBATIM
    )
    if(TARGET alpha_recorder_e2e_output)
        add_dependencies(alpha_recorder_stage_obs_tree alpha_recorder_e2e_output)
    endif()

    if(BUILD_TESTING AND ALPHA_RECORDER_BUILD_E2E_TESTS)
        add_custom_target(alpha_recorder_run_e2e
            COMMAND ${CMAKE_COMMAND}
                "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DSTAGE_DIR=${alpha_recorder_stage_dir}"
                "-DOBS_ROOT=${OBS_ROOT}"
                "-DCONFIGURATION=$<CONFIG>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/RunE2E.cmake"
            DEPENDS alpha_recorder_stage_obs_tree
            VERBATIM
        )
    endif()
endif()

if(BUILD_TESTING AND _alpha_recorder_build_full_targets AND ALPHA_RECORDER_ENABLE_OBS_APP_E2E)
    add_test(NAME alpha_recorder.e2e.obs_app
        COMMAND ${CMAKE_COMMAND}
            "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DSTAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/obs-app-stage"
            "-DOBS_ROOT=${OBS_ROOT}"
            "-DCONFIGURATION=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/RunObsAppE2E.cmake"
    )
    set_tests_properties(alpha_recorder.e2e.obs_app PROPERTIES
        LABELS "obs-app-e2e;e2e"
        TIMEOUT 420
    )
endif()

if(_alpha_recorder_build_full_targets)
    set(_alpha_recorder_ffmpeg_hints
        "${alpha_recorder_obs_runtime_bin_dir}"
        "${OBS_ROOT}/bin"
        "${OBS_ROOT}/bin/64bit"
        "${_alpha_recorder_obs_deps_root}/bin"
    )
    find_program(ALPHA_RECORDER_FFMPEG_EXECUTABLE ffmpeg HINTS ${_alpha_recorder_ffmpeg_hints})

    function(alpha_recorder_probe_obs_app_e2e_hevc_encoder vendor encoder_name out_available out_reason)
        if(NOT ALPHA_RECORDER_FFMPEG_EXECUTABLE)
            set(${out_available} FALSE PARENT_SCOPE)
            set(${out_reason} "ffmpeg was not found on PATH, so ${vendor} HEVC availability could not be probed" PARENT_SCOPE)
            return()
        endif()

        execute_process(
            COMMAND "${ALPHA_RECORDER_FFMPEG_EXECUTABLE}"
                -nostdin -hide_banner -loglevel error -f lavfi -i color=c=black:s=1920x1080:r=60
                -frames:v 1 -c:v "${encoder_name}" -f null -
            RESULT_VARIABLE _alpha_recorder_probe_result
            OUTPUT_VARIABLE _alpha_recorder_probe_output
            ERROR_VARIABLE _alpha_recorder_probe_error
            TIMEOUT 20
        )

        if(_alpha_recorder_probe_result EQUAL 0)
            set(${out_available} TRUE PARENT_SCOPE)
            set(${out_reason} "${vendor} HEVC encoder opened successfully via ${ALPHA_RECORDER_FFMPEG_EXECUTABLE}" PARENT_SCOPE)
        else()
            string(STRIP "${_alpha_recorder_probe_error}" _alpha_recorder_probe_error)
            string(REPLACE "\n" " | " _alpha_recorder_probe_error "${_alpha_recorder_probe_error}")
            string(REPLACE "\r" "" _alpha_recorder_probe_error "${_alpha_recorder_probe_error}")
            if(_alpha_recorder_probe_error STREQUAL "")
                set(_alpha_recorder_probe_error "ffmpeg exited with ${_alpha_recorder_probe_result}")
            endif()
            set(${out_available} FALSE PARENT_SCOPE)
            set(${out_reason} "${vendor} HEVC encoder probe failed: ${_alpha_recorder_probe_error}" PARENT_SCOPE)
        endif()
    endfunction()

    alpha_recorder_probe_obs_app_e2e_hevc_encoder("NVENC" "hevc_nvenc" _alpha_recorder_obs_app_e2e_has_nvenc _alpha_recorder_obs_app_e2e_nvenc_reason)
    alpha_recorder_probe_obs_app_e2e_hevc_encoder("AMF" "hevc_amf" _alpha_recorder_obs_app_e2e_has_amf _alpha_recorder_obs_app_e2e_amf_reason)

    function(alpha_recorder_probe_obs_app_e2e_vaapi_encoder out_available out_reason)
        if(NOT UNIX OR APPLE)
            set(${out_available} FALSE PARENT_SCOPE)
            set(${out_reason} "VAAPI OBS app E2E is only registered on Linux runtimes" PARENT_SCOPE)
            return()
        endif()
        if(NOT ALPHA_RECORDER_FFMPEG_EXECUTABLE)
            set(${out_available} FALSE PARENT_SCOPE)
            set(${out_reason} "ffmpeg was not found on PATH, so VAAPI HEVC availability could not be probed" PARENT_SCOPE)
            return()
        endif()

        set(_alpha_recorder_vaapi_devices)
        foreach(_alpha_recorder_vaapi_index RANGE 128 143)
            if(EXISTS "/dev/dri/renderD${_alpha_recorder_vaapi_index}")
                list(APPEND _alpha_recorder_vaapi_devices "/dev/dri/renderD${_alpha_recorder_vaapi_index}")
            endif()
        endforeach()
        foreach(_alpha_recorder_vaapi_index RANGE 0 15)
            if(EXISTS "/dev/dri/card${_alpha_recorder_vaapi_index}")
                list(APPEND _alpha_recorder_vaapi_devices "/dev/dri/card${_alpha_recorder_vaapi_index}")
            endif()
        endforeach()
        if(_alpha_recorder_vaapi_devices STREQUAL "")
            set(${out_available} FALSE PARENT_SCOPE)
            set(${out_reason} "no /dev/dri VAAPI device was found" PARENT_SCOPE)
            return()
        endif()

        set(_alpha_recorder_vaapi_errors)
        foreach(_alpha_recorder_vaapi_device IN LISTS _alpha_recorder_vaapi_devices)
            foreach(_alpha_recorder_vaapi_driver IN ITEMS "" "d3d12")
                set(_alpha_recorder_vaapi_env)
                if(NOT _alpha_recorder_vaapi_driver STREQUAL "")
                    list(APPEND _alpha_recorder_vaapi_env "LIBVA_DRIVER_NAME=${_alpha_recorder_vaapi_driver}")
                endif()
                execute_process(
                    COMMAND "${CMAKE_COMMAND}" -E env ${_alpha_recorder_vaapi_env}
                        "${ALPHA_RECORDER_FFMPEG_EXECUTABLE}"
                        -nostdin -hide_banner -loglevel error
                        -init_hw_device "vaapi=alpha_recorder_vaapi:${_alpha_recorder_vaapi_device}"
                        -filter_hw_device alpha_recorder_vaapi
                        -f lavfi -i color=c=black:s=1920x1080:r=60
                        -frames:v 1 -vf format=nv12,hwupload -c:v hevc_vaapi -f null -
                    RESULT_VARIABLE _alpha_recorder_vaapi_probe_result
                    OUTPUT_VARIABLE _alpha_recorder_vaapi_probe_output
                    ERROR_VARIABLE _alpha_recorder_vaapi_probe_error
                    TIMEOUT 20
                )
                if(_alpha_recorder_vaapi_probe_result EQUAL 0)
                    set(${out_available} TRUE PARENT_SCOPE)
                    if(_alpha_recorder_vaapi_driver STREQUAL "")
                        set(${out_reason} "VAAPI HEVC encoder opened successfully on ${_alpha_recorder_vaapi_device}" PARENT_SCOPE)
                    else()
                        set(${out_reason} "VAAPI HEVC encoder opened successfully on ${_alpha_recorder_vaapi_device} with LIBVA_DRIVER_NAME=${_alpha_recorder_vaapi_driver}" PARENT_SCOPE)
                    endif()
                    return()
                endif()
                string(STRIP "${_alpha_recorder_vaapi_probe_error}" _alpha_recorder_vaapi_probe_error)
                string(STRIP "${_alpha_recorder_vaapi_probe_output}" _alpha_recorder_vaapi_probe_output)
                string(REPLACE "\n" " | " _alpha_recorder_vaapi_probe_error "${_alpha_recorder_vaapi_probe_error}")
                string(REPLACE "\n" " | " _alpha_recorder_vaapi_probe_output "${_alpha_recorder_vaapi_probe_output}")
                string(REPLACE "\r" "" _alpha_recorder_vaapi_probe_error "${_alpha_recorder_vaapi_probe_error}")
                string(REPLACE "\r" "" _alpha_recorder_vaapi_probe_output "${_alpha_recorder_vaapi_probe_output}")
                if(NOT _alpha_recorder_vaapi_probe_output STREQUAL "")
                    if(_alpha_recorder_vaapi_probe_error STREQUAL "")
                        set(_alpha_recorder_vaapi_probe_error "${_alpha_recorder_vaapi_probe_output}")
                    else()
                        string(APPEND _alpha_recorder_vaapi_probe_error " | ${_alpha_recorder_vaapi_probe_output}")
                    endif()
                endif()
                if(_alpha_recorder_vaapi_probe_error STREQUAL "")
                    set(_alpha_recorder_vaapi_probe_error "ffmpeg exited with ${_alpha_recorder_vaapi_probe_result}")
                endif()
                if(_alpha_recorder_vaapi_driver STREQUAL "")
                    list(APPEND _alpha_recorder_vaapi_errors "${_alpha_recorder_vaapi_device}: ${_alpha_recorder_vaapi_probe_error}")
                else()
                    list(APPEND _alpha_recorder_vaapi_errors "${_alpha_recorder_vaapi_device} (${_alpha_recorder_vaapi_driver}): ${_alpha_recorder_vaapi_probe_error}")
                endif()
            endforeach()
        endforeach()

        string(REPLACE ";" " | " _alpha_recorder_vaapi_error_summary "${_alpha_recorder_vaapi_errors}")
        if(_alpha_recorder_vaapi_error_summary STREQUAL "")
            set(_alpha_recorder_vaapi_error_summary "no candidate /dev/dri VAAPI device could be opened by ffmpeg; on WSL, load vgem and verify `LIBVA_DRIVER_NAME=d3d12 vainfo --display drm --device /dev/dri/card0` first")
        endif()
        set(${out_available} FALSE PARENT_SCOPE)
        set(${out_reason} "VAAPI HEVC encoder probe failed: ${_alpha_recorder_vaapi_error_summary}" PARENT_SCOPE)
    endfunction()

    alpha_recorder_probe_obs_app_e2e_vaapi_encoder(_alpha_recorder_obs_app_e2e_has_vaapi _alpha_recorder_obs_app_e2e_vaapi_reason)

    set(_alpha_recorder_obs_app_e2e_has_obs_nvenc_plugin TRUE)
    set(_alpha_recorder_obs_app_e2e_obs_nvenc_plugin_reason "OBS NVENC plugin is available")
    if(UNIX AND NOT APPLE)
        set(_alpha_recorder_obs_app_e2e_has_obs_nvenc_plugin FALSE)
        set(_alpha_recorder_obs_app_e2e_obs_nvenc_plugin_reason
            "OBS NVENC RGB encoder plugin is not present under ${alpha_recorder_obs_plugin_dir}, while software RGB HEVC-alpha targets may still register if FFmpeg opens hevc_nvenc"
        )
        foreach(_alpha_recorder_obs_nvenc_plugin_name IN ITEMS obs-nvenc.so libobs-nvenc.so)
            if(EXISTS "${alpha_recorder_obs_plugin_dir}/${_alpha_recorder_obs_nvenc_plugin_name}")
                set(_alpha_recorder_obs_app_e2e_has_obs_nvenc_plugin TRUE)
                set(_alpha_recorder_obs_app_e2e_obs_nvenc_plugin_reason
                    "OBS NVENC RGB encoder plugin found at ${alpha_recorder_obs_plugin_dir}/${_alpha_recorder_obs_nvenc_plugin_name}"
                )
                break()
            endif()
        endforeach()
    endif()
    set(_alpha_recorder_obs_app_e2e_has_nvenc_rgb FALSE)
    if(_alpha_recorder_obs_app_e2e_has_nvenc AND _alpha_recorder_obs_app_e2e_has_obs_nvenc_plugin)
        set(_alpha_recorder_obs_app_e2e_has_nvenc_rgb TRUE)
        set(_alpha_recorder_obs_app_e2e_nvenc_rgb_reason "${_alpha_recorder_obs_app_e2e_nvenc_reason}")
    elseif(_alpha_recorder_obs_app_e2e_has_nvenc)
        set(_alpha_recorder_obs_app_e2e_nvenc_rgb_reason "${_alpha_recorder_obs_app_e2e_obs_nvenc_plugin_reason}")
    else()
        set(_alpha_recorder_obs_app_e2e_nvenc_rgb_reason "${_alpha_recorder_obs_app_e2e_nvenc_reason}")
    endif()

    set(_alpha_recorder_obs_app_e2e_registered_targets)
    set(_alpha_recorder_obs_app_e2e_skipped_targets)

    function(alpha_recorder_add_obs_app_e2e_target target_name stage_suffix)
        set(options)
        set(one_value_args FINALIZATION_FORMAT RGB_ENCODER WIDTH HEIGHT FPS RECORD_SECONDS MAX_RECORD_SECONDS SYNC_RECORD_SECONDS SYNC_ATTEMPTS DURABILITY_RECORD_SECONDS)
        set(multi_value_args)
        cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

        if(NOT ARG_FINALIZATION_FORMAT)
            set(ARG_FINALIZATION_FORMAT "mask_png_mov")
        endif()
        if(NOT ARG_RGB_ENCODER)
            set(ARG_RGB_ENCODER "software")
        endif()
        if(NOT ARG_WIDTH)
            set(ARG_WIDTH "1920")
        endif()
        if(NOT ARG_HEIGHT)
            set(ARG_HEIGHT "1080")
        endif()
        if(NOT ARG_FPS)
            set(ARG_FPS "60")
        endif()
        if(ARG_RECORD_SECONDS AND NOT ARG_SYNC_RECORD_SECONDS)
            set(ARG_SYNC_RECORD_SECONDS "${ARG_RECORD_SECONDS}")
        endif()
        if(ARG_MAX_RECORD_SECONDS AND NOT ARG_DURABILITY_RECORD_SECONDS)
            set(ARG_DURABILITY_RECORD_SECONDS "${ARG_MAX_RECORD_SECONDS}")
        endif()
        if(NOT ARG_SYNC_RECORD_SECONDS)
            set(ARG_SYNC_RECORD_SECONDS "2")
        endif()
        if(NOT ARG_SYNC_ATTEMPTS)
            set(ARG_SYNC_ATTEMPTS "5")
        endif()
        if(NOT ARG_DURABILITY_RECORD_SECONDS)
            set(ARG_DURABILITY_RECORD_SECONDS "30")
        endif()

        add_custom_target(${target_name}
            COMMAND ${CMAKE_COMMAND}
                "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DSTAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/obs-app-stage-${stage_suffix}"
                "-DOBS_ROOT=${OBS_ROOT}"
                "-DCONFIGURATION=$<CONFIG>"
                "-DFINALIZATION_FORMAT=${ARG_FINALIZATION_FORMAT}"
                "-DRGB_ENCODER=${ARG_RGB_ENCODER}"
                "-DWIDTH=${ARG_WIDTH}"
                "-DHEIGHT=${ARG_HEIGHT}"
                "-DFPS=${ARG_FPS}"
                "-DSYNC_RECORD_SECONDS=${ARG_SYNC_RECORD_SECONDS}"
                "-DSYNC_ATTEMPTS=${ARG_SYNC_ATTEMPTS}"
                "-DDURABILITY_RECORD_SECONDS=${ARG_DURABILITY_RECORD_SECONDS}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/RunObsAppE2E.cmake"
            VERBATIM
        )
        set(_alpha_recorder_obs_app_e2e_registered_targets ${_alpha_recorder_obs_app_e2e_registered_targets} ${target_name} PARENT_SCOPE)
    endfunction()

    function(alpha_recorder_skip_obs_app_e2e_target target_name reason)
        add_custom_target(${target_name}
            COMMAND ${CMAKE_COMMAND} -E echo "Alpha Recorder OBS app E2E target ${target_name} skipped: ${reason}"
            VERBATIM
        )
        list(APPEND _alpha_recorder_obs_app_e2e_skipped_targets "${target_name}: ${reason}")
        set(_alpha_recorder_obs_app_e2e_skipped_targets ${_alpha_recorder_obs_app_e2e_skipped_targets} PARENT_SCOPE)
    endfunction()

    alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_png_mov fhd60-rgb-sw-alpha-png-mov
        FINALIZATION_FORMAT mask_png_mov
        RGB_ENCODER software
    )
    if(_alpha_recorder_obs_app_e2e_has_nvenc)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc fhd60-rgb-sw-alpha-hevc-nvenc
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc "${_alpha_recorder_obs_app_e2e_nvenc_reason}")
    endif()
    if(_alpha_recorder_obs_app_e2e_has_amf)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_amf fhd60-rgb-sw-alpha-hevc-amf
            FINALIZATION_FORMAT mask_hevc_amf
            RGB_ENCODER software
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_amf "${_alpha_recorder_obs_app_e2e_amf_reason}")
    endif()
    if(_alpha_recorder_obs_app_e2e_has_vaapi)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_vaapi fhd60-rgb-sw-alpha-hevc-vaapi
            FINALIZATION_FORMAT mask_hevc_vaapi
            RGB_ENCODER software
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_vaapi "${_alpha_recorder_obs_app_e2e_vaapi_reason}")
    endif()
    if(_alpha_recorder_obs_app_e2e_has_nvenc_rgb)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_png_mov fhd60-rgb-nvenc-hevc-alpha-png-mov
            FINALIZATION_FORMAT mask_png_mov
            RGB_ENCODER nvenc_hevc
        )
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc fhd60-rgb-nvenc-hevc-alpha-hevc-nvenc
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_png_mov "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
    endif()
    if(_alpha_recorder_obs_app_e2e_has_amf)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_png_mov fhd60-rgb-amf-hevc-alpha-png-mov
            FINALIZATION_FORMAT mask_png_mov
            RGB_ENCODER amd_hevc
        )
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_hevc_amf fhd60-rgb-amf-hevc-alpha-hevc-amf
            FINALIZATION_FORMAT mask_hevc_amf
            RGB_ENCODER amd_hevc
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_png_mov "${_alpha_recorder_obs_app_e2e_amf_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_hevc_amf "${_alpha_recorder_obs_app_e2e_amf_reason}")
    endif()
    function(alpha_recorder_add_obs_app_e2e_hardware_load_matrix load_name width height fps)
        if(_alpha_recorder_obs_app_e2e_has_nvenc_rgb)
            alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_nvenc_hevc_alpha_png_mov ${load_name}-rgb-nvenc-hevc-alpha-png-mov
                FINALIZATION_FORMAT mask_png_mov
                RGB_ENCODER nvenc_hevc
                WIDTH ${width}
                HEIGHT ${height}
                FPS ${fps}
            )
            alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_nvenc_hevc_alpha_hevc_nvenc ${load_name}-rgb-nvenc-hevc-alpha-hevc-nvenc
                FINALIZATION_FORMAT mask_hevc_nvenc
                RGB_ENCODER nvenc_hevc
                WIDTH ${width}
                HEIGHT ${height}
                FPS ${fps}
            )
        else()
            alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_nvenc_hevc_alpha_png_mov "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
            alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_nvenc_hevc_alpha_hevc_nvenc "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        endif()
        if(_alpha_recorder_obs_app_e2e_has_amf)
            alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_amf_hevc_alpha_png_mov ${load_name}-rgb-amf-hevc-alpha-png-mov
                FINALIZATION_FORMAT mask_png_mov
                RGB_ENCODER amd_hevc
                WIDTH ${width}
                HEIGHT ${height}
                FPS ${fps}
            )
            alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_amf_hevc_alpha_hevc_amf ${load_name}-rgb-amf-hevc-alpha-hevc-amf
                FINALIZATION_FORMAT mask_hevc_amf
                RGB_ENCODER amd_hevc
                WIDTH ${width}
                HEIGHT ${height}
                FPS ${fps}
            )
        else()
            alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_amf_hevc_alpha_png_mov "${_alpha_recorder_obs_app_e2e_amf_reason}")
            alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_${load_name}_rgb_amf_hevc_alpha_hevc_amf "${_alpha_recorder_obs_app_e2e_amf_reason}")
        endif()
        set(_alpha_recorder_obs_app_e2e_registered_targets ${_alpha_recorder_obs_app_e2e_registered_targets} PARENT_SCOPE)
        set(_alpha_recorder_obs_app_e2e_skipped_targets ${_alpha_recorder_obs_app_e2e_skipped_targets} PARENT_SCOPE)
    endfunction()

    alpha_recorder_add_obs_app_e2e_hardware_load_matrix(wqhd60 2560 1440 60)
    alpha_recorder_add_obs_app_e2e_hardware_load_matrix(4k30 3840 2160 30)

    string(REPLACE ";" "\n  " _alpha_recorder_obs_app_e2e_registered_summary "${_alpha_recorder_obs_app_e2e_registered_targets}")
    string(REPLACE ";" "\n  " _alpha_recorder_obs_app_e2e_skipped_summary "${_alpha_recorder_obs_app_e2e_skipped_targets}")
    if(_alpha_recorder_obs_app_e2e_registered_summary STREQUAL "")
        set(_alpha_recorder_obs_app_e2e_registered_summary "(none)")
    endif()
    if(_alpha_recorder_obs_app_e2e_skipped_summary STREQUAL "")
        set(_alpha_recorder_obs_app_e2e_skipped_summary "(none)")
    endif()
    message(STATUS "Alpha Recorder OBS app E2E runtime matrix registered:\n  ${_alpha_recorder_obs_app_e2e_registered_summary}")
    message(STATUS "Alpha Recorder OBS app E2E runtime matrix skipped:\n  ${_alpha_recorder_obs_app_e2e_skipped_summary}")

    add_custom_target(alpha_recorder_run_obs_app_e2e
        DEPENDS ${_alpha_recorder_obs_app_e2e_registered_targets}
        COMMAND ${CMAKE_COMMAND} -E echo "Alpha Recorder OBS app E2E registered targets: ${_alpha_recorder_obs_app_e2e_registered_targets}"
        COMMAND ${CMAKE_COMMAND} -E echo "Alpha Recorder OBS app E2E skipped targets: ${_alpha_recorder_obs_app_e2e_skipped_targets}"
        VERBATIM
    )
endif()

install(TARGETS alpha_recorder_e2e_scenario ARCHIVE DESTINATION lib)

if(TARGET alpha_recorder_core)
    install(TARGETS alpha_recorder_core
        ARCHIVE DESTINATION lib
    )
endif()

if(TARGET alpha_recorder_plugin)
    install(TARGETS alpha_recorder_plugin
        RUNTIME DESTINATION obs-plugins/64bit
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
        BUNDLE DESTINATION obs-plugins
    )
endif()

install(DIRECTORY include/ DESTINATION include PATTERN "*.in" EXCLUDE)
install(DIRECTORY "${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}/" DESTINATION include)
