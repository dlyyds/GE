/**
 * @file ModelLoader.h
 * @brief 模型加载统一入口 —— 格式分派与共享 CPU 装配。
 *
 * 承载从 Mesh 拆出的「加载编排 / 几何生成」职责：
 * - ParseModelData：按扩展名把模型解析分派到各格式解析器（OBJ / 未来的 glTF / FBX），
 *   所有解析器统一输出 MeshData。
 * - ParseOBJData：OBJ 格式解析器（实现在 OBJLoader.cpp）。
 * - ComputeTangents：格式无关的切线计算（同步装配与异步 decode 共享）。
 *   （内置几何生成 GenerateBuiltinMeshData 与 builtin 路径判定 IsBuiltinPath /
 *   GetBuiltinType 均已归 MeshManager。）
 *
 * 消费者的分工：
 * - MeshManager::Load 负责编排（选路、造空壳、组装后台上传任务、finalize 注入）。
 * - Mesh 只负责"MeshData → 资源"的装配（切线 + 上传 + 摘要）。
 */

#pragma once

#include "Render/Mesh.h"

namespace GE {

/**
 * @brief 按文件扩展名把模型解析分派到对应格式解析器（纯 CPU，不含 GPU 上传）。
 *
 * 新增模型格式时：实现一个签名相同的 ParseXXXData 解析器（输出 MeshData）并在此
 * 注册一行扩展名判断即可，同步 / 异步两条路径自动同时生效。
 *
 * @param filepath 模型文件路径
 * @param out      解析输出（顶点/索引/子网格/材质数据，std::move 入）
 * @return 解析成功且含有效几何数据返回 true
 */
bool ParseModelData(const std::string &filepath, MeshData &out);

/**
 * @brief OBJ 格式解析器（tinyobjloader），实现在 OBJLoader.cpp。
 *
 * 内容 = tinyobj 解析 + 顶点装配/量化/去重 + 子网格拆分 + MTL → MaterialData 捕获。
 * 切线计算不在此处（由调用方经 ComputeTangents 完成）。
 *
 * @param filepath OBJ 文件路径
 * @param out      解析输出（MeshData）
 * @return 解析成功且含有效几何数据返回 true
 */
bool ParseOBJData(const std::string &filepath, MeshData &out);

/**
 * @brief 根据三角形 (位置, UV) 计算每个顶点的切线向量（析取 MeshData 的顶点/索引）。
 *
 * 标准算法：对每个三角形计算切线与副切线插值，累加到三个顶点上，最后按顶点的
 * 法线用 Gram-Schmidt 正交化得到最终的切线方向，并以 w 分量记录手性符号
 * （用于在着色器中重建正确的副切线方向）。同步装配（Mesh::Create）与异步 decode
 * 共用，保证两条加载路径的切线结果完全一致。
 *
 * @param data 顶点/索引数据（就地修改 Tangent 字段）
 */
void ComputeTangents(MeshData &data);

} // namespace GE