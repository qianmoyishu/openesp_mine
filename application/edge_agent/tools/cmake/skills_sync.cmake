function(edge_agent_collect_project_component_dirs out_var)
    set(options INCLUDE_MAIN_DIR)
    cmake_parse_arguments(arg "${options}" "" "" ${ARGN})

    idf_build_get_property(build_components BUILD_COMPONENTS)

    get_filename_component(project_root_dir "${CMAKE_SOURCE_DIR}" REALPATH)
    get_filename_component(shared_components_root "${CMAKE_SOURCE_DIR}/../../components" REALPATH)
    get_filename_component(managed_components_root "${CMAKE_SOURCE_DIR}/managed_components" REALPATH)

    set(component_specs)
    foreach(component_name IN LISTS build_components)
        idf_component_get_property(component_dir "${component_name}" COMPONENT_DIR)
        if(NOT component_dir)
            continue()
        endif()

        get_filename_component(component_dir "${component_dir}" REALPATH)

        if(component_dir MATCHES "^${managed_components_root}(/|$)")
            continue()
        endif()

        if(component_dir MATCHES "^${project_root_dir}(/|$)" OR
           component_dir MATCHES "^${shared_components_root}(/|$)")
            if(arg_INCLUDE_MAIN_DIR AND component_name STREQUAL "main")
                continue()
            endif()
            list(APPEND component_specs "${component_name}=${component_dir}")
        endif()
    endforeach()

    if(arg_INCLUDE_MAIN_DIR)
        get_filename_component(main_dir "${CMAKE_SOURCE_DIR}/main" REALPATH)
        list(APPEND component_specs "__main__=${main_dir}")
    endif()

    set(${out_var} "${component_specs}" PARENT_SCOPE)
endfunction()

function(edge_agent_collect_component_resource_files out_var resource_subdir)
    set(options INCLUDE_MAIN_DIR)
    set(multi_value_args GLOB_PATTERNS)
    cmake_parse_arguments(arg "${options}" "" "${multi_value_args}" ${ARGN})

    if(NOT arg_GLOB_PATTERNS)
        message(FATAL_ERROR "edge_agent_collect_component_resource_files requires GLOB_PATTERNS")
    endif()

    edge_agent_collect_project_component_dirs(component_specs ${ARGN})

    set(resource_files)
    foreach(component_spec IN LISTS component_specs)
        string(FIND "${component_spec}" "=" separator_index)
        if(separator_index EQUAL -1)
            message(FATAL_ERROR "Invalid component spec '${component_spec}'")
        endif()

        math(EXPR path_index "${separator_index} + 1")
        string(SUBSTRING "${component_spec}" ${path_index} -1 component_dir)
        set(resource_dir "${component_dir}/${resource_subdir}")

        if(NOT EXISTS "${resource_dir}")
            continue()
        endif()

        foreach(glob_pattern IN LISTS arg_GLOB_PATTERNS)
            file(
                GLOB component_resource_files
                CONFIGURE_DEPENDS
                LIST_DIRECTORIES false
                "${resource_dir}/${glob_pattern}"
            )
            list(APPEND resource_files ${component_resource_files})
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES resource_files)
    set(${out_var} "${resource_files}" PARENT_SCOPE)
endfunction()

function(edge_agent_append_component_args out_var)
    set(options INCLUDE_MAIN_DIR)
    set(one_value_args MAIN_DIR_ARG)
    cmake_parse_arguments(arg "${options}" "${one_value_args}" "" ${ARGN})

    edge_agent_collect_project_component_dirs(component_specs ${ARGN})

    set(component_args)
    foreach(component_spec IN LISTS component_specs)
        string(FIND "${component_spec}" "=" separator_index)
        if(separator_index EQUAL -1)
            message(FATAL_ERROR "Invalid component spec '${component_spec}'")
        endif()

        string(SUBSTRING "${component_spec}" 0 ${separator_index} component_name)
        math(EXPR path_index "${separator_index} + 1")
        string(SUBSTRING "${component_spec}" ${path_index} -1 component_dir)

        if(component_name STREQUAL "__main__")
            if(arg_MAIN_DIR_ARG)
                list(APPEND component_args "${arg_MAIN_DIR_ARG}" "${component_dir}")
            endif()
        else()
            list(APPEND component_args --component "${component_name}=${component_dir}")
        endif()
    endforeach()

    set(${out_var} "${component_args}" PARENT_SCOPE)
endfunction()

function(edge_agent_get_resource_generator_targets out_var resource_kind)
    set(property_names "APP_RESOURCE_GENERATOR_TARGETS_${resource_kind}")
    if(resource_kind STREQUAL "skills")
        list(APPEND property_names "APP_SKILL_GENERATOR_TARGETS")
    endif()

    set(generator_targets)
    foreach(property_name IN LISTS property_names)
        get_property(property_set GLOBAL PROPERTY "${property_name}" SET)
        if(property_set)
            get_property(property_targets GLOBAL PROPERTY "${property_name}")
            if(property_targets)
                list(APPEND generator_targets ${property_targets})
            endif()
        endif()
    endforeach()

    list(REMOVE_DUPLICATES generator_targets)
    set(${out_var} "${generator_targets}" PARENT_SCOPE)
endfunction()

