/**
 * @file GLTFSceneImporter.cpp
 * @brief glTF 场景图导入器实现 —— 遍历 node 树建实体、填变换、建父子、挂网格。
 */

#include "pch.h"

#include "Scene/GLTFSceneImporter.h"

#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Render/GLTFLoader.h"
#include "Render/MeshManager.h"
#include "Render/Mesh.h"
#include "Core/Log.h"

#include "tinygltf/tiny_gltf.h"

#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <unordered_map>

namespace GE {

namespace {

/**
 * @brief 把 glTF node 的变换（matrix 或 TRS）填写进 TransformComponent。
 *
 * 引擎 TransformComponent 用四元数存旋转，glTF node 给的 matrix 需分解为
 * Translation/quat/Scale；TRS 则直接读取。glTF 与引擎同为右手系 + Y-up，无需翻转。
 */
void FillTransform(TransformComponent &tc, const tinygltf::Node &node) {
    tc.Translation = {0.0f, 0.0f, 0.0f};
    tc.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    tc.Scale = {1.0f, 1.0f, 1.0f};

    if (node.matrix.size() == 16) {
        // glTF matrix 为列主序（column-major）16 个 double
        glm::mat4 m(1.0f);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
            }
        }
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(m, tc.Scale, tc.Rotation, tc.Translation, skew, perspective);
        return;
    }

    if (node.translation.size() >= 3) {
        tc.Translation = {static_cast<float>(node.translation[0]),
                          static_cast<float>(node.translation[1]),
                          static_cast<float>(node.translation[2])};
    }
    if (node.rotation.size() >= 4) {
        // glTF 旋转四元数 [x,y,z,w]，glm::quat 为 (w,x,y,z)
        tc.Rotation = glm::quat(static_cast<float>(node.rotation[3]),
                                static_cast<float>(node.rotation[0]),
                                static_cast<float>(node.rotation[1]),
                                static_cast<float>(node.rotation[2]));
    }
    if (node.scale.size() >= 3) {
        tc.Scale = {static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]),
                    static_cast<float>(node.scale[2])};
    }
}

/**
 * @brief 读取 skin 的逆绑定矩阵（FLOAT mat4 accessor）。
 *
 * glTF IBM accessor 为 MAT4/FLOAT，每元素 16 个 float（列主序），与 glm::mat4
 * 内存布局一致，可直接 memcpy。bufferView/buffer 越界、类型不符或 stride 异常
 * 时返回空向量，调用方容错跳过该皮肤。
 */
std::vector<glm::mat4> ReadInverseBindMatrices(const tinygltf::Model &model, int accessorIdx) {
    std::vector<glm::mat4> out;
    if (accessorIdx < 0 || accessorIdx >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const tinygltf::Accessor &acc = model.accessors[static_cast<size_t>(accessorIdx)];
    if (acc.type != TINYGLTF_TYPE_MAT4 || acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
        GE_CORE_WARN("[GLTF] 逆绑定矩阵 accessor 类型必须为 MAT4/FLOAT");
        return out;
    }
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return out;
    }
    const tinygltf::BufferView &bv = model.bufferViews[static_cast<size_t>(acc.bufferView)];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size())) {
        return out;
    }
    const auto &buf = model.buffers[static_cast<size_t>(bv.buffer)];
    const int byteStride = acc.ByteStride(bv);
    if (byteStride < static_cast<int>(sizeof(float) * 4)) {
        // 一个 mat4 至少 64 字节，stride 异常视为数据损坏
        return out;
    }
    const size_t base = static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(acc.byteOffset);
    if (base >= buf.data.size() ||
        acc.count > (buf.data.size() - base) / static_cast<size_t>(byteStride)) {
        return out;
    }
    out.reserve(acc.count);
    for (size_t i = 0; i < acc.count; ++i) {
        glm::mat4 m(1.0f);
        std::memcpy(&m, buf.data.data() + base + i * static_cast<size_t>(byteStride),
                    sizeof(glm::mat4));
        out.push_back(m);
    }
    return out;
}

} // namespace

