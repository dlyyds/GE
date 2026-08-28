#pragma once

#include <memory>
#include <string>

#include "Core/Timestep.h"
#include "entt.hpp"

namespace GE {

class Scene;

struct Impl; ///< 持 sol::state（命名空间级前向声明，头文件不引 sol，隔离编译）

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

    /// 热重载单脚本（清行为缓存，使用它的实例 OnDestroy→重建→重跑 OnCreate，实例字段保留）。
    void Reload(const std::string &relPath);

    /// 热重载所有已加载脚本。
    void ReloadAll();

    /// 清空实例/行为缓存（场景卸载时调；sol::state 随析构关闭）。
    void Shutdown();

private:
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GE