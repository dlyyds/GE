#pragma once

#include "glm/glm.hpp"

namespace GE {

class Event; // forward decl — only needed for OnEvent

class Camera {
public:
    enum class Mode { Orbit = 0, FPS = 1 };

    Camera();

    ~Camera() = default;

    Camera(const Camera &) = default;

    Camera &operator=(const Camera &) = default;

    // ---- Mode ----
    void SetMode(Mode mode);

    [[nodiscard]] Mode GetMode() const { return m_Mode; }

    // ---- Matrices ----
    [[nodiscard]] glm::mat4 GetView() const;

    [[nodiscard]] glm::mat4 GetProj() const;

    [[nodiscard]] glm::mat4 GetViewProj() const { return GetProj() * GetView(); }

    // ---- Projection ----
    void SetPerspective(float fov_degrees, float aspect, float near_plane = 0.1f, float far_plane = 100.0f);

    void SetAspect(float aspect);

    [[nodiscard]] float GetFov() const { return m_Fov; }
    [[nodiscard]] float GetAspect() const { return m_Aspect; }

    // ---- FPS ----
    void SetPosition(const glm::vec3 &pos);

    void SetYawPitch(float yaw_degrees, float pitch_degrees);

    [[nodiscard]] glm::vec3 GetPosition() const;

    [[nodiscard]] float GetYaw() const { return m_Yaw; }
    [[nodiscard]] float GetPitch() const { return m_Pitch; }

    // ---- Orbit ----
    void SetTarget(const glm::vec3 &target);

    void SetOrbit(float theta_degrees, float phi_degrees, float distance);

    [[nodiscard]] glm::vec3 GetTarget() const { return m_Target; }
    [[nodiscard]] float GetTheta() const { return m_Theta; }
    [[nodiscard]] float GetPhi() const { return m_Phi; }
    [[nodiscard]] float GetDistance() const { return m_Distance; }

    // ---- Event processing (call from Layer::OnEvent) ----
    // Handles MouseMoved, MouseButtonPressed/Released, MouseScrolled internally.
    void OnEvent(Event &event);

    // ---- Legacy mouse input (used internally; public in case you want raw control) ----
    // dx, dy in pixels (e.g. io.MouseDelta)
    void OnMouseMove(float dx, float dy, bool left_down, bool right_down);
    // dy: positive = scroll up, negative = scroll down (e.g. io.MouseWheel)
    void OnScroll(float dy);

    // ---- Keyboard input for FPS mode ----
    void MoveForward(float amount);

    void MoveRight(float amount);

    void MoveUp(float amount);

    // ---- Sensitivity ----
    float MouseSensitivity = 0.3f;
    float ScrollSensitivity = 1.0f;
    float MoveSpeed = 2.0f;

    // ---- Clamp ----
    float MinPitch = -89.0f;
    float MaxPitch = 89.0f;
    float MinDistance = 0.5f;
    float MaxDistance = 50.0f;

private:
    [[nodiscard]] glm::vec3 GetForward() const;

    [[nodiscard]] glm::vec3 GetRight() const;

    Mode m_Mode = Mode::Orbit;

    // Projection
    float m_Fov = 45.0f;
    float m_Aspect = 16.0f / 9.0f;
    float m_Near = 0.1f;
    float m_Far = 100.0f;

    // FPS
    glm::vec3 m_Position{0.0f, 0.0f, 2.0f};
    float m_Yaw = 0.0f;
    float m_Pitch = 0.0f;

    // Orbit
    glm::vec3 m_Target{0.0f, 0.0f, 0.0f};
    float m_Theta = 0.0f;
    float m_Phi = 0.0f;
    float m_Distance = 2.0f;

    // Mouse state (used by OnEvent)
    float m_LastMouseX = 0.0f;
    float m_LastMouseY = 0.0f;
    bool m_LeftDown = false;
    bool m_RightDown = false;
};

} // namespace GE
