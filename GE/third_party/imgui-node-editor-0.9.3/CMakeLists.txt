cmake_minimum_required(VERSION 3.10)
project(imgui_node_editor)

# imgui-node-editor 0.9.3 —— 节点图编辑器（仅 GE_Editor 链接）
# 编译 4 个实现源；头文件/inl/natvis 不单独编译。
add_library(imgui_node_editor STATIC
        crude_json.cpp
        imgui_canvas.cpp
        imgui_node_editor.cpp
        imgui_node_editor_api.cpp
)

target_include_directories(imgui_node_editor
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 复用引擎现有 imgui 目标（库 0.9.3 的 splitter 辅助函数库内自带，与引擎 imgui 1.92.8 兼容）
target_link_libraries(imgui_node_editor PUBLIC imgui)
