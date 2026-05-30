#pragma once
#include "inclusions.hpp" // IWYU pragma: keep

#include "backendData.hpp"
#include "render.hpp"

namespace CGIMBGFX {
    inline void ImGui_Implbgfx_Shutdown() noexcept {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);
        io.BackendRendererName = nullptr;
        auto* backendData = const_cast<BackendData*>(ImGui_Implbgfx_extras_GetBackendData());
        if (backendData != nullptr && backendData->autoShaderSampler) {
            if (bgfx::isValid(backendData->shader)) bgfx::destroy(backendData->shader);
            if (bgfx::isValid(backendData->sampler)) bgfx::destroy(backendData->sampler);
        }
        IM_DELETE(reinterpret_cast<BackendData*>(io.BackendRendererUserData));
        io.BackendRendererUserData = nullptr;
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        for (ImTextureData* textureData : platformIo.Textures) if (textureData != nullptr && textureData->RefCount == 1) {
            //Shutdown bypasses the ImGui frame tick that normally increments this before WantDestroy is processed.
            textureData->UnusedFrames = 1;
            textureData->SetStatus(ImTextureStatus_WantDestroy);
            internal::updateTexture(textureData);
        }
        ImGui::DestroyPlatformWindows();
        platformIo.Renderer_CreateWindow = nullptr;
        platformIo.Renderer_DestroyWindow = nullptr;
        platformIo.Renderer_SetWindowSize = nullptr;
        platformIo.Renderer_RenderWindow = nullptr;
        platformIo.Renderer_SwapBuffers = nullptr;
    }
}