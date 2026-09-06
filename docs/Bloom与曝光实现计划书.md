# Bloom 与曝光控制实现计划书

> 承接《HDR中间缓冲实现计划书》。以当前已落地的链为前提：
> `GBuffer → Lighting → Transparent(HDR) → Tonemap → Scene2D`
> （S1~S3 已完成，Tonemap 开关已接入调试面板）。
> 本计划把 HDR 计划书 §10 的 **Bloom** 与 **曝光控制** 两项细化为可独立构建/验收的分阶段方案。

> 更新：2026-09-06，初稿。
> 实现状态：2026-09-06，**B1（Bloom pass 链骨架）** 与 **B2（编辑器开关 / 阈值 / 强度 / mip 级数）** 已实现。

---

## 1. 目标与范围

最终目标延迟链：

```
GBuffer → Lighting → Transparent → Bloom → Tonemap → Scene2D → UIPass
                                 ↑ 读/写 Scene_HDR（RGBA16F）
```

**Bloom**
- 在 `Transparent → Tonemap` 之间插入 Bloom pass 链；
- 只对「几何/合成」像素的高光提取泛光，**天空不参与**（利用 `Scene_HDR.a` 元数据）；
- 泛光在 **线性 HDR 空间**加回后，再统一进 Tonemap（曝光 + ACES）。

**曝光控制**
- **手动（Uniform Exposure）**：Tonemap UBO 的 `exposure.x` 由相机/场景/编辑器驱动；
- **自动（可选）**：通过 HDR 亮度统计计算曝光，平滑适配场景明暗；
- 保留现有语义：天空不受曝光/tonemap 影响；透明已在 HDR 空间合成。

**本阶段不做**：HDR 调试视图、compute tonemap、多视口 HDR、镜头自动光圈物理仿真等。

---

## 2. 现状与接入点

### 2.1 当前链

```
GBuffer → Lighting → Transparent(HDR) → Tonemap → Scene2D
                                                      ↑
                              Bloom 插入点：Transparent 之后、Tonemap 之前
```

| 现状 | 位置 | 对 Bloom/曝光的影响 |
|---|---|---|
| `Scene_HDR` 为帧内 RGBA16F 虚拟资源，alpha 存天空/几何元数据 | `SceneLayer.cpp` 虚拟资源声明 | Bloom 抽取可读同一资源；几何=1、天空=0 |
| `TonemapUBO` 已有 `exposure.x`（已由相机接入，默认 1.0）与 `flags.x/y` | `Renderer3D.h`、`tonemap.frag` | 曝光直接接入现有 UBO，无需新管线路由 |
| `tonemap.frag` 已实现「曝光 × HDR → ACES → 视口」（天空跳过） | `assets/shaders/glsl/tonemap.frag` | 曝光写入 `exposure.x` 即生效 |
| `FlushTonemap` / `ConfigureTonemapPipeline` 已有全屏三角形范式 | `Renderer3D.cpp` | Bloom 各 pass 复用同一管线范式，开发成本低 |
| `PassExecuteContext::readImageViews` 可透传多张采样图 | `RenderPassDesc.h` | Bloom 链各 pass 用现有 readImages 机制 |

### 2.2 必须遵守的两个语义约束

1. **天空不参与 bloom、不参与曝光**：`Scene_HDR.a < 0.5` 表示天空；Bloom 提取与自动曝光统计都必须跳过这些像素。
2. **bloom 必须在曝光/ACES 之前完成**：高光先在 HDR 空间扩散并加回 `Scene_HDR`，Tonemap 最后统一处理，否则高光会被 ACES 截断后再扩散，泛光失真。

---

## 3. Bloom 设计

### 3.1 Pass 链与资源

```
Transparent
  → BloomExtract      : 读 Scene_HDR，阈值提取几何高光 → BloomDown0（半分辨率）
  → BloomDownsample   : N 级 1/2 降采样 + 模糊，写 BloomDown1..N
  → BloomUpsample     : N-1..0 级升采样 + 模糊，写 BloomUp0（最终泛光层）
  → BloomComposite    : 读 Scene_HDR + BloomUp0，相加写回 Scene_HDR
  → Tonemap
```

**新增虚拟资源**（随 RenderGraph 虚拟资源池按 extent/resize 复用）：

