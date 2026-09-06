# 阴影专用剔除方案计划书（Shadow-AABB Culling）

> 现状基准：《阴影贴图实现计划书》S1~S5 已完成——方向光阴影链路（光矩阵 → ShadowMap
> pass → 批次共享 → Lighting 采样 → 开关/回归）全部落地。本计划书解决其中一个**正确性
> 缺口**：**主相机视锥外的物体，其阴影可能投射进视锥，但当前剔除把它们整段删掉，阴影丢失**。

---

## 1. 问题陈述

`Scene::RenderMeshes3D`（`Scene.cpp:838`）用**主相机视锥**（`Frustum::FromViewProjection(projection*view)`）
对全部网格实体做剔除——视锥外的实体不调用 `r3d.Draw*SubMesh`，根本进不了渲染器的
`m_Meshes`。而 ShadowMap pass 画的 `m_OpaqueBatches` 又恰恰是从 `m_Meshes` 推导出来的
（`PrepareDeferredBatches`）。于是：

```
主相机视锥外、但影子能投进视锥的物体
   ↓ 主相机视锥剔除（Scene 层）
   ✗ 不提交 → 不在 m_Meshes → 不在 m_OpaqueBatches
   → ShadowMap pass 画不到它 → 阴影丢失
```

典型场景：物体在主相机身后/侧方，光从斜侧照来，它的影子落进视野——本应投出阴影，
现在整段缺失。

---

## 2. 现状梳理：剔除与批次流转

```
Scene::RenderMeshes3D（主相机视锥剔除）
  └ 通过 视锥内 的实体 → r3d.DrawSubMesh / DrawSkinnedSubMesh
       └ Renderer3D::m_Meshes（帧内采集，BeginScene 清空）
            └ PrepareDeferredBatches（S3 幂等公共入口）
                 ├ m_OpaqueBatches   ← 排序后切出的不透明段
                 └ m_TransparentBatches
                      └ FlushShadow 与 FlushGBuffer 都画 m_OpaqueBatches
```

**结论**：Shadow pass 的输入被主相机视锥"预过滤"过一遍，剔除发生在到达渲染器**之前**。
要让阴影 pass 看到更多物体，必须在 Scene 层**额外提交**一套「阴影可见」的物体，而不是
复用主可见批次。

---

## 3. 方案：阴影专用剔除视锥（两套剔除）

在 Scene 层把剔除拆成两条路径，各产出一套批次：

| 路径 | 剔除体 | 产物 | 消费 pass |
|---|---|---|---|
| 主可见 | 主相机视锥（现状不变） | `m_OpaqueBatches` | GBuffer / Transparent |
| **阴影可见** | **阴影世界 AABB**（新增） | `m_ShadowBatches` | **ShadowMap** |

### 3.1 阴影视锥（Shadow Culling Volume）怎么算

1. **主相机视锥 8 角点** → 光空间 → 取 min/max 得 **Light Space AABB**（就是
   `ComputeLightViewProj` 内部那套 `minP/maxP`，`Scene.cpp:51-65`）；
2. 这个正交盒的 8 角点经 `inverse(lightView)` 变换回世界空间 → 取世界 min/max →
   **世界空间 AABB**（阴影视锥，Axis-Aligned，可直接与物体世界 AABB 做相交判定）。

```
        ☀ 光源
        │ 光空间
  ┌─────┼─────┐   Light Space AABB（覆盖主视锥在光方向的投影）
  │  主视锥  │
  └─────┼─────┘
        │ inverse(lightView) → 世界空间 AABB = 阴影视锥
```

### 3.2 保守性分析（本方案成立的关键）

**问**：有限的光空间 AABB 是否"包得住所有能把影子投进视锥的物体"？

理想情况下，把影子投进视锥的物体，其光空间位置应满足：x/y 落在主视锥的光空间足迹内，
z 从「视锥最靠光的一侧」一直**朝光源方向无限外推**（物体越靠近光源，影子越远地投进
视锥）。所以理想剔除体 = 视锥足迹 × [−∞, maxZ]（朝光方向无界），而非有限 AABB。

**但**：ShadowMap pass 的光正交投影近/远面 = 有限 AABB 的 z 范围（`§6.3` 并带 5% 余量）。
一个比近面更靠光源的物体，在阴影 pass 里 `gl_Position.z < near` → **被近面裁剪**，
画不画都不会投出阴影。也就是说：

> **有限 AABB 正是 ShadowMap 实际能捕获的覆盖范围。用它做剔除，与阴影图自身的能力
> 完全一致——不会比"画全套"多丢任何影子。**