bool GLTFSceneImporter::Import(Scene &scene, MeshManager &meshManager,
                               const std::string &filepath) {
    tinygltf::Model model;
    std::string err;
    if (!GLTF::LoadModel(filepath, model, &err)) {
        GE_CORE_ERROR("[GLTF] 场景导入失败 '{}': {}", filepath, err);
        return false;
    }
    if (model.nodes.empty()) {
        GE_CORE_WARN("[GLTF] 场景无 node: {}", filepath);
        return false;
    }

    // 找出根节点：未被任何 node 作为 child 引用的节点
    std::vector<bool> isChild(model.nodes.size(), false);
    for (const auto &n : model.nodes) {
        for (int c : n.children) {
            if (c >= 0 && c < static_cast<int>(model.nodes.size())) {
                isChild[static_cast<size_t>(c)] = true;
            }
        }
    }

    // 优先 defaultScene 的 node 列表；否则取全部非 child 节点为根
    std::vector<int> roots;
    const int sceneIdx = model.defaultScene;
    if (sceneIdx >= 0 && sceneIdx < static_cast<int>(model.scenes.size())) {
        roots = model.scenes[static_cast<size_t>(sceneIdx)].nodes;
    } else {
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (!isChild[i]) {
                roots.push_back(static_cast<int>(i));
            }
        }
    }

    // DFS：递归建实体树（parent 为空 = 根）
    // 先声明后赋值的 std::function，规避 MSVC 对「自引用 lambda 与声明同处一行」的解析问题
    // nodeEntities：node 索引 → 实体，供第二遍把 skin.joints 的 node 解析成关节实体句柄
    // skinNodes：带 skin 的 node（node 索引, skin 索引），供第二遍回填 SkinComponent
    std::unordered_map<int, Entity> nodeEntities;
    std::vector<std::pair<int, int>> skinNodes;
    std::function<Entity(int, Entity)> buildNode;
    buildNode = [&](int nodeIdx, Entity parent) -> Entity {
        if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) {
            return Entity{};
        }
        const tinygltf::Node &node = model.nodes[static_cast<size_t>(nodeIdx)];

        const std::string name = node.name.empty()
            ? ("glTFNode_" + std::to_string(nodeIdx)) : node.name;
        Entity entity = scene.CreateEntity(name);
        nodeEntities[nodeIdx] = entity;
        FillTransform(entity.GetComponent<TransformComponent>(), node);
        if (parent) {
            scene.SetParent(entity, parent);
        }

        if (node.mesh >= 0) {
            // 复用本函数顶部已 LoadModel 的 model，避免每个 mesh 重新读盘解析
            Mesh *mesh = meshManager.LoadGLTFMesh(filepath, static_cast<size_t>(node.mesh), model);
            if (mesh) {
                entity.AddComponent<MeshRendererComponent>(mesh);
                // 带 mesh + skin 的 node：挂 SkinComponent（joints/IBM 在第二遍回填），
                // MeshPtr 在此即席填好，便于第二遍与绘制时定位网格。
                if (node.skin >= 0) {
                    auto &sc = entity.AddComponent<SkinComponent>();
                    sc.MeshPtr = mesh;
                    skinNodes.emplace_back(nodeIdx, node.skin);
                }
            } else {
                GE_CORE_WARN("[GLTF] node '{}' 的 mesh {} 加载失败", name, node.mesh);
            }
        }

        for (int child : node.children) {
            buildNode(child, entity);
        }
        return entity;
    };

    bool anyCreated = false;
    for (int r : roots) {
        if (r < 0 || r >= static_cast<int>(model.nodes.size())) {
            continue;
        }
        if (buildNode(r, Entity{})) {
            anyCreated = true;
        }
    }

    // ── 第二遍：回填蒙皮引用（必须在全部实体建成、层级接好后才能接句柄） ──
    //   否则关节实体可能还没建出来、句柄无效。分两遍是硬性顺序约束。
    for (size_t sk = 0; sk < model.skins.size(); ++sk) {
        const tinygltf::Skin &skin = model.skins[sk];
        if (skin.joints.empty()) {
            continue;
        }

        // 解析 IBM（常量，配合 joints 使用，与 joints 一一对应）
        std::vector<glm::mat4> ibm = ReadInverseBindMatrices(model, skin.inverseBindMatrices);
        if (ibm.size() != skin.joints.size()) {
            GE_CORE_WARN("[GLTF] skin[{}] 的逆绑定矩阵数量({}) 与关节数({}) 不一致，跳过",
                         sk, ibm.size(), skin.joints.size());
            continue;
        }

        // 把 skin.joints 各 node 解析为实体句柄 + 给关节实体挂 JointComponent
        std::vector<entt::entity> jointHandles;
        jointHandles.reserve(skin.joints.size());
        for (size_t j = 0; j < skin.joints.size(); ++j) {
            const int jointNodeIdx = skin.joints[j];
            auto it = nodeEntities.find(jointNodeIdx);
            if (it == nodeEntities.end() || !it->second) {
                // 空洞校验：关节 node 找不到实体时容错跳过（该关节不参与驱动）
                GE_CORE_WARN("[GLTF] skin[{}] 关节 node {} 未找到实体，跳过", sk, jointNodeIdx);
                continue;
            }
            Entity jointEntity = it->second;
            // 同一关节 node 可能被多个 skin 引用：只允许挂一次 JointComponent
            if (!jointEntity.HasComponent<JointComponent>()) {
                jointEntity.AddComponent<JointComponent>(static_cast<int>(j));
            }
            jointHandles.push_back(static_cast<entt::entity>(jointEntity));
        }
        if (jointHandles.size() != skin.joints.size()) {
            GE_CORE_WARN("[GLTF] skin[{}] 有效关节 {} / {} 个，部分空洞，按缺省处理", sk,
                         jointHandles.size(), skin.joints.size());
        }

        // 创建共享皮肤定义：同一条 glTF skin 只建一份 SkinDef，
        // 后续所有引用本皮肤的 node 的 SkinComponent 共享同一 shared_ptr，
        // 关节链与 IBM 只存一份，运行时也只需算一次上传一次。
        auto skinDef = std::make_shared<SkinDef>();
        skinDef->joints = std::move(jointHandles);
        skinDef->inverseBindMatrices = std::move(ibm);

        // 回填所有引用本皮肤节点的 SkinComponent（共享同一 SkinDef）
        for (const auto &[nodeIdx, skinIdx] : skinNodes) {
            if (skinIdx != static_cast<int>(sk)) {
                continue;
            }
            auto nit = nodeEntities.find(nodeIdx);
            if (nit == nodeEntities.end() || !nit->second) {
                continue;
            }
            auto &sc = nit->second.GetComponent<SkinComponent>();
            sc.skin = skinDef;
            sc.RequiresJointUpload = true;
            GE_CORE_INFO("[GLTF] skin[{}] 已接入：{} 个关节, 网格 = {}",
                         sk, skinDef->joints.size(),
                         sc.MeshPtr ? "mesh" : "无");
        }
    }

    // ── 阶段 A 临时验证：打印动画数据概览 ──
    //   阶段 B 将把本块替换为「挂 AnimationComponent + 解析 channelTargets」，
    //   当前只读不改，用于验收「1 条动画 / ~980 channel / 时长 24.1s / 全 LINEAR」。
    if (!model.animations.empty()) {
        GE_CORE_INFO("[Anim] {} 含 {} 条动画, 共 {} node / {} skin",
                     filepath, model.animations.size(), model.nodes.size(), model.skins.size());
        for (size_t ai = 0; ai < model.animations.size(); ++ai) {
            AnimationClip clip;
            std::string aerr;
            if (!GLTF::BuildAnimations(model, ai, clip, &aerr)) {
                GE_CORE_ERROR("[Anim] 动画[{}] 解析失败: {}", ai, aerr);
                continue;
            }
            // 统计插值分布与键帧总量
            size_t nLinear = 0, nStep = 0, nCubic = 0, keyTotal = 0;
            for (const auto &c : clip.channels) {
                switch (c.interp) {
                case AnimationChannel::Interp::Step: nStep++; break;
                case AnimationChannel::Interp::CubicSpline: nCubic++; break;
                default: nLinear++; break;
                }
                keyTotal += c.times.size();
            }
            GE_CORE_INFO("[Anim]   动画[{}] '{}': channels={}, 时长={:.3f}s, "
                         "插值 LINEAR={} STEP={} CUBIC={}, 键帧总数={}",
                         ai, clip.name, clip.channels.size(), clip.duration,
                         nLinear, nStep, nCubic, keyTotal);
            // 抽查首条合法 channel 的时间轴首尾与插值类型
            for (const auto &c : clip.channels) {
                if (c.times.size() < 2) {
                    continue;
                }
                const char *pathStr = (c.path == AnimationChannel::Path::Translation) ? "translation"
                                    : (c.path == AnimationChannel::Path::Rotation) ? "rotation"
                                    : "scale";
                const char *interpStr = (c.interp == AnimationChannel::Interp::Linear) ? "LINEAR"
                                      : (c.interp == AnimationChannel::Interp::Step) ? "STEP"
                                      : "CUBICSPLINE";
                GE_CORE_INFO("[Anim]     抽查 channel: node={} path={} interp={} 键帧={}, "
                             "时间轴 [{:.3f}, {:.3f}]",
                             c.nodeIndex, pathStr, interpStr, c.times.size(),
                             c.times.front(), c.times.back());
                break;
            }
        }
    }

    if (!anyCreated) {
        GE_CORE_WARN("[GLTF] 未创建任何实体: {}", filepath);
        return false;
    }
    GE_CORE_INFO("[GLTF] 场景导入完成: {}（{} node）", filepath, model.nodes.size());
    return true;
}

} // namespace GE
