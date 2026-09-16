#include "pch.h"

#include "FileSystem/FileSystem.h"
#include "FileSystem/VFS.h"
#include "Core/Log.h"

#include <cstring>

namespace GE {

std::vector<uint32_t> FileSystem::ReadBinaryU32(const std::string &canonicalPath) {
    std::vector<uint8_t> bytes;
    if (!VFS::ReadAll(canonicalPath, bytes)) {
        GE_CORE_ERROR("FileSystem: 读不到资产 '{0}'", canonicalPath);
        return {};
    }

    if (bytes.empty()) {
        GE_CORE_WARN("FileSystem: 资产 '{0}' 为空", canonicalPath);
        return {};
    }

    // 非 4 字节对齐时截尾：SPIR-V 模块必须是 4 字节字序列，多出来的尾字节
    // 交给 createShaderModule 只会变成校验层报错，不如在这里说明白。
    const size_t count = bytes.size() / sizeof(uint32_t);
    if (count * sizeof(uint32_t) != bytes.size()) {
        GE_CORE_WARN("FileSystem: 资产 '{0}' 长度 {1} 非 4 的倍数，已截尾",
                     canonicalPath, bytes.size());
    }

    std::vector<uint32_t> result(count);
    std::memcpy(result.data(), bytes.data(), count * sizeof(uint32_t));

    GE_CORE_TRACE("FileSystem: 已读 '{0}' —— {1} 个 uint32_t", canonicalPath, count);
    return result;
}

}