function(edge_agent_enable_component_resource_sync)
    set(options INCLUDE_MAIN_DIR)
    set(one_value_args
        TARGET
        RESOURCE_KIND
        RESOURCE_SUBDIR
        OUTPUT_DIR
        OUTPUT_ARG_NAME
        MANIFEST_PATH
        PYTHON_SCRIPT
        STAMP_PATH
        MAIN_DIR_ARG
        COMMENT
    )
    set(multi_value_args GLOB_PATTERNS SCRIPT_ARGS)
    cmake_parse_arguments(arg "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    foreach(required_arg TARGET RESOURCE_KIND RESOURCE_SUBDIR OUTPUT_DIR OUTPUT_ARG_NAME MANIFEST_PATH PYTHON_SCRIPT STAMP_PATH COMMENT)
        if(NOT arg_${required_arg})
            message(FATAL_ERROR "edge_agent_enable_component_resource_sync requires ${required_arg}")
        endif()
    endforeach()

    if(NOT arg_GLOB_PATTERNS)
        message(FATAL_ERROR "edge_agent_enable_component_resource_sync requires GLOB_PATTERNS")
    endif()

    idf_build_get_property(python PYTHON)
    edge_agent_collect_component_resource_files(
        resource_dependency_files
        "${arg_RESOURCE_SUBDIR}"
        ${ARGN}
    )
    edge_agent_append_component_args(component_args ${ARGN})

    add_custom_command(
        OUTPUT "${arg_STAMP_PATH}"
        COMMAND ${python}
                "${arg_PYTHON_SCRIPT}"
                --manifest-path "${arg_MANIFEST_PATH}"
                "${arg_OUTPUT_ARG_NAME}" "${arg_OUTPUT_DIR}"
                ${arg_SCRIPT_ARGS}
                ${component_args}
        COMMAND ${CMAKE_COMMAND} -E touch "${arg_STAMP_PATH}"
        DEPENDS
                "${arg_PYTHON_SCRIPT}"
                ${resource_dependency_files}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "${arg_COMMENT}"
        VERBATIM
    )

    set(sync_target "app_sync_${arg_RESOURCE_KIND}")
    add_custom_target(${sync_target} ALL DEPENDS "${arg_STAMP_PATH}")

    edge_agent_get_resource_generator_targets(generator_targets "${arg_RESOURCE_KIND}")
    if(generator_targets)
        add_dependencies(${sync_target} ${generator_targets})
    endif()

    add_dependencies(${arg_TARGET} ${sync_target})
endfunction()

function(edge_agent_enable_component_skills_sync)
    set(options)
    set(one_value_args TARGET DEMO_SKILLS_DIR MANIFEST_PATH)
    cmake_parse_arguments(arg "${options}" "${one_value_args}" "" ${ARGN})

    if(NOT arg_TARGET)
        message(FATAL_ERROR "edge_agent_enable_component_skills_sync requires TARGET")
    endif()
    if(NOT arg_DEMO_SKILLS_DIR)
        message(FATAL_ERROR "edge_agent_enable_component_skills_sync requires DEMO_SKILLS_DIR")
    endif()
    if(NOT arg_MANIFEST_PATH)
        message(FATAL_ERROR "edge_agent_enable_component_skills_sync requires MANIFEST_PATH")
    endif()

    set(skill_sync_extra_args --demo-skills-dir "${arg_DEMO_SKILLS_DIR}")
    if(CONFIG_APP_CLAW_MEMORY_MODE_LIGHTWEIGHT)
        list(APPEND skill_sync_extra_args --exclude-skill-id memory_ops)
    endif()

    edge_agent_enable_component_resource_sync(
        TARGET "${arg_TARGET}"
        RESOURCE_KIND skills
        RESOURCE_SUBDIR skills
        OUTPUT_DIR "${arg_DEMO_SKILLS_DIR}"
        OUTPUT_ARG_NAME --demo-skills-dir
        MANIFEST_PATH "${arg_MANIFEST_PATH}"
        PYTHON_SCRIPT "${CMAKE_SOURCE_DIR}/tools/sync_component_skills.py"
        STAMP_PATH "${CMAKE_BINARY_DIR}/component_skills_sync.stamp"
        COMMENT "Sync component skills into fatfs_image/skills"
        GLOB_PATTERNS "*"
        SCRIPT_ARGS ${skill_sync_extra_args}
    )
endfunction()

function(edge_agent_enable_component_builtin_lua_sync)
    set(options)
    set(one_value_args TARGET OUTPUT_DIR MANIFEST_PATH)
    cmake_parse_arguments(arg "${options}" "${one_value_args}" "" ${ARGN})

    if(NOT arg_TARGET)
        message(FATAL_ERROR "edge_agent_enable_component_builtin_lua_sync requires TARGET")
    endif()
    if(NOT arg_OUTPUT_DIR)
        message(FATAL_ERROR "edge_agent_enable_component_builtin_lua_sync requires OUTPUT_DIR")
    endif()
    if(NOT arg_MANIFEST_PATH)
        message(FATAL_ERROR "edge_agent_enable_component_builtin_lua_sync requires MANIFEST_PATH")
    endif()

    edge_agent_enable_component_resource_sync(
        TARGET "${arg_TARGET}"
        RESOURCE_KIND builtin_lua
        RESOURCE_SUBDIR lua_scripts
        OUTPUT_DIR "${arg_OUTPUT_DIR}"
        OUTPUT_ARG_NAME --output-dir
        MANIFEST_PATH "${arg_MANIFEST_PATH}"
        PYTHON_SCRIPT "${CMAKE_SOURCE_DIR}/tools/sync_component_lua_scripts.py"
        STAMP_PATH "${CMAKE_BINARY_DIR}/component_builtin_lua_sync.stamp"
        MAIN_DIR_ARG --main-dir
        INCLUDE_MAIN_DIR
        COMMENT "Sync component builtin Lua scripts into fatfs_image/scripts/builtin"
        GLOB_PATTERNS "*.lua"
    )
endfunction()
