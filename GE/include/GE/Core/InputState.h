#pragma once

#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"

#include <bitset>
#include <cstddef>
#include <glm/glm.hpp>

namespace GE {

/// 键位容量：KeyCode 现为 SDL scancode（`SDL_SCANCODE_COUNT == 512`，合法值 0..511），
/// 故 kKeyCapacity 取 512 —— **刚好覆盖、零余量**（索引 511 有效）。SDL 的 400..500
/// 是动态键码保留区（Android 软键盘可能落在其中），故按键事件翻译层做了越界丢弃，
/// 不能让超范围的值到达这里的位集（`std::bitset::operator[]` 越界是 UB）。
/// MouseCode 0~7 取 16。
/// 平铺位集换查询 O(1)，开销可忽略；键/鼠位集独立以免混用 KeyCode/MouseCode。

/**
 * @brief 脚本输入快照 —— 输入到达脚本的唯一入口。
 *
 * 事件轮询期由 Scene::OnEvent 经 Record* 系列写入，帧首 BeginFrameInput 结算成
 * 「本帧」语义后，脚本在 OnUpdate 内经 Entity::GetScene()->GetInputState() 查询
 * （IsHeld/JustPressed/…）。输入是状态、不是事件广播，动画/物理事件另走事件总线。
 * 设计见 docs/脚本输入系统计划书.md。
 */
struct InputState {
    // == 位集容量（kKeyCapacity 须覆盖 SDL scancode 上界 RESERVED/COUNT 之下的 511）==
    static constexpr std::size_t kKeyCapacity   = 512; ///< 键位集容量
    static constexpr std::size_t kMouseCapacity = 16;  ///< 鼠标位集容量

    // == 脚本查询（本帧语义，BeginFrameInput 后有效）==
    std::bitset<kKeyCapacity>   justPressed;       ///< 本帧刚按下（repeat>0 不入）
    std::bitset<kKeyCapacity>   justReleased;      ///< 本帧刚释放
    std::bitset<kMouseCapacity> justMousePressed;  ///< 本帧鼠标刚按下
    std::bitset<kMouseCapacity> justMouseReleased; ///< 本帧鼠标刚释放
    glm::vec2        mousePos   = {0.0f, 0.0f}; ///< 本帧鼠标位置
    glm::vec2        mouseDelta = {0.0f, 0.0f}; ///< 本帧鼠标增量（相对上一帧）
    float            frameScroll = 0.0f;        ///< 本帧滚轮累计（垂直滚动量）

    // == 跨帧状态 ==
    std::bitset<kKeyCapacity>   held;        ///< 当前按住（持续到释放或 FlushAll）
    std::bitset<kMouseCapacity> mouseHeld;   ///< 鼠标按住

    // == 事件轮询期写入（Scene::OnEvent 调）==
    void RecordKeyPressed(KeyCode key, int repeatCount);
    void RecordKeyReleased(KeyCode key);
    void RecordMouseButtonPressed(MouseCode btn);
    void RecordMouseButtonReleased(MouseCode btn);
    void RecordMouseMoved(float x, float y);
    void RecordMouseScrolled(float xOffset, float yOffset);

    /// 帧首结算（OnUpdate3D 顶部、UpdateScripts 之前）：本轮事件累积的边沿升格为
    /// justPressed/justReleased，算鼠标增量并清空累积。held 跨帧保留。
    void BeginFrameInput();

    /// 焦点/视口离开：清空 held（防粘键）与全部帧态/累积，增量基准对齐。
    void FlushAll();

    /// 重置鼠标增量基准到给定位置（光标锁定/解锁后调用，防首帧 delta 爆值）。
    void ResetMouseBaseline(const glm::vec2 &pos);

    // == 脚本查询 ==
    bool IsHeld(KeyCode key) const;
    bool JustPressed(KeyCode key) const;
    bool JustReleased(KeyCode key) const;
    bool IsMouseHeld(MouseCode btn) const;
    bool JustMousePressed(MouseCode btn) const;
    bool JustMouseReleased(MouseCode btn) const;
    glm::vec2 GetMousePos() const;
    glm::vec2 GetMouseDelta() const;
    float     GetScrollDelta() const;

private:
    /// 越界保护：key/btn 非枚举非法值（如反序列化脏数据）时，bitset::set/test 会抛
    /// std::out_of_range。写入函数越界直接丢弃事件，查询函数返回 false，保证不崩溃。
    static bool IsKeyIndexValid(KeyCode key)       { return key < kKeyCapacity; }
    static bool IsMouseIndexValid(MouseCode btn)   { return btn < kMouseCapacity; }

