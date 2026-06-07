#pragma once

#include "Core/Base.h"
#include <vector>
#include <filesystem>

namespace GE {

class FileSystem {
public:
    static std::vector<uint32_t> ReadBinaryU32(const std::filesystem::path &filepath);

    static std::vector<uint32_t> ReadBinaryU32(const std::string &filepath) {
        return ReadBinaryU32(std::filesystem::path(filepath));
    }
};

}
