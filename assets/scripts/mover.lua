-- mover.lua —— Lua 脚本示例：WASD 平移 / 持续旋转 / 空格日志 / public 字段演示
-- 引擎注入的全局 API：transform.* (局部 TRS, 欧拉角=度) / input.* (输入快照查询)
--                  / log.* / entity.* / public.* (编辑器可调字段) / Key.* / Mouse.*
local M = {}

-- public 字段声明：编辑器面板据此生成输入控件，值随场景序列化落盘，脚本经 public.get 实时读本实体值
M.PUBLIC_FIELDS = {
    speed        = { type = "number", default = 2.0 },
    auto_rotate  = { type = "bool",   default = true },
    label        = { type = "string", default = "mover" },
}

function M.OnCreate(self)
    -- self 字段仍按实体独立且热重载保留（换逻辑不换状态）；public.* 是编辑器可调配置
    self.spinAccum = 0.0
end

function M.OnUpdate(self, ts)
    -- ts 单位秒；public 字段缺省时用 or 兜底（面板尚未填值时取默认）
    local speed = public.get("speed") or 2.0
    local label = public.get("label") or "mover"

    -- WASD 沿局部 XYZ 平移
    local x, y, z = transform.get_translation()
    local dx, dy, dz = 0, 0, 0
    if input.is_held(Key.W) then dz = dz - speed * ts end
    if input.is_held(Key.S) then dz = dz + speed * ts end
    if input.is_held(Key.A) then dx = dx - speed * ts end
    if input.is_held(Key.D) then dx = dx + speed * ts end
    transform.set_translation(x + dx, y + dy, z + dz)

    -- auto_rotate 为真时持续绕自身 Z 轴旋转（四元数右乘增量，避免欧拉回读）
    if public.get("auto_rotate") then
        self.spinAccum = self.spinAccum + ts
        transform.rotate_local(0.0, 0.0, 1.0, 45.0 * ts)
    end

    if input.just_pressed(Key.Space) then
        log.info(string.format("mover [%s]: SPACE 按下, speed=%.1f, tag=%s",
                               label, speed, entity.get_tag()))
    end
end

function M.OnDestroy(self)
    log.info("mover: destroyed")
end

return M