#pragma once
#include <array>
#include <cstring>
#include "inclusions.hpp" // IWYU pragma: keep

#include "backendData.hpp"
#include "util.hpp"

namespace CGIMBGFX {
    typedef uint16_t u16;
    typedef int32_t i32;
    typedef uint32_t u32;
    typedef uint64_t u64;
    using std::array, std::memcpy;

    namespace detail {
        [[nodiscard]] inline bool fromImTextureID(ImTextureID id, bgfx::TextureHandle& result) noexcept {
            if (id == ImTextureID_Invalid || id == 0 || id > internal::U16_MAX) return false;
            result.idx = static_cast<u16>(id - 1);
            if (!bgfx::isValid(result)) return false;
            return true;
        }

        [[nodiscard]] inline bool fillBuffers(bgfx::TransientVertexBuffer& tvb, bgfx::TransientIndexBuffer& tib, const ImDrawData& drawData, const BackendData& backendData) noexcept {
            const u32
                vertexCount = static_cast<u32>(drawData.TotalVtxCount),
                indexCount = static_cast<u32>(drawData.TotalIdxCount);
            if (vertexCount == 0 || indexCount == 0) return false;

            const bool use32BitIndexBuffer = vertexCount > 0xFFFFu;
            if (
                bgfx::getAvailTransientVertexBuffer(vertexCount, backendData.layout) < vertexCount
             || bgfx::getAvailTransientIndexBuffer(indexCount, use32BitIndexBuffer) < indexCount
            ) return false;

            bgfx::allocTransientVertexBuffer(&tvb, vertexCount, backendData.layout);
            bgfx::allocTransientIndexBuffer(&tib, indexCount, use32BitIndexBuffer);

            auto* vertexCursor = reinterpret_cast<ImDrawVert*>(tvb.data);
            for (i32 drawListIndex = 0; drawListIndex < drawData.CmdListsCount; drawListIndex++) {
                const ImDrawList* drawList = drawData.CmdLists[drawListIndex];
                const size_t vertexByteCount = static_cast<size_t>(drawList->VtxBuffer.Size) * sizeof(ImDrawVert);
                memcpy(vertexCursor, drawList->VtxBuffer.Data, vertexByteCount);
                vertexCursor += drawList->VtxBuffer.Size;
            }

            u32 globalVertexOffset = 0, globalIndexOffset = 0;
            if (use32BitIndexBuffer) {
                auto* indexCursor = reinterpret_cast<u32*>(tib.data);
                for (i32 drawListIndex = 0; drawListIndex < drawData.CmdListsCount; drawListIndex++) {
                    const ImDrawList* drawList = drawData.CmdLists[drawListIndex];
                    for (i32 drawCommandIndex = 0; drawCommandIndex < drawList->CmdBuffer.Size; drawCommandIndex++) {
                        const ImDrawCmd& drawCommand = drawList->CmdBuffer[drawCommandIndex];
                        if (drawCommand.ElemCount == 0) continue;
                        const u32 rebasedVertexOffset = globalVertexOffset + drawCommand.VtxOffset;
                        const ImDrawIdx* sourceIndexPointer = drawList->IdxBuffer.Data + drawCommand.IdxOffset;
                        u32* destinationIndexPointer = indexCursor + globalIndexOffset + drawCommand.IdxOffset;
                        for (u32 index = 0; index < drawCommand.ElemCount; index++) destinationIndexPointer[index] = static_cast<u32>(sourceIndexPointer[index]) + rebasedVertexOffset;
                    }
                    globalVertexOffset += static_cast<u32>(drawList->VtxBuffer.Size);
                    globalIndexOffset += static_cast<u32>(drawList->IdxBuffer.Size);
                }
            }
            else {
                auto* indexCursor = reinterpret_cast<u16*>(tib.data);
                for (i32 drawListIndex = 0; drawListIndex < drawData.CmdListsCount; drawListIndex++) {
                    const ImDrawList* drawList = drawData.CmdLists[drawListIndex];
                    for (i32 drawCommandIndex = 0; drawCommandIndex < drawList->CmdBuffer.Size; drawCommandIndex++) {
                        const ImDrawCmd& drawCommand = drawList->CmdBuffer[drawCommandIndex];
                        if (drawCommand.ElemCount == 0) continue;
                        const u32 rebasedVertexOffset = globalVertexOffset + drawCommand.VtxOffset;
                        const ImDrawIdx* sourceIndexPointer = drawList->IdxBuffer.Data + drawCommand.IdxOffset;
                        u16* destinationIndexPointer = indexCursor + globalIndexOffset + drawCommand.IdxOffset;
                        for (u32 index = 0; index < drawCommand.ElemCount; index++) {
                            const u32 rebasedIndex = static_cast<u32>(sourceIndexPointer[index]) + rebasedVertexOffset;
                            IM_ASSERT(rebasedIndex <= 0xFFFFu);
                            destinationIndexPointer[index] = static_cast<u16>(rebasedIndex);
                        }
                    }
                    globalVertexOffset += static_cast<u32>(drawList->VtxBuffer.Size);
                    globalIndexOffset += static_cast<u32>(drawList->IdxBuffer.Size);
                }
            }
            return true;
        }

