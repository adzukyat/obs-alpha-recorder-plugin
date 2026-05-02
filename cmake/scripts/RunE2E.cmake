cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/ObsHelpers.cmake")

alpha_recorder_require(REPO_ROOT)
alpha_recorder_require(BUILD_DIR)

set(STAGE_DIR "${REPO_ROOT}/out/stage/obs" CACHE PATH "Portable OBS stage directory")
set(OBS_ROOT "$ENV{OBS_ROOT}" CACHE PATH "OBS runtime root")
set(CONFIGURATION "Debug" CACHE STRING "Build configuration")
set(SKIP_STAGE OFF CACHE BOOL "Skip staging before running deterministic E2E")

alpha_recorder_abs_path(REPO_ROOT "${REPO_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")
alpha_recorder_abs_path(BUILD_DIR "${BUILD_DIR}" "${REPO_ROOT}")
alpha_recorder_abs_path(STAGE_DIR "${STAGE_DIR}" "${REPO_ROOT}")
if(NOT OBS_ROOT STREQUAL "")
    alpha_recorder_abs_path(OBS_ROOT "${OBS_ROOT}" "${REPO_ROOT}")
endif()

if(CONFIGURATION STREQUAL "")
    set(CONFIGURATION "RelWithDebInfo")
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

if(APPLE)
    set(stage_bin_path "${STAGE_DIR}/bin")
    set(stage_plugin_path "${STAGE_DIR}/obs-plugins/alpha_recorder_e2e.plugin")
    if(NOT EXISTS "${stage_plugin_path}")
        set(stage_plugin_path "${STAGE_DIR}/obs-plugins/alpha_recorder_e2e.dylib")
    endif()
else()
    set(stage_bin_path "${STAGE_DIR}/bin/64bit")
    set(stage_plugin_path "${STAGE_DIR}/obs-plugins/64bit/alpha_recorder_e2e.dll")
endif()

if(NOT EXISTS "${stage_plugin_path}")
    message(FATAL_ERROR "Expected staged E2E plugin is missing: ${stage_plugin_path}")
endif()
if(NOT EXISTS "${stage_bin_path}")
    message(FATAL_ERROR "Expected staged OBS runtime bin directory is missing: ${stage_bin_path}")
endif()

set(ENV{ALPHA_RECORDER_STAGE_DIR} "${STAGE_DIR}")
set(ENV{ALPHA_RECORDER_SCENARIO_DIR} "${REPO_ROOT}/tests/e2e/scenarios")
if(WIN32)
    set(ENV{PATH} "${stage_bin_path};$ENV{PATH}")
else()
    set(ENV{PATH} "${stage_bin_path}:$ENV{PATH}")
    if(APPLE)
        set(ENV{DYLD_LIBRARY_PATH} "${stage_bin_path}:$ENV{DYLD_LIBRARY_PATH}")
    endif()
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" -C "${CONFIGURATION}" -L e2e --output-on-failure
    COMMAND_ERROR_IS_FATAL ANY
)
