#pragma once

#include <string>

namespace GE {

class FileDialogs {
public:
    // These return empty strings if cancelled
    static std::string OpenFile(const char *filter, const char *initialDir = nullptr);

    static std::string SaveFile(const char *filter, const char *initialDir = nullptr);
};

}