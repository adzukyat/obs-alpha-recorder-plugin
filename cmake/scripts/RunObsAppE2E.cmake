cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/ObsHelpers.cmake")

alpha_recorder_require(REPO_ROOT)
alpha_recorder_require(BUILD_DIR)

set(STAGE_DIR "${BUILD_DIR}/Testing/Temporary/obs-app-stage" CACHE PATH "Portable OBS app E2E stage directory")
set(OBS_ROOT "$ENV{OBS_ROOT}" CACHE PATH "OBS runtime root")
set(CONFIGURATION "RelWithDebInfo" CACHE STRING "Build configuration")
set(PORT 0 CACHE STRING "obs-websocket port, or 0 for an ephemeral port")
set(SYNC_RECORD_SECONDS 2 CACHE STRING "Short recording duration for sync-correctness attempts")
set(SYNC_ATTEMPTS 5 CACHE STRING "Number of short sync-correctness recording attempts")
set(DURABILITY_RECORD_SECONDS 30 CACHE STRING "Sustained recording duration for durability validation")
set(WIDTH 1920 CACHE STRING "Canvas width")
set(HEIGHT 1080 CACHE STRING "Canvas height")
set(FPS 60 CACHE STRING "Recording FPS")
set(RECORD_FORMAT "mkv" CACHE STRING "OBS recording container format")
set(ARTIFACT_BASE "" CACHE PATH "Optional OBS app E2E artifact base directory")
set(OUTPUT_MODE "simple" CACHE STRING "OBS output mode for E2E: simple or advanced-standard")
set(RECORD_AUDIO_ENCODER "aac" CACHE STRING "OBS recording audio encoder for E2E: aac or pcm_s16le")
set(WITH_AUDIO OFF CACHE BOOL "Add a deterministic audio source to the OBS app E2E scene")
set(RGB_ENCODER "software" CACHE STRING "RGB recording encoder profile: software, apple_hevc, nvenc_hevc, or amd_hevc")
set(FINALIZATION_FORMAT "mask_png_mov" CACHE STRING "Alpha Recorder finalization format")
set(HEVC_QUALITY_PROFILE "high_quality" CACHE STRING "Alpha Recorder HEVC quality profile")
set(HEVC_QUALITY_CQ 19 CACHE STRING "Alpha Recorder HEVC CQ value")
set(HEVC_PRESET "nvenc_p3" CACHE STRING "Alpha Recorder HEVC preset")
set(HEVC_NVENC_TUNE "hq" CACHE STRING "Alpha Recorder NVENC tune")
set(HEVC_GOP_SIZE 0 CACHE STRING "Alpha Recorder HEVC GOP size in frames, or 0 for auto")
set(SKIP_BUILD OFF CACHE BOOL "Skip plugin build before OBS app E2E")
set(SKIP_STAGE OFF CACHE BOOL "Skip staging before OBS app E2E")
set(KEEP_OBS_OPEN OFF CACHE BOOL "Keep OBS open after the E2E run")
set(ALLOW_OVERLOAD OFF CACHE BOOL "Continue OBS app E2E after OBS reports overload")
set(VERIFY_NLE_TIMELINE OFF CACHE BOOL "Verify alpha ISO BMFF output uses an NLE-friendly exact-CFR timeline")

alpha_recorder_abs_path(REPO_ROOT "${REPO_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")
alpha_recorder_abs_path(BUILD_DIR "${BUILD_DIR}" "${REPO_ROOT}")
alpha_recorder_abs_path(STAGE_DIR "${STAGE_DIR}" "${REPO_ROOT}")
if(NOT OBS_ROOT STREQUAL "")
    alpha_recorder_abs_path(OBS_ROOT "${OBS_ROOT}" "${REPO_ROOT}")
endif()
if(NOT ARTIFACT_BASE STREQUAL "")
    alpha_recorder_abs_path(ARTIFACT_BASE "${ARTIFACT_BASE}" "${REPO_ROOT}")
endif()

if(CONFIGURATION STREQUAL "")
    set(CONFIGURATION "RelWithDebInfo")
endif()

if(NOT SKIP_BUILD)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIGURATION}" --target alpha_recorder_plugin alpha_recorder_e2e_output
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

