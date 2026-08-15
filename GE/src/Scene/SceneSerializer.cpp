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
#include "Render/Material.h"
#include "Render/Mesh.h"
#include "Render/Renderer.h"
#include "Render/MeshManager.h"
#include "Render/MaterialManager.h"
#include "Render/AssetManager.h"
#include "Core/Log.h"
#include "Render/TextureManager.h"

#include <yaml-cpp/yaml.h>
#include <glm/glm.hpp>

#include <fstream>
#include <sstream>
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace GE {

namespace {

// ============================================================
// 采样器参数辅助
// ============================================================

/// 采样器寻址模式 → 字符串
const char *AddressModeToString(vk::SamplerAddressMode mode) {
    switch (mode) {
    case vk::SamplerAddressMode::eMirroredRepeat: return "MirroredRepeat";
    case vk::SamplerAddressMode::eClampToEdge:    return "ClampToEdge";
    case vk::SamplerAddressMode::eClampToBorder:  return "ClampToBorder";
    default:                                      return "Repeat";
    }
}

/// 字符串 → 采样器寻址模式
vk::SamplerAddressMode AddressModeFromString(const std::string &s) {
    if (s == "MirroredRepeat") return vk::SamplerAddressMode::eMirroredRepeat;
    if (s == "ClampToEdge")    return vk::SamplerAddressMode::eClampToEdge;
    if (s == "ClampToBorder")  return vk::SamplerAddressMode::eClampToBorder;
    return vk::SamplerAddressMode::eRepeat;
}

/// 纹素过滤器 → 字符串
const char *FilterToString(vk::Filter f) {
    return (f == vk::Filter::eNearest) ? "Nearest" : "Linear";
}

/// 字符串 → 纹素过滤器
vk::Filter FilterFromString(const std::string &s) {
    return (s == "Nearest") ? vk::Filter::eNearest : vk::Filter::eLinear;
}

/**
 * @brief 将纹理采样器参数写入 YAML 节点。
 *
 * 保存过滤方式（Mag/MinFilter）、寻址模式（AddressMode）和各向异性开关，
 * 供反序列化时恢复。纹理指针为空时为空操作。
 */
void SerializeSamplerNode(YAML::Node &samplerNode, Texture *tex) {
    if (!tex) {
        return;
    }
    samplerNode["MagFilter"]   = FilterToString(tex->GetMagFilter());
    samplerNode["MinFilter"]   = FilterToString(tex->GetMinFilter());
    samplerNode["AddressMode"] = AddressModeToString(tex->GetAddressMode());
    samplerNode["Anisotropy"]  = tex->GetAnisotropyEnabled();
}

/**
 * @brief 将采样器参数应用到纹理（反序列化恢复）。
 *
 * 纹理通过 TextureManager 按路径加载，默认采样参数为线性过滤 + 重复寻址；
 * 这里按 YAML 中保存的参数调用 Set* 便捷方法重建采样器。
 * 纹理指针为空或节点缺失时为空操作。
 */
void ApplySamplerParams(Texture *tex, const YAML::Node &samplerNode) {
    if (!tex || !samplerNode) {
        return;
    }

    if (samplerNode["MagFilter"] && samplerNode["MinFilter"]) {
        vk::Filter mag = FilterFromString(samplerNode["MagFilter"].as<std::string>("Linear"));
        vk::Filter min = FilterFromString(samplerNode["MinFilter"].as<std::string>("Linear"));
        tex->SetFilter(mag, min);
    }
    if (samplerNode["AddressMode"]) {
        tex->SetAddressMode(AddressModeFromString(samplerNode["AddressMode"].as<std::string>("Repeat")));
    }
    if (samplerNode["Anisotropy"]) {
        tex->SetAnisotropy(samplerNode["Anisotropy"].as<bool>(false));
    }
}

// ============================================================
// 材质辅助（MeshRenderer 子网格材质覆写序列化）
// ============================================================

/// 材质纹理槽位名（与 Material::TextureSlot 顺序一一对应）
const char *kTextureSlotNames[] = {"Albedo", "Normal", "Emissive", "MetallicRoughness"};

/// 材质纹理槽位名 → 槽位枚举（用于反序列化）
Material::TextureSlot TextureSlotFromName(const std::string &name) {
    if (name == "Normal") return Material::Normal;
    if (name == "Emissive") return Material::Emissive;
    if (name == "MetallicRoughness") return Material::MetallicRoughness;
    return Material::Albedo;
}

/**
 * @brief 将材质写入 YAML 节点（纹理槽位 + 浮点参数）。
 *
 * 纹理以文件路径写入；浮点参数以 name → value 的 map 写入 FloatParams 节点。
 */
void SerializeMaterialNode(YAML::Node &matNode, Material *mat) {
    if (!mat) {
        return;
    }

    // 材质类型（BlinnPhong / PBR），决定渲染管线
    matNode["Type"] = (mat->GetType() == Material::Type::PBR) ? "PBR" : "BlinnPhong";

    // 纹理槽位（仅写有纹理且带文件路径的槽位）
    for (int s = 0; s < Material::Count; ++s) {
        auto slot = static_cast<Material::TextureSlot>(s);
        Texture *tex = mat->GetTexture(slot);
        if (tex && !tex->GetFilePath().empty()) {
            std::string texKey = std::string(kTextureSlotNames[s]) + "Texture";
            matNode[texKey] = tex->GetFilePath();
            YAML::Node samplerNode = matNode[texKey + "Sampler"];
            SerializeSamplerNode(samplerNode, tex);
        }
    }

    // 浮点参数（如 shininess、specularStrength、pbr 系数）
    const auto &params = mat->GetFloatParams();
    if (!params.empty()) {
        YAML::Node fp = matNode["FloatParams"];
        for (const auto &kv : params) {
            fp[kv.first] = kv.second;
        }
    }
}

/**
 * @brief 从材质 YAML 节点创建材质并注册到 MaterialManager。
 *
 * 按内容生成 key（纹理路径 + 类型 + 浮点参数），内容相同的材质复用同一实例。
 * 用于 MeshRenderer 子网格材质覆写的反序列化。
 *
 * @param matNode 材质节点
 * @return 材质指针
 */
Material *DeserializeMaterialNode(const YAML::Node &matNode) {
    auto &matMgr = Renderer::GetMaterialManager();

    // 构建内容 key（类型 + 纹理 + 参数拼接），用于去重
    std::string key;
    key += matNode["Type"] ? matNode["Type"].as<std::string>() : "BlinnPhong";
    key += ";";
    for (auto name : kTextureSlotNames) {
        std::string texKey = std::string(name) + "Texture";
        if (matNode[texKey]) {
            key += std::string(name) + ":" + matNode[texKey].as<std::string>() + ";";
        }
    }
    if (matNode["FloatParams"]) {
        std::vector<std::string> names;
        for (const auto &it : matNode["FloatParams"]) {
            names.push_back(it.first.as<std::string>());
        }
        std::sort(names.begin(), names.end());
        for (const auto &n : names) {
            key += n + "=" + matNode["FloatParams"][n].as<std::string>() + ";";
        }
    }
    const std::string fullKey = "scene:" + key;

    if (Material *existing = matMgr.Get(fullKey)) {
        return existing;
    }

    auto mat = std::make_unique<Material>();
    mat->SetDebugName(fullKey);

    if (matNode["Type"] && matNode["Type"].as<std::string>() == "PBR") {
        mat->SetType(Material::Type::PBR);
    }

    for (auto name : kTextureSlotNames) {
        std::string texKey = std::string(name) + "Texture";
        if (matNode[texKey]) {
            std::string path = matNode[texKey].as<std::string>("");
            if (Texture *tex = Renderer::GetAssetManager().LoadTexture(path)) {
                ApplySamplerParams(tex, matNode[texKey + "Sampler"]);
                mat->SetTexture(TextureSlotFromName(name), tex);
            } else {
                GE_CORE_WARN("SceneSerializer: 材质纹理加载失败: {0}", path);
            }
        }
    }

    if (matNode["FloatParams"]) {
        for (const auto &it : matNode["FloatParams"]) {
            mat->SetFloat(it.first.as<std::string>(), it.second.as<float>());
        }
    }

    return matMgr.Register(fullKey, std::move(mat));
}

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

SceneSerializer::SceneSerializer(Scene *scene)
    : m_Scene(scene) {
}

SceneSerializer::~SceneSerializer() {
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

            // 纹理路径 + 采样器参数
            if (src.SpriteTexture && !src.SpriteTexture->GetFilePath().empty()) {
                spriteNode["Texture"] = src.SpriteTexture->GetFilePath();
                // 同时保存采样器参数，供反序列化恢复
                YAML::Node samplerNode = spriteNode["TextureSampler"];
                SerializeSamplerNode(samplerNode, src.SpriteTexture);
            }
        }

        // ---- MeshRendererComponent ----
        if (entity.HasComponent<MeshRendererComponent>()) {
            const auto &mc = entity.GetComponent<MeshRendererComponent>();
            YAML::Node meshNode = entityNode["MeshRenderer"];
            meshNode["Color"] = SerializeVec4(mc.Color);

            // 网格路径
            if (mc.MeshPtr && !mc.MeshPtr->GetFilePath().empty()) {
                meshNode["Mesh"] = mc.MeshPtr->GetFilePath();
            }

            // 子网格材质覆写表（每实体独立）：<子网格索引, 材质内容>
            if (!mc.materialOverrides.empty()) {
                YAML::Node overridesNode = meshNode["MaterialOverrides"];
                for (const auto &kv : mc.materialOverrides) {
                    YAML::Node ovNode = overridesNode[std::to_string(kv.first)];
                    SerializeMaterialNode(ovNode, kv.second);
                }
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

        // ---- DirectionalLightComponent ----
        if (entity.HasComponent<DirectionalLightComponent>()) {
            const auto &dlc = entity.GetComponent<DirectionalLightComponent>();
            YAML::Node lightNode = entityNode["DirectionalLight"];
            lightNode["Color"] = SerializeVec4(dlc.Color);
        }

        // ---- AmbientLightComponent ----
        if (entity.HasComponent<AmbientLightComponent>()) {
            const auto &alc = entity.GetComponent<AmbientLightComponent>();
            YAML::Node lightNode = entityNode["AmbientLight"];
            lightNode["Color"] = SerializeVec4(alc.Color);
        }

        // ---- EnvironmentComponent ----
        if (entity.HasComponent<EnvironmentComponent>()) {
            const auto &ec = entity.GetComponent<EnvironmentComponent>();
            YAML::Node envNode = entityNode["Environment"];
            envNode["Name"] = ec.Name;
            envNode["SkyboxEnabled"] = ec.SkyboxEnabled;
        }

        // ---- RigidBodyComponent ----
        if (entity.HasComponent<RigidBodyComponent>()) {
            const auto &rbc = entity.GetComponent<RigidBodyComponent>();
            YAML::Node rbNode = entityNode["RigidBody"];

            std::string typeStr;
            switch (rbc.Type) {
            case Physics::RigidBodyType::Static: typeStr = "Static";
                break;
            case Physics::RigidBodyType::Kinematic: typeStr = "Kinematic";
                break;
            case Physics::RigidBodyType::Dynamic: typeStr = "Dynamic";
                break;
            }
            rbNode["Type"] = typeStr;
            rbNode["Mass"] = rbc.Mass;
            rbNode["Friction"] = rbc.Friction;
            rbNode["Restitution"] = rbc.Restitution;
            rbNode["LinearDamping"] = rbc.LinearDamping;
            rbNode["AngularDamping"] = rbc.AngularDamping;
            rbNode["IsSensor"] = rbc.IsSensor;
        }

        // ---- BoxColliderComponent ----
        if (entity.HasComponent<BoxColliderComponent>()) {
            const auto &bcc = entity.GetComponent<BoxColliderComponent>();
            YAML::Node boxNode = entityNode["BoxCollider"];
            boxNode["HalfExtents"] = SerializeVec3(bcc.HalfExtents);
            boxNode["Offset"] = SerializeVec3(bcc.Offset);
        }

        // ---- SphereColliderComponent ----
        if (entity.HasComponent<SphereColliderComponent>()) {
            const auto &scc = entity.GetComponent<SphereColliderComponent>();
            YAML::Node sphereNode = entityNode["SphereCollider"];
            sphereNode["Radius"] = scc.Radius;
            sphereNode["Offset"] = SerializeVec3(scc.Offset);
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
                src.SpriteTexture = Renderer::GetAssetManager().LoadTexture(texPath);
                // 恢复采样器参数（若保存了）
                ApplySamplerParams(src.SpriteTexture, spriteNode["TextureSampler"]);
            }
        }

        // ---- MeshRendererComponent ----
        if (entityNode["MeshRenderer"]) {
            YAML::Node meshNode = entityNode["MeshRenderer"];
            auto &mc = entity.AddComponent<MeshRendererComponent>();

            mc.Color = DeserializeVec4(meshNode["Color"], {1.0f, 1.0f, 1.0f, 1.0f});

            // 网格路径（通过全局 MeshManager 加载 / 去重，Serializer 不持有所有权）
            if (meshNode["Mesh"]) {
                std::string meshPath = meshNode["Mesh"].as<std::string>("");
                mc.MeshPtr = Renderer::GetAssetManager().LoadMesh(meshPath);
            }

            // 子网格材质覆写表：重建材质并写入组件（每实体独立）
            if (meshNode["MaterialOverrides"]) {
                for (const auto &kv : meshNode["MaterialOverrides"]) {
                    uint32_t index = static_cast<uint32_t>(std::stoul(kv.first.as<std::string>()));
                    if (Material *mat = DeserializeMaterialNode(kv.second)) {
                        mc.materialOverrides[index] = mat;
                    }
                }
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

        // ---- DirectionalLightComponent ----
        if (entityNode["DirectionalLight"]) {
            YAML::Node lightNode = entityNode["DirectionalLight"];
            auto &dlc = entity.AddComponent<DirectionalLightComponent>();

            dlc.Color = DeserializeVec4(lightNode["Color"], {1.0f, 1.0f, 1.0f, 1.0f});
        }

        // ---- AmbientLightComponent ----
        if (entityNode["AmbientLight"]) {
            YAML::Node lightNode = entityNode["AmbientLight"];
            auto &alc = entity.AddComponent<AmbientLightComponent>();

            alc.Color = DeserializeVec4(lightNode["Color"], {0.3f, 0.3f, 0.3f, 1.0f});
        }

        // ---- EnvironmentComponent ----
        if (entityNode["Environment"]) {
            YAML::Node envNode = entityNode["Environment"];
            auto &ec = entity.AddComponent<EnvironmentComponent>();

            ec.Name = envNode["Name"] ? envNode["Name"].as<std::string>() : std::string();
            ec.SkyboxEnabled = envNode["SkyboxEnabled"] ? envNode["SkyboxEnabled"].as<bool>(true) : true;
        }

        // ---- RigidBodyComponent ----
        if (entityNode["RigidBody"]) {
            YAML::Node rbNode = entityNode["RigidBody"];
            auto &rbc = entity.AddComponent<RigidBodyComponent>();

            std::string typeStr = rbNode["Type"] ? rbNode["Type"].as<std::string>("Static") : "Static";
            if (typeStr == "Kinematic")
                rbc.Type = Physics::RigidBodyType::Kinematic;
            else if (typeStr == "Dynamic")
                rbc.Type = Physics::RigidBodyType::Dynamic;
            else
                rbc.Type = Physics::RigidBodyType::Static;

            rbc.Mass = rbNode["Mass"] ? rbNode["Mass"].as<float>(1.0f) : 1.0f;
            rbc.Friction = rbNode["Friction"] ? rbNode["Friction"].as<float>(0.6f) : 0.6f;
            rbc.Restitution = rbNode["Restitution"] ? rbNode["Restitution"].as<float>(0.0f) : 0.0f;
            rbc.LinearDamping = rbNode["LinearDamping"] ? rbNode["LinearDamping"].as<float>(0.05f) : 0.05f;
            rbc.AngularDamping = rbNode["AngularDamping"] ? rbNode["AngularDamping"].as<float>(0.05f) : 0.05f;
            rbc.IsSensor = rbNode["IsSensor"] ? rbNode["IsSensor"].as<bool>(false) : false;
        }

        // ---- BoxColliderComponent ----
        if (entityNode["BoxCollider"]) {
            YAML::Node boxNode = entityNode["BoxCollider"];
            auto &bcc = entity.AddComponent<BoxColliderComponent>();

            bcc.HalfExtents = DeserializeVec3(boxNode["HalfExtents"], {0.5f, 0.5f, 0.5f});
            bcc.Offset = DeserializeVec3(boxNode["Offset"], {0.0f, 0.0f, 0.0f});
        }

        // ---- SphereColliderComponent ----
        if (entityNode["SphereCollider"]) {
            YAML::Node sphereNode = entityNode["SphereCollider"];
            auto &scc = entity.AddComponent<SphereColliderComponent>();

            scc.Radius = sphereNode["Radius"] ? sphereNode["Radius"].as<float>(0.5f) : 0.5f;
            scc.Offset = DeserializeVec3(sphereNode["Offset"], {0.0f, 0.0f, 0.0f});
        }

        // ---- ScriptComponent ----
        // 不反序列化：无法恢复回调函数

        entityCount++;
    }

    GE_CORE_INFO("SceneSerializer: 场景已从 {} 加载（{} 个实体）",
                 filepath, entityCount);

    return true;
}

} // namespace GE
