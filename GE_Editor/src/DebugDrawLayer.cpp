#include "DebugDrawLayer.h"

#include "GE/Render/Mesh.h"
#include "GE/Scene/Components.h"
#include "GE/Scene/Entity.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace GE {

namespace {

/**
 * @brief 世界坐标 → 视口屏幕坐标（ImGui 像素，Y 向下）。
 *
 * 与 GizmoController 同约定：传入 OpenGL 语义投影（投影 [1][1] 未做 Vulkan 翻转），
 * 屏幕映射 sy = (0.5 - ndc.y*0.5)*h 与 ImGuizmo worldToPos 的 y=1-y 一致，
 * 保证包围盒线与画面 / gizmo 精确对齐。
 *
 * @return false = 角点在相机背面（clip.w<=0），调用方按边跳过，避免投影发散
 */
bool ProjectWorldToScreen(const glm::mat4 &viewProjGL, const glm::vec3 &world,
                          const glm::vec2 &origin, const glm::vec2 &size, glm::vec2 &out) {
    const glm::vec4 clip = viewProjGL * glm::vec4(world, 1.0f);
    if (clip.w <= 0.0f) {
        return false; // 相机背面
    }
    const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
    out = glm::vec2(origin.x + (0.5f + ndc.x * 0.5f) * size.x,
                    origin.y + (0.5f - ndc.y * 0.5f) * size.y);
    return true;
}

} // namespace

DebugDrawLayer::DebugDrawLayer(std::shared_ptr<EditorContext> context)
    : Layer("DebugDrawLayer"), m_Context(std::move(context)) {
}

DebugDrawLayer::~DebugDrawLayer() = default;

void DebugDrawLayer::OnAttach() {
}

void DebugDrawLayer::OnDetach() {
}

void DebugDrawLayer::OnUpdate(Timestep &ts) {
}

void DebugDrawLayer::OnEvent(Event &event) {
}

