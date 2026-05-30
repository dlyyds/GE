#pragma once

#include "Base.h"
#include "Core/Timestep.h"
#include "Events/Event.h"

namespace GE {

class Layer {
  public:
    Layer(const std::string &name = "Layer");
    virtual ~Layer();

    virtual void OnAttach() {}
    virtual void OnDetach() {}
    virtual void OnUpdate(Timestep &ts) {}
    virtual void OnEvent(Event &event) {}
    virtual void OnImGuiRender() {}
    inline const std::string &GetName() const { return m_DebugName; }

  protected:
    std::string m_DebugName;
};

} // namespace GE