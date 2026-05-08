include_guard(GLOBAL)

function(alpha_recorder_require variable_name)
    if(NOT DEFINED ${variable_name} OR "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR "${variable_name} is required")
    endif()
endfunction()

function(alpha_recorder_abs_path out_var path base_path)
    if(IS_ABSOLUTE "${path}")
        cmake_path(ABSOLUTE_PATH path NORMALIZE OUTPUT_VARIABLE resolved)
    else()
        cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${base_path}" NORMALIZE OUTPUT_VARIABLE resolved)
    endif()
    set(${out_var} "${resolved}" PARENT_SCOPE)
endfunction()

function(alpha_recorder_read_obs_tag out_var version_file)
    if(NOT EXISTS "${version_file}")
        message(FATAL_ERROR "OBS version manifest not found: ${version_file}")
    endif()

    file(STRINGS "${version_file}" tag_line REGEX "^tag=" LIMIT_COUNT 1)
    if(NOT tag_line)
        message(FATAL_ERROR "OBS version manifest does not define a tag: ${version_file}")
    endif()

    string(REGEX REPLACE "^tag=" "" tag "${tag_line}")
    string(STRIP "${tag}" tag)
    set(${out_var} "${tag}" PARENT_SCOPE)
endfunction()

function(alpha_recorder_any_exists out_var)
    set(found OFF)
    foreach(candidate IN LISTS ARGN)
        if(EXISTS "${candidate}")
            set(found ON)
            break()
        endif()
    endforeach()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

function(alpha_recorder_resolve_obs_runtime_root out_var obs_root)
    set(resolved "${obs_root}")
    if(APPLE)
        if(EXISTS "${resolved}/OBS.app/Contents")
            set(resolved "${resolved}/OBS.app/Contents")
        elseif(EXISTS "${resolved}/Contents")
            set(resolved "${resolved}/Contents")
        endif()
    endif()
    cmake_path(ABSOLUTE_PATH resolved NORMALIZE OUTPUT_VARIABLE resolved)
    set(${out_var} "${resolved}" PARENT_SCOPE)
endfunction()

function(alpha_recorder_linux_library_dirs out_var obs_root)
    set(candidates)
    if(CMAKE_LIBRARY_ARCHITECTURE)
        list(APPEND candidates "${obs_root}/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    endif()
    foreach(arch IN ITEMS x86_64-linux-gnu aarch64-linux-gnu arm-linux-gnueabihf)
        list(APPEND candidates "${obs_root}/lib/${arch}")
    endforeach()
    list(APPEND candidates "${obs_root}/lib")

    set(found)
    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}")
            list(APPEND found "${candidate}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES found)
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

function(alpha_recorder_first_existing out_var)
    foreach(candidate IN LISTS ARGN)
        if(EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(alpha_recorder_linux_runtime_layout obs_root out_bin_dir out_plugin_dir out_data_dir out_lib_dirs)
    set(bin_dir "${obs_root}/bin")
    alpha_recorder_linux_library_dirs(lib_dirs "${obs_root}")

    set(plugin_candidates)
    foreach(lib_dir IN LISTS lib_dirs)
        list(APPEND plugin_candidates "${lib_dir}/obs-plugins")
    endforeach()
    list(APPEND plugin_candidates "${obs_root}/obs-plugins")
    alpha_recorder_first_existing(plugin_dir ${plugin_candidates})

    alpha_recorder_first_existing(data_dir
        "${obs_root}/share/obs/obs-studio"
        "${obs_root}/data"
    )

    set(${out_bin_dir} "${bin_dir}" PARENT_SCOPE)
    set(${out_plugin_dir} "${plugin_dir}" PARENT_SCOPE)
    set(${out_data_dir} "${data_dir}" PARENT_SCOPE)
    set(${out_lib_dirs} "${lib_dirs}" PARENT_SCOPE)
endfunction()

function(alpha_recorder_validate_obs_runtime obs_root)
    alpha_recorder_resolve_obs_runtime_root(obs_root "${obs_root}")

    if(APPLE)
        set(bin_dir "${obs_root}/bin")
        set(plugin_dir "${obs_root}/obs-plugins")
        alpha_recorder_any_exists(has_runtime
            "${obs_root}/bin/obs.dylib"
            "${obs_root}/bin/libobs.dylib"
            "${obs_root}/Frameworks/obs.dylib"
            "${obs_root}/Frameworks/libobs.dylib"
            "${obs_root}/Frameworks/libobs.framework"
        )
        if(EXISTS "${obs_root}/Frameworks")
            set(bin_dir "${obs_root}/Frameworks")
        endif()
        if(EXISTS "${obs_root}/PlugIns")
            set(plugin_dir "${obs_root}/PlugIns")
        endif()
        if(EXISTS "${obs_root}/Resources")
            set(data_dir "${obs_root}/Resources")
        else()
            set(data_dir "${obs_root}/data")
        endif()
    elseif(WIN32)
        set(bin_dir "${obs_root}/bin/64bit")
        set(plugin_dir "${obs_root}/obs-plugins/64bit")
        set(data_dir "${obs_root}/data")
        alpha_recorder_any_exists(has_runtime
            "${obs_root}/bin/64bit/obs.dll"
            "${obs_root}/bin/64bit/libobs.dll"
            "${obs_root}/bin/obs.dll"
            "${obs_root}/bin/libobs.dll"
        )
    else()
        alpha_recorder_linux_runtime_layout("${obs_root}" bin_dir plugin_dir data_dir lib_dirs)
        set(has_runtime OFF)
        foreach(lib_dir IN LISTS lib_dirs)
            alpha_recorder_any_exists(has_runtime_candidate
                "${lib_dir}/libobs.so"
                "${lib_dir}/libobs.so.0"
            )
            if(has_runtime_candidate)
                set(has_runtime ON)
                break()
            endif()
        endforeach()
    endif()

    if(NOT EXISTS "${bin_dir}" OR NOT EXISTS "${data_dir}" OR NOT EXISTS "${plugin_dir}" OR NOT has_runtime)
        message(FATAL_ERROR "OBS root does not look like a runtime OBS tree: ${obs_root}")
    endif()
endfunction()

function(alpha_recorder_validate_obs_developer source_dir build_dir configuration)
    if(APPLE)
        alpha_recorder_any_exists(has_library
            "${build_dir}/libobs/libobs.dylib"
            "${build_dir}/libobs/${configuration}/libobs.dylib"
            "${build_dir}/libobs/libobs.framework"
            "${build_dir}/libobs/${configuration}/libobs.framework"
        )
    elseif(WIN32)
        alpha_recorder_any_exists(has_library
            "${build_dir}/libobs/${configuration}/obs.lib"
        )
    else()
        alpha_recorder_any_exists(has_library
            "${build_dir}/libobs/libobs.so"
            "${build_dir}/libobs/libobs.so.0"
            "${build_dir}/libobs/${configuration}/libobs.so"
            "${build_dir}/libobs/${configuration}/libobs.so.0"
        )
    endif()

    alpha_recorder_any_exists(has_header
        "${source_dir}/libobs/obs-module.h"
        "${source_dir}/libobs/obs.h"
        "${source_dir}/libobs/obs-config.h"
    )
    alpha_recorder_any_exists(has_config
        "${build_dir}/config/obsconfig.h"
    )

    if(NOT has_header OR NOT has_config OR NOT has_library)
        message(FATAL_ERROR "Pinned OBS source/build trees are missing developer files: source=${source_dir} build=${build_dir}")
    endif()
endfunction()

function(alpha_recorder_write_obs_root_config obs_root config_file)
    cmake_path(ABSOLUTE_PATH obs_root NORMALIZE OUTPUT_VARIABLE resolved_obs_root)
    cmake_path(GET config_file PARENT_PATH config_dir)
    file(MAKE_DIRECTORY "${config_dir}")
    file(WRITE "${config_file}" "set(OBS_ROOT \"${resolved_obs_root}\" CACHE PATH \"Path to a staged OBS developer tree\" FORCE)\n")
endfunction()

function(alpha_recorder_write_json_manifest output_path)
    cmake_path(GET output_path PARENT_PATH manifest_dir)
    file(MAKE_DIRECTORY "${manifest_dir}")
    string(TIMESTAMP timestamp "%Y-%m-%dT%H:%M:%SZ" UTC)
    set(json "{\n")
    set(first ON)
    foreach(entry IN LISTS ARGN)
        string(FIND "${entry}" "=" equals)
        if(equals LESS 1)
            message(FATAL_ERROR "Manifest entries must be key=value: ${entry}")
        endif()
        string(SUBSTRING "${entry}" 0 "${equals}" key)
        math(EXPR value_start "${equals} + 1")
        string(SUBSTRING "${entry}" "${value_start}" -1 value)
        string(REPLACE "\\" "\\\\" encoded_value "${value}")
        string(REPLACE "\"" "\\\"" encoded_value "${encoded_value}")
        if(first)
            set(first OFF)
        else()
            string(APPEND json ",\n")
        endif()
        string(APPEND json "  \"${key}\": \"${encoded_value}\"")
    endforeach()
    if(NOT first)
        string(APPEND json ",\n")
    endif()
    string(APPEND json "  \"timestamp\": \"${timestamp}\"\n}\n")
    file(WRITE "${output_path}" "${json}")
endfunction()
