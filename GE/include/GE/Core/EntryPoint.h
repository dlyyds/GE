#pragma once

#include "Application.h"
#include "Debug/Instrumentor.h"
#include "Log.h"

#ifdef _WIN64


int main(int argc, char **argv) {

    GE::Log::Init();

    GE_PROFILE_BEGIN_SESSION("Startup", "GEProfile-Startup.json");
    auto app = GE::CreateApplication({argc, argv});
    GE_PROFILE_END_SESSION();

    GE_PROFILE_BEGIN_SESSION("Runtime", "GEProfile-Runtime.json");
    app->Run();
    GE_PROFILE_END_SESSION();

    GE_PROFILE_BEGIN_SESSION("Shutdown", "GEProfile-Shutdown.json");
    delete app;
    GE_PROFILE_END_SESSION();

    return 0;
}

#endif