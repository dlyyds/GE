/**
 * @file GEMeshLoader.h
 * @brief .gemesh 引擎内置网格格式 —— 序列化 / 反序列化（格式编解码边界）。
 *
 * .gemesh 是引擎原生装载格式：把 OBJ/glTF 的「文本解析 + 几何预处理」从加载期
 * 搬到离线烘焙期，运行时只做「读文件 → 反序列化成 MeshData → 建 GPU 缓冲」。
 *
 * 与 OBJLoader 对称：ParseGEMesh 输出同一类型 MeshData，接入 ModelLoader::Parse
 * 分派后，MeshManager::Load 的异步链路（建缓冲 / 装 Mesh / 材质构建 / 回收）零改动
 * 复用。SerializeGEMesh 供离线导出器使用，不在运行时加载路径上。
 *
 * 文件布局（chunk 托盘，见方案书 §3）：
 *   Magic "GEMSH"(5B) + version(u16，v1=48B 顶点 / v2=80B 顶点含蒙皮字段)
 *   ChunkTable: count(u32) + 每项{ id(u32), offset(u64), size(u64) }
 *   Chunk VERTICES / INDICES / SUBMESHES / MATERIALS / META
 *   变长字符串（子网格 materialName / 材质贴图路径）经各 chunk 尾部字符串池偏移引用。
 *   版本兼容：v2 起 VERTICES 顶点流为 80B（新增 JOINTS/WEIGHTS 蒙皮字段）；
 *   读端对 v1（48B）文件做升格读取（蒙皮字段补 0）以兼容既有资产，写端恒写 v2。
 */

#pragma once

#include "Render/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace GE {

/**
 * @brief META chunk 承载的元信息（不参与渲染，用于编辑器展示 / 调试 / 碰撞）。
 */
struct GEMeshMeta {
    glm::vec3 aabbMin{0.0f}; ///< 包围盒最小角（导出时计算，v1）
    glm::vec3 aabbMax{0.0f}; ///< 包围盒最大角
    std::string sourceAsset; ///< 源资产路径（如 foo.obj，仅供追溯）
};

/**
 * @brief 从 .gemesh 文件反序列化为 MeshData（运行时加载路径）。
 *
 * 产出与 OBJ 解析相同类型的 MeshData：vertices(已量化/去重/含切线)、indices、子网格、
 * 材质数据。调用方（ModelLoader::Parse）拿到后照常交给 Mesh::Create / 异步链路装配。
 *
 * @param filepath .gemesh 文件路径
 * @param out      解析输出（MeshData）
 * @param outMeta  可选：若非空，回填 META chunk 的包围盒 / 源资产
 * @return 解析成功且含有效几何数据返回 true
 */
bool ParseGEMesh(const std::string &filepath, MeshData &out, GEMeshMeta *outMeta = nullptr);

/**
 * @brief 把 MeshData 序列化成 .gemesh 文件（离线导出器使用）。
 *
 * 输入应为格式无关的 MeshData（可含已算好的切线）；本函数只做编解码，不计算切线。
 * 若需要构建期预处理（量化 / 切线），由导出器调用方可选地先走 ModelLoader::ComputeTangents。
 *
 * @param outPath    输出 .gemesh 路径
 * @param data       待序列化的 MeshData
 * @param meta       META chunk 元信息（包围盒 / 源资产）
 * @param err        非空时回填错误描述
 * @return 序列化成功返回 true
 */
bool SerializeGEMesh(const std::string &outPath, const MeshData &data,
                     const GEMeshMeta &meta, std::string *err = nullptr);

/**
 * @brief 把待写入 .gemesh 的内嵌资产引用压成相对资源根的规范形。
 *
 * .gemesh 不是自包含格式：材质贴图槽与 META.sourceAsset 以字符串存盘，运行期由
 * MeshManager 直接拿这些串去加载贴图。烘焙时若残留开发机绝对路径，产物换一台机器
 * 或换一个资源根就必断——所以归一必须发生在写盘之前，而不是等打包时再补救。
 *
 * 归一失败的引用（根外路径 / ".." 逃出根）保留原串并打告警：烘焙期不做取舍判断，
 * 由打包侧（gepack --strict）报 Error 拦下。
 *
 * @param data      待序列化的 MeshData（就地修改各材质贴图槽）
 * @param meta      META 元信息（就地修改 sourceAsset）
 * @param assetRoot 资源根目录；传空则跳过归一
 */
void CanonicalizeGEMeshEmbeddedRefs(MeshData &data, GEMeshMeta &meta,
                                    const std::filesystem::path &assetRoot);

} // namespace GE
