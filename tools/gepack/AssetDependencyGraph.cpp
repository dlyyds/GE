/**
 * @file AssetDependencyGraph.cpp
 * @brief 依赖收集与校验实现。
 */

#include "AssetDependencyGraph.h"

#include "Animation/AnimationClipLoader.h" // DeriveAnimationBakePath（派生 .geanim 的唯一口径）
#include "Render/AssetManager.h"           // AssetPaths 常量（目录约定）
#include "Render/AssetPathUtil.h"          // 规范形 / 伪键 / 根内判定
#include "Render/GEMeshLoader.h"           // ParseGEMesh（取 .gemesh 内嵌材质贴图槽）
#include "Scene/SceneAssetScanner.h"       // 场景 YAML 的资产引用清单

// nlohmann/json（tinygltf 自带的那份，随 GE/third_party 一起在 include 路径上）
#include "tinygltf/json.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gepack {

namespace {

AssetKind MapKind(GE::AssetRefKind kind) {
    switch (kind) {
        case GE::AssetRefKind::Mesh:        return AssetKind::Mesh;
        case GE::AssetRefKind::Texture:     return AssetKind::Texture;
        case GE::AssetRefKind::Audio:       return AssetKind::Audio;
        case GE::AssetRefKind::Script:      return AssetKind::Script;
        case GE::AssetRefKind::Clip:        return AssetKind::Clip;
        case GE::AssetRefKind::Material:    return AssetKind::Material;
        case GE::AssetRefKind::Environment: return AssetKind::Environment;
    }
    return AssetKind::Other;
}

std::string Lower(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool EndsWithLower(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == suffix;
}

/// glTF 的 uri 里非 ASCII 名会被百分号编码（Blender 导出中文名即如此），解码后才对得上磁盘文件
std::string PercentDecode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

/// 递归列目录下的常规文件，回填相对 relDir 的路径（'/' 分隔）
void WalkFiles(const std::filesystem::path &dir, const std::string &relPrefix,
               std::vector<std::string> &out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string rel =
            relPrefix + it->path().lexically_relative(dir).generic_string();
        out.push_back(rel);
    }
}

} // namespace

const char *AssetKindName(AssetKind kind) {
    switch (kind) {
        case AssetKind::Scene:       return "场景";
        case AssetKind::Mesh:        return "网格";
        case AssetKind::Texture:     return "贴图";
        case AssetKind::Audio:       return "音频";
        case AssetKind::Script:      return "脚本";
        case AssetKind::Clip:        return "动画";
        case AssetKind::Material:    return "材质";
        case AssetKind::Environment: return "环境";
        case AssetKind::Shader:      return "着色器";
        case AssetKind::Font:        return "字体";
        case AssetKind::SourceModel: return "源模型";
        default:                     return "其它";
    }
}

AssetDependencyGraph::AssetDependencyGraph(std::filesystem::path assetRoot, PackRules rules)
    : m_AssetRoot(std::move(assetRoot)), m_Rules(std::move(rules)) {}

std::filesystem::path AssetDependencyGraph::FullPath(const std::string &canonical) const {
    return (m_AssetRoot / std::filesystem::path(canonical)).lexically_normal();
}

bool AssetDependencyGraph::HasErrors() const {
    return std::any_of(m_Problems.begin(), m_Problems.end(),
                       [](const Problem &p) { return p.level == ProblemLevel::Error; });
}

std::uintmax_t AssetDependencyGraph::TotalBytes() const {
    std::uintmax_t total = 0;
    for (const auto &kv : m_Nodes) {
        total += kv.second.size;
    }
    return total;
}

std::size_t AssetDependencyGraph::CountByKind(AssetKind kind) const {
    return static_cast<std::size_t>(std::count_if(
        m_Nodes.begin(), m_Nodes.end(),
        [kind](const auto &kv) { return kv.second.kind == kind; }));
}

void AssetDependencyGraph::AddProblem(ProblemLevel level, const std::string &message) {
    m_Problems.push_back(Problem{level, message});
}

