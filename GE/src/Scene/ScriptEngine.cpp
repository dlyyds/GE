#include "pch.h"
#include "Scene/ScriptEngine.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Scene/Components.h"
#include "Scene/Entity.h"
#include "Scene/Scene.h"
#include "Core/KeyCodes.h"
#include "Core/Log.h"
#include "Core/MouseCodes.h"

#include <glm/gtc/quaternion.hpp>
#include <sol/sol.hpp>

namespace GE {

// ------------------------------------------------------------------
// Impl 定义（头文件仅前向声明，隔离 sol）
// ------------------------------------------------------------------
/// 一个脚本实例：实例表（独立状态）+ 元表（__index → 行为表）+ 运行时错误状态。
/// 热重载只改 meta 的 __index（换逻辑不换状态，实例字段保留）；错误计数供限频禁用，
/// lastError 供面板状态行展示。
struct InstanceData {
    sol::table inst;            ///< 实例表（脚本自有字段，重载保留）
    sol::table meta;            ///< 元表（__index = 行为表；重载时改绑新行为表）
    int errorStreak = 0;        ///< OnUpdate 连续出错次数（≥kErrorLimit 自动禁用）
    std::string lastError;      ///< 最近一次 hook 错误信息（空 = 正常）
};

struct Impl {
    sol::state lua;
    Scene *scene = nullptr;
    std::string baseDir;                                     ///< 以 '/' 结尾
    std::unordered_map<std::string, sol::table> behaviors;   ///< 相对路径 → 行为表（共享函数）
    std::unordered_map<entt::entity, InstanceData> instances; ///< 实体 → 实例（状态 + 错误状态）
    entt::entity activeEntity = entt::null;                  ///< 当前正在执行回调的实体
};

namespace {

// ------------------------------------------------------------------
// 键码/鼠标码 名 → 值（与 Core/KeyCodes.h 的 GLFW 布局一致，注入 Lua 的 Key/Mouse 表）
// ------------------------------------------------------------------
struct KeyName { const char *name; int code; };
constexpr KeyName kKeyNames[] = {
    {"Space", Key::Space}, {"Apostrophe", Key::Apostrophe}, {"Comma", Key::Comma},
    {"Minus", Key::Minus}, {"Period", Key::Period}, {"Slash", Key::Slash},
    {"D0", Key::D0}, {"D1", Key::D1}, {"D2", Key::D2}, {"D3", Key::D3}, {"D4", Key::D4},
    {"D5", Key::D5}, {"D6", Key::D6}, {"D7", Key::D7}, {"D8", Key::D8}, {"D9", Key::D9},
    {"Semicolon", Key::Semicolon}, {"Equal", Key::Equal},
    {"A", Key::A}, {"B", Key::B}, {"C", Key::C}, {"D", Key::D}, {"E", Key::E}, {"F", Key::F},
    {"G", Key::G}, {"H", Key::H}, {"I", Key::I}, {"J", Key::J}, {"K", Key::K}, {"L", Key::L},
    {"M", Key::M}, {"N", Key::N}, {"O", Key::O}, {"P", Key::P}, {"Q", Key::Q}, {"R", Key::R},
    {"S", Key::S}, {"T", Key::T}, {"U", Key::U}, {"V", Key::V}, {"W", Key::W}, {"X", Key::X},
    {"Y", Key::Y}, {"Z", Key::Z},
    {"LeftBracket", Key::LeftBracket}, {"Backslash", Key::Backslash},
    {"RightBracket", Key::RightBracket}, {"GraveAccent", Key::GraveAccent},
    {"World1", Key::World1}, {"World2", Key::World2},
    {"Escape", Key::Escape}, {"Enter", Key::Enter}, {"Tab", Key::Tab},
    {"Backspace", Key::Backspace}, {"Insert", Key::Insert}, {"Delete", Key::Delete},
    {"Right", Key::Right}, {"Left", Key::Left}, {"Down", Key::Down}, {"Up", Key::Up},
    {"PageUp", Key::PageUp}, {"PageDown", Key::PageDown}, {"Home", Key::Home}, {"End", Key::End},
    {"CapsLock", Key::CapsLock}, {"ScrollLock", Key::ScrollLock}, {"NumLock", Key::NumLock},
    {"PrintScreen", Key::PrintScreen}, {"Pause", Key::Pause},
    {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4}, {"F5", Key::F5},
    {"F6", Key::F6}, {"F7", Key::F7}, {"F8", Key::F8}, {"F9", Key::F9}, {"F10", Key::F10},
    {"F11", Key::F11}, {"F12", Key::F12}, {"F13", Key::F13}, {"F14", Key::F14},
    {"F15", Key::F15}, {"F16", Key::F16}, {"F17", Key::F17}, {"F18", Key::F18},
    {"F19", Key::F19}, {"F20", Key::F20}, {"F21", Key::F21}, {"F22", Key::F22},
    {"F23", Key::F23}, {"F24", Key::F24}, {"F25", Key::F25},
    {"KP0", Key::KP0}, {"KP1", Key::KP1}, {"KP2", Key::KP2}, {"KP3", Key::KP3},
    {"KP4", Key::KP4}, {"KP5", Key::KP5}, {"KP6", Key::KP6}, {"KP7", Key::KP7},
    {"KP8", Key::KP8}, {"KP9", Key::KP9}, {"KPDecimal", Key::KPDecimal},
    {"KPDivide", Key::KPDivide}, {"KPMultiply", Key::KPMultiply}, {"KPSubtract", Key::KPSubtract},
    {"KPAdd", Key::KPAdd}, {"KPEnter", Key::KPEnter}, {"KPEqual", Key::KPEqual},
    {"LeftShift", Key::LeftShift}, {"LeftControl", Key::LeftControl}, {"LeftAlt", Key::LeftAlt},
    {"LeftSuper", Key::LeftSuper}, {"RightShift", Key::RightShift}, {"RightControl", Key::RightControl},
    {"RightAlt", Key::RightAlt}, {"RightSuper", Key::RightSuper}, {"Menu", Key::Menu},
};

struct MouseName { const char *name; int code; };
constexpr MouseName kMouseNames[] = {
    {"Button0", Mouse::Button0}, {"Button1", Mouse::Button1}, {"Button2", Mouse::Button2},
    {"Button3", Mouse::Button3}, {"Button4", Mouse::Button4}, {"Button5", Mouse::Button5},
    {"Button6", Mouse::Button6}, {"Button7", Mouse::Button7},
    {"ButtonLeft", Mouse::ButtonLeft}, {"ButtonRight", Mouse::ButtonRight},
    {"ButtonMiddle", Mouse::ButtonMiddle},
};

/// 连续 OnUpdate 报错达到该次数 → 自动置 Enabled=false（防刷屏，见计划书 2.6/阶段 C）。
constexpr int kErrorLimit = 3;

// 读/写当前活动实体的 Transform（无则返回 nullptr）
TransformComponent *ActiveTransform(Impl &eng) {
    if (!eng.scene || eng.activeEntity == entt::null)
        return nullptr;
    return eng.scene->Reg().try_get<TransformComponent>(eng.activeEntity);
}

std::string ReadFileContents(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 注入脚本 API：log / input / transform / entity / public / Key / Mouse
void RegisterApi(Impl &eng) {
    sol::state &lua = eng.lua;

    // ---- log → spdlog ----
    sol::table logT = lua.create_table();
    logT["info"] = [](const std::string &m) { GE_CORE_INFO("[Lua] {}", m); };
    logT["warn"] = [](const std::string &m) { GE_CORE_WARN("[Lua] {}", m); };
    logT["error"] = [](const std::string &m) { GE_CORE_ERROR("[Lua] {}", m); };
    lua["log"] = logT;

    // ---- input → 场景输入快照（与 C++ 脚本同帧同源，见脚本输入系统计划书）----
    auto keyState = [&eng](bool (InputState::*fn)(KeyCode) const) {
        return [&eng, fn](int key) -> bool {
            if (!eng.scene || key < 0 || key >= 512)
                return false;
            return (eng.scene->GetInputState().*fn)(static_cast<KeyCode>(key));
        };
    };
    auto mouseState = [&eng](bool (InputState::*fn)(MouseCode) const) {
        return [&eng, fn](int btn) -> bool {
            if (!eng.scene || btn < 0 || btn >= 16)
                return false;
            return (eng.scene->GetInputState().*fn)(static_cast<MouseCode>(btn));
        };
    };

    sol::table inT = lua.create_table();
    inT["is_held"] = keyState(&InputState::IsHeld);
    inT["just_pressed"] = keyState(&InputState::JustPressed);
    inT["just_released"] = keyState(&InputState::JustReleased);
    inT["is_mouse_down"] = mouseState(&InputState::IsMouseHeld);
    inT["just_mouse_pressed"] = mouseState(&InputState::JustMousePressed);
    inT["just_mouse_released"] = mouseState(&InputState::JustMouseReleased);
    inT["mouse_pos"] = [&eng]() {
        const glm::vec2 p = eng.scene ? eng.scene->GetInputState().GetMousePos() : glm::vec2{0.0f};
        return std::make_tuple(p.x, p.y);
    };
    inT["mouse_delta"] = [&eng]() {
        const glm::vec2 d = eng.scene ? eng.scene->GetInputState().GetMouseDelta() : glm::vec2{0.0f};
        return std::make_tuple(d.x, d.y);
    };
    inT["scroll"] = [&eng]() -> float {
        return eng.scene ? eng.scene->GetInputState().GetScrollDelta() : 0.0f;
    };
    lua["input"] = inT;

    // ---- transform → 局部 TRS（作用于挂载该脚本的实体）----
    sol::table trT = lua.create_table();
    trT["get_translation"] = [&eng]() {
        if (auto *t = ActiveTransform(eng))
            return std::make_tuple(t->Translation.x, t->Translation.y, t->Translation.z);
        return std::make_tuple(0.0f, 0.0f, 0.0f);
    };
    trT["set_translation"] = [&eng](float x, float y, float z) {
        if (auto *t = ActiveTransform(eng)) {
            t->Translation = {x, y, z};
            return true;
        }
        GE_CORE_WARN("[Lua] transform.set_translation: 无 Transform 组件");
        return false;
    };
    trT["get_rotation"] = [&eng]() { // 欧拉角，度数（与编辑器一致）
        if (auto *t = ActiveTransform(eng)) {
            const glm::vec3 e = glm::degrees(glm::eulerAngles(t->Rotation));
            return std::make_tuple(e.x, e.y, e.z);
        }
        return std::make_tuple(0.0f, 0.0f, 0.0f);
    };
    trT["set_rotation"] = [&eng](float degX, float degY, float degZ) {
        if (auto *t = ActiveTransform(eng)) {
            t->Rotation = glm::quat(glm::radians(glm::vec3(degX, degY, degZ)));
            return true;
        }
        GE_CORE_WARN("[Lua] transform.set_rotation: 无 Transform 组件");
        return false;
    };
    trT["rotate_local"] = [&eng](float ax, float ay, float az, float deg) {
        // 绕该实体【当前局部】 (ax,ay,az) 轴旋转 deg 度：四元数右乘增量，
        // 全程不碰欧拉回读（解不唯一 + 万向锁不稳），增量旋转请用它而非 get/set_rotation。
        if (auto *t = ActiveTransform(eng)) {
            const glm::vec3 axis(ax, ay, az);
            const float len = glm::length(axis);
            if (len < 1e-5f) {
                GE_CORE_WARN("[Lua] transform.rotate_local: 轴为零向量");
                return false;
            }
            t->Rotation = glm::normalize(
                t->Rotation * glm::angleAxis(glm::radians(deg), axis / len));
            return true;
        }
        GE_CORE_WARN("[Lua] transform.rotate_local: 无 Transform 组件");
        return false;
    };
    trT["get_scale"] = [&eng]() {
        if (auto *t = ActiveTransform(eng))
            return std::make_tuple(t->Scale.x, t->Scale.y, t->Scale.z);
        return std::make_tuple(1.0f, 1.0f, 1.0f);
    };
    trT["set_scale"] = [&eng](float x, float y, float z) {
        if (auto *t = ActiveTransform(eng)) {
            t->Scale = {x, y, z};
            return true;
        }
        GE_CORE_WARN("[Lua] transform.set_scale: 无 Transform 组件");
        return false;
    };
    lua["transform"] = trT;

    // ---- entity → 挂载实体的基本查询 ----
    auto hasAny = [&eng](const char *name) -> bool {
        if (!eng.scene || eng.activeEntity == entt::null)
            return false;
        auto &reg = eng.scene->Reg();
        const entt::entity e = eng.activeEntity;
        if (name == std::string_view("Transform")) return reg.any_of<TransformComponent>(e);
        if (name == std::string_view("MeshRenderer")) return reg.any_of<MeshRendererComponent>(e);
        if (name == std::string_view("Camera")) return reg.any_of<CameraComponent>(e);
        if (name == std::string_view("RigidBody")) return reg.any_of<RigidBodyComponent>(e);
        if (name == std::string_view("Script")) return reg.any_of<ScriptComponent>(e);
        if (name == std::string_view("Tag")) return reg.any_of<TagComponent>(e);
        if (name == std::string_view("Animation")) return reg.any_of<AnimationComponent>(e);
        if (name == std::string_view("SpriteRenderer")) return reg.any_of<SpriteRendererComponent>(e);
        if (name == std::string_view("PointLight")) return reg.any_of<PointLightComponent>(e);
        if (name == std::string_view("DirectionalLight")) return reg.any_of<DirectionalLightComponent>(e);
        if (name == std::string_view("AmbientLight")) return reg.any_of<AmbientLightComponent>(e);
        if (name == std::string_view("Environment")) return reg.any_of<EnvironmentComponent>(e);
        if (name == std::string_view("BoxCollider")) return reg.any_of<BoxColliderComponent>(e);
        if (name == std::string_view("SphereCollider")) return reg.any_of<SphereColliderComponent>(e);
        if (name == std::string_view("Joint")) return reg.any_of<JointComponent>(e);
        if (name == std::string_view("Skin")) return reg.any_of<SkinComponent>(e);
        if (name == std::string_view("BoundingBox")) return reg.any_of<BoundingBoxComponent>(e);
        return false;
    };

    sol::table entT = lua.create_table();
    entT["get_tag"] = [&eng]() -> std::string {
        if (!eng.scene || eng.activeEntity == entt::null)
            return "";
        if (auto *t = eng.scene->Reg().try_get<TagComponent>(eng.activeEntity))
            return t->Tag;
        return "";
    };
    entT["has_component"] = [hasAny](const std::string &name) -> bool { return hasAny(name.c_str()); };
    lua["entity"] = entT;

    // ---- public → 本实体 public 字段（脚本 PUBLIC_FIELDS 声明的可调配置）。
    // 每次 get 实时读 ScriptComponent.PublicFields：面板改值/场景加载即生效，无需同步运行中实例。
    sol::table pubT = lua.create_table();
    pubT["get"] = [&eng](const std::string &name) -> sol::object {
        if (!eng.scene || eng.activeEntity == entt::null)
            return sol::lua_nil;
        auto *sc = eng.scene->Reg().try_get<ScriptComponent>(eng.activeEntity);
        if (!sc)
            return sol::lua_nil;
        auto it = sc->PublicFields.find(name);
        if (it == sc->PublicFields.end()) {
            GE_CORE_WARN("[Lua] public 字段不存在: {}", name);
            return sol::lua_nil;
        }
        switch (it->second.Type) {
            case ScriptFieldType::Number: return sol::make_object(eng.lua, it->second.Number);
            case ScriptFieldType::Bool:   return sol::make_object(eng.lua, it->second.Bool);
            case ScriptFieldType::String: return sol::make_object(eng.lua, it->second.String);
            default: return sol::lua_nil;
        }
    };
    lua["public"] = pubT;

    // ---- Key / Mouse 键码表 ----
    sol::table keyT = lua.create_table();
    for (const auto &k : kKeyNames)
        keyT[k.name] = k.code;
    lua["Key"] = keyT;

    sol::table mouseT = lua.create_table();
    for (const auto &m : kMouseNames)
        mouseT[m.name] = m.code;
    lua["Mouse"] = mouseT;
}

// 加载并缓存行为表；失败返回 false（调用方决定告警/禁用）
bool EnsureBehavior(Impl &eng, const std::string &relPath) {
    if (eng.behaviors.count(relPath))
        return true;

    const std::string fullPath = eng.baseDir + relPath;
    std::string code = ReadFileContents(fullPath);
    if (code.empty()) {
        GE_CORE_WARN("[Lua] 脚本文件不存在或为空: {}", fullPath);
        return false;
    }

    sol::protected_function_result result = eng.lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        GE_CORE_ERROR("[Lua] 加载脚本失败 {}: {}", fullPath, err.what());
        return false;
    }
    sol::object obj = result;
    if (obj.get_type() != sol::type::table) {
        GE_CORE_ERROR("[Lua] 脚本 {} 顶层应返回 table（行为表）", fullPath);
        return false;
    }
    eng.behaviors[relPath] = obj.as<sol::table>();
    return true;
}

// 建实例：空实例表 + 独立元表（__index → 行为表）。字段赋值落实例、函数走共享行为表。
// meta 单独持有：热重载时改它单个对象的 __index 即整体换绑，实例表不动。
InstanceData MakeInstance(Impl &eng, const std::string &relPath) {
    sol::table behavior = eng.behaviors.at(relPath);
    InstanceData id;
    id.inst = eng.lua.create_table();
    id.meta = eng.lua.create_table();
    id.meta[sol::meta_function::index] = behavior;
    id.inst[sol::metatable_key] = id.meta;
    return id;
}

// 解析行为表顶部的 PUBLIC_FIELDS 声明 → 面板控件规格（非 table/空则空表）
std::vector<ScriptFieldMeta> ParsePublicFields(const sol::table &behavior) {
    std::vector<ScriptFieldMeta> out;
    sol::object pfObj = behavior["PUBLIC_FIELDS"];
    if (pfObj.get_type() != sol::type::table)
        return out;
    sol::table fields = pfObj.as<sol::table>();
    for (auto [key, value] : fields) { // 拷贝绑定，避免 as<T>() 需要非 const 的隐患
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table)
            continue;
        ScriptFieldMeta m;
        m.Name = key.as<std::string>();
        sol::table def = value.as<sol::table>();
        const std::string typeStr = def["type"].get_or<std::string>("number");
        sol::object defObj = def["default"];
        if (typeStr == "bool" || typeStr == "boolean") {
            m.Type = ScriptFieldType::Bool;
            m.BoolDefault = (defObj.get_type() == sol::type::boolean) ? defObj.as<bool>() : false;
        } else if (typeStr == "string") {
            m.Type = ScriptFieldType::String;
            m.StringDefault =
                (defObj.get_type() == sol::type::string) ? defObj.as<std::string>() : std::string{};
        } else {
            m.Type = ScriptFieldType::Number;
            m.NumberDefault = (defObj.get_type() == sol::type::number) ? defObj.as<float>() : 0.0f;
        }
        out.push_back(std::move(m));
    }
    return out;
}

// 用行为表 schema 补全实体 public 字段：缺失按 default 填入，已有值保留；脚本类型变了重置为默认
void EnsurePublicFields(Impl &eng, entt::entity entity, const sol::table &behavior) {
    auto *sc = eng.scene->Reg().try_get<ScriptComponent>(entity);
    if (!sc)
        return;
    for (const auto &m : ParsePublicFields(behavior)) {
        auto it = sc->PublicFields.find(m.Name);
        if (it != sc->PublicFields.end()) {
            if (it->second.Type == m.Type)
                continue;
            sc->PublicFields.erase(it); // 类型随脚本更新 → 按新类型重置默认
        }
        sc->PublicFields.emplace(m.Name,
            ScriptPublicField{m.Type, m.NumberDefault, m.BoolDefault, m.StringDefault});
    }
}

// 调用实例函数(self, args...)。功能未定义 → false；运行出错 → 记录 lastError + 日志 + false（隔离，不拖垮引擎）。
bool CallHook(InstanceData &id, const char *name) {
    sol::object fn = id.inst[name];
    if (fn.get_type() != sol::type::function)
        return false; // 未定义该函数
    sol::protected_function pf = fn.as<sol::protected_function>();
    try {
        sol::protected_function_result res = pf(id.inst);
        if (!res.valid()) {
            sol::error err = res;
            id.lastError = err.what();
            GE_CORE_ERROR("[Lua] 函数 {} 出错: {}", name, id.lastError);
            return false;
        }
        return true;
    } catch (const sol::error &e) {
        id.lastError = e.what();
        GE_CORE_ERROR("[Lua] 函数 {} 异常: {}", name, id.lastError);
        return false;
    }
}

template <typename T>
bool CallHook(InstanceData &id, const char *name, T &&arg) {
    sol::object fn = id.inst[name];
    if (fn.get_type() != sol::type::function)
        return false;
    sol::protected_function pf = fn.as<sol::protected_function>();
    try {
        sol::protected_function_result res = pf(id.inst, std::forward<T>(arg));
        if (!res.valid()) {
            sol::error err = res;
            id.lastError = err.what();
            GE_CORE_ERROR("[Lua] 函数 {} 出错: {}", name, id.lastError);
            return false;
        }
        return true;
    } catch (const sol::error &e) {
        id.lastError = e.what();
        GE_CORE_ERROR("[Lua] 函数 {} 异常: {}", name, id.lastError);
        return false;
    }
}

} // namespace


