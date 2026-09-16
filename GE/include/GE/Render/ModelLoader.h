/**
 * @file ModelLoader.h
 * @brief 模型加载统一入口 —— 格式分派与共享 CPU 装配（静态工具类）。
 *
 * 承载从 Mesh 拆出的「格式分派 / 共享装配」职责：
 * - Parse：按扩展名把模型解析分派到各格式解析器（OBJ / 未来的 glTF / FBX），
 *   所有解析器统一输出 MeshData。
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

#include <cstddef>
#include <filesystem>

namespace GE {

/**
 * @brief 模型加载静态工具类 —— 格式分派 + 共享 CPU 装配。
 *
 * 全静态、无状态，仅提供格式解析的分派入口与格式无关的几何预处理。
 * 新增模型格式时：实现一个签名相同的 ParseXXXData 解析器（输出 MeshData），
 * 在 Parse 中注册一行扩展名判断即可，同步 / 异步两条路径自动同时生效。
 */
class ModelLoader {
public:
    /**
     * @brief 按文件扩展名把模型解析分派到对应格式解析器（纯 CPU，不含 GPU 上传）。
     *
     * @param filepath 模型文件路径
     * @param out      解析输出（顶点/索引/子网格/材质数据，std::move 入）
     * @return 解析成功且含有效几何数据返回 true
     */
    static bool Parse(const std::string &filepath, MeshData &out);

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
    static void ComputeTangents(MeshData &data);

    /**
     * @brief 解析 glTF 文件（.gltf / .glb）中的第 meshIndex 个 mesh 为一份 MeshData。
     *
     * 一个 tinygltf::Mesh 对应一份 MeshData，其多个 primitive 各自装配为一段连续
     * 子网格区间（firstVertex/vertexCount 指向段起点）。顶点量化/去重复用现有工具，
     * 缺 TANGENT 时补 ComputeTangents。材质按 metallic-roughness 映射为 MaterialData，
     * 贴图 URI 相对 glTF 所在目录解析为绝对路径。
     *
     * @param filepath  glTF 文件路径
     * @param meshIndex 要解析的 mesh 索引（0 起始）
     * @param out       解析输出（MeshData）
     * @return 解析成功且含有效几何数据返回 true
     */
    static bool ParseGLTF(const std::string &filepath, size_t meshIndex, MeshData &out);

    /**
     * @brief 读取 glTF 文件的 mesh 数量（多 mesh 遍历 / 校验用）。
     *
     * @param filepath glTF 文件路径
     * @return mesh 数量，文件非 glTF 或加载失败返回 0
     */
    static size_t GetGLTFMeshCount(const std::string &filepath);

    /**
     * @brief 把源模型（.obj / .gltf）离线烘焙为 .gemesh（导出工具）。
     *
     * 流程：解析源 → ComputeTangents 计算切线 → 包围盒 → 内嵌路径归一 → SerializeGEMesh。
     * 产物可直接被 LoadMesh("x.gemesh") 加载（.gemesh 已接入 ModelLoader::Parse）。
     *
     * 内嵌路径归一：.gemesh 不是自包含格式——材质贴图槽与 GEMeshMeta::sourceAsset 以字符串
     * 存盘，运行期直接拿去加载贴图，残留开发机绝对路径的产物换台机器必断。故写盘前会用
     * CanonicalizeGEMeshEmbeddedRefs 压成相对 assetRoot 的规范形。
     * 注意：各解析器按「源文件所在目录」拼贴图路径，故 srcPath 传绝对路径最稳——传相对路径
     * 时拼出的相对串缺少锚点，归一只会原样放行。
     *
     * @param srcPath   源模型路径（.obj / .gltf / .glb）
     * @param outPath   输出 .gemesh 路径
     * @param err       非空时回填错误描述
     * @param assetRoot 资源根目录，用于内嵌路径归一；传空则跳过（并告警）
     * @param meshIndex glTF 的 mesh 索引（.obj 忽略）
     * @return 转换成功返回 true
     */
    static bool ConvertToGEMesh(const std::string &srcPath, const std::string &outPath,
                                std::string *err = nullptr,
                                const std::filesystem::path &assetRoot = {},
                                size_t meshIndex = 0);

    /**
     * @brief 把单个资产引用归一为**规范形**（VFS 路径，正斜杠）。
     *
     * `.gemesh` / `.gemat` 这类把引用当字符串存盘的产物，历史上（内嵌引用归一代码
     * 落地之前烘焙的）存的是开发机**绝对路径**，如 `F:\proj\assets\models\x.png`。
     * 归一走两段：
     * 1. 有 `assetRoot` 时精确判定（正常路径，桌面可用）
     * 2. 失败则**根无关**切分 —— 按 `/<资源根名>/` 段取出候选，用 **VFS 实际能否
     *    读到**定夺。Android 上不存在资源根的绝对路径（资产在 APK 里），只有这条
     *    可用；桌面上把 `dist/` 拷到别处跑也走这条。
     *
     * 切不出资源根名时不动 `ref`（不猜）。
     */
    static void CanonicalizeAssetRef(std::string &ref, const std::filesystem::path &assetRoot);

    /**
     * @brief 归一 `MeshData` 内嵌的全部贴图槽引用。
     *
     * 这是**模型加载的收口点**：各格式解析器（.gemesh 的字符串池、.obj 的 MTL
     * 纹理名、.glTF 的 image uri）都用相对路径表达贴图，必须在这里压成规范形，
     * 否则交给 `TextureManager` 的可能是开发机绝对路径 —— 在 Android 上必然读不到，
     * 现象是"模型发白"而**不报错**。
     */
    static void CanonicalizeEmbeddedMaterialRefs(MeshData &data,
                                                 const std::filesystem::path &assetRoot);

private:
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
    static bool ParseOBJ(const std::string &filepath, MeshData &out);
};

} // namespace GE