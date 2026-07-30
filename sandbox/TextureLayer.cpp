//
// 使用场景系统绘制单个精灵的 TextureLayer，可通过 ImGui 调节组件参数
//

#include "TextureLayer.h"
#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Scene/Components.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

namespace GE {

TextureLayer::TextureLayer() : Layer("TextureLayer") {
}

TextureLayer::~TextureLayer() = default;

void TextureLayer::OnAttach() {
    auto &ctx = Application::GetVulkanContext();
    auto &device = ctx.GetDevice();
    auto &cache = device.GetResourceCache();

    // 加载棋盘纹理
    m_Texture = Texture::LoadFromFile(device, cache,
                                      "assets/textures/Checkerboard.png",
                                      vk::Format::eR8G8B8A8Srgb,
                                      vk::Filter::eNearest,
                                      vk::Filter::eNearest);
    m_Texture->SetDebugName("Checkerboard");

    // 创建场景与精灵实体
    m_Scene = std::make_unique<Scene>();
    m_SpriteEntity = m_Scene->CreateEntity("Sprite");

    // 添加精灵渲染组件（关联棋盘纹理，默认白色）
    m_SpriteEntity.AddComponent<SpriteRendererComponent>(m_Texture.get());
}

void TextureLayer::OnDetach() {
    m_SpriteEntity = {};
    m_Scene.reset();
    m_Texture.reset();
}

void TextureLayer::OnUpdate(Timestep &ts) {
    auto extent = Application::GetSwapchain().GetExtent();

    // 构建正交投影矩阵（2D 精灵用正交投影更合适）
    float aspect = static_cast<float>(extent.width) /
                   static_cast<float>(extent.height);
    float halfHeight = 5.0f; // 视口高度的一半
    float halfWidth = halfHeight * aspect;

    glm::mat4 viewProjection = glm::ortho(-halfWidth, halfWidth,
                                          -halfHeight, halfHeight,
                                          -1.0f, 1.0f);
    // Vulkan Y 轴翻转
    viewProjection[1][1] *= -1.0f;

    // 自动旋转：修改 TransformComponent 的 Z 轴旋转
    if (m_AutoRotate) {
        auto &tc = m_SpriteEntity.GetComponent<TransformComponent>();
        tc.Rotation.z += ts.GetSeconds() * m_AutoRotateSpeed;
    }

    // 场景更新 + 渲染（内部遍历精灵组件并提交给 Renderer2D）
    m_Scene->OnUpdate(ts, viewProjection);
}

void TextureLayer::OnEvent(Event &event) {
}

void TextureLayer::OnImGuiRender() {
    ImGui::Begin("TextureLayer");
    ImGui::Text("场景系统：单精灵实体（Scene + Entity + 组件）");
    ImGui::Separator();

    if (!m_SpriteEntity) {
        ImGui::TextDisabled("实体未创建");
        ImGui::End();
        return;
    }

    auto &tc = m_SpriteEntity.GetComponent<TransformComponent>();
    auto &sc = m_SpriteEntity.GetComponent<SpriteRendererComponent>();

    // Tag
    auto &tag = m_SpriteEntity.GetComponent<TagComponent>().Tag;
    ImGui::Text("实体名称：%s", tag.c_str());
    ImGui::Separator();

    // Transform 组件参数
    ImGui::Text("Transform 组件");
    ImGui::DragFloat3("位置 (Translation)", &tc.Translation.x, 0.05f,
                      -10.0f, 10.0f);
    ImGui::SliderFloat3("旋转 (Rotation, rad)", &tc.Rotation.x,
                        -3.14159f * 2.0f, 3.14159f * 2.0f);
    ImGui::DragFloat3("缩放 (Scale)", &tc.Scale.x, 0.05f,
                      0.01f, 10.0f);

    ImGui::Separator();

    // SpriteRenderer 组件参数
    ImGui::Text("SpriteRenderer 组件");
    ImGui::ColorEdit4("颜色 (Color)", &sc.Color.r);
    ImGui::Text("纹理：%s",
                sc.SpriteTexture ? "Checkerboard.png" : "(null)");
    if (sc.SpriteTexture) {
        ImGui::Text("纹理尺寸：%d x %d",
                    sc.SpriteTexture->GetExtent().width,
                    sc.SpriteTexture->GetExtent().height);
    }

    ImGui::Separator();

    // 行为参数
    ImGui::Text("行为");
    ImGui::Checkbox("自动旋转（Z 轴）", &m_AutoRotate);
    if (m_AutoRotate) {
        ImGui::DragFloat("旋转速度", &m_AutoRotateSpeed, 0.05f,
                         -10.0f, 10.0f, "%.3f rad/s");
    }

    ImGui::Separator();

    // 统计信息
    auto view = m_Scene->Reg().view<SpriteRendererComponent>();
    ImGui::Text("场景精灵数：%zu", view.size());
    ImGui::Text("Draw call 数：1（同纹理合并）");

    // 重置按钮
    if (ImGui::Button("重置参数")) {
        tc.Translation = {0.0f, 0.0f, 0.0f};
        tc.Rotation = {0.0f, 0.0f, 0.0f};
        tc.Scale = {1.0f, 1.0f, 1.0f};
        sc.Color = {1.0f, 1.0f, 1.0f, 1.0f};
        m_AutoRotate = true;
        m_AutoRotateSpeed = 0.5f;
    }

    ImGui::End();
}

} // namespace GE
