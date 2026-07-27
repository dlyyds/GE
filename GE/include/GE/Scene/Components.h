#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include "ScriptableEntity.h"


#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace GE {


struct TagComponent {
    std::string Tag;

    TagComponent() = default;

    TagComponent(const TagComponent &) = default;

    explicit TagComponent(std::string tag) : Tag(std::move(tag)) {
    }
};

struct TransformComponent {
    glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation = {0.0f, 0.0f, 0.0f};
    glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

    TransformComponent() = default;

    TransformComponent(const TransformComponent &) = default;

    explicit TransformComponent(const glm::vec3 &translation)
        : Translation(translation) {
    }

    [[nodiscard]] glm::mat4 GetTransform() const {

        const glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));

        return glm::translate(glm::mat4(1.0f), Translation)
               * rotation
               * glm::scale(glm::mat4(1.0f), Scale);
    }
};


}