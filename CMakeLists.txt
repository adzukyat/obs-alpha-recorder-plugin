cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ALPHA_RECORDER_PROJECT_VERSION)
string(STRIP "${ALPHA_RECORDER_PROJECT_VERSION}" ALPHA_RECORDER_PROJECT_VERSION)
if(NOT ALPHA_RECORDER_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-.+][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as X.Y.Z; got '${ALPHA_RECORDER_PROJECT_VERSION}'")
endif()

project(alpha_recorder_plugin
    VERSION "${ALPHA_RECORDER_PROJECT_VERSION}"
    DESCRIPTION "OBS plugin for recording a time-aligned grayscale alpha-mask movie"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/cmake/finders")

if(WIN32 OR APPLE)
    set(_alpha_recorder_obs_build_cache "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-build/CMakeCache.txt")
    set(_alpha_recorder_obs_deps_roots)
    if(EXISTS "${_alpha_recorder_obs_build_cache}")
        file(STRINGS "${_alpha_recorder_obs_build_cache}" _alpha_recorder_obs_prefix_line
            REGEX "^CMAKE_PREFIX_PATH:PATH=" LIMIT_COUNT 1)
        string(REGEX REPLACE "^CMAKE_PREFIX_PATH:PATH=" "" _alpha_recorder_obs_prefixes
            "${_alpha_recorder_obs_prefix_line}")
        foreach(_alpha_recorder_obs_prefix IN LISTS _alpha_recorder_obs_prefixes)
            if(IS_DIRECTORY "${_alpha_recorder_obs_prefix}")
                list(APPEND _alpha_recorder_obs_deps_roots "${_alpha_recorder_obs_prefix}")
            endif()
        endforeach()
    endif()

    if(NOT _alpha_recorder_obs_deps_roots)
        file(GLOB _alpha_recorder_obs_deps_roots LIST_DIRECTORIES true
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/.deps/obs-deps-[0-9]*")
        list(SORT _alpha_recorder_obs_deps_roots COMPARE NATURAL ORDER DESCENDING)
    endif()

    if(_alpha_recorder_obs_deps_roots)
        list(APPEND CMAKE_PREFIX_PATH ${_alpha_recorder_obs_deps_roots})
    endif()
endif()

include(ProjectOptions)
include(Warnings)
include(GNUInstallDirs)

alpha_recorder_set_project_options()

include(CTest)

set(ALPHA_RECORDER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/alpha_recorder/version.hpp.in"
    "${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}/alpha_recorder/version.hpp"
    @ONLY
)

if(NOT DEFINED OBS_ROOT OR OBS_ROOT STREQUAL "")
    include("${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-root.cmake" OPTIONAL)
endif()
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

set(_alpha_recorder_build_full_targets OFF)
if(ALPHA_RECORDER_BUILD_OBS_PLUGIN OR (BUILD_TESTING AND ALPHA_RECORDER_BUILD_UNIT_TESTS))
    set(_alpha_recorder_build_full_targets ON)
endif()

if(_alpha_recorder_build_full_targets)
find_package(OBS REQUIRED MODULE)
find_package(FFmpeg REQUIRED avcodec avformat avutil)

set(_alpha_recorder_ffmpeg_include_dirs
    "${FFmpeg_avcodec_INCLUDE_DIR}"
    "${FFmpeg_avformat_INCLUDE_DIR}"
    "${FFmpeg_avutil_INCLUDE_DIR}"
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

set(_alpha_recorder_gpu_texture_muxer_sources
    src/direct_mp4_muxer.cpp
    src/gpu_texture_alpha_output_sink.cpp
    src/matroska_hevc_muxer.cpp
)
endif()

if(_alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_OBS_PLUGIN)
    add_library(alpha_recorder_plugin MODULE
        src/alpha_alignment_engine.cpp
        src/alpha_mask_video_writer.cpp
        src/alpha_plane_extractor.cpp
        ${_alpha_recorder_gpu_texture_muxer_sources}
        src/diagnostic_log.cpp
        src/gpu_texture_recording_output.cpp
        src/gpu_texture_timeline_ledger.cpp
        src/replay_generation_ledger.cpp
        src/plugin.cpp
        src/recording_session_controller.cpp
        src/settings.cpp
        src/settings_ui.cpp
        src/websocket_vendor_api.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_plugin)
    alpha_recorder_apply_warnings(alpha_recorder_plugin)
    target_compile_definitions(alpha_recorder_plugin PRIVATE QT_NO_VERSION_TAGGING)
    target_link_libraries(alpha_recorder_plugin PRIVATE
        OBS::libobs
        OBS::frontend-api
        FFmpeg::avcodec
        FFmpeg::avformat
        FFmpeg::avutil
    )
    target_include_directories(alpha_recorder_plugin PRIVATE
        $<BUILD_INTERFACE:${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-studio/plugins/obs-websocket/lib>
        ${_alpha_recorder_ffmpeg_include_dirs}
    )

    set_target_properties(alpha_recorder_plugin PROPERTIES
        OUTPUT_NAME "alpha_recorder"
        FOLDER "alpha_recorder"
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

if(_alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_OBS_PLUGIN)
    add_library(alpha_recorder_obs_app_e2e_source MODULE EXCLUDE_FROM_ALL
        src/obs_app_e2e_source.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_obs_app_e2e_source)
    alpha_recorder_apply_warnings(alpha_recorder_obs_app_e2e_source)
    target_link_libraries(alpha_recorder_obs_app_e2e_source PRIVATE OBS::libobs)
    set_target_properties(alpha_recorder_obs_app_e2e_source PROPERTIES
        OUTPUT_NAME "alpha_recorder_obs_app_e2e_source"
        FOLDER "alpha_recorder/tests/obs-app-e2e"
    )

    if(APPLE)
        set_target_properties(alpha_recorder_obs_app_e2e_source PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION plugin
        )
    endif()
endif()

if(BUILD_TESTING AND _alpha_recorder_build_full_targets AND ALPHA_RECORDER_BUILD_UNIT_TESTS)
    add_executable(alpha_recorder_unit_recording_session_cadence
        tests/unit/recording_session_controller_cadence_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_recording_session_cadence)
    alpha_recorder_apply_warnings(alpha_recorder_unit_recording_session_cadence)
    target_include_directories(alpha_recorder_unit_recording_session_cadence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(alpha_recorder_unit_recording_session_cadence PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.recording_session_cadence COMMAND alpha_recorder_unit_recording_session_cadence)
    set_tests_properties(alpha_recorder.unit.recording_session_cadence PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_alpha_alignment_engine
        tests/unit/alpha_alignment_engine_test.cpp
        src/alpha_alignment_engine.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_alpha_alignment_engine)
    alpha_recorder_apply_warnings(alpha_recorder_unit_alpha_alignment_engine)
    target_include_directories(alpha_recorder_unit_alpha_alignment_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(alpha_recorder_unit_alpha_alignment_engine PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.alpha_alignment_engine COMMAND alpha_recorder_unit_alpha_alignment_engine)
    set_tests_properties(alpha_recorder.unit.alpha_alignment_engine PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_gpu_texture_timeline_ledger
        tests/unit/gpu_texture_timeline_ledger_test.cpp
        src/gpu_texture_timeline_ledger.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_gpu_texture_timeline_ledger)
    alpha_recorder_apply_warnings(alpha_recorder_unit_gpu_texture_timeline_ledger)
    target_include_directories(alpha_recorder_unit_gpu_texture_timeline_ledger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}
    )
    set_target_properties(alpha_recorder_unit_gpu_texture_timeline_ledger PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.gpu_texture_timeline_ledger COMMAND alpha_recorder_unit_gpu_texture_timeline_ledger)
    set_tests_properties(alpha_recorder.unit.gpu_texture_timeline_ledger PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_replay_generation_ledger
        tests/unit/replay_generation_ledger_test.cpp
        src/replay_generation_ledger.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_replay_generation_ledger)
    alpha_recorder_apply_warnings(alpha_recorder_unit_replay_generation_ledger)
    target_include_directories(alpha_recorder_unit_replay_generation_ledger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}
    )
    set_target_properties(alpha_recorder_unit_replay_generation_ledger PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.replay_generation_ledger COMMAND alpha_recorder_unit_replay_generation_ledger)
    set_tests_properties(alpha_recorder.unit.replay_generation_ledger PROPERTIES LABELS "unit")

    add_executable(alpha_recorder_unit_settings
        tests/unit/settings_test.cpp
        src/alpha_mask_video_writer.cpp
        src/settings.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_settings)
    alpha_recorder_apply_warnings(alpha_recorder_unit_settings)
    target_link_libraries(alpha_recorder_unit_settings PRIVATE
        OBS::libobs
        FFmpeg::avcodec
        FFmpeg::avformat
        FFmpeg::avutil
    )
    target_include_directories(alpha_recorder_unit_settings PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ALPHA_RECORDER_GENERATED_INCLUDE_DIR}
        ${_alpha_recorder_ffmpeg_include_dirs}
    )
    set_target_properties(alpha_recorder_unit_settings PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.settings COMMAND alpha_recorder_unit_settings)
    set_tests_properties(alpha_recorder.unit.settings PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${alpha_recorder_obs_runtime_bin_dir};PATH=path_list_prepend:${_alpha_recorder_obs_deps_root}/bin"
    )
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
endif()

if(BUILD_TESTING AND TARGET alpha_recorder_plugin AND ALPHA_RECORDER_ENABLE_OBS_APP_E2E)
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

if(TARGET alpha_recorder_plugin)
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
        set(options ALLOW_OVERLOAD EXPERIMENTAL WITH_AUDIO VERIFY_NLE_TIMELINE VERIFY_REMUX_TIMELINE STRICT_ALL_FRAMES REQUIRE_MAIN_PTS_GAP REQUIRE_PACKET_REORDER REQUIRE_REPLAY_UNDERFLOW REQUIRE_REPLAY_CATCHUP REQUIRE_OVERLOAD REQUIRE_TAIL_REPEAT BEST_EFFORT_SYNC)
        set(one_value_args FINALIZATION_FORMAT RGB_ENCODER WIDTH HEIGHT FPS FPS_NUM FPS_DEN RECORD_FORMAT OUTPUT_MODE RECORD_AUDIO_ENCODER RECORD_SECONDS MAX_RECORD_SECONDS SYNC_RECORD_SECONDS SYNC_ATTEMPTS DURABILITY_RECORD_SECONDS HEVC_GOP_SIZE PHASE_SWEEP_STEPS PAUSE_AT_MS PAUSE_DURATION_MS SPLIT_AT_MS OVERLOAD_PULSE_AT_MS OVERLOAD_PULSE_DURATION_MS OVERLOAD_PULSE_DELAY_MS TEST_FAULT FAULT_SEGMENT REPLAY_GAP_PACKETS EXPECT_RESULT)
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
        if(NOT ARG_FPS_NUM)
            set(ARG_FPS_NUM "${ARG_FPS}")
        endif()
        if(NOT ARG_FPS_DEN)
            set(ARG_FPS_DEN "1")
        endif()
        if(NOT ARG_RECORD_FORMAT)
            set(ARG_RECORD_FORMAT "mkv")
        endif()
        if(NOT ARG_OUTPUT_MODE)
            set(ARG_OUTPUT_MODE "simple")
        endif()
        if(NOT ARG_RECORD_AUDIO_ENCODER)
            set(ARG_RECORD_AUDIO_ENCODER "aac")
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
        if(NOT DEFINED ARG_HEVC_GOP_SIZE)
            set(ARG_HEVC_GOP_SIZE "0")
        endif()
        if(NOT ARG_PHASE_SWEEP_STEPS)
            set(ARG_PHASE_SWEEP_STEPS "0")
        endif()
        if(NOT DEFINED ARG_PAUSE_AT_MS)
            set(ARG_PAUSE_AT_MS "-1")
        endif()
        if(NOT DEFINED ARG_PAUSE_DURATION_MS)
            set(ARG_PAUSE_DURATION_MS "0")
        endif()
        if(NOT DEFINED ARG_SPLIT_AT_MS)
            set(ARG_SPLIT_AT_MS "-1")
        endif()
        if(NOT DEFINED ARG_OVERLOAD_PULSE_AT_MS)
            set(ARG_OVERLOAD_PULSE_AT_MS "-1")
        endif()
        if(NOT DEFINED ARG_OVERLOAD_PULSE_DURATION_MS)
            set(ARG_OVERLOAD_PULSE_DURATION_MS "0")
        endif()
        if(NOT DEFINED ARG_OVERLOAD_PULSE_DELAY_MS)
            set(ARG_OVERLOAD_PULSE_DELAY_MS "0")
        endif()
        if(NOT DEFINED ARG_FAULT_SEGMENT)
            set(ARG_FAULT_SEGMENT "0")
        endif()
        if(NOT DEFINED ARG_REPLAY_GAP_PACKETS)
            set(ARG_REPLAY_GAP_PACKETS "24")
        endif()
        if(NOT ARG_EXPECT_RESULT)
            set(ARG_EXPECT_RESULT "normal")
        endif()
        if(ARG_ALLOW_OVERLOAD)
            set(ARG_ALLOW_OVERLOAD_VALUE "ON")
        else()
            set(ARG_ALLOW_OVERLOAD_VALUE "OFF")
        endif()
        if(ARG_WITH_AUDIO)
            set(ARG_WITH_AUDIO_VALUE "ON")
        else()
            set(ARG_WITH_AUDIO_VALUE "OFF")
        endif()
        if(ARG_VERIFY_NLE_TIMELINE)
            set(ARG_VERIFY_NLE_TIMELINE_VALUE "ON")
        else()
            set(ARG_VERIFY_NLE_TIMELINE_VALUE "OFF")
        endif()
        if(ARG_VERIFY_REMUX_TIMELINE)
            set(ARG_VERIFY_REMUX_TIMELINE_VALUE "ON")
        else()
            set(ARG_VERIFY_REMUX_TIMELINE_VALUE "OFF")
        endif()
        foreach(_alpha_recorder_bool_name IN ITEMS
                STRICT_ALL_FRAMES
                REQUIRE_MAIN_PTS_GAP
                REQUIRE_PACKET_REORDER
                REQUIRE_REPLAY_UNDERFLOW
                REQUIRE_REPLAY_CATCHUP
                REQUIRE_OVERLOAD
                REQUIRE_TAIL_REPEAT)
            if(ARG_${_alpha_recorder_bool_name})
                set(ARG_${_alpha_recorder_bool_name}_VALUE "ON")
            else()
                set(ARG_${_alpha_recorder_bool_name}_VALUE "OFF")
            endif()
        endforeach()

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
                "-DFPS_NUM=${ARG_FPS_NUM}"
                "-DFPS_DEN=${ARG_FPS_DEN}"
                "-DRECORD_FORMAT=${ARG_RECORD_FORMAT}"
                "-DOUTPUT_MODE=${ARG_OUTPUT_MODE}"
                "-DRECORD_AUDIO_ENCODER=${ARG_RECORD_AUDIO_ENCODER}"
                "-DSYNC_RECORD_SECONDS=${ARG_SYNC_RECORD_SECONDS}"
                "-DSYNC_ATTEMPTS=${ARG_SYNC_ATTEMPTS}"
                "-DDURABILITY_RECORD_SECONDS=${ARG_DURABILITY_RECORD_SECONDS}"
                "-DHEVC_GOP_SIZE=${ARG_HEVC_GOP_SIZE}"
                "-DALLOW_OVERLOAD=${ARG_ALLOW_OVERLOAD_VALUE}"
                "-DWITH_AUDIO=${ARG_WITH_AUDIO_VALUE}"
                "-DVERIFY_NLE_TIMELINE=${ARG_VERIFY_NLE_TIMELINE_VALUE}"
                "-DVERIFY_REMUX_TIMELINE=${ARG_VERIFY_REMUX_TIMELINE_VALUE}"
                "-DSTRICT_ALL_FRAMES=${ARG_STRICT_ALL_FRAMES_VALUE}"
                "-DPHASE_SWEEP_STEPS=${ARG_PHASE_SWEEP_STEPS}"
                "-DREQUIRE_MAIN_PTS_GAP=${ARG_REQUIRE_MAIN_PTS_GAP_VALUE}"
                "-DREQUIRE_PACKET_REORDER=${ARG_REQUIRE_PACKET_REORDER_VALUE}"
                "-DREQUIRE_REPLAY_UNDERFLOW=${ARG_REQUIRE_REPLAY_UNDERFLOW_VALUE}"
                "-DREQUIRE_REPLAY_CATCHUP=${ARG_REQUIRE_REPLAY_CATCHUP_VALUE}"
                "-DREQUIRE_OVERLOAD=${ARG_REQUIRE_OVERLOAD_VALUE}"
                "-DREQUIRE_TAIL_REPEAT=${ARG_REQUIRE_TAIL_REPEAT_VALUE}"
                "-DPAUSE_AT_MS=${ARG_PAUSE_AT_MS}"
                "-DPAUSE_DURATION_MS=${ARG_PAUSE_DURATION_MS}"
                "-DSPLIT_AT_MS=${ARG_SPLIT_AT_MS}"
                "-DOVERLOAD_PULSE_AT_MS=${ARG_OVERLOAD_PULSE_AT_MS}"
                "-DOVERLOAD_PULSE_DURATION_MS=${ARG_OVERLOAD_PULSE_DURATION_MS}"
                "-DOVERLOAD_PULSE_DELAY_MS=${ARG_OVERLOAD_PULSE_DELAY_MS}"
                "-DBEST_EFFORT_SYNC=${ARG_BEST_EFFORT_SYNC}"
                "-DTEST_FAULT=${ARG_TEST_FAULT}"
                "-DFAULT_SEGMENT=${ARG_FAULT_SEGMENT}"
                "-DREPLAY_GAP_PACKETS=${ARG_REPLAY_GAP_PACKETS}"
                "-DEXPECT_RESULT=${ARG_EXPECT_RESULT}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/RunObsAppE2E.cmake"
            VERBATIM
        )
        if(NOT ARG_EXPERIMENTAL)
            set(_alpha_recorder_obs_app_e2e_registered_targets ${_alpha_recorder_obs_app_e2e_registered_targets} ${target_name} PARENT_SCOPE)
        endif()
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
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc_gop120 fhd60-rgb-sw-alpha-hevc-nvenc-gop120
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            HEVC_GOP_SIZE 120
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc "${_alpha_recorder_obs_app_e2e_nvenc_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc_gop120 "${_alpha_recorder_obs_app_e2e_nvenc_reason}")
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
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_mp4 fhd60-rgb-nvenc-hevc-alpha-hevc-nvenc-mp4
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            RECORD_FORMAT mp4
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_mkv_remux_timeline fhd60-rgb-nvenc-hevc-alpha-hevc-nvenc-mkv-remux-timeline
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            RECORD_FORMAT mkv
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 2
            WITH_AUDIO
            VERIFY_REMUX_TIMELINE
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_gop120 fhd60-rgb-nvenc-hevc-alpha-hevc-nvenc-gop120
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            HEVC_GOP_SIZE 120
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_nle_timeline fhd60-rgb-nvenc-hevc-alpha-hevc-nvenc-nle-timeline
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            RECORD_FORMAT mov
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 2
            WITH_AUDIO
            VERIFY_NLE_TIMELINE
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_hybrid_mov fhd60-rgb-nvenc-hevc-alpha-hevc-nvenc-hybrid-mov
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            RECORD_FORMAT hybrid_mov
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 2
            VERIFY_NLE_TIMELINE
            EXPERIMENTAL
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_png_mov "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_mp4 "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_mkv_remux_timeline "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_gop120 "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_hybrid_mov "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
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
    if(_alpha_recorder_obs_app_e2e_has_nvenc_rgb)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_4k60_allow_overload_rgb_nvenc_hevc_alpha_hevc_nvenc 4k60-allow-overload-rgb-nvenc-hevc-alpha-hevc-nvenc
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            WIDTH 3840
            HEIGHT 2160
            FPS 60
            SYNC_RECORD_SECONDS 4
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 12
            HEVC_GOP_SIZE 120
            BEST_EFFORT_SYNC
            ALLOW_OVERLOAD
            EXPERIMENTAL
        )
    else()
        alpha_recorder_skip_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_4k60_allow_overload_rgb_nvenc_hevc_alpha_hevc_nvenc "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
    endif()

    set(_alpha_recorder_sync_pr_targets)
    set(_alpha_recorder_sync_nightly_targets)
    set(_alpha_recorder_sync_release_targets)
    if(_alpha_recorder_obs_app_e2e_has_nvenc)
        alpha_recorder_add_obs_app_e2e_target(alpha_recorder_run_obs_app_e2e_sync_pr_strict
            sync-pr-strict
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 2
            DURABILITY_RECORD_SECONDS 3
            STRICT_ALL_FRAMES
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_pr_targets
            alpha_recorder_run_obs_app_e2e_sync_pr_strict)

        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_stop_boundary_reconcile
            sync-stop-boundary-reconcile
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 1
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 0.1
            TEST_FAULT unwritten-main-tail
            STRICT_ALL_FRAMES
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_pr_targets
            alpha_recorder_run_obs_app_e2e_sync_stop_boundary_reconcile)

        foreach(_alpha_recorder_sync_fault IN ITEMS
                missing-prefix
                missing-alpha-generation
                missing-tail
                ambiguous-generation)
            string(REPLACE "-" "_" _alpha_recorder_sync_fault_target
                "${_alpha_recorder_sync_fault}")
            alpha_recorder_add_obs_app_e2e_target(
                alpha_recorder_run_obs_app_e2e_sync_fault_${_alpha_recorder_sync_fault_target}
                sync-fault-${_alpha_recorder_sync_fault}
                FINALIZATION_FORMAT mask_hevc_nvenc
                RGB_ENCODER software
                SYNC_RECORD_SECONDS 1
                SYNC_ATTEMPTS 1
                DURABILITY_RECORD_SECONDS 1
                TEST_FAULT ${_alpha_recorder_sync_fault}
                EXPECT_RESULT sync-invalid
                EXPERIMENTAL
            )
            list(APPEND _alpha_recorder_sync_nightly_targets
                alpha_recorder_run_obs_app_e2e_sync_fault_${_alpha_recorder_sync_fault_target})
        endforeach()
        list(APPEND _alpha_recorder_sync_pr_targets
            alpha_recorder_run_obs_app_e2e_sync_fault_missing_alpha_generation)

        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_fault_best_effort
            sync-fault-best-effort
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 1
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 1
            TEST_FAULT missing-tail
            BEST_EFFORT_SYNC
            EXPECT_RESULT normal
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_fault_mux_finalize
            sync-fault-mux-finalize
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 1
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 1
            TEST_FAULT mux-finalize
            EXPECT_RESULT no-alpha
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_fault_deactivate_timeout
            sync-fault-deactivate-timeout
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 1
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 1
            TEST_FAULT deactivate-timeout
            EXPECT_RESULT no-alpha
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_fault_publish_rename
            sync-fault-publish-rename
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 1
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 1
            TEST_FAULT publish-rename
            EXPECT_RESULT temp-preserved
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_nightly_targets
            alpha_recorder_run_obs_app_e2e_sync_fault_best_effort
            alpha_recorder_run_obs_app_e2e_sync_fault_mux_finalize
            alpha_recorder_run_obs_app_e2e_sync_fault_deactivate_timeout
            alpha_recorder_run_obs_app_e2e_sync_fault_publish_rename)

        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_phase_sweep
            sync-phase-sweep
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 0.35
            SYNC_ATTEMPTS 20
            DURABILITY_RECORD_SECONDS 1
            PHASE_SWEEP_STEPS 20
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_5994
            sync-5994
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            FPS_NUM 60000
            FPS_DEN 1001
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 3
            DURABILITY_RECORD_SECONDS 5
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_pause_resume
            sync-pause-resume
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            OUTPUT_MODE advanced-standard
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 5
            DURABILITY_RECORD_SECONDS 3
            PHASE_SWEEP_STEPS 5
            PAUSE_AT_MS 750
            PAUSE_DURATION_MS 517
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_split
            sync-split
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            OUTPUT_MODE advanced-standard
            SYNC_RECORD_SECONDS 6
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 7
            SPLIT_AT_MS 750
            BEST_EFFORT_SYNC
            ALLOW_OVERLOAD
            EXPECT_RESULT split-published
            EXPERIMENTAL
        )
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_split_failure_isolated
            sync-split-failure-isolated
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            OUTPUT_MODE advanced-standard
            SYNC_RECORD_SECONDS 6
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 7
            SPLIT_AT_MS 750
            TEST_FAULT mux-finalize
            FAULT_SEGMENT 1
            BEST_EFFORT_SYNC
            ALLOW_OVERLOAD
            EXPECT_RESULT split-isolated
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_nightly_targets
            alpha_recorder_run_obs_app_e2e_sync_phase_sweep
            alpha_recorder_run_obs_app_e2e_sync_5994
            alpha_recorder_run_obs_app_e2e_sync_pause_resume
            alpha_recorder_run_obs_app_e2e_sync_split
            alpha_recorder_run_obs_app_e2e_sync_split_failure_isolated)

        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_replay_gap_recovery
            sync-replay-gap-recovery
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 4
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 5
            TEST_FAULT replay-evidence-gap
            REPLAY_GAP_PACKETS 24
            BEST_EFFORT_SYNC
            REQUIRE_REPLAY_UNDERFLOW
            REQUIRE_REPLAY_CATCHUP
            REQUIRE_TAIL_REPEAT
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_release_targets
            alpha_recorder_run_obs_app_e2e_sync_replay_gap_recovery)

        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_overload_software_fail_closed
            sync-overload-software-fail-closed
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER software
            SYNC_RECORD_SECONDS 4
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 5
            OVERLOAD_PULSE_AT_MS 750
            OVERLOAD_PULSE_DURATION_MS 900
            OVERLOAD_PULSE_DELAY_MS 25
            ALLOW_OVERLOAD
            REQUIRE_OVERLOAD
            EXPECT_RESULT sync-invalid
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_release_targets
            alpha_recorder_run_obs_app_e2e_sync_overload_software_fail_closed)
    else()
        foreach(_alpha_recorder_sync_target IN ITEMS
                alpha_recorder_run_obs_app_e2e_sync_pr_strict
                alpha_recorder_run_obs_app_e2e_sync_fault_missing_prefix
                alpha_recorder_run_obs_app_e2e_sync_fault_missing_alpha_generation
                alpha_recorder_run_obs_app_e2e_sync_fault_missing_tail
                alpha_recorder_run_obs_app_e2e_sync_fault_ambiguous_generation
                alpha_recorder_run_obs_app_e2e_sync_fault_best_effort
                alpha_recorder_run_obs_app_e2e_sync_fault_mux_finalize
                alpha_recorder_run_obs_app_e2e_sync_fault_deactivate_timeout
                alpha_recorder_run_obs_app_e2e_sync_fault_publish_rename
                alpha_recorder_run_obs_app_e2e_sync_phase_sweep
                alpha_recorder_run_obs_app_e2e_sync_5994
                alpha_recorder_run_obs_app_e2e_sync_pause_resume
                alpha_recorder_run_obs_app_e2e_sync_split
                alpha_recorder_run_obs_app_e2e_sync_split_failure_isolated
                alpha_recorder_run_obs_app_e2e_sync_replay_gap_recovery
                alpha_recorder_run_obs_app_e2e_sync_overload_software_fail_closed)
            alpha_recorder_skip_obs_app_e2e_target(
                ${_alpha_recorder_sync_target}
                "${_alpha_recorder_obs_app_e2e_nvenc_reason}")
        endforeach()
    endif()

    if(_alpha_recorder_obs_app_e2e_has_nvenc_rgb)
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_texture_lag_counter_generation_proof
            sync-texture-lag-counter-generation-proof
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 1
            DURABILITY_RECORD_SECONDS 2
            TEST_FAULT graphics-lag-counter
            EXPECT_RESULT normal
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_release_targets
            alpha_recorder_run_obs_app_e2e_sync_texture_lag_counter_generation_proof)
        alpha_recorder_add_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_packet_reorder
            sync-packet-reorder
            FINALIZATION_FORMAT mask_hevc_nvenc
            RGB_ENCODER nvenc_hevc
            SYNC_RECORD_SECONDS 2
            SYNC_ATTEMPTS 3
            DURABILITY_RECORD_SECONDS 5
            REQUIRE_PACKET_REORDER
            EXPECT_RESULT normal-or-sync-invalid
            EXPERIMENTAL
        )
        list(APPEND _alpha_recorder_sync_nightly_targets
            alpha_recorder_run_obs_app_e2e_sync_packet_reorder)
        list(APPEND _alpha_recorder_sync_release_targets
            alpha_recorder_run_obs_app_e2e_4k60_allow_overload_rgb_nvenc_hevc_alpha_hevc_nvenc)
        list(APPEND _alpha_recorder_sync_release_targets
            alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc_mkv_remux_timeline)
    else()
        alpha_recorder_skip_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_texture_lag_counter_generation_proof
            "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
        alpha_recorder_skip_obs_app_e2e_target(
            alpha_recorder_run_obs_app_e2e_sync_packet_reorder
            "${_alpha_recorder_obs_app_e2e_nvenc_rgb_reason}")
    endif()

    add_custom_target(alpha_recorder_test_sync_unit
        COMMAND ${CMAKE_CTEST_COMMAND}
            --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
            -C $<CONFIG>
            -L unit
            --output-on-failure
        DEPENDS
            alpha_recorder_unit_recording_session_cadence
            alpha_recorder_unit_alpha_alignment_engine
            alpha_recorder_unit_gpu_texture_timeline_ledger
            alpha_recorder_unit_replay_generation_ledger
            alpha_recorder_unit_settings
        VERBATIM
    )
    add_custom_target(alpha_recorder_test_sync_pr
        DEPENDS alpha_recorder_test_sync_unit ${_alpha_recorder_sync_pr_targets}
    )
    add_custom_target(alpha_recorder_test_sync_nightly
        DEPENDS alpha_recorder_test_sync_pr ${_alpha_recorder_sync_nightly_targets}
    )
    add_custom_target(alpha_recorder_test_sync_release
        DEPENDS alpha_recorder_test_sync_nightly ${_alpha_recorder_sync_release_targets}
    )

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

if(TARGET alpha_recorder_plugin)
    install(TARGETS alpha_recorder_plugin
        RUNTIME DESTINATION obs-plugins/64bit
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
        BUNDLE DESTINATION obs-plugins
    )
endif()
