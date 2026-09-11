#pragma once

#include "GE/GE.h"

#include "EditorContext.h"
#include "Panels/ResourcePanel.h"

#include <memory>

namespace GE {

/// 资源面板层 —— 独立承载 ResourcePanel（"Resource" 窗口）。
/// 展示并可编辑全局 纹理/材质/网格 资源。
///
/// 与场景层共享 EditorContext：材质面板要把「网格自带材质」的编辑提升为
/// `.gemat` 资产并给使用该网格的实体写覆写，必须能遍历场景实体。场景对象在
/// 新建/加载时会被替换，本层每帧对比指针，变了就重新绑定面板。
class ResourceLayer : public Layer {
public:
    explicit ResourceLayer(std::shared_ptr<EditorContext> context);

    ~ResourceLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

private:
    std::shared_ptr<EditorContext> m_Context;  ///< 共享场景上下文
    ResourcePanel m_Panel;                    ///< 资源面板（ImGui）
    Scene *m_LastScene = nullptr;             ///< 上次绑定的场景对象（用于感知替换）
};

} // namespace GE