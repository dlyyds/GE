#include "pch.h"
#include "Core/GEInput.h"

#include "Core/Application.h"
#include <GLFW/glfw3.h>

namespace GE {

// 辅助函数：获取 GLFW 窗口指针（注意：GetNativeWindow 返回的是 Win32 HWND，不是 GLFWwindow*）
static GLFWwindow *GetGlfwWindow() {
    return static_cast<GLFWwindow *>(Application::Get().GetWindow().GetGlfwWindow());
}

bool Input::IsKeyPressed(KeyCode key) {
    auto *window = GetGlfwWindow();
    if (!window) {
        return false;
    }
    auto state = glfwGetKey(window, static_cast<int32_t>(key));
    return state == GLFW_PRESS || state == GLFW_REPEAT;
}

bool Input::IsMouseButtonPressed(MouseCode button) {
    auto *window = GetGlfwWindow();
    if (!window) {
        return false;
    }
    auto state = glfwGetMouseButton(window, static_cast<int32_t>(button));
    return state == GLFW_PRESS;
}

glm::vec2 Input::GetMousePosition() {
    auto *window = GetGlfwWindow();
    if (!window) {
        return {0.0f, 0.0f};
    }
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    return {static_cast<float>(xpos), static_cast<float>(ypos)};
}

float Input::GetMouseX() {
    const auto pos = GetMousePosition();
    return pos.x;
}

float Input::GetMouseY() {
    const auto pos = GetMousePosition();
    return pos.y;
}

}