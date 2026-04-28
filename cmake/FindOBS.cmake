include_guard(GLOBAL)

include(FindPackageHandleStandardArgs)

set(_alpha_recorder_obs_root_candidates
    "${OBS_ROOT}"
    "$ENV{OBS_ROOT}"
    "$ENV{OBS_INSTALL_DIR}"
    "$ENV{OBS_DIR}"
)

set(_alpha_recorder_obs_build_root_candidates)
set(_alpha_recorder_obs_frontend_source_dir "${CMAKE_CURRENT_LIST_DIR}/../deps/obs/obs-studio/frontend/api")

set(_alpha_recorder_obs_hint_paths)
foreach(_candidate IN LISTS _alpha_recorder_obs_root_candidates)
    if(NOT "${_candidate}" STREQUAL "")
        get_filename_component(_alpha_recorder_obs_candidate_abs "${_candidate}" ABSOLUTE)
        list(APPEND _alpha_recorder_obs_hint_paths "${_alpha_recorder_obs_candidate_abs}")

        get_filename_component(_alpha_recorder_obs_candidate_build_root "${_alpha_recorder_obs_candidate_abs}" DIRECTORY)
        get_filename_component(_alpha_recorder_obs_candidate_build_root "${_alpha_recorder_obs_candidate_build_root}" DIRECTORY)
        list(APPEND _alpha_recorder_obs_build_root_candidates "${_alpha_recorder_obs_candidate_build_root}")

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
list(REMOVE_DUPLICATES _alpha_recorder_obs_build_root_candidates)

find_path(OBS_INCLUDE_DIR
    NAMES obs-module.h obs.h
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES include libobs/include libobs libobs/libobs
)

find_path(OBS_CONFIG_INCLUDE_DIR
    NAMES obsconfig.h obs-config.h
    HINTS ${_alpha_recorder_obs_hint_paths}
    PATH_SUFFIXES include libobs/include libobs libobs/libobs config build/config
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

find_path(OBS_FRONTEND_API_INCLUDE_DIR
    NAMES obs-frontend-api.h
    HINTS "${_alpha_recorder_obs_frontend_source_dir}"
)

find_library(OBS_FRONTEND_API_LIBRARY
    NAMES obs-frontend-api
    HINTS ${_alpha_recorder_obs_build_root_candidates}
    PATH_SUFFIXES frontend/api/RelWithDebInfo frontend/api/Debug frontend/api/Release frontend/api
)

find_file(OBS_FRONTEND_API_DLL
    NAMES obs-frontend-api.dll
    HINTS ${_alpha_recorder_obs_hint_paths} ${_alpha_recorder_obs_build_root_candidates}
    PATH_SUFFIXES bin/64bit bin frontend/api/RelWithDebInfo frontend/api/Debug frontend/api/Release frontend/api
)

if(OBS_INCLUDE_DIR AND OBS_LIBOBS_LIBRARY)
    if(NOT TARGET OBS::libobs)
        add_library(OBS::libobs SHARED IMPORTED GLOBAL)
    endif()

    set(_alpha_recorder_obs_include_dirs "${OBS_INCLUDE_DIR}")
    if(OBS_CONFIG_INCLUDE_DIR)
        list(APPEND _alpha_recorder_obs_include_dirs "${OBS_CONFIG_INCLUDE_DIR}")
    endif()

    if(WIN32)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_IMPLIB "${OBS_LIBOBS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_alpha_recorder_obs_include_dirs}"
        )

        if(OBS_LIBOBS_DLL)
            set_target_properties(OBS::libobs PROPERTIES
                IMPORTED_LOCATION "${OBS_LIBOBS_DLL}"
            )
        endif()
    else()
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION "${OBS_LIBOBS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_alpha_recorder_obs_include_dirs}"
        )
    endif()
endif()

if(OBS_FRONTEND_API_INCLUDE_DIR AND OBS_FRONTEND_API_LIBRARY)
    if(NOT TARGET OBS::frontend-api)
        add_library(OBS::frontend-api SHARED IMPORTED GLOBAL)
    endif()
    
    set(_alpha_recorder_obs_frontend_include_dirs "${OBS_FRONTEND_API_INCLUDE_DIR}")

    if(WIN32)
        set_target_properties(OBS::frontend-api PROPERTIES
            IMPORTED_IMPLIB "${OBS_FRONTEND_API_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_alpha_recorder_obs_frontend_include_dirs}"
        )

        if(OBS_FRONTEND_API_DLL)
            set_target_properties(OBS::frontend-api PROPERTIES
                IMPORTED_LOCATION "${OBS_FRONTEND_API_DLL}"
            )
        endif()
    else()
        set_target_properties(OBS::frontend-api PROPERTIES
            IMPORTED_LOCATION "${OBS_FRONTEND_API_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_alpha_recorder_obs_frontend_include_dirs}"
        )
    endif()
endif()

find_package_handle_standard_args(OBS
    REQUIRED_VARS OBS_INCLUDE_DIR OBS_CONFIG_INCLUDE_DIR OBS_LIBOBS_LIBRARY
)

mark_as_advanced(OBS_INCLUDE_DIR OBS_CONFIG_INCLUDE_DIR OBS_LIBOBS_LIBRARY OBS_LIBOBS_DLL OBS_FRONTEND_API_INCLUDE_DIR OBS_FRONTEND_API_LIBRARY OBS_FRONTEND_API_DLL)