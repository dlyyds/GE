#pragma once

#include "imgui.h"

namespace GE {

/// 资源面板 —— 用 ImGui 展示全局加载的 纹理 / 材质 / 网格 资源。
///
/// 功能：
/// - "Textures"  段：列出所有已加载纹理（路径/尺寸/格式）
/// - "Materials" 段：列出所有已加载材质（类型/纹理槽位/标量参数/渲染状态）
/// - "Meshes"    段：列出所有已加载网格（内置标识或文件路径/顶点数/索引数）
///
/// 数据来源为 Renderer 全局管理器（TextureManager / MaterialManager / MeshManager），
/// 面板不拥有任何资源，仅做只读展示，资源生命周期由各管理器统一管理。
class ResourcePanel {
public:
    ResourcePanel() = default;

    /// 每帧 ImGui 渲染
    void OnImGuiRender();

private:
    /// 绘制纹理资源列表
    void DrawTextureSection();

    /// 绘制材质资源列表
    void DrawMaterialSection();

    /// 绘制网格资源列表
    void DrawMeshSection();

private:
    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"，首帧初始化一次）
    ImGuiID m_DockSpaceID = 0;
};

} // namespace GE