// ------------------------------------------------------------------
// ScriptEngine 公共接口
// ------------------------------------------------------------------
ScriptEngine::ScriptEngine() = default;

ScriptEngine::~ScriptEngine() = default;

void ScriptEngine::Init(Scene *scene, const std::string &scriptsBaseDir) {
    Shutdown();

    m_Impl = std::make_unique<Impl>();
    Impl &eng = *m_Impl;
    eng.scene = scene;
    eng.baseDir = scriptsBaseDir;
    if (eng.baseDir.empty() || eng.baseDir.back() != '/')
        eng.baseDir += '/';

    eng.lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::string,
                           sol::lib::table, sol::lib::math);
    const std::string defaultPath = eng.lua["package"]["path"].get_or<std::string>("./?.lua");
    eng.lua["package"]["path"] = eng.baseDir + "?.lua;" + defaultPath;

    RegisterApi(eng);
}

void ScriptEngine::Shutdown() {
    if (!m_Impl)
        return;
    // 不调 OnDestroy：场景卸载路径由 Scene 解挂 on_destroy 兜底，避免碰卸了一半的场景
    m_Impl->instances.clear();
    m_Impl->behaviors.clear();
    m_Impl->lua.collect_garbage();
}

void ScriptEngine::OnComponentAdded(entt::entity entity) {
    if (!m_Impl || !m_Impl->scene)
        return;
    Impl &eng = *m_Impl;
    auto *sc = eng.scene->Reg().try_get<ScriptComponent>(entity);
    if (!sc || sc->ScriptPath.empty())
        return;

    // 换路径/重挂：先清旧实例
    OnEntityDestroyed(entity);

    if (EnsureBehavior(eng, sc->ScriptPath)) {
        eng.instances[entity] = MakeInstance(eng, sc->ScriptPath);
        EnsurePublicFields(eng, entity, eng.behaviors.at(sc->ScriptPath)); // 先补 public 默认，OnCreate 才能读
        eng.activeEntity = entity;
        CallHook(eng.instances[entity], "OnCreate");
        eng.activeEntity = entt::null;
    } else {
        GE_CORE_WARN("[Lua] 脚本加载失败，组件禁用: {}", sc->ScriptPath);
        sc->Enabled = false;
    }
}

