#pragma once

#include "GE/GE.h"
#include "GE/Render/VulkanBase/Texture.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"

namespace GE {

/// 使用场景系统（Scene + Entity + 组件）的精灵绘制层。
/// 单个精灵实体，通过 ImGui 面板调节其 Transform / SpriteRenderer 组件参数。
class TextureLayer : public Layer {
public:
    TextureLayer();
    ~TextureLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Timestep &ts) override;
    void OnEvent(Event &event) override;
    void OnImGuiRender() override;

private:
    std::unique_ptr<Texture> m_Texture;   ///< 棋盘纹理
    std::unique_ptr<Scene> m_Scene;       ///< 场景
    Entity m_SpriteEntity;                ///< 精灵实体

    // 行为参数（不属于组件，控制每帧逻辑）
    bool m_AutoRotate{true};              ///< 是否自动旋转
    float m_AutoRotateSpeed{0.5f};        ///< 自动旋转速度（弧度/秒），作用于 Z 轴
};

} // namespace GE
