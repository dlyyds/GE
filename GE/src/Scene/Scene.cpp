#include "pch.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include  "Scene/Entity.h"


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


void Scene::OnUpdateRuntime(const Timestep ts) {
    //update Script
    {
        m_Registry.view<NativeScriptComponent>().each([&](auto entity, NativeScriptComponent &nsc) {
            if (!nsc.Instance) {
                nsc.Instance = nsc.InstantiateScript();
                nsc.Instance->m_Entity = {entity, this};
                nsc.Instance->OnCreate();
            }
            nsc.Instance->OnUpdate(ts);
        });
    }

    const Camera *mainCamera = nullptr;
    glm::mat4 cameraTransform;

    const auto cameraView = m_Registry.view<TransformComponent, CameraComponent>();
    for (auto [entity, transform, camera] : cameraView.each()) {
        if (camera.Primary) {
            mainCamera = &camera.Camera;
            cameraTransform = transform.GetTransform();
            break;
        }
    }

    static bool warnOnce = false;
    if (!mainCamera) {
        if (!warnOnce) {
            GE_WARN("mianCamera not find!");
            warnOnce = true;
        }
        return;
    }
    warnOnce = false;

    Renderer::BeginScene(*mainCamera, cameraTransform);

    const auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
    for (const auto entity : group) {
        const auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

        Renderer::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(entity));
    }

    Renderer::EndScene();

}

void Scene::OnUpdateEditor(Timestep ts, EditorCamera &camera) {
    Renderer::BeginScene(camera);

    auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
    for (auto entity : group) {
        auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

        Renderer::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(entity));
    }

    Renderer::EndScene();
}


void Scene::OnViewportResize(const uint32_t width, const uint32_t height) {
    m_ViewportWidth = width;
    m_ViewportHeight = height;

    const auto view = m_Registry.view<CameraComponent>();
    for (auto [entity, cameraComponent] : view.each()) {
        if (!cameraComponent.FixedAspectRatio) {
            cameraComponent.Camera.SetViewportSize(width, height);
        }
    }
}

Entity Scene::GetPrimaryCameraEntity() {
    auto view = m_Registry.view<CameraComponent>();
    for (auto entity : view) {
        const auto &camera = view.get<CameraComponent>(entity);
        if (camera.Primary)
            return Entity{entity, this};
    }
    return {};
}


template <typename T>
void Scene::OnComponentAdded(Entity entity, T &component) {
    static_assert(false);
}

template <>
void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent &component) {
}

template <>
void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent &component) {
    if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
        component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
}

template <>
void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent &component) {
}

template <>
void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent &component) {
}

template <>
void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent &component) {
}

} //
// Created by Lenovo on 2026/5/9.
//