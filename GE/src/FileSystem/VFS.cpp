/**
 * @file VFS.cpp
 * @brief 只读资产 VFS 实现（磁盘后端 + Android AAssetManager 后端）。
 *
 * 两个后端刻意放在同一个 TU 里：Android 后端整块包在 `#ifdef` 内，独立成文件的话
 * 桌面构建会得到一个空 TU（MSVC 给 LNK4221），而这点平台分支并不值得付那个代价。
 */

#include "pch.h"

#include "FileSystem/VFS.h"
#include "Core/Log.h"

#include <fstream>

#ifdef GE_PLATFORM_ANDROID
#include <android/asset_manager.h>
#endif

namespace GE::VFS {

namespace {

Backend s_Backend = Backend::None;

/// 磁盘后端的资产根（绝对路径）。只写一次，之后只读 → 后台线程并发读安全。
std::filesystem::path s_DiskRoot;

#ifdef GE_PLATFORM_ANDROID
/// AAssetManager 后端的句柄。由 SDL/JNI 在启动时取得，进程生命周期内有效。
AAssetManager *s_AssetManager = nullptr;
#endif

// ============================================================================
// 磁盘后端
// ============================================================================

bool ReadAllDisk(const std::string &canonical, std::vector<uint8_t> &out) {
    // 伪键（builtin: / solid:）不是文件路径，打开必然失败 —— 直接短路，避免在
    // Windows 上把 "builtin:cube" 当成"驱动器 relative 路径"去解析。
    if (canonical.empty() || canonical.find(':') != std::string::npos) {
        return false;
    }

    const std::filesystem::path full = s_DiskRoot / std::filesystem::path(canonical);

    std::ifstream in(full, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }

    const std::streampos end = in.tellg();
    if (end <= 0) {
        // 空文件与读取失败都落到这里：调用方拿到的都是"读不到内容"，无需区分
        return false;
    }
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf(static_cast<size_t>(end));
    in.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!in) {
        return false;
    }

    out = std::move(buf);
    return true;
}

bool ExistsDisk(const std::string &canonical) {
    if (canonical.empty() || canonical.find(':') != std::string::npos) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(s_DiskRoot / std::filesystem::path(canonical), ec);
}

// ============================================================================
// Android 后端（APK 内的 assets/，只读）
// ============================================================================

#ifdef GE_PLATFORM_ANDROID

bool ReadAllAndroid(const std::string &canonical, std::vector<uint8_t> &out) {
    if (canonical.empty() || canonical.find(':') != std::string::npos) {
        return false;
    }

    AAsset *asset = AAssetManager_open(s_AssetManager, canonical.c_str(), AASSET_MODE_RANDOM);
    if (!asset) {
        return false;
    }

    const off64_t size = AAsset_getLength64(asset);
    if (size <= 0) {
        AAsset_close(asset);
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(size));

    // 循环读满：AAsset_read 单次不保证填满整个缓冲（尤其对 APK 内被压缩的资产），
    // 一次读就比对长度会在真机上误判成"文件损坏"。192MB 的 skybox 尤其吃这条。
    size_t total = 0;
    while (total < buf.size()) {
        const int got = AAsset_read(asset, buf.data() + total, buf.size() - total);
        if (got <= 0) {
            break; // -1 = 错误，0 = 提前结束
        }
        total += static_cast<size_t>(got);
    }
    AAsset_close(asset);

    if (total != buf.size()) {
        GE_CORE_ERROR("VFS: 资产短读 {0}（期望 {1} 字节，实得 {2}）", canonical, buf.size(), total);
        return false;
    }

    out = std::move(buf);
    return true;
}

bool ExistsAndroid(const std::string &canonical) {
    if (canonical.empty() || canonical.find(':') != std::string::npos) {
        return false;
    }
    AAsset *asset = AAssetManager_open(s_AssetManager, canonical.c_str(), AASSET_MODE_UNKNOWN);
    if (!asset) {
        return false;
    }
    AAsset_close(asset);
    return true;
}

#endif // GE_PLATFORM_ANDROID

} // namespace

// ============================================================================
// 公共接口
// ============================================================================

void Init(const std::filesystem::path &assetRootAbs) {
    s_DiskRoot = assetRootAbs.lexically_normal();
    s_Backend = s_DiskRoot.empty() ? Backend::None : Backend::Disk;
    GE_CORE_INFO("VFS: 磁盘后端，资产根 = {0}", s_DiskRoot.string());
}

#ifdef GE_PLATFORM_ANDROID

void InitAndroid(void *assetManager) {
    s_AssetManager = static_cast<AAssetManager *>(assetManager);
    s_Backend = s_AssetManager ? Backend::AndroidAsset : Backend::None;
    GE_CORE_INFO("VFS: AAssetManager 后端（APK 内 assets/），句柄 {0}",
                 s_AssetManager ? "有效" : "为空 —— 资产将全部读取失败");
}

#endif

void Shutdown() {
#ifdef GE_PLATFORM_ANDROID
    s_AssetManager = nullptr;
#endif
    s_DiskRoot.clear();
    s_Backend = Backend::None;
}

bool IsInitialized() {
    return s_Backend != Backend::None;
}

Backend GetBackend() {
    return s_Backend;
}

bool ReadAll(const std::string &canonical, std::vector<uint8_t> &out) {
    switch (s_Backend) {
        case Backend::Disk:
            return ReadAllDisk(canonical, out);
#ifdef GE_PLATFORM_ANDROID
        case Backend::AndroidAsset:
            return ReadAllAndroid(canonical, out);
#endif
        case Backend::None:
        default:
            return false;
    }
}

std::vector<uint8_t> ReadAll(const std::string &canonical) {
    std::vector<uint8_t> out;
    ReadAll(canonical, out);
    return out;
}

std::string ReadText(const std::string &canonical) {
    // 失败**刻意不打日志**：读不到的语义由各调用方按自己的上下文表述（YAML 解析
    // 失败 / 着色器读不到 / 材质缺失…），这里再打一条只是重复。更要紧的是
    // Lua 的 `package.searchers` 会对每个 require 逐个试探，探测失败是**正常路径**，
    // 在这里打错误会把日志刷满。
    std::vector<uint8_t> bytes;
    if (!ReadAll(canonical, bytes)) {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

bool Exists(const std::string &canonical) {
    switch (s_Backend) {
        case Backend::Disk:
            return ExistsDisk(canonical);
#ifdef GE_PLATFORM_ANDROID
        case Backend::AndroidAsset:
            return ExistsAndroid(canonical);
#endif
        case Backend::None:
        default:
            return false;
    }
}

} // namespace GE::VFS
