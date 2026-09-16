/**
 * @file AssetPathUtil.cpp
 * @brief 资产引用路径的规范形工具实现。
 */

#include "Render/AssetPathUtil.h"

#include <string_view>

namespace GE::AssetPathUtil {
namespace {

/// 前缀比较：Windows 下大小写不敏感（路径大小写不参与语义）。
bool StartsWith(const std::string &s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i];
        char b = prefix[i];
#ifdef _WIN32
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
#endif
        if (a != b) {
            return false;
        }
    }
    return true;
}

/// 去掉尾部 '/'（"F:/a/b/" → "F:/a/b"）。
std::string StripTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

/// 资源根的绝对形：相对根按当前工作目录绝对化（不访问文件系统）。
std::filesystem::path AbsoluteRoot(const std::filesystem::path &root) {
    if (root.is_absolute()) {
        return root;
    }
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(root, ec);
    return ec ? root : abs;
}

/// 路径 → 归一后的正斜杠字符串（去尾斜杠）。
std::string CanonicalString(const std::filesystem::path &p) {
    return StripTrailingSlash(NormalizeSeparators(p.lexically_normal().string()));
}

} // namespace

bool IsPseudoKey(const std::string &path) {
    return path.rfind("builtin:", 0) == 0 || path.rfind("solid:", 0) == 0;
}

bool LooksLikeWindowsDrivePath(const std::string &path) {
    // 形态：<字母> ':' ('/' | '\\') …
    // 只看形态、不看平台 —— 这正是 path::is_absolute() 做不到的事（见头文件注释）。
    if (path.size() < 3) {
        return false;
    }
    const char c0 = path[0];
    const bool isAlpha = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
    return isAlpha && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

std::string NormalizeSeparators(const std::string &path) {
    std::string out = path;
    for (char &c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

bool IsUnderRoot(const std::filesystem::path &abs, const std::filesystem::path &root) {
    if (abs.empty() || root.empty() || !abs.is_absolute()) {
        return false;
    }
    const std::string a = CanonicalString(abs);
    const std::string r = CanonicalString(AbsoluteRoot(root));
    // 需严格长于根，且根之后紧跟分隔符（否则 "assets2/x" 会被误判在 "assets" 之下）
    if (r.empty() || a.size() <= r.size()) {
        return false;
    }
    return StartsWith(a, r) && a[r.size()] == '/';
}

std::optional<std::string> ToCanonical(const std::string &raw,
                                       const std::filesystem::path &assetRoot) {
    if (raw.empty()) {
        return std::nullopt;
    }
    if (IsPseudoKey(raw)) {
        return raw;
    }

    const std::filesystem::path p(NormalizeSeparators(raw));

    // 绝对性**不能只信 path::is_absolute()**：POSIX 上它只认前导 '/'，于是
    // "F:\proj\assets\x.png" 在 Windows 判为绝对、在 Android 判为相对（已实测确认）。
    // 那样同一份开发机绝对路径在 Android 上会落进下面的相对分支被**原样放行** ——
    // 归一"成功"、无告警，最后静默读不到。所以这里补一个与平台无关的形态判定。
    if (p.is_absolute() || LooksLikeWindowsDrivePath(raw)) {
        // 根外引用无法随包分发，交由调用方（打包校验）报错
        if (!IsUnderRoot(p, assetRoot)) {
            return std::nullopt;
        }
        const std::string a = CanonicalString(p);
        const std::string r = CanonicalString(AbsoluteRoot(assetRoot));
        const std::string rel = a.substr(r.size() + 1); // 跳过分隔符
        if (rel.empty()) {
            return std::nullopt;
        }
        return std::filesystem::path(rel).lexically_normal().generic_string();
    }

    // 相对路径：剥掉可能的资源根目录名前缀（"assets/textures/x.png" → "textures/x.png"），
    // 避免 ResolveCanonical 拼成 "assets/assets/..."。其余按"已相对资源根"处理。
    std::string s = p.lexically_normal().generic_string();
    const std::string rootLeaf = assetRoot.filename().string();
    if (!rootLeaf.empty()) {
        if (StartsWith(s, rootLeaf + "/")) {
            s = s.substr(rootLeaf.size() + 1);
        } else if (StartsWith(s, rootLeaf) && s.size() == rootLeaf.size()) {
            return std::nullopt; // 只给了资源根目录名，不是资产
        }
    }
    if (s.empty() || s == ".." || StartsWith(s, "../")) {
        return std::nullopt; // 空串或越界到资源根之外
    }
    return s;
}

std::vector<std::string> CanonicalCandidates(const std::string &raw, std::string_view rootName) {    std::vector<std::string> candidates;
    if (raw.empty() || rootName.empty() || IsPseudoKey(raw)) {
        return candidates;
    }

    const std::string s = NormalizeSeparators(raw);
    const std::string needle = "/" + std::string(rootName) + "/";

    // 逐个扫出每一处 "/<rootName>/"：出现得越早，越可能是真正的资源根
    for (size_t pos = s.find(needle); pos != std::string::npos; pos = s.find(needle, pos + 1)) {
        std::string rel = s.substr(pos + needle.size());
        rel = std::filesystem::path(rel).lexically_normal().generic_string();
        if (rel.empty() || rel == ".." || StartsWith(rel, "../")) {
            continue; // 该段之后是越界/空 —— 这个候选没有意义，但后面的段仍可能有效
        }
        candidates.push_back(std::move(rel));
    }
    return candidates;
}

std::optional<std::string> ToCanonicalLenient(
    const std::string &raw, const std::filesystem::path &assetRoot,
    const std::function<bool(const std::string &)> &exists) {
    if (raw.empty()) {
        return std::nullopt;
    }
    if (IsPseudoKey(raw)) {
        return raw;
    }

    // 一、精确判定。有真实资源根时这是唯一正常路径（桌面即走这里）。
    //     注意 ToCanonical 已按"形态"识别 Windows 盘符路径，故残留的
    //     F:\...\assets\x.png 在 Android 上会**正确地失败**而不是被当相对路径放行。
    if (!assetRoot.empty()) {
        if (auto rel = ToCanonical(raw, assetRoot)) {
            return rel;
        }
    }

    // 二、根无关兜底：按 /<资源根名>/ 段切候选，用存在性定夺。
    //     Android 上不存在资源根的绝对形（资产在 APK 里），只有这条路可用。
    std::string rootName = assetRoot.filename().string();
    if (rootName.empty()) {
        rootName = "assets";
    }
    const auto candidates = CanonicalCandidates(raw, rootName);
    if (candidates.empty()) {
        return std::nullopt; // 切不出资源根段：不猜，交给调用方报错
    }
    if (exists) {
        for (const auto &cand : candidates) {
            if (exists(cand)) {
                return cand;
            }
        }
    }
    // 都不存在（或未提供 exists）：取最外层候选 —— 至少路径形态是对的，
    // 后续补上该资产即可命中；调用方通常会据此打一条告警。
    return candidates.front();
}

} // namespace GE::AssetPathUtil