void DebugDrawLayer::OnImGuiRender() {
    ImGui::SetNextWindowDockID(ImGui::GetID("MainDockspace"), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene Debug");
    ImGui::Checkbox("显示包围盒", &m_ShowBounds);
    ImGui::Checkbox("关节露点", &m_ShowJointDots);
    ImGui::Checkbox("显示碰撞体", &m_ShowColliders);
    ImGui::Checkbox("显示视点", &m_ShowFPSEyes);
    ImGui::Checkbox("显示方向光", &m_ShowDirLights);
    ImGui::End();
}

void DebugDrawLayer::RenderSceneOverlay(const Camera &camera, const glm::vec2 &imagePos,
                                        const glm::vec2 &viewportSize) {
    if (!m_Context->Scene) {
        return;
    }

    if (m_ShowBounds) {
        DrawWorldBounds(camera, imagePos, viewportSize);
    }
    if (m_ShowColliders) {
        DrawColliders(camera, imagePos, viewportSize);
    }
    if (m_ShowFPSEyes) {
        DrawFirstPersonEyes(camera, imagePos, viewportSize);
    }
    if (m_ShowDirLights) {
        DrawDirectionalLights(camera, imagePos, viewportSize);
    }
}

// ============================================================
// 包围盒线框叠加
// ============================================================

void DebugDrawLayer::DrawWorldBounds(const Camera &camera, const glm::vec2 &imagePos,
                                     const glm::vec2 &viewportSize) {
    if (!m_Context->Scene) {
        return;
    }

    // 还原 OpenGL 投影（渲染/剔除用 Vulkan Y 翻转投影；ImGuizmo 同款还原保证对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{viewportSize.x, viewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 立方体 12 条边：0/1/2/3 = z=min 面，4/5/6/7 = z=max 面
    static const int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };

    // 画一个世界空间 AABB 的 12 条边线框（供网格盒与实体盒共用）
    const auto drawWorldAabb = [&](const AABB &world, ImU32 color) {
        const glm::vec3 c[8] = {
            {world.min.x, world.min.y, world.min.z},
            {world.max.x, world.min.y, world.min.z},
            {world.min.x, world.max.y, world.min.z},
            {world.max.x, world.max.y, world.min.z},
            {world.min.x, world.min.y, world.max.z},
            {world.max.x, world.min.y, world.max.z},
            {world.min.x, world.max.y, world.max.z},
            {world.max.x, world.max.y, world.max.z},
        };
        glm::vec2 scr[8];
        bool front[8] = {};
        for (int i = 0; i < 8; ++i) {
            front[i] = ProjectWorldToScreen(viewProjGL, c[i], origin, size, scr[i]);
        }
        for (int e = 0; e < 12; ++e) {
            const int a = kEdges[e][0], b = kEdges[e][1];
            // 边跨相机背面则不画（避免投影发散）；两端都在正面才连线
            if (front[a] && front[b]) {
                dl->AddLine(ImVec2(scr[a].x, scr[a].y), ImVec2(scr[b].x, scr[b].y), color, 1.2f);
            }
        }
    };

    // 1) 网格级盒：仅静态网格灰蓝（蒙皮绑定盒覆盖不了动画变形，且红显易误导，
    //    其可靠表示统一由下方的 BoundingBoxComponent 黄盒承担）
    auto meshView = m_Context->Scene->Reg().view<TransformComponent, MeshRendererComponent>();
    const ImU32 meshColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f, 0.60f, 0.70f, 1.0f));
    for (auto entity : meshView) {
        const auto &tc = meshView.get<TransformComponent>(entity);
        const auto &mc = meshView.get<MeshRendererComponent>(entity);
        if (!mc.MeshPtr) {
            continue;
        }
        // 蒙皮实体不画（无可靠盒可示），改由用户手动摆放的黄盒表示
        if (m_Context->Scene->Reg().try_get<SkinComponent>(entity)) {
            continue;
        }
        const AABB &local = mc.MeshPtr->GetAABB();
        if (!local.IsValid()) {
            continue;
        }
        drawWorldAabb(local.Transformed(tc.GetWorldMatrix()), meshColor);
    }

    // 2) 实体级手动摆放盒（BoundingBoxComponent）用黄叠出，便于对照剔除覆盖范围。
    //    额外两层可视化解决「单方向看不出是否完全包围」：
    //      a. 半透明盒面（前亮后暗），直读前后重叠与深度关系；
    //      b. 盒覆盖子树的全部蒙皮关节投影点（盒内绿 / 盒外红），一眼见露骨。
    auto boundsView = m_Context->Scene->Reg().view<TransformComponent, BoundingBoxComponent>();
    const ImU32 bbColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 1.0f));
    const ImU32 bbFaceFront = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 0.16f));
    const ImU32 bbFaceBack = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.80f, 0.10f, 0.05f));
    const glm::vec3 camPos = camera.GetPosition();

    // 世界空间 AABB 的 6 个面（下标引用 cW[8] 角点 + 向外法线）
    struct BoxFace {
        int idx[4];
        glm::vec3 n;
    };
    static const BoxFace kBoxFaces[6] = {
        {{4, 5, 7, 6}, {0.0f, 0.0f, 1.0f}}, // +Z
        {{0, 1, 3, 2}, {0.0f, 0.0f, -1.0f}}, // -Z
        {{2, 3, 7, 6}, {0.0f, 1.0f, 0.0f}}, // +Y
        {{0, 1, 5, 4}, {0.0f, -1.0f, 0.0f}}, // -Y
        {{1, 3, 7, 5}, {1.0f, 0.0f, 0.0f}}, // +X
        {{0, 2, 6, 4}, {-1.0f, 0.0f, 0.0f}}, // -X
    };

    // 蒙皮关节查重（多个盒共享同一子树时避免重复画点）
    std::unordered_set<entt::entity> jointDrawn;

    for (auto entity : boundsView) {
        const auto &tc = boundsView.get<TransformComponent>(entity);
        const auto &bb = boundsView.get<BoundingBoxComponent>(entity);
        if (!bb.IsValid()) {
            continue; // 未摆放，不参与剔除也不画
        }
        AABB local;
        local.min = bb.minCorner();
        local.max = bb.maxCorner();
        const AABB world = local.Transformed(tc.GetWorldMatrix());

        // a. 半透明面：面法线朝向相机（看到的是外表面）→ 亮，背向 → 暗，
        //    区分前后两层，解决单视角下「前后两面重叠分不清」的问题
        const glm::vec3 wc[8] = {
            {world.min.x, world.min.y, world.min.z}, {world.max.x, world.min.y, world.min.z},
            {world.min.x, world.max.y, world.min.z}, {world.max.x, world.max.y, world.min.z},
            {world.min.x, world.min.y, world.max.z}, {world.max.x, world.min.y, world.max.z},
            {world.min.x, world.max.y, world.max.z}, {world.max.x, world.max.y, world.max.z},
        };
        for (const BoxFace &f : kBoxFaces) {
            glm::vec3 faceCenter(0.0f);
            ImVec2 pts[4];
            bool frontAll = true;
            for (int k = 0; k < 4; ++k) {
                const glm::vec3 &p = wc[f.idx[k]];
                faceCenter += p;
                glm::vec2 scr;
                if (!ProjectWorldToScreen(viewProjGL, p, origin, size, scr)) {
                    frontAll = false;
                    break; // 面有角点在相机背面，跳过避免投影发散
                }
                pts[k] = ImVec2(scr.x, scr.y);
            }
            if (!frontAll) {
                continue;
            }
            faceCenter *= 0.25f;
            const bool facingCam = glm::dot(f.n, camPos - faceCenter) > 0.0f;
            dl->AddConvexPolyFilled(pts, 4, facingCam ? bbFaceFront : bbFaceBack);
        }

        // 黄线框最后画（压在面上，线清晰可读）
        drawWorldAabb(world, bbColor);

        // b. 露点检查：盒覆盖子树的全部蒙皮关节（盒内绿 / 盒外红）。
        //    Skeleton 关节是世界实体（SkinDef::joints），取世界矩阵平移列即骨骼枢轴点。
        if (m_ShowJointDots) {
            std::vector<Entity> stack;
            stack.push_back(Entity(entity, m_Context->Scene.get()));
            while (!stack.empty()) {
                const Entity n = stack.back();
                stack.pop_back();
                const entt::entity h = static_cast<entt::entity>(n);
                if (const auto *sc = m_Context->Scene->Reg().try_get<SkinComponent>(h)) {
                    for (const entt::entity jh : sc->joints()) {
                        if (jointDrawn.count(jh)) {
                            continue; // 已被别的盒画过，跳过
                        }
                        jointDrawn.insert(jh);
                        const auto *jtc = m_Context->Scene->Reg().try_get<TransformComponent>(jh);
                        if (!jtc) {
                            continue;
                        }
                        const glm::vec3 jp = glm::vec3(jtc->GetWorldMatrix()[3]);
                        const bool inside = jp.x >= world.min.x && jp.x <= world.max.x
                                            && jp.y >= world.min.y && jp.y <= world.max.y
                                            && jp.z >= world.min.z && jp.z <= world.max.z;
                        glm::vec2 scr;
                        if (!ProjectWorldToScreen(viewProjGL, jp, origin, size, scr)) {
                            continue;
                        }
                        const ImU32 jc = ImGui::ColorConvertFloat4ToU32(
                            inside
                                ? ImVec4(0.20f, 0.90f, 0.30f, 1.0f) // 盒内：绿
                                : ImVec4(0.95f, 0.30f, 0.25f, 1.0f)); // 盒外：红
                        // 圆点 + 深色描边，保证在亮/暗背景上都可读
                        dl->AddCircleFilled(ImVec2(scr.x, scr.y), 4.0f, jc);
                        dl->AddCircle(ImVec2(scr.x, scr.y), 4.0f, IM_COL32(0, 0, 0, 200));
                    }
                }
                for (const auto &child : m_Context->Scene->GetChildren(n)) {
                    stack.push_back(child); // 继续下钻同棵子树
                }
            }
        }
    }
}

