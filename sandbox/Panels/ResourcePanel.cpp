#include "Panels/ResourcePanel.h"

#include "GE/Render/Renderer.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/MeshManager.h"

#include "imgui.h"

#include <vector>

namespace GE {

namespace {

/// 纹理槽位显示名称（与 Material::TextureSlot 一致）
const char *const kTextureSlotNames[] = {"Albedo", "Normal", "Emissive"};

/// 将 vk::Format 枚举转换为可读字符串（仅覆盖常用格式）
const char *FormatToString(vk::Format format) {
    switch (format) {
        case vk::Format::eR8G8B8A8Unorm: return "R8G8B8A8Unorm";
        case vk::Format::eR8G8B8A8Srgb:  return "R8G8B8A8Srgb";
        case vk::Format::eR16G16B16A16Sfloat: return "R16G16B16A16Float";
        case vk::Format::eR32G32B32A32Sfloat: return "R32G32B32A32Float";
        case vk::Format::eB8G8R8A8Unorm:  return "B8G8R8A8Unorm";
        case vk::Format::eB8G8R8A8Srgb:   return "B8G8R8A8Srgb";
        default: return "Unknown";
    }
}

} // namespace

void ResourcePanel::OnImGuiRender() {
    ImGui::Begin("Resource");

    // 三段可折叠资源列表
    DrawTextureSection();
    DrawMaterialSection();
    DrawMeshSection();

    ImGui::End();
}

void ResourcePanel::DrawTextureSection() {
    auto &texMgr = Renderer::GetTextureManager();

    if (ImGui::CollapsingHeader("Textures")) {
        ImGui::TextDisabled("共 %zu 张纹理", texMgr.GetCount());
        ImGui::Separator();

        const auto keys = texMgr.GetAllKeys();
        for (const auto &key : keys) {
            Texture *tex = texMgr.Get(key);
            if (!tex) {
                continue;
            }

            // 用树节点展示单张纹理，二级展开显示详细信息
            const auto &extent = tex->GetExtent();
            if (ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%ux%u, %s)",
                                    extent.width, extent.height, FormatToString(tex->GetFormat()));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tex->GetFilePath().c_str());
                }
            }
        }
    }
}

void ResourcePanel::DrawMaterialSection() {
    auto &matMgr = Renderer::GetMaterialManager();

    if (ImGui::CollapsingHeader("Materials")) {
        ImGui::TextDisabled("共 %zu 个材质", matMgr.GetCount());
        ImGui::Separator();

        const auto names = matMgr.GetAllNames();
        for (const auto &name : names) {
            Material *mat = matMgr.Get(name);
            if (!mat) {
                continue;
            }

            if (ImGui::TreeNode(name.c_str())) {
                // 调试名称
                if (!mat->GetDebugName().empty()) {
                    ImGui::Text("调试名称: %s", mat->GetDebugName().c_str());
                }

                // 类型
                const char *typeStr = "Unknown";
                switch (mat->GetType()) {
                    case Material::Type::BlinnPhong: typeStr = "BlinnPhong"; break;
                }
                ImGui::Text("类型: %s", typeStr);

                // 纹理槽位
                for (size_t i = 0; i < Material::TextureSlot::Count; ++i) {
                    Texture *slotTex = mat->GetTexture(static_cast<Material::TextureSlot>(i));
                    if (slotTex) {
                        ImGui::Text("%s: %s",
                                    kTextureSlotNames[i], slotTex->GetFilePath().c_str());
                    }
                }

                // 标量参数
                const auto &floatParams = mat->GetFloatParams();
                if (!floatParams.empty()) {
                    if (ImGui::TreeNode("标量参数")) {
                        for (const auto &[paramName, value] : floatParams) {
                            ImGui::Text("%s = %.3f", paramName.c_str(), value);
                        }
                        ImGui::TreePop();
                    }
                }

                // 渲染状态
                ImGui::Text("AlphaTest: %s, DoubleSided: %s",
                            mat->alphaTest ? "On" : "Off",
                            mat->doubleSided ? "On" : "Off");

                ImGui::TreePop();
            }
        }
    }
}

void ResourcePanel::DrawMeshSection() {
    auto &meshMgr = Renderer::GetMeshManager();

    if (ImGui::CollapsingHeader("Meshes")) {
        ImGui::TextDisabled("共 %zu 个网格", meshMgr.GetCount());
        ImGui::Separator();

        const auto keys = meshMgr.GetAllKeys();
        for (const auto &key : keys) {
            Mesh *mesh = meshMgr.Get(key);
            if (!mesh) {
                continue;
            }

            if (ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%u 顶点, %u 索引, %s)",
                                    mesh->GetVertexCount(), mesh->GetIndexCount(),
                                    mesh->GetFilePath().c_str());
            }
        }
    }
}

} // namespace GE