void ScriptEngine::OnUpdate(Timestep ts) {
    if (!m_Impl || !m_Impl->scene)
        return;
    Impl &eng = *m_Impl;
    const auto view = eng.scene->Reg().view<ScriptComponent>();
    for (entt::entity e : view) {
        auto &sc = view.get<ScriptComponent>(e);
        if (!sc.Enabled || sc.ScriptPath.empty())
            continue;

        auto it = eng.instances.find(e);
        if (it == eng.instances.end()) {
            // 兜底：路径在面板被改等未走 OnComponentAdded 的场景
            OnComponentAdded(e);
            it = eng.instances.find(e);
            if (it == eng.instances.end())
                continue;
        }
        eng.activeEntity = e;
        const bool ok = CallHook(it->second, "OnUpdate", ts.GetSeconds());
        eng.activeEntity = entt::null;
        if (ok) {
            it->second.errorStreak = 0;
            it->second.lastError.clear(); // 恢复后清最近错误，面板回绿
        } else if (++it->second.errorStreak >= kErrorLimit) {
            // 连续报错 → 自动禁用（防刷屏）。计数清零等下次重挂/重新勾选后才重新累计。
            sc.Enabled = false;
            it->second.errorStreak = 0;
            GE_CORE_ERROR("[Lua] 脚本连续报错 {} 次，已自动禁用: {}", kErrorLimit, sc.ScriptPath);
        }
    }
}

