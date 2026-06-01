#include <GE.h>
#include <GE/Core/EntryPoint.h>


#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "SandboxLayer.h"
#include "TextureSampleLayer.h"

namespace GE {
class Sandbox : public Application {
public:
    explicit Sandbox(ApplicationCommandLineArgs args) : Application("Sandbox", args) {
        GE_PROFILE_FUNCTION();
        PushLayer(CreateRef<SandboxLayer>());
        //PushLayer(CreateRef<TextureSampleLayer>());
    }

    ~Sandbox() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new Sandbox(args); }


}