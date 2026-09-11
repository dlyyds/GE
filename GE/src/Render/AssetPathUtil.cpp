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

    if (p.is_absolute()) {
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
    // 避免 ResolvePath 拼成 "assets/assets/..."。其余按"已相对资源根"处理。
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

} // namespace GE::AssetPathUtil
