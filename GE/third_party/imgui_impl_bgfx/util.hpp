#pragma once
#include <cstdint>
#include <limits>

namespace CGIMBGFX::internal {
    typedef uint16_t u16;
    using std::numeric_limits;

    inline constexpr u16 U16_MAX = numeric_limits<u16>::max();
    inline constexpr float U16_MAX_FLOAT = static_cast<float>(U16_MAX);
    
    [[nodiscard]] inline u16 clampFloatToU16(float input) noexcept {
        if (input <= 0.0f) return 0;
        if (input >= U16_MAX_FLOAT) return U16_MAX;
        return static_cast<u16>(input + 0.5f);
    }
}