#pragma once

//The obscure namespace name is meant to avoid them being shown alongside with the intended names in the auto-completion list.

#include "init.hpp"
using ImGui_Implbgfx_InitConfig = CGIMBGFX::InitConfig;
using CGIMBGFX::ImGui_Implbgfx_Init;

#include "multiViewport.hpp"
using CGIMBGFX::ImGui_Implbgfx_RendererSupportsMultiViewport;

#include "render.hpp"
using CGIMBGFX::ImGui_Implbgfx_RenderDrawData;

#include "shutdown.hpp"
using CGIMBGFX::ImGui_Implbgfx_Shutdown;