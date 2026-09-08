//
// 场景层级面板 —— 网格/材质类组件属性绘制。
//
// 本文件为 SceneHierarchyPanel 拆分的一部分，包含：Mesh Renderer（含材质编辑器
// DrawTextureSlot / DrawMaterialEditor / DrawSubMeshMaterialEditor）、Sprite Renderer。
// 其余域见 SceneHierarchyPanel.cpp 顶部注释。
//

#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>

#include "GE/Scene/Components.h"
#include "GE/Animation/AnimationSystem.h"
#include "GE/Scene/Scene.h"
#include "GE/Physics/PhysicsWorld.h"
#include "GE/Render/Camera.h"
#include "GE/Render/Material.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/MeshManager.h"
#include "GE/Utils/PlatformUtils.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <functional>
#include <unordered_set>

#include "SceneHierarchyPanelInternal.h"

namespace GE {
// ============================================================
// Mesh 组件
// ============================================================

/// 绘制单个纹理槽位的下拉选择器（从 TextureManager 选纹理绑定到材质槽位）。
static bool DrawTextureSlot(const char *label, Material *material, Material::TextureSlot slot) {
    if (!material) {
        ImGui::Text("%s: (无材质)", label);
        return false;
    }

    auto &texMgr = Renderer::GetTextureManager();
    auto allKeys = texMgr.GetAllKeys();

    // 当前纹理的 key（用于在下拉框中显示预览文本）
    Texture *currentTex = material->GetTexture(slot);
    std::string currentPreview = "(无)";
    if (currentTex) {
        for (const auto &key : allKeys) {
            if (texMgr.Get(key) == currentTex) {
                currentPreview = key;
                break;
            }
        }
        if (currentPreview == "(无)" && !currentTex->GetFilePath().empty()) {
            currentPreview = currentTex->GetFilePath();
        } else if (currentPreview == "(无)") {
            currentPreview = "(未命名纹理)";
        }
    }

    bool changed = false;
    std::string comboLabel = std::string(label) + "##slot_" + std::to_string(slot);
    if (ImGui::BeginCombo(comboLabel.c_str(), currentPreview.c_str())) {
        // None 选项。独立压栈避免与某个 key 恰好为 "(无)" 的纹理撞 ID。
        ImGui::PushID("none");
        if (ImGui::Selectable("(无)", currentTex == nullptr)) {
            material->SetTexture(slot, nullptr);
            changed = true;
        }
        ImGui::PopID();
        if (currentTex == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出所有已加载纹理；按 key（唯一）压栈隔离，杜绝同名项 ID 冲突
        for (const auto &key : allKeys) {
            Texture *tex = texMgr.Get(key);
            bool isSelected = (tex == currentTex);
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                material->SetTexture(slot, tex);
                changed = true;
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}

/// 绘制材质内容编辑器：类型 / 纹理槽位 / 标量参数 / 渲染状态。
///
/// 直接编辑 MaterialManager 持有的 Material 对象；同一材质被多个子网格共享时
/// 改动对所有引用方即时生效。
static void DrawMaterialEditor(Material *material) {
    if (!material) {
        return;
    }

    // 材质显示名（独立字段，不改变 manager 注册 key）
    char nameBuf[128];
    snprintf(nameBuf, sizeof(nameBuf), "%s", material->GetName().c_str());
    if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
        material->SetName(nameBuf);
    }

    // 材质类型（Blinn-Phong / PBR），决定渲染管线
    const char *typeNames[] = {"Blinn-Phong", "PBR"};
    int typeIdx = static_cast<int>(material->GetType());
    if (ImGui::Combo("类型", &typeIdx, typeNames, 2)) {
        material->SetType(typeIdx == 1 ? Material::Type::PBR : Material::Type::BlinnPhong);
    }

    ImGui::Separator();

    // 纹理槽位
    DrawTextureSlot("反照率",   material, Material::Albedo);
    DrawTextureSlot("法线",   material, Material::Normal);
    DrawTextureSlot("自发光", material, Material::Emissive);
    // PBR 专属：金属-粗糙度贴图（glTF 惯例：B=metallic, G=roughness）
    if (material->GetType() == Material::Type::PBR) {
        bool mrChanged = DrawTextureSlot("金属粗糙度", material, Material::MetallicRoughness);
        // 绑定贴图时让贴图如实驱动金属度/粗糙度：标量系数自动归 1
        // （否则贴图 B/G 通道会被默认的 metallic=0、roughness=0.5 乘掉）。
        // 仅在本帧发生了"绑定"（仍是贴图）时触发，取消绑定（回到 none）不干预。
        if (mrChanged && material->GetTexture(Material::MetallicRoughness)) {
            material->SetFloat("metallic", 1.0f);
            material->SetFloat("roughness", 1.0f);
        }
    }

    ImGui::Separator();

    // 标量参数（按材质类型分流）
    if (material->GetType() == Material::Type::PBR) {
        float metallic = material->GetFloat("metallic", 0.0f);
        if (ImGui::SliderFloat("金属度", &metallic, 0.0f, 1.0f)) {
            material->SetFloat("metallic", metallic);
        }
        float roughness = material->GetFloat("roughness", 0.5f);
        if (ImGui::SliderFloat("粗糙度", &roughness, 0.0f, 1.0f)) {
            material->SetFloat("roughness", roughness);
        }
    } else {
        // Blinn-Phong：高光指数（对数刻度 0~8 → shininess = 2^位置）
        float shininess = material->GetFloat("shininess", 32.0f);
        float logShininess = std::log2(std::max(shininess, 1.0f));
        if (ImGui::SliderFloat("高光指数（对数刻度）", &logShininess,
                               0.0f, 8.0f)) {
            material->SetFloat("shininess", std::pow(2.0f, logShininess));
        }
        float specularStrength = material->GetFloat("specularStrength", 0.5f);
        if (ImGui::SliderFloat("镜面强度", &specularStrength,
                               0.0f, 2.0f)) {
            material->SetFloat("specularStrength", specularStrength);
        }
    }

    // 自发光颜色因子（两种类型共用，glTF emissiveFactor）
    // 最终自发光颜色 = 自发光贴图颜色 × 该因子；[0,0,0] 表示不发光
    glm::vec3 emissiveFactor = material->GetEmissiveFactor();
    if (ImGui::ColorEdit3("自发光颜色", glm::value_ptr(emissiveFactor))) {
        material->SetEmissiveFactor(emissiveFactor);
    }

    // 纹理平铺 / UV 缩放密度（两种类型共用，采样前乘 inUV）
    float uvTiling = material->GetFloat("uvTiling", 1.0f);
    if (ImGui::SliderFloat("纹理平铺", &uvTiling,
                           0.1f, 10.0f)) {
        material->SetFloat("uvTiling", uvTiling);
    }

    ImGui::Separator();

    // 渲染状态
    // 透明模式（对齐 glTF alphaMode）：Opaque=不透明 / Mask=裁剪 / Blend=半透明
    static const char *kAlphaModeNames[] = {"不透明", "裁剪", "半透明"};
    int alphaMode = static_cast<int>(material->alphaMode);
    if (ImGui::Combo("透明模式", &alphaMode, kAlphaModeNames,
                     IM_ARRAYSIZE(kAlphaModeNames))) {
        material->alphaMode = static_cast<Material::AlphaMode>(alphaMode);
    }
    if (material->alphaMode == Material::AlphaMode::Mask) {
        ImGui::SliderFloat("裁剪阈值", &material->alphaCutoff, 0.0f, 1.0f);
    }
    ImGui::Checkbox("双面渲染", &material->doubleSided);
}

/// 绘制子网格材质选择器：选材质即写入 MeshRendererComponent 的覆写表。
///
/// 当前生效材质 = 覆写（materialOverrides）优先，否则子网格默认材质。
/// 选择某材质 → 生成覆写（每实体独立）；选 "(使用默认)" → 清除覆写回退默认。
static void DrawSubMeshMaterialEditor(MeshRendererComponent &comp, size_t index, const SubMesh &sub) {
    auto &matMgr = Renderer::GetMaterialManager();
    auto allMats = matMgr.GetAllNames();

    // 当前覆写与生效材质
    Material *override = nullptr;
    auto it = comp.materialOverrides.find(static_cast<uint32_t>(index));
    if (it != comp.materialOverrides.end()) {
        override = it->second;
    }
    Material *effective = override ? override : sub.defaultMaterial;

    // 显示名：用材质的显示名（GetName），不显示 manager 注册 key
    std::string currentName = "(使用默认)";
    if (effective) {
        currentName = effective->GetName();
        if (currentName.empty()) {
            currentName = "(未命名)";
        }
        if (!override) {
            currentName += " [默认]";
        }
    }

    std::string label = "材质##sub_" + std::to_string(index);
    if (ImGui::BeginCombo(label.c_str(), currentName.c_str())) {
        // 使用默认（清除覆写）。独立压栈避免与某个恰好叫 "(使用默认)"
        // 的材质显示名撞 ID。
        ImGui::PushID("use_default");
        if (ImGui::Selectable("(使用默认)", override == nullptr)) {
            comp.materialOverrides.erase(static_cast<uint32_t>(index));
        }
        ImGui::PopID();
        if (override == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出 MaterialManager 中所有已加载材质（显示名 GetName，选即生成覆写）。
        // 同一显示名会对应多个材质对象（如内置几何 / 无 MTL 网格都叫 "default"）：
        // 若直接以显示名作标签，弹窗内会出现多条同名 Selectable → ID 冲突。
        // 故每条按注册 key（唯一）PushID 隔离，显示名重复也不撞 ID。
        for (const auto &name : allMats) {
            Material *mat = matMgr.Get(name);
            bool isSelected = (mat == effective);
            std::string displayName = mat ? mat->GetName() : name;
            if (displayName.empty()) {
                displayName = name;  // 兜底：无显示名时退回 key
            }
            ImGui::PushID(name.c_str());
            if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                comp.materialOverrides[static_cast<uint32_t>(index)] = mat;
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    // 有覆写 → 编辑覆写材质（每实体独立）；无覆写 → 只读提示
    if (override) {
        ImGui::Indent();
        DrawMaterialEditor(override);
        ImGui::Unindent();
    } else if (sub.defaultMaterial) {
        ImGui::TextDisabled("使用默认（只读）— 选一个材质以覆写");
    }
}

void SceneHierarchyPanel::DrawMeshRendererComponent(MeshRendererComponent &component) {
    ImGui::ColorEdit4("颜色", glm::value_ptr(component.Color));

    // ---- 网格选择下拉框 ----
    // 列出内置几何体 + 所有已加载网格，选择即替换组件的 MeshPtr。
    auto &meshMgr = Renderer::GetMeshManager();

    // 内置几何体（显示名中文化；引擎 key 仍用英文枚举，选择时按需加载并缓存）
    const char *builtinNames[] = {"立方体", "球体", "平面", "四边形"};
    const char *builtinKeys[] = {"cube", "sphere", "plane", "quad"};

    // 当前网格的显示标识（GetFilePath 为 builtin:xxx 或模型文件路径）
    std::string currentKey = "(空)";
    if (component.MeshPtr) {
        currentKey = component.MeshPtr->GetFilePath();
    }

    if (ImGui::BeginCombo("网格", currentKey.c_str())) {
        // None 选项
        if (ImGui::Selectable("(空)", component.MeshPtr == nullptr)) {
            component.MeshPtr = nullptr;
        }
        if (component.MeshPtr == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 内置几何体
        for (int bi = 0; bi < IM_ARRAYSIZE(builtinKeys); ++bi) {
            const std::string key = "builtin:" + std::string(builtinKeys[bi]);
            const bool isSelected = (component.MeshPtr &&
                                     component.MeshPtr->GetFilePath() == key);
            if (ImGui::Selectable(builtinNames[bi], isSelected)) {
                component.MeshPtr = meshMgr.GetBuiltin(builtinKeys[bi]);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        // 已加载的文件模型（排除内置，避免列表重复）
        for (const auto &key : meshMgr.GetAllKeys()) {
            if (key.rfind("builtin:", 0) == 0) {
                continue; // 已由上面的内置项覆盖
            }
            Mesh *mesh = meshMgr.Get(key);
            bool isSelected = (mesh == component.MeshPtr);
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                component.MeshPtr = mesh;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    // ---- 加载模型文件（.obj / .gemesh / .gltf / .glb）----
    if (ImGui::Button("加载模型文件 (OBJ / GEMESH / GLTF)...")) {
        std::string path = FileDialogs::OpenFile(
            "模型文件 (*.obj;*.gemesh;*.gltf;*.glb)\0*.obj;*.gemesh;*.gltf;*.glb\0"
            "Wavefront OBJ (*.obj)\0*.obj\0"
            ".gemesh 内置格式 (*.gemesh)\0*.gemesh\0"
            "glTF (*.gltf;*.glb)\0*.gltf;*.glb\0"
            "All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            Mesh *mesh = meshMgr.Load(path);
            if (mesh) {
                component.MeshPtr = mesh;
            } else {
                GE_CORE_WARN("SceneHierarchyPanel: 网格加载失败: {0}", path);
                ImGui::OpenPopup("MeshLoadFailed");
            }
        }
    }
    // 加载失败提示
    if (ImGui::BeginPopup("MeshLoadFailed")) {
        ImGui::Text("网格加载失败（请确认是合法的 .obj / .gemesh / .gltf / .glb 文件）");
        if (ImGui::Button("确定")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 网格信息 + 子网格材质编辑
    if (component.MeshPtr) {
        ImGui::Separator();
        ImGui::Text("路径: %s", component.MeshPtr->GetFilePath().c_str());

        // 异步加载中尚未就绪：不显示 0 统计 / 空子网格（避免误导），就绪后自动显示
        if (!component.MeshPtr->IsReady()) {
            ImGui::TextDisabled("加载中（异步）...");
        } else {
            ImGui::Text("顶点数: %u", component.MeshPtr->GetVertexCount());
            ImGui::Text("索引数:  %u", component.MeshPtr->GetIndexCount());

            // ---- 子网格列表：每个子网格一个可折叠下拉框，展开后绑定/编辑材质 ----
            const auto &subMeshes = component.MeshPtr->GetSubMeshes();
            ImGui::Separator();
            ImGui::Text("子网格数: %zu", subMeshes.size());
            for (size_t i = 0; i < subMeshes.size(); ++i) {
                // 折叠标题：显示子网格索引 + 索引数量
                std::string header = "子网格 " + std::to_string(i) +
                                     "（" + std::to_string(subMeshes[i].indexCount) + " 索引）";
                if (ImGui::CollapsingHeader(header.c_str())) {
                    // CollapsingHeader 带 NoTreePushOnOpen，内容不会推入 ID 栈：若不在
                    // 这里按子网格索引 PushID，各子网格材质编辑器内同名的 Combo/Slider/
                    // InputText（如 Albedo##slot_0）在 Properties 窗口共享同一 ID 作用域，
                    // 会产生大量「ID 已被占用」的告警。故此处显式按索引开作用域。
                    ImGui::Indent();
                    ImGui::PushID(static_cast<int>(i));
                    DrawSubMeshMaterialEditor(component, i, subMeshes[i]);
                    ImGui::PopID();
                    ImGui::Unindent();
                }
            }
        }
    }
}

// ============================================================
// Sprite Renderer 组件
// ============================================================
void SceneHierarchyPanel::DrawSpriteRendererComponent(SpriteRendererComponent &component) {
    ImGui::ColorEdit4("颜色", glm::value_ptr(component.Color));
    ImGui::Text("纹理: %s", component.SpriteTexture ? "(已指定)" : "(空)");
    ImGui::Checkbox("UI 精灵（屏幕空间，不参与深度）", &component.IsUI);
}

} // namespace GE
