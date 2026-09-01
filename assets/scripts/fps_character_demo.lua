-- fps_character_demo.lua —— 第一人称跟随角色示例：鼠标视角 + 相机相对 WASD 移动 + ASM 动画
-- 前置：
--   角色实体：TransformComponent + CharacterControllerComponent +
--             FirstPersonCameraComponent + AnimStateMachineComponent +
--             AnimationComponent（不挂 RigidBodyComponent）
--   相机实体：CameraComponent（Primary = true）；由引擎 UpdateFirstPersonCamera
--             每帧把相机钉到角色视点、把相机朝向写回角色朝向（FacingYaw 通道）。
-- 分工：
--   camera.*  → 主相机只读查询（视角由引擎每帧从鼠标增量更新，脚本只读 yaw 旋转输入）
--   character.* → 角色控制器：水平期望速度 set_move + 起跳 jump / 回读 get_*
--   anim.*    → ASM 参数表：floats(速度)、bools(贴地)、triggers(跳跃脉冲)
--   引擎侧 UpdateFirstPersonCamera 负责"脸朝相机 + 位置跟随"，脚本只管移动方向。
local M = {}

-- public 字段：speed 为水平移动速度（m/s），编辑面板可调、随场景落盘
M.PUBLIC_FIELDS = {
    speed = { type = "number", default = 4.0 },
}

function M.OnCreate(self)
    -- 初值：ASM 参数表随创建重填（不序列化）；贴地态先按"脚底着地"假设
    anim.set("speed", 0.0)
    anim.set_bool("on_ground", true)
end

function M.OnUpdate(self, ts)
    local speed = public.get("speed") or 4.0

    -- 相机 yaw（度）：引擎每帧从鼠标增量更新，脚本只读
    local yaw = camera.get_yaw()
    local ry = math.rad(yaw)

    -- WASD 输入（世界轴：W = -Z）
    local ix, iz = 0, 0
    if input.is_held(Key.W) then iz = iz - 1 end
    if input.is_held(Key.S) then iz = iz + 1 end
    if input.is_held(Key.A) then ix = ix - 1 end
    if input.is_held(Key.D) then ix = ix + 1 end

    -- 绕 Y 旋转到相机朝向系：W/S 沿相机前向，A/D 沿相机右向。
    -- 前向 = (-sin yaw, 0, -cos yaw)，右向 = (cos yaw, 0, -sin yaw)
    local fx, fz = -math.sin(ry), -math.cos(ry)
    local rx, rz =  math.cos(ry), -math.sin(ry)
    local wx = fx * (-iz) + rx * ix
    local wz = fz * (-iz) + rz * ix

    -- 归一化并缩放（斜向移动不加速）
    local len = math.sqrt(wx * wx + wz * wz)
    if len > 0.0001 then
        wx, wz = wx / len * speed, wz / len * speed
    end
    character.set_move(wx, wz)


    -- 动画：ASM 参数驱动（同 character_asm_demo 约定）
    -- speed   = 真实水平合速度（顶墙被挡时物理速度≈0 → 动画回 idle）
    -- on_ground = 贴地态（引擎每物理子步回写）
    -- jump    = 一次性脉冲，仅贴地起跳成功才发（空中误触不跳动效）
    local vx, vy, vz = character.get_velocity()
    local hspd = math.sqrt(vx * vx + vz * vz)
    anim.set("speed", hspd)
    anim.set_bool("on_ground", character.get_grounded())

    -- 空格起跳：仅贴地时生效（character.jump 已做贴地判定，空中按压不缓冲）
    if input.just_pressed(Key.Space) then
        if character.jump() then
            anim.trigger("jump")
        end
    end
end

return M
