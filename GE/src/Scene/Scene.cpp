#include "pch.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/Entity.h"
#include "Events/Event.h"
#include "Events/ApplicationEvent.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"
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

    // ── 收集场景中的光源（方向光 / 环境光 / 点光源） ─────────────────
    {
        auto &lightParams = r3d.GetLightParams();

        // ---- 方向光：取场景中第一个方向光组件 ----
        {
            auto dirLightView = m_Registry.view<TransformComponent, DirectionalLightComponent>();
            if (dirLightView.begin() != dirLightView.end()) {
                auto entity = *dirLightView.begin();
                auto &tc = dirLightView.get<TransformComponent>(entity);
                auto &dlc = dirLightView.get<DirectionalLightComponent>(entity);

                // 由 Transform 的旋转推导出方向光方向（前向向量，-Z 轴旋转后为光线射出方向）
                // 着色器中 dirLightDirection 表示"指向光源的方向"（即从表面指向光源），
                // 与光线射出方向相反，因此取反
                glm::quat rot = glm::quat(tc.Rotation);
                glm::vec3 lightDir = rot * glm::vec3(0.0f, 0.0f, -1.0f);
                lightParams.dirLightDirection = glm::normalize(-lightDir);
                lightParams.dirLightColor = dlc.Color;
            } else {
                // 场景中无方向光组件时，使用默认值（斜向下的白色方向光）
                lightParams.dirLightDirection = {0.0f, -1.0f, 0.0f};
                lightParams.dirLightColor = {1.0f, 1.0f, 1.0f, 1.0f};
            }
        }

        // ---- 环境光：取场景中第一个环境光组件 ----
        {
            auto ambientView = m_Registry.view<AmbientLightComponent>();
            if (ambientView.begin() != ambientView.end()) {
                auto entity = *ambientView.begin();
                auto &alc = ambientView.get<AmbientLightComponent>(entity);
                lightParams.ambient = alc.Color;
            } else {
                // 场景中无环境光组件时，使用默认值保证可见性
                lightParams.ambient = {0.3f, 0.3f, 0.3f, 1.0f};
            }
        }

        // ---- 点光源 ----
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


void Scene::OnEvent(Event &e) {
    // ── 系统级事件 ──────────────────────────────────────────────────────
    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<WindowResizeEvent>([this](WindowResizeEvent &ev) {
        OnViewportResize(ev.GetWidth(), ev.GetHeight());
        return false;  // 不消费，事件继续向 Layer 上层传播
    });

    // ── 输入事件：分发给所有 ScriptComponent ───────────────────────────
    if (e.IsInCategory(EventCategoryInput)) {
        DispatchInputEventToScripts(e);
    }
}

void Scene::DispatchInputEventToScripts(Event &e) {
    auto view = m_Registry.view<ScriptComponent>();
    if (view.empty()) return;

    EventDispatcher dispatcher(e);

    // ---- 按键按下 ----
    dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent &ev) {
        for (auto handle : view) {
            auto &sc = view.get<ScriptComponent>(handle);
            if (!sc.Enabled || !sc.OnKeyPressed) continue;
            Entity entity{handle, this};
            if (sc.OnKeyPressed(entity, ev.GetKeyCode(), ev.GetRepeatCount())) {
                ev.Handled = true;
                return true;
            }
        }
        return false;
    });
    if (e.Handled) return;

    // ---- 按键释放 ----
    dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent &ev) {
        for (auto handle : view) {
            auto &sc = view.get<ScriptComponent>(handle);
            if (!sc.Enabled || !sc.OnKeyReleased) continue;
            Entity entity{handle, this};
            if (sc.OnKeyReleased(entity, ev.GetKeyCode(), 0)) {
                ev.Handled = true;
                return true;
            }
        }
        return false;
    });
    if (e.Handled) return;

    // ---- 鼠标按下 ----
    dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent &ev) {
        for (auto handle : view) {
            auto &sc = view.get<ScriptComponent>(handle);
            if (!sc.Enabled || !sc.OnMouseButtonPressed) continue;
            Entity entity{handle, this};
            if (sc.OnMouseButtonPressed(entity, ev.GetMouseButton())) {
                ev.Handled = true;
                return true;
            }
        }
        return false;
    });
    if (e.Handled) return;

    // ---- 鼠标释放 ----
    dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent &ev) {
        for (auto handle : view) {
            auto &sc = view.get<ScriptComponent>(handle);
            if (!sc.Enabled || !sc.OnMouseButtonReleased) continue;
            Entity entity{handle, this};
            if (sc.OnMouseButtonReleased(entity, ev.GetMouseButton())) {
                ev.Handled = true;
                return true;
            }
        }
        return false;
    });
    if (e.Handled) return;

    // ---- 鼠标移动（不消费事件） ----
    dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent &ev) {
        for (auto handle : view) {
            auto &sc = view.get<ScriptComponent>(handle);
            if (!sc.Enabled || !sc.OnMouseMoved) continue;
            Entity entity{handle, this};
            sc.OnMouseMoved(entity, ev.GetX(), ev.GetY());
        }
        return false;
    });
    if (e.Handled) return;

    // ---- 鼠标滚轮（不消费事件） ----
    dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent &ev) {
        for (auto handle : view) {
            auto &sc = view.get<ScriptComponent>(handle);
            if (!sc.Enabled || !sc.OnMouseScrolled) continue;
            Entity entity{handle, this};
            sc.OnMouseScrolled(entity, ev.GetXOffset(), ev.GetYOffset());
        }
        return false;
    });
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

template <>
void Scene::OnComponentAdded<DirectionalLightComponent>(Entity entity, DirectionalLightComponent &component) {
}

template <>
void Scene::OnComponentAdded<AmbientLightComponent>(Entity entity, AmbientLightComponent &component) {
}


} //
// Created by Lenovo on 2026/5/9.
