-- asm_demo.lua —— ASM 输入驱动样例：速度参数 + 跳跃 trigger 脉冲 + 落地/死亡开关
-- 前提：实体挂 AnimStateMachine 组件（如 idle/walk/jump/dead 状态，含对应转换条件），
--       本脚本只按输入/按键写参数，状态机按条件自动切换 clip（见 docs/动画状态机ASM计划书.md）。
-- 引擎注入：anim.set / anim.set_bool / anim.trigger / anim.get（ASM 参数表，计划书 §2.2）
--           input.* / log.* / Key.* / entity.* / transform.*
local M = {}

function M.OnCreate(self)
    anim.set("speed", 0.0)
    anim.set_bool("on_ground", true)
    anim.set_bool("dead", false)
    self.grounded = true -- 地面状态本地镜像（anim 无 get_bool，自行记录一份）
    self.t = 0.0
end

function M.OnUpdate(self, ts)
    self.t = self.t + ts

    -- 速度参数：任一移动键 → 1，否则 0（状态机按 speed>0.5 在 idle/walk 间切换）
    local moving = input.is_held(Key.W) or input.is_held(Key.S)
               or input.is_held(Key.A) or input.is_held(Key.D)
    anim.set("speed", moving and 1.0 or 0.0)

    -- 跳跃：落地才可跳；空格 → trigger 一次性脉冲（求值读到即消费，状态机据此跳一次）
    if input.just_pressed(Key.Space) and self.grounded then
        anim.trigger("jump")
        self.grounded = false
    end
    -- 模拟 1s 后"落地"复位（on_ground false→true，触发落地转换/驻留条件）
    if not self.grounded and self.t % 2.0 < 0.2 then
        self.grounded = true
    end
    anim.set_bool("on_ground", self.grounded)

    -- 死亡演示：K 键置 dead=true（配 ANY→dead 转换；一次性状态可配 StateEnded 播完回 idle）
    if input.just_pressed(Key.K) then
        anim.set_bool("dead", true)
    end
end

function M.OnAnimationEvent(self, name)
    -- anim.get 未命中的 float 参数名回退到当前状态驻留时长 stateTime（计划书 2.2）
    log.info(string.format("asm_demo: 事件 %s @ stateTime %.2fs", name, anim.get("state_time")))
end

return M