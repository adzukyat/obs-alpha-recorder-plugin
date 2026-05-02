include_guard(GLOBAL)

option(ALPHA_RECORDER_BUILD_OBS_PLUGIN "Build the OBS module wrapper" ON)
option(ALPHA_RECORDER_BUILD_UNIT_TESTS "Build the unit test targets" ON)
option(ALPHA_RECORDER_BUILD_CORE_UNIT_TESTS "Build unit tests that require the full core dependency stack" ON)
option(ALPHA_RECORDER_BUILD_E2E_TESTS "Build the E2E test targets" ON)
option(ALPHA_RECORDER_ENABLE_OBS_APP_E2E "Register the slow real OBS app E2E test" OFF)

function(alpha_recorder_set_project_options)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ language standard" FORCE)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    
    if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment version")
    endif()

    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Archive output directory" FORCE)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Library output directory" FORCE)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Runtime output directory" FORCE)
endfunction()

function(alpha_recorder_setup_target target)
    target_compile_features(${target} PUBLIC cxx_std_17)

    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()