```cpp
// 半分辨率起步，逐级 1/2。初始只用 RGBA16F 与 Scene_HDR 对齐，简单稳妥。
RenderGraphResourceDesc bloomDesc;
bloomDesc.samples = vk::SampleCountFlagBits::e1;
bloomDesc.format  = vk::Format::eR16G16B16A16Sfloat;

for (uint32_t m = 0; m <= maxMip; ++m) {
    bloomDesc.extent = vk::Extent2D{extent.width >> (1 + m),
                                    extent.height >> (1 + m)};
    ResourceHandle hBloomDown[m] = b.CreateVirtualResource(bloomDesc, "Bloom_Down" + m);
    ResourceHandle hBloomUp[m]   = b.CreateVirtualResource(bloomDesc, "Bloom_Up" + m);
}
```

> 说明：bloom mip 不需要 alpha 元数据，后续可考虑 `R11G11B10F` 或仅 R16F 降带宽；先用 RGBA16F 降低格式换算风险。

### 3.2 着色器

建议新增文件（与 `tonemap.vert/frag` 同构，纯全屏三角形 + sampler）：

| 文件 | 职责 |
|---|---|
| `bloom.vert` | 全屏三角形，坐标/UV 与 tonemap 一致 |
| `bloom_extract.frag` | 读 `Scene_HDR`，`hdr.a < 0.5` 直接返回 0（天空排除）；否则 `max(0.0, luminance(hdr.rgb) - threshold)` 或 `hdr.rgb` 过阈值分量 |
| `bloom_downsample.frag` | 双线性 4-tap / box 平均降采样，逐级写 `Bloom_DownN` |
| `bloom_upsample.frag` | 双线性 + 高斯权重升采样，逐级写 `Bloom_UpN` |
| `bloom_composite.frag` | 读 `Scene_HDR` + `Bloom_Up0`，相加写回 `Scene_HDR`；保持 alpha 元数据不变 |

统一参数 UBO（std140）：

```cpp
struct BloomUBO {
    vec4 params; // x = threshold, y = intensity, z = 保留, w = 启用(>0.5)
    vec4 texelSize; // x,y = 当前 mip 1/尺寸，zw 预留
};
```

### 3.3 Renderer3D 接口

```cpp
void SetBloomEnabled(bool enabled);
bool IsBloomEnabled() const;
void SetBloomThreshold(float threshold);
void SetBloomIntensity(float intensity);
void SetBloomMipLevels(uint32_t levels);

// pass 录制
void FlushBloomExtract(PassExecuteContext &ctx);
void FlushBloomDownsample(PassExecuteContext &ctx, uint32_t mip);
void FlushBloomUpsample(PassExecuteContext &ctx, uint32_t mip);
void FlushBloomComposite(PassExecuteContext &ctx);
```

### 3.4 编辑器面板

并入 `SceneLayer` 调试区（与「延迟渲染 / Tonemap」同一面板）：
- `Bloom` 复选框
- `阈值 threshold`（默认 1.0）
- `强度 intensity`（默认 0.7）
- `mip 级数`（默认 5，建议 3~7）

---

## 4. 曝光控制设计

### 4.1 TonemapUBO 扩展

现状：

```cpp
struct TonemapUBO {
    glm::vec4 exposure; // x = 曝光系数，yzw 预留
    glm::vec4 flags;    // x = tonemap 开关，y = 天空旗标开关，zw 预留
};
```

扩展为：

```cpp
struct TonemapUBO {
    glm::vec4 exposure; // x = 最终曝光系数，y = 曝光下限，z = 曝光上限，w 预留
    glm::vec4 flags;    // x = tonemap 开关，y = 天空旗标开关，z = 自动曝光开关，w = 预留
};
```

`tonemap.frag` 变化很小：

```glsl
vec3 color = hdr.rgb;
if (天空旗标 && hdr.a < 0.5) {
    outColor = vec4(color, 1.0); // 天空不曝光
    return;
}
color *= tonemap.exposure.x;  // 已由 CPU 完成手动/自动合并
if (tonemap.flags.x > 0.5) color = acesToneMap(color);
outColor = vec4(color, 1.0);
```

### 4.2 手动曝光（Uniform Exposure）

- **来源**：推荐在 `Camera` 增加曝光字段（`float Exposure = 1.0f`），编辑相机与场景相机共用；或新增 `ExposureComponent` 组件。
- **接线**：每帧由渲染管线读取相机曝光 → `Renderer3D::SetExposure(...)` → `UploadTonemapUBO` 写入 `exposure.x`。
- **编辑器**：SceneLayer 面板加曝光滑杆（0.01 ~ 8.0，默认 1.0）。

### 4.3 自动曝光（可选，推荐渐进式）

#### 方案 A：mip-average（先做，纯 Raster）

