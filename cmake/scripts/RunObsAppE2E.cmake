cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/ObsHelpers.cmake")

alpha_recorder_require(REPO_ROOT)
alpha_recorder_require(BUILD_DIR)

set(STAGE_DIR "${BUILD_DIR}/Testing/Temporary/obs-app-stage" CACHE PATH "Portable OBS app E2E stage directory")
set(OBS_ROOT "$ENV{OBS_ROOT}" CACHE PATH "OBS runtime root")
set(CONFIGURATION "RelWithDebInfo" CACHE STRING "Build configuration")
set(PORT 0 CACHE STRING "obs-websocket port, or 0 for an ephemeral port")
set(RECORD_SECONDS 5 CACHE STRING "Recording duration")
set(WIDTH 1280 CACHE STRING "Canvas width")
set(HEIGHT 720 CACHE STRING "Canvas height")
set(FINALIZATION_FORMAT "mask_prores_422" CACHE STRING "Alpha Recorder finalization format")
set(SKIP_BUILD OFF CACHE BOOL "Skip plugin build before OBS app E2E")
set(SKIP_STAGE OFF CACHE BOOL "Skip staging before OBS app E2E")
set(KEEP_OBS_OPEN OFF CACHE BOOL "Keep OBS open after the E2E run")

alpha_recorder_abs_path(REPO_ROOT "${REPO_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")
alpha_recorder_abs_path(BUILD_DIR "${BUILD_DIR}" "${REPO_ROOT}")
alpha_recorder_abs_path(STAGE_DIR "${STAGE_DIR}" "${REPO_ROOT}")
if(NOT OBS_ROOT STREQUAL "")
    alpha_recorder_abs_path(OBS_ROOT "${OBS_ROOT}" "${REPO_ROOT}")
endif()

if(CONFIGURATION STREQUAL "")
    set(CONFIGURATION "RelWithDebInfo")
endif()

if(NOT SKIP_BUILD)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIGURATION}" --target alpha_recorder_plugin alpha_recorder_frontend
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

if(NOT SKIP_STAGE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DREPO_ROOT=${REPO_ROOT}"
            "-DBUILD_DIR=${BUILD_DIR}"
            "-DSTAGE_DIR=${STAGE_DIR}"
            "-DOBS_ROOT=${OBS_ROOT}"
            "-DCONFIGURATION=${CONFIGURATION}"
            -P "${CMAKE_CURRENT_LIST_DIR}/StageObsTree.cmake"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

set(BUN_EXECUTABLE "")
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
    "--record-seconds" "${RECORD_SECONDS}"
    "--width" "${WIDTH}"
    "--height" "${HEIGHT}"
    "--finalization-format" "${FINALIZATION_FORMAT}"
)
if(KEEP_OBS_OPEN)
    list(APPEND args "--keep-obs-open")
endif()

execute_process(
    COMMAND "${BUN_EXECUTABLE}" ${args}
    COMMAND_ERROR_IS_FATAL ANY
)
