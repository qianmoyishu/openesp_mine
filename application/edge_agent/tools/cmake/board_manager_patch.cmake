set(EDGE_AGENT_PROJECT_LOG_PREFIX "[edge_agent]")
set(EDGE_AGENT_BOARD_MANAGER_PATCH_SCRIPT "${CMAKE_SOURCE_DIR}/tools/bmgr_patch.py")

if(NOT EXISTS "${EDGE_AGENT_BOARD_MANAGER_PATCH_SCRIPT}")
    message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} Board manager patch script not found: ${EDGE_AGENT_BOARD_MANAGER_PATCH_SCRIPT}")
endif()

idf_build_get_property(python PYTHON)

add_custom_target(edge_agent_patch_managed_components ALL
    COMMAND ${python} "${EDGE_AGENT_BOARD_MANAGER_PATCH_SCRIPT}" --project-dir "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "${EDGE_AGENT_PROJECT_LOG_PREFIX} Patching managed component sources"
    VERBATIM
)
add_dependencies(app edge_agent_patch_managed_components)
