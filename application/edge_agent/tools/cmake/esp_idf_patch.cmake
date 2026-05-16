set(EDGE_AGENT_PROJECT_LOG_PREFIX "[edge_agent]")
set(EDGE_AGENT_ESP_IDF_PATCH "${CMAKE_SOURCE_DIR}/tools/esp-idf.patch")

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} IDF_PATH environment variable is not set")
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

if(EXISTS "${EDGE_AGENT_ESP_IDF_PATCH}")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${EDGE_AGENT_ESP_IDF_PATCH}"
        WORKING_DIRECTORY "$ENV{IDF_PATH}"
        RESULT_VARIABLE EDGE_AGENT_PATCH_ALREADY_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(EDGE_AGENT_PATCH_ALREADY_APPLIED EQUAL 0)
        message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch already applied: ${EDGE_AGENT_ESP_IDF_PATCH}")
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply "${EDGE_AGENT_ESP_IDF_PATCH}"
            WORKING_DIRECTORY "$ENV{IDF_PATH}"
            RESULT_VARIABLE EDGE_AGENT_PATCH_APPLY_RESULT
            OUTPUT_VARIABLE EDGE_AGENT_PATCH_APPLY_STDOUT
            ERROR_VARIABLE EDGE_AGENT_PATCH_APPLY_STDERR
        )

        if(NOT EDGE_AGENT_PATCH_APPLY_RESULT EQUAL 0)
            message(FATAL_ERROR
                "${EDGE_AGENT_PROJECT_LOG_PREFIX} Failed to apply ESP-IDF patch: ${EDGE_AGENT_ESP_IDF_PATCH}\n"
                "stdout:\n${EDGE_AGENT_PATCH_APPLY_STDOUT}\n"
                "stderr:\n${EDGE_AGENT_PATCH_APPLY_STDERR}")
        endif()

        message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} Applied ESP-IDF patch: ${EDGE_AGENT_ESP_IDF_PATCH}")
    endif()
else()
    message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch file not found: ${EDGE_AGENT_ESP_IDF_PATCH}")
endif()
