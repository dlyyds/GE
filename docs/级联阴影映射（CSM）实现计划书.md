# 级联阴影映射（CSM）实现计划书

> 现状基准：方向光**单级阴影**（《阴影贴图实现计划书》S1~S5 完成：单张覆盖整个相机
> 视锥的 4096² 深度图 + Lighting 单图 PCF 采样）与**阴影专用剔除**（《阴影剔除
> （Shadow-AABB Culling）计划书》S1~S3 完成：第二遍遍历 + 阴影视锥 + `m_ShadowBatches`
> 集合）均已落地。本计划书把单级阴影升级为**按深度分档的多级阴影（Cascaded Shadow
> Maps）**，解决单级「近处密度不足、远景精度浪费」的核心矛盾；并让阴影剔除的「阴影视锥」
> 直接按档位复用——这正是《阴影剔除计划书》§7 预定的「CSM 整合：本方案的阴影视锥正是
> CSM 按距离分档的雏形，可平滑迁移」。

> 范围限定**单个方向光、延迟渲染路径**；级间过渡混合（crossfade）、texel 稳定化、
> 每级独立偏差、前向路径级联等列为后续阶段（§7）。透明物体不产生也不接收阴影的
> 语义不变（Blend 不进阴影批次）。

```
现状（单级）                      CSM（多级，本计划）
  整个视锥 ──→ 一张 4096²           近片/中片/远片 ──→ 各一张 2048²
  近处物体与远景抢同一张图           近片世界跨度小 → 同样分辨率密度高
  密度 = 全视锥跨度 / 4096          密度 = 该片跨度 / 2048，逐片递减
```

---

## 1. 目标与范围

让方向光阴影按相机距离分档：把相机视锥沿深度切成 **N 档（默认 3，上限
`kMaxCascades=4`）**，每档生成一张**独立光空间深度图**，Lighting 里按片元所在档位
采样对应深度图。目标收益：

- **近处阴影清晰**：近档世界跨度小，相同分辨率下 texel 密度成倍提升（近档 ≈ 数倍于
  现状），消除「离相机近的墙/人物阴影糊成一团」；
- **远景不被浪费**：远档单独一张图，不再挤占近档分辨率；远景密度降低是**主动取舍**
  而非被动受损；
- **每档按「该档阴影视锥」做二次剔除**：各档只画落在自己体积内的物体——阴影总提交量
  比「画全场景 N 遍」大幅下降（物体只进它跨越的档）。

**阶段 1 明确不做**：级间 blend/crossfade（硬切换，边界可见不连续，记为已知问题，
§6.1）；texel 稳定化（每档 AABB 随相机逐帧重投影，阴影边缘爬动沿用单级现状）；
每级独立偏差/PCF 半径（阶段 1 各级共享一组可调参数）；点光源阴影、前向路径级联。
**级联数 = 1 时行为与现状完全一致**（同一套矩阵构造核心，见 §4.2），保底兼容与调试。

---

## 2. 现状梳理：单级阴影的数据流（本计划要改的每一处）

```
Scene::UpdateLightParams()
  └ ComputeLightViewProj(lightDir, view, projection, &minP, &maxP, &lightView)  ← 单一矩阵
       ├ lightParams.lightViewProj = ortho(minP..maxP) * lightView
       └ m_ShadowVolume = BuildShadowVolume(minP, maxP, lightView)   ← 单一阴影视锥

Scene::RenderMeshes3D 遍历②（阴影剔除 S3）
  └ 对每个 mesh：m_ShadowVolume.Overlaps(worldAabb) → DrawShadow*SubMesh    ← 单一集合

Renderer3D
  ├ m_ShadowMeshes / m_ShadowBatches / m_ShadowInstanceBuffer  ← 各一份
  ├ PrepareDeferredBatches → 构建单套阴影批次
  └ FlushShadow(ctx) → 画 m_ShadowBatches（FrameUBO.view = lightViewProj）   ← 单一 pass

SceneLayer::RecordScenePasses → 单个 "ShadowMap" pass + hShadow 虚拟资源    ← 单一深度图

Lighting（deferred_lighting.frag）
  └ PCFShadow(worldPos)：lightViewProj 投影 → samplerShadowDepth(binding7) 单图 9 tap
```

**结论**：单级阴影把「视锥 AABB → 光矩阵/剔除体 → 深度图 → 采样」整条链都做成了
**单份**。CSM 的本质是这条链的**数组化**：视锥切片、每片一套矩阵/体积/深度图/批次，
Lighting 按深度选片。每个环节的改动都是「单例 → 按档位数组」，结构性清晰。

