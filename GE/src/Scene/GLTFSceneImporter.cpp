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
    std::function<Entity(int, Entity)> buildNode;
    buildNode = [&](int nodeIdx, Entity parent) -> Entity {
        if (nodeIdx < 0 || nodeIdx >= static_cast<int>(model.nodes.size())) {
            return Entity{};
        }
        const tinygltf::Node &node = model.nodes[static_cast<size_t>(nodeIdx)];

        const std::string name = node.name.empty()
            ? ("glTFNode_" + std::to_string(nodeIdx)) : node.name;
        Entity entity = scene.CreateEntity(name);
        FillTransform(entity.GetComponent<TransformComponent>(), node);
        if (parent) {
            scene.SetParent(entity, parent);
        }

        if (node.mesh >= 0) {
            // 复用本函数顶部已 LoadModel 的 model，避免每个 mesh 重新读盘解析
            Mesh *mesh = meshManager.LoadGLTFMesh(filepath, static_cast<size_t>(node.mesh), model);
            if (mesh) {
                entity.AddComponent<MeshRendererComponent>(mesh);
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

    if (!anyCreated) {
        GE_CORE_WARN("[GLTF] 未创建任何实体: {}", filepath);
        return false;
    }
    GE_CORE_INFO("[GLTF] 场景导入完成: {}（{} node）", filepath, model.nodes.size());
    return true;
}

} // namespace GE