1. 新增 `ExposureLog` 链：把 `Scene_HDR` 中几何像素亮度经 2×2 降采样一路缩到 **1×1**（或最小可读回的小图）。
2. 每 **5~10 帧** 做一次 CPU readback（或低频 map + memcpy），避免每帧 GPU 同步卡顿。
3. CPU 侧计算：
   ```
   avgLum = max(avgLum, kEpsilon);
   newExposure = targetLum / avgLum;      // targetLum 默认 0.18
   exposure = exp(lerp(log(exposure), log(newExposure), 1 - exp(-dt / adaptSpeed)));
   ```
4. 写入 `TonemapUBO.exposure.x`。

优点：无 compute，复用现有 Raster 全屏三角形；开发快。
缺点：只是平均亮度，过大高光会拉低曝光，抗闪烁需要时间平滑。

#### 方案 B：直方图（advanced，可选）

1. Compute pass 读 `Scene_HDR`，按亮度对数域分 256 桶直方图；
2. Reduce 桶到少量计数器，CPU/GPU 求解日志平均/加权曝光；
3. 可配置权重依赖、曝光补偿、上下限。

先做方案 A；若后续需要电影感自动曝光，再立项方案 B。

### 4.4 Renderer3D 接口

```cpp
void SetExposure(float exposure);          // 手动曝光
void SetAutoExposureEnabled(bool enabled);
void SetAutoExposureSpeed(float speed);    // 适应速度
void SetExposureRange(float minLum, float maxLum);
```

---

## 5. 实施步骤（每步可独立构建验证）

**B1 Bloom 骨架**
1. 新增 `bloom.vert`、`bloom_extract/downsample/upsample/composite.frag`；
2. `Renderer3D` 加载 shader、建立布局、新增 `FlushBloom*` 与 `BloomUBO`；
3. `SceneLayer` 声明 `hBloomDown/Up` 虚拟资源；
4. 在 `Transparent` 后、`Tonemap` 前插入 Bloom pass 链，Tonemap 改读合成后的 `Scene_HDR`。

验收：场景中高光/自发亮度 > 阈值时出现柔和光晕；天空不被泛光放大；关掉 Bloom 与原画面一致。

**B2 Bloom 参数与回归**
1. 编辑器开关 + 阈值/强度/mip 级数；
2. 验证 resize、透明覆盖天空、RenderDoc 各 mip 资源。

**E1 手动曝光**
1. `Camera`（或 `ExposureComponent`）增加 `Exposure`；
2. `UploadTonemapUBO` 接 `m_Exposure`；
3. 编辑器滑杆。

验收：拉高曝光画面变亮、降低变暗；`exposure=1.0` 与现状无差；天空不亮度变化。

**E2 自动曝光（方案 A）**
1. `ExposureLog` 降采样 pass（几何像素亮度 → 1×1 / 小图）；
2. 低频 CPU readback + 指数平滑；
3. 编辑器：自动曝光开关、适应速度、曝光上下限。

验收：相机从暗处转向亮处时画面平滑过渡、不闪烁；关闭自动曝光回手动值。

**E3 直方图自动曝光（可选）**
- 待 E2 稳定后，作为 compute 进阶项立项。

---

## 6. 风险与注意

1. **天空排除**：Bloom 抽取、曝光统计都必须按 `Scene_HDR.a >= 0.5` 过滤；若透明 alpha 回归（未强制收敛到 1），透明盖天空处会被误判为天空，需回归验证。
2. **曝光顺序**：`color *= exposure` 必须在 ACES 前；天空不走该分支。
3. **GPU readback 卡顿**：自动曝光避免每帧 `vkQueueWaitIdle`；用低频读回 + 时间平滑（≥5 帧/次）。
4. **bloom 带宽**：RGBA16F mip 链从半分辨率开始，每级面积 1/4，总成本可控；若担忧可切 R11G11B10F 或 R16F。
5. **shader 与 .spv 同步**：新增 `.frag` 要确认 CMake `file(GLOB ...)` 已重配，避免运行期用旧 SPIR-V。
6. **resize**：`hBloomDown/Up` 走虚拟资源池，随视口 resize 自动复用/淘汰，无需持久 RG。
7. **参数稳定**：默认 `threshold=1.0`、`intensity=0.7`、`mip=5`，避免开箱即糊/无效果。

---

## 7. 后续扩展

- **HDR 调试视图**：编辑器切看 Scene_HDR / Bloom 各 mip / 曝光前后；
- **曝光补偿**：EV / 曝光补偿滑杆进入自动曝光计算；
- **Compute Bloom**：一次 dispatch 完成多 mip 与模糊，减少 Raster pass 切换；
- **物理曝光**：ISO/快门/光圈到 EV100 的映射，接入手动曝光。