void ScriptEngine::OnEntityDestroyed(entt::entity entity) {
    if (!m_Impl)
        return;
    auto &eng = *m_Impl;
    auto it = eng.instances.find(entity);
    if (it == eng.instances.end())
        return;
    eng.activeEntity = entity;
    CallHook(it->second, "OnDestroy");
    eng.activeEntity = entt::null;
    eng.instances.erase(it);
}

void ScriptEngine::DispatchAnimationEvent(entt::entity entity, const std::string &eventName) {
    if (!m_Impl || !m_Impl->scene)
        return;
    Impl &eng = *m_Impl;
    if (entity == entt::null)
        return;
    // 无脚本/未启用 → 静默丢弃（计划书 2.4）
    auto *sc = eng.scene->Reg().try_get<ScriptComponent>(entity);
    if (!sc || !sc->Enabled || sc->ScriptPath.empty())
        return;
    auto it = eng.instances.find(entity);
    if (it == eng.instances.end())
        return; // 实例未装配（路径面板直改等）→ 不补挂，边沿通知一次性丢弃
    eng.activeEntity = entity; // 钩子内经 transform/entity API 作用于此实体的实例
    CallHook(it->second, "OnAnimationEvent", eventName);
    eng.activeEntity = entt::null;
    // 事件钩子出错只记 lastError（CallHook 已做），不累计限频——限频仅针对 OnUpdate 运行循环（计划书 7.4）
}

