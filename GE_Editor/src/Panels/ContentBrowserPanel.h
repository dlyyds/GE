#pragma once

#include <filesystem>
#include "Renderer/Texture.h"

namespace GE {

extern const std::filesystem::path g_AssetPath;

class ContentBrowserPanel {
public:
    ContentBrowserPanel();

    void OnImGuiRender();

private:
    std::filesystem::path m_CurrentDirectory;

    Ref<Texture2D> m_DirectoryIcon;
    Ref<Texture2D> m_FileIcon;
};

}
