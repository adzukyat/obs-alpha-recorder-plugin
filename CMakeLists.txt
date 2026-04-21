cmake_minimum_required(VERSION 3.24)

project(alpha_recorder_plugin
    VERSION 0.1.0
    DESCRIPTION "OBS native plugin scaffold for pair-based RGBA recording"
    LANGUAGES CXX
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

alpha_recorder_set_project_options()
alpha_recorder_fetch_test_deps()

include(CTest)

include("${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-root.cmake" OPTIONAL)
find_package(OBS REQUIRED MODULE)
find_package(jansson REQUIRED)
find_package(FFmpeg REQUIRED avcodec avformat avutil swscale)

set(alpha_recorder_obs_runtime_bin_dir "${OBS_ROOT}/bin/64bit")

add_library(alpha_recorder_core STATIC
    src/core/pair_gate.cpp
    src/core/manifest_writer.cpp
    src/core/export_worker.cpp
    src/core/sidecar_reader.cpp
    src/core/sidecar_writer.cpp
    src/core/settings.cpp
    src/core/e2e_scenario.cpp
)
alpha_recorder_setup_target(alpha_recorder_core)
alpha_recorder_apply_warnings(alpha_recorder_core)
target_link_libraries(alpha_recorder_core PRIVATE OBS::libobs)
target_link_libraries(alpha_recorder_core PUBLIC jansson::jansson FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale)
target_include_directories(alpha_recorder_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(alpha_recorder_core PROPERTIES
    OUTPUT_NAME "alpha_recorder_core"
    FOLDER "alpha_recorder/core"
)

if(ALPHA_RECORDER_BUILD_OBS_PLUGIN)
    add_library(alpha_recorder_plugin MODULE
        src/obs/plugin.cpp
        src/obs/recording_session_controller.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_plugin)
    alpha_recorder_apply_warnings(alpha_recorder_plugin)
    target_link_libraries(alpha_recorder_plugin PRIVATE alpha_recorder_core OBS::libobs OBS::frontend-api)
    target_include_directories(alpha_recorder_plugin PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

    set_target_properties(alpha_recorder_plugin PROPERTIES
        OUTPUT_NAME "alpha_recorder"
        FOLDER "alpha_recorder/obs"
    )

    add_custom_command(TARGET alpha_recorder_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_ROOT}/obs-plugins/64bit"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:alpha_recorder_plugin>"
                "${OBS_ROOT}/obs-plugins/64bit/$<TARGET_FILE_NAME:alpha_recorder_plugin>"
        VERBATIM
    )

    if(TARGET OBS::frontend-api)
        if(NOT Qt6_DIR)
            set(_alpha_recorder_obs_qt6_cache "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs/obs-build/CMakeCache.txt")
            if(EXISTS "${_alpha_recorder_obs_qt6_cache}")
                file(STRINGS "${_alpha_recorder_obs_qt6_cache}" _alpha_recorder_qt6_dir_line REGEX "^Qt6_DIR:PATH=" LIMIT_COUNT 1)
                if(_alpha_recorder_qt6_dir_line)
                    string(REGEX REPLACE "^Qt6_DIR:PATH=" "" Qt6_DIR "${_alpha_recorder_qt6_dir_line}")
                    set(Qt6_DIR "${Qt6_DIR}" CACHE PATH "Qt6 package location from the pinned OBS build tree" FORCE)
                endif()
            endif()
        endif()

        find_package(Qt6 REQUIRED COMPONENTS Widgets)

        add_library(alpha_recorder_frontend MODULE
            src/obs/settings_ui.cpp
        )
        alpha_recorder_setup_target(alpha_recorder_frontend)
        alpha_recorder_apply_warnings(alpha_recorder_frontend)
        target_include_directories(alpha_recorder_frontend PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
        target_link_libraries(alpha_recorder_frontend PRIVATE alpha_recorder_core OBS::libobs OBS::frontend-api Qt6::Widgets)
        set_target_properties(alpha_recorder_frontend PROPERTIES
            OUTPUT_NAME "alpha_recorder_frontend"
            FOLDER "alpha_recorder/obs"
        )

        add_custom_command(TARGET alpha_recorder_frontend POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_ROOT}/obs-plugins/64bit"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:alpha_recorder_frontend>"
                    "${OBS_ROOT}/obs-plugins/64bit/$<TARGET_FILE_NAME:alpha_recorder_frontend>"
            VERBATIM
        )
    endif()
endif()

if(BUILD_TESTING AND ALPHA_RECORDER_BUILD_E2E_TESTS)
    add_library(alpha_recorder_e2e_output MODULE
        src/obs/plugin.cpp
        src/obs/obs_output_bridge.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_e2e_output)
    alpha_recorder_apply_warnings(alpha_recorder_e2e_output)
    target_compile_definitions(alpha_recorder_e2e_output PRIVATE ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
    target_link_libraries(alpha_recorder_e2e_output PRIVATE alpha_recorder_core OBS::libobs)
    target_include_directories(alpha_recorder_e2e_output PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    set_target_properties(alpha_recorder_e2e_output PROPERTIES
        OUTPUT_NAME "alpha_recorder_e2e"
        FOLDER "alpha_recorder/tests/e2e"
    )

    add_custom_command(TARGET alpha_recorder_e2e_output POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_ROOT}/obs-plugins/64bit"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:alpha_recorder_e2e_output>"
                "${OBS_ROOT}/obs-plugins/64bit/$<TARGET_FILE_NAME:alpha_recorder_e2e_output>"
        VERBATIM
    )
endif()

if(BUILD_TESTING AND ALPHA_RECORDER_BUILD_UNIT_TESTS)
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

    add_executable(alpha_recorder_unit_e2e_scenario
        tests/unit/e2e_scenario_test.cpp
    )
    alpha_recorder_setup_target(alpha_recorder_unit_e2e_scenario)
    alpha_recorder_apply_warnings(alpha_recorder_unit_e2e_scenario)
    target_link_libraries(alpha_recorder_unit_e2e_scenario PRIVATE alpha_recorder_core)
    set_target_properties(alpha_recorder_unit_e2e_scenario PROPERTIES
        FOLDER "alpha_recorder/tests/unit"
    )
    add_test(NAME alpha_recorder.unit.e2e_scenario COMMAND alpha_recorder_unit_e2e_scenario)
    set_tests_properties(alpha_recorder.unit.e2e_scenario PROPERTIES LABELS "unit")

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
    set_tests_properties(alpha_recorder.unit.settings PROPERTIES LABELS "unit")
endif()

if(BUILD_TESTING AND ALPHA_RECORDER_BUILD_E2E_TESTS)
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
            ENVIRONMENT_MODIFICATION "ALPHA_RECORDER_STAGE_DIR=set:${OBS_ROOT};PATH=path_list_prepend:${alpha_recorder_obs_runtime_bin_dir}"
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
endif()

install(TARGETS alpha_recorder_core
    ARCHIVE DESTINATION lib
)

if(TARGET alpha_recorder_plugin)
    install(TARGETS alpha_recorder_plugin
        RUNTIME DESTINATION obs-plugins/64bit
        LIBRARY DESTINATION obs-plugins/64bit
    )
endif()

if(TARGET alpha_recorder_frontend)
    install(TARGETS alpha_recorder_frontend
        RUNTIME DESTINATION obs-plugins/64bit
        LIBRARY DESTINATION obs-plugins/64bit
    )
endif()

install(DIRECTORY include/ DESTINATION include)