| 物体位置（光空间） | 阴影 pass 行为 | 有限 AABB 剔除 |
|---|---|---|
| z ∈ [minZ, maxZ]（AABB 内） | 正常写入深度 | 保留 ✓ |
| x/y 在足迹外 | 影子落不进视锥，画了白画 | 剔除 ✓（优化） |
| z < near（比视锥最靠光侧更近光） | 被近面裁剪，不投影 | 剔除 ✓（与能力一致） |
| z > far（超出远面） | 被远面裁剪 | 剔除 ✓ |

**结论**：Shadow-AABB 是**正确且自洽**的 Shadow pass 剔除体。它不是「理想最大保守」
（近光侧投影物仍丢），但那部分丢影子是光正交近/远面的既有限制，与剔除无关——要补需
推光近面 + 外推剔除体（见 §7 后续）。

### 3.3 世界 AABB 判定（vs 光空间判定）

- **世界 AABB**：把 Light Space AABB 转回世界取 min/max，每物体一次 AABB-AABB 相交判定
  （6 次比较），便宜；因世界 AABB 是旋转盒的轴对齐包络，比光空间盒略松（多画一点，无
  漏剔，保守方向正确）。
- **光空间判定**：每物体把世界 AABB 经 `lightView` 变换到光空间再判，精确但每物体多
  一组矩阵变换。

阶段 1 选**世界 AABB**（便宜、无漏剔、贴合用户方案）；光空间判定列为精度优化。

---

## 4. 详细设计

### 4.1 AABB 新增相交判定

`AABB`（`Mesh.h:174`）目前有 `IsValid / Transformed / Expand`，缺 AABB-AABB 相交。加：

```cpp
/// 与另一 AABB 是否相交（各轴分离判反例；无效盒视为不相交）
bool Overlaps(const AABB &o) const {
    if (!IsValid() || !o.IsValid()) return false;
    return min.x <= o.max.x && max.x >= o.min.x
        && min.y <= o.max.y && max.y >= o.min.y
        && min.z <= o.max.z && max.z >= o.min.z;
}
```

### 4.2 `ComputeLightViewProj` 暴露 Light Space AABB

现函数只返回矩阵，内部 `minP/maxP` 不外泄（`Scene.cpp:36-79`）。增加输出参数：

```cpp
glm::mat4 ComputeLightViewProj(const glm::vec3 &lightDir,
                               const glm::mat4 &view, const glm::mat4 &projection,
                               glm::vec3 *outMinP = nullptr, glm::vec3 *outMaxP = nullptr);
```

Scene 侧拿 `outMinP/outMaxP`（已含 5% z 余量）构造阴影视锥。

### 4.3 Scene：`RenderMeshes3D` 增加阴影遍历

在主遍历之后、`r3d.EndScene()` 之前，新增第二遍：遍历同一批 `meshView`，用**阴影世界
AABB** 剔除，命中者调新接口提交到 `m_ShadowMeshes`。两遍逻辑高度重叠，抽公共 lambda。

- **剔除体**：`shadowVolume = worldAabbOf(lightSpaceAabb)`；无方向光或 `castShadow==false`
  时跳过整遍（无阴影）。
- **蒙皮实体**：与主遍历一致——绑定盒追不上变形，**跳过剔除、一律提交**（保守）。
- **子网格细剔除**：沿用现有 `sub.aabb.Transformed(world)` 对 `shadowVolume` 判交
  （仅 `CullingMode::SubMesh` 时）。
- **BoundingBoxComponent 子树粗剔除**：阴影遍历**暂不做**（正确性优先，该优化对阴影
  边界处可能漏——物体在阴影盒内但实体盒判外；列后续）。
- **Blend 材质**：不主动跳过——渲染器端 `m_ShadowBatches` 只切不透明段，透明自然不进
  阴影（与 `depth_only.frag` / 计划 §1「透明不投影」一致）。

### 4.4 Renderer3D：阴影集合与批次

- 新增成员 `std::vector<MeshInstance> m_ShadowMeshes`（`BeginScene` 一并清空）；
- 提交接口：`DrawShadowSubMesh` / `DrawShadowSkinnedSubMesh`（或 `DrawSubMeshImpl` 加
  `forShadow` 标记，命中则入 `m_ShadowMeshes` 而非 `m_Meshes`）；
- `PrepareDeferredBatches` 在构建 `m_OpaqueBatches` 后，额外构建：
  - `m_ShadowBatches`：`m_ShadowMeshes` 排序 → 切出**不透明段**（`passId==Opaque`）；
  - `m_ShadowInstanceBuffer`：`m_ShadowMeshes` 的 per-instance 数据上传（与主实例缓冲
    分离，因阴影集合含主视锥外物体，实例内容不同）；
