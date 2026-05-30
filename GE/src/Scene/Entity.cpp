#include "pch.h"
#include "Scene/Entity.h"

namespace GE {

Entity::Entity(entt::entity handle, Scene *scene)
    : m_EntityHandle(handle), m_Scene(scene) {
}

}