---

## 3. 核心思路：按深度切片，每片一个「小视锥」

### 3.1 为什么分档能同时解决「近糊远废」

单张阴影图的 texel 密度 = `光空间覆盖的世界跨度 / 分辨率`，覆盖的是**整个相机视锥**。
于是同一个 texel：近处代表几厘米、远处代表几十米——近处细节被「均摊」掉了。

CSM 把视锥沿深度切成近/中/远 N 片，**每片各自**用一个贴着该片的光空间正交视锥框住
（近片跨度小、远片跨度大），各画一张同分辨率的深度图：

```
        ☀ 光源（光空间）
  近片跨度小 → 2048² 里塞的全是近处 → 每 texel 覆盖的世界小 → 清晰
  远片跨度大 → 2048² 里塞的是远景    → 每 texel 覆盖的世界大   → 糊（主动取舍）
```

近处密度 ↑、远处密度 ↓ 正是 CSM 的默认权衡；远景密度不足由「远片给更大分辨率」缓解
（每档尺寸独立可配）。

### 3.2 每片的光空间视锥 = 阴影剔除 S 系列的「阴影视锥」按片复用

《阴影剔除计划书》§3 已经证明：**有限光空间 AABB 正是该视锥能捕获的阴影范围**。
CSM 里每片的光空间 AABB（该片 8 个视锥角点转光空间取 min/max）就是**该片的阴影视锥**
——用它做剔除、用它定正交近远面，与该片深度图能力完全自洽：

```
片 i 的视锥切片角点（世界空间 8 点）
   ├→ 转光空间 → 片 i 光空间 AABB → ortho × lightView = 片 i lightViewProj
   └→ 转光空间 AABB 的 8 角点经 inverse(lightView) 回世界 → 片 i 世界阴影视锥
        （= m_ShadowVolume[i]，S1 的 BuildShadowVolume 逐片复用）
```

因此本计划**不引入新的剔除数学**——把 S3 的「单体积第二遍遍历」扩成「逐片体积各遍历
一次」，剔除体、Overlaps 判定、蒙皮跳过规则全部原样保留（§4.3）。

### 3.3 片元如何选片：按视图空间深度

Lighting 片元有 `worldPos`（G2）和 `invView`（UBO 已有），算 `viewZ =
(invView * vec4(worldPos,1)).z`（相机朝 -Z，viewZ 为负）。切分距离是正值
`split[0]=near < split[1] < ... < split[N-1]=far`，选片：

```glsl
int cascade = kCascadeCount - 1;
for (int i = 0; i < kCascadeCount - 1; i++) {
    if (-viewZ <= cascadeSplits[i]) { cascade = i; break; }
}
```

选片后投影用该片的 `cascadeViewProj[cascade]`、采样该片的深度图。近片距近平面
之前的片元落到 cascade 0，超出远平面的落到最后一档（投影出界 → 视为受光，§4.6）。

---

## 4. 详细设计

### 4.1 切分距离（practical split）

`near/far` 为主相机近/远（从投影矩阵标准公式反解，或改 `UpdateLightParams` 签名由
主相机直接传入；建议前者，不改接口）。对 `i ∈ [1, N]` 求第 i 片远端：

```
split_log[i] = near * pow(far / near, i / N)        // 对数切分：近密远疏，均匀屏幕感
split_uni[i] = near + (far - near) * i / N          // 均匀切分
split[i]     = mix(split_uni[i], split_log[i], λ)   // practical split，默认 λ = 0.5
```

`split[0] = near`。级联数 N、λ 暴露为 Renderer3D 可调参数（默认 `N=3, λ=0.5`），存
`LightParams.cascadeSplits[]` + `cascadeCount`，每帧切分距离随相机近远/视锥变化重算。

### 4.2 每级光矩阵 + 阴影视锥（改造 `ComputeLightViewProj`）

现有 `ComputeLightViewProj`（`Scene.cpp` 匿名空间）拿**整个视锥**的 NDC 角点经
`inverse(viewProj)` 求光空间 AABB。CSM 需要**切片**的角点。重构为两段、复用同一核心：

1. **公共核心 `BuildLightVolumeCorners`（新）**：输入 8 个**世界空间角点** + 光方向，
   输出 `lightView + lightSpaceAabb(minP/maxP，含 5% z 余量) + ortho*lightView`。
   现有全视锥路径改为「先由 NDC 角点反解全视锥世界角点 → 调核心」，**行为逐像素不变**。
