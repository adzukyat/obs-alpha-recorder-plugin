include_guard(GLOBAL)

include(FetchContent)

option(ALPHA_RECORDER_FETCH_GTEST "Fetch GoogleTest for future unit-test expansion" OFF)

function(alpha_recorder_fetch_test_deps)
    if(NOT ALPHA_RECORDER_FETCH_GTEST)
        return()
    endif()

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endfunction()