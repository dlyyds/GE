#include <GE.h>
#include <GE/Core/EntryPoint.h>

#include "EditorLayer.h"

namespace GE {

class bgfxtest : public Application {
public:
    explicit bgfxtest(ApplicationCommandLineArgs args)
        : Application("GEEditor", args) {
        PushLayer(CreateRef<EditorLayer>());
    }

    ~bgfxtest() override = default;
};

Application *CreateApplication(ApplicationCommandLineArgs args) {
    return new bgfxtest(args);
}
}