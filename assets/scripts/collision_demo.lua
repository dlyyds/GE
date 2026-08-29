-- collision_demo.lua —— 物理碰撞事件演示：接触/传感器事件钩子
-- 使用方式：给带刚体的实体挂本脚本，场景里放另一个带刚体的物体 / IsSensor 触发器
-- 点编辑器 Play，看日志验证：Enter/Exit 序列、双侧都收到、冲量非零、Stay 按需开启
local M = {}

function M.OnCreate(self)
    self.enterCount = 0
end

-- 普通碰撞进入：otherTag=对端 Tag，impulse=法向冲量，normal=法线（指向自己）
function M.OnCollisionEnter(self, otherTag, impulse, nx, ny, nz)
    self.enterCount = self.enterCount + 1
    log.info(string.format("collision[%s]: Enter %s，冲量 %.2f，法线 (%.2f, %.2f, %.2f)（第 %d 次）",
                           entity.get_tag(), otherTag, impulse, nx, ny, nz, self.enterCount))
end

function M.OnCollisionExit(self, otherTag)
    log.info(string.format("collision[%s]: Exit %s", entity.get_tag(), otherTag))
end

-- Stay 高频通道：写了才启用（未写则引擎不收集 Persisted，零开销）
function M.OnCollisionStay(self, otherTag)
    log.info(string.format("collision[%s]: Stay %s", entity.get_tag(), otherTag))
end

-- 传感器（IsSensor 体）事件
function M.OnTriggerEnter(self, otherTag)
    log.info(string.format("collision[%s]: TriggerEnter %s", entity.get_tag(), otherTag))
end

function M.OnTriggerExit(self, otherTag)
    log.info(string.format("collision[%s]: TriggerExit %s", entity.get_tag(), otherTag))
end

return M