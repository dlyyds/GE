#pragma once

#include "Base.h"
#include "Layer.h"

#include <vector>

namespace GE {

class LayerStack {
public:
    LayerStack();

    ~LayerStack();

    void PushLayer(std::shared_ptr<Layer> layer);

    void PushOverlay(std::shared_ptr<Layer> overlay);

    void PopLayer(Layer *layer);

    void PopOverlay(Layer *overlay);

    void Clear();

    std::vector<std::shared_ptr<Layer> >::iterator begin() { return m_Layers.begin(); }
    std::vector<std::shared_ptr<Layer> >::iterator end() { return m_Layers.end(); }
    std::vector<std::shared_ptr<Layer> >::reverse_iterator rbegin() { return m_Layers.rbegin(); }
    std::vector<std::shared_ptr<Layer> >::reverse_iterator rend() { return m_Layers.rend(); }

    [[nodiscard]] std::vector<std::shared_ptr<Layer> >::const_iterator begin() const { return m_Layers.begin(); }
    [[nodiscard]] std::vector<std::shared_ptr<Layer> >::const_iterator end() const { return m_Layers.end(); }
    [[nodiscard]] std::vector<std::shared_ptr<Layer> >::const_reverse_iterator rbegin() const { return m_Layers.rbegin(); }
    [[nodiscard]] std::vector<std::shared_ptr<Layer> >::const_reverse_iterator rend() const { return m_Layers.rend(); }

private:
    std::vector<std::shared_ptr<Layer> > m_Layers;
    uint32_t m_LayerInsertIndex = 0;
};

} // namespace GE