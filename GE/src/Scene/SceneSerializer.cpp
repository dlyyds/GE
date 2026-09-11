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
#include "Render/AssetPathUtil.h"
#include "Core/Log.h"
#include "Render/TextureManager.h"
#include "Render/GEMeshLoader.h"
#include "Render/ModelLoader.h"
#include "Render/GLTFLoader.h"
#include "Animation/AnimationClipManager.h"

#include <yaml-cpp/yaml.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fstream>
#include <sstream>
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace GE {

namespace {

// ============================================================
// 资产引用归一
// ============================================================

/**
 * @brief 把资产引用归一为规范形后再写入场景（相对资源根、正斜杠）。
 *
 * 归一失败（根外绝对路径 / 越界）时原样写入并告警——场景仍可正常保存，
 * 具体问题留给打包校验显式报错，避免在保存路径上直接失败而丢掉用户改动。
 *
 * @param raw 原始引用（"builtin:*" 伪键 / 相对资源根 / 绝对路径）
 * @return 应写入 YAML 的路径字符串
 */
std::string CanonicalAssetRef(const std::string &raw) {
    if (raw.empty()) {
        return raw;
    }
    if (auto canon = AssetPathUtil::ToCanonical(raw, Renderer::GetAssetManager().GetAssetRoot())) {
        return *canon;
    }
    GE_CORE_WARN("SceneSerializer: 资产引用无法归一为相对资源根路径，原样写入: {0}", raw);
    return raw;
}

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

// ============================================================
// Script public 字段辅助
// ============================================================

/// public 字段类型枚举 → 字符串标签（可读性，落盘用）
const char *ScriptFieldTypeName(ScriptFieldType type) {
    switch (type) {
    case ScriptFieldType::Number: return "number";
    case ScriptFieldType::Bool:   return "bool";
    case ScriptFieldType::String: return "string";
    default:                      return "none";
    }
}

/// 字符串标签 → public 字段类型枚举（未知回退 None）
ScriptFieldType ScriptFieldTypeFromName(const std::string &name) {
    if (name == "number") return ScriptFieldType::Number;
    if (name == "bool")   return ScriptFieldType::Bool;
    if (name == "string") return ScriptFieldType::String;
    return ScriptFieldType::None;
}

const char *AttenuationToString(Audio::AttenuationModel model) {
    switch (model) {
    case Audio::AttenuationModel::Linear:      return "Linear";
    case Audio::AttenuationModel::Exponential: return "Exponential";
    case Audio::AttenuationModel::None:        return "None";
    default:                                   return "Inverse";
    }
}

Audio::AttenuationModel AttenuationFromString(const std::string &s) {
    if (s == "Linear")      return Audio::AttenuationModel::Linear;
    if (s == "Exponential") return Audio::AttenuationModel::Exponential;
    if (s == "None")        return Audio::AttenuationModel::None;
    return Audio::AttenuationModel::Inverse;
}


// ============================================================
// 动画状态机 ASM 辅助（条件类型 / 比较符 ←→ 字符串标签）
// ============================================================

/// 条件类型枚举 → 字符串标签（可读性，落盘用）
const char *AnimConditionTypeName(AnimCondition::Type type) {
    switch (type) {
    case AnimCondition::Type::Bool:       return "Bool";
    case AnimCondition::Type::StateTime:  return "StateTime";
    case AnimCondition::Type::StateEnded: return "StateEnded";
    default:                              return "FloatCmp";
    }
}

/// 字符串标签 → 条件类型枚举（未知回退 FloatCmp）
AnimCondition::Type AnimConditionTypeFromName(const std::string &name) {
    if (name == "Bool")       return AnimCondition::Type::Bool;
    if (name == "StateTime")  return AnimCondition::Type::StateTime;
    if (name == "StateEnded") return AnimCondition::Type::StateEnded;
    return AnimCondition::Type::FloatCmp;
}

/// 比较符枚举 → 字符串标签（可读性，落盘用）
const char *AnimConditionCmpName(AnimCondition::Cmp cmp) {
    switch (cmp) {
    case AnimCondition::Cmp::GreaterEq: return "GreaterEq";
    case AnimCondition::Cmp::Less:      return "Less";
    case AnimCondition::Cmp::LessEq:    return "LessEq";
    case AnimCondition::Cmp::NearEq:    return "NearEq";
    case AnimCondition::Cmp::Not:       return "Not";
    default:                            return "Greater";
    }
}

/// 字符串标签 → 比较符枚举（未知回退 Greater）
AnimCondition::Cmp AnimConditionCmpFromName(const std::string &name) {
    if (name == "GreaterEq") return AnimCondition::Cmp::GreaterEq;
    if (name == "Less")      return AnimCondition::Cmp::Less;
    if (name == "LessEq")    return AnimCondition::Cmp::LessEq;
    if (name == "NearEq")    return AnimCondition::Cmp::NearEq;
    if (name == "Not")       return AnimCondition::Cmp::Not;
    return AnimCondition::Cmp::Greater;
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

// 前向声明：SerializeVec3 / DeserializeVec3 定义在本文件下方的
// "YAML 转换辅助函数"区，材质序列化函数先于其定义使用，需在此声明。
// （不带默认实参，定义处的默认值在调用点不可见，调用时显式传参。）
YAML::Node SerializeVec3(const glm::vec3 &v);
glm::vec3 DeserializeVec3(const YAML::Node &node, const glm::vec3 &def);

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

    // 材质显示名（m_Name），非空才写，保持序列化文件干净。
    // 以 "scene:" 开头的是反序列化时按内容生成的去重键（拼接了纹理路径），并非用户命名
    // 的显示名——写出去只会把机器绝对路径带进场景文件，且下次加载会重新生成，故跳过。
    const std::string &matName = mat->GetName();
    if (!matName.empty() && matName.rfind("scene:", 0) != 0) {
        matNode["Name"] = matName;
    }

    // 纹理槽位（仅写有纹理且带文件路径的槽位）
    for (int s = 0; s < Material::Count; ++s) {
        auto slot = static_cast<Material::TextureSlot>(s);
        Texture *tex = mat->GetTexture(slot);
        if (tex && !tex->GetFilePath().empty()) {
            std::string texKey = std::string(kTextureSlotNames[s]) + "Texture";
            matNode[texKey] = CanonicalAssetRef(tex->GetFilePath());
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

    // 自发光颜色因子（glTF emissiveFactor，乘自发光贴图颜色，两类型共用）
    matNode["EmissiveFactor"] = SerializeVec3(mat->GetEmissiveFactor());
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
    // 自发光颜色因子参与去重：因子不同的材质不复用同一实例
    if (matNode["EmissiveFactor"]) {
        key += "EmissiveFactor:";
        for (const auto &v : matNode["EmissiveFactor"]) {
            key += v.as<std::string>() + ",";
        }
        key += ";";
    }
    const std::string fullKey = "scene:" + key;

    if (Material *existing = matMgr.Get(fullKey)) {
        return existing;
    }

    auto mat = std::make_unique<Material>();
    // 显示名优先取序列化里的 Name，缺省退回内容 key（保持非空）
    if (matNode["Name"]) {
        mat->SetName(matNode["Name"].as<std::string>());
    } else {
        mat->SetName(fullKey);
    }

    if (matNode["Type"] && matNode["Type"].as<std::string>() == "PBR") {
        mat->SetType(Material::Type::PBR);
    }

    for (auto name : kTextureSlotNames) {
        std::string texKey = std::string(name) + "Texture";
        if (matNode[texKey]) {
            std::string path = matNode[texKey].as<std::string>("");
            // 异步加载：返回未就绪空壳，渲染端 IsReady() 门控降级默认纹理，
            // 就绪后自动亮相，避免反序列化场景时主线程阻塞在纹理解码/上传
            if (Texture *tex = Renderer::GetAssetManager().LoadTextureAsync(path)) {
                ApplySamplerParams(tex, matNode[texKey + "Sampler"]);
                mat->SetTexture(TextureSlotFromName(name), tex);
            } else {
                GE_CORE_WARN("SceneSerializer: 材质纹理异步加载失败: {0}", path);
            }
        }
    }

    if (matNode["FloatParams"]) {
        for (const auto &it : matNode["FloatParams"]) {
            mat->SetFloat(it.first.as<std::string>(), it.second.as<float>());
        }
    }

    // 自发光颜色因子（缺省 [0,0,0] = 不发光）
    if (matNode["EmissiveFactor"]) {
        mat->SetEmissiveFactor(DeserializeVec3(matNode["EmissiveFactor"], {0.0f, 0.0f, 0.0f}));
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

// 四元数序列化为 [w, x, y, z]，便于与旧版 3 元素欧拉角 [x, y, z] 区分
YAML::Node SerializeQuat(const glm::quat &q) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node.push_back(q.w);
    node.push_back(q.x);
    node.push_back(q.y);
    node.push_back(q.z);
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

// 兼容新版 [w,x,y,z] 四元数与旧版 [x,y,z] 欧拉角（弧度）两种场景文件
glm::quat DeserializeQuat(const YAML::Node &node, const glm::quat &def = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
    if (!node || !node.IsSequence())
        return def;

    if (node.size() == 3) {
        // 旧版格式：三个元素是欧拉角（弧度），转为四元数
        return glm::quat(glm::vec3(
            node[0].as<float>(), node[1].as<float>(), node[2].as<float>()));
    }
    if (node.size() >= 4) {
        return glm::quat(
            node[0].as<float>(def.w),
            node[1].as<float>(def.x),
            node[2].as<float>(def.y),
            node[3].as<float>(def.z));
    }
    return def;
}

// ============================================================
// .gemesh 烘焙辅助 —— 场景保存时自动把非 .gemesh 来源转为引擎内置格式
// ============================================================

/// 扩展名是否为 .gemesh（大小写不敏感）
bool IsGemeshPath(const std::string &path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }
    return ext == ".gemesh";
}

/**
 * @brief 拆分 glTF 多 mesh 复合键（"foo.gltf#N"）为<基础文件, mesh 索引>。
 *
 * 仅当源路径扩展名是 .gltf/.glb 且 # 之后全为数字时才当 mesh 索引解析；
 * 其余情形（普通文件名里恰好含 #、OBJ 等）原样当作整条路径、索引为 0。
 */
void SplitGLTFMeshKey(const std::string &srcKey, std::string &filePath, size_t &meshIndex) {
    filePath  = srcKey;
    meshIndex = 0;

    const size_t hashPos = srcKey.rfind('#');
    if (hashPos == std::string::npos) {
        return;
    }
    const std::string suffix = srcKey.substr(hashPos + 1);
    const bool allDigit = !suffix.empty()
        && std::all_of(suffix.begin(), suffix.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
    if (!allDigit) {
        return;
    }

    const std::string base = srcKey.substr(0, hashPos);
    std::string baseExt = std::filesystem::path(base).extension().string();
    for (char &c : baseExt) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }
    if (baseExt != ".gltf" && baseExt != ".glb") {
        return;
    }

    filePath  = base;
    meshIndex = static_cast<size_t>(std::stoul(suffix));
}

/**
 * @brief 由来源标识派生烘焙输出路径（与源文件同目录）。
 *
 *   foo.obj / foo.gltf    -> <同目录>/foo.gemesh
 *   foo.gltf#N（N>0）     -> <同目录>/foo_N.gemesh（与 #0 的裸路径区分）
 */
std::string DeriveGemeshOutPath(const std::string &srcKey) {
    std::string filePath;
    size_t meshIndex = 0;
    SplitGLTFMeshKey(srcKey, filePath, meshIndex);

    const std::filesystem::path src(filePath);
    if (meshIndex > 0) {
        return (src.parent_path() /
                (src.stem().string() + "_" + std::to_string(meshIndex) + ".gemesh")).string();
    }
    return (src.parent_path() / (src.stem().string() + ".gemesh")).string();
}

/**
 * @brief 把源资产烘焙为 .gemesh（重读源文件重建 MeshData → 切线 → 包围盒 → 序列化）。
 *
 * Mesh 上传 GPU 后不保留 CPU 顶点/索引副本（Mesh.cpp），故此处要按来源重新解析：
 *   - .gltf/.glb（含 #N 复合键）→ GLTF::BuildMeshData(filePath, index, out)
 *   - .obj                      → ModelLoader::Parse(filePath, out)
 * 输出文件已存在时跳过烘焙直接视为成功（避免每次保存都重解析大模型）。
 *
 * @param srcKey  来源标识（文件路径，可为 "foo.gltf#N"）
 * @param outPath 输出 .gemesh 路径
 * @param err     非空时回填错误描述
 * @return 成功（或产物已存在）返回 true
 */
bool BakeSourceToGemesh(const std::string &srcKey, const std::string &outPath,
                        std::string &err) {
    if (std::filesystem::exists(outPath)) {
        return true;
    }

    std::string filePath;
    size_t meshIndex = 0;
    SplitGLTFMeshKey(srcKey, filePath, meshIndex);

    // 重建 CPU 载荷（Mesh 不持有顶点数组，需重读源资产）
    MeshData data;
    std::string ext = std::filesystem::path(filePath).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }

    bool parsed = false;
    if (ext == ".gltf" || ext == ".glb") {
        parsed = GLTF::BuildMeshData(filePath, meshIndex, data);
    } else if (ext == ".obj") {
        parsed = ModelLoader::Parse(filePath, data);
    } else {
        err = "不支持的来源格式: " + srcKey;
        return false;
    }
    if (!parsed || data.vertices.empty() || data.indices.empty()) {
        err = "源资产解析失败: " + srcKey;
        return false;
    }

    // 切线计算，保证与运行时加载路径（MeshManager 异步 decode）逐字节一致
    ModelLoader::ComputeTangents(data);

    // 未产出子网格的源（防御）补一个全覆盖子网格，镜像 Mesh::Create 的行为
    if (data.subMeshes.empty()) {
        data.subMeshes.push_back(SubMesh{0, static_cast<uint32_t>(data.vertices.size()),
                                         0, static_cast<uint32_t>(data.indices.size()),
                                         {}, nullptr});
    }

    // 包围盒 + 源资产追溯信息
    GEMeshMeta meta;
    meta.aabbMin = meta.aabbMax = data.vertices[0].Position;
    for (const auto &v : data.vertices) {
        meta.aabbMin = glm::min(meta.aabbMin, v.Position);
        meta.aabbMax = glm::max(meta.aabbMax, v.Position);
    }
    meta.sourceAsset = srcKey;

    return SerializeGEMesh(outPath, data, meta, &err);
}

/**
 * @brief 把一个 mesh 引用解析为应写入场景 YAML 的路径。
 *
 *   - 空路径                → 空（不写 Mesh 字段）
 *   - "builtin:*" 前缀       → 原样（内置几何体无文件依赖，不烘焙）
 *   - ".gemesh" 后缀         → 原样（已是引擎内置格式）
 *   - 其它                   → 烘焙为 .gemesh 后返回产物路径；烘焙失败回退原路径并告警
 *
 * @param src        网格来源标识（GetFilePath() 返回值）
 * @param bakedCache <源路径 → 烘焙产物路径>，同源只烘焙一次（含碰撞去重的反查）
 * @return 应写入场景的路径（空 = 不写 Mesh 字段）
 */
std::string ResolveMeshSerializedPath(
    const std::string &src,
    std::unordered_map<std::string, std::string> &bakedCache) {
    if (src.empty()) {
        return {};
    }
    // 内置几何体 / 已烘焙格式原样保留
    if (src.rfind("builtin:", 0) == 0 || IsGemeshPath(src)) {
        return src;
    }

    // 同源（场景中多处引用同一模型）只烘焙一次
    const auto cached = bakedCache.find(src);
    if (cached != bakedCache.end()) {
        return cached->second;
    }

    std::string outPath = DeriveGemeshOutPath(src);
    if (outPath.empty()) {
        GE_CORE_WARN("SceneSerializer: 无法为网格 {0} 推导烘焙输出路径，回退原路径", src);
        bakedCache[src] = src;
        return src;
    }

    // 碰撞兜底：产物路径已被其它来源占用（如 foo#1 与真实 foo_1.obj）时追加 _2/_3 区分
    for (int n = 2;; ++n) {
        bool collision = false;
        for (const auto &kv : bakedCache) {
            if (kv.second == outPath && kv.first != src) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            break;
        }
        const std::filesystem::path p(outPath);
        outPath = (p.parent_path() /
                   (p.stem().string() + "_" + std::to_string(n) + ".gemesh")).string();
    }

    std::string err;
    if (!BakeSourceToGemesh(src, outPath, err)) {
        // 单个网格烘焙失败不中断整场景保存，回退原路径保证场景仍可加载
        GE_CORE_WARN("SceneSerializer: 网格 {0} 烘焙失败（{1}），回退原路径", src, err);
        bakedCache[src] = src;
        return src;
    }

    bakedCache[src] = outPath;
    GE_CORE_INFO("SceneSerializer: 网格 {0} 已烘焙为 {1}", src, outPath);
    return outPath;
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

    // 烘焙去重缓存：<网格来源路径 → 烘焙 .gemesh 产物路径>，同源只烘焙一次
    std::unordered_map<std::string, std::string> bakedGemeshCache;

    // 先收集 distinct 皮肤定义：同一条 glTF skin 被多 node 引用时共享同一 SkinDef，
    // 每份去重后只写一个全局 Skins 表条目，实体侧只存 skinId 引用（避免 107 份重复）。
    // skinDefToId：SkinDef* → 生成的文件内 uuid（实体侧引用）；skinTableOrder：保写入序。
    std::unordered_map<const SkinDef *, std::string> skinDefToId;
    std::vector<std::pair<std::string, const SkinDef *>> skinTableOrder;
    {
        std::unordered_set<const SkinDef *> seenSkinDef;
        auto skinCompView = reg.view<SkinComponent>();
        for (auto e : skinCompView) {
            const auto &sc = skinCompView.get<SkinComponent>(e);
            if (!sc.skin) {
                continue;
            }
            const SkinDef *def = sc.skin.get();
            if (!seenSkinDef.insert(def).second) {
                continue;
            }
            const std::string sid = GenerateUUID();
            skinDefToId[def] = sid;
            skinTableOrder.emplace_back(sid, def);
        }
    }

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

        // ---- IDComponent（持久化引用标识）----
        if (entity.HasComponent<IDComponent>()) {
            entityNode["Id"] = entity.GetComponent<IDComponent>().UUID;
        }

        // ---- 父实体引用（写父实体的 UUID 字符串，根实体省略）----
        // worldMatrix 是每帧 DFS 的派生值，绝不落盘（纪律 3）
        if (Entity parent = m_Scene->GetParent(entity)) {
            if (parent.HasComponent<IDComponent>())
                entityNode["Parent"] = parent.GetComponent<IDComponent>().UUID;
        }

        // ---- TransformComponent ----
        if (entity.HasComponent<TransformComponent>()) {
            const auto &tc = entity.GetComponent<TransformComponent>();
            YAML::Node transformNode = entityNode["Transform"];
            transformNode["Translation"] = SerializeVec3(tc.Translation);
            transformNode["Rotation"] = SerializeQuat(tc.Rotation);
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
                spriteNode["Texture"] = CanonicalAssetRef(src.SpriteTexture->GetFilePath());
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

            // 网格路径：非 .gemesh 来源自动烘焙为 .gemesh（内置几何体原样保留）
            if (mc.MeshPtr) {
                const std::string meshPath =
                    ResolveMeshSerializedPath(mc.MeshPtr->GetFilePath(), bakedGemeshCache);
                if (!meshPath.empty()) {
                    meshNode["Mesh"] = CanonicalAssetRef(meshPath);
                }
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
            cameraNode["Mode"] = (cam.GetMode() == Camera::Mode::Orbit) ? "Orbit" : "FreeLook";

            // 投影参数
            cameraNode["Fov"] = cam.GetFov();
            cameraNode["Aspect"] = cam.GetAspect();

            cameraNode["Near"] = cam.GetNear();
            cameraNode["Far"] = cam.GetFar();
            cameraNode["Exposure"] = cam.GetExposure();

            // Orbit 模式参数
            cameraNode["Target"] = SerializeVec3(cam.GetTarget());
            cameraNode["Theta"] = cam.GetTheta();
            cameraNode["Phi"] = cam.GetPhi();
            cameraNode["Distance"] = cam.GetDistance();

            // FreeLook 模式参数
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
            lightNode["CastShadow"] = dlc.CastShadow;
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
            envNode["Enabled"] = ec.Enabled;
            envNode["SkyboxEnabled"] = ec.SkyboxEnabled;
            envNode["IBLEnabled"] = ec.IBLEnabled;
            envNode["IBLIntensity"] = ec.IBLIntensity;
        }

        // ---- AudioSourceComponent / AudioListenerComponent ----
        if (entity.HasComponent<AudioSourceComponent>()) {
            const auto &as = entity.GetComponent<AudioSourceComponent>();
            YAML::Node srcNode = entityNode["AudioSource"];
            srcNode["Enabled"] = as.Enabled;
            srcNode["Spatial"] = as.Spatial;
            if (!as.Sounds.empty()) {
                YAML::Node soundsNode = srcNode["Sounds"];
                soundsNode.SetStyle(YAML::EmitterStyle::Block);
                for (const auto &s : as.Sounds) {
                    YAML::Node sn;
                    sn["Name"] = s.Name;
                    sn["SoundPath"] = CanonicalAssetRef(s.SoundPath);
                    sn["PlayOnAwake"] = s.PlayOnAwake;
                    sn["Loop"] = s.Loop;
                    sn["Volume"] = s.Volume;
                    sn["Pitch"] = s.Pitch;
                    sn["MinDistance"] = s.MinDistance;
                    sn["MaxDistance"] = s.MaxDistance;
                    sn["Rolloff"] = s.Rolloff;
                    sn["Attenuation"] = AttenuationToString(s.Attenuation);
                    soundsNode.push_back(sn);
                }
            }
        }

        if (entity.HasComponent<AudioListenerComponent>()) {
            const auto &al = entity.GetComponent<AudioListenerComponent>();
            YAML::Node alNode = entityNode["AudioListener"];
            alNode["Enabled"] = al.Enabled;
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

        // ---- CharacterControllerComponent ----
        // 运行时字段（IsInitialized/WishVelocity/IsGrounded/...）不落盘，只写配置四项。
        if (entity.HasComponent<CharacterControllerComponent>()) {
            const auto &ccc = entity.GetComponent<CharacterControllerComponent>();
            YAML::Node ccNode = entityNode["CharacterController"];
            ccNode["Radius"] = ccc.Radius;
            ccNode["Height"] = ccc.Height;
            ccNode["Axis"] = static_cast<int>(ccc.Axis);
            ccNode["FrontAxis"] = static_cast<int>(ccc.FrontAxis);
            ccNode["Offset"] = SerializeVec3(ccc.Offset);
            ccNode["MaxSlopeAngle"] = ccc.MaxSlopeAngle;
            ccNode["MaxJumpSpeed"] = ccc.MaxJumpSpeed;
            ccNode["GravityScale"] = ccc.GravityScale;
            ccNode["FaceMovement"] = ccc.FaceMovement;
            ccNode["TurnSpeed"] = ccc.TurnSpeed;
            ccNode["InvertFront"] = ccc.InvertFront;
            ccNode["UseRootMotion"] = ccc.UseRootMotion;
            ccNode["RootBoneNodeIndex"] = ccc.RootBoneNodeIndex;
            ccNode["ZeroRootBoneLocal"] = ccc.ZeroRootBoneLocal;
        }

        // ---- FollowCameraComponent ----
        // 运行时数据（CurrentMode/CurrentDistance/CurrentPos/SnapPending）不落盘，
        // 姿态存在 CameraComponent 的 Camera 里；作者配置全部落盘。
        if (entity.HasComponent<FollowCameraComponent>()) {
            const auto &fcc = entity.GetComponent<FollowCameraComponent>();
            YAML::Node fcNode = entityNode["FollowCamera"];
            fcNode["Enabled"] = fcc.Enabled;
            fcNode["StartMode"] = (fcc.StartMode == FollowCameraViewMode::FirstPerson) ? "First" : "Third";
            fcNode["ToggleEnabled"] = fcc.ToggleEnabled;
            fcNode["ToggleKey"] = fcc.ToggleKey;
            fcNode["EyeOffset"] = SerializeVec3(fcc.EyeOffset);
            fcNode["YawSpeed"] = fcc.YawSpeed;
            fcNode["PitchSpeed"] = fcc.PitchSpeed;
            fcNode["MinPitch"] = fcc.MinPitch;
            fcNode["MaxPitch"] = fcc.MaxPitch;
            fcNode["InvertY"] = fcc.InvertY;
            fcNode["TargetOffset"] = SerializeVec3(fcc.TargetOffset);
            fcNode["Distance"] = fcc.Distance;
            fcNode["MinDistance"] = fcc.MinDistance;
            fcNode["MaxDistance"] = fcc.MaxDistance;
            fcNode["ShoulderOffset"] = fcc.ShoulderOffset;
            fcNode["CollisionEnabled"] = fcc.CollisionEnabled;
            fcNode["CollisionRadius"] = fcc.CollisionRadius;
            fcNode["CollisionMargin"] = fcc.CollisionMargin;
            fcNode["Smoothing"] = fcc.Smoothing;
            fcNode["ZoomSpeed"] = fcc.ZoomSpeed;
        }

        // ---- WaterComponent（阶段 1 + 阶段 3 序列化）----
        if (entity.HasComponent<WaterComponent>()) {
            const auto &wc = entity.GetComponent<WaterComponent>();
            YAML::Node waterNode = entityNode["Water"];
            waterNode["Size"] = SerializeVec2(wc.Size);
            waterNode["Height"] = wc.Height;
            waterNode["Resolution"] = wc.Resolution;
            waterNode["TimeScale"] = wc.TimeScale;
            waterNode["DeepColor"] = SerializeVec3(wc.DeepColor);
            waterNode["ShallowColor"] = SerializeVec3(wc.ShallowColor);
            if (wc.NormalMap && !wc.NormalMap->GetFilePath().empty()) {
                waterNode["NormalMap"] = CanonicalAssetRef(wc.NormalMap->GetFilePath());
            }
            waterNode["NormalTiling"] = wc.NormalTiling;
            waterNode["NormalStrength"] = wc.NormalStrength;
            if (wc.ColorMap && !wc.ColorMap->GetFilePath().empty()) {
                waterNode["ColorMap"] = CanonicalAssetRef(wc.ColorMap->GetFilePath());
            }
            waterNode["ColorTiling"] = wc.ColorTiling;
            waterNode["ColorStrength"] = wc.ColorStrength;
            waterNode["Roughness"] = wc.Roughness;
            waterNode["Opacity"] = wc.Opacity;
            waterNode["AlphaCoverage"] = wc.AlphaCoverage;
            waterNode["ReflectionStrength"] = wc.ReflectionStrength;
            waterNode["RefractionStrength"] = wc.RefractionStrength;
            waterNode["AbsorptionDepth"] = wc.AbsorptionDepth;
            waterNode["FoamDistance"] = wc.FoamDistance;
            waterNode["FoamIntensity"] = wc.FoamIntensity;
            waterNode["FogDensity"] = wc.FogDensity;
            waterNode["CausticsEnabled"] = wc.CausticsEnabled;
            waterNode["CausticsIntensity"] = wc.CausticsIntensity;
            for (int i = 0; i < 4; ++i) {
                const auto &w = wc.Waves[i];
                YAML::Node waveNode = waterNode["Waves"][std::to_string(i)];
                waveNode["Direction"] = SerializeVec2(w.Direction);
                waveNode["Amplitude"] = w.Amplitude;
                waveNode["Wavelength"] = w.Wavelength;
                waveNode["Speed"] = w.Speed;
            }
        }

        // ---- BoxColliderComponent ----
        if (entity.HasComponent<BoxColliderComponent>()) {
            const auto &bcc = entity.GetComponent<BoxColliderComponent>();
            YAML::Node boxNode = entityNode["BoxCollider"];
            boxNode["HalfExtents"] = SerializeVec3(bcc.HalfExtents);
            boxNode["Offset"] = SerializeVec3(bcc.Offset);
            boxNode["DrawDebug"] = bcc.DrawDebug;
        }

        // ---- BoundingBoxComponent（实体级粗剔除盒，编辑器 gizmo 手动摆放）----
        if (entity.HasComponent<BoundingBoxComponent>()) {
            const auto &bb = entity.GetComponent<BoundingBoxComponent>();
            YAML::Node bbNode = entityNode["BoundingBox"];
            bbNode["Center"] = SerializeVec3(bb.Center);
            bbNode["Size"] = SerializeVec3(bb.Size);
        }

        // ---- SphereColliderComponent ----
        if (entity.HasComponent<SphereColliderComponent>()) {
            const auto &scc = entity.GetComponent<SphereColliderComponent>();
            YAML::Node sphereNode = entityNode["SphereCollider"];
            sphereNode["Radius"] = scc.Radius;
            sphereNode["Offset"] = SerializeVec3(scc.Offset);
            sphereNode["DrawDebug"] = scc.DrawDebug;
        }

        // ---- CapsuleColliderComponent ----
        if (entity.HasComponent<CapsuleColliderComponent>()) {
            const auto &ccc = entity.GetComponent<CapsuleColliderComponent>();
            YAML::Node capsuleNode = entityNode["CapsuleCollider"];
            capsuleNode["Radius"] = ccc.Radius;
            capsuleNode["HalfHeight"] = ccc.HalfHeight;
            capsuleNode["Axis"] = static_cast<int>(ccc.Axis);
            capsuleNode["Offset"] = SerializeVec3(ccc.Offset);
            capsuleNode["DrawDebug"] = ccc.DrawDebug;
        }

        // ---- JointComponent（骨骼关节标记）----
        // 关节序号是导入器按 skin.joints 写入的皮肤内索引，不落盘则骨架链丢失。
        if (entity.HasComponent<JointComponent>()) {
            entityNode["Joint"]["JointIndex"] = entity.GetComponent<JointComponent>().jointIndex;
        }

        // ---- SkinComponent（皮肤引用）----
        // 全场景皮肤定义集中在顶层 Skins 表（按 SkinDef 去重），此处仅存引用：
        //   Skin.Id      — Skins 表条目的 uuid（反序列化按它接回共享 SkinDef）
        //   Skin.Mesh    — 关联网格序列化路径（可选，供面板展示；非 .gemesh 自动烘焙）
        if (entity.HasComponent<SkinComponent>()) {
            const auto &sc = entity.GetComponent<SkinComponent>();
            if (sc.skin) {
                auto it = skinDefToId.find(sc.skin.get());
                if (it != skinDefToId.end()) {
                    entityNode["Skin"]["Id"] = it->second;
                    if (sc.MeshPtr) {
                        const std::string skinMeshPath =
                            ResolveMeshSerializedPath(sc.MeshPtr->GetFilePath(), bakedGemeshCache);
                        if (!skinMeshPath.empty()) {
                            entityNode["Skin"]["Mesh"] = CanonicalAssetRef(skinMeshPath);
                        }
                    }
                }
            }
        }

        // ---- ScriptComponent ----
        // 只落 ScriptPath + Enabled（行为表是引擎注入的共享函数，Lua 状态随场景重建）。
        // 空路径 = 未挂载，不写块；旧场景无 Script 键照常加载。
        if (entity.HasComponent<ScriptComponent>()) {
            const auto &sc = entity.GetComponent<ScriptComponent>();
            if (!sc.ScriptPath.empty()) {
                YAML::Node scriptNode = entityNode["Script"];
                scriptNode["ScriptPath"] = sc.ScriptPath;
                scriptNode["Enabled"] = sc.Enabled;
                // public 字段值：<名, {Type, Value}>（随实体各存一份，面板编辑 → 落盘）
                if (!sc.PublicFields.empty()) {
                    YAML::Node pfNode = scriptNode["PublicFields"];
                    for (const auto &[name, field] : sc.PublicFields) {
                        YAML::Node fn = pfNode[name];
                        fn["Type"] = ScriptFieldTypeName(field.Type);
                        switch (field.Type) {
                        case ScriptFieldType::Number: fn["Value"] = field.Number; break;
                        case ScriptFieldType::Bool:   fn["Value"] = field.Bool;   break;
                        case ScriptFieldType::String: fn["Value"] = field.String; break;
                        default: break;
                        }
                    }
                }
            }
        }

        // ---- AnimationComponent（骨骼动画驱动）----
        // 结构中各 clip 是共享键帧（模型级不变量），此处只持久化引用与实例状态：
        //   Clips[].Clip    — 动画片段源键 "path#N"（反序列化经 AnimationClipManager 取共享 clip）
        //   Clips[].Targets — clip.channels 一一对应的目标实体 UUID 列表（跨文件稳定引用）
        //   Active/Time/Speed/Playing/Loop — 播放状态
        // world 矩阵与键帧本体都不落盘（键帧随源模型重建，播放状态随组件走）。
        if (entity.HasComponent<AnimationComponent>()) {
            const auto &ac = entity.GetComponent<AnimationComponent>();
            YAML::Node animNode = entityNode["Animation"];
            animNode["Active"] = ac.active;
            animNode["Time"] = ac.time;
            animNode["Speed"] = ac.speed;
            animNode["Playing"] = ac.playing;
            animNode["Loop"] = ac.loop;

            if (!ac.clips.empty()) {
                YAML::Node clipsNode = animNode["Clips"];
                clipsNode.SetStyle(YAML::EmitterStyle::Block);
                for (const auto &inst : ac.clips) {
                    if (!inst.clip) {
                        continue;
                    }
                    YAML::Node clipNode;
                    clipNode["Clip"] = CanonicalAssetRef(inst.clip->source);
                    YAML::Node targetsNode = clipNode["Targets"];
                    targetsNode.SetStyle(YAML::EmitterStyle::Flow);
                    for (entt::entity t : inst.channelTargets) {
                        const auto *idc = reg.try_get<IDComponent>(t);
                        targetsNode.push_back(idc ? idc->UUID : "");
                    }
                    // 事件表是场景数据（非模型不变量），随每 clip 落盘；为空则不写键（旧工具链文件照常）
                    if (!inst.events.empty()) {
                        YAML::Node eventsNode = clipNode["Events"];
                        eventsNode.SetStyle(YAML::EmitterStyle::Block);
                        for (const auto &e : inst.events) {
                            YAML::Node evtNode;
                            evtNode["Time"] = e.time;
                            evtNode["Name"] = e.name;
                            eventsNode.push_back(evtNode);
                        }
                    }
                    clipsNode.push_back(clipNode);
                }
            }
        }

        // ---- AnimStateMachineComponent（动画状态机：状态 + 转换 + 条件）----
        // 运行时字段（current/stateTime/参数表）不落盘：玩法态初值由脚本 OnCreate 重填，
        // 反序列化后 current=SIZE_MAX，首次 enabled=true 时从 initial 进入。
        // From/To 以状态名序列化（"*" = ANY 全局转换），反序列化时转回下标；引用越界容错为 ANY。
        if (entity.HasComponent<AnimStateMachineComponent>()) {
            const auto &asmc = entity.GetComponent<AnimStateMachineComponent>();
            YAML::Node asmNode = entityNode["AnimStateMachine"];
            asmNode["Enabled"] = asmc.enabled;
            if (!asmc.initialState.empty()) {
                asmNode["Initial"] = asmc.initialState;
            }

            if (!asmc.states.empty()) {
                YAML::Node statesNode = asmNode["States"];
                statesNode.SetStyle(YAML::EmitterStyle::Block);
                for (const auto &st : asmc.states) {
                    YAML::Node stateNode;
                    stateNode["Name"] = st.name;
                    stateNode["Clip"] = st.clipName;
                    stateNode["Loop"] = st.loop;
                    stateNode["Speed"] = st.speed;
                    statesNode.push_back(stateNode);
                }
            }

            if (!asmc.transitions.empty()) {
                YAML::Node transNode = asmNode["Transitions"];
                transNode.SetStyle(YAML::EmitterStyle::Block);
                for (const auto &tr : asmc.transitions) {
                    YAML::Node tNode;
                    const bool fromValid = tr.from != SIZE_MAX && tr.from < asmc.states.size();
                    const bool toValid = tr.to < asmc.states.size();
                    tNode["From"] = fromValid ? asmc.states[tr.from].name : "*";
                    tNode["To"] = toValid ? asmc.states[tr.to].name : "*";
                    tNode["BlendSec"] = tr.blendSec;
                    if (!tr.conditions.empty()) {
                        YAML::Node condsNode = tNode["Conditions"];
                        condsNode.SetStyle(YAML::EmitterStyle::Block);
                        for (const auto &c : tr.conditions) {
                            YAML::Node condNode;
                            condNode["Type"] = AnimConditionTypeName(c.type);
                            switch (c.type) {
                            case AnimCondition::Type::FloatCmp:
                                condNode["Param"] = c.param;
                                condNode["Cmp"] = AnimConditionCmpName(c.cmp);
                                condNode["Value"] = c.value;
                                break;
                            case AnimCondition::Type::Bool:
                                condNode["Param"] = c.param;
                                condNode["Expect"] = c.expect;
                                break;
                            case AnimCondition::Type::StateTime:
                                condNode["Value"] = c.value;
                                break;
                            case AnimCondition::Type::StateEnded:
                                break;
                            }
                            condsNode.push_back(condNode);
                        }
                    }
                    transNode.push_back(tNode);
                }
            }
        }

        entitiesNode.push_back(entityNode);
    }

    // ---- 全局皮肤定义表（Skins）：每个 distinct SkinDef 一个条目 ----
    //   Id          — 文件内唯一 uuid（实体 Skin.Id 引用）
    //   Joints       — 关节实体的 UUID 列表（跨文件稳定，按皮肤关节序）
    //   InverseBindMatrices — 每关节一个 16 元素浮点展开（列主序，与 glm::mat4 一致）
    if (!skinTableOrder.empty()) {
        YAML::Node skinsNode = sceneNode["Skins"];
        skinsNode.SetStyle(YAML::EmitterStyle::Block);
        for (const auto &[sid, def] : skinTableOrder) {
            YAML::Node skinNode;
            skinNode["Id"] = sid;

            YAML::Node jointsNode = skinNode["Joints"];
            jointsNode.SetStyle(YAML::EmitterStyle::Flow);
            for (entt::entity jh : def->joints) {
                if (const auto *idc = reg.try_get<IDComponent>(jh)) {
                    jointsNode.push_back(idc->UUID);
                } else {
                    jointsNode.push_back(""); // 空洞：实体已销毁，反序列化端跳过
                }
            }

            YAML::Node ibmNode = skinNode["InverseBindMatrices"];
            for (const auto &m : def->inverseBindMatrices) {
                YAML::Node row;
                row.SetStyle(YAML::EmitterStyle::Flow);
                for (int col = 0; col < 4; ++col) {
                    for (int r = 0; r < 4; ++r) {
                        row.push_back(m[col][r]);
                    }
                }
                ibmNode.push_back(row);
            }

            skinsNode.push_back(skinNode);
        }
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
    m_Scene->ClearAllEntities();

    if (!entitiesNode || !entitiesNode.IsSequence()) {
        GE_CORE_WARN("SceneSerializer::Deserialize: 场景中没有 Entities 节点，将加载为空场景");
        return true;
    }

    uint32_t entityCount = 0;

    // 两遍反序列化：先全建实体并记录 Id → 实体句柄映射，再二次遍历接上 parent。
    // 不存在依赖"数组索引"的脆弱方案，重排/增删实体不影响引用语义。
    std::unordered_map<std::string, Entity> idToEntity;
    struct PendingParentLink {
        Entity child;
        std::string parentId;
    };
    std::vector<PendingParentLink> pendingParentLinks;

    // 皮肤引用延迟回填：SkinComponent 需要 Skins 表（其 Joints 是实体 UUID）在全部
    // 实体建成后才能解析，故第一遍只记待回填记录，Skins 表重建后再逐个挂上共享 SkinDef。
    struct PendingSkinComponent {
        Entity entity;
        std::string skinId;
        std::string meshPath;
    };
    std::vector<PendingSkinComponent> pendingSkins;

    // 动画回填：AnimationComponent 需要 clip 源键 + 目标实体 UUID，在全部实体建成后
    // 才能把 UUID 解析为句柄（与皮肤同理），故第一遍只记待回填记录。
    struct PendingAnimationClip {
        std::string clipKey;               ///< 动画片段源键 "path#N"
        std::vector<std::string> targetIds; ///< 与 clip.channels 一一对应的目标实体 UUID
        std::vector<AnimationEvent> events; ///< 场景级事件表（缺省为空，旧场景照常）
    };
    struct PendingAnimation {
        Entity entity;
        std::vector<PendingAnimationClip> clips;
        size_t active = 0;
        float time = 0.0f, speed = 1.0f;
        bool playing = true, loop = true;
    };
    std::vector<PendingAnimation> pendingAnimations;

    for (const auto &entityNode : entitiesNode) {
        // ---- 实体名称 ----
        std::string name = "Entity";
        if (entityNode["Name"]) {
            name = entityNode["Name"].as<std::string>("Entity");
        }

        Entity entity = m_Scene->CreateEntity(name);

        // ---- Id：文件带 Id 时覆盖 CreateEntity 生成的随机 UUID，缺省（旧场景）保留刚生成的 ----
        std::string id = entityNode["Id"] ? entityNode["Id"].as<std::string>("") : std::string();
        if (!id.empty())
            entity.GetComponent<IDComponent>().UUID = id;
        id = entity.GetComponent<IDComponent>().UUID;
        if (idToEntity.count(id)) {
            GE_CORE_WARN("SceneSerializer: 检测到重复实体 Id {0}，后者覆盖前者", id);
        }
        idToEntity[id] = entity;

        // ---- 父引用：记下父实体 UUID，等全部实体建好后第二遍再连接 ----
        if (entityNode["Parent"]) {
            std::string parentId = entityNode["Parent"].as<std::string>("");
            if (!parentId.empty())
                pendingParentLinks.push_back({entity, parentId});
        }

        // ---- TransformComponent ----
        // 注意：CreateEntity 已经添加了 TransformComponent，这里只需修改值
        if (entityNode["Transform"]) {
            auto &tc = entity.GetComponent<TransformComponent>();
            YAML::Node transformNode = entityNode["Transform"];
            tc.Translation = DeserializeVec3(transformNode["Translation"], {0.0f, 0.0f, 0.0f});
            tc.Rotation = DeserializeQuat(transformNode["Rotation"], glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
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
                // 异步加载：未就绪前渲染器降级默认纹理，就绪后自动亮相
                src.SpriteTexture = Renderer::GetAssetManager().LoadTextureAsync(texPath);
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

            // 曝光系数（缺省 1.0 = 不改亮度，旧场景文件照常）
            float exposure = cameraNode["Exposure"] ? cameraNode["Exposure"].as<float>(1.0f) : 1.0f;
            cam.SetExposure(exposure);

            // 模式
            std::string modeStr = cameraNode["Mode"] ? cameraNode["Mode"].as<std::string>("Orbit") : "Orbit";
            Camera::Mode mode = (modeStr == "FPS" || modeStr == "FreeLook") ? Camera::Mode::FreeLook : Camera::Mode::Orbit;
            cam.SetMode(mode);

            // Orbit 模式参数
            if (cameraNode["Target"]) {
                cam.SetTarget(DeserializeVec3(cameraNode["Target"], {0.0f, 0.0f, 0.0f}));
            }
            float theta = cameraNode["Theta"] ? cameraNode["Theta"].as<float>(0.0f) : 0.0f;
            float phi = cameraNode["Phi"] ? cameraNode["Phi"].as<float>(0.0f) : 0.0f;
            float distance = cameraNode["Distance"] ? cameraNode["Distance"].as<float>(2.0f) : 2.0f;
            cam.SetOrbit(theta, phi, distance);

            // FreeLook 模式参数
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
            dlc.CastShadow = lightNode["CastShadow"] ? lightNode["CastShadow"].as<bool>(true) : true;
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
            ec.Enabled = envNode["Enabled"] ? envNode["Enabled"].as<bool>(true) : true;
            ec.SkyboxEnabled = envNode["SkyboxEnabled"] ? envNode["SkyboxEnabled"].as<bool>(true) : true;
            ec.IBLEnabled = envNode["IBLEnabled"] ? envNode["IBLEnabled"].as<bool>(true) : true;
            ec.IBLIntensity = envNode["IBLIntensity"] ? envNode["IBLIntensity"].as<float>(1.0f) : 1.0f;
        }

        // ---- AudioSourceComponent / AudioListenerComponent ----
        if (entityNode["AudioSource"]) {
            YAML::Node srcNode = entityNode["AudioSource"];
            auto &as = entity.AddComponent<AudioSourceComponent>();
            as.Enabled = srcNode["Enabled"] ? srcNode["Enabled"].as<bool>(true) : true;
            as.Spatial = srcNode["Spatial"] ? srcNode["Spatial"].as<bool>(true) : true;
            if (srcNode["Sounds"] && srcNode["Sounds"].IsSequence()) {
                for (const auto &sn : srcNode["Sounds"]) {
                    AudioSound s;
                    s.Name = sn["Name"] ? sn["Name"].as<std::string>() : std::string();
                    s.SoundPath = sn["SoundPath"] ? sn["SoundPath"].as<std::string>() : std::string();
                    s.PlayOnAwake = sn["PlayOnAwake"] ? sn["PlayOnAwake"].as<bool>(false) : false;
                    s.Loop = sn["Loop"] ? sn["Loop"].as<bool>(false) : false;
                    s.Volume = sn["Volume"] ? sn["Volume"].as<float>(1.0f) : 1.0f;
                    s.Pitch = sn["Pitch"] ? sn["Pitch"].as<float>(1.0f) : 1.0f;
                    s.MinDistance = sn["MinDistance"] ? sn["MinDistance"].as<float>(1.0f) : 1.0f;
                    s.MaxDistance = sn["MaxDistance"] ? sn["MaxDistance"].as<float>(100.0f) : 100.0f;
                    s.Rolloff = sn["Rolloff"] ? sn["Rolloff"].as<float>(1.0f) : 1.0f;
                    s.Attenuation = AttenuationFromString(sn["Attenuation"] ? sn["Attenuation"].as<std::string>("Inverse") : "Inverse");
                    as.Sounds.push_back(s);
                }
            }
        }

        if (entityNode["AudioListener"]) {
            YAML::Node alNode = entityNode["AudioListener"];
            auto &alc = entity.AddComponent<AudioListenerComponent>();
            alc.Enabled = alNode["Enabled"] ? alNode["Enabled"].as<bool>(true) : true;
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

        // ---- CharacterControllerComponent ----
        // AddComponent 触发 OnComponentAdded → RequestCreateCharacter（延迟创建）
        if (entityNode["CharacterController"]) {
            YAML::Node ccNode = entityNode["CharacterController"];
            auto &ccc = entity.AddComponent<CharacterControllerComponent>();

            ccc.Radius = ccNode["Radius"] ? ccNode["Radius"].as<float>(0.35f) : 0.35f;
            ccc.Height = ccNode["Height"] ? ccNode["Height"].as<float>(1.80f) : 1.80f;
            ccc.Axis = static_cast<CapsuleAxis>(ccNode["Axis"] ? ccNode["Axis"].as<int>(0) : 0);
            ccc.FrontAxis = static_cast<CapsuleAxis>(ccNode["FrontAxis"] ? ccNode["FrontAxis"].as<int>(2) : 2);
            ccc.Offset = DeserializeVec3(ccNode["Offset"], {0.0f, 0.0f, 0.0f});
            ccc.MaxSlopeAngle = ccNode["MaxSlopeAngle"] ? ccNode["MaxSlopeAngle"].as<float>(45.0f) : 45.0f;
            ccc.MaxJumpSpeed = ccNode["MaxJumpSpeed"] ? ccNode["MaxJumpSpeed"].as<float>(5.0f) : 5.0f;
            ccc.GravityScale = ccNode["GravityScale"] ? ccNode["GravityScale"].as<float>(1.0f) : 1.0f;
            ccc.FaceMovement = ccNode["FaceMovement"] ? ccNode["FaceMovement"].as<bool>(true) : true;
            ccc.TurnSpeed = ccNode["TurnSpeed"] ? ccNode["TurnSpeed"].as<float>(540.0f) : 540.0f;
            ccc.InvertFront = ccNode["InvertFront"] ? ccNode["InvertFront"].as<bool>(false) : false;
            ccc.UseRootMotion = ccNode["UseRootMotion"] ? ccNode["UseRootMotion"].as<bool>(false) : false;
            ccc.RootBoneNodeIndex = ccNode["RootBoneNodeIndex"] ? ccNode["RootBoneNodeIndex"].as<int>(-1) : -1;
            ccc.ZeroRootBoneLocal = ccNode["ZeroRootBoneLocal"] ? ccNode["ZeroRootBoneLocal"].as<bool>(true) : true;
        }

        // ---- FollowCameraComponent ----
        // AddComponent 触发 OnComponentAdded（纯配置，无物理重建）。
        // 旧字段缺省即默认值；旧 FirstPersonCamera 节点继续兼容（FollowCamera 优先）。
        YAML::Node fcNode = entityNode["FollowCamera"] ? entityNode["FollowCamera"] : entityNode["FirstPersonCamera"];
        if (fcNode) {
            auto &fcc = entity.AddComponent<FollowCameraComponent>();

            fcc.Enabled = fcNode["Enabled"] ? fcNode["Enabled"].as<bool>(true) : true;
            const std::string startMode = fcNode["StartMode"] ? fcNode["StartMode"].as<std::string>("First") : "First";
            fcc.StartMode = (startMode == "Third") ? FollowCameraViewMode::ThirdPerson : FollowCameraViewMode::FirstPerson;
            fcc.ToggleEnabled = fcNode["ToggleEnabled"] ? fcNode["ToggleEnabled"].as<bool>(true) : true;
            fcc.ToggleKey = fcNode["ToggleKey"] ? static_cast<KeyCode>(fcNode["ToggleKey"].as<unsigned int>(Key::V)) : Key::V;
            fcc.EyeOffset = DeserializeVec3(fcNode["EyeOffset"], {0.0f, 1.65f, 0.0f});
            fcc.YawSpeed = fcNode["YawSpeed"] ? fcNode["YawSpeed"].as<float>(0.10f) : 0.10f;
            fcc.PitchSpeed = fcNode["PitchSpeed"] ? fcNode["PitchSpeed"].as<float>(0.10f) : 0.10f;
            fcc.MinPitch = fcNode["MinPitch"] ? fcNode["MinPitch"].as<float>(-89.0f) : -89.0f;
            fcc.MaxPitch = fcNode["MaxPitch"] ? fcNode["MaxPitch"].as<float>(89.0f) : 89.0f;
            fcc.InvertY = fcNode["InvertY"] ? fcNode["InvertY"].as<bool>(false) : false;
            fcc.TargetOffset = DeserializeVec3(fcNode["TargetOffset"], {0.0f, 1.60f, 0.0f});
            fcc.Distance = fcNode["Distance"] ? fcNode["Distance"].as<float>(3.50f) : 3.50f;
            fcc.MinDistance = fcNode["MinDistance"] ? fcNode["MinDistance"].as<float>(1.00f) : 1.00f;
            fcc.MaxDistance = fcNode["MaxDistance"] ? fcNode["MaxDistance"].as<float>(12.0f) : 12.0f;
            fcc.ShoulderOffset = fcNode["ShoulderOffset"] ? fcNode["ShoulderOffset"].as<float>(0.00f) : 0.00f;
            fcc.CollisionEnabled = fcNode["CollisionEnabled"] ? fcNode["CollisionEnabled"].as<bool>(true) : true;
            fcc.CollisionRadius = fcNode["CollisionRadius"] ? fcNode["CollisionRadius"].as<float>(0.25f) : 0.25f;
            fcc.CollisionMargin = fcNode["CollisionMargin"] ? fcNode["CollisionMargin"].as<float>(0.05f) : 0.05f;
            fcc.Smoothing = fcNode["Smoothing"] ? fcNode["Smoothing"].as<float>(8.0f) : 8.0f;
            fcc.ZoomSpeed = fcNode["ZoomSpeed"] ? fcNode["ZoomSpeed"].as<float>(0.40f) : 0.40f;
            // 目标距离必须落在 [MinDistance, MaxDistance]：旧场景/手改 Min/Max 导致越界时，
            // 运行时会被 clamp，而面板仍显示越界值造成“数值一样但效果不同”的困惑。
            fcc.Distance = std::clamp(fcc.Distance, fcc.MinDistance, fcc.MaxDistance);
        }

        // ---- WaterComponent ----
        if (entityNode["Water"]) {
            YAML::Node wn = entityNode["Water"];
            auto &wc = entity.AddComponent<WaterComponent>();
            wc.Size = DeserializeVec2(wn["Size"], {40.0f, 40.0f});
            wc.Height = wn["Height"] ? wn["Height"].as<float>(0.0f) : 0.0f;
            wc.Resolution = wn["Resolution"] ? wn["Resolution"].as<uint32_t>(48) : 48;
            wc.TimeScale = wn["TimeScale"] ? wn["TimeScale"].as<float>(1.0f) : 1.0f;
            wc.DeepColor = DeserializeVec3(wn["DeepColor"], {0.012f, 0.055f, 0.09f});
            wc.ShallowColor = DeserializeVec3(wn["ShallowColor"], {0.05f, 0.30f, 0.38f});
            wc.NormalTiling = wn["NormalTiling"] ? wn["NormalTiling"].as<float>(4.0f) : 4.0f;
            wc.NormalStrength = wn["NormalStrength"] ? wn["NormalStrength"].as<float>(0.55f) : 0.55f;
            wc.ColorTiling = wn["ColorTiling"] ? wn["ColorTiling"].as<float>(1.0f) : 1.0f;
            wc.ColorStrength = wn["ColorStrength"] ? wn["ColorStrength"].as<float>(0.6f) : 0.6f;
            wc.Roughness = wn["Roughness"] ? wn["Roughness"].as<float>(0.12f) : 0.12f;
            wc.Opacity = wn["Opacity"] ? wn["Opacity"].as<float>(1.0f) : 1.0f;
            wc.AlphaCoverage = wn["AlphaCoverage"] ? wn["AlphaCoverage"].as<float>(0.55f) : 0.55f;
            wc.ReflectionStrength = wn["ReflectionStrength"] ? wn["ReflectionStrength"].as<float>(0.85f) : 0.85f;
            wc.RefractionStrength = wn["RefractionStrength"] ? wn["RefractionStrength"].as<float>(0.25f) : 0.25f;
            wc.AbsorptionDepth = wn["AbsorptionDepth"] ? wn["AbsorptionDepth"].as<float>(2.0f) : 2.0f;
            wc.FoamDistance = wn["FoamDistance"] ? wn["FoamDistance"].as<float>(0.8f) : 0.8f;
            wc.FoamIntensity = wn["FoamIntensity"] ? wn["FoamIntensity"].as<float>(0.9f) : 0.9f;
            wc.FogDensity = wn["FogDensity"] ? wn["FogDensity"].as<float>(1.0f) : 1.0f;
            wc.CausticsEnabled = wn["CausticsEnabled"] ? wn["CausticsEnabled"].as<bool>(true) : true;
            wc.CausticsIntensity = wn["CausticsIntensity"] ? wn["CausticsIntensity"].as<float>(1.0f) : 1.0f;
            if (wn["NormalMap"]) {
                const std::string texPath = wn["NormalMap"].as<std::string>("");
                if (!texPath.empty()) {
                    wc.NormalMap = Renderer::GetAssetManager().LoadTextureAsync(texPath);
                }
            }
            if (wn["ColorMap"]) {
                const std::string texPath = wn["ColorMap"].as<std::string>("");
                if (!texPath.empty()) {
                    wc.ColorMap = Renderer::GetAssetManager().LoadTextureAsync(texPath);
                }
            }
            if (wn["Waves"]) {
                for (int i = 0; i < 4; ++i) {
                    YAML::Node waveNode = wn["Waves"][std::to_string(i)];
                    if (!waveNode) {
                        continue;
                    }
                    auto &w = wc.Waves[i];
                    w.Direction = DeserializeVec2(waveNode["Direction"], {1.0f, 0.0f});
                    w.Amplitude = waveNode["Amplitude"] ? waveNode["Amplitude"].as<float>(0.5f) : 0.5f;
                    w.Wavelength = waveNode["Wavelength"] ? waveNode["Wavelength"].as<float>(8.0f) : 8.0f;
                    w.Speed = waveNode["Speed"] ? waveNode["Speed"].as<float>(1.2f) : 1.2f;
                }
            }
        }

        // ---- BoxColliderComponent ----
        if (entityNode["BoxCollider"]) {
            YAML::Node boxNode = entityNode["BoxCollider"];
            auto &bcc = entity.AddComponent<BoxColliderComponent>();

            bcc.HalfExtents = DeserializeVec3(boxNode["HalfExtents"], {0.5f, 0.5f, 0.5f});
            bcc.Offset = DeserializeVec3(boxNode["Offset"], {0.0f, 0.0f, 0.0f});
            bcc.DrawDebug = boxNode["DrawDebug"] ? boxNode["DrawDebug"].as<bool>(true) : true;
        }

        // ---- BoundingBoxComponent ----
        if (entityNode["BoundingBox"]) {
            YAML::Node bbNode = entityNode["BoundingBox"];
            auto &bb = entity.AddComponent<BoundingBoxComponent>();

            bb.Center = DeserializeVec3(bbNode["Center"], {0.0f, 0.0f, 0.0f});
            bb.Size = DeserializeVec3(bbNode["Size"], {0.0f, 0.0f, 0.0f});
        }

        // ---- SphereColliderComponent ----
        if (entityNode["SphereCollider"]) {
            YAML::Node sphereNode = entityNode["SphereCollider"];
            auto &scc = entity.AddComponent<SphereColliderComponent>();

            scc.Radius = sphereNode["Radius"] ? sphereNode["Radius"].as<float>(0.5f) : 0.5f;
            scc.Offset = DeserializeVec3(sphereNode["Offset"], {0.0f, 0.0f, 0.0f});
            scc.DrawDebug = sphereNode["DrawDebug"] ? sphereNode["DrawDebug"].as<bool>(true) : true;
        }

        // ---- CapsuleColliderComponent ----
        if (entityNode["CapsuleCollider"]) {
            YAML::Node capsuleNode = entityNode["CapsuleCollider"];
            auto &ccc = entity.AddComponent<CapsuleColliderComponent>();

            ccc.Radius = capsuleNode["Radius"] ? capsuleNode["Radius"].as<float>(0.5f) : 0.5f;
            ccc.HalfHeight = capsuleNode["HalfHeight"] ? capsuleNode["HalfHeight"].as<float>(0.5f) : 0.5f;
            ccc.Axis = static_cast<CapsuleAxis>(capsuleNode["Axis"] ? capsuleNode["Axis"].as<int>(0) : 0);
            ccc.Offset = DeserializeVec3(capsuleNode["Offset"], {0.0f, 0.0f, 0.0f});
            ccc.DrawDebug = capsuleNode["DrawDebug"] ? capsuleNode["DrawDebug"].as<bool>(true) : true;
        }

        // ---- JointComponent（骨骼关节标记）----
        if (entityNode["Joint"]) {
            const int jointIndex = entityNode["Joint"]["JointIndex"]
                                       ? entityNode["Joint"]["JointIndex"].as<int>(0)
                                       : 0;
            entity.AddComponent<JointComponent>(jointIndex);
        }

        // ---- SkinComponent：延迟到 Skins 表重建后回填共享 SkinDef ----
        if (entityNode["Skin"]) {
            PendingSkinComponent p;
            p.entity = entity;
            p.skinId = entityNode["Skin"]["Id"] ? entityNode["Skin"]["Id"].as<std::string>("")
                                                : std::string();
            p.meshPath = entityNode["Skin"]["Mesh"] ? entityNode["Skin"]["Mesh"].as<std::string>("")
                                                    : std::string();
            pendingSkins.push_back(p);
        }

        // ---- AnimationComponent：延迟到全部实体建成后回填 channelTargets ----
        if (entityNode["Animation"]) {
            PendingAnimation p;
            p.entity = entity;
            p.active = entityNode["Animation"]["Active"]
                           ? entityNode["Animation"]["Active"].as<size_t>(0) : 0;
            p.time = entityNode["Animation"]["Time"]
                         ? entityNode["Animation"]["Time"].as<float>(0.0f) : 0.0f;
            p.speed = entityNode["Animation"]["Speed"]
                          ? entityNode["Animation"]["Speed"].as<float>(1.0f) : 1.0f;
            p.playing = entityNode["Animation"]["Playing"]
                            ? entityNode["Animation"]["Playing"].as<bool>(true) : true;
            p.loop = entityNode["Animation"]["Loop"]
                         ? entityNode["Animation"]["Loop"].as<bool>(true) : true;
            if (entityNode["Animation"]["Clips"] && entityNode["Animation"]["Clips"].IsSequence()) {
                for (const auto &clipNode : entityNode["Animation"]["Clips"]) {
                    PendingAnimationClip pc;
                    pc.clipKey = clipNode["Clip"] ? clipNode["Clip"].as<std::string>("")
                                                  : std::string();
                    if (clipNode["Targets"] && clipNode["Targets"].IsSequence()) {
                        for (const auto &tu : clipNode["Targets"]) {
                            pc.targetIds.push_back(tu.as<std::string>());
                        }
                    }
                    if (clipNode["Events"] && clipNode["Events"].IsSequence()) {
                        for (const auto &en : clipNode["Events"]) {
                            AnimationEvent e;
                            e.time = en["Time"] ? en["Time"].as<float>(0.0f) : 0.0f;
                            e.name = en["Name"] ? en["Name"].as<std::string>("") : std::string();
                            pc.events.push_back(std::move(e));
                        }
                    }
                    p.clips.push_back(std::move(pc));
                }
            }
            pendingAnimations.push_back(std::move(p));
        }

        // ---- ScriptComponent ----
        // 反序列化即直接挂载：AddComponent 触发 OnComponentAdded → ScriptEngine 建实例。
        // 脚本文件缺失 → ScriptEngine 发 warn 并置 Enabled=false（§9.6），不打断加载。
        if (entityNode["Script"] && entityNode["Script"]["ScriptPath"]) {
            auto &sc = entity.AddComponent<ScriptComponent>(
                entityNode["Script"]["ScriptPath"].as<std::string>());
            sc.Enabled = entityNode["Script"]["Enabled"]
                             ? entityNode["Script"]["Enabled"].as<bool>()
                             : true;
            // public 字段回读（AddComponent 已触发一次挂载；public.get 实时读组件，loaded 值下一帧生效，
            // 序列化时使用本块覆盖的最终值落盘）
            if (entityNode["Script"]["PublicFields"]) {
                const YAML::Node pfNode = entityNode["Script"]["PublicFields"];
                for (auto it = pfNode.begin(); it != pfNode.end(); ++it) {
                    const std::string name = it->first.as<std::string>();
                    const YAML::Node fn = it->second;
                    ScriptPublicField field;
                    field.Type = ScriptFieldTypeFromName(fn["Type"]
                                                             ? fn["Type"].as<std::string>()
                                                             : std::string("number"));
                    switch (field.Type) {
                    case ScriptFieldType::Number: field.Number = fn["Value"].as<float>(0.0f); break;
                    case ScriptFieldType::Bool:   field.Bool   = fn["Value"].as<bool>(false); break;
                    case ScriptFieldType::String: field.String = fn["Value"].as<std::string>(std::string()); break;
                    default: break;
                    }
                    sc.PublicFields[name] = field;
                }
            }
        }

        // ---- AnimStateMachineComponent（状态/转换/条件全在组件内部自洽，随实体一并反序列化）----
        // From/To 以状态名解析回下标（"*" = ANY）；引用不存在的状态名 → 跳过该转换容错。
        // clipName 暂以字符串存，运行时按 AnimationComponent.clips 查 name 解析下标（阶段 B，不在本序列化器内）。
        if (entityNode["AnimStateMachine"]) {
            YAML::Node asmNode = entityNode["AnimStateMachine"];
            auto &asmc = entity.AddComponent<AnimStateMachineComponent>();

            asmc.enabled = asmNode["Enabled"] ? asmNode["Enabled"].as<bool>(false) : false;
            asmc.initialState = asmNode["Initial"] ? asmNode["Initial"].as<std::string>()
                                                   : std::string();

            if (asmNode["States"] && asmNode["States"].IsSequence()) {
                for (const auto &sn : asmNode["States"]) {
                    AnimStateDef st;
                    st.name = sn["Name"] ? sn["Name"].as<std::string>() : std::string();
                    st.clipName = sn["Clip"] ? sn["Clip"].as<std::string>() : std::string();
                    st.loop = sn["Loop"] ? sn["Loop"].as<bool>(true) : true;
                    st.speed = sn["Speed"] ? sn["Speed"].as<float>(1.0f) : 1.0f;
                    asmc.states.push_back(std::move(st));
                }
            }

            // 状态名 → 下标映射（From/To 解析用）
            std::unordered_map<std::string, size_t> stateIndex;
            for (size_t i = 0; i < asmc.states.size(); ++i) {
                stateIndex[asmc.states[i].name] = i;
            }

            if (asmNode["Transitions"] && asmNode["Transitions"].IsSequence()) {
                for (const auto &tn : asmNode["Transitions"]) {
                    AnimTransitionDef tr;
                    const std::string fromName = tn["From"] ? tn["From"].as<std::string>()
                                                            : std::string();
                    const std::string toName = tn["To"] ? tn["To"].as<std::string>()
                                                         : std::string();

                    if (fromName.empty() || fromName == "*") {
                        tr.from = SIZE_MAX; // ANY 全局转换
                    } else {
                        auto it = stateIndex.find(fromName);
                        if (it == stateIndex.end()) {
                            GE_CORE_WARN("SceneSerializer: 状态机转换 From 状态 '{0}' 不存在，跳过该转换",
                                         fromName);
                            continue;
                        }
                        tr.from = it->second;
                    }
                    auto itTo = stateIndex.find(toName);
                    if (itTo == stateIndex.end()) {
                        GE_CORE_WARN("SceneSerializer: 状态机转换 To 状态 '{0}' 不存在，跳过该转换", toName);
                        continue;
                    }
                    tr.to = itTo->second;
                    tr.blendSec = tn["BlendSec"] ? tn["BlendSec"].as<float>(0.25f) : 0.25f;

                    if (tn["Conditions"] && tn["Conditions"].IsSequence()) {
                        for (const auto &cn : tn["Conditions"]) {
                            AnimCondition cond;
                            const std::string typeStr = cn["Type"] ? cn["Type"].as<std::string>()
                                                                   : std::string();
                            cond.type = AnimConditionTypeFromName(typeStr);
                            switch (cond.type) {
                            case AnimCondition::Type::FloatCmp:
                                cond.param = cn["Param"] ? cn["Param"].as<std::string>()
                                                          : std::string();
                                cond.cmp = AnimConditionCmpFromName(cn["Cmp"]
                                                                         ? cn["Cmp"].as<std::string>()
                                                                         : std::string());
                                cond.value = cn["Value"] ? cn["Value"].as<float>(0.0f) : 0.0f;
                                break;
                            case AnimCondition::Type::Bool:
                                cond.param = cn["Param"] ? cn["Param"].as<std::string>()
                                                          : std::string();
                                cond.expect = cn["Expect"] ? cn["Expect"].as<bool>(true) : true;
                                break;
                            case AnimCondition::Type::StateTime:
                                cond.value = cn["Value"] ? cn["Value"].as<float>(0.0f) : 0.0f;
                                break;
                            case AnimCondition::Type::StateEnded:
                                break;
                            }
                            tr.conditions.push_back(std::move(cond));
                        }
                    }
                    asmc.transitions.push_back(std::move(tr));
                }
            }
        }

        entityCount++;
    }

    // ---- 第二遍：按父实体 UUID 二次遍历接上父子关系（SetParent 内部带环检测） ----
    for (auto &link : pendingParentLinks) {
        auto it = idToEntity.find(link.parentId);
        if (it == idToEntity.end()) {
            GE_CORE_WARN("SceneSerializer: 找不到父实体 Id {0}，实体 {1} 将作为根处理",
                         link.parentId, link.child.GetComponent<TagComponent>().Tag);
            continue;
        }
        if (!m_Scene->SetParent(link.child, it->second)) {
            GE_CORE_WARN("SceneSerializer: 设置父子关系失败（{0} -> {1}），可能构成环引用",
                         link.child.GetComponent<TagComponent>().Tag,
                         it->second.GetComponent<TagComponent>().Tag);
        }
    }

    // ---- 第三遍：重建全局皮肤定义表（Skins）+ 回填各实体 SkinComponent ----
    // 皮肤表 Joints 是实体 UUID，需全部实体建成（idToEntity 齐全）后解析为句柄；
    // 共享 SkinDef 重建后，引用同一皮肤的多实体重新共享同一 shared_ptr（共享保持）。
    std::unordered_map<std::string, std::shared_ptr<SkinDef>> idToSkinDef;
    YAML::Node skinsNode = sceneNode["Skins"];
    if (skinsNode && skinsNode.IsSequence()) {
        for (const auto &skinNode : skinsNode) {
            const std::string sid = skinNode["Id"] ? skinNode["Id"].as<std::string>("")
                                                   : std::string();
            if (sid.empty()) {
                continue;
            }
            auto skinDef = std::make_shared<SkinDef>();

            if (skinNode["Joints"] && skinNode["Joints"].IsSequence()) {
                for (const auto &ju : skinNode["Joints"]) {
                    const std::string juid = ju.as<std::string>();
                    auto it = idToEntity.find(juid);
                    if (it == idToEntity.end()) {
                        GE_CORE_WARN("SceneSerializer: 皮肤 {} 关节实体 {} 找不到，已跳过", sid, juid);
                        continue;
                    }
                    skinDef->joints.push_back(static_cast<entt::entity>(it->second));
                }
            }

            if (skinNode["InverseBindMatrices"] && skinNode["InverseBindMatrices"].IsSequence()) {
                for (const auto &rowNode : skinNode["InverseBindMatrices"]) {
                    if (!rowNode.IsSequence() || rowNode.size() < 16) {
                        GE_CORE_WARN("SceneSerializer: 皮肤 {} 的 IBM 行尺寸不足 16，已跳过", sid);
                        continue;
                    }
                    glm::mat4 m(1.0f);
                    int k = 0;
                    for (int col = 0; col < 4; ++col) {
                        for (int row = 0; row < 4; ++row) {
                            m[col][row] = rowNode[static_cast<size_t>(k++)].as<float>();
                        }
                    }
                    skinDef->inverseBindMatrices.push_back(m);
                }
            }

            if (skinDef->inverseBindMatrices.size() != skinDef->joints.size()) {
                GE_CORE_WARN("SceneSerializer: 皮肤 {} 关节数({})与 IBM 数({})不一致，加载后退化为静态",
                             sid, skinDef->joints.size(), skinDef->inverseBindMatrices.size());
            }
            idToSkinDef[sid] = skinDef;
        }
    }

    // 回填：给每个带 Skin 引用的实体挂上共享 SkinDef（+ 可选 MeshPtr 供面板展示）
    for (auto &p : pendingSkins) {
        auto &sc = p.entity.AddComponent<SkinComponent>();
        auto it = idToSkinDef.find(p.skinId);
        if (it != idToSkinDef.end()) {
            sc.skin = it->second;
        } else {
            GE_CORE_WARN("SceneSerializer: 实体 {} 引用的皮肤 {} 未定义",
                         p.entity.GetComponent<TagComponent>().Tag, p.skinId);
        }
        if (!p.meshPath.empty()) {
            sc.MeshPtr = Renderer::GetAssetManager().LoadMesh(p.meshPath);
        }
    }

    // ---- 回填动画：经 AnimationClipManager 取共享 clip，Targets UUID → 实体句柄 ----
    // Clip 源键缺失（旧场景文件）/ 源文件缺失 / 无合法 channel 时跳过该动画，
    // 所在骨架退化为绑定姿态，不崩溃（兼容规则见计划书 §4 阶段 C1）。
    for (auto &p : pendingAnimations) {
        auto &ac = p.entity.AddComponent<AnimationComponent>();
        for (const auto &pc : p.clips) {
            std::shared_ptr<AnimationClip> clip =
                pc.clipKey.empty() ? nullptr : AnimationClipManager::Get().LoadByKey(pc.clipKey);
            if (!clip) {
                GE_CORE_WARN("SceneSerializer: 实体 {} 动画 clip '{}' 加载失败，跳过（退化为绑定姿态）",
                             p.entity.GetComponent<TagComponent>().Tag, pc.clipKey);
                continue;
            }
            ClipInstance inst;
            inst.clip = clip;
            inst.channelTargets.reserve(pc.targetIds.size());
            for (const auto &tid : pc.targetIds) {
                auto it = idToEntity.find(tid);
                inst.channelTargets.push_back(it != idToEntity.end()
                                                  ? static_cast<entt::entity>(it->second)
                                                  : entt::null);
            }
            inst.keyHints.assign(clip->channels.size(), 0u); // 采样键帧下界缓存初始化（阶段 A）
            inst.events = pc.events; // 场景级事件表回填（缺省为空，旧场景文件照常）
            ac.clips.push_back(std::move(inst));
        }
        ac.active = (p.active < ac.clips.size()) ? p.active : 0;
        ac.time = p.time;
        ac.speed = p.speed;
        ac.playing = p.playing;
        ac.loop = p.loop;
    }

    GE_CORE_INFO("SceneSerializer: 场景已从 {} 加载（{} 个实体）",
                 filepath, entityCount);

    return true;
}

} // namespace GE
