cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/ObsHelpers.cmake")

alpha_recorder_require(REPO_ROOT)

set(OBS_ROOT "$ENV{OBS_ROOT}" CACHE PATH "Existing OBS runtime root")
set(VERSION_FILE "${REPO_ROOT}/deps/obs.version" CACHE FILEPATH "OBS version manifest")
set(CONFIG_FILE "${REPO_ROOT}/deps/obs/obs-root.cmake" CACHE FILEPATH "Generated OBS root CMake fragment")
set(SOURCE_DIR "${REPO_ROOT}/deps/obs/obs-studio" CACHE PATH "OBS source checkout")
set(BUILD_DIR "${REPO_ROOT}/deps/obs/obs-build" CACHE PATH "OBS build directory")
set(INSTALL_DIR "" CACHE PATH "OBS install/runtime directory")
set(SOURCE_URL "https://github.com/obsproject/obs-studio.git" CACHE STRING "OBS source URL")
set(GENERATOR "" CACHE STRING "Generator used for OBS source builds")
set(ARCHITECTURE "" CACHE STRING "Optional CMake architecture for OBS source builds")
set(CONFIGURATION "RelWithDebInfo" CACHE STRING "OBS build configuration")
set(CLONE_SOURCE OFF CACHE BOOL "Clone or refresh the pinned OBS source")
set(BUILD_FROM_SOURCE OFF CACHE BOOL "Build OBS from source")

if(GENERATOR STREQUAL "")
    if(APPLE)
        set(GENERATOR "Xcode")
    elseif(WIN32)
        set(GENERATOR "Visual Studio 17 2022")
    else()
        set(GENERATOR "Ninja")
    endif()
endif()

if(CONFIGURATION STREQUAL "")
    set(CONFIGURATION "RelWithDebInfo")
endif()

if(WIN32 AND ARCHITECTURE STREQUAL "")
    set(ARCHITECTURE "x64")
endif()

alpha_recorder_abs_path(REPO_ROOT "${REPO_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")
alpha_recorder_abs_path(VERSION_FILE "${VERSION_FILE}" "${REPO_ROOT}")
alpha_recorder_abs_path(CONFIG_FILE "${CONFIG_FILE}" "${REPO_ROOT}")
alpha_recorder_abs_path(SOURCE_DIR "${SOURCE_DIR}" "${REPO_ROOT}")
alpha_recorder_abs_path(BUILD_DIR "${BUILD_DIR}" "${REPO_ROOT}")
if(INSTALL_DIR STREQUAL "")
    set(INSTALL_DIR "${BUILD_DIR}/rundir/${CONFIGURATION}")
else()
    alpha_recorder_abs_path(INSTALL_DIR "${INSTALL_DIR}" "${REPO_ROOT}")
endif()

alpha_recorder_read_obs_tag(OBS_TAG "${VERSION_FILE}")

if(NOT CLONE_SOURCE AND NOT BUILD_FROM_SOURCE)
    if(OBS_ROOT STREQUAL "")
        message(FATAL_ERROR "An OBS developer tree is required. Set OBS_ROOT, pass -DOBS_ROOT=..., or configure with -DCLONE_SOURCE=ON/-DBUILD_FROM_SOURCE=ON.")
    endif()
    alpha_recorder_abs_path(OBS_ROOT "${OBS_ROOT}" "${REPO_ROOT}")
    alpha_recorder_validate_obs_runtime("${OBS_ROOT}")
    alpha_recorder_validate_obs_developer("${SOURCE_DIR}" "${BUILD_DIR}" "${CONFIGURATION}")
    alpha_recorder_write_obs_root_config("${OBS_ROOT}" "${CONFIG_FILE}")
    message(STATUS "Validated staged OBS developer tree at ${OBS_ROOT}")
    message(STATUS "Wrote OBS CMake config to ${CONFIG_FILE}")
    return()
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

