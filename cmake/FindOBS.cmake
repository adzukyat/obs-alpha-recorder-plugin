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
        get_filename_component(_alpha_recorder_obs_candidate_abs "${_candidate}" ABSOLUTE)
        list(APPEND _alpha_recorder_obs_hint_paths "${_alpha_recorder_obs_candidate_abs}")

        get_filename_component(_alpha_recorder_obs_parent_dir "${_alpha_recorder_obs_candidate_abs}" DIRECTORY)
        foreach(_alpha_recorder_obs_sibling IN ITEMS obs-studio obs-build obs-install)
            set(_alpha_recorder_obs_sibling_path "${_alpha_recorder_obs_parent_dir}/${_alpha_recorder_obs_sibling}")
            if(EXISTS "${_alpha_recorder_obs_sibling_path}")
                list(APPEND _alpha_recorder_obs_hint_paths "${_alpha_recorder_obs_sibling_path}")
            endif()
        endforeach()

        set(_alpha_recorder_obs_runtime_root "${_alpha_recorder_obs_candidate_abs}/rundir/RelWithDebInfo")
        if(EXISTS "${_alpha_recorder_obs_runtime_root}")
            list(APPEND _alpha_recorder_obs_hint_paths "${_alpha_recorder_obs_runtime_root}")
        endif()
    endif()
endforeach()

list(REMOVE_DUPLICATES _alpha_recorder_obs_hint_paths)

find_path(OBS_INCLUDE_DIR
    NAMES obs-module.h obs.h
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES include libobs/include libobs
)

find_path(OBS_CONFIG_INCLUDE_DIR
    NAMES obsconfig.h
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES config build/config
)

find_library(OBS_LIBOBS_LIBRARY
    NAMES libobs obs
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES lib bin/64bit bin libobs/lib libobs/RelWithDebInfo build/libobs/Debug build/libobs/Release build/libobs/RelWithDebInfo
)

find_file(OBS_LIBOBS_DLL
    NAMES libobs.dll obs.dll
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES bin/64bit bin lib rundir/RelWithDebInfo/bin/64bit rundir/RelWithDebInfo/bin
)

if(OBS_INCLUDE_DIR AND OBS_LIBOBS_LIBRARY)
    if(NOT TARGET OBS::libobs)
        add_library(OBS::libobs SHARED IMPORTED GLOBAL)
    endif()

    set(_alpha_recorder_obs_include_dirs "${OBS_INCLUDE_DIR}")
    if(OBS_CONFIG_INCLUDE_DIR)
        list(APPEND _alpha_recorder_obs_include_dirs "${OBS_CONFIG_INCLUDE_DIR}")
    endif()

    set_target_properties(OBS::libobs PROPERTIES
        IMPORTED_IMPLIB "${OBS_LIBOBS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${_alpha_recorder_obs_include_dirs}"
    )

    if(OBS_LIBOBS_DLL)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION "${OBS_LIBOBS_DLL}"
        )
    endif()
endif()

find_package_handle_standard_args(OBS
    REQUIRED_VARS OBS_INCLUDE_DIR OBS_CONFIG_INCLUDE_DIR OBS_LIBOBS_LIBRARY
)

mark_as_advanced(OBS_INCLUDE_DIR OBS_CONFIG_INCLUDE_DIR OBS_LIBOBS_LIBRARY OBS_LIBOBS_DLL)