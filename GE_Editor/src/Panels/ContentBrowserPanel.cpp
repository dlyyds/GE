#include "pch.h"
#include "ContentBrowserPanel.h"

#include <imgui/imgui.h>

namespace GE {

// Once we have projects, change this
const std::filesystem::path g_AssetPath = "assets";

ContentBrowserPanel::ContentBrowserPanel()
    : m_CurrentDirectory(g_AssetPath) {
    m_DirectoryIcon = Texture2D::Create("Resources/Icons/ContentBrowser/DirectoryIcon.png");
    m_FileIcon = Texture2D::Create("Resources/Icons/ContentBrowser/FileIcon.png");
}

void ContentBrowserPanel::OnImGuiRender() {
    ImGui::Begin("Content Browser");

    if (m_CurrentDirectory != std::filesystem::path(g_AssetPath)) {
        if (ImGui::Button("<-")) {
            m_CurrentDirectory = m_CurrentDirectory.parent_path();
        }
    }

    static float padding = 16.0f;
    static float thumbnailSize = 128.0f;
    float cellSize = thumbnailSize + padding;

    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = (int)(panelWidth / cellSize);
    if (columnCount < 1)
        columnCount = 1;

    ImGui::Columns(columnCount, 0, false);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    for (auto &directoryEntry : std::filesystem::directory_iterator(m_CurrentDirectory)) {
        const auto &path = directoryEntry.path();
        auto relativePath = std::filesystem::relative(path, g_AssetPath);
        std::string filenameString = relativePath.filename().string();

        const Ref<Texture2D> icon = directoryEntry.is_directory() ? m_DirectoryIcon : m_FileIcon;
        ImGui::ImageButton(filenameString.c_str(), (ImTextureID)icon->GetRendererID(), {thumbnailSize, thumbnailSize}, {0, 1}, {1, 0});

        if (ImGui::BeginDragDropSource()) {
            const wchar_t *itemPath = relativePath.c_str();
            ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath, (wcslen(itemPath) + 1) * sizeof(wchar_t));
            ImGui::EndDragDropSource();
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (directoryEntry.is_directory())
                m_CurrentDirectory /= path.filename();

        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (thumbnailSize - ImGui::CalcTextSize(filenameString.c_str()).x) * 0.5f);
        ImGui::Text("%s", filenameString.c_str());

        ImGui::NextColumn();

    }
    ImGui::PopStyleColor();
    ImGui::Columns(1);

    ImGui::SliderFloat("Thumbnail Size", &thumbnailSize, 16, 512);
    ImGui::SliderFloat("Padding", &padding, 0, 32);

    ImGui::End();
}

}
