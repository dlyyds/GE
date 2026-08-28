-- mover.lua —— Lua 脚本示例：WASD 平移 / R 旋转 / 空格日志
-- 引擎注入的全局 API：transform.* (局部 TRS, 欧拉角=度) / input.* (输入快照查询)
--                  / log.* / entity.* / Key.* / Mouse.*
local M = {}

function M.OnCreate(self)
    self.speed = 2.0          -- 每实体独立字段（存实例表，共享函数在行为表）
end

function M.OnUpdate(self, ts)
    -- ts 单位秒；WASD 沿局部 XYZ 平移
    local x, y, z = transform.get_translation()
    local dx, dy, dz = 0, 0, 0
    if input.is_held(Key.W) then dz = dz - self.speed * ts end
    if input.is_held(Key.S) then dz = dz + self.speed * ts end
    if input.is_held(Key.A) then dx = dx - self.speed * ts end
    if input.is_held(Key.D) then dx = dx + self.speed * ts end
    transform.set_translation(x + dx, y + dy, z + dz)

    -- R 按下单帧边沿：绕 Y 转 90°
    if input.just_pressed(Key.R) then
        local rx, ry, rz = transform.get_rotation()
        transform.set_rotation(rx, ry + 90.0, rz)
        log.info("mover: rotate by 90deg, tag=" .. entity.get_tag())
    end

    -- 空格按下打日志（边沿只触发一帧）
    if input.just_pressed(Key.Space) then
        log.info("mover: SPACE pressed")
    end
end

function M.OnDestroy(self)
    log.info("mover: destroyed")
end

return M