#include <GE.h>
#include <GE/Core/EntryPoint.h>


#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


#include "TriangleLayer.h"
#include "TextureLayer.h"

namespace GE {
class Sandbox : public Application {
public:
    explicit Sandbox(ApplicationCommandLineArgs args) : Application("Sandbox", args) {
        GE_PROFILE_FUNCTION();
        //PushLayer(std::make_shared<SandboxLayer>());
        //PushLayer(std::make_shared<TextureSampleLayer>());
        //PushLayer(std::make_shared<VulkanLayer>());
        //PushLayer(std::make_shared<TriangleLayer>());
        PushLayer(std::make_shared<TextureLayer>());
    }

    ~Sandbox() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new Sandbox(args); }


}