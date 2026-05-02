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

function(alpha_recorder_validate_obs_runtime obs_root)
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
    else()
        set(bin_dir "${obs_root}/bin/64bit")
        set(plugin_dir "${obs_root}/obs-plugins/64bit")
        set(data_dir "${obs_root}/data")
        alpha_recorder_any_exists(has_runtime
            "${obs_root}/bin/64bit/obs.dll"
            "${obs_root}/bin/64bit/libobs.dll"
            "${obs_root}/bin/obs.dll"
            "${obs_root}/bin/libobs.dll"
        )
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
        )
    else()
        alpha_recorder_any_exists(has_library
            "${build_dir}/libobs/${configuration}/obs.lib"
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
