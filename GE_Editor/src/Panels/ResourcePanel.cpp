#include "Panels/ResourcePanel.h"

#include "GE/Core/Log.h"
#include "GE/Utils/PlatformUtils.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/MeshManager.h"

#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdio>  // std::snprintf
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GE {

namespace {

/// 纹理槽位显示名称（与 Material::TextureSlot 一致）
const char *const kTextureSlotNames[] = {"Albedo", "Normal", "Emissive", "MetallicRoughness"};

/// 将 vk::Format 枚举转换为可读字符串（仅覆盖常用格式）
const char *FormatToString(vk::Format format) {
    switch (format) {
        case vk::Format::eR8G8B8A8Unorm:   return "R8G8B8A8Unorm";
        case vk::Format::eR8G8B8A8Srgb:    return "R8G8B8A8Srgb";
        case vk::Format::eR16G16B16A16Sfloat: return "R16G16B16A16Float";
        case vk::Format::eR32G32B32A32Sfloat: return "R32G32B32A32Float";
        case vk::Format::eB8G8R8A8Unorm:   return "B8G8R8A8Unorm";
        case vk::Format::eB8G8R8A8Srgb:    return "B8G8R8A8Srgb";
        default: return "Unknown";
    }
}

/// 已知标量参数的编辑描述（标签 + 范围 + 步进），未知参数用通用 DragFloat
struct FloatParamDesc {
    const char *label;
    float min, max, speed;
};

/// 按参数名查找已知参数描述，未命中返回 nullptr
const FloatParamDesc *GetFloatParamDesc(const std::string &name) {
    static const std::unordered_map<std::string, FloatParamDesc> kKnown = {
        {"shininess",        {"高光锐度",       2.0f, 512.0f, 1.0f}},
        {"specularStrength", {"镜面强度",       0.0f, 2.0f,   0.01f}},
        {"emissiveStrength", {"自发光强度",     0.0f, 20.0f,  0.05f}},
        {"metallic",         {"金属度",         0.0f, 1.0f,   0.005f}},
        {"roughness",        {"粗糙度",         0.0f, 1.0f,   0.005f}},
    };
    auto it = kKnown.find(name);
    return it != kKnown.end() ? &it->second : nullptr;
}

/// 从文件路径中截取最后一段作为显示名（用于材料内缩略列表）
std::string FileNameFromPath(const std::string &path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

} // namespace

// 注意：ResourcePanel 析构时不释放缩略图描述符集。
// 这些描述符集由 ImGui 后端的描述符池持有，会在 ImGui_ImplVulkan_Shutdown()
// 销毁池时一并释放；而本面板析构发生在 LayerStack::Clear() 的 m_Layers.clear()
// 阶段，此时 ImGui 后端已先一步 Shutdown（bd 为 null），故不可在析构里调用
// ImGui_ImplVulkan_RemoveTexture，否则会崩溃。

void ResourcePanel::OnImGuiRender() {
    // 根上下文取一次停靠目标 ID（与 DockSpaceLayer 中 GetID("MainDockspace") 一致）
    if (m_DockSpaceID == 0) {
        m_DockSpaceID = ImGui::GetID("MainDockspace");
    }

    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("Resource");

    // 收集当前仍存活的纹理（管理器缓存 + 材质槽位引用），清理失效的缩略图
    std::vector<const Texture *> live;
    auto &texMgr = Renderer::GetTextureManager();
    for (const auto &k : texMgr.GetAllKeys()) {
        if (auto *t = texMgr.Get(k)) {
            live.push_back(t);
        }
    }
    auto &matMgr = Renderer::GetMaterialManager();
    for (const auto &n : matMgr.GetAllNames()) {
        if (auto *m = matMgr.Get(n)) {
            for (size_t s = 0; s < Material::TextureSlot::Count; ++s) {
                if (auto *t = m->GetTexture(static_cast<Material::TextureSlot>(s))) {
                    live.push_back(t);
                }
            }
        }
    }
    PruneThumbnails(live);

    DrawStatsBar();

    // 三段切换为 Tab 页，视觉更紧凑
    if (ImGui::BeginTabBar("##ResourceTabs")) {
        if (ImGui::BeginTabItem("纹理")) {
            DrawTextureSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("材质")) {
            DrawMaterialSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("网格")) {
            DrawMeshSection();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void ResourcePanel::DrawStatsBar() {
    auto &texMgr   = Renderer::GetTextureManager();
    auto &matMgr   = Renderer::GetMaterialManager();
    auto &meshMgr  = Renderer::GetMeshManager();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "纹理"); ImGui::SameLine();
    ImGui::Text("%zu", texMgr.GetCount()); ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.55f, 1.0f), "材质"); ImGui::SameLine();
    ImGui::Text("%zu", matMgr.GetCount()); ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "网格"); ImGui::SameLine();
    ImGui::Text("%zu", meshMgr.GetCount());
    ImGui::Separator();
    ImGui::PopStyleVar();
}

void ResourcePanel::DrawTextureSection() {
    auto &texMgr = Renderer::GetTextureManager();

    ImGui::InputTextWithHint("##texfilter", "过滤纹理名...", m_TextureFilter, sizeof(m_TextureFilter));

    // 新增纹理表单
    DrawTextureCreationControls();

    ImGui::Separator();

    const auto keys = texMgr.GetAllKeys();
    if (keys.empty()) {
        ImGui::TextDisabled("暂无纹理");
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    for (const auto &key : keys) {
        if (m_TextureFilter[0] && key.find(m_TextureFilter) == std::string::npos) {
            continue;
        }
        Texture *tex = texMgr.Get(key);
        if (!tex) {
            continue;
        }

        ImGui::PushID(key.c_str());

        // 树节点：名称 + 缩略图 + 尺寸/格式，展开后编辑采样器
        bool open = ImGui::TreeNode(key.c_str());
        if (ImTextureID tid = GetThumbnail(tex)) {
            ImGui::SameLine();
            ImGui::Image(tid, ImVec2(20.0f, 20.0f));
        }
        ImGui::SameLine();
        const auto &e = tex->GetExtent();
        ImGui::TextDisabled("%ux%u  %s", e.width, e.height, FormatToString(tex->GetFormat()));
        if (ImGui::IsItemHovered() && !tex->GetFilePath().empty()) {
            ImGui::SetTooltip("%s", tex->GetFilePath().c_str());
        }

        if (open) {
            DrawSamplerControls(tex);
            ImGui::TreePop();
        }

        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::PopStyleVar();
}

void ResourcePanel::DrawTextureCreationControls() {
    auto &texMgr = Renderer::GetTextureManager();

    if (ImGui::CollapsingHeader("新增纹理", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 纯色纹理
        ImGui::ColorEdit4("颜色", m_NewTexColor, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        if (ImGui::Button("创建纯色纹理")) {
            // 按颜色去重：已存在则复用，否则新建 1x1 纯色纹理
            texMgr.GetSolidColor(glm::vec4(m_NewTexColor[0], m_NewTexColor[1],
                                           m_NewTexColor[2], m_NewTexColor[3]));
        }

        // 从文件加载
        ImGui::InputText("文件路径", m_NewTexPath, sizeof(m_NewTexPath));
        ImGui::SameLine();
        if (ImGui::Button("浏览...")) {
            std::string path = FileDialogs::OpenFile(
                "Image Files (*.png *.jpg *.jpeg *.bmp *.tga)\0"
                "*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
                "All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                std::snprintf(m_NewTexPath, sizeof(m_NewTexPath), "%s", path.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("加载纹理") && m_NewTexPath[0]) {
            if (!texMgr.Load(m_NewTexPath)) {
                GE_CORE_ERROR("纹理加载失败：{}", m_NewTexPath);
            }
        }

        ImGui::Separator();
    }
}

void ResourcePanel::DrawSamplerControls(Texture *tex) {
    // 与材质段一致的属性表：左列控件，右列左对齐标签
    if (ImGui::BeginTable("##samp", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("widget", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 120.0f);

        const char *filterNames[] = {"Nearest", "Linear"};
        const char *addrNames[]   = {"Repeat", "ClampToEdge", "MirrorRepeat"};

        // 放大过滤
        int mag = (tex->GetMagFilter() == vk::Filter::eNearest) ? 0 : 1;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##mag", &mag, filterNames, 2)) {
            tex->SetFilter(mag == 0 ? vk::Filter::eNearest : vk::Filter::eLinear,
                           tex->GetMinFilter());
        }
        DrawPropertyLabel("Mag 过滤");

        // 缩小过滤
        int min = (tex->GetMinFilter() == vk::Filter::eNearest) ? 0 : 1;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##min", &min, filterNames, 2)) {
            tex->SetFilter(tex->GetMagFilter(),
                           min == 0 ? vk::Filter::eNearest : vk::Filter::eLinear);
        }
        DrawPropertyLabel("Min 过滤");

        // 寻址模式
        int addr = 0;
        switch (tex->GetAddressMode()) {
            case vk::SamplerAddressMode::eClampToEdge:   addr = 1; break;
            case vk::SamplerAddressMode::eMirroredRepeat: addr = 2; break;
            default:                                      addr = 0; break;
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##addr", &addr, addrNames, 3)) {
            vk::SamplerAddressMode mode = vk::SamplerAddressMode::eRepeat;
            if (addr == 1) {
                mode = vk::SamplerAddressMode::eClampToEdge;
            } else if (addr == 2) {
                mode = vk::SamplerAddressMode::eMirroredRepeat;
            }
            tex->SetAddressMode(mode);
        }
        DrawPropertyLabel("寻址模式");

        // 各向异性
        bool anis = tex->GetAnisotropyEnabled();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("##anis", &anis);
        DrawPropertyLabel("各向异性");
        if (anis != tex->GetAnisotropyEnabled()) {
            tex->SetAnisotropy(anis);
        }

        ImGui::EndTable();
    }
}

void ResourcePanel::DrawMaterialSection() {
    auto &matMgr = Renderer::GetMaterialManager();
    auto &texMgr = Renderer::GetTextureManager();

    const auto names = matMgr.GetAllNames();
    if (names.empty()) {
        ImGui::TextDisabled("暂无材质");
        return;
    }

    const auto texKeys = texMgr.GetAllKeys();

    for (const auto &name : names) {
        Material *mat = matMgr.Get(name);
        if (!mat) {
            continue;
        }

        ImGui::PushID(name.c_str());
        if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            // 属性表：左列控件（统一宽度），右列右侧对齐标签
            if (ImGui::BeginTable("##Props", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("widget", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 180.0f);

                // 着色器类型（运行时切换会重建对应管线）
                const char *types[] = {"BlinnPhong", "PBR"};
                int typeIdx = (mat->GetType() == Material::Type::PBR) ? 1 : 0;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::Combo("##type", &typeIdx, types, 2)) {
                    mat->SetType(typeIdx == 1 ? Material::Type::PBR : Material::Type::BlinnPhong);
                }
                DrawPropertyLabel("着色器类型");

                // 纹理槽位（左列下拉赋值，右列标签带缩略图）
                for (size_t i = 0; i < Material::TextureSlot::Count; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    DrawTextureAssignRow(mat, static_cast<Material::TextureSlot>(i), texKeys);
                    DrawPropertyLabel(kTextureSlotNames[i],
                                      mat->GetTexture(static_cast<Material::TextureSlot>(i)));
                }

                // 标量参数（可编辑；先拷贝再写回，避免修改 unordered_map 时迭代器失效）
                std::vector<std::pair<std::string, float>> params(
                    mat->GetFloatParams().begin(), mat->GetFloatParams().end());
                if (params.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("无标量参数");
                    DrawPropertyLabel("标量参数");
                }
                for (auto &[pname, pval] : params) {
                    const FloatParamDesc *desc = GetFloatParamDesc(pname);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(pname.c_str());
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (desc) {
                        if (ImGui::SliderFloat("##p", &pval, desc->min, desc->max, "%.3f")) {
                            mat->SetFloat(pname, pval);
                        }
                    } else if (ImGui::DragFloat("##p", &pval, 0.01f, -1000.0f, 1000.0f, "%.3f")) {
                        mat->SetFloat(pname, pval);
                    }
                    ImGui::PopID();
                    DrawPropertyLabel(desc ? desc->label : pname.c_str());
                }

                // 渲染状态（复选框位于左列）
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("##alpha", &mat->alphaTest);
                DrawPropertyLabel("Alpha 测试");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("##double", &mat->doubleSided);
                DrawPropertyLabel("双面渲染");

                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void ResourcePanel::DrawTextureAssignRow(Material *mat, Material::TextureSlot slot,
                                         const std::vector<std::string> &texKeys) {
    auto &texMgr = Renderer::GetTextureManager();
    Texture *cur = mat->GetTexture(slot);

    ImGui::PushID(static_cast<int>(slot));

    // 预览名（取文件最后一段，避免太长）
    std::string preview = "无";
    if (cur) {
        preview = cur->GetFilePath().empty() ? "(纹理)" : FileNameFromPath(cur->GetFilePath());
    }

    // 定位当前选中项在 texKeys 中的下标（0 = 无）
    int selected = 0;
    if (cur) {
        for (size_t j = 0; j < texKeys.size(); ++j) {
            if (texMgr.Get(texKeys[j]) == cur) {
                selected = static_cast<int>(j) + 1;
                break;
            }
        }
    }

    // 下拉填满右列，右侧对齐
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##tex", preview.c_str())) {
        if (ImGui::Selectable("无", selected == 0)) {
            mat->SetTexture(slot, nullptr);
        }
        for (size_t j = 0; j < texKeys.size(); ++j) {
            bool itemSel = selected == static_cast<int>(j) + 1;
            if (ImGui::Selectable(texKeys[j].c_str(), itemSel)) {
                mat->SetTexture(slot, texMgr.Get(texKeys[j]));
            }
        }
        ImGui::EndCombo();
    }

    ImGui::PopID();
}

void ResourcePanel::DrawPropertyLabel(const char *text, Texture *thumbnail) {
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();

    // 组 = 可选缩略图 + 文本，整组左侧对齐
    if (thumbnail) {
        if (ImTextureID tid = GetThumbnail(thumbnail)) {
            ImGui::Image(tid, ImVec2(24.0f, 24.0f));
            ImGui::SameLine();
        }
    }
    ImGui::TextUnformatted(text);
}

void ResourcePanel::DrawMeshSection() {
    auto &meshMgr = Renderer::GetMeshManager();

    ImGui::InputTextWithHint("##meshfilter", "过滤网格名...", m_MeshFilter, sizeof(m_MeshFilter));
    ImGui::Separator();

    const auto keys = meshMgr.GetAllKeys();
    if (keys.empty()) {
        ImGui::TextDisabled("暂无网格");
        return;
    }

    if (ImGui::BeginTable("##mesh", 2, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("网格", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("统计", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        for (const auto &key : keys) {
            if (m_MeshFilter[0] && key.find(m_MeshFilter) == std::string::npos) {
                continue;
            }
            Mesh *mesh = meshMgr.Get(key);
            if (!mesh) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(key.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%u 顶点 / %u 索引", mesh->GetVertexCount(), mesh->GetIndexCount());
        }
        ImGui::EndTable();
    }
}

ImTextureID ResourcePanel::GetThumbnail(Texture *tex) {
    if (!tex) {
        return ImTextureID(0);
    }
    auto it = m_Thumbnails.find(tex);
    if (it != m_Thumbnails.end()) {
        return it->second;
    }
    // 采样器 + ImageView 采样作为 ImGui 图片（纹理加载后布局即 SHADER_READ_ONLY）
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        tex->GetSampler().GetHandle(),
        tex->GetImageView().GetHandle(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImTextureID id = reinterpret_cast<ImTextureID>(set);
    m_Thumbnails[tex] = id;
    return id;
}

void ResourcePanel::PruneThumbnails(const std::vector<const Texture *> &live) {
    std::unordered_set<const Texture *> present(live.begin(), live.end());
    bool needWait = false;
    auto it = m_Thumbnails.begin();
    while (it != m_Thumbnails.end()) {
        if (it->second && present.count(it->first) == 0) {
            needWait = true;
            ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(it->second));
            it = m_Thumbnails.erase(it);
        } else {
            ++it;
        }
    }
    if (needWait) {
        // 被释放的描述符集可能仍被上一帧引用，等 GPU 空闲以确保安全
        Renderer::Get().WaitIdle();
    }
}

} // namespace GE