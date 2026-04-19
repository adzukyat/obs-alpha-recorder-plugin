include_guard(GLOBAL)

include(FindPackageHandleStandardArgs)

set(_alpha_recorder_obs_root_candidates
    "${OBS_ROOT}"
    "$ENV{OBS_ROOT}"
    "$ENV{OBS_INSTALL_DIR}"
    "$ENV{OBS_DIR}"
)

set(_alpha_recorder_obs_hint_paths)
foreach(_candidate IN LISTS _alpha_recorder_obs_root_candidates)
    if(NOT "${_candidate}" STREQUAL "")
        list(APPEND _alpha_recorder_obs_hint_paths "${_candidate}")
    endif()
endforeach()

list(REMOVE_DUPLICATES _alpha_recorder_obs_hint_paths)

find_path(OBS_INCLUDE_DIR
    NAMES obs-module.h obs.h
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES include libobs/include libobs
)

find_library(OBS_LIBOBS_LIBRARY
    NAMES libobs obs
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES lib bin/64bit bin libobs/lib build/libobs/Debug build/libobs/Release
)

find_file(OBS_LIBOBS_DLL
    NAMES libobs.dll obs.dll
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES bin/64bit bin lib
)

if(OBS_INCLUDE_DIR AND OBS_LIBOBS_LIBRARY)
    if(NOT TARGET OBS::libobs)
        add_library(OBS::libobs SHARED IMPORTED GLOBAL)
    endif()

    set_target_properties(OBS::libobs PROPERTIES
        IMPORTED_IMPLIB "${OBS_LIBOBS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
    )

    if(OBS_LIBOBS_DLL)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION "${OBS_LIBOBS_DLL}"
        )
    endif()
endif()

find_package_handle_standard_args(OBS
    REQUIRED_VARS OBS_INCLUDE_DIR OBS_LIBOBS_LIBRARY
)

mark_as_advanced(OBS_INCLUDE_DIR OBS_LIBOBS_LIBRARY OBS_LIBOBS_DLL)