bool ScriptEngine::HasInstance(entt::entity entity) const {
    return m_Impl && m_Impl->instances.count(entity) > 0;
}

std::string ScriptEngine::GetLastError(entt::entity entity) const {
    if (!m_Impl)
        return {};
    const auto it = m_Impl->instances.find(entity);
    return (it != m_Impl->instances.end()) ? it->second.lastError : std::string{};
}

std::vector<ScriptFieldMeta> ScriptEngine::GetPublicFieldSchema(entt::entity entity) const {
    if (!m_Impl || !m_Impl->scene)
        return {};
    const auto *sc = m_Impl->scene->Reg().try_get<ScriptComponent>(entity);
    if (!sc || sc->ScriptPath.empty())
        return {};
    const auto bIt = m_Impl->behaviors.find(sc->ScriptPath);
    if (bIt == m_Impl->behaviors.end())
        return {}; // 未加载（加载失败）→ 无 schema
    return ParsePublicFields(bIt->second);
}

void ScriptEngine::Reload(const std::string &relPath) {
    if (!m_Impl || !m_Impl->scene)
        return;
    Impl &eng = *m_Impl;

    // 热重载 = 换逻辑不换状态：清行为缓存重新 dofile；实例表保留，只改绑元表 __index。
    const auto oldIt = eng.behaviors.find(relPath);
    const sol::table oldBehavior = (oldIt != eng.behaviors.end()) ? oldIt->second : sol::table{};
    eng.behaviors.erase(relPath);
    if (!EnsureBehavior(eng, relPath)) {
        if (oldBehavior.valid())
            eng.behaviors[relPath] = oldBehavior; // 重载失败：回退旧逻辑，运行中实例不受影响
        GE_CORE_WARN("[Lua] 重载失败，保留旧逻辑: {}", relPath);
        return;
    }
    const sol::table newBehavior = eng.behaviors.at(relPath);

    for (entt::entity e : eng.scene->Reg().view<ScriptComponent>()) {
        if (eng.scene->Reg().get<ScriptComponent>(e).ScriptPath != relPath)
            continue;
        auto it = eng.instances.find(e);
        if (it == eng.instances.end())
            continue; // 该实体未挂成运行实例，无需动作
        // 顺序：先 OnDestroy（旧行为）→ 改绑 __index 至新行为表 → 补 public 字段 → 重跑 OnCreate（新行为）
        eng.activeEntity = e;
        CallHook(it->second, "OnDestroy");
        it->second.meta[sol::meta_function::index] = newBehavior;
        EnsurePublicFields(eng, e, newBehavior);
        CallHook(it->second, "OnCreate");
        eng.activeEntity = entt::null;
    }
}

void ScriptEngine::ReloadAll() {
    if (!m_Impl || !m_Impl->scene)
        return;
    std::vector<std::string> paths;
    for (entt::entity e : m_Impl->scene->Reg().view<ScriptComponent>()) {
        const std::string &p = m_Impl->scene->Reg().get<ScriptComponent>(e).ScriptPath;
        if (!p.empty())
            paths.push_back(p);
    }
    for (const auto &p : paths)
        Reload(p);
}

} // namespace GE