- `FlushShadow` 改画 `m_ShadowBatches` + `m_ShadowInstanceBuffer`（管线路由/剔除/
  蒙皮关节绑定全部复用 `DrawMeshInstances`，只换批次输入）；
- 两个集合成员的生命周期与 `m_OpaqueBatches` 一致（`FlushTransparent` 尾部清空）。

### 4.5 数据流（改后）

```
Scene::RenderMeshes3D
  ├ 遍历① 主相机视锥剔除 → r3d.Draw*SubMesh        → m_Meshes
  └ 遍历② 阴影世界AABB剔除 → r3d.DrawShadow*SubMesh → m_ShadowMeshes

PrepareDeferredBatches（幂等）
  ├ m_Meshes         → m_OpaqueBatches      → GBuffer / Transparent
  └ m_ShadowMeshes   → m_ShadowBatches      → ShadowMap（FlushShadow）
```

> 主可见物体进**两个**集合（两次提交），CPU 侧翻倍但正确；GPU 侧本来就要画两遍（GBuffer
> 一遍 + Shadow 一遍），无新增 draw。优化（可见物只提一次、渲染器内分流）列 §7。

---

## 5. 实施步骤（每步可独立构建验证）

**S1 阴影视锥计算 + AABB 判定（纯 CPU，无视觉变化）**
`AABB::Overlaps`；`ComputeLightViewProj` 加输出参数；Scene 算 `shadowVolume` 世界 AABB。
验收：RenderDoc 无变化；日志/断点确认 shadowVolume 数值随相机与光方向正确变化。

**S2 Renderer3D 阴影集合与批次（FlushShadow 仍用旧批次）**
`m_ShadowMeshes` + `DrawShadow*SubMesh` + `PrepareDeferredBatches` 构建 `m_ShadowBatches`/
`m_ShadowInstanceBuffer`；`BeginScene` 清空。
验收：主画面与 Shadow pass 完全不变（还没切换）；RenderDoc 帧捕获下断点确认两套批次
存在、主可见物在两个集合里都有。

**S3 第二遍剔除 + FlushShadow 切换**
`RenderMeshes3D` 加阴影遍历；`FlushShadow` 改画 `m_ShadowBatches`。
验收：**主视锥外的投影物影子出现了**；GBuffer 画面与之前逐像素一致；主视锥内物体影子
不变。

**S4 回归**
空场景/全透明/纯点光场景空过不报错；蒙皮、MASK 镂空影、双面剔除正常；`SetDeferred`
前向回退阴影消失；RenderDoc 确认 Shadow pass 只画阴影可见物体、池无重复分配。

---

## 6. 风险与注意

1. **重复提交开销**：主可见物两次进提交队列。小/中场景可忽略；超大场景可做「主可见物
   只提一次、渲染器内按标记分流」优化（§7）。
2. **近光侧投影物仍丢影**：比光近面更靠光源的物体被近面裁剪（`§3.2`）。这不是剔除的
   锅，但用户若遇到「正上方物体没影子」属此限制，需推光近面 + 外推剔除体（§7）。
3. **世界 AABB 略松**：旋转盒的轴对齐包络多画少量物体，无漏剔。要精确换光空间判定。
4. **BoundingBoxComponent 子树剔除在阴影遍缺位**：阴影盒与实体盒边界处的粗筛行为与
   主遍不一致（阴影遍更保守），可接受；要做需按阴影盒另算。
5. **蒙皮阴影**：绑定盒追不上变形，阴影遍也跳过剔除——阴影可见的远处蒙皮角色会多画，
   与主遍行为一致。
6. **精度不受 AABB 放大影响**：阴影遍剔除体更大但**光正交 z 范围不变**（近远面仍取
   主视锥 AABB 的 z），只有 x/y 密度随足迹变大摊薄——与 CSM 方向一致，阶段 1 接受。

---

## 7. 后续阶段（本计划书不做）

- **近光侧覆盖**：把光正交近面朝光源方向外推 + 阴影剔除体同步外推（足迹 × [−∞,maxZ]），
  捕获「光源与视锥之间」的投影物；代价是 z 精度摊薄（配反向 Z）或走 CSM。
- **光空间精确判定**：每物体世界 AABB 经 `lightView` 变换到光空间判交，替换松散的世界
  AABB 包络。
- **单次提交分流**：主可见物只提交一次，渲染器内按「主可见 / 阴影可见」标记分流到两套
  批次，省一半 CPU 提交。
- **阴影遍子树粗筛**：BoundingBoxComponent 按阴影盒做实体级粗剔除，与主遍对齐。
- **CSM 整合**：本方案的阴影视锥正是 CSM「按距离分档」的雏形，可平滑迁移。
