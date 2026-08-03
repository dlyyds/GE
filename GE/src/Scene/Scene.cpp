#include "pch.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/Entity.h"
#include "Render/Renderer.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"
#include "Render/Mesh.h"

#include <algorithm>
#include <glm/glm.hpp>

namespace GE {


Scene::Scene() = default;

Scene::~Scene() = default;

Entity Scene::CreateEntity(const std::string &name) {
    Entity entity{m_Registry.create(), this};

    entity.AddComponent<TransformComponent>();
    entity.AddComponent<TagComponent>(name);

    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    m_Registry.destroy(static_cast<entt::entity>(entity));
}


void Scene::OnUpdate(Timestep ts,
                     const glm::mat4 &viewProjection,
                     const glm::vec4 &clearColor) {
    // ── 脚本更新 ────────────────────────────────────────────────────────
    {
        auto view = m_Registry.view<ScriptComponent>();
        for (auto entityHandle : view) {
            auto &sc = view.get<ScriptComponent>(entityHandle);
            if (sc.Enabled && sc.OnUpdate) {
                Entity entity{entityHandle, this};
                sc.OnUpdate(ts, entity);
            }
        }
    }

    // ── 2D 精灵渲染 ────────────────────────────────────────────────────
    auto &r2d = Renderer::Get2DRenderer();
    r2d.BeginScene(glm::mat4(1.0f), viewProjection, false, clearColor);

    auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>();
    for (auto entity : view) {
        auto &tc = view.get<TransformComponent>(entity);
        auto &sc = view.get<SpriteRendererComponent>(entity);

        r2d.DrawSprite(
            tc.GetTransform(),
            sc.SpriteTexture,
            sc.Color
        );
    }

    r2d.EndScene();
}

void Scene::OnUpdate3D(Timestep ts,
                       const glm::mat4 &view,
                       const glm::mat4 &projection,
                       const glm::vec3 &viewPos,
                       const glm::vec4 &clearColor) {
    // ── 脚本更新 ────────────────────────────────────────────────────────
    {
        auto scriptView = m_Registry.view<ScriptComponent>();
        for (auto entityHandle : scriptView) {
            auto &sc = scriptView.get<ScriptComponent>(entityHandle);
            if (sc.Enabled && sc.OnUpdate) {
                Entity entity{entityHandle, this};
                sc.OnUpdate(ts, entity);
            }
        }
    }

    // ── 3D 网格渲染 ────────────────────────────────────────────────────
    auto &r3d = Renderer::Get3DRenderer();

    // ── 收集场景中的点光源 ─────────────────────────────────────────────
    {
        auto &lightParams = r3d.GetLightParams();
        size_t lightIndex = 0;

        auto pointLightView = m_Registry.view<TransformComponent, PointLightComponent>();
        for (auto entity : pointLightView) {
            if (lightIndex >= Renderer3D::MAX_POINT_LIGHTS) {
                break;  // 超过上限，忽略多余的点光源
            }

            auto &tc = pointLightView.get<TransformComponent>(entity);
            auto &plc = pointLightView.get<PointLightComponent>(entity);

            auto &dst = lightParams.pointLights[lightIndex];
            dst.position  = tc.Translation;
            dst.color     = plc.Color;
            dst.radiusInv = plc.RadiusInv;

            lightIndex++;
        }

        lightParams.pointLightCount = lightIndex;
    }

    r3d.BeginScene(view, projection, viewPos, clearColor);

    auto meshView = m_Registry.view<TransformComponent, MeshComponent>();
    for (auto entity : meshView) {
        auto &tc = meshView.get<TransformComponent>(entity);
        auto &mc = meshView.get<MeshComponent>(entity);

        if (mc.MeshPtr) {
            r3d.DrawMesh(
                tc.GetTransform(),
                mc.MeshPtr,
                mc.BaseTexture,
                mc.Color
            );
        }
    }

    r3d.EndScene();

    // ── 2D 精灵渲染：分世界空间 + UI 空间两批 ─────────────────────────
    glm::mat4 viewProjection = projection * view;

    auto &r2d = Renderer::Get2DRenderer();
    auto spriteView = m_Registry.view<TransformComponent, SpriteRendererComponent>();

    // ---- 第一批：世界空间精灵（IsUI=false），参与深度测试 ----
    bool hasWorldSprites = false;
    for (auto entity : spriteView) {
        if (!spriteView.get<SpriteRendererComponent>(entity).IsUI) {
            hasWorldSprites = true;
            break;
        }
    }
    if (hasWorldSprites) {
        r2d.BeginScene(view, projection, true, glm::vec4(-1.0f));
        for (auto entity : spriteView) {
            auto &tc = spriteView.get<TransformComponent>(entity);
            auto &sc = spriteView.get<SpriteRendererComponent>(entity);
            if (sc.IsUI) continue;

            r2d.DrawSprite(
                tc.GetTransform(),
                sc.SpriteTexture,
                sc.Color
            );
        }
        r2d.EndScene();
    }

    // ---- 第二批：UI 精灵（IsUI=true），像素坐标正交投影，无深度 ----
    // 收集 UI 精灵，按 z 值从小到大排序（z 小的先画，z 大的后画、盖在上面）
    struct UISprite {
        TransformComponent *tc;
        SpriteRendererComponent *sc;
        float z;
    };
    std::vector<UISprite> uiSprites;
    for (auto entity : spriteView) {
        auto &sc = spriteView.get<SpriteRendererComponent>(entity);
        if (!sc.IsUI) continue;
        auto &tc = spriteView.get<TransformComponent>(entity);
        uiSprites.push_back({&tc, &sc, tc.Translation.z});
    }
    if (!uiSprites.empty() && m_ViewportWidth > 0 && m_ViewportHeight > 0) {
        // 按 z 从小到大排序（小的先画，大的后画 → 大的盖在上面）
        std::sort(uiSprites.begin(), uiSprites.end(),
                  [](const UISprite &a, const UISprite &b) { return a.z < b.z; });

        // 正交投影：像素坐标，左上角 (0,0)，右下角 (w,h)
        // Vulkan 屏幕原点在左上角，top=0, bottom=h 即 Y 轴向下
        float w = static_cast<float>(m_ViewportWidth);
        float h = static_cast<float>(m_ViewportHeight);
        glm::mat4 uiProjection = glm::ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f);

        r2d.BeginScene(glm::mat4(1.0f), uiProjection, false, glm::vec4(-1.0f));
        for (auto &sp : uiSprites) {
            r2d.DrawSprite(
                sp.tc->GetTransform(),
                sp.sc->SpriteTexture,
                sp.sc->Color
            );
        }
        r2d.EndScene();
    }
}


void Scene::OnViewportResize(const uint32_t width, const uint32_t height) {
    m_ViewportWidth = width;
    m_ViewportHeight = height;
}


template <typename T>
void Scene::OnComponentAdded(Entity entity, T &component) {
    static_assert(false);
}

template <>
void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent &component) {
}


template <>
void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent &component) {
}

template <>
void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent &component) {
}

template <>
void Scene::OnComponentAdded<ScriptComponent>(Entity entity, ScriptComponent &component) {
}

template <>
void Scene::OnComponentAdded<MeshComponent>(Entity entity, MeshComponent &component) {
}

template <>
void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent &component) {
}

template <>
void Scene::OnComponentAdded<PointLightComponent>(Entity entity, PointLightComponent &component) {
}


} //
// Created by Lenovo on 2026/5/9.
