cmake_minimum_required(VERSION 3.24)

function(alpha_recorder_require var_name)
    if(NOT DEFINED ${var_name} OR "${${var_name}}" STREQUAL "")
        message(FATAL_ERROR "${var_name} is required")
    endif()
endfunction()

alpha_recorder_require(REPO_ROOT)
alpha_recorder_require(BUILD_DIR)
alpha_recorder_require(PACKAGE_DIR)

set(CONFIGURATION "RelWithDebInfo" CACHE STRING "Build configuration")
set(PLUGIN_NAME "alpha_recorder" CACHE STRING "Main plugin output name")

cmake_path(ABSOLUTE_PATH REPO_ROOT NORMALIZE OUTPUT_VARIABLE REPO_ROOT)
cmake_path(ABSOLUTE_PATH BUILD_DIR BASE_DIRECTORY "${REPO_ROOT}" NORMALIZE OUTPUT_VARIABLE BUILD_DIR)
cmake_path(ABSOLUTE_PATH PACKAGE_DIR BASE_DIRECTORY "${REPO_ROOT}" NORMALIZE OUTPUT_VARIABLE PACKAGE_DIR)

function(alpha_recorder_find_plugin out_var plugin_name)
    set(candidate_roots
        "${BUILD_DIR}/bin/${CONFIGURATION}"
        "${BUILD_DIR}/${CONFIGURATION}"
        "${BUILD_DIR}/bin"
        "${BUILD_DIR}"
    )

    if(APPLE)
        set(extensions ".plugin" ".dylib")
    elseif(WIN32)
        set(extensions ".dll")
    else()
        set(extensions ".so")
    endif()

    foreach(root IN LISTS candidate_roots)
        if(NOT EXISTS "${root}")
            continue()
        endif()
        foreach(ext IN LISTS extensions)
            set(candidate "${root}/${plugin_name}${ext}")
            if(EXISTS "${candidate}")
                cmake_path(ABSOLUTE_PATH candidate NORMALIZE OUTPUT_VARIABLE resolved)
                set(${out_var} "${resolved}" PARENT_SCOPE)
                return()
            endif()
            if(ext STREQUAL ".so")
                set(candidate "${root}/lib${plugin_name}${ext}")
                if(EXISTS "${candidate}")
                    cmake_path(ABSOLUTE_PATH candidate NORMALIZE OUTPUT_VARIABLE resolved)
                    set(${out_var} "${resolved}" PARENT_SCOPE)
                    return()
                endif()
            endif()
        endforeach()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

alpha_recorder_find_plugin(plugin_path "${PLUGIN_NAME}")
if(plugin_path STREQUAL "")
    message(FATAL_ERROR "Failed to locate ${PLUGIN_NAME} in the build tree: ${BUILD_DIR}")
endif()

file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}")

if(WIN32)
    set(plugin_target_dir "${PACKAGE_DIR}/obs-plugins/64bit")
elseif(APPLE)
    set(plugin_target_dir "${PACKAGE_DIR}/obs-plugins")
else()
    include(GNUInstallDirs)
    set(plugin_target_dir "${PACKAGE_DIR}/${CMAKE_INSTALL_LIBDIR}/obs-plugins")
endif()
file(MAKE_DIRECTORY "${plugin_target_dir}")

if(IS_DIRECTORY "${plugin_path}")
    file(COPY "${plugin_path}" DESTINATION "${plugin_target_dir}")
else()
    get_filename_component(plugin_file_name "${plugin_path}" NAME)
    file(COPY_FILE "${plugin_path}" "${plugin_target_dir}/${plugin_file_name}" ONLY_IF_DIFFERENT)
endif()

foreach(doc_file IN ITEMS README.md LICENSE VERSION)
    if(EXISTS "${REPO_ROOT}/${doc_file}")
        file(COPY_FILE "${REPO_ROOT}/${doc_file}" "${PACKAGE_DIR}/${doc_file}" ONLY_IF_DIFFERENT)
    endif()
endforeach()

message(STATUS "Packaged ${PLUGIN_NAME} from ${plugin_path}")
message(STATUS "Package root: ${PACKAGE_DIR}")