void AssetDependencyGraph::AddDep(const std::string &from, const std::string &depCanonical) {
    auto it = m_Nodes.find(from);
    if (it != m_Nodes.end()) {
        it->second.deps.push_back(depCanonical);
    }
}

// ============================================================================
// 入队：规整引用 + 分流
// ============================================================================

void AssetDependencyGraph::EnqueueCanonical(const std::string &canonical, AssetKind kind,
                                            const std::string &from) {
    if (canonical.empty()) {
        return;
    }
    // 注意：排除规则**不**在这里生效——被引用的资产一律收（少一个文件包就是坏的）。
    // 排除只作用于固定集合的目录遍历与「未引用资产」统计：那里才是「没被需要的东西」。
    m_Queue.push_back(Pending{canonical, kind, from});
}

void AssetDependencyGraph::AddRef(GE::AssetRefKind kind, const std::string &raw,
                                  const std::string &from) {
    if (raw.empty() || GE::AssetPathUtil::IsPseudoKey(raw)) {
        return; // 伪键（builtin: / solid:）不是文件，无依赖
    }

    // 动画与环境的引用不是文件本身：前者要换成派生的 .geanim，后者是目录名
    if (kind == GE::AssetRefKind::Clip) {
        ExpandClip(raw, from);
        return;
    }
    if (kind == GE::AssetRefKind::Environment) {
        ExpandEnvironment(raw, from);
        return;
    }

    // 脚本的基准是 assets/scripts，不是资源根——先补前缀再走统一归一
    std::string ref = raw;
    if (kind == GE::AssetRefKind::Script) {
        ref = std::string(GE::AssetPaths::Scripts) + "/" +
              GE::AssetPathUtil::NormalizeSeparators(raw);
    }

    const auto canonical = GE::AssetPathUtil::ToCanonical(ref, m_AssetRoot);
    if (!canonical) {
        AddProblem(ProblemLevel::Error,
                   "资源根之外的引用（无法随包分发）: " + raw + "  ← " + from);
        return;
    }
    EnqueueCanonical(*canonical, MapKind(kind), from);
}

void AssetDependencyGraph::ExpandEnvironment(const std::string &folderName,
                                            const std::string &from) {
    // 环境是目录名（EnvironmentComponent.Name），运行期读该目录下的两份烘焙产物
    const std::string base = std::string("environments/") + folderName + "/";
    m_Notes.push_back("environment:" + folderName);

    const std::string skybox = base + "skybox.ktx2";
    const std::string prefilter = base + "prefilter.ktx";

    if (!std::filesystem::is_regular_file(FullPath(skybox))) {
        AddProblem(ProblemLevel::Error, "环境缺天空盒: " + skybox + "  ← " + from);
    } else {
        EnqueueCanonical(skybox, AssetKind::Environment, from);
    }

    if (!std::filesystem::is_regular_file(FullPath(prefilter))) {
        // _default_cube 这类只有天空盒的环境没有预过滤，属正常
        AddProblem(ProblemLevel::Warn, "环境 " + folderName + " 缺 prefilter.ktx（无 IBL 预过滤）");
    } else {
        EnqueueCanonical(prefilter, AssetKind::Environment, from);
    }
}

void AssetDependencyGraph::ExpandClip(const std::string &raw, const std::string &from) {
    // 场景里存的是源键 "foo.gltf#N"；运行期优先读派生的 .geanim（缺失才回退源 glTF）
    const auto canonical = GE::AssetPathUtil::ToCanonical(raw, m_AssetRoot);
    if (!canonical) {
        AddProblem(ProblemLevel::Error,
                   "资源根之外的动画引用（无法随包分发）: " + raw + "  ← " + from);
        return;
    }
    const std::string bake = GE::DeriveAnimationBakePath(*canonical);
    if (std::filesystem::is_regular_file(FullPath(bake))) {
        EnqueueCanonical(bake, AssetKind::Clip, from + " 派生的 " + bake);
        return;
    }

    AddProblem(ProblemLevel::Warn,
               "动画缺烘焙产物 " + bake + "，改为打包源 glTF（运行时每次要搬整份源文件）: " + from);
    const std::string source = canonical->substr(0, canonical->rfind('#'));
    ExpandGLTFClosure(source, from + " 的动画源");
}

