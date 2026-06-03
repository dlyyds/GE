#pragma once

#include "Base.h"
#include "Core/Timestep.h"
#include "Events/Event.h"

namespace GE {

class Layer {
public:
    Layer(const std::string &name = "Layer");

    virtual ~Layer();

    virtual void OnAttach() = 0;

    virtual void OnDetach() = 0;

    virtual void OnUpdate(Timestep &ts) = 0;

    virtual void OnEvent(Event &event) = 0;

    virtual void OnImGuiRender() = 0;

    inline const std::string &GetName() const { return m_DebugName; }

protected:
    std::string m_DebugName;
};

} // namespace GE