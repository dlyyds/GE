#include "TouchControls.h"

#include "GE/Core/KeyCodes.h"
#include "GE/Core/Log.h"
#include "GE/Events/KeyEvent.h"
#include "GE/Events/MouseEvent.h"
#include "GE/Events/TouchEvent.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GE {

namespace {

// ── 手势几何：一律取"屏幕短边的比例"，与分辨率、密度、横竖屏无关 ──

/// 摇杆半径
constexpr float kStickRadiusRatio = 0.11f;
/// 死区 = 半径 × 该值。手指静止时也有细微抖动，没有死区会持续误出方向
constexpr float kStickDeadzoneRatio = 0.22f;
/// 轻点判定：从按下到抬起累计位移小于该比例即算轻点（不是拖拽）
constexpr float kTapMaxTravelRatio = 0.03f;

/// 八分区切换阈值 = cos(67.5°)。归一化方向向量在主轴上的分量超过它就进入该扇区，
/// 于是八向判定退化成四个独立的布尔判断（含对角）
constexpr float kOctantThreshold = 0.3827f;

/// 轻点合成的攻击按键。与 `fps_character_combat_demo.lua` 的
/// `input.just_mouse_pressed(Mouse.ButtonLeft)` 对应
constexpr MouseCode kAttackButton = Mouse::ButtonLeft;

/// 左半屏 / 右半屏的分界（归一化 x）
constexpr float kHalfSplit = 0.5f;

} // namespace

TouchControls::TouchControls(Emitter emitter, const InputState &inputState)
    : m_Emit(std::move(emitter)), m_InputState(inputState) {
}

void TouchControls::SetViewport(uint32_t width, uint32_t height) {
    m_Width = width;
    m_Height = height;
}

float TouchControls::StickRadiusPx() const {
    return kStickRadiusRatio * static_cast<float>(std::min(m_Width, m_Height));
}

float TouchControls::StickDeadzonePx() const {
    return kStickDeadzoneRatio * StickRadiusPx();
}

glm::vec2 TouchControls::ToPixels(float nx, float ny) const {
    // 窗口覆盖整个屏幕，所以"占屏幕的比例 × 像素尺寸"就是手指的像素位置 ——
    // 归一化基数（逻辑尺寸）与这里的像素尺寸描述的是同一块区域，换算不会偏。
    return {nx * static_cast<float>(m_Width), ny * static_cast<float>(m_Height)};
}

bool TouchControls::OnEvent(Event &e) {
    if (m_Width == 0 || m_Height == 0) {
        // 窗口尺寸还没同步（首个 OnUpdate 之前）：不消费，免得把事件吞掉
        GE_CORE_WARN("TouchControls: 触摸事件到达时视口尺寸仍为 0（{0}x{1}），本帧不处理", m_Width,
                     m_Height);
        return false;
    }

    if (!m_LoggedFirstTouch) {
        m_LoggedFirstTouch = true;
        GE_CORE_INFO("TouchControls: 首个触摸事件到达 —— 触屏控制已生效"
                     "（左半屏浮动摇杆移动 / 右半屏拖拽转视角 / 右侧轻点攻击）");
    }

    EventDispatcher dispatcher(e);

    // 注意 EventDispatcher 会把 e.Handled 设成回调的返回值 —— 触摸事件一律返回 true，
    // 即"已被触屏控制层消费"，不再向下传播（运行时目前只有本层，这更多是表态）
    if (dispatcher.Dispatch<TouchPressedEvent>([this](TouchPressedEvent &ev) {
            OnTouchPressed(ev);
            return true;
        })) {
        return true;
    }
    if (dispatcher.Dispatch<TouchMovedEvent>([this](TouchMovedEvent &ev) {
            OnTouchMoved(ev);
            return true;
        })) {
        return true;
    }
    if (dispatcher.Dispatch<TouchReleasedEvent>([this](TouchReleasedEvent &ev) {
            OnTouchReleased(ev);
            return true;
        })) {
        return true;
    }

    return false;
}

void TouchControls::OnTouchPressed(TouchPressedEvent &e) {
    const glm::vec2 px = ToPixels(e.GetX(), e.GetY());

    // 左半屏 = 摇杆（浮动：落点即圆心）。只认第一根手指，多余的忽略 ——
    // 状态越少越不容易出"某根手指的键没松"这类卡键
    if (e.GetX() < kHalfSplit) {
        if (m_StickActive) {
            return;
        }
        m_StickActive = true;
        m_StickId = e.GetPointerId();
        m_StickCenterPx = px;
        m_StickOffsetPx = {0.0f, 0.0f};
        return;
    }

    // 右半屏 = 视角，同样只认第一根
    if (m_LookActive) {
        return;
    }
    m_LookActive = true;
    m_LookId = e.GetPointerId();
    m_LookLastPx = px;
    m_LookTravelPx = 0.0f;

    // 播种虚拟鼠标位置：增量是"本帧位置 − 上帧位置"，不播种的话（接了实体鼠标的设备上
    // 真实位置非零）首帧 delta 会是 −真实位置，视角直接甩飞
    m_VirtualMousePos = m_InputState.GetMousePos();

    // 仅自由视角兜底路径需要"按住左键才转视角"（见类注释第 1 条）
    if (m_LookEmitsLeftButton && !m_LookLeftDown) {
        MouseButtonPressedEvent ev(kAttackButton);
        m_Emit(ev);
        m_LookLeftDown = true;
    }
}

