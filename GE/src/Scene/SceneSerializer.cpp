/**
 * @file SceneSerializer.cpp
 * @brief 场景序列化器实现 —— 基于 yaml-cpp 的 YAML 格式序列化/反序列化。
 *
 * 序列化流程：遍历 entt registry 中的每个实体，逐个写出其所有组件的数据。
 * 反序列化流程：解析 YAML，逐个实体创建并添加组件，按需加载纹理和网格资源。
 */

#include "Scene/SceneSerializer.h"

#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Render/Texture.h"
#include "Render/Mesh.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanResourceCache.h"
#include "Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <glm/glm.hpp>

#include <fstream>
#include <sstream>
#include <filesystem>

namespace GE {

namespace {

// ============================================================
// YAML 转换辅助函数（glm 向量 → YAML Node）
// ============================================================

YAML::Node SerializeVec2(const glm::vec2 &v) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node.push_back(v.x);
    node.push_back(v.y);
    return node;
}

YAML::Node SerializeVec3(const glm::vec3 &v) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node.push_back(v.x);
    node.push_back(v.y);
    node.push_back(v.z);
    return node;
}

YAML::Node SerializeVec4(const glm::vec4 &v) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node.push_back(v.x);
    node.push_back(v.y);
    node.push_back(v.z);
    node.push_back(v.w);
    return node;
}

// ============================================================
// YAML → glm 向量解析
// ============================================================

glm::vec2 DeserializeVec2(const YAML::Node &node, const glm::vec2 &def = {0.0f, 0.0f}) {
    if (!node || !node.IsSequence() || node.size() < 2) {
        return def;
    }
    return {node[0].as<float>(def.x), node[1].as<float>(def.y)};
}

glm::vec3 DeserializeVec3(const YAML::Node &node, const glm::vec3 &def = {0.0f, 0.0f, 0.0f}) {
    if (!node || !node.IsSequence() || node.size() < 3) {
        return def;
    }
    return {
        node[0].as<float>(def.x),
        node[1].as<float>(def.y),
        node[2].as<float>(def.z)
    };
}

glm::vec4 DeserializeVec4(const YAML::Node &node, const glm::vec4 &def = {0.0f, 0.0f, 0.0f, 0.0f}) {
    if (!node || !node.IsSequence() || node.size() < 4) {
        return def;
    }
    return {
        node[0].as<float>(def.x),
        node[1].as<float>(def.y),
        node[2].as<float>(def.z),
        node[3].as<float>(def.w)
    };
}

} // anonymous namespace

// ============================================================
// 构造 / 析构
// ============================================================

SceneSerializer::SceneSerializer(Scene *scene,
                                 VulkanDevice *device,
                                 VulkanResourceCache *cache)
    : m_Scene(scene), m_Device(device), m_ResourceCache(cache) {
}

SceneSerializer::~SceneSerializer() {
    // unique_ptr 自动清理资源
}

// ============================================================
// 资源加载辅助
// ============================================================

Texture *SceneSerializer::GetOrLoadTexture(const std::string &filepath) {
    if (filepath.empty()) {
        return nullptr;
    }

    // 先查缓存
    auto it = m_LoadedTextures.find(filepath);
    if (it != m_LoadedTextures.end()) {
        return it->second.get();
    }

    // 需要设备和缓存才能加载
    if (!m_Device || !m_ResourceCache) {
        GE_CORE_WARN("SceneSerializer: 无法加载纹理 {0}（未提供 VulkanDevice/ResourceCache）", filepath);
        return nullptr;
    }

    auto tex = Texture::LoadFromFile(*m_Device, *m_ResourceCache, filepath);
    if (!tex) {
        GE_CORE_WARN("SceneSerializer: 纹理加载失败: {0}", filepath);
        return nullptr;
    }

    Texture *raw = tex.get();
    m_LoadedTextures[filepath] = std::move(tex);
    return raw;
}

Mesh *SceneSerializer::GetOrLoadMesh(const std::string &filepath) {
    if (filepath.empty()) {
        return nullptr;
    }

    auto it = m_LoadedMeshes.find(filepath);
    if (it != m_LoadedMeshes.end()) {
        return it->second.get();
    }

    if (!m_Device) {
        GE_CORE_WARN("SceneSerializer: 无法加载网格 {0}（未提供 VulkanDevice）", filepath);
        return nullptr;
    }

    auto mesh = Mesh::LoadFromFile(*m_Device, filepath);
    if (!mesh) {
        GE_CORE_WARN("SceneSerializer: 网格加载失败: {0}", filepath);
        return nullptr;
    }

    Mesh *raw = mesh.get();
    m_LoadedMeshes[filepath] = std::move(mesh);
    return raw;
}

