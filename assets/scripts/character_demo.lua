-- character_demo.lua —— Lua 脚本示例：角色控制器 WASD 平移 + 空格跳跃
-- 前置：实体须挂 CharacterControllerComponent（不挂 RigidBodyComponent），
--       引擎每物理子步积分重力并驱动 Jolt CharacterVirtual（自动上楼/贴地吸附）。
-- 脚本只负责水平输入：character.set_move(x,z) 是【世界空间】水平向，
--       垂直速度由引擎积分，脚本不管 y。
-- 引擎注入的全局 API：character.*（角色输入/查询） / input.* / public.* / log.*
local M = {}

-- public 字段声明：speed 为水平移动速度（m/s），编辑面板可调、随场景落盘
M.PUBLIC_FIELDS = {
    speed = { type = "number", default = 4.0 },
}

function M.OnUpdate(self, ts)
    local speed = public.get("speed") or 4.0

    -- WASD → 水平期望速度（世界空间；相机相对移动见计划书 §4，不在本次范围）
    local dx, dz = 0, 0
    if input.is_held(Key.W) then dz = dz - 1 end
    if input.is_held(Key.S) then dz = dz + 1 end
    if input.is_held(Key.A) then dx = dx - 1 end
    if input.is_held(Key.D) then dx = dx + 1 end
    character.set_move(dx * speed, dz * speed)

    -- 空格起跳：仅贴地时生效（character.jump 已做贴地判定，空中按压不缓冲）
    if input.just_pressed(Key.Space) then
        character.jump()
    end
end

return M