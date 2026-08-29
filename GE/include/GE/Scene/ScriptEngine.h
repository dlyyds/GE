#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Core/Timestep.h"
#include "Physics/PhysicsTypes.h"
#include "Scene/Components.h"
#include "entt.hpp"

namespace GE {

class Scene;

struct Impl; ///< 持 sol::state（命名空间级前向声明，头文件不引 sol，隔离编译）

/// public 字段声明规格（脚本行为表 PUBLIC_FIELDS 解析而来，面板据此生成输入控件）。
struct ScriptFieldMeta {
    std::string Name;
    ScriptFieldType Type = ScriptFieldType::None;
    float NumberDefault = 0.0f;
    bool BoolDefault = false;
    std::string StringDefault;
};

/**
 * @brief Lua 脚本运行时管理器 —— 每个 Scene 一个，共享一个 Lua 状态（Lua 5.4 + sol2）。
 *
 * 行为表（dofile 缓存，函数共享）+ 实例表（metatable __index → 行为表，字段按实体独立），
 * 即「class + instance」。脚本顶层返回一个 table，可选字段：
 *   OnCreate(self) / OnUpdate(self, ts) / OnDestroy(self)
 * 输入不经事件分发——脚本在 OnUpdate 内经注入的 input.* 查询场景输入快照（与 C++ 脚本同源）。
 * 设计见 docs/Lua脚本系统计划书.md。
 */
class ScriptEngine {
public:
    ScriptEngine();

    ~ScriptEngine();

    ScriptEngine(const ScriptEngine &) = delete;

    ScriptEngine &operator=(const ScriptEngine &) = delete;

    /// 绑定场景、建 Lua 状态、注入 API。scriptsBaseDir 如 "assets/scripts"（自动补 "/"）。
    void Init(Scene *scene, const std::string &scriptsBaseDir);

    /// 挂载/换路径：预加载行为表并建实例、重跑 OnCreate（空路径跳过）。
    void OnComponentAdded(entt::entity entity);

    /// 每帧：对每个启用且有脚本的实体推进实例 OnUpdate。
    void OnUpdate(Timestep ts);

    /// 实体销毁/移除组件：调 OnDestroy 并清除实例。
    void OnEntityDestroyed(entt::entity entity);

    /// 动画事件派发：动画跨过事件时间点后由 Scene::UpdateAnimations 调。
    /// 实体无脚本/未启用/未定义 OnAnimationEvent → 空操作；错误走 lastError，不累计限频。
    void DispatchAnimationEvent(entt::entity entity, const std::string &eventName);

    /// 物理碰撞事件派发：StepPhysics 消费碰撞事件后对**该侧**实体调一次（参数已按侧算好）。
    /// 按 (isTrigger, phase) 映射到 OnCollision*/OnTrigger* 约定钩子；未定义钩子 → 空操作。
    void DispatchCollisionEvent(entt::entity entity, const std::string &otherTag,
                                bool isTrigger, Physics::CollisionPhase phase,
                                float impulse, float nx, float ny, float nz);

    /// 任一已加载脚本实例是否定义了该钩子（阶段 B4：Stay 高频通道全局按需开关用）。
    /// 主线程调用；OnCollisionStay/OnTriggerStay 任意一个存在即返回 true。
    bool AnyInstanceDefinesHook(const char *hookName) const;

    /// 查询实体是否已有脚本运行实例（面板状态行用）。
    bool HasInstance(entt::entity entity) const;

    /// 实体当前脚本的 public 字段声明表（从行为表 PUBLIC_FIELDS 解析；未挂载/无声明返回空表）。
    std::vector<ScriptFieldMeta> GetPublicFieldSchema(entt::entity entity) const;

    /// 该实体脚本实例最近一次运行错误信息（空 = 无/已恢复；面板状态行/自动禁用提示用）。
    std::string GetLastError(entt::entity entity) const;

    /// 热重载单脚本（清行为缓存重新 dofile；使用它的实例保留字段、只改绑元表 __index，重跑 OnCreate）。
    void Reload(const std::string &relPath);

    /// 热重载所有已加载脚本。
    void ReloadAll();

    /// 清空实例/行为缓存（场景卸载时调；sol::state 随析构关闭）。
    void Shutdown();

private:
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GE