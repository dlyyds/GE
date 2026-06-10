//
// Created by Lenovo on 2026/6/10.
//

#include "Render/ResourceManager.h"

namespace GE {

ResourceManager::~ResourceManager() {
    Shutdown();
}

void ResourceManager::Init(VmaAllocator allocator, vk::Queue queue, uint32_t queueFamilyIndex) {
    m_Textures.Init(allocator, queue, queueFamilyIndex);
}

void ResourceManager::Shutdown() {
    m_Textures.Clear();
}

} // namespace GE
