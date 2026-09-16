#pragma once

#include "GE/Core/InputState.h"
#include "GE/Core/KeyCodes.h"
#include "GE/Events/Event.h"
#include "GE/Events/MouseEvent.h"
#include "GE/Events/TouchEvent.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>

namespace GE {

/**
 * @brief 运行时触屏控制：左手浮动摇杆移动、右手拖拽转视角、右侧轻点攻击。
 *
 * ## 为什么要把触摸单独做一层（而不是开 `SDL_HINT_TOUCH_MOUSE_EVENTS`）
 *
 * 计划书阶段 C 原打算让 SDL 把单指触摸合成鼠标即可。实际不行：SDL 的合成只产生**一个**
 * 鼠标指针，而这类游戏的操控天然是"左拇指按摇杆 + 右拇指拖视角"同时进行 —— 合成鼠标
 * 只能跟着其中一根手指，另一根会被抢走。故 `SdlWindow` 显式关掉了该 hint，由本类按
 * `PointerId` 自己做多指仲裁。
 *
 * ## 合成的是"引擎事件"，不是新 API
 *
 * 输出一律是引擎既有的事件类型（`KeyPressed/ReleasedEvent`、`MouseMovedEvent`、
 * `MouseButtonPressed/ReleasedEvent`），由 `GameLayer` 喂进 `Scene::OnEvent` —— 也就是
 * 真实输入走的那条路径。收益是整个下游（`InputState` → Lua `input.is_held(Key.W)` →
 * `character.set_move`）**一行都不用改**，现成的 6 个角色脚本直接可用。
 *
 * ## 三条"看起来可以省、其实不能省"的约束（都对应一次真实会踩的错）
 *
 * 1. **视角只发 `MouseMovedEvent`，不发鼠标按键**。跟随相机路径同时有两个消费者：
 *    `Scene::UpdateFollowCamera` 直接读 `GetMouseDelta()`，而场景主相机的
 *    `Camera::OnMouseMove` 在**左键按住**时也会转。若合成左键，两边各转一次 = 灵敏度翻倍。
 *    自由视角兜底路径（无主相机）没有前者，此时才需要左键 —— 由
 *    `SetLookEmitsLeftButton()` 显式打开。
 * 2. **轻点必须在手指抬起时判定并触发**，不能在按下时。否则每次拖拽视角的起手都会
 *    多挥一刀。
 * 3. **虚拟鼠标位置必须用当前 `InputState::GetMousePos()` 播种**。`InputState` 的增量是
 *    "本帧位置 − 上帧位置"，若虚拟位置从 0 开始而真实位置另有其值（接了实体鼠标的
 *    设备），首帧会得到一个巨大的 delta，视角直接甩飞。
 *
 * ## 坐标空间
 *
 * 事件里是 SDL 的归一化坐标 `[0,1]`；本类内部一律换算成**像素**再做几何（圆形死区、
 * 摇杆半径、轻点阈值），因为归一化坐标是按轴分别归一化的，在非正方形屏幕上"圆"会变成
 * 椭圆。绘制同样用像素，两端一致（见 `TouchEvent.h` 的说明）。
 */
class TouchControls {
public:
    /// 合成事件的出口。由 `GameLayer` 注入，内容等价于"把事件交给场景的那段转发逻辑"。
    using Emitter = std::function<void(Event &)>;

    TouchControls(Emitter emitter, const InputState &inputState);

    /// 每帧由 `GameLayer` 同步窗口像素尺寸（旋转/缩放后会变）。
    void SetViewport(uint32_t width, uint32_t height);

    /// 处理事件；返回 true 表示已被本类消费（触摸事件一律消费，避免继续传播）。
    bool OnEvent(Event &e);

    /// 每帧结算摇杆 → 合成 WASD 键事件。应在场景仿真推进**之前**调用。
    void Update();

    /// 释放所有手指（`GameLayer::OnDetach`、切后台等）：撤销摇杆按键与视角左键。
    void ReleaseAll();

    /// 自由视角兜底路径需要靠"按住左键"来转视角时才置真（见类注释第 1 条）。
    void SetLookEmitsLeftButton(bool enabled) {
        m_LookEmitsLeftButton = enabled;
    }

    // ---- HUD 绘制用（像素坐标；`GameLayer::OnImGuiRender` 读它画摇杆）----

    bool IsStickActive() const {
        return m_StickActive;
    }

    glm::vec2 GetStickCenterPx() const {
        return m_StickCenterPx;
    }

    /// 摇杆头位置（已按半径钳制）。
    glm::vec2 GetStickKnobPx() const {
        return m_StickCenterPx + m_StickOffsetPx;
    }

    float GetStickRadiusPx() const {
        return StickRadiusPx();
    }

private:
    /// 摇杆半径：取屏幕短边的比例，横竖屏都合适。
    float StickRadiusPx() const;
    /// 死区：半径的比例。低于它不出方向，避免手指静止时的抖动被当成输入。
    float StickDeadzonePx() const;

    /// 归一化 → 像素。窗口覆盖整个屏幕，故"占屏幕的比例 × 像素尺寸"即手指的像素位置。
    glm::vec2 ToPixels(float nx, float ny) const;

    void OnTouchPressed(TouchPressedEvent &e);

    void OnTouchMoved(TouchMovedEvent &e);

    void OnTouchReleased(TouchReleasedEvent &e);

    /// 发一个方向键的按下/抬起事件。
    void EmitKey(KeyCode key, bool pressed);

    /// 松掉摇杆对应的全部方向键（抬起 / 释放时用）。
    void ReleaseStickKeys();

    Emitter m_Emit;
    const InputState &m_InputState;

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;

    // ---- 摇杆（左半屏，浮动：落点即圆心）----
    bool m_StickActive = false;
    int64_t m_StickId = -1;
    glm::vec2 m_StickCenterPx{0.0f};
    glm::vec2 m_StickOffsetPx{0.0f};   ///< 已钳制到半径内的偏移
    bool m_KeyW = false, m_KeyA = false, m_KeyS = false, m_KeyD = false;

    // ---- 视角（右半屏）----
    bool m_LookActive = false;
    int64_t m_LookId = -1;
    glm::vec2 m_LookLastPx{0.0f};
    float m_LookTravelPx = 0.0f;          ///< 累计位移，用于判定"轻点"
    bool m_LookEmitsLeftButton = false;   ///< 兜底自由视角路径才需要（见类注释）
    bool m_LookLeftDown = false;          ///< 已经合成过左键按下（用于对称释放）
    glm::vec2 m_VirtualMousePos{0.0f};

    /// 首个触摸事件到达时打一次横幅：触屏这条路是否真的接通，一眼可查
    bool m_LoggedFirstTouch = false;
};

} // namespace GE