// ============================================================================
// 展开：一类资产怎么变成它的依赖
// ============================================================================

void AssetDependencyGraph::ResolveNode(const Pending &item) {
    const std::string lower = Lower(item.canonical);
    if (m_SeenLower.count(lower) != 0) {
        return; // 同一文件（Windows 下大小写不敏感）只展开一次
    }
    m_SeenLower.insert(lower);

    AssetNode node;
    node.kind = item.kind;
    node.importers.push_back(item.from);

    const auto full = FullPath(item.canonical);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(full, ec)) {
        AddProblem(ProblemLevel::Error, "引用的文件不存在: " + item.canonical + "  ← " + item.from);
        m_Nodes[item.canonical] = node;
        return;
    }
    node.size = std::filesystem::file_size(full, ec);
    m_Nodes[item.canonical] = node;

    switch (item.kind) {
        case AssetKind::Mesh:
            ExpandMesh(item.canonical);
            break;
        case AssetKind::Material:
            ExpandMaterialGemat(item.canonical);
            break;
        default:
            break; // 贴图/音频/脚本/烘焙产物/着色器/字体都是叶子
    }
}

void AssetDependencyGraph::ExpandMesh(const std::string &canonical) {
    if (EndsWithLower(canonical, ".gemesh")) {
        // .gemesh 不是自包含格式：材质贴图路径写在字符串池里，运行期直接拿去加载
        GE::MeshData data;
        GE::GEMeshMeta meta;
        std::string err;
        // **传 canonical，不是 FullPath**：ParseGEMesh 经 VFS 读盘，入参是规范形。
        // 传已拼上资源根的 full 会变成 <root>/<root>/... 而读不到 —— 且失败信息是
        // 「格式版本不符或已损坏」，把方向指偏。
        if (!GE::ParseGEMesh(canonical, data, &meta)) {
            AddProblem(ProblemLevel::Error, "无法解析 .gemesh（读不到或格式版本不符）: " + canonical);
            return;
        }
        for (const auto &md : data.materialData) {
            const std::pair<const char *, const std::string *> slots[] = {
                {"albedoMap", &md.albedoMap},
                {"normalMap", &md.normalMap},
                {"emissiveMap", &md.emissiveMap},
                {"metallicMap", &md.metallicMap},
                {"roughnessMap", &md.roughnessMap},
                {"metallicRoughnessMap", &md.metallicRoughnessMap},
            };
            for (const auto &slot : slots) {
                const std::string &ref = *slot.second;
                if (ref.empty() || GE::AssetPathUtil::IsPseudoKey(ref)) {
                    continue;
                }
                // 内嵌引用已是烘焙期归一过的规范形；仍过一遍 ToCanonical 以拦住
                // 「旧资产里残留绝对路径 / 逃出根」的情况——这种包在别的机器上必断
                const auto dep = GE::AssetPathUtil::ToCanonical(ref, m_AssetRoot);
                if (!dep) {
                    AddProblem(ProblemLevel::Error,
                               ".gemesh 内嵌了资源根之外的贴图（需重烘该网格）: " + ref +
                                   "  ← " + canonical);
                    continue;
                }
                AddDep(canonical, *dep);
                EnqueueCanonical(*dep, AssetKind::Texture, canonical + " 内嵌 " + slot.first);
            }
        }
        return;
    }

    if (EndsWithLower(canonical, ".gltf") || EndsWithLower(canonical, ".glb")) {
        AddProblem(ProblemLevel::Warn,
                   "网格未烘焙（应烘成 .gemesh 以免运行期解析大 glTF）: " + canonical);
        ExpandGLTFClosure(canonical, "未烘焙网格");
        return;
    }

    if (EndsWithLower(canonical, ".obj")) {
        AddProblem(ProblemLevel::Warn,
                   "网格未烘焙（.obj 需连同 .mtl 一起分发，且运行期每次重新装配）: " + canonical);
        return;
    }
}

