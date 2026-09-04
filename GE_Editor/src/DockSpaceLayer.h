#pragma once

#include "GE/GE.h"

namespace GE {

class SceneLayer;

/// 全屏 DockSpace 宿主层：在每帧 ImGui 渲染时第一个执行，创建主停靠区并建立默认布局。
///
/// 所有编辑器窗口（Scene / SceneLayer / Scene Hierarchy / Properties / Resource / 渲染统计）
/// 通过「首帧 DockBuilder + 各窗口 Begin 前的 SetNextWindowDockID」停靠进该 DockSpace。
///
/// DockSpace 的稳定 ID 在根上下文取 ImGui::GetID("MainDockspace")，
/// 各窗口只要在根上下文用同一字符串即可得到相同 ID，无需跨层传引用。
class DockSpaceLayer : public Layer {
public:
    DockSpaceLayer();

    ~DockSpaceLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

    /// 绑定场景层，用于在顶部「文件」菜单中触发 保存 / 加载 / 新建 场景操作
    void SetSceneLayer(SceneLayer *scene_layer) { m_SceneLayer = scene_layer; }

private:
    /// 场景层句柄（由 EditorApp 在创建场景层后绑定）
    SceneLayer *m_SceneLayer = nullptr;

    /// 是否需要在下一帧重新建立默认布局（首帧或用户点击"重置布局"后为 true）
    bool m_FirstFrame = true;
};

} // namespace GE