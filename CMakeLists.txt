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


add_subdirectory(GE/third_party/tracy)

# GE 层 Profiler 开关（控制 GE_PROFILE_* 宏，独立于 Tracy 的 TRACY_ENABLE）
# ON 时向编译定义传入 GE_PROFILE_ENABLE=1，OFF 时传入 =0。
# FORCE：改源码行的值即覆盖缓存，开关以源码行为准。
set(GE_PROFILE_ENABLE OFF CACHE BOOL "Enable GE_PROFILE_* profiling macros" FORCE)

find_package(Vulkan REQUIRED)

# ==============================================
# Jolt Physics
# ==============================================
# 关闭 Jolt 的示例/测试/Viewer 构建
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

# 与项目保持一致：使用 DLL 运行时库 (/MD)，关闭浮点异常，启用 RTTI/异常
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)

add_subdirectory(GE/third_party/JoltPhysics/Build JoltPhysics)


# ==============================================
# KTX-Software (libktx) — KTX2 纹理加载/上传
# ==============================================
# 只编译 libktx（目标名 ktx，含写入能力，可导出 .ktx2）。
# 关闭工具/测试/各语言绑定/文档，避免拖慢构建。
# EXCLUDE_FROM_ALL：不进入默认全量构建，仅在 GE 链接它时按需编译。
# 子模块（basis_universal/dfdutils/astc-encoder）已随仓库带全。
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS "" CACHE STRING "" FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)  # 静态链接，与 GE 静态库保持一致
add_subdirectory(GE/third_party/KTX-Software-main KTX-Software EXCLUDE_FROM_ALL)


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
        ${CMAKE_SOURCE_DIR}/GE/src/Physics/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/stb/stb_image.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinyobjloader/tiny_obj_loader.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinygltf/tiny_gltf.cc

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
        NOMINMAX
        GLFW_INCLUDE_NONE
        GE_DEBUG
        VK_USE_PLATFORM_WIN32_KHR
        VK_DEBUG
        VK_NO_PROTOTYPES
        VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
        VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        GE_PROFILE_ENABLE=$<IF:$<BOOL:${GE_PROFILE_ENABLE}>,1,0>
)


target_link_libraries(${GE_NAME} PUBLIC
        spdlog::spdlog
        glfw
        imgui
        imguizmo
        yaml-cpp::yaml-cpp
        TracyClient
        ${Vulkan_LIBRARIES}
        Jolt
        ktx
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
if (GLSLC)
    file(GLOB GE_SHADER_SOURCES
            ${CMAKE_SOURCE_DIR}/GE/assets/shaders/glsl/*.vert
            ${CMAKE_SOURCE_DIR}/GE/assets/shaders/glsl/*.frag)

    foreach (shader ${GE_SHADER_SOURCES})
        set(spv_output "${shader}.spv")
        add_custom_command(
                OUTPUT ${spv_output}
                COMMAND ${GLSLC} "${shader}" -o ${spv_output}
                DEPENDS "${shader}"
                COMMENT "Compiling engine shader ${shader} → SPIR-V"
        )
        list(APPEND GE_SPV_OUTPUTS ${spv_output})
    endforeach ()

    if (GE_SPV_OUTPUTS)
        add_custom_target(GEShaderCompile ALL DEPENDS ${GE_SPV_OUTPUTS})
        add_dependencies(${GE_NAME} GEShaderCompile)
    endif ()
else ()
    message(WARNING "glslc not found — engine shaders won't be auto-compiled")
endif ()


#Sandbox EXE（最小冒烟测试应用）
# ==============================================

set(Sandbox_SRC
        ${CMAKE_SOURCE_DIR}/sandbox/SandboxApp.cpp
)

add_executable(${Sandbox_NAME} ${Sandbox_SRC})
target_include_directories(${Sandbox_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include
        ${CMAKE_SOURCE_DIR}/sandbox
)


target_link_libraries(${Sandbox_NAME} PRIVATE ${GE_NAME})


#GE_Editor EXE（独立编辑器可执行）
# ==============================================

set(Editor_SRC
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/EditorApp.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/SceneLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/SceneViewport.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/DockSpaceLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/HierarchyLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/GizmoController.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/ResourceLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/ResourcePanel.cpp
)

add_executable(${Editor_NAME} ${Editor_SRC})
target_include_directories(${Editor_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include
        ${CMAKE_SOURCE_DIR}/GE_Editor/src
)


target_link_libraries(${Editor_NAME} PRIVATE ${GE_NAME})

#gemesh CLI（.obj → .gemesh 离线导出工具）
# ==============================================
set(GEMeshTools_SRC
        ${CMAKE_SOURCE_DIR}/tools/gemesh/gemesh_main.cpp
)

add_executable(gemesh ${GEMeshTools_SRC})
target_include_directories(gemesh PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include
)

target_link_libraries(gemesh PRIVATE ${GE_NAME})

#共享着色器（GLSL → SPIR-V），Sandbox 与 GE_Editor 均依赖
add_subdirectory(assets/shaders/glsl)
