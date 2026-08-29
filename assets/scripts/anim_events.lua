-- anim_events.lua —— 动画事件演示：跨过时间轴事件点时触发 OnAnimationEvent
-- 使用方式：给带 AnimationComponent 的实体挂本脚本 → 在动画面板事件表放若干事件
-- （时间 + 名字，如 "footstep"/"hit"）→ 播放，看日志验证：
--   次数/时间精确、loop 回绕不重不漏、暂停/拖 Scrubber/负速不触发、保存→加载保留
local M = {}

M.PUBLIC_FIELDS = {
    prefix = { type = "string", default = "anim" },
}

function M.OnCreate(self)
    self.count = 0
end

-- 动画事件钩子（约定命名钩子：写了即订阅，没写引擎跳过）。参数 name = 事件名
function M.OnAnimationEvent(self, name)
    self.count = self.count + 1
    log.info(string.format("anim_events[%s]: 事件 '%s' 命中（第 %d 次）",
                           public.get("prefix") or "anim", name, self.count))
end

function M.OnUpdate(self, ts)
    -- 本脚本仅演示事件钩子，无常驻逻辑
end

return M