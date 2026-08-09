#include <GE.h>
#include <GE/Core/EntryPoint.h>


#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


#include "TriangleLayer.h"
#include "TextureLayer.h"
#include "ModelTestLayer.h"
#include "SceneLayer.h"
#include "DockSpaceLayer.h"

namespace GE {
class Sandbox : public Application {
public:
    explicit Sandbox(ApplicationCommandLineArgs args) : Application("Sandbox", args) {
        GE_PROFILE_FUNCTION();
        //PushLayer(std::make_shared<SandboxLayer>());
        //PushLayer(std::make_shared<TextureSampleLayer>());
        //PushLayer(std::make_shared<VulkanLayer>());
        //PushLayer(std::make_shared<TriangleLayer>());
        //PushLayer(std::make_shared<TextureLayer>());
        //PushLayer(std::make_shared<ModelTestLayer>());
        // DockSpaceLayer 须最先渲染，才能让后续窗口停靠进它创建的 DockSpace
        auto dock_space = std::make_shared<DockSpaceLayer>();
        auto scene_layer = std::make_shared<SceneLayer>();
        // 让顶部「文件」菜单里的场景操作绑定到场景层
        dock_space->SetSceneLayer(scene_layer.get());
        PushLayer(dock_space);
        PushLayer(scene_layer);
    }

    ~Sandbox() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new Sandbox(args); }


}