        inline void resetState(const array<float, 16>& proj, bgfx::ViewId viewId, u16 frameBufferW, u16 frameBufferH) noexcept {
            bgfx::setViewMode(viewId, bgfx::ViewMode::Sequential);
            bgfx::setViewTransform(viewId, nullptr, proj.data());
            bgfx::setViewRect(viewId, 0, 0, frameBufferW, frameBufferH);
        }
    }

    namespace internal {
        inline void updateTexture(ImTextureData* textureData) noexcept {
            if (textureData == nullptr) return;
            switch (textureData->Status) {
                case ImTextureStatus_OK: break;
                case ImTextureStatus_WantCreate: {
                    IM_ASSERT(textureData->Format == ImTextureFormat_RGBA32);
                    if (textureData->Width <= 0 || textureData->Height <= 0) break;
                    const auto bgfxTexHandle = bgfx::createTexture2D(
                        static_cast<u16>(textureData->Width),
                        static_cast<u16>(textureData->Height),
                        false, 1, bgfx::TextureFormat::RGBA8,
                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                    );
                    if (!bgfx::isValid(bgfxTexHandle)) break;
                    bgfx::updateTexture2D(
                        bgfxTexHandle, 0, 0, 0, 0,
                        static_cast<u16>(textureData->Width),
                        static_cast<u16>(textureData->Height),
                        bgfx::copy(textureData->GetPixels(), static_cast<u32>(textureData->GetSizeInBytes()))
                    );
                    //We add 1 to the index to make it impossible to produce `0`. `0` is reserved by ImGui to mean "invalid texture".
                    textureData->SetTexID(static_cast<ImTextureID>(bgfxTexHandle.idx + 1));
                    //We can simply subtract 1 from the index to get the original texture handle. There's nothing more to store.
                    textureData->BackendUserData = nullptr;
                    textureData->SetStatus(ImTextureStatus_OK);
                    break;
                }
                case ImTextureStatus_WantUpdates: {
                    const ImTextureID texID = textureData->GetTexID();
                    bgfx::TextureHandle textureHandle;
                    if (!detail::fromImTextureID(texID, textureHandle)) break;
                    for (ImTextureRect& textureRect : textureData->Updates) {
                        const u32
                            bytesInARow = static_cast<u32>(textureRect.w) * static_cast<u32>(textureData->BytesPerPixel),
                            totalBytes = bytesInARow * static_cast<u32>(textureRect.h);
                        const bgfx::Memory* updateMemory = bgfx::alloc(totalBytes);
                        const auto* sourceRow = static_cast<const u8*>(textureData->GetPixelsAt(textureRect.x, textureRect.y));
                        auto* destinationRow = updateMemory->data;
                        for (u16 row = 0; row < textureRect.h; row++) {
                            memcpy(destinationRow, sourceRow, bytesInARow);
                            sourceRow += textureData->GetPitch();
                            destinationRow += bytesInARow;
                        }
                        bgfx::updateTexture2D(
                            textureHandle,
                            0, 0,
                            textureRect.x,
                            textureRect.y,
                            textureRect.w,
                            textureRect.h,
                            updateMemory
                        );
                    }
                    textureData->SetStatus(ImTextureStatus_OK);
                    break;
                }
                case ImTextureStatus_WantDestroy: {
                    if (textureData->UnusedFrames <= 0) break;
                    const ImTextureID texID = textureData->GetTexID();
                    bgfx::TextureHandle textureHandle;
                    if (!detail::fromImTextureID(texID, textureHandle)) break;
                    bgfx::destroy(textureHandle);
                    textureData->SetTexID(ImTextureID_Invalid);
                    textureData->BackendUserData = nullptr;
                    textureData->SetStatus(ImTextureStatus_Destroyed);
                    break;
                }
                case ImTextureStatus_Destroyed: break;
            }
        }

