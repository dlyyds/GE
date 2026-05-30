cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 20)

project(GE)

# 匹配 bimg 的静态 CRT (/MT /MTd)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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


#GE lib
# ==============================================
file(GLOB GE_SRC
        ${CMAKE_SOURCE_DIR}/GE/src/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Platform/Windows/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/Core/*.cpp
        ${CMAKE_SOURCE_DIR}/GE/src/ImGui/*.cpp
)

set(BGFX_DIR "F:/yxy/c++lib/bgfx")


add_library(${GE_NAME} STATIC ${GE_SRC})

target_include_directories(${GE_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/GE/include/GE
        ${CMAKE_SOURCE_DIR}/GE/third_party/glm
        ${CMAKE_SOURCE_DIR}/GE/third_party/entt
        ${CMAKE_SOURCE_DIR}/GE/third_party
        ${CMAKE_SOURCE_DIR}/GE/third_party/GLFW/include
        ${CMAKE_SOURCE_DIR}/GE/third_party/bx
        ${CMAKE_SOURCE_DIR}/GE/third_party/imgui_impl_bgfx
)

target_compile_definitions(${GE_NAME} PUBLIC
        GLFW_INCLUDE_NONE
        GE_DEBUG
        $<$<CONFIG:Debug>:BX_CONFIG_DEBUG=1>
        $<$<CONFIG:Release>:BX_CONFIG_DEBUG=0>
)

set(BGFX_BUILD_DIR "${BGFX_DIR}/.build/win64_vs2022/bin")


target_link_libraries(${GE_NAME} PUBLIC
        spdlog::spdlog
        glfw
        imgui
        yaml-cpp::yaml-cpp
        # Debug
        debug ${BGFX_BUILD_DIR}/bgfxDebug.lib
        debug ${BGFX_BUILD_DIR}/bxDebug.lib
        debug ${BGFX_BUILD_DIR}/bimgDebug.lib
        debug ${BGFX_BUILD_DIR}/bimg_decodeDebug.lib

        # Release
        optimized ${BGFX_BUILD_DIR}/bgfxRelease.lib
        optimized ${BGFX_BUILD_DIR}/bxRelease.lib
        optimized ${BGFX_BUILD_DIR}/bimgRelease.lib
        optimized ${BGFX_BUILD_DIR}/bimg_decodeRelease.lib
)


# 开启预编译头 PCH
target_precompile_headers(${GE_NAME} PRIVATE GE/include/GE/pch.h)


#Sandbox EXE
# ==============================================


file(GLOB Sandbox_SRC
        ${CMAKE_SOURCE_DIR}/sandbox/*.cpp
)

add_executable(${Sandbox_NAME} ${Sandbox_SRC})
target_include_directories(${Sandbox_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/GE/include

)


target_link_libraries(${Sandbox_NAME} PRIVATE ${GE_NAME})


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
