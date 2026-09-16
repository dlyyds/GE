#pragma once

// 与 KeyCodes.h 同理：自己引入 `<cstdint>`，别指望调用方的 PCH
#include <cstdint>

namespace GE {

using MouseCode = uint16_t;

/**
 * @brief 鼠标按键码 —— 取值与 SDL 的 `SDL_BUTTON_*` 对应。
 *
 * ## 与 GLFW 布局的语义差异（升级时注意）
 *
 * GLFW 的编号是 `左=0, 右=1, 中=2`；SDL 是 `左=1, 中=2, 右=3`。
 * 本枚举改用 SDL 编号后，`Button0..Button7` 的含义随之改变 —— 例如 Lua 里
 * `Mouse.Button1` 由「右键」变成「左键」。这是本次换库唯一一处**静默的语义变化**。
 *
 * 之所以敢改：仓库内没有任何脚本或 C++ 代码按数值使用 `ButtonN`（脚本只用
 * `Mouse.ButtonLeft`，C++ 只用 `ButtonLeft` / `ButtonRight` 做具名比较）。
 * 具名常量是推荐且稳定的 API，其语义未变。
 *
 * 改成 SDL 编号的直接收益：`SdlWindow` 翻译事件时 `event.button.button` 可以
 * 原样透传，不需要任何映射表。
 */
namespace Mouse {
enum : MouseCode {
    // 与 SDL 编号对齐。SDL 不产生 0 号按键，Button0 仅占位以保持编号一致。
    Button0 = 0,
    Button1 = 1, ///< SDL_BUTTON_LEFT
    Button2 = 2, ///< SDL_BUTTON_MIDDLE
    Button3 = 3, ///< SDL_BUTTON_RIGHT
    Button4 = 4, ///< SDL_BUTTON_X1
    Button5 = 5, ///< SDL_BUTTON_X2
    Button6 = 6,
    Button7 = 7,

    ButtonLast = Button7,

    // ---- 具名别名（推荐的用法，语义与换库前一致）----
    ButtonLeft = Button1,
    ButtonMiddle = Button2,
    ButtonRight = Button3,
    ButtonX1 = Button4,
    ButtonX2 = Button5
};
}

} // namespace GE