2. **每片世界角点 `SliceCorners(view, fovY, aspect, splitLo, splitHi)`（新）**：对
   `d ∈ {splitLo, splitHi}` 各取 4 个视图空间角点 `(±tanHalfFovY*aspect*d, ±tanHalfFovY*d, -d)`
   （相机朝 -Z），`inverse(view)` 变到世界 → 8 角点。喂给公共核心得该片
   `cascadeViewProj[i] + minP[i]/maxP[i] + lightView[i]`。

数据落点（`LightParams`，`Renderer3D.h:129`）：

```cpp
std::array<glm::mat4, kMaxCascades> cascadeViewProj;   ///< 每级光空间 view-proj
std::array<float, kMaxCascades> cascadeSplits;         ///< 每级远端切分距离（split[0]=near）
uint32_t cascadeCount = 1;                              ///< 生效级数（默认 3；1 = 现状）
```

每级世界阴影视锥存 Scene 侧（`Scene.h:255` 的 `m_ShadowVolume` 数组化）：

```cpp
std::array<AABB, kMaxCascades> m_ShadowVolume;   ///< 每级世界阴影视锥（无效盒 = 该级不激活）
```

> 级联数 = 1 时，`SliceCorners` 退化为全视锥角点，`cascadeViewProj[0]` 与现有
> `ComputeLightViewProj` 完全一致——**兼容回退即天然存在**。

### 4.3 Scene：第二遍遍历数组化（逐级剔除）

现状 S3 第二遍遍历（`Scene.cpp:1033-1083`）对**单个** `m_ShadowVolume` 判交。CSM 改为
**逐级**：对每档体积各遍历一次 `meshView`，命中者提交到**该级集合**。提交接口加级号：

```cpp
// 遍历②（CSM）：for c in [0, cascadeCount)
//   剔除体 = m_ShadowVolume[c]（无效则跳过该级）
//   对 mesh：isSkinned || !meshAabb.IsValid() || m_ShadowVolume[c].Overlaps(worldAabb)
//            → DrawShadow*SubMesh(..., cascade = c)
//   子网格细剔、蒙皮跳过规则与 S3 完全一致（仅把 shadowVolume 换成 m_ShadowVolume[c]）
```

- 渲染器侧提交接口 `DrawShadowSubMesh / DrawShadowSkinnedSubMesh` 各加
  `uint32_t cascade` 参数（默认 0），命中入 `m_ShadowMeshes[cascade]`；
- 物体世界 AABB 跨多档时进入多档集合（正确：它在每档都能投影），单档物体只进一档
  ——**这是 CSM 性能核心**：阴影总提交 ≈ 各档体积内物体之和，远小于「N×全场景」；
- 蒙皮实体与 S3 一致跳过剔除、逐级提交（绑定盒追不上变形，保守正确，远处角色多画
  的代价每级受剔除体积约束）；
- BoundingBoxComponent 子树粗剔逐级均**不做**（沿用 S3 决策，列后续）。

> 阶段 1 用「逐级各遍历一次」（N 小，直观、贴合现有代码）；单趟遍历按 bitmask 判
> 每物体命中哪些级再批量入集合的优化列 §7。

### 4.4 Renderer3D：批次数组化 + 逐级 ShadowMap pass

成员数组化（生命周期与现状一致：`BeginScene` / `FlushTransparent` 尾部清空）：

```cpp
std::array<std::vector<MeshInstance>, kMaxCascades> m_ShadowMeshes;
std::array<std::vector<RenderBatch>, kMaxCascades> m_ShadowBatches;
std::array<BufferAllocation, kMaxCascades> m_ShadowInstanceBuffer;
```

- `PrepareDeferredBatches`（`Renderer3D.cpp:643`）：现有「排序 → 切不透明段 → 上传
  实例缓冲」循环执行 `cascadeCount` 次，每次喂 `m_ShadowMeshes[c]`；