// ============================================================
// 物理碰撞体线框叠加
// ============================================================

void DebugDrawLayer::DrawColliders(const Camera &camera, const glm::vec2 &imagePos,
                                   const glm::vec2 &viewportSize) {
    if (!m_Context->Scene) {
        return;
    }

    // 还原 OpenGL 投影（与 DrawWorldBounds 同款，保证线与画面/gizmo 对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{viewportSize.x, viewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 立方体 12 条边（下标约定与 DrawWorldBounds 一致）
    static const int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    constexpr float kPi = 3.14159265358979f;

    // 碰撞体线框颜色（青绿，与灰网格盒/黄实体盒区分）
    const ImU32 colliderColor =
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.80f, 0.90f, 1.0f));

    // 线段 → 屏幕坐标：先裁剪到相机近平面再投影，不做相机背面裁剪。
    // 一端在相机背面时把端点裁到近平面上，整段在视线内的照常画出，
    // 避免大地这类大框在相机靠近/进入时线被整批丢弃。
    // 返回 false 表示线段完全在相机背面，无可见部分。
    const auto projectSegment = [&](const glm::vec3 &a, const glm::vec3 &b,
                                    glm::vec2 &sa, glm::vec2 &sb) -> bool {
        constexpr float kNearW = 1e-3f;
        const glm::vec4 ca = viewProjGL * glm::vec4(a, 1.0f);
        const glm::vec4 cb = viewProjGL * glm::vec4(b, 1.0f);
        const bool aFront = ca.w > kNearW;
        const bool bFront = cb.w > kNearW;
        if (!aFront && !bFront) {
            return false; // 整段在相机背面
        }
        glm::vec4 cA = ca, cB = cb;
        if (aFront && !bFront) {
            // b 在背面：沿线段插值到近平面
            const float t = (kNearW - cb.w) / (ca.w - cb.w);
            cB = cb + (ca - cb) * t;
            cB.w = kNearW;
        } else if (!aFront && bFront) {
            // a 在背面：沿线段插值到近平面
            const float t = (kNearW - ca.w) / (cb.w - ca.w);
            cA = ca + (cb - ca) * t;
            cA.w = kNearW;
        }
        const auto toPos = [&](const glm::vec4 &c) {
            return glm::vec2(origin.x + (0.5f + c.x / c.w * 0.5f) * size.x,
                             origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
        };
        sa = toPos(cA);
        sb = toPos(cB);
        return true;
    };

    // 画一条世界坐标线段（内部经过近平面裁剪）
    const auto drawWorldSegment = [&](const glm::vec3 &a, const glm::vec3 &b) {
        glm::vec2 sa, sb;
        if (projectSegment(a, b, sa, sb)) {
            dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), colliderColor, 1.2f);
        }
    };

    // 画胶囊线框：N 条经线剖面（上球帽弧 + 圆柱母线 + 下球帽弧）+ 三条圆环（±H 赤道 / y=0 中段）。
    // center 为胶囊中心，capsuleRot 为胶囊轴向旋转（沿局部 Y），radius 半径，halfHeight 圆柱段半高。
    // 刚体胶囊与角色控制器胶囊共用，保证两处线框形态一致。
    const auto drawCapsuleMesh = [&](const glm::vec3 &center, const glm::quat &capsuleRot,
                                     float radius, float halfHeight) {
        constexpr int kMeri = 8; // 经线数量
        constexpr int kArcSegs = 8; // 每条半球帽弧的分段数
        for (int m = 0; m < kMeri; ++m) {
            const float ang = (2.0f * kPi * m) / kMeri;
            const float dx = std::cos(ang), dz = std::sin(ang);
            // 上球帽弧：极点 (0, +H+R) → 赤道 (R, +H)
            glm::vec3 prev = center + capsuleRot * glm::vec3(0.0f, halfHeight + radius, 0.0f);
            for (int i = 1; i <= kArcSegs; ++i) {
                const float a = (static_cast<float>(i) / kArcSegs) * kPi * 0.5f;
                const glm::vec3 cur = center + capsuleRot * glm::vec3(
                                          std::sin(a) * radius * dx,
                                          halfHeight + std::cos(a) * radius,
                                          std::sin(a) * radius * dz);
                drawWorldSegment(prev, cur);
                prev = cur;
            }
            // 圆柱母线：下赤道 → 上赤道
            drawWorldSegment(
                center + capsuleRot * glm::vec3(radius * dx, +halfHeight, radius * dz),
                center + capsuleRot * glm::vec3(radius * dx, -halfHeight, radius * dz));
            // 下球帽弧：赤道 (R, -H) → 极点 (0, -H-R)
            glm::vec3 prev2 = center + capsuleRot * glm::vec3(radius * dx, -halfHeight, radius * dz);
            for (int i = 1; i <= kArcSegs; ++i) {
                const float a = (static_cast<float>(i) / kArcSegs) * kPi * 0.5f;
                const glm::vec3 cur = center + capsuleRot * glm::vec3(
                                          std::sin(a) * radius * dx,
                                          -halfHeight - std::cos(a) * radius,
                                          std::sin(a) * radius * dz);
                drawWorldSegment(prev2, cur);
                prev2 = cur;
            }
        }
        // 圆环绕 y = ±H（赤道/圆柱边）与 y = 0（圆柱中段）各画一圈
        constexpr int kSegs = 24; // 圆环分段数
        for (int ring = 0; ring < 3; ++ring) {
            const float y = (ring == 0) ? -halfHeight : ((ring == 1) ? 0.0f : halfHeight);
            for (int i = 0; i < kSegs; ++i) {
                const float a0 = (2.0f * kPi * i) / kSegs;
                const float a1 = (2.0f * kPi * (i + 1)) / kSegs;
                drawWorldSegment(
                    center + capsuleRot * glm::vec3(std::cos(a0) * radius, y, std::sin(a0) * radius),
                    center + capsuleRot * glm::vec3(std::cos(a1) * radius, y, std::sin(a1) * radius));
            }
        }
    };

    // 遍历刚体实体，仅绘制真正进入了物理世界的碰撞体（需同时具备刚体 + 碰撞体）。
    // 变换语义与 PhysicsWorld::BuildShapeForEntity 一致：用实体局部 TRS，
    // 半尺寸/半径乘比例烘焙进形状，Offset 只旋转不乘比例。
    const auto rbView = m_Context->Scene->Reg().view<TransformComponent, RigidBodyComponent>();
    for (auto entity : rbView) {
        const auto &tc = rbView.get<TransformComponent>(entity);
        const auto *box = m_Context->Scene->Reg().try_get<BoxColliderComponent>(entity);
        const auto *sphere = m_Context->Scene->Reg().try_get<SphereColliderComponent>(entity);
        const auto *capsule = m_Context->Scene->Reg().try_get<CapsuleColliderComponent>(entity);
        if (!box && !sphere && !capsule) {
            continue; // 无碰撞体，未创建刚体
        }
        const glm::quat &rot = tc.Rotation;

        // ---- 盒子碰撞体：中心 = T + R*Offset，半尺寸含比例；DrawDebug 关闭则不画 ----
        if (box && box->DrawDebug) {
            const glm::vec3 center = tc.Translation + rot * box->Offset;
            const glm::vec3 half = box->HalfExtents * tc.Scale;

            // 外棱：12 条边（角点序与 kEdges 一致：bit0=X, bit1=Y, bit2=Z）
            const glm::vec3 wc[8] = {
                center + rot * (half * glm::vec3(-1.0f, -1.0f, -1.0f)),
                center + rot * (half * glm::vec3(1.0f, -1.0f, -1.0f)),
                center + rot * (half * glm::vec3(-1.0f, 1.0f, -1.0f)),
                center + rot * (half * glm::vec3(1.0f, 1.0f, -1.0f)),
                center + rot * (half * glm::vec3(-1.0f, -1.0f, 1.0f)),
                center + rot * (half * glm::vec3(1.0f, -1.0f, 1.0f)),
                center + rot * (half * glm::vec3(-1.0f, 1.0f, 1.0f)),
                center + rot * (half * glm::vec3(1.0f, 1.0f, 1.0f)),
            };
            for (int e = 0; e < 12; ++e) {
                drawWorldSegment(wc[kEdges[e][0]], wc[kEdges[e][1]]);
            }
        }

        // ---- 球体碰撞体：中心 = T + R*Offset，半径取比例最大值（与 Jolt 一致）；DrawDebug 关闭则不画 ----
        if (sphere && sphere->DrawDebug) {
            const glm::vec3 center = tc.Translation + rot * sphere->Offset;
            const float radius = sphere->Radius
                                 * std::max({tc.Scale.x, tc.Scale.y, tc.Scale.z});
            // 3 个正交大圆环（XY/XZ/YZ 平面）构成线框球；环旋转随刚体取向
            constexpr int kSegs = 24;
            for (int plane = 0; plane < 3; ++plane) {
                for (int i = 0; i < kSegs; ++i) {
                    const float a0 = (2.0f * kPi * i) / kSegs;
                    const float a1 = (2.0f * kPi * (i + 1)) / kSegs;
                    glm::vec3 d0{0.0f, 0.0f, 0.0f}, d1{0.0f, 0.0f, 0.0f};
                    if (plane == 0) {
                        d0 = {std::cos(a0), std::sin(a0), 0.0f};
                        d1 = {std::cos(a1), std::sin(a1), 0.0f};
                    } else if (plane == 1) {
                        d0 = {std::cos(a0), 0.0f, std::sin(a0)};
                        d1 = {std::cos(a1), 0.0f, std::sin(a1)};
                    } else {
                        d0 = {0.0f, std::cos(a0), std::sin(a0)};
                        d1 = {0.0f, std::cos(a1), std::sin(a1)};
                    }
                    drawWorldSegment(center + rot * (d0 * radius),
                                     center + rot * (d1 * radius));
                }
            }
        }

        // ---- 胶囊碰撞体：主轴沿实体局部某轴（默认 Y），旋转跟随 Transform 并叠加轴向烘焙；DrawDebug 关闭则不画 ----
        if (capsule && capsule->DrawDebug) {
            const glm::vec3 center = tc.Translation + rot * capsule->Offset;
            const float radius = capsule->Radius
                                 * std::max({tc.Scale.x, tc.Scale.y, tc.Scale.z});
            // 半高缩放跟随胶囊主轴对应的轴分量（与 PhysicsWorld::BuildShapeForEntity 一致）
            float heightScale = tc.Scale.y;
            if (capsule->Axis == CapsuleAxis::X)
                heightScale = tc.Scale.x;
            else if (capsule->Axis == CapsuleAxis::Z)
                heightScale = tc.Scale.z;
            const float halfHeight = capsule->HalfHeight * heightScale;

            // 轴向烘焙（与形状构建一致：X: 绕局部 Z -90°，Z: 绕局部 X +90°），与实体旋转复合后作用于局部坐标
            glm::quat axisRot(1.0f, 0.0f, 0.0f, 0.0f);
            constexpr float kSqrtHalf = 0.707106781f;
            if (capsule->Axis == CapsuleAxis::X)
                axisRot = glm::quat(kSqrtHalf, 0.0f, 0.0f, -kSqrtHalf);
            else if (capsule->Axis == CapsuleAxis::Z)
                axisRot = glm::quat(kSqrtHalf, kSqrtHalf, 0.0f, 0.0f);
            const glm::quat capsuleRot = rot * axisRot;

            drawCapsuleMesh(center, capsuleRot, radius, halfHeight);
        }
    }

    // ---- 角色控制器胶囊（CharacterVirtual）：角色实体不挂 RigidBodyComponent，单独遍历 ----
    // 形状语义与 PhysicsWorld::ProcessPendingCharacters 一致：半径 = cc.Radius（不乘实体比例），
    // 圆柱半高 = H/2 - R，胶囊底部对齐脚底（Transform.Translation），中心在脚底上方 H/2 处。
    const auto charView = m_Context->Scene->Reg().view<
        TransformComponent, CharacterControllerComponent>();
    for (auto entity : charView) {
        const auto &tc = charView.get<TransformComponent>(entity);
        const auto &cc = charView.get<CharacterControllerComponent>(entity);

        const float cylHalf = std::max(cc.Height * 0.5f - cc.Radius, 0.0f);
        // 轴向烘焙 + 沿所选轴把底部抬到脚底（与 ProcessPendingCharacters 的形状构建一致）
        glm::quat axisRot(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 shift(0.0f);
        constexpr float kSqrtHalf = 0.707106781f;
        switch (cc.Axis) {
        case CapsuleAxis::X: axisRot = glm::quat(kSqrtHalf, 0.0f, 0.0f, -kSqrtHalf);
            shift = {cc.Height * 0.5f, 0.0f, 0.0f};
            break;
        case CapsuleAxis::Z: axisRot = glm::quat(kSqrtHalf, kSqrtHalf, 0.0f, 0.0f);
            shift = {0.0f, 0.0f, cc.Height * 0.5f};
            break;
        default: // Y（默认）
            shift = {0.0f, cc.Height * 0.5f, 0.0f};
            break;
        }
        const glm::vec3 center = tc.Translation + tc.Rotation * (shift + cc.Offset);
        drawCapsuleMesh(center, tc.Rotation * axisRot, cc.Radius, cylHalf);
    }
}

