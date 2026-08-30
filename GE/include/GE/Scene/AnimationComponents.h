#pragma once

// ============================================================
// 骨骼动画 / 动画状态机组件（数据层）
// 由 Components.h 统一包含（各 TU 仍可只含本头文件以缩小依赖）。
// <骨骼蒙皮> 与 <动画播放/状态机> 两类，详见：
//   docs/骨骼动画实现计划书.md      — 蒙皮（Joint/Skin）与动画播放（ClipInstance/AnimationComponent）
//   docs/动画状态机ASM计划书.md     — AnimStateDef/AnimCondition/AnimTransitionDef/AnimStateMachineComponent
// ============================================================

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "entt.hpp"

namespace GE {

class Mesh; // 前向声明，避免引入整个 Mesh 头文件（SkinComponent 引用网格指针）


/**
 * @brief 关节标记组件 —— 挂在 glTF 关节 node 对应的实体上，标出它是骨骼关节。
 *
 * 关节实体本身是普通实体（带 TransformComponent，位于骨架层级中），由
 * SkinComponent.joints 引用；jointIndex 是该关节在 skin.joints 里的序号（0..N-1）。
 * 关节的 world 变换每帧由 Scene::UpdateWorldTransforms DFS 计算，蒙皮据此变形。
 */
struct JointComponent {
    int jointIndex = 0; ///< 该关节在 skin.joints 里的索引（0..N-1）

    JointComponent() = default;

    explicit JointComponent(int idx) : jointIndex(idx) {
    }
};

/**
 * @brief 一条 glTF skin 的定义（关节链 + 逆绑定矩阵）。
 *
 * 同一条 skin 可被多个 node 引用（如整车多块蒙皮共享一骨架，valhalla 107 个
 * node 共用 skin[0]）。各 SkinComponent 以 shared_ptr 共享同一份 SkinDef，
 * 数据只存一份；Scene::UpdateSkins 每帧按 SkinDef 去重，仅计算并上传一次
 * 关节矩阵，多 node 复用同一结果，避免逐组件重复计算/上传。
 *
 * joints 存原始 entt::entity 句柄（与 TransformComponent::parent 一致），避免
 * Components.h 里 Entity 类型不完整而无法整存 std::vector<Entity> 的问题。
 * inverseBindMatrices 是常量（绑定姿态快照），与 joints 一一对应。
 */
struct SkinDef {
    std::vector<entt::entity> joints; ///< 关节实体句柄（有序，索引 = skin.joints 序）
    std::vector<glm::mat4> inverseBindMatrices; ///< 逆绑定矩阵（与 joints 一一对应）
};

/**
 * @brief 皮肤组件 —— 挂在「带 mesh 且被骨骼驱动的 node」实体上。
 *
 * 记录驱动该网格的共享皮肤定义（SkinDef）+ 关联网格。加载期由 GLTFSceneImporter
 * 填充，运行时 Scene::UpdateSkins 每帧据此计算 jointMatrix = world × IBM 并上传 GPU。
 *
 * skin 为空（nullptr）表示未接入皮肤定义（如手动 Add Component、或导入时皮肤
 * 被跳过），绘制时按静态网格处理。joints()/inverseBindMatrices() 便捷访问器在
 * skin 为空时返回空表，保证读取安全。
 */
struct SkinComponent {
    std::shared_ptr<SkinDef> skin; ///< 共享皮肤定义（同 glTF skin 各 node 共享；null = 未接入）
    Mesh *MeshPtr = nullptr; ///< 被本皮肤驱动的网格（可选，可从绘制时取）
    bool RequiresJointUpload = true; ///< 脏标记：需重新上传关节矩阵（首版每帧重算，暂未用）

    SkinComponent() = default;

    SkinComponent(const SkinComponent &) = default;

    /// @brief 共享关节表（skin 为空时返回空表，读取安全）
    [[nodiscard]] const std::vector<entt::entity> &joints() const {
        static const std::vector<entt::entity> empty;
        return skin ? skin->joints : empty;
    }

    /// @brief 共享逆绑定矩阵（skin 为空时返回空表，读取安全）
    [[nodiscard]] const std::vector<glm::mat4> &inverseBindMatrices() const {
        static const std::vector<glm::mat4> empty;
        return skin ? skin->inverseBindMatrices : empty;
    }
};


// ============================================================
// 动画组件（数据层，详见 docs/骨骼动画实现计划书.md §4 阶段 A）
// ============================================================

/// 一条动画通道：动某个节点的某条路径（TRS）
struct AnimationChannel {
    enum class Interp : uint8_t { Linear = 0, Step = 1, CubicSpline = 2 };

