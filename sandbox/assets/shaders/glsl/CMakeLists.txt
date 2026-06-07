# GLSL → SPIR-V compilation
# ==============================================
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(GLSLC)
    file(GLOB SHADER_SOURCES *.vert *.frag)

    foreach(shader ${SHADER_SOURCES})
        set(spv_output "${shader}.spv")
        add_custom_command(
            OUTPUT ${spv_output}
            COMMAND ${GLSLC} "${shader}" -o ${spv_output}
            DEPENDS "${shader}"
            COMMENT "Compiling ${shader} → SPIR-V"
        )
        list(APPEND SPV_OUTPUTS ${spv_output})
    endforeach()

    if(SPV_OUTPUTS)
        add_custom_target(ShaderCompile ALL DEPENDS ${SPV_OUTPUTS})
        add_dependencies(${Sandbox_NAME} ShaderCompile)
    endif()
else()
    message(WARNING "glslc not found — shaders won't be auto-compiled")
endif()