void DebugDrawLayer::DrawFirstPersonEyes(const Camera &camera, const glm::vec2 &imagePos,
                                         const glm::vec2 &viewportSize) {
    if (!m_Context->Scene) {
        return;
    }

    // 还原 OpenGL 投影（与 DrawColliders/DrawWorldBounds 同款，保证标记与画面/gizmo 对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{viewportSize.x, viewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 视点世界坐标投影到屏幕。坐标 = 角色脚底 + EyeOffset（与 Scene::UpdateFollowCamera
    // 一致：角色 yaw 每帧与相机同步后，绕 up 旋转 EyeOffset 恒等于原向量）。
    // 单个投影点不裁剪（点若在相机背面，投影后屏幕坐标越界、AddLine 自然不画）。
    const auto projectPoint = [&](const glm::vec3 &world) -> glm::vec2 {
        const glm::vec4 c = viewProjGL * glm::vec4(world, 1.0f);
        return glm::vec2(origin.x + (0.5f + c.x / c.w * 0.5f) * size.x,
                         origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
    };

    // 视点标记颜色（橙，区分青绿碰撞体/灰盒/红绑定盒）
    const ImU32 eyeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.00f, 0.65f, 0.10f, 1.0f));
    const ImU32 footColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.00f, 0.65f, 0.10f, 0.45f));

    const auto followView = m_Context->Scene->Reg().view<TransformComponent, CharacterControllerComponent,
                                                      FollowCameraComponent>();
    for (auto entity : followView) {
        const auto &tc = followView.get<TransformComponent>(entity);
        const auto &fc = followView.get<FollowCameraComponent>(entity);
        if (!fc.Enabled) {
            continue; // 关闭视点的角色不画
        }
        const glm::vec3 foot = tc.Translation;
        const glm::vec3 eye = foot + fc.EyeOffset;
        const glm::vec2 sEye = projectPoint(eye);
        const glm::vec2 sFoot = projectPoint(foot);

        // 到脚底的虚线：先画粗的深色底（压场景高亮），再叠半透明橙色，视觉更清楚
        dl->AddLine(ImVec2(sFoot.x, sFoot.y), ImVec2(sEye.x, sEye.y), IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddLine(ImVec2(sFoot.x, sFoot.y), ImVec2(sEye.x, sEye.y), footColor, 1.5f);

        // 视点十字（水平 12px × 垂直 12px），随屏幕朝向、不随角色旋转
        constexpr float kHalf = 6.0f;
        dl->AddLine(ImVec2(sEye.x - kHalf, sEye.y), ImVec2(sEye.x + kHalf, sEye.y), eyeColor, 2.0f);
        dl->AddLine(ImVec2(sEye.x, sEye.y - kHalf), ImVec2(sEye.x, sEye.y + kHalf), eyeColor, 2.0f);
    }
}

