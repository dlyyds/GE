cmake_minimum_required(VERSION 3.10)
project(imgui)

# 收集所有 imgui 核心源文件
set(IMGUI_SOURCES
        imgui.cpp
        imgui_draw.cpp
        imgui_widgets.cpp
        imgui_tables.cpp
)

# 创建静态库
add_library(imgui STATIC ${IMGUI_SOURCES})

# 公开头文件目录（让主项目能 #include "imgui.h"）
target_include_directories(imgui
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(imgui PUBLIC SDL3::SDL3)