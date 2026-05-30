#pragma once
#include <vector>
#include <utility>
#include "inclusions.hpp" // IWYU pragma: keep

#include "util.hpp"

namespace CGIMBGFX {
    typedef uint8_t u8;
    typedef uint16_t u16;
    using std::vector, std::pair;

    namespace detail {
        [[nodiscard]] inline void* default_GetNativeWindowHandle(const ImGuiViewport& viewport) noexcept { return viewport.PlatformHandleRaw; }
        [[nodiscard]] inline ImVec2 default_GetFrameBufferScale(const ImGuiViewport& viewport) noexcept {
            if (viewport.FramebufferScale.x == 0.0f || viewport.FramebufferScale.y == 0.0f) return ImVec2(1.0f, 1.0f);
            return viewport.FramebufferScale;
        }
    }

    struct PlatformBridge {
        //The function to get native window handle from an `ImGuiViewport`.
        void* (*getNativeWindowHandle)(const ImGuiViewport& viewport) {detail::default_GetNativeWindowHandle};
        //The function to get the framebuffer scale.
        ImVec2 (*getFrameBufferScale)(const ImGuiViewport& viewport) {detail::default_GetFrameBufferScale};
    };

    struct BackendData {
        bgfx::ProgramHandle shader = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle renderToFrameBuffer = BGFX_INVALID_HANDLE;
        bgfx::ViewId startingViewId{1};
        //Uses u8 instead of bool so we don't need to pay the performance cost of a dynamic bitset.
        vector<u8> viewIdUsed;
        bgfx::VertexLayout layout;
        bool autoShaderSampler{false};
        PlatformBridge platformBridge;
    };

    [[nodiscard]] inline const BackendData* ImGui_Implbgfx_extras_GetBackendData() noexcept {
        return ImGui::GetCurrentContext() ? reinterpret_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData) : nullptr;
    }

    namespace internal {
        [[nodiscard]] inline bgfx::ViewId allocateViewId() noexcept {
            auto* data = const_cast<BackendData*>(CGIMBGFX::ImGui_Implbgfx_extras_GetBackendData());
            if (data == nullptr || data->viewIdUsed.size() == 0) return bgfx::ViewId(U16_MAX);
            for (u16 i = 0; i < data->viewIdUsed.size(); i++) if (!data->viewIdUsed[i]) {
                data->viewIdUsed[i] = true;
                return data->startingViewId + i;
            }
            return bgfx::ViewId(U16_MAX);
        }

        inline void releaseViewId(bgfx::ViewId viewId) noexcept {
            auto* data = const_cast<BackendData*>(CGIMBGFX::ImGui_Implbgfx_extras_GetBackendData());
            if (data == nullptr || data->viewIdUsed.size() == 0) return;
            const u16 index = static_cast<u16>(viewId - data->startingViewId);
            if (index >= data->viewIdUsed.size() || !data->viewIdUsed[index]) return;
            data->viewIdUsed[index] = false;
        }
    }
}