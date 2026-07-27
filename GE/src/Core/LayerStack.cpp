#include "Core/LayerStack.h"


namespace GE {

LayerStack::LayerStack() {
    GE_PROFILE_FUNCTION();
    m_LayerInsertIndex = 0;
}

LayerStack::~LayerStack() {
    for (auto &layer : m_Layers)
        layer->OnDetach();
}

void LayerStack::PushLayer(std::shared_ptr<Layer> layer) {
    m_Layers.emplace(begin() + m_LayerInsertIndex, std::move(layer));
    m_LayerInsertIndex++;
}

void LayerStack::PushOverlay(std::shared_ptr<Layer> overlay) { m_Layers.emplace_back(std::move(overlay)); }


void LayerStack::PopLayer(Layer *layer) {
    auto it = std::find_if(m_Layers.begin(), m_Layers.begin() + m_LayerInsertIndex,
                           [layer](const std::shared_ptr<Layer> &ref) { return ref.get() == layer; });

    if (it != m_Layers.begin() + m_LayerInsertIndex) {
        (*it)->OnDetach();
        m_Layers.erase(it);
        m_LayerInsertIndex--;
    }
}

void LayerStack::PopOverlay(Layer *overlay) {
    auto it = std::find_if(m_Layers.begin(), m_Layers.end(),
                           [overlay](const std::shared_ptr<Layer> &ref) { return ref.get() == overlay; });
    if (it != m_Layers.end())
        m_Layers.erase(it);
}

void LayerStack::Clear() {
    for (auto &layer : m_Layers)
        layer->OnDetach();
    m_Layers.clear();
}

} // namespace GE