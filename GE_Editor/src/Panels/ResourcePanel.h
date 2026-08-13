#pragma once

#include "imgui.h"

#include "GE/Render/Material.h"

#include <unordered_map>
#include <vector>

namespace GE {

/// 资源面板 —— 用 ImGui 展示并调试全局 纹理 / 材质 / 网格 资源。
///
/// 功能：
/// - 顶部统计条：纹理 / 材质 / 网格 数量总览
/// - "纹理" 段：缩略图 + 路径 / 尺寸 / 格式，支持按名过滤
/// - "材质" 段：可实时编辑着色器类型、纹理槽位、标量参数与渲染状态
/// - "网格"  段：顶点 / 索引数一览，支持按名过滤
///
/// 数据来源为 Renderer 全局管理器（TextureManager / MaterialManager / MeshManager），
/// 面板不拥有任何资源，仅做调试展示与参数调整，资源生命周期由各管理器统一管理。
class ResourcePanel {
public:
    ResourcePanel() = default;

    /// 每帧 ImGui 渲染
    void OnImGuiRender();

private:
    /// 顶部统计条
    void DrawStatsBar();

    /// 绘制纹理资源列表（缩略图 + 过滤）
    void DrawTextureSection();

    /// 绘制「新增纹理」表单（纯色 / 从文件加载）
    void DrawTextureCreationControls();

    /// 绘制材质资源列表（可编辑）
    void DrawMaterialSection();

    /// 绘制网格资源列表（过滤）
    void DrawMeshSection();

    /// 绘制单张纹理的采样器编辑控件（过滤/寻址/各向异性）
    void DrawSamplerControls(Texture *tex);

    /// 单个纹理槽位的赋值下拉（置于属性表右列）
    void DrawTextureAssignRow(Material *mat, Material::TextureSlot slot,
                              const std::vector<std::string> &texKeys);

    /// 属性表左列：右侧对齐的标签（可选带纹理缩略图）
    void DrawPropertyLabel(const char *text, Texture *thumbnail = nullptr);

    /// 获取（或创建并缓存）指定纹理的 ImGui 缩略图描述符集
    ImTextureID GetThumbnail(Texture *tex);

    /// 清理仍被缓存但已不再存活的缩略图描述符集
    void PruneThumbnails(const std::vector<const Texture *> &live);

private:
    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"，首帧初始化一次）
    ImGuiID m_DockSpaceID = 0;

    /// 纹理指针 -> ImGui 缩略图描述符集 缓存（避免每帧重复创建）
    std::unordered_map<const Texture *, ImTextureID> m_Thumbnails;

    /// 纹理 / 网格列表的按名过滤输入框
    char m_TextureFilter[128] = "";
    char m_MeshFilter[128] = "";

    /// 新增纹理表单的状态
    float m_NewTexColor[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 纯色纹理 RGBA
    char  m_NewTexPath[256] = "";                      ///< 从文件加载的路径
};

} // namespace GE