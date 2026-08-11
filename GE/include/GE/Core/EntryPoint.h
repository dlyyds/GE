#pragma once

#include "Application.h"
#include "Debug/Profiler.h"
#include "Log.h"

#ifdef _WIN64


int main(int argc, char **argv) {

    GE::Log::Init();

    // Tracy 整个应用生命周期只需一个会话（连接）。
    GE_PROFILE_BEGIN_SESSION();
    auto app = GE::CreateApplication({argc, argv});
    app->Run();
    delete app;
    GE_PROFILE_END_SESSION();

    return 0;
}

#endif