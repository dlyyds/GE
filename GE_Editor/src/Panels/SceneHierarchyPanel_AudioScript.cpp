//
// 场景层级面板 —— 音频 / 脚本类组件属性绘制。
//
// 本文件为 SceneHierarchyPanel 拆分的一部分，包含：AudioSource（含
// SoundPathFromDialog）、AudioListener、Script（含 ScriptPathFromDialog）。
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
// 把文件对话框返回的绝对路径转成资产根相对路径（AudioSource 保存相对路径）
static std::string SoundPathFromDialog(const std::string &absPath) {
    std::string p = std::filesystem::absolute(absPath).lexically_normal().generic_string();
    std::string root = std::filesystem::absolute(Renderer::GetAssetManager().GetAssetRoot())
                                  .lexically_normal().generic_string();
    if (!root.empty() && root.back() != '/')
        root += '/';
    if (p.size() >= root.size() && p.compare(0, root.size(), root) == 0)
        return p.substr(root.size());
    return p;
}

// ============================================================
// Audio Source / Audio Listener 组件
// ============================================================
void SceneHierarchyPanel::DrawAudioSourceComponent(AudioSourceComponent &component) {
    ImGui::Checkbox("Enabled (启用)", &component.Enabled);
    ImGui::Checkbox("Spatial (3D 空间化)", &component.Spatial);
    ImGui::Separator();

    if (ImGui::Button("Add Sound (添加声音)"))
        component.Sounds.emplace_back();

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(component.Sounds.size()); ++i) {
        AudioSound &s = component.Sounds[i];
        ImGui::PushID(i);
        ImGui::Text("Slot %d (音槽)", i);

        char nameBuf[256] = {};
        strncpy_s(nameBuf, sizeof(nameBuf), s.Name.c_str(), _TRUNCATE);
        if (ImGui::InputText("Name (名称)##slot", nameBuf, sizeof(nameBuf)))
            s.Name = nameBuf;

        ImGui::Text("Sound Path (声音路径): %s", s.SoundPath.empty() ? "(未选择)" : s.SoundPath.c_str());
        if (ImGui::Button("浏览声音...##slot")) {
            std::string absPath = FileDialogs::OpenFile(
                "音频文件 (*.wav;*.ogg;*.flac;*.mp3)\0*.wav;*.ogg;*.flac;*.mp3\0"
                "All Files (*.*)\0*.*\0",
                "assets/audio");
            if (!absPath.empty())
                s.SoundPath = SoundPathFromDialog(absPath);
        }
        if (!s.SoundPath.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("清除##slot"))
                s.SoundPath.clear();
        }

        ImGui::Checkbox("Play On Awake (播放时开始)", &s.PlayOnAwake);
        ImGui::Checkbox("Loop (循环)", &s.Loop);
        ImGui::DragFloat("Volume (音量)", &s.Volume, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Pitch (音高)", &s.Pitch, 0.01f, 0.1f, 4.0f);

        ImGui::DragFloat("Min Distance (最小距离)", &s.MinDistance, 0.1f, 0.0f, 1000.0f);
        ImGui::DragFloat("Max Distance (最大距离)", &s.MaxDistance, 0.1f, 0.0f, 1000.0f);
        ImGui::DragFloat("Rolloff (衰减率)", &s.Rolloff, 0.05f, 0.0f, 10.0f);

        const char *attItems[] = { "Inverse (反比)", "Linear (线性)", "Exponential (指数)", "None (无)" };
        int attIdx = 0;
        switch (s.Attenuation) {
        case Audio::AttenuationModel::Linear:      attIdx = 1; break;
        case Audio::AttenuationModel::Exponential: attIdx = 2; break;
        case Audio::AttenuationModel::None:        attIdx = 3; break;
        default:                                   attIdx = 0; break;
        }
        if (ImGui::Combo("Attenuation (衰减模型)##slot", &attIdx, attItems, 4)) {
            if (attIdx == 1)      s.Attenuation = Audio::AttenuationModel::Linear;
            else if (attIdx == 2) s.Attenuation = Audio::AttenuationModel::Exponential;
            else if (attIdx == 3) s.Attenuation = Audio::AttenuationModel::None;
            else                  s.Attenuation = Audio::AttenuationModel::Inverse;
        }

        if (!s.SoundPath.empty() && ImGui::Button("Preview (试听)##slot"))
            Renderer::GetAssetManager().PlayOneShot(s.SoundPath, 1.0f);

        ImGui::SameLine();
        if (ImGui::Button("Remove (移除)##slot"))
            removeIndex = i;

        ImGui::Separator();
        ImGui::PopID();
    }

    if (removeIndex >= 0)
        component.Sounds.erase(component.Sounds.begin() + removeIndex);
}

void SceneHierarchyPanel::DrawAudioListenerComponent(AudioListenerComponent &component) {
    ImGui::Checkbox("Enabled (启用)", &component.Enabled);
    ImGui::TextWrapped("Listener 位置来自实体 Transform；无 Listener 时回退主相机。");
}

