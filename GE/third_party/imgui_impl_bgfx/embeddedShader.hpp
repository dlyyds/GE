#pragma once
#include <cstdint>
#include <cstring>
#include "inclusions.hpp" // IWYU pragma: keep

#include "shader/embedded.vert_dx11.h"
#include "shader/embedded.vert_dx12.h"
#include "shader/embedded.vert_metal.h"
#include "shader/embedded.vert_opengl.h"
#include "shader/embedded.vert_opengles.h"
#include "shader/embedded.vert_vulkan.h"

#include "shader/embedded.frag_dx11.h"
#include "shader/embedded.frag_dx12.h"
#include "shader/embedded.frag_metal.h"
#include "shader/embedded.frag_opengl.h"
#include "shader/embedded.frag_opengles.h"
#include "shader/embedded.frag_vulkan.h"

namespace CGIMBGFX::internal {
    typedef uint8_t u8;
    using std::memcpy;

    [[nodiscard]] inline bool loadEmbeddedShader(bgfx::ProgramHandle& result) noexcept {
        const u8* vertData = nullptr;
        const u8* fragData = nullptr;
        size_t vertSize = 0, fragSize = 0;
        switch (bgfx::getRendererType()) {
            using enum bgfx::RendererType::Enum;
            case Direct3D11:
                vertData = embedded_vert_dx11;
                vertSize = sizeof(embedded_vert_dx11);
                fragData = embedded_frag_dx11;
                fragSize = sizeof(embedded_frag_dx11);
                break;
            case Direct3D12:
                vertData = embedded_vert_dx12;
                vertSize = sizeof(embedded_vert_dx12);
                fragData = embedded_frag_dx12;
                fragSize = sizeof(embedded_frag_dx12);
                break;
            case Metal:
                vertData = embedded_vert_metal;
                vertSize = sizeof(embedded_vert_metal);
                fragData = embedded_frag_metal;
                fragSize = sizeof(embedded_frag_metal);
                break;
            case OpenGL:
                vertData = embedded_vert_opengl;
                vertSize = sizeof(embedded_vert_opengl);
                fragData = embedded_frag_opengl;
                fragSize = sizeof(embedded_frag_opengl);
                break;
            case OpenGLES:
                vertData = embedded_vert_opengles;
                vertSize = sizeof(embedded_vert_opengles);
                fragData = embedded_frag_opengles;
                fragSize = sizeof(embedded_frag_opengles);
                break;
            case Vulkan:
                vertData = embedded_vert_vulkan;
                vertSize = sizeof(embedded_vert_vulkan);
                fragData = embedded_frag_vulkan;
                fragSize = sizeof(embedded_frag_vulkan);
                break;
            default:
                return false;
        }
        const bgfx::Memory* vertMemory = bgfx::alloc(vertSize + 1);
        vertMemory->data[vertMemory->size - 1] = '\0';
        memcpy(vertMemory->data, vertData, vertSize);
        const auto vertHandle = bgfx::createShader(vertMemory);
        if (!bgfx::isValid(vertHandle)) return false;
        const bgfx::Memory* fragMemory = bgfx::alloc(fragSize + 1);
        fragMemory->data[fragMemory->size - 1] = '\0';
        memcpy(fragMemory->data, fragData, fragSize);
        const auto fragHandle = bgfx::createShader(fragMemory);
        if (!bgfx::isValid(fragHandle)) {
            bgfx::destroy(vertHandle);
            return false;
        }
        result = bgfx::createProgram(vertHandle, fragHandle, true);
        if (!bgfx::isValid(result)) {
            bgfx::destroy(vertHandle);
            bgfx::destroy(fragHandle);
            return false;
        }
        return true;
    }
}