- `FlushShadow(ctx, cascade)`：上传该级阴影 FrameUBO（`projection=单位阵、
  `view=cascadeViewProj[cascade]`），视口 = 该级深度图尺寸，画
  `m_ShadowBatches[cascade]` + `m_ShadowInstanceBuffer[cascade]`。管线路由 / 剔除 /
  蒙皮关节绑定全部复用 `DrawMeshInstances` 的 shadow 路径，只换批次输入与矩阵。

**SceneLayer 逐级声明**（`SceneLayer.cpp:212-238` 的单 pass 循环化）：

```cpp
// ShadowMap_C0..C{N-1}：每级一张独立深度图（虚拟资源，尺寸可配，默认 2048² D32F）
for (uint32_t c = 0; c < cascadeCount; ++c) {
    RenderGraphResourceDesc desc; desc.extent = {cascadeSize[c], cascadeSize[c]};
    desc.format = vk::Format::eD32Sfloat;
    ResourceHandle hShadowC = b.CreateVirtualResource(desc, "ShadowMap_C" + std::to_string(c));
    RenderPassDesc &p = b.AddPass("ShadowMap_C" + std::to_string(c));
    // depthAttachment = hShadowC（eClear / eStore）；renderArea = 该级尺寸
    // execute 闭包捕获 c → FlushShadow(ctx, c)
}
// Lighting pass 的 readImages 追加 hShadow[0..N-1] → readImageViews[4..4+N-1]
```

默认每级 2048²（近级可配 4096²）。内存账：3×2048² D32F ≈ 50MB，**反而低于**现状
单张 4096² 的 ≈ 67MB，近处密度却翻倍。

### 4.5 Lighting：级联采样

**UBO**（`LightingUBO`，`Renderer3D.h:367` 与 `deferred_lighting.frag:7` 同步，std140，
追加在现有阴影字段位置，注意 mat4 数组按 64B 连续排布）：

```glsl
mat4  cascadeViewProj[kMaxCascades]; // 每级光空间 view-proj（世界 → 该级光裁剪空间）
vec4  cascadeSplits;                 // x/y/z/w = split[0..3]（远端；cascadeCount 之后作废）
vec4  cascadeParams;                 // x = 生效级数，yz 预留
vec4  shadowParams;                  // 复用：x = 单级尺寸，y = 偏差，z = 开关，w = PCF 半径
```

> std140 下 `mat4[kMaxCascades]` 与现有单个 `lightViewProj` 布局兼容地替换：数组每级
> 64B、无额外填充。C++ 结构与 GLSL 块同步增删（沿用 §5.3 的既定做法）。

**采样器数组**：`samplerShadowDepth`（binding 7）由单图改
`layout(set=1, binding=7) uniform sampler2D samplerShadowDepth[kMaxCascades];`
—— Lighting pipeline layout 的 binding 7 改**数组描述符**，`FlushLighting` 按
`readImageViews[4..]` 绑定各级深度图。PCF 不变（每 tap 硬比较），只是 `uv/texel/bias`
取该级参数：

```glsl
float CascadePCF(vec3 worldPos, int cascade, float bias) {
    vec4 sc = lighting.cascadeViewProj[cascade] * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / sc.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    float depth = proj.z;                       // ZO：该级光空间深度已是 [0,1]
    if (uv 出界 || depth 出界) return 1.0;      // 出界 = 受光（§4.6）
    float texel = 1.0 / lighting.shadowParams.x;
    ... 3×3 盒式 PCF 逐 tap 硬比较，采样 samplerShadowDepth[cascade] ...
}