void AssetDependencyGraph::ExpandMaterialGemat(const std::string &canonical) {
    // .gemat 的四个贴图槽。槽名与 MaterialSerializer 的 kTextureSlotNames 一致；
    // 那边改名时这里要同步（不能直接 include 其头文件：它会拖进 Renderer/Material）。
    static const char *kSlots[] = {"AlbedoTexture", "NormalTexture", "EmissiveTexture",
                                   "MetallicRoughnessTexture"};
    try {
        const YAML::Node root = YAML::LoadFile(FullPath(canonical).string());
        const YAML::Node node = root["Material"];
        if (!node) {
            AddProblem(ProblemLevel::Error, ".gemat 缺少 Material 段: " + canonical);
            return;
        }
        for (const char *slot : kSlots) {
            if (!node[slot]) {
                continue;
            }
            const std::string ref = node[slot].as<std::string>("");
            if (ref.empty()) {
                continue;
            }
            const auto dep = GE::AssetPathUtil::ToCanonical(ref, m_AssetRoot);
            if (!dep) {
                AddProblem(ProblemLevel::Error,
                           ".gemat 引用了资源根之外的贴图: " + ref + "  ← " + canonical);
                continue;
            }
            AddDep(canonical, *dep);
            EnqueueCanonical(*dep, AssetKind::Texture, canonical + " 的 " + slot);
        }
    } catch (const std::exception &e) {
        AddProblem(ProblemLevel::Error,
                   std::string(".gemat 解析失败: ") + canonical + "（" + e.what() + "）");
    }
}

void AssetDependencyGraph::ExpandGLTFClosure(const std::string &canonical,
                                             const std::string &from) {
    const auto full = FullPath(canonical);
    std::ifstream fin(full, std::ios::binary);
    if (!fin.is_open()) {
        AddProblem(ProblemLevel::Error, "源 glTF 打不开: " + canonical + "  ← " + from);
        return;
    }
    std::stringstream ss;
    ss << fin.rdbuf();

    const nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        AddProblem(ProblemLevel::Error, "源 glTF 解析失败: " + canonical + "  ← " + from);
        return;
    }

    const std::filesystem::path base = std::filesystem::path(canonical).parent_path();
    const auto addUri = [&](const nlohmann::json &arr, bool isTexture, const char *what) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto &item : arr) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) {
                continue; // 内嵌 base64 / 无 uri 的资源没有外部依赖
            }
            const std::string uri = item["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) {
                continue;
            }
            const std::string rel =
                (base / GE::AssetPathUtil::NormalizeSeparators(PercentDecode(uri)))
                    .generic_string();
            const auto dep = GE::AssetPathUtil::ToCanonical(rel, m_AssetRoot);
            if (!dep) {
                AddProblem(ProblemLevel::Error,
                           "源 glTF 引用了资源根之外的文件: " + uri + "  ← " + canonical);
                continue;
            }
            const std::string desc = canonical + " 的 " + what;
            AddDep(canonical, *dep);
            EnqueueCanonical(*dep, isTexture ? AssetKind::Texture : AssetKind::SourceModel, desc);
        }
    };
    addUri(doc.contains("buffers") ? doc["buffers"] : nlohmann::json(), false, "buffer");
    addUri(doc.contains("images") ? doc["images"] : nlohmann::json(), true, "image");

    EnqueueCanonical(canonical, AssetKind::SourceModel, from);
}

// ============================================================================
// 固定集合：场景看不见的依赖
// ============================================================================

