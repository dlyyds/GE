#pragma once

#include "Base.h"
#include "Layer.h"

#include <vector>

namespace GE {

class LayerStack {
public:
    LayerStack();

    ~LayerStack();

    void PushLayer(Ref<Layer> layer);

    void PushOverlay(Ref<Layer> overlay);

    void PopLayer(Layer *layer);

    void PopOverlay(Layer *overlay);

    std::vector<Ref<Layer>>::iterator begin() { return m_Layers.begin(); }
    std::vector<Ref<Layer>>::iterator end() { return m_Layers.end(); }
    std::vector<Ref<Layer>>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
    std::vector<Ref<Layer>>::reverse_iterator rend() { return m_Layers.rend(); }

    [[nodiscard]] std::vector<Ref<Layer>>::const_iterator begin() const { return m_Layers.begin(); }
    [[nodiscard]] std::vector<Ref<Layer>>::const_iterator end() const { return m_Layers.end(); }
    [[nodiscard]] std::vector<Ref<Layer>>::const_reverse_iterator rbegin() const { return m_Layers.rbegin(); }
    [[nodiscard]] std::vector<Ref<Layer>>::const_reverse_iterator rend() const { return m_Layers.rend(); }

private:
    std::vector<Ref<Layer>> m_Layers;
    uint32_t m_LayerInsertIndex = 0;
};

} // namespace GE