#pragma once

namespace GE {

using KeyCode = uint16_t;

/**
 * @brief 键码 —— 取值与 SDL3 的 `SDL_Scancode` 一一对应。
 *
 * ## 为什么是 SDL 布局
 *
 * 本枚举原先逐字复制自 glfw3.h（`Space=32, A=65, Escape=256 ...`）。换掉 GLFW 后
 * 改为直接采用 `SDL_SCANCODE_*` 的数值，理由是：SDL 的 scancode 是与物理键位绑定的
 * 跨平台稳定值（USB HID usage），桌面与 Android 由 SDL 归一到同一套值，因此
 * **不再需要 `AKEYCODE_* → GLFW 数字` 那样的平台映射表**。
 *
 * ## 名字是 API，数值不是
 *
 * 枚举**名**通过 `ScriptEngine.cpp` 的 `kKeyNames` 表注入 Lua 的 `Key` 表，
 * 是脚本可见的公开 API —— **改名会破坏脚本，改数值不会**。本次重编号保持了
 * 除 `F25` 外的全部名字（见下）。
 *
 * ## 与 GLFW 布局的两处**行为差异**（升级时注意）
 *
 * 1. **`F25` 已移除**。SDL 的 scancode 集合只有 F1–F24，没有 F25 —— 保留一个
 *    永远不可能触发的键名会静默误导（脚本写 `Key.F25` 却永不生效）。故从本枚举
 *    与 Lua 的 `Key` 表中一并删除；脚本若引用会得到 `nil` 而**显式报错**，比静默
 *    失效更容易发现。
 * 2. **`Mouse` 的按钮编号语义变了**，见 `MouseCodes.h`。
 *
 * ## 容量约束
 *
 * `SDL_SCANCODE_COUNT == 512`，即合法 scancode 为 0..511。`InputState` 的
 * `kKeyCapacity` 正好是 512，**刚好覆盖、零余量**（索引 511 有效）。又因 SDL 的
 * 400..500 段是「动态键码」保留区、Android 软键盘可能落在其中，事件翻译层做了
 * 越界丢弃，见 `SdlWindow` 的按键事件翻译。
 */
namespace Key {
enum : KeyCode {
    // -- 主键区 --
    A = 4,
    B = 5,
    C = 6,
    D = 7,
    E = 8,
    F = 9,
    G = 10,
    H = 11,
    I = 12,
    J = 13,
    K = 14,
    L = 15,
    M = 16,
    N = 17,
    O = 18,
    P = 19,
    Q = 20,
    R = 21,
    S = 22,
    T = 23,
    U = 24,
    V = 25,
    W = 26,
    X = 27,
    Y = 28,
    Z = 29,

    D1 = 30,
    D2 = 31,
    D3 = 32,
    D4 = 33,
    D5 = 34,
    D6 = 35,
    D7 = 36,
    D8 = 37,
    D9 = 38,
    D0 = 39,

    Enter = 40,
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,

    Minus = 45,
    Equal = 46,
    LeftBracket = 47,
    RightBracket = 48,
    Backslash = 49,
    World1 = 50, /* 非 US 键盘的 #1（SDL_SCANCODE_NONUSHASH） */
    Semicolon = 51,
    Apostrophe = 52,
    GraveAccent = 53,
    Comma = 54,
    Period = 55,
    Slash = 56,
    CapsLock = 57,

    // -- 功能键 --
    F1 = 58,
    F2 = 59,
    F3 = 60,
    F4 = 61,
    F5 = 62,
    F6 = 63,
    F7 = 64,
    F8 = 65,
    F9 = 66,
    F10 = 67,
    F11 = 68,
    F12 = 69,

    PrintScreen = 70,
    ScrollLock = 71,
    Pause = 72,
    Insert = 73,
    Home = 74,
    PageUp = 75,
    Delete = 76,
    End = 77,
    PageDown = 78,
    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,
    NumLock = 83, /* SDL_SCANCODE_NUMLOCKCLEAR */

    // -- 小键盘 --
    KPDivide = 84,
    KPMultiply = 85,
    KPSubtract = 86, /* SDL_SCANCODE_KP_MINUS */
    KPAdd = 87, /* SDL_SCANCODE_KP_PLUS */
    KPEnter = 88,
    KP1 = 89,
    KP2 = 90,
    KP3 = 91,
    KP4 = 92,
    KP5 = 93,
    KP6 = 94,
    KP7 = 95,
    KP8 = 96,
    KP9 = 97,
    KP0 = 98,
    KPDecimal = 99, /* SDL_SCANCODE_KP_PERIOD */

    World2 = 100, /* 非 US 键盘的 #2（SDL_SCANCODE_NONUSBACKSLASH） */
    Menu = 101, /* 上下文菜单键（SDL_SCANCODE_APPLICATION） */
    KPEqual = 103,

    F13 = 104,
    F14 = 105,
    F15 = 106,
    F16 = 107,
    F17 = 108,
    F18 = 109,
    F19 = 110,
    F20 = 111,
    F21 = 112,
    F22 = 113,
    F23 = 114,
    F24 = 115,

    // -- 修饰键 --
    LeftControl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    LeftSuper = 227,
    RightControl = 228,
    RightShift = 229,
    RightAlt = 230,
    RightSuper = 231
};
}

} // namespace GE
