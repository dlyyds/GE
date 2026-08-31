-- character_asm_demo.lua —— 角色控制器 + 动画状态机(ASM)联动样例
-- 前置：同一根实体挂 CharacterControllerComponent + AnimStateMachineComponent +
--       AnimationComponent + ScriptComponent（不挂 RigidBodyComponent）。
-- 两条桥的分工：
--   character.* → 角色控制器：水平期望速度 set_move + 起跳 jump / 回读真实状态 get_*
--   anim.*      → ASM 参数表：floats(速度)、bools(贴地)、triggers(跳跃脉冲)
--   ASM 求值期按条件自动切 clip（idle/walk/jump/fall/land）。
-- 配套 ASM 配置见 docs/动画状态机ASM计划书.md（可参考其中的示例状态/转换条件）。
local M = {}

-- public 字段：speed 为水平移动速度（m/s）
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

    -- WASD → 水平期望速度（世界空间；垂直速度由引擎积分，脚本不管 y）
    local dx, dz = 0, 0
    if input.is_held(Key.W) then dz = dz - 1 end
    if input.is_held(Key.S) then dz = dz + 1 end
    if input.is_held(Key.A) then dx = dx - 1 end
    if input.is_held(Key.D) then dx = dx + 1 end
    character.set_move(dx * speed, dz * speed)

    -- 跳跃：仅 character.jump() 成功（贴地）才发 ASM 脉冲，空中误触不跳动效
    if input.just_pressed(Key.Space) then
        if character.jump() then
            anim.trigger("jump")
        end
    end

    -- 速度参数：取真实水平合速度（顶墙被挡时物理速度≈0 → 动画回 idle）
    local vx, vy, vz = character.get_velocity()
    local hspd = math.sqrt(vx * vx + vz * vz)
    anim.set("speed", hspd)

    -- 贴地态：直接回读组件（引擎每物理子步写回）
    anim.set_bool("on_ground", character.get_grounded())
end

return M