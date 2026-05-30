#include "pch.h"
#include "Core/GEInput.h"

#include "Core/Application.h"
#include <GLFW/glfw3.h>

namespace GE {

bool Input::IsKeyPressed(KeyCode key) {
    auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
    auto state = glfwGetKey(window, static_cast<int32_t>(key));
    return state == GLFW_PRESS || state == GLFW_REPEAT;
}

bool Input::IsMouseButtonPressed(MouseCode button) {
    auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
    auto state = glfwGetMouseButton(window, static_cast<int32_t>(button));
    return state == GLFW_PRESS;
}

glm::vec2 Input::GetMousePosition() {
    const auto window = static_cast<GLFWwindow *>(Application::Get().GetWindow().GetNativeWindow());
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