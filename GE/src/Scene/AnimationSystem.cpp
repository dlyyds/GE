#include "pch.h"
#include "Scene/AnimationSystem.h"

#include "Scene/Components.h"     // TransformComponent（采样写局部 TRS）
#include "Scene/ScriptEngine.h"   // FireEvents → DispatchAnimationEvent
#include "Render/AnimationClipManager.h" // ReloadClipSource → 按源文件枚举重载 + 补齐新增
#include "Render/GLTFLoader.h"    // ReloadClipSource → 重读源文件枚举 model.animations
#include "tinygltf/tiny_gltf.h"   // tinygltf::Model（前述头只前向声明）
#include "Core/Log.h"             // GE_CORE_WARN（CUBICSPLINE 近似提示）

#include <algorithm> // std::upper_bound / std::clamp
#include <cmath>     // std::fmod / std::abs
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GE {

// ============================================================================
// 仅本 TU 内部使用的动画细节（采样/ASM 求值）
// ============================================================================
namespace {

/// NearEq 比较 / StateEnded 判末使用的内容差（数值 / 秒），见计划书 §2.3
constexpr float kAnimNearEps = 0.01f;

/**
 * @brief CUBICSPLINE 通道仅一次警告：v1 按 LINEAR 近似采样，完整 Hermite 留阶段 D。
 */
void WarnCubicSplineOnce() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        GE_CORE_WARN("[Anim] CUBICSPLINE 通道按 LINEAR 近似采样（完整 Hermite 留阶段 D）");
    }
}

/// clip 名 → AnimationComponent.clips 下标（未命中返回 SIZE_MAX，PlayClip 越界忽略）。
/// clipName 是序列化稳定标识；下标随模型重排会错位，故运行期按名查（决策 9.2）。
size_t ResolveClipIndex(const AnimationComponent &ac, const std::string &clipName) {
    for (size_t i = 0; i < ac.clips.size(); ++i) {
        if (ac.clips[i].clip && ac.clips[i].clip->name == clipName) {
            return i;
        }
    }
    return SIZE_MAX;
}

/// 求值一条转换下的全部条件（AND）：全满足才 true；空条件 = 恒真。
/// trigger 采用「帧末统一消费」：Bool 条件读到的名字只标记到 consumedTriggers 而不删——
/// 同一次脉冲可同时供给本帧多条边的条件判断（声明序命中优先），由调用方在求值末尾
/// 从集合统一清除，仍是一次性语义（未被任何边命中也会消失）。
bool EvalConditions(AnimStateMachineComponent &asmc, const AnimationComponent &ac,
                    const std::vector<AnimCondition> &conditions,
                    std::vector<std::string> &consumedTriggers) {
    for (const AnimCondition &c : conditions) {
        switch (c.type) {
        case AnimCondition::Type::FloatCmp: {
            const float a = asmc.floats.count(c.param) ? asmc.floats.at(c.param) : 0.0f; // 缺省按 0
            const float b = c.value;
            bool pass = false;
            switch (c.cmp) {
            case AnimCondition::Cmp::Greater: pass = a > b;
                break;
            case AnimCondition::Cmp::GreaterEq: pass = a >= b;
                break;
            case AnimCondition::Cmp::Less: pass = a < b;
                break;
            case AnimCondition::Cmp::LessEq: pass = a <= b;
                break;
            case AnimCondition::Cmp::NearEq: pass = std::abs(a - b) <= kAnimNearEps;
                break;
            default: pass = (a != b);
                break; // Not
            }
            if (!pass) {
                return false;
            }
        }
        break;
        case AnimCondition::Type::Bool: {
            const bool fired = asmc.triggers.count(c.param) > 0; // 读，不删（帧末统一消费）
            if (fired) {
                consumedTriggers.push_back(c.param);
            }
            const bool val = fired || (asmc.bools.count(c.param) ? asmc.bools.at(c.param) : false);
            if (val != c.expect) {
                return false;
            }
        }
        break;
        case AnimCondition::Type::StateTime: if (asmc.stateTime < c.value) {
                // 驻留时间下限，防触发后立刻回跳（振铃）
                return false;
            }
            break;
        case AnimCondition::Type::StateEnded: {
            const AnimationClip *clip = ac.activeClip();
            if (!clip || ac.loop) {
                // 无有效 clip / 循环动画永不"播完"
                return false;
            }
            if (ac.time < clip->duration - kAnimNearEps) {
                return false;
            }
        }
        break;
        }
    }
    return true;
}

