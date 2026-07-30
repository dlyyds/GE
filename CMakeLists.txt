cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 20)

project(GE)


set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_BUILD_TYPE Release)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)


set(GE_NAME GE)
set(Sandbox_NAME Sandbox)
set(Editor_NAME GE_Editor)

if (MSVC)
    add_compile_options(/Zc:__cplusplus)
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/EHsc)
endif ()


add_subdirectory(GE/third_party/spdlog)
add_subdirectory(GE/third_party/glfw)
add_subdirectory(GE/third_party/yaml-cpp)
add_subdirectory(GE/third_party/imgui)
add_subdirectory(GE/third_party/ImGuizmo)

# Tracy 性能分析
add_subdirectory(GE/third_party/tracy)

find_package(Vulkan REQUIRED)


#GE lib
# ==============================================
file(GLOB GE_SRC
        ${CMAKE_SOURCE_DIR}/GE/src/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Platform/Windows/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Core/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/FileSystem/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/ImGui/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/VulkanBase/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Scene/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/stb/stb_image.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinyobjloader/tiny_obj_loader.cpp

)


add_library(${GE_NAME} STATIC ${GE_SRC})

target_include_directories(${GE_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/GE/include/GE
        ${CMAKE_SOURCE_DIR}/GE/third_party/glm
        ${CMAKE_SOURCE_DIR}/GE/third_party/entt
        ${CMAKE_SOURCE_DIR}/GE/third_party
        ${CMAKE_SOURCE_DIR}/GE/third_party/vma
        ${CMAKE_SOURCE_DIR}/GE/third_party/GLFW/include
        ${CMAKE_SOURCE_DIR}/GE/third_party/stb
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinyobjloader
        ${Vulkan_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIRS}/spirv_cross
)

target_compile_definitions(${GE_NAME} PUBLIC
        GLFW_INCLUDE_NONE
        GE_DEBUG
        TRACY_ENABLE         # 取消注释以启用 Tracy 性能分析
        VK_USE_PLATFORM_WIN32_KHR
        VK_DEBUG
        VK_NO_PROTOTYPES
        VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
        VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)


target_link_libraries(${GE_NAME} PUBLIC
        spdlog::spdlog
        glfw
        imgui
        yaml-cpp::yaml-cpp
        TracyClient
        ${Vulkan_LIBRARIES}
)

# 链接 Vulkan SDK 自带的 SPIRV-Cross 预编译库
if (DEFINED ENV{VULKAN_SDK})
    target_link_libraries(${GE_NAME} PUBLIC
            "$ENV{VULKAN_SDK}/Lib/spirv-cross-glsl$<$<CONFIG:Debug>:d>.lib"
            "$ENV{VULKAN_SDK}/Lib/spirv-cross-core$<$<CONFIG:Debug>:d>.lib"
    )
endif ()


# 开启预编译头 PCH
target_precompile_headers(${GE_NAME} PRIVATE GE/include/GE/pch.h)


# 引擎内置着色器编译（GLSL → SPIR-V）
# ==============================================
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(GLSLC)
    file(GLOB GE_SHADER_SOURCES
            ${CMAKE_SOURCE_DIR}/GE/assets/shaders/glsl/*.vert
            ${CMAKE_SOURCE_DIR}/GE/assets/shaders/glsl/*.frag)

    foreach(shader ${GE_SHADER_SOURCES})
        set(spv_output "${shader}.spv")
        add_custom_command(
                OUTPUT ${spv_output}
                COMMAND ${GLSLC} "${shader}" -o ${spv_output}
                DEPENDS "${shader}"
                COMMENT "Compiling engine shader ${shader} → SPIR-V"
        )
        list(APPEND GE_SPV_OUTPUTS ${spv_output})
    endforeach()

    if(GE_SPV_OUTPUTS)
        add_custom_target(GEShaderCompile ALL DEPENDS ${GE_SPV_OUTPUTS})
        add_dependencies(${GE_NAME} GEShaderCompile)
    endif()
else()
    message(WARNING "glslc not found — engine shaders won't be auto-compiled")
endif()


#Sandbox EXE
# ==============================================

set(Sandbox_SRC
        ${CMAKE_SOURCE_DIR}/sandbox/SandboxApp.cpp
        ${CMAKE_SOURCE_DIR}/sandbox/TriangleLayer.cpp
        ${CMAKE_SOURCE_DIR}/sandbox/TextureLayer.cpp
)

add_executable(${Sandbox_NAME} ${Sandbox_SRC})
target_include_directories(${Sandbox_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include

)
target_compile_definitions(${Sandbox_NAME} PRIVATE

)


target_link_libraries(${Sandbox_NAME} PRIVATE ${GE_NAME})

add_subdirectory(sandbox/assets/shaders/glsl)


#Editor
#file(GLOB Editor_SRC
#        ${CMAKE_SOURCE_DIR}/GE_Editor/src/*.cpp
#        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/*.cpp
#)
#
#
#add_executable(${Editor_NAME} ${Editor_SRC})
#target_include_directories(${Editor_NAME} PRIVATE
#        ${CMAKE_SOURCE_DIR}/GE/include
#        ${CMAKE_SOURCE_DIR}/GE/third_party/glm
#        ${CMAKE_SOURCE_DIR}/GE/third_party/imgui
#)
#target_link_libraries(${Editor_NAME} PRIVATE ${GE_NAME})
