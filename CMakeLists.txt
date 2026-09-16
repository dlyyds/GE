cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)

project(GE)


set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

#set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_BUILD_TYPE Release)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 输出目录（仅桌面）。
# Android 下**必须不设**：Gradle 的 externalNativeBuild 自己管理中间产物与 .so 的
# 落点，把 CMAKE_LIBRARY_OUTPUT_DIRECTORY 重定向到仓库根 bin/ 会让 AGP 找不到产物、
# 打不进 APK（这是 AGP + CMake 的经典坑）。
if (NOT ANDROID)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif ()


set(GE_NAME GE)
set(Editor_NAME GE_Editor)


if (MSVC)
    add_compile_options(/Zc:__cplusplus)
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/EHsc)
    # add_compile_options(/fsanitize=address)
    # add_link_options(/fsanitize=address /INCREMENTAL:NO)

endif ()


add_subdirectory(GE/third_party/spdlog)

# ==============================================
# SDL3 —— 跨平台窗口 / 输入 / 生命周期（取代 GLFW）
# ==============================================
# 静态链接：与 GE 静态库、BUILD_SHARED_LIBS OFF 一致；关掉上游自带的示例与测试
# 程序（test/ + examples/ 共 17MB，已同时列入 .gitignore）。
# 入口点机制：Win32 侧 SDL 提供 main/WinMain（按子系统自动选）并调用被重命名的
# SDL_main；Android 侧导出 SDL_main 供 SDLActivity 调用。两端共用同一个
# int main()，见 GE/include/GE/Core/EntryPoint.h。
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(GE/third_party/SDL)

add_subdirectory(GE/third_party/yaml-cpp)
add_subdirectory(GE/third_party/imgui)

# ImGuizmo 与 imgui-node-editor 是**编辑器专用**（gizmo 操纵柄、ASM 节点图），
# 只有 GE_Editor 用。Android 只移 GE_Runtime，故跳过，省编译量与 APK 体积。
if (NOT ANDROID)
    add_subdirectory(GE/third_party/ImGuizmo)
    add_subdirectory(GE/third_party/imgui-node-editor-0.9.3)
endif ()


add_subdirectory(GE/third_party/tracy)

# SPIRV-Cross（源码编译，替代 Vulkan SDK 预编译库）
# 引擎只用 CompilerGLSL，因此仅启用 GLSL，关闭 HLSL/MSL/CPP/C-API/CLI/测试加速构建。
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
add_subdirectory(GE/third_party/SPIRV-Cross)

# GE 层 Profiler 开关（控制 GE_PROFILE_* 宏，独立于 Tracy 的 TRACY_ENABLE）
# ON 时向编译定义传入 GE_PROFILE_ENABLE=1，OFF 时传入 =0。
# FORCE：改源码行的值即覆盖缓存，开关以源码行为准。
set(GE_PROFILE_ENABLE ON CACHE BOOL "Enable GE_PROFILE_* profiling macros" FORCE)

# ==============================================
# Vulkan
# ==============================================
# 桌面：Vulkan SDK 的 find_package 提供头文件目录与 vulkan-1.lib。
# Android：NDK 的 sysroot 自带 vulkan/vulkan.h（**注意是小写路径**，Windows 不区分
#          大小写把这一点藏了很久，见 GE/Core/GEWindow.h 的注释），运行时是系统
#          的 libvulkan.so，用 find_library 取即可 —— NDK 不提供 Vulkan 的 CMake
#          config 包，find_package(Vulkan) 在 Android 上不可靠。
if (ANDROID)
    find_library(VULKAN_LIB vulkan REQUIRED)
    message(STATUS "Android Vulkan 运行时库: ${VULKAN_LIB}")
else ()
    find_package(Vulkan REQUIRED)
endif ()

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
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)  # 静态链接，与 GE 静态库保持一致
add_subdirectory(GE/third_party/KTX-Software-4.4.2 KTX-Software EXCLUDE_FROM_ALL)


#GE lib
# ==============================================
file(GLOB GE_SRC CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/GE/src/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Platform/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Core/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/FileSystem/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/ImGui/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/VulkanBase/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Render/RenderGraph/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Scene/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Physics/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Audio/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Animation/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/stb/stb_image.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinyobjloader/tiny_obj_loader.cpp
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinygltf/tiny_gltf.cc

)


add_library(${GE_NAME} STATIC ${GE_SRC})

