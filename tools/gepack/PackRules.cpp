/**
 * @file PackRules.cpp
 * @brief 打包规则加载与 glob 匹配实现。
 */

#include "PackRules.h"

#include <yaml-cpp/yaml.h>

namespace gepack {

namespace {

/// 段内匹配：`*` 任意字符（不跨 '/'，段已切开）、`?` 单字符
bool MatchSegment(const std::string &pat, const std::string &seg) {
    // 无通配符走快路径
    if (pat.find_first_of("*?") == std::string::npos) {
        return pat == seg;
    }

    size_t p = 0, s = 0;
    size_t starP = std::string::npos, starS = 0;
    while (s < seg.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == seg[s])) {
            ++p;
            ++s;
            continue;
        }
        if (p < pat.size() && pat[p] == '*') {
            starP = p++;
            starS = s;
            continue;
        }
        if (starP != std::string::npos) {
            p = starP + 1;
            s = ++starS;
            continue;
        }
        return false;
    }
    while (p < pat.size() && pat[p] == '*') {
        ++p;
    }
    return p == pat.size();
}

std::vector<std::string> Split(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find('/', start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

bool MatchSegments(const std::vector<std::string> &ps, size_t pi,
                   const std::vector<std::string> &ss, size_t si) {
    while (true) {
        if (pi == ps.size()) {
            return si == ss.size();
        }
        if (ps[pi] == "**") {
            // '**' 吃掉 0..N 段（含末段），逐种尝试
            for (size_t skip = si; skip <= ss.size(); ++skip) {
                if (MatchSegments(ps, pi + 1, ss, skip)) {
                    return true;
                }
            }
            return false;
        }
        if (si == ss.size() || !MatchSegment(ps[pi], ss[si])) {
            return false;
        }
        ++pi;
        ++si;
    }
}

} // namespace

bool GlobMatch(const std::string &pattern, const std::string &path) {
    if (pattern.empty()) {
        return false;
    }
    return MatchSegments(Split(pattern), 0, Split(path), 0);
}

bool IsExcluded(const PackRules &rules, const std::string &canonicalPath) {
    for (const auto &pattern : rules.excludes) {
        if (GlobMatch(pattern, canonicalPath)) {
            return true;
        }
    }
    return false;
}

PackRules DefaultPackRules() {
    PackRules rules;
    rules.excludes = {
        "Screenshot/**",
        "cache/**",
        "environments/**/source.exr",
        "environments/**/preview.png",
        "**/*.ilk",
        "**/*.pdb",
        // 着色器只收运行期要的 .spv；.vert/.frag/.glsl/.comp 是编辑期产物
        "shaders/glsl/**/*.vert",
        "shaders/glsl/**/*.frag",
        "shaders/glsl/**/*.glsl",
        "shaders/glsl/**/*.comp",
    };
    return rules;
}

PackRules LoadPackRules(const std::string &yamlPath, std::string &err) {
    if (yamlPath.empty()) {
        err = "未指定规则文件，使用内置默认规则";
        return DefaultPackRules();
    }
    try {
        const YAML::Node root = YAML::LoadFile(yamlPath);
        PackRules rules;
        rules.sourcePath = yamlPath;
        if (const YAML::Node ex = root["exclude"]) {
            if (ex.IsSequence()) {
                for (const auto &item : ex) {
                    const std::string pattern = item.as<std::string>("");
                    if (!pattern.empty()) {
                        rules.excludes.push_back(pattern);
                    }
                }
            } else {
                err = "规则文件 exclude 段不是序列: " + yamlPath + "（已忽略该段）";
                return DefaultPackRules();
            }
        }
        return rules;
    } catch (const std::exception &e) {
        err = std::string("规则文件读取失败，使用内置默认规则: ") + e.what();
        return DefaultPackRules();
    }
}

} // namespace gepack