float shadowVis = 1.0;
if (lighting.shadowParams.z > 0.5) {
    int c = CascadeIndex(-viewZ);               // §3.3 选片
    shadowVis = CascadePCF(worldPos, c, lighting.shadowParams.y);
}
```

### 4.6 级间边界与出界处理

- **选片边界**：片元 `-viewZ` 落在某档内 → 只采样该档。出片不采样（不跨档插值），
  避免「近档深度与远档深度混比」；
- **出界 = 受光**：片元在该档光空间投影 UV/深度出 [0,1]（物体在该档覆盖范围之外 / 近远
  裁剪之外）→ 返回 1.0（沿 `《阴影贴图实现计划书》§3.4` 的判定语义，杜绝边界一片黑）；
- **级间硬切换**：不同档矩阵/深度不同，切档处阴影可能有一条密度突变缝。阶段 1 **接受**，
  记录为已知（§6.1），后续 blend 消除。

---

## 5. 实施步骤（每步可独立构建验证）

**C1 切分 + 每级光矩阵/阴影视锥（纯 CPU，无视觉变化）**
抽取 `BuildLightVolumeCorners`（全视锥路径行为不变）+ 新增 `SliceCorners`；`UpdateLightParams`
算每级 `cascadeViewProj/splits` + Scene 存每级 `m_ShadowVolume`；`LightParams` 数组化。
验收：画面逐像素不变；断点确认近级体积明显小于远级、矩阵随相机/光方向正确变化；
`cascadeCount=1` 时与现状逐位一致。

**C2 逐级 ShadowMap pass + 逐级批次（FlushShadow 未接采样，视觉不变）**
`m_ShadowMeshes/Batches/InstanceBuffer` 数组化；提交接口加 cascade；Scene 遍历② 逐级；
`PrepareDeferredBatches` 循环；SceneLayer 声明 N 个 pass + N 张深度图；`FlushShadow(ctx,c)`。
验收：RenderDoc 见 N 张深度图、近级密度明显更高；每级只画落该级体积内的物体；
Lighting 未接时主画面无变化。

**C3 Lighting 级联采样**
`LightingUBO` 加 `cascadeViewProj[]/splits/cascadeParams`；binding 7 改数组描述符 +
`FlushLighting` 绑定各级；shader 选片 + `CascadePCF`。
验收：近处阴影清晰、远处密度降低但正确；级边界硬切换（已知）；关闭阴影/级联数=1
回退正常。

**C4 调参与回归**
级数 N / λ / 每级尺寸 / 偏差独立调参；空场景/全透明/纯点光场景各级空过不报错；蒙皮
各级形变正确；MASK 镂空逐级一致；`SetDeferred` 前向回退阴影消失；RenderDoc 确认
「N 段深度写 → Lighting 数组读」屏障正确、池按 `(desc,usage)` 复用无重复分配。

---

## 6. 风险与注意

1. **级间硬边（seam）**：切档处阴影密度突变产生可见缝。阶段 1 接受；消除需 blend
   （每片元按距边界的权重对相邻两级做线性混合，列 §7）。
2. **阴影边缘爬动（texel 抖动）**：每级 AABB 随相机逐帧重投影，阴影边缘缓慢爬动——
   单级已有、CSM 逐级同样。texel 稳定化（AABB 对齐 texel 网格）列 §7。
3. **远级密度低于现状单张**：远级 2048² 比现状 4096² 覆盖同区域时密度减半——远景
   更糊是主动取舍；可给远级配更大分辨率，或接受近处收益。调参窗口（每级尺寸独立）留够。
4. **每级共享偏差的局限**：近级密度高、远级密度低，同一 `bias` 近级可能 Acne、远级
   可能 Peter Panning。阶段 1 共享可调；每级独立偏差列 §7（同理 PCF 半径）。
5. **描述符数组改动**：Lighting binding 7 改数组描述符，需同步 pipeline layout 与该
   绑定处的绑定逻辑（`FlushLighting` 的 `BindImage` 改为逐级绑定或一次性绑数组）。
6. **多级 pass 开销**：N 次深度 pass（每级一次 draw 循环）。每级剔除已把提交量压到
   「各级体积内物体之和」；超大场景可继续做「阴影 pass 并入相机视锥剔除结果」优化
   （《阴影贴图实现计划书》§9 已列）。
7. **蒙皮关节变换 ×N**：蒙皮角色跨多档时每档都做一次顶点变形。每级剔除后总量受控，
   阶段 1 接受；Shadow LOD / 距离裁剪列后续。
8. **兼容回退**：`cascadeCount=1` 必须与现状逐位一致（§4.2 同核心 + C1 验收兜底），
   防止 CSM 落地过程把单级阴影带坏。

---

## 7. 后续阶段（本计划书不做）

- **级间 blend**：片元接近切档边界时对相邻两级深度图加权混合，消除硬边；配合选片
  权重过渡，属常规 CSM 收尾项；
- **texel 稳定化（阴影相机稳定）**：每级 AABB 对齐光空间 texel 网格，消除边缘爬动与
  逐帧重投影闪烁；
- **每级独立偏差 / PCF 半径 / 分辨率**：按级密度自适应（近级小偏差、远级大偏差 /
  大 PCF 核），配合「近大远小」的尺寸配置；
- **单趟级联归属**：第二遍遍历一次算每物体命中档位 bitmask 再批量入各档集合，省
  去逐级重复遍历（与 S 系列「单次提交分流」优化同向）；
- **前向路径级联**：FrameUBO 同步级联字段与采样器数组；
- **CSM + 阴影预算整合**：与相机视锥剔除结果合并、Shadow LOD / 距离裁剪，进一步压
  多级深度 pass 开销。
