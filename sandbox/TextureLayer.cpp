//
// 使用 Renderer2D 绘制 5 个旋转精灵的 TextureLayer
//

#include "TextureLayer.h"
#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer2D.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

namespace {
constexpr size_t kInstanceCount = 5;
}

TextureLayer::TextureLayer() : Layer("TextureLayer") {
}

TextureLayer::~TextureLayer() = default;

void TextureLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    auto &cache  = device.GetResourceCache();

    // 加载棋盘纹理
    m_Texture = Texture::LoadFromFile(device, cache,
                                      "assets/textures/Checkerboard.png",
                                      vk::Format::eR8G8B8A8Srgb,
                                      vk::Filter::eNearest,
                                      vk::Filter::eNearest);
}

void TextureLayer::OnDetach() {
    m_Texture.reset();
}

void TextureLayer::OnUpdate(Timestep &ts) {
    auto extent = Application::GetSwapchain().GetExtent();

    // 构建正交投影矩阵（2D 精灵用正交投影更合适）
    // 视图矩阵设为单位矩阵（我们以世界坐标直接绘制）
    float aspect = static_cast<float>(extent.width) /
                   static_cast<float>(extent.height);
    float halfHeight = 5.0f;   // 视口高度的一半
    float halfWidth  = halfHeight * aspect;

    glm::mat4 viewProjection = glm::ortho(-halfWidth, halfWidth,
                                          -halfHeight, halfHeight,
                                          -1.0f, 1.0f);
    // Vulkan Y 轴翻转
    viewProjection[1][1] *= -1.0f;

    static float rotation = 0.0f;
    rotation += ts.GetSeconds() * 0.5f;

    // 5 个实例的颜色和角度
    glm::vec3 instanceColors[kInstanceCount] = {
        {1.0f, 1.0f, 1.0f},   // 白
        {1.0f, 0.4f, 0.4f},   // 红
        {0.4f, 1.0f, 0.4f},   // 绿
        {0.4f, 0.6f, 1.0f},   // 蓝
        {1.0f, 0.9f, 0.3f},   // 黄
    };
    float instanceAngles[kInstanceCount] = {
        0.0f,
        glm::radians(72.0f),
        glm::radians(144.0f),
        glm::radians(216.0f),
        glm::radians(288.0f),
    };

    // ── 使用 Renderer2D 批量绘制精灵 ──────────────────────────────────
    auto &r2d = Renderer::Get2DRenderer();
    r2d.BeginScene(viewProjection);

    for (size_t i = 0; i < kInstanceCount; ++i) {
        float angle = rotation + instanceAngles[i];
        glm::vec2 pos(std::cos(angle) * 1.5f, std::sin(angle) * 1.5f);

        r2d.DrawSprite(
            pos,                       // 位置
            glm::vec2(1.2f, 1.2f),     // 尺寸
            rotation * 2.0f,           // 旋转（弧度）
            m_Texture.get(),           // 纹理
            glm::vec4(instanceColors[i], 1.0f)  // 颜色
        );
    }

    r2d.EndScene();
}

void TextureLayer::OnEvent(Event &event) {
}

void TextureLayer::OnImGuiRender() {
    ImGui::Begin("TextureLayer");
    ImGui::Text("5 个旋转精灵（Renderer2D 批处理）");
    ImGui::Separator();
    ImGui::Text("渲染器：Renderer2D（批处理）");
    ImGui::Text("纹理：Checkerboard.png");
    ImGui::Text("精灵数：%zu", kInstanceCount);
    ImGui::Text("Draw call 数：1（同纹理合并）");
    if (m_Texture) {
        ImGui::Text("纹理尺寸：%d x %d",
                    m_Texture->GetExtent().width,
                    m_Texture->GetExtent().height);
    }
    ImGui::End();
}

} // namespace GE
