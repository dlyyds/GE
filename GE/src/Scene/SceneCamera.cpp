//
// Created by Lenovo on 2026/5/9.
//
#include "pch.h"
#include "Scene/SceneCamera.h"

#include "Debug/Assert.h"
#include <glm/gtc/matrix_transform.hpp>

namespace GE {

SceneCamera::SceneCamera() {
    RecalculateProjection();
}

void SceneCamera::SetOrthographic(const float size, const float nearClip, const float farClip) {
    m_ProjectionType = ProjectionType::Orthographic;
    m_OrthographicSize = size;
    m_OrthographicNear = nearClip;
    m_OrthographicFar = farClip;
    RecalculateProjection();
}

void SceneCamera::SetPerspective(float verticalFOV, float nearClip, float farClip) {
    m_ProjectionType = ProjectionType::Perspective;
    m_PerspectiveFOV = verticalFOV;
    m_PerspectiveNear = nearClip;
    m_PerspectiveFar = farClip;
    RecalculateProjection();
}

void SceneCamera::SetViewportSize(const uint32_t width, const uint32_t height) {
    GE_CORE_ASSERT(width > 0 && height > 0);
    m_AspectRatio = static_cast<float>(width) / static_cast<float>(height);
    RecalculateProjection();
}

void SceneCamera::RecalculateProjection() {
    if (m_ProjectionType == ProjectionType::Perspective) {
        m_Projection = glm::perspective(m_PerspectiveFOV, m_AspectRatio, m_PerspectiveNear, m_PerspectiveFar);
    } else {
        float orthoLeft = -m_OrthographicSize * m_AspectRatio * 0.5f;
        float orthoRight = m_OrthographicSize * m_AspectRatio * 0.5f;
        float orthoBottom = -m_OrthographicSize * 0.5f;
        float orthoTop = m_OrthographicSize * 0.5f;

        m_Projection = glm::ortho(orthoLeft, orthoRight,
                                  orthoBottom, orthoTop, m_OrthographicNear, m_OrthographicFar);
    }
}

}