    enum class Path : uint8_t { Translation = 0, Rotation = 1, Scale = 2 };

    int nodeIndex = -1; ///< glTF 目标节点索引（导入期解析成实体）
    Path path = Path::Translation;
    Interp interp = Interp::Linear;
    // 键帧（按路径类型分存，避免每帧类型转换；rotation 已转 glm::quat 内存序）
    std::vector<float> times; ///< 键帧时间（秒，单调递增）
    std::vector<glm::vec3> vecKeys; ///< translation / scale 值（每键帧一个）
    std::vector<glm::quat> quatKeys; ///< rotation 值（每键帧一个，wxyz 序）
};

/// 动画片段（模型级共享资源：与 SkinDef 同构，跨实体按 "path#N" 去重）
struct AnimationClip {
    std::string name;
    std::string source; ///< 源键 "path#N"（加载期由 AnimationClipManager 填，序列化回读用）
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

/// 动画事件：挂在动画时间轴上的"某时刻通知"（播放跨过 e.time 时触发脚本 OnAnimationEvent）
struct AnimationEvent {
    float time = 0.0f;   ///< 秒，相对 clip 起点
    std::string name;    ///< 事件名（脚本/音频等按名订阅）
};

/// 过渡混合槽：键合 (实体, 路径) 的临时姿态，clip 过渡期双路求值并入后统一写回。
/// w 为累计权重：2-clip 交叉淡化恒为 1（为将来多源叠加预留），运行时计算不用。
struct BlendSlot {
    entt::entity e = entt::null; ///< 目标实体
    AnimationChannel::Path p = AnimationChannel::Path::Translation; ///< 驱动路径
    float w = 1.0f; ///< 累计权重（2-clip 交叉淡化恒为 1）
    glm::vec3 v{}; ///< Translation / Scale 值
    glm::quat q = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); ///< Rotation 值
};

/// 绑定到场景的动画实例：clip（共享键帧）+ 本次解析的目标实体
struct ClipInstance {
    std::shared_ptr<AnimationClip> clip;
    std::vector<entt::entity> channelTargets; ///< 与 clip->channels 一一对应（未解析为 null）
    std::vector<uint32_t> keyHints; ///< 与 channels 一一对应：上次采样键帧下界（运行时缓存；不序列化）
    std::vector<AnimationEvent> events; ///< 场景级事件表（编辑/序列化；共享键帧里没有事件）
};

/// 动画组件：挂在带骨架的角色实体上（与 SkinComponent 同实体）
struct AnimationComponent {
    std::vector<ClipInstance> clips; ///< 模型全部动画（mint 1 条）
    size_t active = 0; ///< 当前播放 clip 索引
    float time = 0.0f; ///< 播放时间（秒）
    float speed = 1.0f; ///< 播放倍速
    bool playing = true; ///< 是否在播
    bool loop = true; ///< 是否循环

    // ---- clip 过渡状态（决策 9.4/9.6；不序列化）----
    // 切换瞬间 active 已是目标 clip、time 已是目标时间轴，源 clip 由下述字段续播。
    // 保存→加载后自然落在目标 clip，无残留过渡状态（故过渡不进序列化）。
    size_t transitionFrom = SIZE_MAX; ///< 源 clip 实例索引；SIZE_MAX = 无过渡
    float transitionFromTime = 0.0f; ///< 源 clip 播放时间（续播）
    float transitionElapsed = 0.0f; ///< 过渡已进行墙钟时间
    float transitionDuration = 0.0f; ///< 过渡总时长（秒；0 = 立即）
    std::vector<BlendSlot> blendBuffer; ///< 过渡临时姿态槽（成员复用容量，免每帧分配）

    // ---- 编辑器 UI 暂存（不序列化）：过渡时长输入（秒）----
    float uiBlendSec = 0.25f; ///< 面板过渡时长输入默认值（秒）

    // 运行时求值状态（不序列化）：记录上次实际应用过的采样时间。
    // 暂停时编辑器 Scrubber 只改 ac.time，据此判断是否需要重新采样应用姿态，
    // 而静止帧（无拖动）直接跳过免无用功。
    float appliedTime = 0.0f; ///< 上次实际应用过的采样时间
    bool timeApplied = false; ///< 是否至少应用过一次（首次帧强制应用，避免加载后停在绑定姿态）

