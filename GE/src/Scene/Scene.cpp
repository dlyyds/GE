#include "pch.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/Entity.h"
#include "Render/Renderer.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"
#include "Render/Mesh.h"

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
            if (sc.OnUpdate) {
                Entity entity{entityHandle, this};
                sc.OnUpdate(ts, entity);
            }
        }
    }

    // ── 2D 精灵渲染 ────────────────────────────────────────────────────
    auto &r2d = Renderer::Get2DRenderer();
    r2d.BeginScene(viewProjection, clearColor);

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
            if (sc.OnUpdate) {
                Entity entity{entityHandle, this};
                sc.OnUpdate(ts, entity);
            }
        }
    }

    // ── 3D 网格渲染 ────────────────────────────────────────────────────
    auto &r3d = Renderer::Get3DRenderer();
    r3d.BeginScene(view, projection, viewPos, clearColor);

    auto meshView = m_Registry.view<TransformComponent, MeshComponent>();
    for (auto entity : meshView) {
        auto &tc = meshView.get<TransformComponent>(entity);
        auto &mc = meshView.get<MeshComponent>(entity);

        if (mc.MeshPtr) {
            r3d.DrawMesh(
                tc.GetTransform(),
                mc.MeshPtr,
                nullptr,  // TODO: MeshComponent 暂未包含纹理字段
                mc.Color
            );
        }
    }

    r3d.EndScene();

    // ── 2D 精灵渲染（叠加在 3D 之上，不清屏） ──────────────────────────
    glm::mat4 viewProjection = projection * view;

    auto &r2d = Renderer::Get2DRenderer();
    r2d.BeginScene(viewProjection, glm::vec4(-1.0f));

    auto spriteView = m_Registry.view<TransformComponent, SpriteRendererComponent>();
    for (auto entity : spriteView) {
        auto &tc = spriteView.get<TransformComponent>(entity);
        auto &sc = spriteView.get<SpriteRendererComponent>(entity);

        r2d.DrawSprite(
            tc.GetTransform(),
            sc.SpriteTexture,
            sc.Color
        );
    }

    r2d.EndScene();
}


void Scene::OnViewportResize(const uint32_t width, const uint32_t height) {

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


} //
// Created by Lenovo on 2026/5/9.
