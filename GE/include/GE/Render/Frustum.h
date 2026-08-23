/**
 * @file Frustum.h
 * @brief 视锥（6 平面边界）与视锥剔除 —— 纯 CPU 几何判定工具。
 *
 * 从 viewProjection 裁剪矩阵提取 6 个平面（Gribb-Hartmann 方法），对世界空间
 * AABB 做「完全在外」判定：AABB 完全位于某一平面外侧（最近角也在外）才判
 * 不可见，其余情况（完全在内 / 相交）保守保留。平面未归一化——符号判定
 * 不受均匀缩放影响，省去除法。
 *
 * 深度/NDC 约定与相机一致：投影由 glm::perspective 生成（GL 风格 z∈[-1,1]），
 * 未定义 GLM_FORCE_DEPTH_ZERO_TO_ONE，故裁剪体积为 clip 立方体 [-w,w]^3，
 * 近/远平面取 row3±row2。
 */

#pragma once

#include "Render/Mesh.h"

#include <glm/glm.hpp>

namespace GE {

/**
 * @brief 视锥：6 个平面（法线朝内），平面方程 Ax+By+Cz+D=0（w 存 D）。
 */
class Frustum {
public:
    enum PlaneIndex : int {
        kLeft = 0,
        kRight,
        kBottom,
        kTop,
        kNear,
        kFar,
        kPlaneCount,
    };

    /**
     * @brief 由裁剪矩阵（viewProjection = projection * view）提取视锥。
     *
     * Gribb-Hartmann：取矩阵三行（列主序 m[col][row]），第 3 行 ± 第 i 行
     * 得对应 clip 平面方程。平面法线朝内。
     */
    static Frustum FromViewProjection(const glm::mat4 &viewProj) {
        const glm::vec4 row0{viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]};
        const glm::vec4 row1{viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]};
        const glm::vec4 row2{viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]};
        const glm::vec4 row3{viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]};

        Frustum f;
        f.m_Planes[kLeft]   = row3 + row0;
        f.m_Planes[kRight]  = row3 - row0;
        f.m_Planes[kBottom] = row3 + row1;
        f.m_Planes[kTop]    = row3 - row1;
        f.m_Planes[kNear]   = row3 + row2;
        f.m_Planes[kFar]    = row3 - row2;
        return f;
    }

    /**
     * @brief 世界空间 AABB 对当前视锥的可见性（保守判定）。
     *
     * 对每个平面取 AABB 沿其法线正方向最远角（p-vertex）：若该角仍在平面
     * 内侧（Ax+By+Cz+D >= 0），则整个 AABB 不可能完全在平面外；仅当某平面
     * 上连最远角都在外时判为不可见。
     *
     * @param aabb 世界空间轴对齐包围盒
     * @return true = 在视锥内或与其相交（应绘制）；false = 完全在视锥外（可剔除）
     */
    bool IsVisible(const AABB &aabb) const {
        for (int i = 0; i < kPlaneCount; ++i) {
            const glm::vec4 &p = m_Planes[i];
            const glm::vec3 pv{
                p.x >= 0.0f ? aabb.max.x : aabb.min.x,
                p.y >= 0.0f ? aabb.max.y : aabb.min.y,
                p.z >= 0.0f ? aabb.max.z : aabb.min.z,
            };
            if (glm::dot(pv, glm::vec3(p)) + p.w < 0.0f) {
                return false;
            }
        }
        return true;
    }

private:
    glm::vec4 m_Planes[kPlaneCount]; ///< 6 个平面（未归一化，法线朝内）
};

} // namespace GE