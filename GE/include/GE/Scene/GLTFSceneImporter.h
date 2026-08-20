/**
 * @file GLTFSceneImporter.h
 * @brief glTF 场景图导入器 —— 把 glTF 当「场景文件」导入，保留 node 层级与变换。
 *
 * 与 .gemesh 完全解耦（见 `docs/glTF模型导入计划书.md` §6）：
 * - node 归本导入器：逐 node 建实体，填 Transform（TRS/矩阵 → Translation/quat/Scale），
 *   经 Scene::SetParent 保留层级，有 mesh 的 node 挂 MeshRendererComponent。
 * - mesh 归 MeshManager：多 node 复用同 mesh 时经 LoadGLTFMesh 的 "path#N" 键去重缓存，
 *   只产生一份 GPU 网格，多个 MeshRendererComponent 共享同一 Mesh*。
 */

#pragma once

#include <string>

namespace GE {

class Scene;
class MeshManager;

class GLTFSceneImporter {
public:
    /**
     * @brief 把 glTF（.gltf/.glb）导入为场景实体树。
     *
     * 保留 node 层级与变换：node 名称 → TagComponent、变换 → TransformComponent、
     * 父子关系 → Scene::SetParent、有 mesh 的 node → MeshRendererComponent。
     * mesh 经 MeshManager 按 "path#N" 键去重缓存（跨 node 复用同 mesh 只一份 GPU 网格）。
     *
     * @param scene       目标场景（实体树写入此场景）
     * @param meshManager 网格管理器（mesh 加载 / 去重缓存）
     * @param filepath    glTF 文件路径
     * @return 导入成功返回 true；文件非 glTF / 解析失败返回 false
     */
    static bool Import(Scene &scene, MeshManager &meshManager, const std::string &filepath);
};

} // namespace GE
