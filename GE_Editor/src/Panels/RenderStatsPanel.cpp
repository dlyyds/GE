//
// 渲染统计面板实现 —— 展示 Renderer 每帧 draw call / 三角形 / FPS。
//

#include "RenderStatsPanel.h"

#include "GE/Core/Application.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/AssetManager.h"

#include "imgui.h"

namespace GE {

void RenderStatsPanel::OnImGuiRender() {
    // ── 渲染统计面板 ──────────────────────────────────────────────────
    // 停靠进主 DockSpace（根上下文取 "MainDockspace"，与 DockSpaceLayer 一致）
    ImGui::SetNextWindowDockID(ImGui::GetID("MainDockspace"), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("渲染统计")) {
        const auto &stats = Renderer::GetStats();

        ImGui::Text("帧率: %.1f FPS", Application::Get().GetFPS());
        ImGui::Text("帧率上限 (FPS, 0=不限):");
        ImGui::SameLine();
        float fpsLimit = Application::Get().GetFrameRateLimit();
        if (ImGui::InputFloat("##FrameRateLimit", &fpsLimit, 1.0f, 10.0f, "%.1f")) {
            Application::Get().SetFrameRateLimit(fpsLimit);
        }
        ImGui::SameLine();
        if (Application::Get().IsFrameRateLimited()) {
            ImGui::TextDisabled("已启用");
        } else {
            ImGui::TextDisabled("未启用");
        }

        ImGui::Separator();
        ImGui::Text("音频");
        if (ImGui::Button("Play Test Tone")) {
            Renderer::GetAssetManager().PlayOneShot("audio/sfx/test.wav", 1.0f);
        }

        ImGui::Separator();
        ImGui::Text("2D（精灵批处理）");
        ImGui::BulletText("Draw Calls: %u", stats.drawCalls2D);
        ImGui::BulletText("三角形: %u", stats.triangles2D);

        ImGui::Separator();
        ImGui::Text("3D（网格）");
        ImGui::BulletText("Draw Calls: %u", stats.drawCalls3D);
        ImGui::BulletText("排序批次: %u", stats.batches3D);
        ImGui::BulletText("三角形: %u", stats.triangles3D);

        ImGui::Separator();
        ImGui::Text("总计");
        ImGui::BulletText("Draw Calls: %u", stats.TotalDrawCalls());
        ImGui::BulletText("三角形: %u", stats.TotalTriangles());
    }
    ImGui::End();
}

} // namespace GE