if(EXISTS "${SOURCE_DIR}")
    if(NOT EXISTS "${SOURCE_DIR}/.git")
        message(FATAL_ERROR "Existing OBS source directory is not a git checkout: ${SOURCE_DIR}")
    endif()
    if(CLONE_SOURCE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" fetch --depth 1 origin --tags COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" checkout --force "${OBS_TAG}" COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --exact-match HEAD
            OUTPUT_VARIABLE current_tag OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE tag_result)
        if(NOT tag_result EQUAL 0 OR NOT current_tag STREQUAL OBS_TAG)
            message(FATAL_ERROR "OBS source checkout at ${SOURCE_DIR} is on '${current_tag}' but the pinned tag is '${OBS_TAG}'. Re-run with -DCLONE_SOURCE=ON to refresh it.")
        endif()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" submodule update --init --recursive COMMAND_ERROR_IS_FATAL ANY)
else()
    if(NOT CLONE_SOURCE)
        message(FATAL_ERROR "No OBS source tree found at ${SOURCE_DIR}. Re-run with -DCLONE_SOURCE=ON to fetch the pinned OBS tag.")
    endif()
    cmake_path(GET SOURCE_DIR PARENT_PATH source_parent)
    file(MAKE_DIRECTORY "${source_parent}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" clone --depth 1 --branch "${OBS_TAG}" --recurse-submodules "${SOURCE_URL}" "${SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" submodule update --init --recursive COMMAND_ERROR_IS_FATAL ANY)
endif()

set(configure_args -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" "-DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}")
if(APPLE)
    list(APPEND configure_args
        "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/ObsBootstrapProjectInclude.cmake"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0"
        "-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF"
        "-DOBS_COMPILE_DEPRECATION_AS_WARNING=ON"
        "-DENABLE_VIRTUALCAM=OFF"
    )
endif()
if(NOT ARCHITECTURE STREQUAL "")
    list(APPEND configure_args -A "${ARCHITECTURE}")
endif()

set(_alpha_recorder_obs_cache "${BUILD_DIR}/CMakeCache.txt")
if(EXISTS "${_alpha_recorder_obs_cache}")
    file(STRINGS "${_alpha_recorder_obs_cache}" _alpha_recorder_obs_cached_generator REGEX "^CMAKE_GENERATOR:INTERNAL=" LIMIT_COUNT 1)
    if(_alpha_recorder_obs_cached_generator)
        string(REGEX REPLACE "^CMAKE_GENERATOR:INTERNAL=" "" _alpha_recorder_obs_cached_generator "${_alpha_recorder_obs_cached_generator}")
        if(NOT _alpha_recorder_obs_cached_generator STREQUAL GENERATOR)
            message(STATUS "Removing stale OBS build directory configured with ${_alpha_recorder_obs_cached_generator}; expected ${GENERATOR}")
            file(REMOVE_RECURSE "${BUILD_DIR}")
        endif()
    endif()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args} COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIGURATION}" --target install COMMAND_ERROR_IS_FATAL ANY)

alpha_recorder_resolve_obs_runtime_root(OBS_RUNTIME_ROOT "${INSTALL_DIR}")
alpha_recorder_validate_obs_runtime("${OBS_RUNTIME_ROOT}")
alpha_recorder_validate_obs_developer("${SOURCE_DIR}" "${BUILD_DIR}" "${CONFIGURATION}")
alpha_recorder_write_obs_root_config("${OBS_RUNTIME_ROOT}" "${CONFIG_FILE}")

alpha_recorder_write_json_manifest("${REPO_ROOT}/deps/obs/obs-source.manifest.json"
    "sourceUrl=${SOURCE_URL}"
    "tag=${OBS_TAG}"
    "sourceDir=${SOURCE_DIR}"
    "buildDir=${BUILD_DIR}"
    "installDir=${INSTALL_DIR}"
    "obsRoot=${OBS_RUNTIME_ROOT}"
    "generator=${GENERATOR}"
    "architecture=${ARCHITECTURE}"
    "configuration=${CONFIGURATION}"
)

message(STATUS "Built and staged OBS developer tree to ${INSTALL_DIR}")
message(STATUS "Wrote OBS CMake config to ${CONFIG_FILE}")
