#pragma once

#include "GE/GE.h"

namespace GE {

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

private:
    /// 是否需要在下一帧重新建立默认布局（首帧或用户点击"重置布局"后为 true）
    bool m_FirstFrame = true;
};

} // namespace GE