        inline void render(ImDrawData* drawData, bgfx::ViewId viewId) noexcept {
            if (drawData == nullptr) return;
            
            const auto* backendData = ImGui_Implbgfx_extras_GetBackendData();
            IM_ASSERT(backendData != nullptr);
            IM_ASSERT(bgfx::isValid(backendData->shader));
            IM_ASSERT(bgfx::isValid(backendData->sampler));
            
            if (drawData->Textures != nullptr) for (ImTextureData* textureData : *drawData->Textures) internal::updateTexture(textureData);
            
            if (drawData->CmdListsCount == 0) return;
            
            const float
                frameBufferWF = drawData->DisplaySize.x * drawData->FramebufferScale.x,
                frameBufferHF = drawData->DisplaySize.y * drawData->FramebufferScale.y;
                if (frameBufferWF <= 0.0f || frameBufferHF <= 0.0f) return;
            
            const u16
                frameBufferW = internal::clampFloatToU16(frameBufferWF),
                frameBufferH = internal::clampFloatToU16(frameBufferHF);
            if (frameBufferW == 0 || frameBufferH == 0) return;
            
            const bgfx::Caps* caps = bgfx::getCaps();
            if (caps == nullptr) return;
            
            array<float, 16> proj;
            {
                const float
                    left = drawData->DisplayPos.x,
                    right = drawData->DisplayPos.x + drawData->DisplaySize.x,
                    top = drawData->DisplayPos.y,
                    bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
                bx::mtxOrtho(
                    proj.data(),
                    left, right, bottom, top,
                    0.0f, 1000.0f, 0.0f,
                    caps->homogeneousDepth
                );
            }
            
            detail::resetState(proj, viewId, frameBufferW, frameBufferH);
            
            bgfx::TransientVertexBuffer tvb;
            bgfx::TransientIndexBuffer tib;
            if (!detail::fillBuffers(tvb, tib, *drawData, *backendData)) return;
            
            const ImVec2
                clipOffset = drawData->DisplayPos,
                clipScale = drawData->FramebufferScale;
            const u64 renderState =
                BGFX_STATE_WRITE_RGB |
                BGFX_STATE_WRITE_A |
                BGFX_STATE_MSAA |
                BGFX_STATE_BLEND_FUNC_SEPARATE(
                    BGFX_STATE_BLEND_SRC_ALPHA,
                    BGFX_STATE_BLEND_INV_SRC_ALPHA,
                    BGFX_STATE_BLEND_ONE,
                    BGFX_STATE_BLEND_INV_SRC_ALPHA
                );
            
            u32 globalIndexOffset = 0;
            for (i32 drawListIndex = 0; drawListIndex < drawData->CmdListsCount; drawListIndex++) {
                const ImDrawList* drawList = drawData->CmdLists[drawListIndex];
                for (i32 drawCommandIndex = 0; drawCommandIndex < drawList->CmdBuffer.Size; drawCommandIndex++) {
                    const ImDrawCmd& drawCommand = drawList->CmdBuffer[drawCommandIndex];
                    if (drawCommand.UserCallback != nullptr) {
                        if (drawCommand.UserCallback == ImDrawCallback_ResetRenderState) detail::resetState(proj, viewId, frameBufferW, frameBufferH);
                        else drawCommand.UserCallback(drawList, &drawCommand);
                        continue;
                    }
                    if (drawCommand.ElemCount == 0) continue;
                    
                    const float
                        clipMinX = (drawCommand.ClipRect.x - clipOffset.x) * clipScale.x,
                        clipMinY = (drawCommand.ClipRect.y - clipOffset.y) * clipScale.y,
                        clipMaxX = (drawCommand.ClipRect.z - clipOffset.x) * clipScale.x,
                        clipMaxY = (drawCommand.ClipRect.w - clipOffset.y) * clipScale.y;
                    if (clipMaxX <= clipMinX || clipMaxY <= clipMinY) continue;
                    
                    const float
                        clampedClipMinX = bx::max(clipMinX, 0.0f),
                        clampedClipMinY = bx::max(clipMinY, 0.0f),
                        clampedClipMaxX = bx::min(clipMaxX, static_cast<float>(frameBufferW)),
                        clampedClipMaxY = bx::min(clipMaxY, static_cast<float>(frameBufferH));
                    if (clampedClipMaxX <= clampedClipMinX || clampedClipMaxY <= clampedClipMinY) continue;
                    
                    const u16
                        scissorX = internal::clampFloatToU16(clampedClipMinX),
                        scissorY = internal::clampFloatToU16(clampedClipMinY),
                        scissorW = internal::clampFloatToU16(clampedClipMaxX - clampedClipMinX),
                        scissorH = internal::clampFloatToU16(clampedClipMaxY - clampedClipMinY);
                    if (scissorW == 0 || scissorH == 0) continue;
                    
                    const ImTextureID encodedTextureId = drawCommand.GetTexID();
                    if (encodedTextureId == ImTextureID_Invalid) continue;
                    
                    bgfx::TextureHandle textureHandle;
                    if (!detail::fromImTextureID(encodedTextureId, textureHandle)) continue;
                    
                    bgfx::setScissor(scissorX, scissorY, scissorW, scissorH);
                    bgfx::setState(renderState);
                    bgfx::setTexture(0, backendData->sampler, textureHandle);
                    bgfx::setVertexBuffer(0, &tvb, 0, static_cast<u32>(drawData->TotalVtxCount));
                    bgfx::setIndexBuffer(&tib, globalIndexOffset + drawCommand.IdxOffset, drawCommand.ElemCount);
                    bgfx::submit(viewId, backendData->shader);
                }
                globalIndexOffset += static_cast<u32>(drawList->IdxBuffer.Size);
            }
        }
    }
    
    inline void ImGui_Implbgfx_RenderDrawData(ImDrawData* drawData) noexcept {
        const auto* backendData = ImGui_Implbgfx_extras_GetBackendData();
        if (backendData == nullptr) return;

        const bool renderToFrameBuffer = bgfx::isValid(backendData->renderToFrameBuffer);
        if (renderToFrameBuffer) bgfx::setViewFrameBuffer(backendData->startingViewId, backendData->renderToFrameBuffer);

        const ImGuiViewport* ownerViewport = drawData != nullptr ? drawData->OwnerViewport : nullptr;
        const bool isMainViewport = ownerViewport == nullptr || ownerViewport == ImGui::GetMainViewport();
        //Don't clear the main viewport. Doing this will clear the entire screen, including the main content of the application.
        if (renderToFrameBuffer || (!isMainViewport && (ownerViewport->Flags & ImGuiViewportFlags_NoRendererClear) == 0)) {
            bgfx::setViewClear(backendData->startingViewId, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
            bgfx::touch(backendData->startingViewId);
        }
        else bgfx::setViewClear(backendData->startingViewId, BGFX_CLEAR_NONE);
        
        internal::render(drawData, backendData->startingViewId);
    }
}