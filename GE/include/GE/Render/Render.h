#pragma once
#include "Core/Application.h"

namespace GE {

class Render {
public:
    enum RenderAPI {
        NONE = 0, OPEN_GL = 1,
    };

    static RenderAPI API;

    static void Init();

    static void Destroy();

};


}