void SceneSerializer::ClearLoadedResources() {
    m_LoadedTextures.clear();
    m_LoadedMeshes.clear();
}

// ============================================================
// 序列化
// ============================================================

bool SceneSerializer::Serialize(const std::string &filepath) {
    if (!m_Scene) {
        GE_CORE_ERROR("SceneSerializer::Serialize: 场景指针为空");
        return false;
    }

    YAML::Node root;
    YAML::Node sceneNode = root["Scene"];
    YAML::Node entitiesNode = sceneNode["Entities"];
    entitiesNode.SetStyle(YAML::EmitterStyle::Block);

    auto &reg = m_Scene->Reg();
    auto view = reg.view<entt::entity>();

    size_t entityCount = 0;

    for (auto entityHandle : view) {
        entityCount++;
        Entity entity(entityHandle, m_Scene);

        YAML::Node entityNode;
        entityNode.SetStyle(YAML::EmitterStyle::Block);

        // ---- TagComponent ----
        if (entity.HasComponent<TagComponent>()) {
            const auto &tc = entity.GetComponent<TagComponent>();
            entityNode["Name"] = tc.Tag;
        } else {
            entityNode["Name"] = "Entity";
        }

        // ---- TransformComponent ----
        if (entity.HasComponent<TransformComponent>()) {
            const auto &tc = entity.GetComponent<TransformComponent>();
            YAML::Node transformNode = entityNode["Transform"];
            transformNode["Translation"] = SerializeVec3(tc.Translation);
            transformNode["Rotation"] = SerializeVec3(tc.Rotation);
            transformNode["Scale"] = SerializeVec3(tc.Scale);
        }

        // ---- SpriteRendererComponent ----
        if (entity.HasComponent<SpriteRendererComponent>()) {
            const auto &src = entity.GetComponent<SpriteRendererComponent>();
            YAML::Node spriteNode = entityNode["SpriteRenderer"];
            spriteNode["Color"] = SerializeVec4(src.Color);
            spriteNode["IsUI"] = src.IsUI;

            // 纹理路径
            if (src.SpriteTexture && !src.SpriteTexture->GetFilePath().empty()) {
                spriteNode["Texture"] = src.SpriteTexture->GetFilePath();
            }
        }

        // ---- MeshComponent ----
        if (entity.HasComponent<MeshComponent>()) {
            const auto &mc = entity.GetComponent<MeshComponent>();
            YAML::Node meshNode = entityNode["MeshRenderer"];
            meshNode["Color"] = SerializeVec4(mc.Color);

            // 网格路径
            if (mc.MeshPtr && !mc.MeshPtr->GetFilePath().empty()) {
                meshNode["Mesh"] = mc.MeshPtr->GetFilePath();
            }

            // 纹理路径
            if (mc.BaseTexture && !mc.BaseTexture->GetFilePath().empty()) {
                meshNode["Texture"] = mc.BaseTexture->GetFilePath();
            }
        }

        // ---- CameraComponent ----
        if (entity.HasComponent<CameraComponent>()) {
            const auto &cc = entity.GetComponent<CameraComponent>();
            const Camera &cam = cc.CameraInstance;

            YAML::Node cameraNode = entityNode["Camera"];
            cameraNode["Primary"] = cc.Primary;
            cameraNode["FixedAspectRatio"] = cc.FixedAspectRatio;

            // 模式
            cameraNode["Mode"] = (cam.GetMode() == Camera::Mode::Orbit) ? "Orbit" : "FPS";

            // 投影参数
            cameraNode["Fov"] = cam.GetFov();
            cameraNode["Aspect"] = cam.GetAspect();

            // 近远裁剪面：Camera 类未直接暴露，通过 SetPerspective 间接设置；
            // 我们用默认值保存（Camera 构造时 Near=0.1, Far=100）
            // 注意：如果 Camera 类以后添加 GetNear/GetFar 方法，这里需要更新
            cameraNode["Near"] = 0.1f;
            cameraNode["Far"] = 100.0f;

            // Orbit 模式参数
            cameraNode["Target"] = SerializeVec3(cam.GetTarget());
            cameraNode["Theta"] = cam.GetTheta();
            cameraNode["Phi"] = cam.GetPhi();
            cameraNode["Distance"] = cam.GetDistance();

            // FPS 模式参数
            cameraNode["Position"] = SerializeVec3(cam.GetPosition());
            cameraNode["Yaw"] = cam.GetYaw();
            cameraNode["Pitch"] = cam.GetPitch();
        }

        // ---- PointLightComponent ----
        if (entity.HasComponent<PointLightComponent>()) {
            const auto &plc = entity.GetComponent<PointLightComponent>();
            YAML::Node lightNode = entityNode["PointLight"];
            lightNode["Color"] = SerializeVec4(plc.Color);
            lightNode["RadiusInv"] = plc.RadiusInv;
        }

        // ---- ScriptComponent ----
        // 不序列化：std::function 无法持久化

        entitiesNode.push_back(entityNode);
    }

    // 写入文件
    try {
        std::ofstream fout(filepath);
        if (!fout.is_open()) {
            GE_CORE_ERROR("SceneSerializer::Serialize: 无法打开文件写入: {0}", filepath);
            return false;
        }

        YAML::Emitter emitter;
        emitter << root;
        fout << emitter.c_str();
        fout.close();

        GE_CORE_INFO("SceneSerializer: 场景已保存到 {}（{} 个实体）",
                     filepath, entityCount);
        return true;
    } catch (const std::exception &e) {
        GE_CORE_ERROR("SceneSerializer::Serialize: 写入文件异常: {0}", e.what());
        return false;
    }
}

