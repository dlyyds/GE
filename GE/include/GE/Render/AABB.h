/**
 * @file AABB.h
 * @brief 轴对齐包围盒（AABB）—— 轻量几何摘要，供视锥/阴影剔除做 CPU 侧相交判定。
 *
 * 独立成头文件：AABB 定义不含任何 Vulkan 依赖，独立后 Scene.h 等核心头可直接引用，
 * 不必连带引入 Render/Mesh.h（顶点缓冲 / 设备句柄 / vulkan.hpp 的重头）。
 */

#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

namespace GE {

/**
 * @brief 轴对齐包围盒（AABB），以最小/最大角表示。
 *
 * 网格装配（BuildMesh / InstallAsyncData）时从顶点位置一次性算出，作为轻量摘要
 * 随 Mesh 常驻，Mesh 自身不持有整份 CPU 顶点数组。初始为「空盒」状态
 * （min 极大 / max 极小，IsValid()==false），未并入任何点前表示无效。
 */
struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};  ///< 最小角
    glm::vec3 max{-std::numeric_limits<float>::max()}; ///< 最大角

    /// 将坐标点并入包围盒（逐分量取 min/max）
    void Expand(const glm::vec3 &p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }

    /// 包围盒是否有效（至少并入过一个点）
    bool IsValid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    /**
     * @brief 将本地（模型）空间 AABB 变换为世界空间 AABB。
     *
     * center/extent 法：用矩阵上三角 3x3（旋转缩放）逐列取绝对值加权 extent，
     * 再叠加平移，正确包住旋转 + 非均匀缩放后的盒子（法向翻转被绝对值吸收）。
     * 用于视锥剔除等需要世界空间包围盒的场景。
     *
     * @param m 模型/世界变换矩阵（列主序）
     */
    AABB Transformed(const glm::mat4 &m) const {
        const glm::mat3 m3 = glm::mat3(m);
        const glm::vec3 center = (min + max) * 0.5f;
        const glm::vec3 extent = (max - min) * 0.5f;
        const glm::vec3 e0{std::fabs(m3[0].x), std::fabs(m3[0].y), std::fabs(m3[0].z)};
        const glm::vec3 e1{std::fabs(m3[1].x), std::fabs(m3[1].y), std::fabs(m3[1].z)};
        const glm::vec3 e2{std::fabs(m3[2].x), std::fabs(m3[2].y), std::fabs(m3[2].z)};
        AABB out;
        out.min = glm::vec3(m[3]) + m3 * center - (e0 * extent.x + e1 * extent.y + e2 * extent.z);
        out.max = glm::vec3(m[3]) + m3 * center + (e0 * extent.x + e1 * extent.y + e2 * extent.z);
        return out;
    }

    /// 与另一 AABB 是否相交（各轴分离判反例；无效盒视为不相交）。
    /// 用于阴影剔除：物体世界 AABB 与阴影视锥（世界 AABB）判交（阴影剔除计划书 §4.1）。
    bool Overlaps(const AABB &o) const {
        if (!IsValid() || !o.IsValid()) {
            return false;
        }
        return min.x <= o.max.x && max.x >= o.min.x
            && min.y <= o.max.y && max.y >= o.min.y
            && min.z <= o.max.z && max.z >= o.min.z;
    }
};

} // namespace GE
