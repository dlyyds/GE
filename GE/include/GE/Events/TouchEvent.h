#pragma once

#include "Event.h"

#include <cstdint>
#include <sstream>

namespace GE {

/**
 * @brief 触摸事件基类（手指按下 / 移动 / 抬起）。
 *
 * ## 两条接口约定，都是刻意选的
 *
 * 1. **坐标是归一化的 `[0,1]`** —— 直接沿用 SDL 的 `SDL_FingerEvent.x/y` 语义，不做像素转换。
 *    这样触摸几何（摇杆圆心、半径、死区、拖拽位移）全部与窗口尺寸和屏幕密度无关：
 *    运行时只要在**绘制时**乘一次 `io.DisplaySize` 就与手指落点几何一致（两者都是"
 *    占屏幕的比例"）。若在这里换成像素，hidpi 设备上 SDL 的归一化基数（逻辑尺寸）与
 *    引擎拿到的像素尺寸不一致，摇杆会被画到手指旁边。
 *
 * 2. **只带 PointerId 与位置，不带增量** —— 增量由消费者自己求差。原因：同一帧内可能有
 *    多根手指在动，增量是"相对上一次同 id 事件"的差值，只有在消费者按 id 维护状态时才
 *    算得对；在事件类里预算是替消费者猜。
 *
 * `PointerId` 就是 SDL 的 `SDL_FingerID`。它存在的意义只有一个但很关键：**多指仲裁**
 * ——运行时要把"左手按摇杆"与"右手拖视角"分开，靠的就是这个 id，而合成鼠标（SDL 的
 * 触摸→鼠标转换）只有一个指针，做不到这件事。
 */
class TouchEvent : public Event {
public:
    inline int64_t GetPointerId() const { return m_PointerId; }

    inline float GetX() const { return m_X; }

    inline float GetY() const { return m_Y; }

    EVENT_CLASS_CATEGORY(EventCategoryTouch | EventCategoryInput)

protected:
    TouchEvent(int64_t pointerId, float x, float y) : m_PointerId(pointerId), m_X(x), m_Y(y) {
    }

    int64_t m_PointerId;
    float m_X, m_Y;
};

class TouchPressedEvent : public TouchEvent {
public:
    TouchPressedEvent(int64_t pointerId, float x, float y) : TouchEvent(pointerId, x, y) {
    }

    std::string ToString() const override {
        std::stringstream ss;
        ss << "TouchPressedEvent: id=" << m_PointerId << ", " << m_X << ", " << m_Y;
        return ss.str();
    }

    EVENT_CLASS_TYPE(TouchPressed)
};

class TouchMovedEvent : public TouchEvent {
public:
    TouchMovedEvent(int64_t pointerId, float x, float y) : TouchEvent(pointerId, x, y) {
    }

    std::string ToString() const override {
        std::stringstream ss;
        ss << "TouchMovedEvent: id=" << m_PointerId << ", " << m_X << ", " << m_Y;
        return ss.str();
    }

    EVENT_CLASS_TYPE(TouchMoved)
};

/**
 * @brief 手指抬起 / 被系统取消。**两者都映射到本事件**，见 `SdlWindow::HandleEvent`。
 *
 * 系统取消（来电、手势被系统接管等）必须走这条路径，否则那根手指合成的按键永远
 * 收不到 release —— 表现为"角色一直往前走"这类卡键。
 */
class TouchReleasedEvent : public TouchEvent {
public:
    TouchReleasedEvent(int64_t pointerId, float x, float y) : TouchEvent(pointerId, x, y) {
    }

    std::string ToString() const override {
        std::stringstream ss;
        ss << "TouchReleasedEvent: id=" << m_PointerId << ", " << m_X << ", " << m_Y;
        return ss.str();
    }

    EVENT_CLASS_TYPE(TouchReleased)
};

} // namespace GE