# ==============================================
# Lua 5.4（vendored）—— 独立静态库
# 单独成目标而不是混入 GE：C 源若并进带 PCH 的 C++ 目标，会同时吃到 /TC 与
# /FI cmake_pch.h，CLion 编译器探测即崩（STL1003）。隔离后每条链路的 FLAGS 干净。
# ==============================================
file(GLOB LUA_LIB_SRC ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8/*.c)
list(REMOVE_ITEM LUA_LIB_SRC
        ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8/lua.c
        ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8/luac.c
        ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8/ltests.c
        ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8/onelua.c)
add_library(ge_lua STATIC ${LUA_LIB_SRC})
target_include_directories(ge_lua PUBLIC ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8)

target_include_directories(${GE_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/GE/include/GE
        ${CMAKE_SOURCE_DIR}/GE/third_party/glm
        ${CMAKE_SOURCE_DIR}/GE/third_party/entt
        ${CMAKE_SOURCE_DIR}/GE/third_party
        ${CMAKE_SOURCE_DIR}/GE/third_party/vma
        ${CMAKE_SOURCE_DIR}/GE/third_party/lua-5.4.8
        ${CMAKE_SOURCE_DIR}/GE/third_party/stb
        ${CMAKE_SOURCE_DIR}/GE/third_party/tinyobjloader
        ${CMAKE_SOURCE_DIR}/GE/third_party/miniaudio-0.11.25
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/GE/third_party/SPIRV-Cross
)

target_compile_definitions(${GE_NAME} PUBLIC
        NOMINMAX
        GE_DEBUG
        VK_DEBUG
        VK_NO_PROTOTYPES
        VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
        VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        # tinygltf：不把图像解码成 RGBA 像素内存（引擎 glTF 贴图加载由纹理子系统单独做）
        TINYGLTF_NO_STB_IMAGE
        # tinygltf：不读磁盘上的外部 .png/.jpg，仅保留 glb 内嵌 buffer 图像二进制块
        TINYGLTF_NO_EXTERNAL_IMAGE
        GE_PROFILE_ENABLE=$<IF:$<BOOL:${GE_PROFILE_ENABLE}>,1,0>
)

# 平台 surface 扩展二选一。VulkanContext::ApplyDefaultExtensions 靠这个宏选
# VK_KHR_win32_surface / VK_KHR_android_surface —— 那个文件里两个分支**早已写好**，
# 这里选对宏即可，无需改渲染层代码。
if (ANDROID)
    target_compile_definitions(${GE_NAME} PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
else ()
    target_compile_definitions(${GE_NAME} PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif ()


target_link_libraries(${GE_NAME} PUBLIC
        spdlog::spdlog
        SDL3::SDL3
        imgui
        yaml-cpp::yaml-cpp
        TracyClient
        Jolt
        ktx
        spirv-cross-glsl
        spirv-cross-core
        ge_lua
)

# imguizmo 是编辑器专用目标，Android 下没建（见上面的 add_subdirectory 分区）。
if (ANDROID)
    target_link_libraries(${GE_NAME} PUBLIC ${VULKAN_LIB})
else ()
    target_link_libraries(${GE_NAME} PUBLIC imguizmo ${Vulkan_LIBRARIES})
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


# ==============================================
# 以下三个目标**仅桌面**（Android 只移 GE_Runtime）
# ==============================================
# - GE_Editor：依赖 ImGuizmo / imgui-node-editor / Win32 FileDialogs，移动端无意义
# - gemesh / gepack：主机侧离线 CLI 工具（模型烘焙、依赖收集打包），
#   它们在开发机上跑，产物（.gemesh / dist）再喂给 Android 构建，不该进 APK
if (NOT ANDROID)

#GE_Editor EXE（独立编辑器可执行）
# ==============================================

set(Editor_SRC
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/EditorApp.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/DebugDrawLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/SceneLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/SceneViewport.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/DockSpaceLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/HierarchyLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/GizmoController.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/ResourceLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/ASMGraphLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/StatsLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel_Components.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel_Mesh.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel_Animation.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel_Physics.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/SceneHierarchyPanel_AudioScript.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/ResourcePanel.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/ASMGraphPanel.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/Panels/RenderStatsPanel.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/NodeEditorUtils/drawing.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/NodeEditorUtils/widgets.cpp
        ${CMAKE_SOURCE_DIR}/GE_Editor/src/NodeEditorUtils/builders.cpp
)

add_executable(${Editor_NAME} ${Editor_SRC})
target_include_directories(${Editor_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include
        ${CMAKE_SOURCE_DIR}/GE_Editor/src
)


target_link_libraries(${Editor_NAME} PRIVATE ${GE_NAME})
target_link_libraries(${Editor_NAME} PRIVATE imgui_node_editor)

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

#gepack CLI（依赖收集 + 打包：场景 → 资产清单 → 校验 → dist/）
# ==============================================
set(GEpackTools_SRC
        ${CMAKE_SOURCE_DIR}/tools/gepack/gepack_main.cpp
        ${CMAKE_SOURCE_DIR}/tools/gepack/PackRules.cpp
        ${CMAKE_SOURCE_DIR}/tools/gepack/AssetDependencyGraph.cpp
        ${CMAKE_SOURCE_DIR}/tools/gepack/Packager.cpp
)

add_executable(gepack ${GEpackTools_SRC})
target_include_directories(gepack PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include
        ${CMAKE_SOURCE_DIR}/tools/gepack
)

target_link_libraries(gepack PRIVATE ${GE_NAME})

endif () # NOT ANDROID

#GE_Runtime EXE（发行版播放器：无编辑器 UI，场景直画背缓冲）
# ==============================================
# 桌面：可执行文件（.exe）。Android：**共享库** —— APK 里是
# lib/<abi>/libGE_Runtime.so，由 SDLActivity 经 SDL_main 调起，
# 所以这里必须产出 SHARED 而不是 executable，否则 AGP 打不进 APK。
set(Runtime_NAME GE_Runtime)

set(Runtime_SRC
        ${CMAKE_SOURCE_DIR}/GE_Runtime/src/RuntimeApp.cpp
        ${CMAKE_SOURCE_DIR}/GE_Runtime/src/GameLayer.cpp
        ${CMAKE_SOURCE_DIR}/GE_Runtime/src/GameConfig.cpp
)

if (ANDROID)
    add_library(${Runtime_NAME} SHARED ${Runtime_SRC})
else ()
    add_executable(${Runtime_NAME} ${Runtime_SRC})
endif ()

target_include_directories(${Runtime_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include
        ${CMAKE_SOURCE_DIR}/GE_Runtime/src
)

target_link_libraries(${Runtime_NAME} PRIVATE ${GE_NAME})

#共享着色器（GLSL → SPIR-V），GE_Editor 依赖
add_subdirectory(assets/shaders/glsl)