    std::bitset<kKeyCapacity>   m_PendingPress;        ///< 本轮按键按下累积
    std::bitset<kKeyCapacity>   m_PendingRelease;      ///< 本轮按键释放累积
    std::bitset<kMouseCapacity> m_PendingMousePress;   ///< 本轮鼠标按下累积
    std::bitset<kMouseCapacity> m_PendingMouseRelease; ///< 本轮鼠标释放累积
    glm::vec2        m_PrevMousePos = {0.0f, 0.0f}; ///< 上一帧鼠标位置（增量基准）
    float            m_AccumScroll  = 0.0f;         ///< 本轮滚轮累积
};

// 实现（头文件内联，InputState 自包含、无 .cpp）

inline void InputState::RecordKeyPressed(KeyCode key, const int repeatCount) {
    if (!IsKeyIndexValid(key))
        return; // 非法 key 丢弃，防止 bitset 越界抛异常
    held.set(key);
    if (repeatCount == 0)
        m_PendingPress.set(key);
}

inline void InputState::RecordKeyReleased(const KeyCode key) {
    if (!IsKeyIndexValid(key))
        return;
    held.reset(key);
    m_PendingRelease.set(key);
}

inline void InputState::RecordMouseButtonPressed(const MouseCode btn) {
    if (!IsMouseIndexValid(btn))
        return;
    mouseHeld.set(btn);
    m_PendingMousePress.set(btn);
}

inline void InputState::RecordMouseButtonReleased(const MouseCode btn) {
    if (!IsMouseIndexValid(btn))
        return;
    mouseHeld.reset(btn);
    m_PendingMouseRelease.set(btn);
}

inline void InputState::RecordMouseMoved(const float x, const float y) {
    mousePos = {x, y};
}

inline void InputState::RecordMouseScrolled(const float xOffset, const float yOffset) {
    // 垂直滚轮用 y 分量；水平滚轮分量暂不消耗，需要时再纳入 GetScrollDelta
    (void)xOffset;
    m_AccumScroll += yOffset;
}

inline void InputState::BeginFrameInput() {
    justPressed = m_PendingPress;
    m_PendingPress.reset();
    justReleased = m_PendingRelease;
    m_PendingRelease.reset();
    justMousePressed = m_PendingMousePress;
    m_PendingMousePress.reset();
    justMouseReleased = m_PendingMouseRelease;
    m_PendingMouseRelease.reset();
    mouseDelta = mousePos - m_PrevMousePos;
    m_PrevMousePos = mousePos;
    frameScroll = m_AccumScroll;
    m_AccumScroll = 0.0f;
}

inline void InputState::FlushAll() {
    held.reset();
    mouseHeld.reset();
    justPressed.reset();
    justReleased.reset();
    justMousePressed.reset();
    justMouseReleased.reset();
    m_PendingPress.reset();
    m_PendingRelease.reset();
    m_PendingMousePress.reset();
    m_PendingMouseRelease.reset();
    m_PrevMousePos = mousePos; // 增量基准对齐，避免重获焦点后 delta 爆值
    mouseDelta = {0.0f, 0.0f};
    frameScroll = 0.0f;
    m_AccumScroll = 0.0f;
}

inline void InputState::ResetMouseBaseline(const glm::vec2 &pos) {
    m_PrevMousePos = pos;
    mousePos = pos;
    mouseDelta = {0.0f, 0.0f};
}

inline bool InputState::IsHeld(KeyCode key) const {
    return IsKeyIndexValid(key) && held.test(key);
}

inline bool InputState::JustPressed(KeyCode key) const {
    return IsKeyIndexValid(key) && justPressed.test(key);
}

inline bool InputState::JustReleased(KeyCode key) const {
    return IsKeyIndexValid(key) && justReleased.test(key);
}

inline bool InputState::IsMouseHeld(MouseCode btn) const {
    return IsMouseIndexValid(btn) && mouseHeld.test(btn);
}

inline bool InputState::JustMousePressed(MouseCode btn) const {
    return IsMouseIndexValid(btn) && justMousePressed.test(btn);
}

inline bool InputState::JustMouseReleased(MouseCode btn) const {
    return IsMouseIndexValid(btn) && justMouseReleased.test(btn);
}

inline glm::vec2 InputState::GetMousePos() const {
    return mousePos;
}

inline glm::vec2 InputState::GetMouseDelta() const {
    return mouseDelta;
}

inline float InputState::GetScrollDelta() const {
    return frameScroll;
}

} // namespace GE