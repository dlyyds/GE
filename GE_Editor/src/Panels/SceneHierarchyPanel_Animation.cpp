//
// 场景层级面板 —— 动画类组件属性绘制。
//
// 本文件为 SceneHierarchyPanel 拆分的一部分，包含：Joint（骨骼关节）、Skin（蒙皮）、
// Animation（骨骼动画）、AnimStateMachine（动画状态机）。
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
// Joint / Skin 组件（骨骼蒙皮阶段 B 新增）
// ============================================================
void SceneHierarchyPanel::DrawJointComponent(JointComponent &component) {
    // 关节序号由导入器按 skin.joints 顺序写入，手改无意义，只读展示
    ImGui::Text("关节索引: %d", component.jointIndex);
}

void SceneHierarchyPanel::DrawSkinComponent(SkinComponent &component) {
    ImGui::Text("关联网格: %s", component.MeshPtr ? "有" : "无");

    // 未接入皮肤定义（手动 Add Component / 导入期皮肤被跳过）：展示占位
    if (!component.skin) {
        ImGui::TextWrapped("未关联皮肤定义（导入期由 GLTFSceneImporter 填充）");
        return;
    }

    const auto &joints = component.joints();
    const auto &ibm = component.inverseBindMatrices();
    ImGui::Text("关节数量: %d", static_cast<int>(joints.size()));
    ImGui::Text("IBM 数量: %d", static_cast<int>(ibm.size()));

    // 关节实体列表（可点击选中对应实体，便于定位骨骼/后续驱动某根骨头）。
    // 同一条 glTF skin 被多 node 引用时共享同一关节表（SkinDef），各面板展示一致。
    if (!joints.empty()) {
        const std::string header = "关节实体列表(" + std::to_string(joints.size()) + ")";
        if (ImGui::TreeNode(header.c_str())) {
            auto &reg = m_Context->Reg();
            for (size_t i = 0; i < joints.size(); ++i) {
                std::string label = "[" + std::to_string(i) + "] ";
                const entt::entity handle = joints[i];
                Entity jointEntity{handle, m_Context};
                if (reg.valid(handle)) {
                    if (const auto *tag = reg.try_get<TagComponent>(handle)) {
                        label += tag->Tag;
                    } else {
                        label += "(无 Tag)";
                    }
                } else {
                    label += "(无效句柄)";
                }
                const bool selected = (m_SelectionContext == jointEntity);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    m_SelectionContext = jointEntity;
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::Checkbox("需重传关节矩阵", &component.RequiresJointUpload);
}

// ============================================================
// Animation 组件（骨骼动画阶段 B/C 新增）
// ============================================================
void SceneHierarchyPanel::DrawAnimationComponent(Entity entity, AnimationComponent &component, Scene *scene) {
    const AnimationClip *clip = component.activeClip();

    // 新增动画片段：从外部 glTF 文件只提取动画加入列表（不导入网格/场景图）。
    // 通道目标按 glTF node 名匹配宿主子树（Tag），未匹配节点洞掉为绑定姿态。
    ImGui::Text("片段源: %s", clip ? clip->source.c_str() : "(空)");
    if (ImGui::Button("新增动画片段...")) {
        std::string absPath = FileDialogs::OpenFile(
            "glTF 文件 (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0", "assets");
        if (!absPath.empty()) {
            GE_CORE_INFO("[Anim][Editor] 新增动画片段源: '{}'", absPath);
            const size_t added = AnimationSystem::AddClipsFromGLTF(
                scene->Reg(), static_cast<entt::entity>(entity), absPath);
            if (added > 0) {
                GE_CORE_INFO("[Anim][Editor] 新增 {} 条动画片段", added);
            } else {
                GE_CORE_WARN("[Anim][Editor] '{}' 无新增片段（无动画 / 加载失败 / 已在列表）", absPath);
            }
            clip = component.activeClip(); // 刷新本地指针供本帧后续 UI 使用
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("从 glTF 只提取动画加入列表");

    // 无动画片段（手动 Add Component / 空组件）：占位说明
    if (component.clips.empty() || !clip) {
        ImGui::TextWrapped("无动画片段（导入带骨骼动画的 glTF 角色会自动挂载）");
        return;
    }
    // 重载片段源：源 glTF 在磁盘更新后，按源键强制重建键帧并同步场景内同源实体。
    // 不改变 clip 数量与名字，仅刷新数据，故 ASM 运行中也可安全执行。
    if (ImGui::Button("重载片段源")) {
        GE_CORE_INFO("[Anim][Editor] 重载片段源: '{}'", clip->source);
        if (AnimationSystem::ReloadClipSource(scene->Reg(), static_cast<entt::entity>(entity))) {
            GE_CORE_INFO("[Anim][Editor] 片段源重载完成");
        }
        clip = component.activeClip(); // 键帧已替换，刷新本地指针供本帧后续 UI 使用
    }
    ImGui::SameLine();
    ImGui::TextDisabled("从源文件重建，同步同源实体并补齐新增动画");

    // 片段选择行（单片段也显示，便于删除）：选中即切换，默认走过渡淡化，填 0 即硬切。
    // 状态机开态下整行被「运行中」占位替代；手动切/删片段 = 外部覆盖、先关停 ASM。
    const bool asmRunning = entity.HasComponent<AnimStateMachineComponent>()
        && entity.GetComponent<AnimStateMachineComponent>().enabled;
    if (asmRunning) {
        const auto &asmc = entity.GetComponent<AnimStateMachineComponent>();
        const char *stateName = (asmc.current != SIZE_MAX && asmc.current < asmc.states.size())
            ? asmc.states[asmc.current].name.c_str() : "(未进入)";
        ImGui::Text("状态机运行中：%s　stateTime %.2fs", stateName, asmc.stateTime);
        ImGui::TextDisabled("改播/删除片段需先在 ASM 面板关停状态机");
    } else {
        const std::string preview = component.clips[component.active].clip
            ? component.clips[component.active].clip->name
            : ("片段 " + std::to_string(component.active));

        // 过渡时长输入：带步进按钮会占用设定宽度一部分，给足余量避免数字截断
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("过渡(s)", &component.uiBlendSec, 0.01f, 0.1f, "%.2f");
        ImGui::SameLine();
        ImGui::Text("片段");
        ImGui::SameLine();
        // 下拉框占满「片段」标签右侧余量，尾部让出「删除片段」按钮宽度；
        // 标签用 ## 隐藏，避免挤在框右侧还额外占宽导致溢出截断。
        const float delBtnW = ImGui::CalcTextSize("删除片段").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        ImGui::SetNextItemWidth(std::max(60.0f,
            ImGui::GetContentRegionAvail().x - delBtnW - ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::BeginCombo("##播放片段", preview.c_str())) {
            for (size_t i = 0; i < component.clips.size(); ++i) {
                const bool selected = (i == component.active);
                const std::string label = component.clips[i].clip
                    ? component.clips[i].clip->name
                    : ("片段 " + std::to_string(i));
                if (ImGui::Selectable(label.c_str(), selected)) {
                    // 手动切 clip = 外部覆盖：若状态机在跑，先关停（决策 9.5）
                    if (entity.HasComponent<AnimStateMachineComponent>()) {
                        auto &asmc = entity.GetComponent<AnimStateMachineComponent>();
                        if (asmc.enabled) {
                            asmc.enabled = false;
                            GE_CORE_WARN("[Anim][Editor] 手动切片段，已关停动画状态机（决策 9.5）");
                        }
                    }
                    GE_CORE_INFO("[Anim][Editor] 切换 → clip {0}（{1:.2f}s 过渡）", i, component.uiBlendSec);
                    component.PlayClip(i, component.uiBlendSec);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("删除片段")) {
            // 删除当前选中片段 = 外部覆盖：先关停状态机（决策 9.5），再清理下标避免悬空。
            if (entity.HasComponent<AnimStateMachineComponent>()) {
                auto &asmc = entity.GetComponent<AnimStateMachineComponent>();
                if (asmc.enabled) {
                    asmc.enabled = false;
                    GE_CORE_WARN("[Anim][Editor] 删除片段前已关停动画状态机（决策 9.5）");
                }
            }
            const size_t idx = component.active;
            if (idx < component.clips.size()) {
                GE_CORE_INFO("[Anim][Editor] 删除片段 → clips[{0}]", idx);
                component.clips.erase(component.clips.begin() + idx);
                // 源片段被删 → 清过渡状态（同 PlayClip 硬切路径）
                component.transitionFrom = SIZE_MAX;
                component.transitionFromTime = 0.0f;
                component.transitionElapsed = 0.0f;
                component.transitionDuration = 0.0f;
                if (component.clips.empty()) {
                    component.active = 0;
                } else if (component.active >= component.clips.size()) {
                    component.active = component.clips.size() - 1;
                }
                component.time = 0.0f;
                component.timeApplied = false;
                // 被删 clip 的共享键帧可能已无引用而释放，立即刷新本地指针避免悬空
                clip = component.activeClip();
            }
        }

        // 过渡进度（仅过渡期显示）
        if (component.transitionFrom != SIZE_MAX) {
            const float alpha = (component.transitionDuration > 0.0f)
                ? std::clamp(component.transitionElapsed / component.transitionDuration, 0.0f, 1.0f)
                : 1.0f;
            char overlay[16] = {};
            snprintf(overlay, sizeof(overlay), "%d%%", static_cast<int>(alpha * 100.0f));
            ImGui::ProgressBar(alpha, ImVec2(-1.0f, 0.0f), overlay);
        }
    }

    // 最后一个片段也被删 → 本帧直接结束，下帧走空占位分支（clips 为空时上方已 return）
    if (!clip) {
        return;
    }
    ImGui::Separator();

    // 播放控制行：播放/暂停按钮 + 循环开关
    if (ImGui::Button(component.playing ? "暂停" : "播放")) {
        component.playing = !component.playing;
    }
    ImGui::SameLine();
    ImGui::Checkbox("循环", &component.loop);

    // 速度滑条（0~3x，允许负速倒放需要输入框，此处滑条取正向）
    float speed = component.speed;
    if (ImGui::SliderFloat("速度", &speed, 0.0f, 3.0f, "%.2fx")) {
        component.speed = speed;
    }

    // 时间轴 Scrubber：手动拖动即设播放时间（便于逐帧/定点验证姿态），
    // 播放状态不受拖动影响，后续帧从该时间继续推进。
    const float duration = (clip->duration > 0.0f) ? clip->duration : 1.0f;
    float time = component.time;
    if (ImGui::SliderFloat("时间", &time, 0.0f, duration, "%.3fs")) {
        component.time = time;
    }
    ImGui::Text("时长: %.3fs, channel 数: %d",
                clip->duration, static_cast<int>(clip->channels.size()));

    // ---- 动画事件表编辑（计划书阶段 A5）：仅编辑数据，不派发 ----
    // 读/写 active clip 的事件表；每行「时间 + 名字 + 删除」，底部添加。
    ImGui::Separator();
    ImGui::Text("动画事件");
    auto &events = component.clips[component.active].events;
    int removeIdx = -1;
    char nameBuf[128] = {};
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        auto &evt = events[i];
        ImGui::PushID(i);
        float t = evt.time;
        if (ImGui::InputFloat("时间(s)", &t, 0.01f, 0.1f, "%.3f")) {
            if (clip->duration > 0.0f)
                evt.time = std::clamp(t, 0.0f, clip->duration);
            else
                evt.time = std::max(0.0f, t);
        }
        ImGui::SameLine();
        strncpy_s(nameBuf, sizeof(nameBuf), evt.name.c_str(), _TRUNCATE);
        if (ImGui::InputText("事件名", nameBuf, sizeof(nameBuf))) {
            evt.name = nameBuf;
        }
        ImGui::SameLine();
        if (ImGui::Button("删除")) {
            removeIdx = i;
        }
        ImGui::PopID();
    }
    if (removeIdx >= 0) {
        events.erase(events.begin() + removeIdx);
    }
    ImGui::TextDisabled("脚本里写 OnAnimationEvent(self, name) 按名分派");
    if (ImGui::Button("添加事件")) {
        events.push_back(AnimationEvent{});
    }
}

// ============================================================
// AnimStateMachine 组件（动画状态机，阶段 D，列表式编辑器）
// ============================================================
void SceneHierarchyPanel::DrawAnimStateMachine(Entity entity, AnimStateMachineComponent &component) {
    // 同实体 AnimationComponent 的 clip 名列表：状态「片段」下拉数据源（无则占位提示）
    const AnimationComponent *ac = m_Context->Reg().try_get<AnimationComponent>(
        static_cast<entt::entity>(entity));

    // 本地复用字符串池（条件类型 / 比较符 → ImGui::Combo 的零结尾串）
    static const char *kCondTypeItems = "浮点比较\0布尔\0驻留时长\0播放结束\0";
    static const char *kCmpItems = "大于\0大于等于\0小于\0小于等于\0约等于\0取反\0";

    // ---- 总开关 + 初始状态 + 运行状态 ----
    const bool wasEnabled = component.enabled;
    if (ImGui::Checkbox("状态机", &component.enabled)) {
        if (wasEnabled && !component.enabled) {
            // 关停 → 回到手动 PlayClip 控制：current 复位，重开时从初始状态重新进入（决策 2.4）
            component.current = SIZE_MAX;
            component.stateTime = 0.0f;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(component.enabled ? "运行中" : "关闭");

    const std::string initialPreview = component.initialState.empty() ? "(默认：第一个)" : component.initialState;
    ImGui::Text("初始状态");
    ImGui::SameLine();
    if (ImGui::BeginCombo("##asmInitial", initialPreview.c_str())) {
        if (ImGui::Selectable("(默认：第一个)", component.initialState.empty())) {
            component.initialState.clear();
        }
        for (const auto &st : component.states) {
            const bool selected = (st.name == component.initialState);
            if (ImGui::Selectable(st.name.c_str(), selected)) {
                component.initialState = st.name;
            }
        }
        ImGui::EndCombo();
    }

    if (component.enabled) {
        const char *curName = (component.current != SIZE_MAX && component.current < component.states.size())
            ? component.states[component.current].name.c_str() : "(未进入)";
        ImGui::Text("当前状态: %s　stateTime %.2fs", curName, component.stateTime);
    }
    ImGui::Separator();

    // ---- 参数调试：滑条/开关直写 floats/bools（驱动测试，不依赖脚本）----
    ImGui::Text("参数调试");
    if (component.floats.empty() && component.bools.empty()) {
        ImGui::TextDisabled("空。脚本 OnCreate 初始化参数，或在下行添加调试参数");
    }
    for (auto &kv : component.floats) {
        if (ImGui::SliderFloat(kv.first.c_str(), &kv.second, -10.0f, 10.0f, "%.2f")) {
        }
    }
    for (auto &kv : component.bools) {
        if (ImGui::Checkbox(kv.first.c_str(), &kv.second)) {
        }
    }
    // 添加调试参数（float/bool）+ 触发一次性脉冲（等价脚本 anim.trigger）
    static char sNewParamName[64] = {};
    static int sNewParamType = 0; // 0=float 1=bool
    ImGui::InputText("##asmNewParam", sNewParamName, sizeof(sNewParamName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("##asmNewParamType", &sNewParamType, "float\0bool\0");
    ImGui::SameLine();
    if (ImGui::Button("添加") && sNewParamName[0]) {
        const std::string name = sNewParamName;
        if (sNewParamType == 0) {
            component.floats[name] = 0.0f;
        } else {
            component.bools[name] = false;
        }
        sNewParamName[0] = '\0';
    }
    static char sTriggerName[64] = {};
    ImGui::InputText("##asmTrigger", sTriggerName, sizeof(sTriggerName));
    ImGui::SameLine();
    if (ImGui::Button("触发脉冲") && sTriggerName[0]) {
        component.triggers.insert(sTriggerName);
        sTriggerName[0] = '\0';
    }
    if (!component.triggers.empty()) {
        std::string pending;
        for (const auto &tn : component.triggers) {
            pending += tn + " ";
        }
        ImGui::TextDisabled("待消费触发: %s", pending.c_str());
    }
    ImGui::Separator();

    // ---- 状态表：名字 + clip 下拉 + loop/speed + 删除；当前状态加标识 ----
    ImGui::Text("状态");
    ImGui::PushID("states"); // 与下方转换表隔离 ID 域（两表是兄弟循环、都用 PushID(i)，同名「删除」会撞 ID）
    int removeState = -1;
    for (int i = 0; i < static_cast<int>(component.states.size()); ++i) {
        auto &st = component.states[i];
        ImGui::PushID(i);
        char nameBuf[128] = {};
        strncpy_s(nameBuf, sizeof(nameBuf), st.name.c_str(), _TRUNCATE);
        if (ImGui::InputText("名", nameBuf, sizeof(nameBuf))) {
            st.name = nameBuf;
        }
        if (component.current == static_cast<size_t>(i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[当前]");
        }
        if (ac && !ac->clips.empty()) {
            const std::string clipPreview = st.clipName.empty() ? "(选片段)" : st.clipName;
            if (ImGui::BeginCombo("片段", clipPreview.c_str())) {
                for (const auto &inst : ac->clips) {
                    if (!inst.clip) {
                        continue;
                    }
                    const bool sel = (inst.clip->name == st.clipName);
                    if (ImGui::Selectable(inst.clip->name.c_str(), sel)) {
                        st.clipName = inst.clip->name;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("无动画片段");
        }
        ImGui::Checkbox("循环", &st.loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat("速度", &st.speed, 0.05f, 0.0f, 10.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("删除")) {
            removeState = i;
        }
        ImGui::PopID();
    }
    if (removeState >= 0) {
        const size_t removed = static_cast<size_t>(removeState);
        // 维护转换引用：引用被删状态的转换移除，其后的下标左移一档（ANY 永不为下标）
        if (component.current == removed) {
            component.current = SIZE_MAX; // 运行中的状态被删 → 下帧求值回初始状态
        } else if (component.current != SIZE_MAX && component.current > removed) {
            --component.current;
        }
        component.states.erase(component.states.begin() + removeState);
        for (size_t ti = 0; ti < component.transitions.size();) {
            auto &tr = component.transitions[ti];
            if (tr.from == removed || tr.to == removed) {
                component.transitions.erase(component.transitions.begin() + ti);
                continue;
            }
            if (tr.from != SIZE_MAX && tr.from > removed) {
                --tr.from;
            }
            if (tr.to > removed) {
                --tr.to;
            }
            ++ti;
        }
    }
    if (ImGui::Button("添加状态")) {
        component.states.push_back(AnimStateDef{});
    }
    ImGui::PopID(); // states
    ImGui::Separator();

    // ---- 转换表：From(ANY)/To/过渡时长 + 点开编辑条件列表 ----
    ImGui::Text("转换");
    ImGui::PushID("trans"); // 与状态表隔离 ID 域
    int removeTrans = -1;
    static int openCondRow = -1; // 正在展开条件编辑的转换行（必须跨帧保持，否则点「条件」只闪一帧）
    for (int i = 0; i < static_cast<int>(component.transitions.size()); ++i) {
        auto &tr = component.transitions[i];
        ImGui::PushID(i);
        const char *fromPreview = (tr.from == SIZE_MAX) ? "ANY"
            : (tr.from < component.states.size() ? component.states[tr.from].name.c_str() : "(无效)");
        const char *toPreview = (tr.to < component.states.size()) ? component.states[tr.to].name.c_str() : "(无效)";
        // 第一行只放 From/To：各占可用宽一半。不按预览文本自适应宽度（长状态名会把某侧下拉
        // 撑爆整行、另一侧被裁掉）；下拉弹层仍按最长的选项自适应，超宽预览在框内截断。
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
        // 先 NewLine 把光标压到新行起点再量可用宽：上游遗留的行尾光标会让 GetContentRegionAvail
        // 测到残余宽度，导致下拉过窄、或 To 右侧顶出窗口被裁。NewLine 自带一档行距，正好分隔各转换行。
        ImGui::NewLine();
        const float rowW = ImGui::GetContentRegionAvail().x;
        // BeginCombo 的内联标签（From/To）渲染在框右侧且计入控件布局宽度（total_bb），
        // 这里必须为两个标签预留 label 宽 + ItemInnerSpacing，否则整行实际宽度超出 rowW，
        // 会把第二个下拉框（To）向右顶出面板被裁掉。
        const float labelW = ImGui::CalcTextSize("从").x + ImGui::CalcTextSize("到").x;
        const float comboW = std::max(40.0f,
            (rowW - ImGui::CalcTextSize("→").x - labelW - 2.0f * innerSpacing - 2.0f * spacing) * 0.5f);
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("从", fromPreview)) {
            if (ImGui::Selectable("ANY (全局)", tr.from == SIZE_MAX)) {
                tr.from = SIZE_MAX;
            }
            for (size_t si = 0; si < component.states.size(); ++si) {
                const bool sel = (tr.from == si);
                if (ImGui::Selectable(component.states[si].name.c_str(), sel)) {
                    tr.from = si;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text("→");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("到", toPreview)) {
            for (size_t si = 0; si < component.states.size(); ++si) {
                const bool sel = (tr.to == si);
                if (ImGui::Selectable(component.states[si].name.c_str(), sel)) {
                    tr.to = si;
                }
            }
            ImGui::EndCombo();
        }
        // 第二行：过渡时长 + 「条件」/「删除」按钮独立成行——不再与 From/To 抢一行，
        // 窄面板也不会把尾部按钮裁掉（ImGui 对超宽控件是裁剪而非换行）。
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("过渡s", &tr.blendSec, 0.01f, 0.0f, 10.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("条件")) {
            openCondRow = (openCondRow == i) ? -1 : i;
        }
        ImGui::SameLine();
        if (ImGui::Button("删除")) {
            removeTrans = i;
        }
        ImGui::TextDisabled("%d 条条件", static_cast<int>(tr.conditions.size()));

        if (i == openCondRow) {
            ImGui::Indent();
            int removeCond = -1;
            for (int ci = 0; ci < static_cast<int>(tr.conditions.size()); ++ci) {
                AnimCondition &cond = tr.conditions[ci];
                ImGui::PushID(ci);
                int typeIdx = static_cast<int>(cond.type);
                if (ImGui::Combo("类型", &typeIdx, kCondTypeItems)) {
                    cond.type = static_cast<AnimCondition::Type>(typeIdx);
                }
                ImGui::SameLine();
                if (ImGui::Button("删除")) {
                    removeCond = ci;
                }
                switch (cond.type) {
                case AnimCondition::Type::FloatCmp: {
                    char pbuf[64] = {};
                    strncpy_s(pbuf, sizeof(pbuf), cond.param.c_str(), _TRUNCATE);
                    if (ImGui::InputText("参数", pbuf, sizeof(pbuf))) {
                        cond.param = pbuf;
                    }
                    int cmpIdx = static_cast<int>(cond.cmp);
                    if (ImGui::Combo("比较", &cmpIdx, kCmpItems)) {
                        cond.cmp = static_cast<AnimCondition::Cmp>(cmpIdx);
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.0f);
                    ImGui::DragFloat("阈值", &cond.value, 0.01f, -100.0f, 100.0f, "%.2f");
                } break;
                case AnimCondition::Type::Bool: {
                    char pbuf[64] = {};
                    strncpy_s(pbuf, sizeof(pbuf), cond.param.c_str(), _TRUNCATE);
                    if (ImGui::InputText("参数", pbuf, sizeof(pbuf))) {
                        cond.param = pbuf;
                    }
                    ImGui::SameLine();
                    const char *expectPreview = cond.expect ? "真" : "假";
                    if (ImGui::BeginCombo("期望", expectPreview)) {
                        if (ImGui::Selectable("真", cond.expect)) {
                            cond.expect = true;
                        }
                        if (ImGui::Selectable("假", !cond.expect)) {
                            cond.expect = false;
                        }
                        ImGui::EndCombo();
                    }
                } break;
                case AnimCondition::Type::StateTime:
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::DragFloat("驻留(秒)", &cond.value, 0.01f, 0.0f, 100.0f, "%.2f");
                    break;
                case AnimCondition::Type::StateEnded:
                    ImGui::TextDisabled("当前动画播放到末尾后离开（需非循环）");
                    break;
                }
                ImGui::PopID();
            }
            if (removeCond >= 0) {
                tr.conditions.erase(tr.conditions.begin() + removeCond);
            }
            if (ImGui::Button("+ 条件")) {
                tr.conditions.push_back(AnimCondition{});
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (removeTrans >= 0) {
        component.transitions.erase(component.transitions.begin() + removeTrans);
    }
    if (ImGui::Button("添加转换")) {
        AnimTransitionDef t;
        t.from = component.states.empty() ? SIZE_MAX : 0;
        t.to = 0;
        component.transitions.push_back(t);
    }
    ImGui::PopID(); // trans
}

} // namespace GE
