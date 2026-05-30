#pragma once
#include "inclusions.hpp" // IWYU pragma: keep

#include "backendData.hpp"
#include "render.hpp"
#include "util.hpp"

namespace CGIMBGFX {
    typedef uint16_t u16;
    
    //warning: This function checks ONLY if the RENDERER backend supports multi-viewport, DO NOT use this function to determine if multi-viewport is supported. You should also check if the PLATFORM backend supports it. According to ChatGPT, multi-viewport doesn't work on Wayland and is problematic on X11, REGARDLESS of whether you're using platform wrappers like SDL or not.
    [[nodiscard]] inline bool ImGui_Implbgfx_RendererSupportsMultiViewport() noexcept {
        const bgfx::Caps* caps = bgfx::getCaps();
        return caps != nullptr && (caps->supported & BGFX_CAPS_SWAP_CHAIN) != 0;
    }
    
    struct ViewportData {
        bgfx::FrameBufferHandle frameBuffer = BGFX_INVALID_HANDLE;
        bgfx::ViewId viewId{bgfx::ViewId(internal::U16_MAX)};
    };
    
    namespace internal {
        namespace detail {
            inline void createWindow(ImGuiViewport* viewport) noexcept {
                IM_ASSERT(viewport != nullptr);
                IM_ASSERT(viewport->RendererUserData == nullptr);
                
                const auto* data = CGIMBGFX::ImGui_Implbgfx_extras_GetBackendData();
                IM_ASSERT(data != nullptr);
                
                if (!ImGui_Implbgfx_RendererSupportsMultiViewport()) return;
                
                void* platformHandle = data->platformBridge.getNativeWindowHandle(*viewport);
                if (platformHandle == nullptr) return;
                
                ImVec2 frameBufferScale = data->platformBridge.getFrameBufferScale(*viewport);
                if (frameBufferScale.x <= 0.0f || frameBufferScale.y <= 0.0f) frameBufferScale = ImVec2(1.0f, 1.0f);
                const u16
                    frameBufferW = clampFloatToU16(viewport->Size.x * frameBufferScale.x),
                    frameBufferH = clampFloatToU16(viewport->Size.y * frameBufferScale.y);
                if (frameBufferW == 0 || frameBufferH == 0) return;
                
                ViewportData* viewportData = IM_NEW(ViewportData)();
                IM_ASSERT(viewportData != nullptr);
                
                viewportData->viewId = CGIMBGFX::internal::allocateViewId();
                if (viewportData->viewId == bgfx::ViewId(U16_MAX)) {
                    IM_DELETE(viewportData);
                    return;
                }
                
                viewportData->frameBuffer = bgfx::createFrameBuffer(
                    platformHandle,
                    frameBufferW, frameBufferH,
                    bgfx::TextureFormat::Count
                );
                if (!bgfx::isValid(viewportData->frameBuffer)) {
                    CGIMBGFX::internal::releaseViewId(viewportData->viewId);
                    IM_DELETE(viewportData);
                    return;
                }
                
                bgfx::setViewName(viewportData->viewId, "ImGui Viewport");
                bgfx::setViewMode(viewportData->viewId, bgfx::ViewMode::Sequential);
                
                viewport->RendererUserData = viewportData;
            }
            
            inline void destroyWindow(ImGuiViewport* viewport) noexcept {
                IM_ASSERT(viewport != nullptr);
                
                ViewportData* viewportData = reinterpret_cast<ViewportData*>(viewport->RendererUserData);
                if (viewportData == nullptr) return;
                
                if (viewportData->viewId != bgfx::ViewId(U16_MAX)) {
                    bgfx::setViewFrameBuffer(viewportData->viewId, BGFX_INVALID_HANDLE);
                    CGIMBGFX::internal::releaseViewId(viewportData->viewId);
                    viewportData->viewId = bgfx::ViewId(U16_MAX);
                }
                
                if (bgfx::isValid(viewportData->frameBuffer)) {
                    bgfx::destroy(viewportData->frameBuffer);
                    viewportData->frameBuffer = BGFX_INVALID_HANDLE;
                }
                
                IM_DELETE(viewportData);
                viewport->RendererUserData = nullptr;
            }
            
            inline void setWindowSize(ImGuiViewport* viewport, ImVec2 size) noexcept {
                IM_ASSERT(viewport != nullptr);
                
                ViewportData* viewportData = reinterpret_cast<ViewportData*>(viewport->RendererUserData);
                if (viewportData == nullptr) return;
                
                const auto* data = CGIMBGFX::ImGui_Implbgfx_extras_GetBackendData();
                IM_ASSERT(data != nullptr);
                
                void* platformHandle = data->platformBridge.getNativeWindowHandle(*viewport);
                if (platformHandle == nullptr) return;
                
                ImVec2 frameBufferScale = data->platformBridge.getFrameBufferScale(*viewport);
                if (frameBufferScale.x <= 0.0f || frameBufferScale.y <= 0.0f) frameBufferScale = ImVec2(1.0f, 1.0f);
                const u16
                    frameBufferW = clampFloatToU16(size.x * frameBufferScale.x),
                    frameBufferH = clampFloatToU16(size.y * frameBufferScale.y);
                
                if (bgfx::isValid(viewportData->frameBuffer)) {
                    bgfx::destroy(viewportData->frameBuffer);
                    viewportData->frameBuffer = BGFX_INVALID_HANDLE;
                }
                
                if (frameBufferW == 0 || frameBufferH == 0) {
                    bgfx::setViewFrameBuffer(viewportData->viewId, BGFX_INVALID_HANDLE);
                    return;
                }
                
                viewportData->frameBuffer = bgfx::createFrameBuffer(
                    platformHandle,
                    frameBufferW, frameBufferH,
                    bgfx::TextureFormat::Count
                );
                
                if (!bgfx::isValid(viewportData->frameBuffer)) {
                    bgfx::setViewFrameBuffer(viewportData->viewId, BGFX_INVALID_HANDLE);
                    return;
                }
            }
            
            inline void renderWindow(ImGuiViewport* viewport, void* renderArg) noexcept {
                static_cast<void>(renderArg); //Unused
                IM_ASSERT(viewport != nullptr);
                
                ViewportData* viewportData = reinterpret_cast<ViewportData*>(viewport->RendererUserData);
                if (viewportData == nullptr || !bgfx::isValid(viewportData->frameBuffer)) return;
                
                ImDrawData* drawData = viewport->DrawData;
                if (drawData == nullptr) return;
                
                bgfx::setViewFrameBuffer(viewportData->viewId, viewportData->frameBuffer);
                
                if ((viewport->Flags & ImGuiViewportFlags_NoRendererClear) == 0) {
                    bgfx::setViewClear(viewportData->viewId, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
                    bgfx::touch(viewportData->viewId);
                }
                else bgfx::setViewClear(viewportData->viewId, BGFX_CLEAR_NONE);
                
                CGIMBGFX::internal::render(drawData, viewportData->viewId);
            }
        }
        
        inline void initMultiViewport() noexcept {
            if (!CGIMBGFX::ImGui_Implbgfx_RendererSupportsMultiViewport()) return;
            ImGuiIO& io = ImGui::GetIO();
            io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
            
            ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
            platformIo.Renderer_CreateWindow = detail::createWindow;
            platformIo.Renderer_DestroyWindow = detail::destroyWindow;
            platformIo.Renderer_SetWindowSize = detail::setWindowSize;
            platformIo.Renderer_RenderWindow = detail::renderWindow;
            platformIo.Renderer_SwapBuffers = nullptr;
        }
    }
}