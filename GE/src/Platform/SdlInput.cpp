#include "pch.h"

#include "Core/GEInput.h"

#include <SDL3/SDL.h>

namespace GE {

/**
 * @brief 基于 SDL3 的输入查询实现 —— **桌面与 Android 共用同一份**。
 *
 * 取代原先的 `Platform/Windows/WindowsInput.cpp`（GLFW 版）。与 GLFW 版的两个区别：
 *
 * 1. **不再依赖 `Application::Get().GetWindow()`**。GLFW 是 per-window 的
 *    `glfwGetKey(window, …)`，SDL 是全局键盘/鼠标状态表，所以不需要窗口句柄，
 *    顺带解掉了一处 Application 反向依赖。引擎是单窗口，语义等价。
 * 2. **键码即 scancode，无需映射**。`KeyCode` 已重编号为 `SDL_SCANCODE_*`，
 *    故 `SDL_GetKeyboardState()` 返回的表可直接用键码索引 —— 这正是换掉 GLFW
 *    所省下的那层平台映射（GLFW 版需要 `AKEYCODE_* → GLFW 数字` 的转换）。
 *
 * 注意：这里是**轮询式**查询（任意时刻调用都反映当前物理状态），与事件流是两条
 * 独立通路。帧语义的边沿检测（just_pressed 等）由 `InputState` 承担，见其注释。
 */

bool Input::IsKeyPressed(KeyCode key) {
    // 越界直接判未按下：InputState 的位集只有 512 格，且 SDL 的 400..500 是动态键码
    // 保留区，不能让越界值传播出去。
    if (static_cast<int>(key) >= SDL_SCANCODE_COUNT) {
        return false;
    }

    int numKeys = 0;
    const bool *state = SDL_GetKeyboardState(&numKeys);
    if (!state || static_cast<int>(key) >= numKeys) {
        return false;
    }
    return state[key];
}

bool Input::IsMouseButtonPressed(MouseCode button) {
    // SDL_BUTTON_MASK 的定义是 (1u << (X - 1))，X == 0 会左移 -1 → UB。
    // MouseCode::Button0 只是占位（SDL 不产生 0 号按键），必须挡在这里。
    if (button == 0 || button > Mouse::ButtonLast) {
        return false;
    }
    const SDL_MouseButtonFlags flags = SDL_GetMouseState(nullptr, nullptr);
    return (flags & SDL_BUTTON_MASK(button)) != 0;
}

glm::vec2 Input::GetMousePosition() {
    float x = 0.0f, y = 0.0f;
    // 坐标系与 GLFW 的 glfwGetCursorPos 一致：相对当前获得鼠标焦点的窗口。
    // 注意光标处于 CursorMode::Disabled（锁定）时该坐标不再有意义，此时应改用
    // InputState 的鼠标增量。
    SDL_GetMouseState(&x, &y);
    return {x, y};
}

float Input::GetMouseX() {
    return GetMousePosition().x;
}

float Input::GetMouseY() {
    return GetMousePosition().y;
}

} // namespace GE