if(NOT SKIP_STAGE)
    set(stage_args)
    if(UNIX AND NOT APPLE)
        list(APPEND stage_args "-DSKIP_PLUGIN_OVERLAY=ON")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DREPO_ROOT=${REPO_ROOT}"
            "-DBUILD_DIR=${BUILD_DIR}"
            "-DSTAGE_DIR=${STAGE_DIR}"
            "-DOBS_ROOT=${OBS_ROOT}"
            "-DCONFIGURATION=${CONFIGURATION}"
            ${stage_args}
            -P "${CMAKE_CURRENT_LIST_DIR}/StageObsTree.cmake"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

unset(BUN_EXECUTABLE)
if(WIN32 AND DEFINED ENV{USERPROFILE})
    file(GLOB _alpha_recorder_scoop_bun_paths LIST_DIRECTORIES false "$ENV{USERPROFILE}/scoop/apps/bun/*/bun.exe")
    list(FILTER _alpha_recorder_scoop_bun_paths EXCLUDE REGEX "/current/")
    list(SORT _alpha_recorder_scoop_bun_paths)
    list(REVERSE _alpha_recorder_scoop_bun_paths)
    foreach(_alpha_recorder_scoop_bun_path IN LISTS _alpha_recorder_scoop_bun_paths)
        if(EXISTS "${_alpha_recorder_scoop_bun_path}")
            set(BUN_EXECUTABLE "${_alpha_recorder_scoop_bun_path}")
            break()
        endif()
    endforeach()
endif()

if(NOT BUN_EXECUTABLE AND DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/.bun/bin/bun")
    set(BUN_EXECUTABLE "$ENV{HOME}/.bun/bin/bun")
endif()

if(NOT BUN_EXECUTABLE)
    find_program(BUN_EXECUTABLE bun REQUIRED)
endif()

set(args
    "${CMAKE_CURRENT_LIST_DIR}/obs_app_e2e.ts"
    "--repo-root" "${REPO_ROOT}"
    "--build-dir" "${BUILD_DIR}"
    "--stage-dir" "${STAGE_DIR}"
    "--configuration" "${CONFIGURATION}"
    "--port" "${PORT}"
    "--sync-record-seconds" "${SYNC_RECORD_SECONDS}"
    "--sync-attempts" "${SYNC_ATTEMPTS}"
    "--durability-record-seconds" "${DURABILITY_RECORD_SECONDS}"
    "--width" "${WIDTH}"
    "--height" "${HEIGHT}"
    "--fps" "${FPS}"
    "--record-format" "${RECORD_FORMAT}"
    "--output-mode" "${OUTPUT_MODE}"
    "--record-audio-encoder" "${RECORD_AUDIO_ENCODER}"
    "--rgb-encoder" "${RGB_ENCODER}"
    "--finalization-format" "${FINALIZATION_FORMAT}"
    "--hevc-quality-profile" "${HEVC_QUALITY_PROFILE}"
    "--hevc-quality-cq" "${HEVC_QUALITY_CQ}"
    "--hevc-preset" "${HEVC_PRESET}"
    "--hevc-nvenc-tune" "${HEVC_NVENC_TUNE}"
    "--hevc-gop-size" "${HEVC_GOP_SIZE}"
)
if(KEEP_OBS_OPEN)
    list(APPEND args "--keep-obs-open")
endif()
if(ALLOW_OVERLOAD)
    list(APPEND args "--allow-overload")
endif()
if(VERIFY_NLE_TIMELINE)
    list(APPEND args "--verify-nle-timeline")
endif()
if(WITH_AUDIO)
    list(APPEND args "--with-audio")
endif()
if(NOT ARTIFACT_BASE STREQUAL "")
    list(APPEND args "--artifact-base" "${ARTIFACT_BASE}")
endif()

if(APPLE)
    execute_process(
        COMMAND /bin/sh -c "exec \"$0\" \"$@\"" "${BUN_EXECUTABLE}" ${args}
        RESULT_VARIABLE run_result
    )
else()
    execute_process(
        COMMAND "${BUN_EXECUTABLE}" ${args}
        RESULT_VARIABLE run_result
    )
endif()
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "OBS app E2E failed with exit code ${run_result}")
endif()
