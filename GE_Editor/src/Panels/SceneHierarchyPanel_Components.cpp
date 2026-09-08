//
// 场景层级面板 —— 渲染/世界类组件属性绘制。
//
// 本文件为 SceneHierarchyPanel 拆分的一部分，包含：变换、相机、跟随相机、
// 点光/方向光/环境光、水面、环境（含环境预览缩略图）。
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
// Transform 组件
// ============================================================
void SceneHierarchyPanel::DrawTransformComponent(TransformComponent &component) {
    DrawVec3Control("平移", component.Translation, 0.0f, 120);

    // 旋转用角度显示，内部存四元数；仅在编辑器边界转成欧拉角
    glm::vec3 rotationDeg = glm::degrees(component.GetRotationEuler());
    DrawVec3Control("旋转", rotationDeg, 0.0f, 120);
    component.SetRotationEuler(glm::radians(rotationDeg));

    DrawVec3Control("缩放", component.Scale, 1.0f, 120);
}

// ============================================================
// Camera 组件
// ============================================================
void SceneHierarchyPanel::DrawCameraComponent(CameraComponent &component) {
    auto &camera = component.CameraInstance;

    ImGui::Checkbox("主相机", &component.Primary);
    ImGui::SameLine();
    ImGui::Checkbox("固定宽高比", &component.FixedAspectRatio);

    // 相机模式
    const char *modeStrings[] = {"轨道", "自由视角"};
    int currentMode = static_cast<int>(camera.GetMode());
    if (ImGui::BeginCombo("模式", modeStrings[currentMode])) {
        for (int i = 0; i < 2; i++) {
            const bool isSelected = currentMode == i;
            if (ImGui::Selectable(modeStrings[i], isSelected)) {
                camera.SetMode(static_cast<Camera::Mode>(i));
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // FOV（Camera 内部使用度数）
    float fov = camera.GetFov();
    if (ImGui::SliderFloat("FOV（度）", &fov, 10.0f, 120.0f)) {
        camera.SetPerspective(fov, camera.GetAspect(), camera.GetNear(), camera.GetFar());
    }

    // 近远裁剪面
    float nearPlane = camera.GetNear();
    if (ImGui::DragFloat("近裁剪面", &nearPlane, 0.01f, 0.001f, camera.GetFar())) {
        camera.SetPerspective(camera.GetFov(), camera.GetAspect(), nearPlane, camera.GetFar());
    }

    float farPlane = camera.GetFar();
    if (ImGui::DragFloat("远裁剪面", &farPlane, 0.5f, camera.GetNear(), 100000.0f)) {
        camera.SetPerspective(camera.GetFov(), camera.GetAspect(), camera.GetNear(), farPlane);
    }

    // 宽高比
    float aspect = camera.GetAspect();
    if (ImGui::DragFloat("宽高比", &aspect, 0.01f, 0.1f, 10.0f)) {
        camera.SetAspect(aspect);
    }

    ImGui::Separator();

    // 曝光（HDR Tonemap，仅延迟渲染生效）：乘在 HDR 线性颜色上、ACES 之前。
    // Play 态主玩法相机会把该值经 SceneLayer::OnUpdate 传给 Renderer3D 的 Tonemap UBO。
    float exposure = camera.GetExposure();
    if (ImGui::SliderFloat("曝光", &exposure, 0.01f, 8.0f, "%.2f")) {
        camera.SetExposure(exposure);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("乘在 HDR 线性颜色上、ACES 之前；1.0 = 不改亮度（仅延迟渲染生效）");
    }

    ImGui::Separator();

    if (camera.GetMode() == Camera::Mode::Orbit) {
        // 轨道相机参数（Theta/Phi/Distance 内部均为度数）
        glm::vec3 target = camera.GetTarget();
        if (ImGui::DragFloat3("目标点", &target.x, 0.1f))
            camera.SetTarget(target);

        float theta = camera.GetTheta();
        float phi   = camera.GetPhi();
        float dist  = camera.GetDistance();

        bool orbitChanged = false;
        orbitChanged |= ImGui::SliderFloat("方位角 θ", &theta, -180.0f, 180.0f);
        orbitChanged |= ImGui::SliderFloat("俯仰角 φ", &phi, -89.0f, 89.0f);
        orbitChanged |= ImGui::DragFloat("距离", &dist, 0.1f, 0.5f, 50.0f);
        if (orbitChanged) {
            camera.SetOrbit(theta, phi, dist);
        }
    } else {
        // FreeLook 相机参数（Yaw/Pitch 内部均为度数）
        glm::vec3 pos = camera.GetPosition();
        if (ImGui::DragFloat3("位置", &pos.x, 0.1f))
            camera.SetPosition(pos);

        float yaw   = camera.GetYaw();
        float pitch = camera.GetPitch();
        bool fpChanged = false;
        fpChanged |= ImGui::SliderFloat("偏航角（度）", &yaw, -180.0f, 180.0f);
        fpChanged |= ImGui::SliderFloat("俯仰角（度）", &pitch, -89.0f, 89.0f);
        if (fpChanged) {
            camera.SetYawPitch(yaw, pitch);
        }
    }

    ImGui::Separator();
    ImGui::DragFloat("鼠标灵敏度", &camera.MouseSensitivity, 0.01f, 0.01f, 5.0f);
    ImGui::DragFloat("滚轮灵敏度", &camera.ScrollSensitivity, 0.05f, 0.1f, 10.0f);
    ImGui::DragFloat("移动速度", &camera.MoveSpeed, 0.1f, 0.1f, 20.0f);
}

// ============================================================
// Follow Camera 组件（纯配置，无物理重建需求）
// ============================================================
void SceneHierarchyPanel::DrawFollowCameraComponent(FollowCameraComponent &component) {
    ImGui::Checkbox("启用", &component.Enabled);
    ImGui::Separator();

    // ---- 模式 ----
    const char *modeNames[] = {"第一人称", "第三人称"};
    const int startModeIndex = component.StartMode == FollowCameraViewMode::ThirdPerson ? 1 : 0;
    if (ImGui::BeginCombo("初始模式", modeNames[startModeIndex])) {
        for (int i = 0; i < 2; ++i) {
            const bool selected = startModeIndex == i;
            if (ImGui::Selectable(modeNames[i], selected)) {
                component.StartMode = (i == 1) ? FollowCameraViewMode::ThirdPerson
                                               : FollowCameraViewMode::FirstPerson;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("允许切换", &component.ToggleEnabled);
    int toggleKey = static_cast<int>(component.ToggleKey);
    if (ImGui::InputInt("切换键（GLFW）", &toggleKey)) {
        component.ToggleKey = static_cast<KeyCode>(toggleKey & 0xFFFF);
    }
    ImGui::TextDisabled("默认 V = 86；运行时按此键在第一/第三人称之间切换");
    if (m_Context && m_Context->IsPlaying()) {
        const char *currentMode = component.CurrentMode == FollowCameraViewMode::ThirdPerson
                                      ? "Third Person" : "First Person";
        ImGui::LabelText("当前模式（运行中）", "%s", currentMode);
    }
    ImGui::Separator();

    // ---- 第一人称 ----
    if (ImGui::CollapsingHeader("第一人称", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawVec3Control("视点偏移", component.EyeOffset, 0.05f, 120);
        ImGui::TextDisabled("相机相对角色的局部偏移（默认 +Y = 角色头顶上方）");
        ImGui::Separator();
    }

    // ---- 第三人称 ----
    if (ImGui::CollapsingHeader("第三人称", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawVec3Control("锚点偏移", component.TargetOffset, 0.05f, 120);
        ImGui::TextDisabled("相机看向的角色锚点（默认角色胸口/头高附近）");
        ImGui::DragFloat("目标距离", &component.Distance, 0.05f, component.MinDistance, component.MaxDistance);
        // 改 Min/Max 时同步钳位 Target Distance：否则序列化保存的面板值可能与运行时
        // clamp 后的实际距离不一致（运行时按 [Min, Max] 生效）。
        if (ImGui::DragFloat("最小距离", &component.MinDistance, 0.05f, 0.10f, component.MaxDistance)) {
            component.Distance = std::clamp(component.Distance, component.MinDistance, component.MaxDistance);
        }
        if (ImGui::DragFloat("最大距离", &component.MaxDistance, 0.05f, component.MinDistance, 100.0f)) {
            component.Distance = std::clamp(component.Distance, component.MinDistance, component.MaxDistance);
        }
        ImGui::DragFloat("肩位偏移", &component.ShoulderOffset, 0.05f, -5.0f, 5.0f);
        ImGui::TextDisabled(">0 右肩、<0 左肩；0 = 正中跟拍");
        ImGui::Checkbox("启用碰撞", &component.CollisionEnabled);
        if (component.CollisionEnabled) {
            ImGui::DragFloat("碰撞半径", &component.CollisionRadius, 0.01f, 0.01f, 2.0f);
            ImGui::DragFloat("碰撞余量", &component.CollisionMargin, 0.01f, 0.0f, 2.0f);
        }
        ImGui::DragFloat("平滑", &component.Smoothing, 0.1f, 0.0f, 50.0f);
        ImGui::DragFloat("缩放速度", &component.ZoomSpeed, 0.05f, 0.0f, 5.0f);
        ImGui::TextDisabled("M1：滚轮在 [最小, 最大] 内缩放；防穿墙（碰撞*）在 M2 落地");
        ImGui::Separator();
    }

    // ---- 共用鼠标 ----
    if (ImGui::CollapsingHeader("共用鼠标", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat("水平转速（度/像素）", &component.YawSpeed, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("俯仰转速（度/像素）", &component.PitchSpeed, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("最小俯仰角（度）", &component.MinPitch, 1.0f, -89.0f, 0.0f);
        ImGui::DragFloat("最大俯仰角（度）", &component.MaxPitch, 1.0f, 0.0f, 89.0f);
        ImGui::Checkbox("反转 Y", &component.InvertY);
        ImGui::TextDisabled("鼠标控制视角；第一人称相机钉在角色视点，第三人称围绕角色锚点转动");
    }
}

// ============================================================
// Point Light 组件
// ============================================================
void SceneHierarchyPanel::DrawPointLightComponent(PointLightComponent &component) {
    // 颜色 + 强度（alpha 通道作为强度）
    ImGui::ColorEdit4("颜色 + 强度", glm::value_ptr(component.Color));

    // 半径倒数（衰减系数）
    ImGui::DragFloat("半径倒数（衰减系数）", &component.RadiusInv, 0.01f, 0.01f, 5.0f);
    ImGui::Text("影响半径 ≈ %.2f", 1.0f / component.RadiusInv);
}

// ============================================================
// Directional Light 组件
// ============================================================
void SceneHierarchyPanel::DrawDirectionalLightComponent(DirectionalLightComponent &component) {
    // 颜色 + 强度（alpha 通道作为强度）
    ImGui::ColorEdit4("颜色 + 强度", glm::value_ptr(component.Color));
    ImGui::Checkbox("投射阴影", &component.CastShadow);

    // CSM 调参（Renderer3D 全局设置，非组件字段）：级数 / practical split 混合系数 /
    // 每级尺寸 / 偏差 / PCF 半径。改动下帧随 UpdateLightParams（重算各级切分矩阵）与
    // SceneLayer（重声明 N 个 pass）生效——渲染图每帧重建，无需重启。每级尺寸用对数
    // 指数滑杆保证只取 2 的幂（池按 desc 复用，尺寸变化会重新分配）。
    if (component.CastShadow) {
        auto &r3d = Renderer::Get3DRenderer();

        int cascadeCount = static_cast<int>(r3d.GetCascadeCount());
        if (ImGui::SliderInt("级联级数", &cascadeCount, 1,
                             static_cast<int>(kMaxCascades), "%d")) {
            r3d.SetCascadeCount(static_cast<uint32_t>(cascadeCount));
        }
        float lambda = r3d.GetCascadeSplitLambda();
        if (ImGui::SliderFloat("切分混合系数", &lambda, 0.0f, 1.0f, "%.2f")) {
            r3d.SetCascadeSplitLambda(lambda);
        }
        for (uint32_t c = 0; c < r3d.GetCascadeCount(); ++c) {
            const uint32_t csize = r3d.GetCascadeShadowSize(c);
            int cexp = std::max(9, static_cast<int>(std::log2(static_cast<double>(csize))));
            if (ImGui::SliderInt(("级 " + std::to_string(c) + " 尺寸 (log2)").c_str(),
                                 &cexp, 9, 13, "%d")) {
                r3d.SetCascadeShadowSize(c, 1u << static_cast<uint32_t>(cexp));
            }
        }
        // 偏差 / PCF 半径各级共享（每级独立值列 CSM 计划书 §7）
        float bias = r3d.GetShadowBias();
        if (ImGui::DragFloat("阴影偏移", &bias, 0.0001f, 0.0f, 0.01f, "%.4f")) {
            r3d.SetShadowBias(bias);
        }
        float pcfRadius = r3d.GetShadowPcfRadius();
        if (ImGui::DragFloat("PCF 半径", &pcfRadius, 0.1f, 0.0f, 4.0f, "%.1f")) {
            r3d.SetShadowPcfRadius(pcfRadius);
        }
    }

    ImGui::TextDisabled("照射方向由 Transform 的 Rotation 决定");
}

// ============================================================
// Ambient Light 组件
// ============================================================
void SceneHierarchyPanel::DrawAmbientLightComponent(AmbientLightComponent &component) {
    // 颜色 + 强度（alpha 通道作为强度）
    ImGui::ColorEdit4("颜色 + 强度", glm::value_ptr(component.Color));
    ImGui::TextDisabled("全局环境光，不依赖 Transform");
}

// ============================================================
// Water 组件（水面）
// ============================================================
void SceneHierarchyPanel::DrawTexturePicker(const char *comboId, const char *emptyHint,
                                            Texture *&texture) {
    // 下拉选择已加载纹理，取代手动填路径；仍支持从资源面板拖入
    auto &texMgr = Renderer::GetTextureManager();
    const auto allKeys = texMgr.GetAllKeys();

    // 当前贴图预览文本：优先 manager key，其次文件路径
    std::string preview = "(无)";
    if (texture) {
        for (const auto &key : allKeys) {
            if (texMgr.Get(key) == texture) {
                preview = key;
                break;
            }
        }
        if (preview == "(无)") {
            preview = texture->GetFilePath().empty()
                          ? "(未命名纹理)" : texture->GetFilePath();
        }
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo(comboId, preview.c_str())) {
        // "无"选项：独立压栈避免与某个恰好同名的纹理 key 撞 ID
        ImGui::PushID("none");
        if (ImGui::Selectable("(无)", texture == nullptr)) {
            texture = nullptr;
        }
        ImGui::PopID();
        if (texture == nullptr) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出所有已加载纹理；按 key（唯一）压栈隔离，杜绝同名项 ID 冲突
        for (const auto &key : allKeys) {
            Texture *tex = texMgr.Get(key);
            bool isSelected = (tex == texture);
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(key.c_str(), isSelected)) {
                texture = tex;
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    // 拖放目标：接受从资源面板拖来的纹理
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("TEXTURE_ASSET")) {
            const size_t keyLen = payload->DataSize > 0
                                      ? static_cast<size_t>(payload->DataSize) - 1
                                      : 0;
            const std::string key(static_cast<const char *>(payload->Data), keyLen);
            if (Texture *tex = Renderer::GetTextureManager().Get(key)) {
                texture = tex;
            }
        }
        ImGui::EndDragDropTarget();
    }
    // 从磁盘加载新贴图（加载后自动出现在下拉框里）
    if (ImGui::Button("浏览并加载...")) {
        std::string path = FileDialogs::OpenFile(
            "Image Files (*.png *.jpg *.jpeg *.bmp *.tga)\0"
            "*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
            "All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            texture = Renderer::GetAssetManager().LoadTextureAsync(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("清除")) {
        texture = nullptr;
    }
    if (texture) {
        ImGui::TextUnformatted(texture->GetFilePath().c_str());
    } else {
        ImGui::TextDisabled(emptyHint);
    }
}

void SceneHierarchyPanel::DrawWaterComponent(WaterComponent &component) {
    if (ImGui::CollapsingHeader("几何", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("尺寸（XZ）", glm::value_ptr(component.Size), 0.5f, 1.0f, 2000.0f, "%.1f");
        ImGui::DragFloat("高度偏移", &component.Height, 0.05f, -50.0f, 50.0f, "%.2f");
        int resolution = static_cast<int>(component.Resolution);
        if (ImGui::SliderInt("分辨率（N）", &resolution, 1, 256)) {
            component.Resolution = static_cast<uint32_t>(resolution);
        }
        ImGui::DragFloat("时间缩放", &component.TimeScale, 0.01f, 0.0f, 10.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("色彩 / 材质", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("深水色", glm::value_ptr(component.DeepColor));
        ImGui::DragFloat("不透明度", &component.Opacity, 0.01f, 0.0f, 1.0f, "%.2f"); // 前向/HDR 统一按 Fresnel 逐像素生效（deepColor.a 传入 shader）
        ImGui::ColorEdit3("浅水色", glm::value_ptr(component.ShallowColor));
        ImGui::DragFloat("粗糙度", &component.Roughness, 0.01f, 0.0f, 1.0f, "%.3f");
        ImGui::DragFloat("法线平铺", &component.NormalTiling, 0.1f, 0.5f, 32.0f, "%.2f");
        ImGui::DragFloat("法线强度", &component.NormalStrength, 0.01f, 0.0f, 2.0f, "%.2f");
        ImGui::DragFloat("反射强度", &component.ReflectionStrength, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::DragFloat("折射强度", &component.RefractionStrength, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::DragFloat("吸收深度", &component.AbsorptionDepth, 0.05f, 0.0f, 20.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("法线贴图", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 在波法线上叠加细节扰动；强度/平铺滑条见「色彩 / 材质」区。
        ImGui::PushID("NormalMap");
        DrawTexturePicker("##slot", "(无法线贴图)，默认平坦法线", component.NormalMap);
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("色彩贴图", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 水面固有色贴图：采样颜色按「色彩强度」与深水色 mix 成受光底色。
        // 未贴图时渲染端强制强度 0 → 视觉等同现在的纯深水色（老场景/老文件不变）。
        ImGui::PushID("ColorMap");
        DrawTexturePicker("##slot", "(无色彩贴图)，保持纯色（深/浅水色）", component.ColorMap);
        const bool hasColorMap = component.ColorMap != nullptr;
        if (!hasColorMap) {
            ImGui::BeginDisabled();
        }
        ImGui::DragFloat("色彩平铺", &component.ColorTiling, 0.1f, 0.1f, 64.0f, "%.2f");
        ImGui::DragFloat("色彩强度", &component.ColorStrength, 0.01f, 0.0f, 1.0f, "%.2f");
        if (!hasColorMap) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("加载贴图后可调（当前无贴图，保持纯深水色）");
        }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Gerstner 波", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("目前支持最多 4 层，波长 <= 0 视为关闭");
        for (int i = 0; i < 4; ++i) {
            auto &w = component.Waves[i];
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
            if (ImGui::CollapsingHeader(("波 " + std::to_string(i + 1)).c_str())) {
                ImGui::DragFloat2("方向", glm::value_ptr(w.Direction), 0.01f);
                ImGui::DragFloat("振幅", &w.Amplitude, 0.01f, 0.0f, 5.0f, "%.3f");
                ImGui::DragFloat("波长", &w.Wavelength, 0.1f, 0.0f, 100.0f, "%.1f");
                ImGui::DragFloat("速度", &w.Speed, 0.01f, 0.0f, 20.0f, "%.2f");
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("岸线（阶段 2 预留）")) {
        ImGui::DragFloat("泡沫距离", &component.FoamDistance, 0.05f, 0.0f, 20.0f, "%.2f");
        ImGui::DragFloat("泡沫强度", &component.FoamIntensity, 0.01f, 0.0f, 2.0f, "%.2f");
        ImGui::TextDisabled("需要阶段 2 场景深度后才生效");
    }
}

// ============================================================
// Environment 组件
// ============================================================
void SceneHierarchyPanel::DrawEnvironmentComponent(EnvironmentComponent &component) {
    // 扫描 assets/environments/ 下的子文件夹，作为可选环境列表
    std::vector<std::string> envNames;
    const auto envRoot = Renderer::GetAssetManager().GetAssetRoot() / "environments";
    std::error_code ec;
    if (std::filesystem::is_directory(envRoot, ec)) {
        for (const auto &entry : std::filesystem::directory_iterator(envRoot, ec)) {
            if (entry.is_directory(ec)) {
                envNames.push_back(entry.path().filename().string());
            }
        }
    }
    std::sort(envNames.begin(), envNames.end());

    // 缩略图尺寸 = 行高 × 行高（1:1，且正好贴合每行高度，不超出）
    const float thumbSize = GImGui->FontSize + GImGui->Style.FramePadding.y * 2.0f;
    const ImVec2 thumbSizeVec(thumbSize, thumbSize);

    // 当前选中环境的预览缩略图（显示在下拉框左侧）
    if (!component.Name.empty()) {
        if (ImTextureID tid = GetEnvironmentThumbnail(component.Name)) {
            ImGui::Image(tid, thumbSizeVec);
            ImGui::SameLine();
        }
    }

    // 环境名下拉框：从扫到的子文件夹中选择，选即切换环境
    std::string currentPreview = component.Name.empty() ? "(无)" : component.Name;
    if (ImGui::BeginCombo("环境名", currentPreview.c_str())) {
        // None 选项（环境名为空）
        if (ImGui::Selectable("(无)", component.Name.empty())) {
            component.Name.clear();
        }
        if (component.Name.empty()) {
            ImGui::SetItemDefaultFocus();
        }

        // 列出 environments/ 下所有子文件夹，每项右侧带预览缩略图
        for (const auto &name : envNames) {
            bool isSelected = (component.Name == name);
            bool itemSelected = ImGui::Selectable(name.c_str(), isSelected);
            // 预览图放在名称右侧同一行
            if (ImTextureID tid = GetEnvironmentThumbnail(name)) {
                ImGui::SameLine();
                ImGui::Image(tid, thumbSizeVec);
            }
            if (itemSelected) {
                component.Name = name;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    // 环境总开关（关则天空盒 + IBL 一并关闭）
    ImGui::Checkbox("启用", &component.Enabled);
    // 天空盒背景开关
    ImGui::Checkbox("天空盒", &component.SkyboxEnabled);
    // IBL 环境光开关
    ImGui::Checkbox("IBL", &component.IBLEnabled);
    // IBL 环境光强度（整体缩放 diffuse + specular 的 IBL 贡献）
    ImGui::SliderFloat("IBL 强度", &component.IBLIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::TextDisabled("环境（天空盒 + IBL）来自 environments/<Name>/，不依赖 Transform");
}

// ============================================================
// Environment 预览图缩略图
// ============================================================
ImTextureID SceneHierarchyPanel::GetEnvironmentThumbnail(const std::string &envName) {
    // 已缓存则直接返回
    auto it = m_EnvThumbnails.find(envName);
    if (it != m_EnvThumbnails.end()) {
        return it->second;
    }

    ImTextureID id = 0;
    // 加载 environments/<名称>/preview.png（不存在则返回 0，不显示缩略图）
    Texture *tex = Renderer::GetTextureManager().Load("assets/environments/" + envName + "/preview.png");
    if (tex) {
        // 用采样器 + ImageView 注册为 ImGui 图片（与 ResourcePanel::GetThumbnail 一致）
        VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
            tex->GetSampler().GetHandle(),
            tex->GetImageView().GetHandle(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        id = reinterpret_cast<ImTextureID>(set);
    }
    m_EnvThumbnails[envName] = id;
    return id;
}

} // namespace GE