    AnimationComponent() = default;

    AnimationComponent(const AnimationComponent &) = default;

    /// 便捷访问当前 clip（无动画返回 nullptr）
    [[nodiscard]] const AnimationClip *activeClip() const {
        return (active < clips.size()) ? clips[active].clip.get() : nullptr;
    }

    /// 请求切换到 clip[idx]，用 blendSeconds 秒过渡（<=0 为硬切；负速倒放/暂停 强行硬切，决策 9.9）。
    /// 切换瞬间 active 指向目标、time 归零，源 clip 以 transitionFrom/transitionFromTime 续播参与混合。
    /// 暂停（无时间轴）时列过渡会因 α 永不推进而卡在源 pose，故暂停一律硬切（编辑器暂停下切换也能立刻看到目标）。
    void PlayClip(size_t idx, float blendSeconds) {
        if (idx >= clips.size() || idx == active) {
            return; // 越界或同片段：忽略
        }
        const bool crossfade = blendSeconds > 0.0f && speed >= 0.0f && playing;
        if (crossfade) {
            transitionFrom = active; // 旧 active 成为源，从当前时间续播
            transitionFromTime = time;
            transitionElapsed = 0.0f;
            transitionDuration = blendSeconds;
        } else {
            transitionFrom = SIZE_MAX; // 硬切：清过渡状态
            transitionFromTime = 0.0f;
            transitionElapsed = 0.0f;
            transitionDuration = 0.0f;
        }
        active = idx;
        time = 0.0f;
        timeApplied = false; // 强制切换帧重新采样（时间轴归零可能与 appliedTime 撞车）
    }
};


// ============================================================
// 动画状态机 ASM（数据层，详见 docs/动画状态机ASM计划书.md）
// ============================================================

/// 单个状态：指到一条 clip + 播放参数覆盖（进入状态时应用到 ac）
struct AnimStateDef {
    std::string name;      ///< 状态名（序列化稳定标识，编辑器/转换引用）
    std::string clipName;  ///< 目标 AnimationClip::name（与 AnimationClipManager 键无关；运行时解析为 clips 下标）
    bool loop = true;      ///< 进入时写 ac.loop（false = 播一次，配合「播完」条件离开）
    float speed = 1.0f;    ///< 进入时写 ac.speed
};

/// 条件节点：各类型取其一（其余字段默认无效）
struct AnimCondition {
    enum class Type : uint8_t { FloatCmp = 0, Bool = 1, StateTime = 2, StateEnded = 3 };
    enum class Cmp : uint8_t { Greater, GreaterEq, Less, LessEq, NearEq, Not };
    Type type = Type::FloatCmp;
    std::string param;     ///< FloatCmp/Bool：参数名
    Cmp cmp = Cmp::Greater;
    float value = 0.0f;    ///< FloatCmp 阈值 / StateTime 秒 / NearEq 用同值 + 内置 eps
    bool expect = true;    ///< Bool 期望值
};

/// 转换边：from → to（from 空 = 全局 ANY，任意状态可退出）
struct AnimTransitionDef {
    size_t from = SIZE_MAX; ///< 源状态下标；SIZE_MAX = ANY（全局转换）
    size_t to = 0;          ///< 目标状态下标
    float blendSec = 0.25f; ///< 复用阶段 C 过渡时长
    std::vector<AnimCondition> conditions; ///< 全满足（AND）才触发；空 = 恒真
};

/// 运行时参数表（不序列化；初值由脚本 OnCreate 填写）
struct AnimStateMachineComponent {
    bool enabled = false;         ///< 总开关；editor/脚本可开
    std::string initialState;      ///< 启用时进入的状态名（空 = 取第一个状态）
    std::vector<AnimStateDef> states;
    std::vector<AnimTransitionDef> transitions;

    // ---- 运行时状态（不序列化）----
    size_t current = SIZE_MAX;    ///< 当前状态下标（SIZE_MAX = 未进入）
    float stateTime = 0.0f;       ///< 当前状态已驻留秒数（求值累加，EnterState 清零）

    // ---- 参数 ----
    std::unordered_map<std::string, float> floats;          ///< 连续参数（速度、方向、血量）
    std::unordered_map<std::string, bool> bools;            ///< 开关参数（on_ground、in_air）
    std::unordered_set<std::string> triggers;               ///< 一次性脉冲，求值读到即消费

    AnimStateMachineComponent() = default;

    AnimStateMachineComponent(const AnimStateMachineComponent &) = default;
};

}