idf_component_register()

set(CAP_BOARDS_SKILLS_DIR "${COMPONENT_DIR}/skills")
set(CAP_BOARDS_GENERATOR "${COMPONENT_DIR}/tools/generate_board_skill.py")
set(CAP_BOARDS_GEN_BMGR_DIR "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes")
set(CAP_BOARDS_OUTPUT_MD "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info.md")

idf_build_get_property(python PYTHON)

add_custom_command(
    OUTPUT "${CAP_BOARDS_OUTPUT_MD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CAP_BOARDS_SKILLS_DIR}"
    COMMAND ${python} "${CAP_BOARDS_GENERATOR}"
            --gen-bmgr-dir "${CAP_BOARDS_GEN_BMGR_DIR}"
            --output-md "${CAP_BOARDS_OUTPUT_MD}"
    DEPENDS
        "${CAP_BOARDS_GENERATOR}"
        "${CAP_BOARDS_GEN_BMGR_DIR}/CMakeLists.txt"
        "${CAP_BOARDS_GEN_BMGR_DIR}/gen_board_info.c"
        "${CAP_BOARDS_GEN_BMGR_DIR}/gen_board_device_config.c"
        "${CAP_BOARDS_GEN_BMGR_DIR}/gen_board_periph_config.c"
    COMMENT "Generating cap_boards skill from gen_bmgr_codes"
    VERBATIM
)

add_custom_target(cap_boards_generate_skill DEPENDS "${CAP_BOARDS_OUTPUT_MD}")
add_dependencies(${COMPONENT_LIB} cap_boards_generate_skill)
set_property(GLOBAL APPEND PROPERTY APP_SKILL_GENERATOR_TARGETS cap_boards_generate_skill)
set_property(GLOBAL APPEND PROPERTY APP_RESOURCE_GENERATOR_TARGETS_skills cap_boards_generate_skill)
