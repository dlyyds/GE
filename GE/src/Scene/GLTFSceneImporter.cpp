/**
 * @file GLTFSceneImporter.cpp
 * @brief glTF 场景图导入器实现 —— 先建导入根实体，再遍历 node 树建实体、
 *        填变换、建父子、挂网格，整棵树统一挂在根实体下。
 */

#include "pch.h"

#include "Scene/GLTFSceneImporter.h"

#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Render/GLTFLoader.h"
#include "Render/AnimationClipManager.h"
#include "Render/MeshManager.h"
#include "Render/Mesh.h"
#include "Core/Log.h"

#include "tinygltf/tiny_gltf.h"

#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <cmath>

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

    // ── 导入根实体：整棵 glTF 场景图统一挂到一个根实体下 ──
    // 根实体以文件名（去扩展名）命名，自身为普通空实体（单位变换、无网格/皮肤），
    // 所有 glTF 顶层 node 都作为其子实体。好处：层级面板可整体选中/拖动/删除整份
    // 导入结果；多顶层 node（多 root 的 glTF）不再散落场景顶层；动画宿主上溯到此根
    // 即停（§9.3 挂载点不影响动画，见第三遍注释）。根实体单位变换不影响世界矩阵，
    // 后续世界尺度归一化对每网格实体局部 Scale 的补偿也天然不受影响。
    const std::string rootName = std::filesystem::path(filepath).stem().string();
    Entity gltfRoot = scene.CreateEntity(rootName.empty() ? "glTF Root" : rootName);

    // DFS：递归建实体树（parent 为空 = 根，本导入统一传 gltfRoot）
    // 先声明后赋值的 std::function，规避 MSVC 对「自引用 lambda 与声明同处一行」的解析问题
    // nodeEntities：node 索引 → 实体，供第二遍把 skin.joints 的 node 解析成关节实体句柄
    // skinNodes：带 skin 的 node（node 索引, skin 索引），供第二遍回填 SkinComponent
    std::unordered_map<int, Entity> nodeEntities;
    std::vector<std::pair<int, int>> skinNodes;
    std::vector<entt::entity> meshNodeEntities; // 本导入带网格的实体（供世界尺度归一化）
    entt::entity firstSkinHost = entt::null; // 本导入首个带皮肤宿主实体（动画挂载点）
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
                meshNodeEntities.push_back(static_cast<entt::entity>(entity));
                // 带 mesh + skin 的 node：挂 SkinComponent（joints/IBM 在第二遍回填），
                // MeshPtr 在此即席填好，便于第二遍与绘制时定位网格。
                if (node.skin >= 0) {
                    auto &sc = entity.AddComponent<SkinComponent>();
                    sc.MeshPtr = mesh;
                    if (firstSkinHost == entt::null) {
                        firstSkinHost = static_cast<entt::entity>(entity);
                    }
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
        if (buildNode(r, gltfRoot)) {
            anyCreated = true;
        }
    }

    // ── 世界尺度归一化：抵消导出残留的整体缩放 ──
    // 有些 Sketchfab/旧 DCC 导出会在节点链上带一个大统一缩放（如 lacrimosa 全链
    // 109.7 倍），角色被放大到约 90 世界单位、默认相机（轨道距离 8）陷入几何体内部，
    // 背面剔除全灭 → 完全不可见。此处对每个网格实体：取其世界矩阵 3×3 行列式的
    // 立方根作为统一缩放 S，S 明显 ≠ 1 时把该实体局部 Scale ÷ S，使世界尺度归一到 ≈1。
    // 只改网格实体自身、不动关节链：蒙皮 sx = jointWorld × IBM 对全局统一缩放不变，
    // 静态网格 / 蒙皮 / 动画都不受影响；mint / valhalla 等 S=1 的模型完全不变。
    if (!meshNodeEntities.empty()) {
        // 导入期 worldMatrix 缓存未就绪，自旋走 parent 链合成世界矩阵
        auto worldOf = [&](entt::entity e) {
            glm::mat4 w(1.0f);
            entt::entity cur = e;
            while (cur != entt::null) {
                auto *tc = scene.Reg().try_get<TransformComponent>(cur);
                if (!tc) {
                    break;
                }
                w = tc->GetLocalMatrix() * w;
                cur = tc->parent;
            }
            return w;
        };
        for (entt::entity e : meshNodeEntities) {
            auto *tc = scene.Reg().try_get<TransformComponent>(e);
            if (!tc) {
                continue;
            }
            const float det = glm::determinant(worldOf(e)); // 3×3 行列式（镜像为负）
            const float scale = std::cbrt(std::fabs(det));   // 等比统一缩放倍数
            if (scale > 1e-6f && std::fabs(scale - 1.0f) > 0.001f) {
                tc->Scale *= (1.0f / scale);
                GE_CORE_INFO("[GLTF] 网格实体世界缩放 {:.4f} × → 归一化到 1", scale);
            }
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

    // ── 第三遍：动画接入（实体树建成 + 层级接好后才能解析目标节点句柄） ──
    //   规则（计划书 §4 阶段 B1）：v1 仅支持有皮肤的角色动画——宿主取本导入首个
    //   带 SkinComponent 的实体，再沿 parent 链上溯到本导入根实体（gltfRoot）为止，
    //   把 AnimationComponent 挂在根实体上（层级面板更好找）；驱动仍经 channelTargets
    //   打目标节点局部 TRS，经 DFS 传播到子树全部关节，挂载点不影响动画（§9.3）。
    //   无皮肤的对象动画列阶段 D。
    if (!model.animations.empty()) {
        if (firstSkinHost == entt::null) {
            GE_CORE_WARN("[Anim] {} 含动画但无皮肤宿主（对象动画列入阶段 D），跳过",
                         filepath);
        } else {
            Entity hostEntity{firstSkinHost, &scene};
            // 向上追溯本导入子树根：停在 parent 为空（glTF scene 根）为止
            {
                Entity ancestor = hostEntity;
                while (Entity parent = scene.GetParent(ancestor)) {
                    ancestor = parent;
                }
                hostEntity = ancestor;
            }
            if (!hostEntity.HasComponent<AnimationComponent>()) { // 防重复导入叠加
                auto &animComp = hostEntity.AddComponent<AnimationComponent>();
                for (size_t ai = 0; ai < model.animations.size(); ++ai) {
                    std::shared_ptr<AnimationClip> clip =
                        AnimationClipManager::Get().Load(filepath, ai, model);
                    if (!clip) { // 无合法 channel（解析失败 / 全被跳过）
                        continue;
                    }
                    ClipInstance inst;
                    inst.clip = clip;
                    inst.channelTargets.reserve(clip->channels.size());
                    size_t validTargets = 0;
                    // 逐 channel 把目标 node 解析为实体句柄：目标未导入或无 Transform
                    // 则洞掉（该通道不驱动，该部位退化为绑定姿态），与皮肤回填同法容错。
                    for (const auto &ch : clip->channels) {
                        auto it = nodeEntities.find(ch.nodeIndex);
                        if (it != nodeEntities.end() && it->second
                            && it->second.HasComponent<TransformComponent>()) {
                            inst.channelTargets.push_back(static_cast<entt::entity>(it->second));
                            ++validTargets;
                        } else {
                            inst.channelTargets.push_back(entt::null);
                        }
                    }
                    inst.keyHints.assign(clip->channels.size(), 0u); // 采样键帧下界缓存初始化（阶段 A）
                    animComp.clips.push_back(std::move(inst));
                    GE_CORE_INFO("[Anim] 动画[{}] '{}' 已接入: {} channel, 时长 {:.3f}s, "
                                 "有效目标 {} 个",
                                 ai, clip->name, clip->channels.size(), clip->duration,
                                 validTargets);
                }
            }
        }
    }

    if (!anyCreated) {
        GE_CORE_WARN("[GLTF] 未创建任何实体: {}", filepath);
        scene.DestroyEntity(gltfRoot); // 一个 node 都没建出来，一并回收导入根实体
        return false;
    }
    GE_CORE_INFO("[GLTF] 场景导入完成: {}（{} node）", filepath, model.nodes.size());
    return true;
}

} // namespace GE
