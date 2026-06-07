#include "pch.h"
#include "FileSystem/FileSystem.h"
#include "Core/Log.h"

#include <fstream>

namespace GE {

std::vector<uint32_t> FileSystem::ReadBinaryU32(const std::filesystem::path &filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        GE_CORE_ERROR("FileSystem: failed to open file '{0}'", filepath.string());
        return {};
    }

    auto size = file.tellg();
    if (size <= 0) {
        GE_CORE_WARN("FileSystem: file '{0}' is empty", filepath.string());
        return {};
    }
    file.seekg(0, std::ios::beg);

    const auto count = static_cast<size_t>(size) / sizeof(uint32_t);
    std::vector<uint32_t> result(count);
    file.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(size));

    if (!file) {
        GE_CORE_ERROR("FileSystem: failed to read file '{0}'", filepath.string());
        return {};
    }

    GE_CORE_INFO("FileSystem: read '{0}' — {1} uint32_t values", filepath.string(), count);
    return result;
}

}