// ============================================================
// 方向光调试图标叠加
// ============================================================

void DebugDrawLayer::DrawDirectionalLights(const Camera &camera, const glm::vec2 &imagePos,
                                           const glm::vec2 &viewportSize) {
    if (!m_Context->Scene) {
        return;
    }

    // 还原 OpenGL 投影（与 DrawColliders 同款，保证线与画面/gizmo 对齐）
    glm::mat4 projGL = camera.GetProj();
    projGL[1][1] *= -1.0f;
    const glm::mat4 viewProjGL = projGL * camera.GetView();

    const glm::vec2 origin{imagePos.x, imagePos.y};
    const glm::vec2 size{viewportSize.x, viewportSize.y};
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 线段 → 屏幕坐标：先裁剪到相机近平面再投影（与 DrawColliders 同款），
    // 保证离锚点很远的射线即使一端在相机背面也能正确画出
    const auto projectSegment = [&](const glm::vec3 &a, const glm::vec3 &b,
                                    glm::vec2 &sa, glm::vec2 &sb) -> bool {
        constexpr float kNearW = 1e-3f;
        const glm::vec4 ca = viewProjGL * glm::vec4(a, 1.0f);
        const glm::vec4 cb = viewProjGL * glm::vec4(b, 1.0f);
        const bool aFront = ca.w > kNearW;
        const bool bFront = cb.w > kNearW;
        if (!aFront && !bFront) {
            return false; // 整段在相机背面
        }
        glm::vec4 cA = ca, cB = cb;
        if (aFront && !bFront) {
            // b 在背面：沿线段插值到近平面
            const float t = (kNearW - cb.w) / (ca.w - cb.w);
            cB = cb + (ca - cb) * t;
            cB.w = kNearW;
        } else if (!aFront && bFront) {
            // a 在背面：沿线段插值到近平面
            const float t = (kNearW - ca.w) / (cb.w - ca.w);
            cA = ca + (cb - ca) * t;
            cA.w = kNearW;
        }
        const auto toPos = [&](const glm::vec4 &c) {
            return glm::vec2(origin.x + (0.5f + c.x / c.w * 0.5f) * size.x,
                             origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
        };
        sa = toPos(cA);
        sb = toPos(cB);
        return true;
    };

    // 画一条世界坐标线段：先画粗的深色底（压场景高亮），再叠灯光颜色，保证在亮/暗背景都可读
    const auto drawWorldSegment = [&](const glm::vec3 &a, const glm::vec3 &b, ImU32 color) {
        glm::vec2 sa, sb;
        if (projectSegment(a, b, sa, sb)) {
            dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), IM_COL32(0, 0, 0, 160), 2.4f);
            dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), color, 1.2f);
        }
    };

    constexpr float kPi = 3.14159265358979f;

    const auto dirView = m_Context->Scene->Reg().view<
        TransformComponent, DirectionalLightComponent>();
    for (auto entity : dirView) {
        const auto &tc = dirView.get<TransformComponent>(entity);
        const auto &dlc = dirView.get<DirectionalLightComponent>(entity);

        // 由世界矩阵的旋转部分推导前向方向（与 Scene::UpdateLightParams 的 lightDir 一致）：
        // 前向向量 -Z 旋转后指向光源（朝太阳）；光芒沿 -dir 即光传播方向照向被照物。
        // 缩放逐列归一化后再转四元数
        const glm::mat3 rot3 = glm::mat3(tc.GetWorldMatrix());
        const glm::mat3 normalizedRot(
            glm::normalize(rot3[0]), glm::normalize(rot3[1]), glm::normalize(rot3[2]));
        const glm::quat worldRot = glm::quat_cast(normalizedRot);
        const glm::vec3 dir = glm::normalize(worldRot * glm::vec3(0.0f, 0.0f, -1.0f));

        // 锚点 = 实体世界位置；颜色取 rgb（HDR 分量截到 [0,1] 供屏幕显示）
        const glm::vec3 center = glm::vec3(tc.GetWorldMatrix()[3]);
        const ImU32 lightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            std::min(dlc.Color.r, 1.0f), std::min(dlc.Color.g, 1.0f),
            std::min(dlc.Color.b, 1.0f), 1.0f));

        // 盘面正交基 u/v（法线沿光出射方向）：任选一个与 dir 不共线的参考轴叉积构造
        const glm::vec3 ref = (std::fabs(dir.y) < 0.99f)
                                  ? glm::vec3(0.0f, 1.0f, 0.0f)
                                  : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 u = glm::normalize(glm::cross(ref, dir));
        const glm::vec3 v = glm::cross(dir, u);

        constexpr float kDiscRadius = 0.5f; // 太阳盘半径
        constexpr float kRayLength = 2.0f; // 光线束长度
        constexpr int kSegs = 24; // 盘面圆环/辐条分段
        constexpr int kRays = 8; // 光线束数量

        // 1) 太阳盘：外圈圆环 + 中心到外圈的辐条（盘面法线背向光照方向）
        for (int i = 0; i < kSegs; ++i) {
            const float a0 = (2.0f * kPi * i) / kSegs;
            const float a1 = (2.0f * kPi * (i + 1)) / kSegs;
            drawWorldSegment(
                center + kDiscRadius * (std::cos(a0) * u + std::sin(a0) * v),
                center + kDiscRadius * (std::cos(a1) * u + std::sin(a1) * v), lightColor);
        }
        for (int i = 0; i < kSegs; ++i) {
            const float a = (2.0f * kPi * i) / kSegs;
            drawWorldSegment(center,
                             center + kDiscRadius * (std::cos(a) * u + std::sin(a) * v),
                             lightColor);
        }

        for (int i = 0; i < kRays; ++i) {
            const float a = (2.0f * kPi * i) / kRays;
            const glm::vec3 start =
                center + kDiscRadius * 0.7f * (std::cos(a) * u + std::sin(a) * v);
            drawWorldSegment(start, start - dir * kRayLength, lightColor);
        }

        // 3) 盘心圆点：固定锚点位置，图标较小时也便于定位
        {
            glm::vec2 sc;
            if (projectSegment(center, center, sc, sc)) {
                dl->AddCircleFilled(ImVec2(sc.x, sc.y), 3.0f, lightColor);
                dl->AddCircle(ImVec2(sc.x, sc.y), 3.0f, IM_COL32(0, 0, 0, 200));
            }
        }
    }
}

} // namespace GE
