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

    // 添加脚本组件（自动旋转逻辑）
    RefreshScript();
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

    // 场景更新 + 渲染（脚本更新 + 精灵渲染均在 Scene::OnUpdate 内完成）
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

    // Script 组件参数
    ImGui::Text("Script 组件（自动旋转）");
    bool scriptChanged = false;
    scriptChanged |= ImGui::Checkbox("启用", &m_AutoRotate);
    if (m_AutoRotate) {
        scriptChanged |= ImGui::DragFloat("旋转速度", &m_AutoRotateSpeed,
                                          0.05f, -10.0f, 10.0f,
                                          "%.3f rad/s");
    }
    if (scriptChanged) {
        RefreshScript();
    }

    ImGui::Separator();

    // 统计信息
    auto spriteView = m_Scene->Reg().view<SpriteRendererComponent>();
    auto scriptView = m_Scene->Reg().view<ScriptComponent>();
    ImGui::Text("场景精灵数：%zu", spriteView.size());
    ImGui::Text("场景脚本数：%zu", scriptView.size());
    ImGui::Text("Draw call 数：1（同纹理合并）");

    // 重置按钮
    if (ImGui::Button("重置参数")) {
        tc.Translation = {0.0f, 0.0f, 0.0f};
        tc.Rotation = {0.0f, 0.0f, 0.0f};
        tc.Scale = {1.0f, 1.0f, 1.0f};
        sc.Color = {1.0f, 1.0f, 1.0f, 1.0f};
        m_AutoRotate = true;
        m_AutoRotateSpeed = 0.5f;
        RefreshScript();
    }

    ImGui::End();
}

void TextureLayer::RefreshScript() {
    if (!m_SpriteEntity) {
        return;
    }

    if (!m_AutoRotate) {
        // 关闭自动旋转：移除脚本组件
        if (m_SpriteEntity.HasComponent<ScriptComponent>()) {
            m_SpriteEntity.RemoveComponent<ScriptComponent>();
        }
        return;
    }

    // 开启自动旋转：添加 / 更新脚本组件
    float speed = m_AutoRotateSpeed;
    auto callback = [speed](Timestep ts, Entity entity) {
        auto &tc = entity.GetComponent<TransformComponent>();
        tc.Rotation.z += ts.GetSeconds() * speed;
    };

    if (m_SpriteEntity.HasComponent<ScriptComponent>()) {
        m_SpriteEntity.GetComponent<ScriptComponent>().OnUpdate = std::move(callback);
    } else {
        m_SpriteEntity.AddComponent<ScriptComponent>(std::move(callback));
    }
}

} // namespace GE