void TouchControls::OnTouchMoved(TouchMovedEvent &e) {
    const glm::vec2 px = ToPixels(e.GetX(), e.GetY());

    if (m_StickActive && e.GetPointerId() == m_StickId) {
        glm::vec2 offset = px - m_StickCenterPx;
        const float radius = StickRadiusPx();
        const float len = glm::length(offset);
        if (len > radius) {
            offset *= radius / len;   // 钳制：手指拉出圆外后摇杆头停在边缘
        }
        m_StickOffsetPx = offset;
        return;
    }

    if (m_LookActive && e.GetPointerId() == m_LookId) {
        const glm::vec2 delta = px - m_LookLastPx;
        m_LookLastPx = px;
        // 累计的是位移的**长度**：手指来回抖动也应算作"动过"，否则会被误判成轻点
        m_LookTravelPx += glm::length(delta);

        // 累加到虚拟坐标后发绝对位置 —— InputState 求差即得本次位移。
        // 一帧内多个 motion 事件只是反复覆盖位置，差的总和仍正确
        m_VirtualMousePos += delta;
        MouseMovedEvent ev(m_VirtualMousePos.x, m_VirtualMousePos.y);
        m_Emit(ev);
    }
}

void TouchControls::OnTouchReleased(TouchReleasedEvent &e) {
    if (m_StickActive && e.GetPointerId() == m_StickId) {
        m_StickActive = false;
        m_StickId = -1;
        m_StickOffsetPx = {0.0f, 0.0f};
        ReleaseStickKeys();   // 立刻松键，不等下一帧的 Update
        return;
    }

    if (m_LookActive && e.GetPointerId() == m_LookId) {
        // **轻点必须在抬起时判定**：按下时判定会让每次拖拽视角的起手都多挥一刀
        const bool isTap = m_LookTravelPx < kTapMaxTravelRatio * static_cast<float>(std::min(m_Width, m_Height));

        m_LookActive = false;
        m_LookId = -1;
        m_LookTravelPx = 0.0f;

        // 兜底路径下先松开视角用的左键
        if (m_LookLeftDown) {
            MouseButtonReleasedEvent ev(kAttackButton);
            m_Emit(ev);
            m_LookLeftDown = false;
        }

        // 轻点 → 合成一次完整的左键点击，触发脚本里的攻击绑定
        if (isTap) {
            MouseButtonPressedEvent pressed(kAttackButton);
            m_Emit(pressed);
            MouseButtonReleasedEvent released(kAttackButton);
            m_Emit(released);
        }
    }
}

void TouchControls::Update() {
    if (!m_StickActive) {
        return;
    }

    bool wantW = false, wantA = false, wantS = false, wantD = false;

    const float len = glm::length(m_StickOffsetPx);
    if (len >= StickDeadzonePx()) {
        const glm::vec2 n = m_StickOffsetPx / len;
        // 屏幕 y 轴向下为正，故"前"是 -y
        wantW = n.y < -kOctantThreshold;
        wantS = n.y > kOctantThreshold;
        wantD = n.x > kOctantThreshold;
        wantA = n.x < -kOctantThreshold;
    }

    // 只发差异。方向切换时松与按各自只发一次，不会出现同一帧里 W 与 S 同时按住
    if (m_KeyW != wantW) {
        EmitKey(Key::W, wantW);
        m_KeyW = wantW;
    }
    if (m_KeyA != wantA) {
        EmitKey(Key::A, wantA);
        m_KeyA = wantA;
    }
    if (m_KeyS != wantS) {
        EmitKey(Key::S, wantS);
        m_KeyS = wantS;
    }
    if (m_KeyD != wantD) {
        EmitKey(Key::D, wantD);
        m_KeyD = wantD;
    }
}

void TouchControls::EmitKey(KeyCode key, bool pressed) {
    if (pressed) {
        // repeatCount 传 0：合成输入不应带键盘自动重复语义
        KeyPressedEvent e(key, 0);
        m_Emit(e);
    } else {
        KeyReleasedEvent e(key);
        m_Emit(e);
    }
}

void TouchControls::ReleaseStickKeys() {
    if (m_KeyW) {
        EmitKey(Key::W, false);
        m_KeyW = false;
    }
    if (m_KeyA) {
        EmitKey(Key::A, false);
        m_KeyA = false;
    }
    if (m_KeyS) {
        EmitKey(Key::S, false);
        m_KeyS = false;
    }
    if (m_KeyD) {
        EmitKey(Key::D, false);
        m_KeyD = false;
    }
}

void TouchControls::ReleaseAll() {
    m_StickActive = false;
    m_StickId = -1;
    m_StickOffsetPx = {0.0f, 0.0f};
    ReleaseStickKeys();

    m_LookActive = false;
    m_LookId = -1;
    m_LookTravelPx = 0.0f;
    if (m_LookLeftDown) {
        MouseButtonReleasedEvent ev(kAttackButton);
        m_Emit(ev);
        m_LookLeftDown = false;
    }
}

} // namespace GE