// ============================================================
// 反序列化
// ============================================================

bool SceneSerializer::Deserialize(const std::string &filepath) {
    if (!m_Scene) {
        GE_CORE_ERROR("SceneSerializer::Deserialize: 场景指针为空");
        return false;
    }

    // 读取并解析 YAML
    YAML::Node root;
    try {
        root = YAML::LoadFile(filepath);
    } catch (const YAML::Exception &e) {
        GE_CORE_ERROR("SceneSerializer::Deserialize: YAML 解析失败 ({0}): {1}", filepath, e.what());
        return false;
    } catch (const std::exception &e) {
        GE_CORE_ERROR("SceneSerializer::Deserialize: 读取文件失败 ({0}): {1}", filepath, e.what());
        return false;
    }

    YAML::Node sceneNode = root["Scene"];
    if (!sceneNode) {
        GE_CORE_ERROR("SceneSerializer::Deserialize: 文件格式错误，缺少 Scene 节点: {0}", filepath);
        return false;
    }

    YAML::Node entitiesNode = sceneNode["Entities"];

    // 清空当前场景中的所有实体（无论文件中是否有实体）
    auto &reg = m_Scene->Reg();
    reg.clear();
    ClearLoadedResources();

    if (!entitiesNode || !entitiesNode.IsSequence()) {
        GE_CORE_WARN("SceneSerializer::Deserialize: 场景中没有 Entities 节点，将加载为空场景");
        return true;
    }

    uint32_t entityCount = 0;

    for (const auto &entityNode : entitiesNode) {
        // ---- 实体名称 ----
        std::string name = "Entity";
        if (entityNode["Name"]) {
            name = entityNode["Name"].as<std::string>("Entity");
        }

        Entity entity = m_Scene->CreateEntity(name);

        // ---- TransformComponent ----
        // 注意：CreateEntity 已经添加了 TransformComponent，这里只需修改值
        if (entityNode["Transform"]) {
            auto &tc = entity.GetComponent<TransformComponent>();
            YAML::Node transformNode = entityNode["Transform"];
            tc.Translation = DeserializeVec3(transformNode["Translation"], {0.0f, 0.0f, 0.0f});
            tc.Rotation = DeserializeVec3(transformNode["Rotation"], {0.0f, 0.0f, 0.0f});
            tc.Scale = DeserializeVec3(transformNode["Scale"], {1.0f, 1.0f, 1.0f});
        }

        // ---- SpriteRendererComponent ----
        if (entityNode["SpriteRenderer"]) {
            YAML::Node spriteNode = entityNode["SpriteRenderer"];
            auto &src = entity.AddComponent<SpriteRendererComponent>();

            src.Color = DeserializeVec4(spriteNode["Color"], {1.0f, 1.0f, 1.0f, 1.0f});
            src.IsUI = spriteNode["IsUI"] ? spriteNode["IsUI"].as<bool>(false) : false;

            // 纹理路径（如果有 Texture 字段，尝试加载）
            if (spriteNode["Texture"]) {
                std::string texPath = spriteNode["Texture"].as<std::string>("");
                src.SpriteTexture = GetOrLoadTexture(texPath);
            }
        }

        // ---- MeshComponent ----
        if (entityNode["MeshRenderer"]) {
            YAML::Node meshNode = entityNode["MeshRenderer"];
            auto &mc = entity.AddComponent<MeshComponent>();

            mc.Color = DeserializeVec4(meshNode["Color"], {1.0f, 1.0f, 1.0f, 1.0f});

            // 网格路径
            if (meshNode["Mesh"]) {
                std::string meshPath = meshNode["Mesh"].as<std::string>("");
                mc.MeshPtr = GetOrLoadMesh(meshPath);
            }

            // 纹理路径
            if (meshNode["Texture"]) {
                std::string texPath = meshNode["Texture"].as<std::string>("");
                mc.BaseTexture = GetOrLoadTexture(texPath);
            }
        }

        // ---- CameraComponent ----
        if (entityNode["Camera"]) {
            YAML::Node cameraNode = entityNode["Camera"];
            auto &cc = entity.AddComponent<CameraComponent>();

            cc.Primary = cameraNode["Primary"] ? cameraNode["Primary"].as<bool>(true) : true;
            cc.FixedAspectRatio = cameraNode["FixedAspectRatio"]
                                      ? cameraNode["FixedAspectRatio"].as<bool>(false)
                                      : false;

            Camera &cam = cc.CameraInstance;

            // 投影参数
            float fov = cameraNode["Fov"] ? cameraNode["Fov"].as<float>(45.0f) : 45.0f;
            float aspect = cameraNode["Aspect"]
                               ? cameraNode["Aspect"].as<float>(16.0f / 9.0f)
                               : 16.0f / 9.0f;
            float nearPlane = cameraNode["Near"] ? cameraNode["Near"].as<float>(0.1f) : 0.1f;
            float farPlane = cameraNode["Far"] ? cameraNode["Far"].as<float>(100.0f) : 100.0f;
            cam.SetPerspective(fov, aspect, nearPlane, farPlane);

            // 模式
            std::string modeStr = cameraNode["Mode"] ? cameraNode["Mode"].as<std::string>("Orbit") : "Orbit";
            Camera::Mode mode = (modeStr == "FPS") ? Camera::Mode::FPS : Camera::Mode::Orbit;
            cam.SetMode(mode);

            // Orbit 模式参数
            if (cameraNode["Target"]) {
                cam.SetTarget(DeserializeVec3(cameraNode["Target"], {0.0f, 0.0f, 0.0f}));
            }
            float theta = cameraNode["Theta"] ? cameraNode["Theta"].as<float>(0.0f) : 0.0f;
            float phi = cameraNode["Phi"] ? cameraNode["Phi"].as<float>(0.0f) : 0.0f;
            float distance = cameraNode["Distance"] ? cameraNode["Distance"].as<float>(2.0f) : 2.0f;
            cam.SetOrbit(theta, phi, distance);

            // FPS 模式参数
            if (cameraNode["Position"]) {
                cam.SetPosition(DeserializeVec3(cameraNode["Position"], {0.0f, 0.0f, 2.0f}));
            }
            float yaw = cameraNode["Yaw"] ? cameraNode["Yaw"].as<float>(0.0f) : 0.0f;
            float pitch = cameraNode["Pitch"] ? cameraNode["Pitch"].as<float>(0.0f) : 0.0f;
            cam.SetYawPitch(yaw, pitch);
        }

        // ---- PointLightComponent ----
        if (entityNode["PointLight"]) {
            YAML::Node lightNode = entityNode["PointLight"];
            auto &plc = entity.AddComponent<PointLightComponent>();

            plc.Color = DeserializeVec4(lightNode["Color"], {1.0f, 1.0f, 1.0f, 1.0f});
            plc.RadiusInv = lightNode["RadiusInv"] ? lightNode["RadiusInv"].as<float>(0.5f) : 0.5f;
        }

        // ---- ScriptComponent ----
        // 不反序列化：无法恢复回调函数

        entityCount++;
    }

    GE_CORE_INFO("SceneSerializer: 场景已从 {} 加载（{} 个实体，{} 个纹理，{} 个网格）",
                 filepath, entityCount,
                 m_LoadedTextures.size(), m_LoadedMeshes.size());

    return true;
}

} // namespace GE
