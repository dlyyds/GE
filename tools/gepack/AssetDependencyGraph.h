/**
 * @file AssetDependencyGraph.h
 * @brief gepack 的依赖图：入口场景 → 全部被引用资产（节点=规范形路径，边=文件→文件）。
 *
 * 收集是「工作队列 + 已访问集合」：每个节点只展开一次，故无需环检测（.gemesh 内嵌贴图、
 * .gemat 四槽、clip 派生 .geanim 都是单向边；即便将来出现自引用，visited 也保证不空转）。
 *
 * 展开规则（每条边都对应运行期一次真实的资源加载）：
 *   .gemesh  → 内嵌材质贴图槽（GEMeshLoader 的字符串池；不导出源 glTF，运行时不需要）
 *   .gemat   → 四个贴图槽（MaterialSerializer 写出的槽名）
 *   clip     → DeriveAnimationBakePath 派生的 .geanim；缺失则回退源 glTF 闭包
 *   未烘焙网格 → 回退源 glTF 闭包（.gltf + buffers/images uri）
 *   环境     → environments/<Name>/skybox.ktx2 + prefilter.ktx
 *
 * 固定集合（场景看不见的依赖）：shaders/glsl 下全部 .spv、scripts/ 整目录、
 * ImGui 字体、默认环境 _default_cube + 共享 brdf_lut.png。
 */

#pragma once

#include "PackRules.h"

#include "Scene/SceneAssetScanner.h" // GE::AssetRefKind（场景级引用种类）

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gepack {

/// 资产种类（场景级引用来自 GE::AssetRefKind，固定集合另加几种）
enum class AssetKind {
    Scene,       ///< .scene（入口）
    Mesh,        ///< .gemesh
    Texture,     ///< 贴图
    Audio,       ///< 音频
    Script,      ///< Lua
    Clip,        ///< .geanim
    Material,    ///< .gemat
    Environment, ///< 环境（skybox/prefilter 已展开成文件节点，此种类仅用于目录名本身）
    Shader,      ///< .spv
    Font,        ///< ImGui 字体
    SourceModel, ///< 未烘焙的源模型（.gltf/.obj，运行时回退用）
    Other,
};

const char *AssetKindName(AssetKind kind);

/// 一个资产节点
struct AssetNode {
    AssetKind kind = AssetKind::Other;
    std::uintmax_t size = 0;            ///< 字节数
    std::vector<std::string> deps;      ///< 出边：本资产依赖的规范形路径
    std::vector<std::string> importers; ///< 入边来源描述（场景位置 / "<gemesh> 内嵌 albedoMap"）
};

enum class ProblemLevel { Info, Warn, Error };

struct Problem {
    ProblemLevel level = ProblemLevel::Info;
    std::string message;
};

class AssetDependencyGraph {
public:
    AssetDependencyGraph(std::filesystem::path assetRoot, PackRules rules);

    /**
     * @brief 收集 + 校验（不写盘）。
     * @param entrySceneCanonical 入口场景的规范形（如 "scenes/2.scene"）
     * @param fatal 非空 = 前置条件失败（入口场景缺失 / 资源根不存在），调用方应直接退出
     * @return 收集完成（可能有 Problem，见 Problems()）返回 true
     */
    bool Build(const std::string &entrySceneCanonical, std::string &fatal);

    const std::map<std::string, AssetNode> &Nodes() const { return m_Nodes; }
    const std::vector<Problem> &Problems() const { return m_Problems; }
    const std::vector<std::string> &Unused() const { return m_Unused; }
    const std::vector<std::string> &Notes() const { return m_Notes; }
    const std::filesystem::path &AssetRoot() const { return m_AssetRoot; }

    bool HasErrors() const;
    std::uintmax_t TotalBytes() const;
    std::size_t CountByKind(AssetKind kind) const;

    /// 资源根 + 规范形 → 实际文件路径（与引擎 ResolvePath 同构）
    std::filesystem::path FullPath(const std::string &canonical) const;

private:
    struct Pending {
        std::string canonical;
        AssetKind kind;
        std::string from; ///< 来源描述（谁引用它，用于报错）
    };

    /// 规整引用（归一 + 伪键过滤 + 大小写去重）后入队
    void AddRef(GE::AssetRefKind kind, const std::string &raw, const std::string &from);
    /// 已是规范形的路径直接入队
    void EnqueueCanonical(const std::string &canonical, AssetKind kind, const std::string &from);

    void ResolveNode(const Pending &item);
    void ExpandMesh(const std::string &canonical);
    void ExpandMaterialGemat(const std::string &canonical);
    void ExpandClip(const std::string &raw, const std::string &from);
    void ExpandEnvironment(const std::string &folderName, const std::string &from);
    void ExpandGLTFClosure(const std::string &canonical, const std::string &from);
    void CollectFixedSets();
    void CollectUnused();
    void AddProblem(ProblemLevel level, const std::string &message);
    void AddDep(const std::string &from, const std::string &depCanonical);

    std::filesystem::path m_AssetRoot;
    PackRules m_Rules;
    std::map<std::string, AssetNode> m_Nodes;    ///< 键 = 规范形（保留原始大小写）
    std::set<std::string> m_SeenLower;           ///< 大小写折叠后的已访问集合（Windows 同文件去重）
    std::vector<Pending> m_Queue;
    std::size_t m_QueueHead = 0;
    std::vector<Problem> m_Problems;
    std::vector<std::string> m_Unused;
    std::vector<std::string> m_Notes;
};

} // namespace gepack
