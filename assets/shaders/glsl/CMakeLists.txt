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
        # GE_Editor 依赖编译出的着色器。
        # 但 Android 下 GE_Editor 不存在（只移 GE_Runtime，见仓库根 CMakeLists 的
        # if(NOT ANDROID) 分区），直接 add_dependencies 会报「non-existent target」。
        # 用 TARGET 判断按需挂 —— 着色器本身仍由上面的 ALL 目标编译。
        if(TARGET ${Editor_NAME})
            add_dependencies(${Editor_NAME} ShaderCompile)
        endif()
    endif()
else()
    message(WARNING "glslc not found — shaders won't be auto-compiled")
endif()