void AssetDependencyGraph::CollectFixedSets() {
    // 着色器按文件名硬编码在渲染代码里（Renderer3D_Lifecycle / Renderer2D 共 28 处），
    // 场景不引用它们，依赖图发现不了 → 全量收 .spv（体积小，安全优先）
    std::vector<std::string> shaders;
    WalkFiles(m_AssetRoot / GE::AssetPaths::Shaders,
              std::string(GE::AssetPaths::Shaders) + "/", shaders);
    for (const auto &rel : shaders) {
        if (EndsWithLower(rel, ".spv") && !IsExcluded(m_Rules, rel)) {
            EnqueueCanonical(rel, AssetKind::Shader, "固定集合：着色器全量收录");
        }
    }
    m_Notes.push_back("shaders:all");

    // Lua 的 require 走 package.path 运行期解析，静态分析不可靠 → 整目录收录
    std::vector<std::string> scripts;
    WalkFiles(m_AssetRoot / GE::AssetPaths::Scripts,
              std::string(GE::AssetPaths::Scripts) + "/", scripts);
    for (const auto &rel : scripts) {
        if (!IsExcluded(m_Rules, rel)) {
            EnqueueCanonical(rel, AssetKind::Script, "固定集合：脚本整目录收录");
        }
    }
    m_Notes.push_back("scripts:whole-dir");

    // ImGui 初始化即加载默认字体（GE_Runtime 与编辑器共享 Application 初始化链）
    EnqueueCanonical(std::string(GE::AssetPaths::Fonts) + "/OpenSans-Regular.ttf",
                     AssetKind::Font, "固定集合：ImGui 默认字体");

    // 场景没有 EnvironmentComponent 时渲染器兜底用它，另有一份全局共享的 BRDF LUT
    EnqueueCanonical("environments/_default_cube/skybox.ktx2", AssetKind::Environment,
                     "固定集合：环境兜底");
    EnqueueCanonical("environments/brdf_lut.png", AssetKind::Texture,
                     "固定集合：共享 BRDF LUT");
}

void AssetDependencyGraph::CollectUnused() {
    // 只报告不剔除：动态加载 / 脚本拼路径的资产静态看不见，剔除会误伤
    std::vector<std::string> all;
    WalkFiles(m_AssetRoot, "", all);
    for (const auto &rel : all) {
        if (IsExcluded(m_Rules, rel)) {
            continue;
        }
        if (m_SeenLower.count(Lower(rel)) == 0) {
            m_Unused.push_back(rel);
        }
    }
    if (!m_Unused.empty()) {
        AddProblem(ProblemLevel::Warn,
                   "未被引用的资产 " + std::to_string(m_Unused.size()) +
                       " 个（仅报告，不剔除——可能是脚本/动态加载在用）");
    }
}

// ============================================================================
// 入口
// ============================================================================

bool AssetDependencyGraph::Build(const std::string &entrySceneCanonical, std::string &fatal) {
    std::error_code ec;
    if (!std::filesystem::is_directory(m_AssetRoot, ec)) {
        fatal = "资源根不存在: " + m_AssetRoot.string();
        return false;
    }
    if (!std::filesystem::is_regular_file(FullPath(entrySceneCanonical), ec)) {
        fatal = "入口场景不存在: " + FullPath(entrySceneCanonical).string();
        return false;
    }

    const GE::SceneScanResult scan =
        GE::ScanSceneAssetRefs(FullPath(entrySceneCanonical).string(), entrySceneCanonical);
    if (!scan.Ok()) {
        fatal = scan.error;
        return false;
    }

    // 把场景里的每条引用喂进依赖图 —— 这一步曾是**缺失**的：AddRef() 定义着却零调用点，
    // 于是依赖图里永远只有「入口场景 + 固定集合」，场景引用的网格/材质/动画/音频全部
    // 被静默丢掉，而工具照报成功。表现为 dist 只有 40 个资产、且把一万多个资产误报成
    // 「未被引用」。对打包工具来说这是最坏的失败模式（产物不完整却校验通过）。
    for (const GE::AssetRef &ref : scan.refs) {
        AddRef(ref.kind, ref.raw, ref.location);
    }

    EnqueueCanonical(entrySceneCanonical, AssetKind::Scene, "入口场景");
    CollectFixedSets();

    while (m_QueueHead < m_Queue.size()) {
        ResolveNode(m_Queue[m_QueueHead++]);
    }

    CollectUnused();
    return true;
}

} // namespace gepack
