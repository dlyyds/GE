#include <GE.h>
#include <GE/Core/EntryPoint.h>


#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

//#include "SandboxLayer.h"
#include "VulkanLayer.h"

namespace GE {
class Sandbox : public Application {
public:
    explicit Sandbox(ApplicationCommandLineArgs args) : Application("Sandbox", args) {
        GE_PROFILE_FUNCTION();
        //PushLayer(CreateRef<SandboxLayer>());
        //PushLayer(CreateRef<TextureSampleLayer>());
        PushLayer(CreateRef<VulkanLayer>());
    }

    ~Sandbox() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new Sandbox(args); }


}