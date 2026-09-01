#include <GE.h>
#include <GE/Core/EntryPoint.h>

#include "SceneLayer.h"
#include "HierarchyLayer.h"
#include "ResourceLayer.h"
#include "DockSpaceLayer.h"
#include "GizmoController.h"
#include "ASMGraphLayer.h"

namespace GE {
class EditorApp : public Application {
public:
    explicit EditorApp(ApplicationCommandLineArgs args) : Application("GE_Editor", args) {
        GE_PROFILE_FUNCTION();
        // DockSpaceLayer 须最先渲染，才能让后续窗口停靠进它创建的 DockSpace
        auto dock_space = std::make_shared<DockSpaceLayer>();
        // 共享场景上下文：场景层与面板层共同持有，新建/加载场景时自动同步
        auto scene_ctx = std::make_shared<EditorContext>();
        auto scene_layer = std::make_shared<SceneLayer>(scene_ctx);
        auto hierarchy_layer = std::make_shared<HierarchyLayer>(scene_ctx);
        auto resource_layer = std::make_shared<ResourceLayer>();
        auto asm_graph_layer = std::make_shared<ASMGraphLayer>(scene_ctx, hierarchy_layer.get());

        // auto sandbox_layer = std::make_shared<SandboxLayer>();
        // 让顶部「文件」菜单里的场景操作绑定到场景层
        dock_space->SetSceneLayer(scene_layer.get());
        // 在场景视口上叠加 ImGuizmo 变换 gizmo（由 SceneLayer 在 Scene 窗口内回调）
        scene_layer->SetGizmoController(std::make_unique<GizmoController>(scene_ctx, hierarchy_layer.get()));
        PushLayer(dock_space);
        PushLayer(scene_layer);
        PushLayer(hierarchy_layer);
        PushLayer(resource_layer);
        PushLayer(asm_graph_layer);
    }

    ~EditorApp() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new EditorApp(args); }

} // namespace GE