// ============================================================
// Script 组件
// ============================================================

// 把对话框返回的绝对路径转成 assets/scripts/ 相对路径（ScriptEngine 基准目录）
static std::string ScriptPathFromDialog(const std::string &absPath) {
    std::string p = absPath;
    for (auto &c : p)
        if (c == '\\')
            c = '/';
    const std::string marker = "assets/scripts/";
    const size_t pos = p.rfind(marker);
    if (pos != std::string::npos)
        return p.substr(pos + marker.size());
    // 兜底：路径不在 assets/scripts 下，退化为仅文件名
    const size_t slash = p.rfind('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

void SceneHierarchyPanel::DrawScriptComponent(ScriptComponent &component, Entity entity) {
    ImGui::Checkbox("Enabled", &component.Enabled);

    // 当前脚本路径（只读显示，选择走对话框）
    ImGui::Text("Script: %s", component.ScriptPath.empty() ? "(未选择)" : component.ScriptPath.c_str());

    if (ImGui::Button("浏览脚本...")) {
        std::string absPath = FileDialogs::OpenFile(
            "Lua Script (*.lua)\0*.lua\0All Files (*.*)\0*.*\0", "assets/scripts");
        if (!absPath.empty()) {
            component.ScriptPath = ScriptPathFromDialog(absPath);
            // 路径选定即重挂（旧实例卸载 + 新脚本载入）
            if (Scene *scene = entity.GetScene()) {
                scene->GetScriptEngine().OnComponentAdded(static_cast<entt::entity>(entity));
            }
        }
    }

    if (!component.ScriptPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("清除脚本")) {
            component.ScriptPath.clear();
            if (Scene *scene = entity.GetScene())
                scene->GetScriptEngine().OnEntityDestroyed(static_cast<entt::entity>(entity));
        }
        ImGui::SameLine();
        if (ImGui::Button("重载")) {
            // 热重载：换逻辑不换状态（实例字段保留，见计划书 9.8）
            if (Scene *scene = entity.GetScene())
                scene->GetScriptEngine().Reload(component.ScriptPath);
        }
    }

    // 状态行：加载状态 + 最近错误（含限频自动禁用提示）
    if (Scene *scene = entity.GetScene()) {
        auto &eng = scene->GetScriptEngine();
        const bool mounted = eng.HasInstance(static_cast<entt::entity>(entity));
        const std::string lastErr = eng.GetLastError(static_cast<entt::entity>(entity));
        if (!mounted) {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "状态: 未加载");
        } else if (!component.Enabled && !lastErr.empty()) {
            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.28f, 1.0f), "状态: 已暂停（连续报错自动禁用）");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("连续 OnUpdate 报错超过 3 次自动置 Enabled=false；再勾选即重新计数");
        } else if (!component.Enabled) {
            ImGui::TextColored(ImVec4(0.85f, 0.8f, 0.5f, 1.0f), "状态: 已暂停（手动禁用）");
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 0.35f, 1.0f), "状态: 已加载");
        }
        if (!lastErr.empty())
            ImGui::TextWrapped("最近错误: %s", lastErr.c_str());
    }

    // ---- public 字段（脚本 PUBLIC_FIELDS 声明 → 输入控件；值写回组件，保存时落盘）----
    if (Scene *scene = entity.GetScene()) {
        auto &eng = scene->GetScriptEngine();
        const auto schema = eng.GetPublicFieldSchema(static_cast<entt::entity>(entity));
        if (!schema.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Public Fields");
            ImGui::Indent();
            for (const auto &meta : schema) {
                auto it = component.PublicFields.find(meta.Name);
                if (it == component.PublicFields.end() || it->second.Type != meta.Type) {
                    // 缺失（脚本新增字段）或类型已变 → 按 schema 重置默认
                    it = component.PublicFields.insert_or_assign(
                             meta.Name,
                             ScriptPublicField{meta.Type, meta.NumberDefault,
                                               meta.BoolDefault, meta.StringDefault})
                             .first;
                }
                switch (meta.Type) {
                case ScriptFieldType::Number:
                    ImGui::DragFloat(meta.Name.c_str(), &it->second.Number, 0.1f);
                    break;
                case ScriptFieldType::Bool:
                    ImGui::Checkbox(meta.Name.c_str(), &it->second.Bool);
                    break;
                case ScriptFieldType::String: {
                    char buf[256];
                    std::strncpy(buf, it->second.String.c_str(), sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';
                    if (ImGui::InputText(meta.Name.c_str(), buf, sizeof(buf)))
                        it->second.String = buf;
                    break;
                }
                default:
                    break;
                }
            }
            ImGui::Unindent();
        }
    }

    ImGui::TextDisabled("脚本位于 assets/scripts/（相对该目录，含 .lua 后缀）");
}

} // namespace GE
