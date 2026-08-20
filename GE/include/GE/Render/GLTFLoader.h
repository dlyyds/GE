/**
 * @file GLTFLoader.h
 * @brief glTF 格式解析器（tinygltf v2）—— 独立于 Mesh 的格式加载文件。
 *
 * 与 OBJLoader 对称：glTF 解析器独立成文件，统一输出 MeshData，由
 * ModelLoader::ParseGLTF / ModelLoader::GetGLTFMeshCount 对外暴露并接入
 * ModelLoader::Parse 分派。Mesh 不感知 tinygltf 的任何细节。
 *
 * 本文件还暴露两个内部 helper（GLTF::LoadModel / GLTF::BuildMesh），供
 * GLTFSceneImporter（场景图导入）复用：导入器一次载入 tinygltf::Model，
 * 逐 mesh 调 BuildMesh，避免重复 IO 与重复装配。
 */

#pragma once

#include <string>

// 前向声明，避免在头文件引入整个 tinygltf（仅 .cpp 需要完整定义）
namespace tinygltf { struct Model; }
namespace GE {
struct MeshData;
}

namespace GE {
namespace GLTF {

/**
 * @brief 载入 glTF 文件（.gltf→ASCII，.glb→Binary）为 tinygltf::Model。
 *
 * @param filepath glTF 文件路径
 * @param outModel 输出的解析模型（含全部 mesh / node / 材质 / 图像）
 * @param err      非空时回填错误描述
 * @return 解析成功返回 true
 */
bool LoadModel(const std::string &filepath, tinygltf::Model &outModel,
               std::string *err = nullptr);

/**
 * @brief 把 model.meshes[meshIndex] 的每个 primitive 装配为一份 MeshData（子网格）。
 *
 * 每 primitive 一段连续顶点/索引区间 → 一个 SubMesh；顶点量化/去重、材质
 * metallic-roughness → MaterialData 均在此完成。切线不在此计算（由 Mesh::BuildMesh
 * 统一 ComputeTangents，单一来源）。缺 TANGENT 的属性流由调用方装配时兜底。
 *
 * @param model     已载入的 tinygltf::Model
 * @param meshIndex 目标 mesh 索引
 * @param filepath  glTF 文件路径（用于材质贴图相对路径解析）
 * @param out       输出（MeshData）
 * @return 成功且含有效几何数据返回 true
 */
bool BuildMesh(const tinygltf::Model &model, size_t meshIndex,
               const std::string &filepath, MeshData &out);

/**
 * @brief 载入 glTF 文件并把第 meshIndex 个 mesh 装配为 MeshData（LoadModel + BuildMesh）。
 *
 * @param filepath  glTF 文件路径
 * @param meshIndex 目标 mesh 索引
 * @param out       输出（MeshData）
 * @return 成功且含有效几何数据返回 true
 */
bool BuildMeshData(const std::string &filepath, size_t meshIndex, MeshData &out);

} // namespace GLTF
} // namespace GE
