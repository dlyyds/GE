#pragma once

#include "Application.h"
#include "Debug/Profiler.h"
#include "Log.h"

#ifdef _WIN64


int main(int argc, char **argv) {

    GE::Log::Init();

    auto app = GE::CreateApplication({argc, argv});
    app->Run();
    delete app;

    return 0;
}

#endif