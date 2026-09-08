-- fps_character_combat_demo.lua —— 第一人称角色示例（增加按 Shift 加速 + 左键普通攻击）
-- 基于 assets/scripts/fps_character_demo.lua 扩展。
-- 前置（与 fps_character_demo.lua 相同）：
--   角色实体：TransformComponent + CharacterControllerComponent +
--             FollowCameraComponent + AnimStateMachineComponent +
--             AnimationComponent（不挂 RigidBodyComponent）
--   相机实体：CameraComponent（Primary = true）；由引擎 UpdateFollowCamera
--             每帧把相机钉到角色视点、把相机朝向写回角色朝向（FacingYaw 通道）。
-- 新增行为：
--   * 按住 Shift（左/右均可）→ 移动速度提升为 sprint_speed（冲刺加速）
--   * 鼠标左键按下一次 → 触发一次普通攻击（anim.trigger("attack")）
-- 分工：
--   camera.*  → 主相机只读查询（视角由引擎每帧从鼠标增量更新，脚本只读 yaw 旋转输入）
--   character.* → 角色控制器：水平期望速度 set_move + 起跳 jump / 回读 get_*
--   anim.*    → ASM 参数表：floats(速度)、bools(贴地/冲刺)、triggers(跳跃/攻击脉冲)
--   引擎侧 UpdateFollowCamera 负责"脸朝相机 + 位置跟随"，脚本只管移动方向。
local M = {}

-- public 字段：speed 为普通水平速度，sprint_speed 为按住 Shift 时的冲刺速度（m/s）
M.PUBLIC_FIELDS = {
    speed = { type = "number", default = 4.0 },
    sprint_speed = { type = "number", default = 7.0 },
    -- 冲刺满速时 walk 音频的播放倍速（miniaudio pitch 即播放速率；2.0 = 二倍速）
    sprint_playback_speed = { type = "number", default = 2.0 },
}

function M.OnCreate(self)
    -- 初值：ASM 参数表随创建重填（不序列化）
    anim.set("speed", 0.0)
    anim.set_bool("on_ground", true)
    anim.set_bool("sprinting", false)
end

function M.OnUpdate(self, ts)
    local speed = public.get("speed") or 4.0
    local sprint_speed = public.get("sprint_speed") or 7.0
    local sprint_playback_speed = public.get("sprint_playback_speed") or 2.0

    -- 相机 yaw（度）：引擎每帧从鼠标增量更新，脚本只读
    local yaw = camera.get_yaw()
    local ry = math.rad(yaw)

    -- WASD 输入（世界轴：W = -Z）
    local ix, iz = 0, 0
    if input.is_held(Key.W) then
        iz = iz - 1
    end
    if input.is_held(Key.S) then
        iz = iz + 1
    end
    if input.is_held(Key.A) then
        ix = ix - 1
    end
    if input.is_held(Key.D) then
        ix = ix + 1
    end

    -- Shift 冲刺：按住左/右 Shift 之一即加速
    local sprinting = input.is_held(Key.LeftShift) or input.is_held(Key.RightShift)
    local move_speed = sprinting and sprint_speed or speed
    anim.set_bool("sprinting", sprinting)

    -- 绕 Y 旋转到相机朝向系：W/S 沿相机前向，A/D 沿相机右向。
    -- 前向 = (-sin yaw, 0, -cos yaw)，右向 = (cos yaw, 0, -sin yaw)
    local fx, fz = -math.sin(ry), -math.cos(ry)
    local rx, rz = math.cos(ry), -math.sin(ry)
    local wx = fx * (-iz) + rx * ix
    local wz = fz * (-iz) + rz * ix

    -- 归一化并缩放（斜向移动不加速）
    local len = math.sqrt(wx * wx + wz * wz)
    if len > 0.0001 then
        wx, wz = wx / len * move_speed, wz / len * move_speed
    else
        wx, wz = 0.0, 0.0
    end
    character.set_move(wx, wz)

    -- 动画参数：真实水平合速度 / 贴地态 / 跳跃 / 攻击
    local vx, vy, vz = character.get_velocity()
    local hspd = math.sqrt(vx * vx + vz * vz)
    anim.set("speed", hspd)
    anim.set_bool("on_ground", character.get_grounded())

    -- 移动脚步音频：贴地且水平合速度超过阈值才播 walk（脚步语义），停住即停播；
    -- 倍速二态：走路 1.0 正常播放，按住 Shift 冲刺时调快为 sprint_playback_speed（默认 2.0 = 二倍速）。
    -- miniaudio 的 pitch 即播放速率（重采样实现，倍速越高脚步越快、音调略升）。
    -- 倍速只在变化时才下发，避免每帧冗余调用。
    -- audio.* 作用于本实体 AudioSource 组件的 slot（按名字解析到 "walk" 槽位）。
    local walking = character.get_grounded() and hspd > 0.5
    local walk_pitch = 1.0
    if walking and sprinting then
        walk_pitch = sprint_playback_speed
    end
    if walking and not audio.is_playing("walk") then
        audio.play("walk")
        audio.set_loop("walk", true)
        audio.set_pitch("walk", walk_pitch)
        self._walk_pitch = walk_pitch
    elseif walking and audio.is_playing("walk") and self._walk_pitch ~= walk_pitch then
        audio.set_pitch("walk", walk_pitch) -- 走路/跑步切换时实时更新倍速
        self._walk_pitch = walk_pitch
        log.info("walk_pitch = {0}", walk_pitch)
    elseif not walking and audio.is_playing("walk") then
        audio.stop("walk")
        self._walk_pitch = nil
    end

    -- 空格起跳：仅贴地时生效（character.jump 已做贴地判定，空中按压不缓冲）
    if input.just_pressed(Key.Space) then
        if character.jump() then
            anim.trigger("jump")
        end
    end

    -- 普通攻击：鼠标左键按下一次触发一次攻击脉冲
    if input.just_mouse_pressed(Mouse.ButtonLeft) then
        anim.trigger("attack")
        log.info("fps_character_combat_demo: 普通攻击")
    end
end

return M