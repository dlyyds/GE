#include "Core/Layer.h"
#include "pch.h"

namespace GE {

Layer::Layer(const std::string &debugName) : m_DebugName(debugName) { GE_PROFILE_FUNCTION(); }

Layer::~Layer() {}

} // namespace GE