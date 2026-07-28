#pragma once

#include "GE/GE.h"
#include "GE/Render/VulkanBase/Texture.h"

namespace GE {

/// 使用 Renderer2D 的精灵绘制层 —— 显示 5 个旋转的棋盘纹理精灵。
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
};

} // namespace GE