/// 启用时的初始状态下标：initialState 名匹配；空 / 未匹配 → 状态 0；无状态 → SIZE_MAX
size_t ResolveInitialStateIndex(const AnimStateMachineComponent &asmc) {
    if (asmc.states.empty()) {
        return SIZE_MAX;
    }
    if (!asmc.initialState.empty()) {
        for (size_t i = 0; i < asmc.states.size(); ++i) {
            if (asmc.states[i].name == asmc.initialState) {
                return i;
            }
        }
    }
    return 0;
}

/// 进入目标状态：写 speed/loop + PlayClip 过渡（复用阶段 C 双路混合，ASM 不重写采样）。
void EnterState(AnimStateMachineComponent &asmc, AnimationComponent &ac, size_t to, float blend) {
    if (to >= asmc.states.size()) {
        return;
    }
    asmc.current = to;
    asmc.stateTime = 0.0f;
    const AnimStateDef &st = asmc.states[to];
    ac.loop = st.loop;
    ac.speed = st.speed;
    ac.PlayClip(ResolveClipIndex(ac, st.clipName), blend); // 解析失败 → SIZE_MAX → PlayClip 忽略
}

} // namespace

namespace AnimationSystem {

/**
 * @brief 定位时间 t 所在的键帧区间左端 k0（带缓存）。
 *
 * k0 = 最后一个 times[i] <= t 的索引（upper_bound - 1）。
 * hint 携带上次采样的 k0：时间单调推进时从 hint 向右/向左线性走几步即达（O(1) 均摊），
 * 大跨度跳变（Scrubber / 切 clip）也能向正确方向走满收敛；hint 越界视为未初始化，
 * 回退全区间二分自愈。改造前后采样结果逐位一致（阶段 A 验收标准）。
 */
size_t FindActiveKey(const std::vector<float> &times, float t, uint32_t &hint) {
    if (hint >= times.size()) {
        hint = static_cast<uint32_t>(std::upper_bound(times.begin(), times.end(), t)
                                     - times.begin()) - 1u;
        return hint;
    }
    size_t k = hint;
    while (k + 1 < times.size() && times[k + 1] <= t) {
        ++k; // 时间前进：向右扫
    }
    while (k > 0 && times[k] > t) {
        --k; // 时间后退（倒放 / 回拖）：向左扫
    }
    hint = static_cast<uint32_t>(k);
    return k;
}

/// vec3 通道采样：LINEAR 线性插值；STEP 取前一键值；CUBICSPLINE 按 LINEAR 近似。
glm::vec3 SampleVec3Channel(const AnimationChannel &ch, float t, uint32_t &hint) {
    const size_t n = ch.times.size();
    if (n == 0) {
        return glm::vec3(0.0f);
    }
    if (t <= ch.times.front()) {
        hint = 0;
        return ch.vecKeys.front();
    }
    if (t >= ch.times.back()) {
        hint = static_cast<uint32_t>(n - 1u);
        return ch.vecKeys.back();
    }
    const size_t k0 = FindActiveKey(ch.times, t, hint);
    if (ch.interp == AnimationChannel::Interp::Step) {
        return ch.vecKeys[k0]; // STEP：保持前一键值
    }
    if (ch.interp == AnimationChannel::Interp::CubicSpline) {
        WarnCubicSplineOnce();
    }
    const float t01 = (t - ch.times[k0]) / (ch.times[k0 + 1] - ch.times[k0]);
    return glm::mix(ch.vecKeys[k0], ch.vecKeys[k0 + 1], t01);
}

/// quat 通道采样：LINEAR 用 slerp；STEP 取前一键值；CUBICSPLINE 近似 slerp。
glm::quat SampleQuatChannel(const AnimationChannel &ch, float t, uint32_t &hint) {
    const size_t n = ch.times.size();
    if (n == 0) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (t <= ch.times.front()) {
        hint = 0;
        return ch.quatKeys.front();
    }
    if (t >= ch.times.back()) {
        hint = static_cast<uint32_t>(n - 1u);
        return ch.quatKeys.back();
    }
    const size_t k0 = FindActiveKey(ch.times, t, hint);
    if (ch.interp == AnimationChannel::Interp::Step) {
        return ch.quatKeys[k0];
    }
    if (ch.interp == AnimationChannel::Interp::CubicSpline) {
        WarnCubicSplineOnce();
    }
    const float t01 = (t - ch.times[k0]) / (ch.times[k0 + 1] - ch.times[k0]);
    return glm::slerp(ch.quatKeys[k0], ch.quatKeys[k0 + 1], t01);
}

/**
 * @brief 推进 clip 播放时间（含 loop 回绕 / 非循环钳制）。
 *
 * duration <= 0 视为自由时间轴（不钳制，采样落到末键帧即可）。目标与源 clip 在
 * 过渡期共用同一回绕/钳制语义（阶段 C）。
 */
void AdvanceClipTime(float &t, float dt, float duration, bool loop) {
    t += dt;
    if (duration > 0.0f) {
        if (loop) {
            t = std::fmod(t, duration);
            if (t < 0.0f) {
                t += duration;
            }
        } else {
            t = std::clamp(t, 0.0f, duration);
        }
    }
}

/**
 * @brief 动画事件区间检测：跨过 e.time（prev < e.time <= cur）触发脚本 OnAnimationEvent。
 *
 * loop 回绕（cur < prev 说明跨过了末尾）拆两段各触发一次；负速倒放（非回绕 cur<prev）
 * 与时间未移动（prev==cur）都不触发（计划书决策 9.9 / 2.2，Scrubber 只改 time 不产生区间）。
 * 过渡期只对目标 clip 触发（源 clip 事件不触发，决策 9.8）——由调用方只传目标事件表实现。
 */
void FireEvents(ScriptEngine &engine, entt::entity entity,
                const std::vector<AnimationEvent> &evts,
                float prev, float cur, float duration, bool loop) {
    if (evts.empty() || prev == cur) {
        return;
    }
    auto fire = [&](float a, float b) {
        for (const auto &e : evts)
            if (e.time > a && e.time <= b)
                engine.DispatchAnimationEvent(entity, e.name);
    };
    if (loop && cur < prev) {
        // 回绕：跨过末尾 → 拆 (prev, duration] 与 [0, cur]
        fire(prev, duration);
        fire(0.0f, cur);
    } else if (cur > prev) {
        fire(prev, cur);
    }
}

/**
 * @brief 过渡期双路求值：目标 clip（active）与源 clip（transitionFrom）各自采样，
 * 以 (entity, path) 键合到 ac.blendBuffer 按 α 混合，统一写回局部 TRS（决策 9.3）。
 *
 * α=0 → 全源姿势；α=1 → 全目标姿势。仅源拥有的节点恒按源值驱动（不同目标集 clip
 * 的已知妥协：过渡结束后该节点保持源构型，不参与目标复位；同骨架多 clip 目标集一致，
 * 不触发该分支）。
 */
void BlendAndApplyTransition(entt::registry &registry, AnimationComponent &ac, float alpha) {
    using Path = AnimationChannel::Path;
    std::vector<BlendSlot> &buffer = ac.blendBuffer;
    buffer.clear();

    auto findSlot = [&](entt::entity e, Path p) -> size_t {
        for (size_t i = 0; i < buffer.size(); ++i)
            if (buffer[i].e == e && buffer[i].p == p)
                return i;
        return buffer.size();
    };

    // pass 1：目标 clip 全通道 → 刷目标值（新槽 w=1，缓存 hint 复用阶段 A 免二分）
    auto &targetInst = ac.clips[ac.active];
    const auto *targetClip = targetInst.clip.get();
    auto &targetHints = targetInst.keyHints;
    if (targetHints.size() != targetClip->channels.size()) {
        targetHints.assign(targetClip->channels.size(), 0u);
    }
    for (size_t ci = 0; ci < targetClip->channels.size(); ++ci) {
        const auto &ch = targetClip->channels[ci];
        const entt::entity e = (ci < targetInst.channelTargets.size())
                                   ? targetInst.channelTargets[ci]
                                   : entt::null;
        if (e == entt::null || !registry.try_get<TransformComponent>(e)) {
            continue;
        }
        size_t s = findSlot(e, ch.path);
        if (s == buffer.size()) {
            buffer.push_back({});
            s = buffer.size() - 1;
            buffer[s].e = e;
            buffer[s].p = ch.path;
            buffer[s].w = 1.0f;
        }
        switch (ch.path) {
        case Path::Translation:
        case Path::Scale: buffer[s].v = SampleVec3Channel(ch, ac.time, targetHints[ci]);
            break;
        case Path::Rotation: buffer[s].q = SampleQuatChannel(ch, ac.time, targetHints[ci]);
            break;
        }
    }

    // pass 2：源 clip 全通道 → 双驱动槽按 α 混合（(1-α)src + αtgt），仅源拥有则补源值
    auto &fromInst = ac.clips[ac.transitionFrom];
    const auto *fromClip = fromInst.clip.get();
    auto &fromHints = fromInst.keyHints;
    if (fromHints.size() != fromClip->channels.size()) {
        fromHints.assign(fromClip->channels.size(), 0u);
    }
    for (size_t ci = 0; ci < fromClip->channels.size(); ++ci) {
        const auto &ch = fromClip->channels[ci];
        const entt::entity e = (ci < fromInst.channelTargets.size())
                                   ? fromInst.channelTargets[ci]
                                   : entt::null;
        if (e == entt::null || !registry.try_get<TransformComponent>(e)) {
            continue;
        }
        const size_t s = findSlot(e, ch.path);
        switch (ch.path) {
        case Path::Translation:
        case Path::Scale: {
            const glm::vec3 src = SampleVec3Channel(ch, ac.transitionFromTime, fromHints[ci]);
            if (s != buffer.size()) {
                buffer[s].v = glm::mix(src, buffer[s].v, alpha); // 目标已写 buffer：向 α 混合源
            } else {
                buffer.push_back({e, ch.path, 1.0f, src, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
            }
            break;
        }
        case Path::Rotation: {
            const glm::quat src = SampleQuatChannel(ch, ac.transitionFromTime, fromHints[ci]);
            if (s != buffer.size()) {
                buffer[s].q = glm::slerp(src, buffer[s].q, alpha);
            } else {
                buffer.push_back({e, ch.path, 1.0f, glm::vec3(0.0f), src});
            }
            break;
        }
        }
    }

    // pass 3：统一写回实体局部 TRS（随后的 DFS 重算 world）
    for (const auto &slot : buffer) {
        auto *tc = registry.try_get<TransformComponent>(slot.e);
        if (!tc) {
            continue;
        }
        switch (slot.p) {
        case Path::Translation: tc->Translation = slot.v;
            break;
        case Path::Rotation: tc->Rotation = slot.q;
            break;
        case Path::Scale: tc->Scale = slot.v;
            break;
        }
    }
}

/**
 * @brief 每实体动画更新：ASM 求值 + 时间轴推进 + 事件触发 + 通道采样应用。
 *
 * 采样写局部 TRS，随后的 DFS 重算 world，蒙皮据此拿到最新关节矩阵（时序见计划书 §10：
 * 脚本→物理→[ASM 求值 + 动画推进]→DFS→蒙皮→渲染）。
 */
void UpdateAnimations(entt::registry &registry, ScriptEngine &scriptEngine, Timestep ts) {
    auto view = registry.view<AnimationComponent>();
    for (auto entity : view) {
        auto &ac = view.get<AnimationComponent>(entity);
        if (ac.clips.empty()) {
            continue;
        }
        const AnimationClip *clip = ac.activeClip();
        if (!clip || clip->channels.empty()) {
            continue;
        }

        // ---- 动画状态机 ASM 求值（阶段 B）：仅播放态，在推进时间轴之前 ----
        // 启用但未进入任何状态 → 先进初始状态（硬切）；随后每帧按声明序检查导出转换，
        // 第一条条件全满足的进入目标状态（PlayClip 交叉淡化由 EnterState 发起）。求值若
        // 触发了 EnterState，本帧推进与混合已按新目标走（决策 2.4）。暂停（playing=false）
        // 跳过求值、stateTime 冻结；手动切 clip 等外部覆盖由调用侧关停 ASM（决策 9.5）。
        if (auto *asmc = registry.try_get<AnimStateMachineComponent>(entity)) {
            if (asmc->enabled && ac.playing) {
                if (asmc->current == SIZE_MAX) {
                    EnterState(*asmc, ac, ResolveInitialStateIndex(*asmc), 0.0f);
                }
                if (asmc->current != SIZE_MAX) {
                    ac.loop = asmc->states[asmc->current].loop; // 状态参数持续生效（防外改漂移）
                    ac.speed = asmc->states[asmc->current].speed;
                    asmc->stateTime += ts.GetSeconds();

                    // 本帧所有 Bool 条件读到的 trigger 名字先记在这、求值末尾统一清：
                    // 同一条脉冲可同时供给本帧多条边的条件判断（声明序命中优先），
                    // 帧末清掉后仍是一次性语义（未被任何边命中也会消失）。
                    std::vector<std::string> consumedTriggers;
                    for (const AnimTransitionDef &t : asmc->transitions) {
                        if (t.from != SIZE_MAX && t.from != asmc->current) {
                            continue;
                        }
                        if (!EvalConditions(*asmc, ac, t.conditions, consumedTriggers)) {
                            continue;
                        }
                        if (t.to != asmc->current && t.to < asmc->states.size()) {
                            const size_t fromState = asmc->current;
                            EnterState(*asmc, ac, t.to, t.blendSec);
                            // 诊断：本帧实际触发了转换，打印命中的边与是否消费了 trigger 脉冲
                            std::string readNames;
                            for (const auto &rn : consumedTriggers) {
                                readNames += (readNames.empty() ? "" : ",") + rn;
                            }
                            // GE_CORE_INFO("[ASM] 状态 {} → {}（blend {:.2f}s）触发, 读取 trigger: {}",
                            //              asmc->states[fromState].name, asmc->states[t.to].name,
                            //              t.blendSec, readNames.empty() ? "-" : readNames);
                        }
                        break; // 声明序首达优先
                    }
                    for (const std::string &name : consumedTriggers) {
                        asmc->triggers.erase(name); // 帧末统一消费
                    }
                    // 诊断：帧末仍有未消费的 trigger 残留 → 说明有脉冲既未被任何边命中、
                    // 也没有随转换被消费（可能就是我们怀疑的"读到未触发却提前清除"反例，或外部注入）
                    if (!asmc->triggers.empty()) {
                        std::string pending;
                        for (const auto &tn : asmc->triggers) {
                            pending += tn + " ";
                        }
                        GE_CORE_INFO("[ASM] 帧末残留未消费 trigger: {}", pending);
                    }
                }
            }
        }

        // 过渡期自愈：源 clip 失效（越界 / 空）时放弃过渡，回退单 clip 驱动。
        // 注意：上方 ASM 求值可能已通过 EnterState 切换 active clip（决策 2.4，求值在本帧
        // 推进前改好目标），此处必须重新取当前 clip，后续推进/事件按新目标走；目标失效则跳过。
        clip = ac.activeClip();
        if (!clip || clip->channels.empty()) {
            continue;
        }
        if (ac.transitionFrom != SIZE_MAX) {
            if (ac.transitionFrom >= ac.clips.size()
                || !ac.clips[ac.transitionFrom].clip
                || ac.clips[ac.transitionFrom].clip->channels.empty()) {
                ac.transitionFrom = SIZE_MAX;
            }
        }
        const bool inTransition = ac.transitionFrom != SIZE_MAX;

        // 推进时间轴（仅播放态）：目标 clip 推进并触发其事件；过渡期源 clip 按同速续播，
        // 过渡进度按墙钟推进（决策 9.7，不随 speed 缩放）。
        if (ac.playing) {
            const float prev = ac.time; // 推进前记录，供事件区间检测
            AdvanceClipTime(ac.time, ts.GetSeconds() * ac.speed, clip->duration, ac.loop);
            const float cur = ac.time;
            const auto &evts = ac.clips[ac.active].events; // active 已由 activeClip 校验非空
            FireEvents(scriptEngine, entity, evts, prev, cur, clip->duration, ac.loop);

            if (inTransition) {
                const auto *fromClip = ac.clips[ac.transitionFrom].clip.get();
                AdvanceClipTime(ac.transitionFromTime, ts.GetSeconds() * ac.speed,
                                fromClip->duration, ac.loop);
                ac.transitionElapsed += ts.GetSeconds();
            }
        }

        // 暂停态也要应用姿态：编辑器 Scrubber 拖动时间轴（只改 time、不停播）能即刻看动作。
        // 时间未变化且已应用过则跳过重采样，避免暂停期间对静止姿态做无用功。
        if (ac.timeApplied && ac.time == ac.appliedTime) {
            continue;
        }
        ac.appliedTime = ac.time;
        ac.timeApplied = true;

        if (inTransition) {
            // 过渡期：双路求值 → buffer 混合 → 统一写回（决策 9.3）。
            const float alpha = (ac.transitionDuration > 0.0f)
                                    ? std::clamp(ac.transitionElapsed / ac.transitionDuration, 0.0f, 1.0f)
                                    : 1.0f;
            BlendAndApplyTransition(registry, ac, alpha);
            if (ac.transitionElapsed >= ac.transitionDuration) {
                ac.transitionFrom = SIZE_MAX; // 过渡结束：仅目标 clip 驱动
            }
            continue;
        }

        // 非过渡（常规单 clip 路径）：逐通道采样 → 写目标实体的局部 TRS（局部字段，
        // 随后的 DFS 重算 world；目标节点可能是皮肤关节的祖先/结构节点，经 DFS 传播子树全部关节）。
        // keyHints 与 channels 一一对应：缓存上次键帧下界，时间单调推进免二分（阶段 A）。
        const auto &channels = clip->channels;
        auto &inst = ac.clips[ac.active]; // active 已由 activeClip 校验
        auto &hints = inst.keyHints;
        if (hints.size() != channels.size()) {
            hints.assign(channels.size(), 0u); // 防御：装配路径缺失缓存时按 0 起步（走位仍收敛）
        }
        const auto &targets = inst.channelTargets;
        for (size_t ci = 0; ci < channels.size(); ++ci) {
            const auto &ch = channels[ci];
            if (ci >= targets.size() || targets[ci] == entt::null) {
                continue; // 空洞目标：该通道不驱动（该部位退化为绑定姿态）
            }
            auto *tc = registry.try_get<TransformComponent>(targets[ci]);
            if (!tc) {
                continue;
            }
            switch (ch.path) {
            case AnimationChannel::Path::Translation: tc->Translation = SampleVec3Channel(ch, ac.time, hints[ci]);
                break;
            case AnimationChannel::Path::Rotation: tc->Rotation = SampleQuatChannel(ch, ac.time, hints[ci]);
                break;
            case AnimationChannel::Path::Scale: tc->Scale = SampleVec3Channel(ch, ac.time, hints[ci]);
                break;
            }
        }
    }
}

bool ReloadClipSource(entt::registry &registry, entt::entity entity) {
    auto *ac = registry.try_get<AnimationComponent>(entity);
    if (!ac) {
        return false;
    }

    // 收集组件各 clip 的源键，并拆出去重后的独立源文件列表。
    // clips 里的旧键看不出"多了一条"——新增动画只能靠重读文件枚举 model.animations 发现。
    std::vector<std::string> keys; // 组件已有源键（判断已挂 / 新增）
    std::vector<std::string> filepaths; // 独立源文件（每个只 LoadModel 一次）
    for (const auto &inst : ac->clips) {
        if (!inst.clip || inst.clip->source.empty()) {
            continue;
        }
        keys.push_back(inst.clip->source);
        const size_t hashPos = inst.clip->source.rfind('#');
        const std::string filepath = (hashPos == std::string::npos)
                                         ? inst.clip->source
                                         : inst.clip->source.substr(0, hashPos);
        if (std::find(filepaths.begin(), filepaths.end(), filepath) == filepaths.end()) {
            filepaths.push_back(filepath);
        }
    }
    if (filepaths.empty()) {
        GE_CORE_WARN("[Anim] 无可重载的片段源（空组件 / clip 无源键）");
        return false;
    }
    const auto hasKey = [&](const std::string &k) {
        return std::find(keys.begin(), keys.end(), k) != keys.end();
    };

    bool reloaded = false;
    for (const auto &filepath : filepaths) {
        std::string loadErr;
        tinygltf::Model model;
        if (!GLTF::LoadModel(filepath, model, &loadErr)) {
            GE_CORE_WARN("[Anim] 片段源 '{}' 源文件加载失败，该文件全部 clip 保留旧数据: {}",
                         filepath, loadErr);
            continue;
        }

        // 本文件 nodeIndex → 实体映射：来自组件里同源 clip 的既有通道目标。
        // glTF nodeIndex 只在各自文件内有意义，故按文件分别建表，避免跨文件索引碰撞。
        std::unordered_map<int, entt::entity> nodeMap;
        for (const auto &inst : ac->clips) {
            if (!inst.clip) {
                continue;
            }
            const size_t hashPos = inst.clip->source.rfind('#');
            if (hashPos == std::string::npos
                || inst.clip->source.substr(0, hashPos) != filepath) {
                continue;
            }
            const auto &chs = inst.clip->channels;
            for (size_t ci = 0; ci < chs.size() && ci < inst.channelTargets.size(); ++ci) {
                const entt::entity t = inst.channelTargets[ci];
                if (t != entt::null && registry.try_get<TransformComponent>(t)) {
                    nodeMap[chs[ci].nodeIndex] = t;
                }
            }
        }

        // 以本文件 model 的动画目录为准：已挂的同步重建，新增的补齐到发起组件
        for (size_t ai = 0; ai < model.animations.size(); ++ai) {
            const std::string key = AnimationClipManager::MakeKey(filepath, ai);
            const std::shared_ptr<AnimationClip> fresh =
                AnimationClipManager::Get().Reload(filepath, ai, model); // 复用已解析 model
            if (!fresh) {
                continue; // 无合法 channel：跳过该索引
            }
            reloaded = true;

            if (hasKey(key)) {
                // 已挂 clip：重建数据 → 同步场景内全部同源实体（保持共享一致）。
                // 通道目标、事件表保留，仅换键帧数据。
                const auto view = registry.view<AnimationComponent>();
                for (auto e : view) {
                    auto &c = view.get<AnimationComponent>(e);
                    for (auto &inst : c.clips) {
                        if (inst.clip && inst.clip->source == key) {
                            inst.clip = fresh;
                            inst.keyHints.assign(inst.clip->channels.size(), 0u);
                            c.timeApplied = false; // 强制下一帧重采样（时间未变会被 appliedTime 跳过）
                        }
                    }
                }
            } else {
                // 源文件新增动画：作为新 clip 追加到发起组件（不动其它实体）。
                // 通道目标按 nodeMap 解析；本文件未出现过的节点洞掉（退化为绑定姿态）。
                ClipInstance inst;
                inst.clip = fresh;
                inst.channelTargets.reserve(fresh->channels.size());
                size_t validTargets = 0;
                for (const auto &ch : fresh->channels) {
                    const auto it = nodeMap.find(ch.nodeIndex);
                    if (it != nodeMap.end()) {
                        inst.channelTargets.push_back(it->second);
                        ++validTargets;
                    } else {
                        inst.channelTargets.push_back(entt::null);
                    }
                }
                inst.keyHints.assign(fresh->channels.size(), 0u);
                ac->clips.push_back(std::move(inst));
                // 把新映射并入 nodeMap，供本文件后续新增动画复用（同批新增可互相引用）
                const auto &ni = ac->clips.back();
                for (size_t ci = 0; ci < ni.clip->channels.size(); ++ci) {
                    if (ci < ni.channelTargets.size() && ni.channelTargets[ci] != entt::null) {
                        nodeMap[ni.clip->channels[ci].nodeIndex] = ni.channelTargets[ci];
                    }
                }
                GE_CORE_INFO("[Anim] 重载发现新增动画 '{}'（{} channel, 有效目标 {} 个）",
                             fresh->name, fresh->channels.size(), validTargets);
            }
        }
    }
    return reloaded;
}

} // namespace AnimationSystem

} // namespace GE