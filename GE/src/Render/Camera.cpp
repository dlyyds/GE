//
// Created by Lenovo on 2026/6/5.
//

#include "Render/Camera.h"
#include "Events/MouseEvent.h"
#include "glm/gtc/matrix_transform.hpp"
#include <algorithm>

namespace GE {

Camera::Camera() {
    // Set a default perspective (aspect will be updated by the app)
    SetPerspective(m_Fov, m_Aspect);
}

void Camera::SetMode(Mode mode) {
    m_Mode = mode;
}

// ---- Matrices ----

glm::mat4 Camera::GetView() const {
    if (m_Mode == Mode::FPS) {
        return glm::lookAt(m_Position, m_Position + GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
    } else {
        float theta_rad = glm::radians(m_Theta);
        float phi_rad = glm::radians(m_Phi);
        glm::vec3 cam_pos{
            m_Distance * cos(phi_rad) * sin(theta_rad),
            m_Distance * sin(phi_rad),
            m_Distance * cos(phi_rad) * cos(theta_rad),
        };
        return glm::lookAt(m_Target + cam_pos, m_Target, glm::vec3(0.0f, 1.0f, 0.0f));
    }
}

glm::mat4 Camera::GetProj() const {
    glm::mat4 proj = glm::perspective(glm::radians(m_Fov), m_Aspect, m_Near, m_Far);
    proj[1][1] *= -1.0f; // Vulkan NDC: Y-axis points down
    return proj;
}

// ---- Projection ----

void Camera::SetPerspective(float fov_degrees, float aspect, float near_plane, float far_plane) {
    m_Fov = fov_degrees;
    m_Aspect = aspect;
    m_Near = near_plane;
    m_Far = far_plane;
}

void Camera::SetAspect(float aspect) {
    m_Aspect = aspect;
}

// ---- FPS ----

void Camera::SetPosition(const glm::vec3 &pos) {
    m_Position = pos;
}

void Camera::SetYawPitch(float yaw_degrees, float pitch_degrees) {
    m_Yaw = yaw_degrees;
    m_Pitch = std::clamp(pitch_degrees, MinPitch, MaxPitch);
}

glm::vec3 Camera::GetPosition() const {
    if (m_Mode == Mode::FPS) {
        return m_Position;
    } else {
        float theta_rad = glm::radians(m_Theta);
        float phi_rad = glm::radians(m_Phi);
        return m_Target + glm::vec3{
            m_Distance * cos(phi_rad) * sin(theta_rad),
            m_Distance * sin(phi_rad),
            m_Distance * cos(phi_rad) * cos(theta_rad),
        };
    }
}

// ---- Orbit ----

void Camera::SetTarget(const glm::vec3 &target) {
    m_Target = target;
}

void Camera::SetOrbit(float theta_degrees, float phi_degrees, float distance) {
    m_Theta = theta_degrees;
    m_Phi = std::clamp(phi_degrees, -89.0f, 89.0f);
    m_Distance = std::clamp(distance, MinDistance, MaxDistance);
}

// ---- Mouse input ----

void Camera::OnMouseMove(float dx, float dy, bool left_down, bool middle_down) {
    if (middle_down) {
        // Pan: move perpendicular to look direction
        glm::vec3 forward = GetForward();
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 up = glm::normalize(glm::cross(right, forward));
        float pan_speed = 0.005f * (m_Mode == Mode::Orbit ? m_Distance : 1.0f);

        if (m_Mode == Mode::FPS) {
            m_Position += right * (-dx * pan_speed) + up * (dy * pan_speed);
        } else {
            m_Target += right * (-dx * pan_speed) + up * (dy * pan_speed);
        }
        return;
    }

    if (!left_down) return;

    if (m_Mode == Mode::FPS) {
        m_Yaw -= dx * MouseSensitivity * 0.1f;
        m_Pitch += dy * MouseSensitivity * 0.1f;
        m_Pitch = std::clamp(m_Pitch, MinPitch, MaxPitch);
    } else {
        m_Theta -= dx * MouseSensitivity * 0.1f;
        m_Phi += dy * MouseSensitivity * 0.1f;
        m_Phi = std::clamp(m_Phi, -89.0f, 89.0f);
    }
}

void Camera::OnScroll(float dy) {
    if (m_Mode == Mode::FPS) {
        m_Position += GetForward() * (-dy * ScrollSensitivity);
    } else {
        m_Distance -= dy * ScrollSensitivity;
        m_Distance = std::clamp(m_Distance, MinDistance, MaxDistance);
    }
}

// ---- Event processing ----

void Camera::OnEvent(Event &event) {
    EventDispatcher dispatcher(event);

    dispatcher.Dispatch<MouseMovedEvent>([this](MouseMovedEvent &e) {
        OnMouseMove(e.GetX() - m_LastMouseX, e.GetY() - m_LastMouseY, m_LeftDown, m_MiddleDown);
        m_LastMouseX = e.GetX();
        m_LastMouseY = e.GetY();
        return false;
    });

    dispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent &e) {
        if (e.GetMouseButton() == Mouse::ButtonLeft)   m_LeftDown = true;
        if (e.GetMouseButton() == Mouse::ButtonMiddle) m_MiddleDown = true;
        return false;
    });

    dispatcher.Dispatch<MouseButtonReleasedEvent>([this](MouseButtonReleasedEvent &e) {
        if (e.GetMouseButton() == Mouse::ButtonLeft)   m_LeftDown = false;
        if (e.GetMouseButton() == Mouse::ButtonMiddle) m_MiddleDown = false;
        return false;
    });

    dispatcher.Dispatch<MouseScrolledEvent>([this](MouseScrolledEvent &e) {
        OnScroll(e.GetYOffset());
        return false;
    });
}

// ---- Keyboard input ----

void Camera::MoveForward(float amount) {
    m_Position += GetForward() * (amount * MoveSpeed);
}

void Camera::MoveRight(float amount) {
    m_Position += GetRight() * (amount * MoveSpeed);
}

void Camera::MoveUp(float amount) {
    m_Position += glm::vec3(0.0f, 1.0f, 0.0f) * (amount * MoveSpeed);
}

// ---- Internal helpers ----

glm::vec3 Camera::GetForward() const {
    if (m_Mode == Mode::FPS) {
        float yaw = glm::radians(m_Yaw);
        float pitch = glm::radians(m_Pitch);
        return glm::normalize(glm::vec3{
            cos(pitch) * sin(yaw),
            sin(pitch),
            cos(pitch) * cos(yaw),
        });
    } else {
        // Orbit forward = from camera toward target
        float theta = glm::radians(m_Theta);
        float phi = glm::radians(m_Phi);
        return glm::normalize(glm::vec3{
            -cos(phi) * sin(theta),
            -sin(phi),
            -cos(phi) * cos(theta),
        });
    }
}

glm::vec3 Camera::GetRight() const {
    glm::vec3 forward = GetForward();
    return glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
}

} // namespace GE
