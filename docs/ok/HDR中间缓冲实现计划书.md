# HDR 中间缓冲实现计划书（RGBA16F + 独立 Tonemap Pass）

> 承接《延迟渲染实现计划书》：在该计划中 HDR 中间缓冲被列为下一阶段，目的是为
> **bloom / 曝光**铺路，并解决"透明混合发生在 ACES 之后"的妥协（延迟计划 §9.3）。
> 本文以当前已落地的 RenderGraph（声明序执行 + 自动屏障 + 虚拟资源池）为基准，
> 给出 HDR 中间缓冲的接入设计、pass 链调整、着色器改动与实施/验收步骤。

> 更新：2026-09-06，初稿。基于延迟渲染链已落地（GBuffer → Lighting → Transparent，
> 前向/延迟开关已存在）与虚拟资源池已就绪（提交 `21d80987`）的状态编写。

---

## 1. 目标与范围

在**延迟渲染链**内部引入一块帧内 HDR 中间缓冲：

- **Lighting Pass 输出 RGBA16F** 线性 HDR 颜色（含 >1.0 的高光/自发光）；
- 新增**独立 Tonemap Pass**，采样 HDR 中间缓冲、应用曝光与 ACES，写入现有视口
  颜色缓冲（LDR / swapchain 格式，供 Scene2D 与 ImGui 采样）；
- 保持天空盒"环境图直接输出、不 tonemap"的现状语义（见 §5.3，用 alpha 旗标隔离）；
- 让透明段最终在 HDR 空间合成（阶段 B），解决延迟计划 §9.3 的 ACES 后混合妥协；
- 为后续 **bloom / 曝光控制** 预留明确插入点：bloom 在 `Transparent → Tonemap`
  之间读 HDR，曝光参数并入 Tonemap UBO。

**本阶段不做**：bloom 实际实现、自动/手动曝光 UI、HDR 视口显示/调色、compute
tonemap、HDR 资源直接与 ImGui 采样。本文只把 HDR 缓冲和 Tonemap 骨架接到现有
Raster 管线上。

---

## 2. 现状与缺口

### 2.1 当前延迟链

```
延迟： GBuffer ──→ Lighting ──→ Transparent ──→ Scene2D ──→ UIPass
       4×G + 深度  写视口颜色    颜色/深度 eLoad  叠加精灵
                  （已 ACES）
```

关键现状事实：

| 事实 | 位置 | 对 HDR 计划的影响 |
|---|---|---|
| `Lighting` 直接写 `ViewportColor`，并在片元内调 `acesToneMap` | `deferred_lighting.frag:71/:290` | 要拆成"Lighting 写 HDR + Tonemap 写视口"两步 |
| 透明 Pass 在 Lighting 之后、颜色附件 = `ViewportColor` | `SceneLayer.cpp:316-334` | 如果透明段要参与 HDR 合成，须改到 Tonemap 之前 |
| `Renderer3D::FlushLighting` 只按 `ctx.colorAttachmentView->get_format()` 配管线 | `Renderer3D.cpp:796-804` | Lighting 改 RGBA16F 时管线格式自然跟随，无需传格式参数 |
| 虚拟资源池已支持 `CreateVirtualResource` 按 (format/extent/samples/usage) 分配并跨帧复用 | `SceneLayer.cpp:201-213`、渲染图虚拟资源池计划书 | HDR 缓冲直接走虚拟资源，无需持久纹理/管理器 |
| `PassExecuteContext::readImageViews` 已透传 `readImages` | `RenderPassDesc.h:80-90`、`Renderer3D.cpp:826-855` | Tonemap 读取 HDR 缓冲可直接用现有 readImages 机制 |
| 透明管线的混合状态由 `ConfigureMeshPipeline(transparent=true)` 设置 | `Renderer3D.cpp:1265-1313` | HDR 透明合成需调整 alpha 通道混合以维护"天空/几何"元数据（§5.3） |

### 2.2 两个必须解决的语义问题

1. **天空盒不应被 tonemap**：当前 `Lighting` 的 sky 分支直接输出环境图颜色，不经过
   ACES。若简单把整张 Lighting 输出接进 Tonemap，天空会从"不 tonemap"变成"tonemap"，
   属于回归。
2. **透明合成应在 tonemap 之前**：当前延迟下透明 Pass 叠在已 ACES 的视口颜色上。
   HDR 化的本意是让所有 3D 场景内容在线性 HDR 空间合成后再统一 tonemap。

---

## 3. 目标 Pass 链

最终目标（含透明 HDR 合成）：

```
现状延迟： GBuffer → Lighting(Viewport/LDR) ─→ Transparent(Viewport) ─→ Scene2D → UIPass

HDR 链：   GBuffer → Lighting(HDR) → Transparent(HDR) → Tonemap → Scene2D → UIPass
                           ↑ RGBA16F                    ↑ 读 HDR，写 ViewportColor
                                                     Bloom 将来插在这里
```

阶段 B 之前的中间态（透明仍走旧链，仅先让 Lighting 和 Tonemap 落地）：

```
中间态：  GBuffer → Lighting(HDR) → Tonemap → Transparent(LDR 旧语义) → Scene2D → UIPass
```

本文按**最终目标**写设计，但实施步骤允许先走中间态再接透明 HDR（§8）。

---

## 4. 资源设计

新增一块帧内虚拟资源，只存在于延迟链：

```cpp
RenderGraphResourceDesc hdrDesc;
hdrDesc.extent  = extent;              // 与视口一致
hdrDesc.samples = vk::SampleCountFlagBits::e1;
hdrDesc.format  = vk::Format::eR16G16B16A16Sfloat;
ResourceHandle hHDR = b.CreateVirtualResource(hdrDesc, "Scene_HDR");
```

- **格式选 RGBA16F 而非 R11G11B10F**：需要 alpha 通道存储"是否需要 tonemap（天空=0，
  几何/合成=1）"元数据（§5.3），同时为后续曝光权重/调试预留空间。
- **usage**：由虚拟资源池从 pass 声明自动推导为 `COLOR_ATTACHMENT | SAMPLED`
  （Lighting/Transparent 写，Tonemap 读）。
- 现有资源不变：
  - `ViewportColor` / `ViewportDepth` 仍是外部导入资源；
  - `ViewportColor` 最终仍由 `Scene2D` 收尾转 `ShaderReadOnlyOptimal` 供 ImGui 采样；
  - `ViewportDepth` 仍由 `GBuffer` 写、`Transparent`/`Scene2D` 读。

---

## 5. Pass 声明改动（SceneLayer.cpp）

在 `SceneLayer::RecordScenePasses` 的延迟分支（当前在 `SceneLayer.cpp:201` 附近）做以下调整。

### 5.1 Lighting Pass 改写 HDR

- 声明 `hHDR` 虚拟资源（§4）；
- `lightingPass.colorAttachments` 从 `hColor` 改为 `hHDR`，loadOp 仍 `eClear`，
  clearValue 沿用 `clearColor`（按线性 HDR 输入处理）；
- `lightingPass.readImages` 不变（仍读 G0~G3、可选阴影/IBL 资源）；
- execute 回调不变：`Renderer3D::FlushLighting(ctx)`。

### 5.2 Transparent Pass 改接 HDR（阶段 B）

- 颜色附件从 `hColor` 改为 `hHDR`，`loadOp = eLoad`（叠加到 Lighting 结果上）；
- 深度附件保持 `hDepth` eLoad；
- execute 回调不变：`Renderer3D::FlushTransparent(ctx)`；
- 若先走中间态（Transparent 仍在 Tonemap 后），本段可延后。

### 5.3 Tonemap Pass

在 Lighting/Transparent 之后、Scene2D 之前新增：

```cpp
RenderPassDesc &tonemapPass = b.AddPass("Tonemap");
tonemapPass.renderArea = renderArea;
tonemapPass.readImages.push_back(
    {hHDR, ResourceUsage::ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal});

AttachmentDesc tonemapColor;
tonemapColor.resource = hColor;
tonemapColor.usage = ResourceUsage::ColorAttachment;
tonemapColor.loadOp = vk::AttachmentLoadOp::eClear;
tonemapColor.storeOp = vk::AttachmentStoreOp::eStore;
tonemapColor.clearValue.color = {0.0f, 0.0f, 0.0f, 0.0f};
tonemapPass.colorAttachments.push_back(tonemapColor);

tonemapPass.execute = [](PassExecuteContext &ctx) {
    Renderer::Get3DRenderer().FlushTonemap(ctx);
};
```

#### 天空不 tonemap 的 alpha 旗标约定

HDR 缓冲的 alpha 通道同时承担"天空/几何"元数据：

| 像素来源 | HDR.rgb | HDR.a |
|---|---|---|
| Lighting 天空分支（无 skybox 时背景色） | 环境/背景 HDR 颜色 | `0.0` |
| Lighting 几何分支 | 线性 HDR 光照结果 | `1.0` |
| Transparent 合成后（覆盖天空或几何） | 线性 HDR 混合结果 | 强制 `1.0`（见 §6.3） |

Tonemap 采样后判断：

```glsl
vec4 hdr = texture(samplerHDR, inUV);
vec3 color = hdr.rgb;
if (hdr.a >= 0.5) {
    color *= tonemap.exposure.x;
    if (tonemap.flags.x > 0.5) color = acesToneMap(color);
}
outColor = vec4(color, 1.0);
```

这样天空保持"不 tonemap、不曝光"；被透明覆盖的像素因 alpha 被强制为 1，会正常
tonemap。

---

## 6. Renderer3D 改动

### 6.1 Lighting 着色器松绑 ACES

`assets/shaders/glsl/deferred_lighting.frag` 最后一行改为：

```glsl
outColor = vec4(result, 1.0);       // 几何：HDR，alpha 元数据 = 1
```

天空分支改为：

```glsl
outColor = vec4(color, 0.0);        // 天空：HDR，alpha 元数据 = 0
```

`acesToneMap` 函数从本文件移除，改由 `tonemap.frag` 持有。`kSkyThreshold` 等
GBuffer 哨兵逻辑不变。

### 6.2 新增 Tonemap 着色器

- `assets/shaders/glsl/tonemap.vert`：与 `deferred_lighting.vert` 相同的全屏三角形；
- `assets/shaders/glsl/tonemap.frag`：

```glsl
#version 460
layout(set = 0, binding = 0, std140) uniform TonemapUBO
{
    vec4 exposure; // x = 曝光系数（默认 1.0），yzw 预留
    vec4 flags;    // x = tonemap 开关，y = 是否启用天空 alpha 旗标，zw 预留
} tonemap;

layout(set = 0, binding = 1) uniform sampler2D samplerHDR;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

vec3 acesToneMap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) /
                 (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec4 hdr = texture(samplerHDR, inUV);
    vec3 color = hdr.rgb;
    if (tonemap.flags.y > 0.5 && hdr.a < 0.5)
    {
        // 天空像素：直接输出，不曝光不 tonemap（维持现状语义）
        outColor = vec4(color, 1.0);
        return;
    }

    color *= tonemap.exposure.x;
    if (tonemap.flags.x > 0.5)
    {
        color = acesToneMap(color);
    }
    outColor = vec4(color, 1.0);
}
```

### 6.3 Renderer3D 新的全屏 Pass

- `Renderer3D.h` 新增：
  - `void FlushTonemap(PassExecuteContext &ctx);`
  - `void ConfigureTonemapPipeline(VulkanCommandBuffer &cmd, vk::Format colorFormat, vk::Extent2D extent);`
  - `BufferAllocation UploadTonemapUBO(VulkanRenderFrame &frame);`
  - `TonemapUBO` 结构体（std140）；
  - 新着色器模块/布局句柄：`m_TonemapVert`、`m_TonemapFrag`、`m_TonemapLayout`。
- `FlushTonemap` 实现：
  - 从 `ctx.colorAttachmentView` 取最终颜色格式；
  - `UploadTonemapUBO`（默认 `exposure.x = 1.0`、`flags.x = 1`、`flags.y = 1`）；
  - `ConfigureTonemapPipeline` 绑定 layout、关闭深度、无混合、全屏三角形视口/剪刀；
  - descriptor：set 0 binding 0 = Tonemap UBO；set 0 binding 1 = `ctx.readImageViews[0]`
    （即 `Scene_HDR`），采样器复用 `m_DefaultWhiteTexture->GetSampler()`（线性过滤）。

### 6.4 透明 HDR 合成（阶段 B）

当前透明段复用 `mesh.frag / mesh_pbr.frag`，其中 `mesh_pbr.frag` 已在片元内调用
ACES。要在 Tonemap 前合成，透明 PBR 输出必须保持线性 HDR：

- 新增/生成无 tonemap 的 PBR 片元变体，例如 `mesh_pbr_hdr.frag`（Blinn 的
  `mesh.frag` 本身不做 ACES，可复用；PBR-IBL 变体同源处理）。也可以用
  `-DHDR_COMPOSE` 编译宏，但与当前每源码文件一枚 .spv 的构建方式相比，单独文件
  更直观；
- `Renderer3D::ConfigureMeshPipeline` 在 HDR 透明路径下使用这些变体对应 layout；
- 混合状态改为：
  - `srcColorBlendFactor = SRC_ALPHA`、`dstColorBlendFactor = ONE_MINUS_SRC_ALPHA`
    （颜色按正常 straight alpha 混合）；
  - `srcAlphaBlendFactor = ONE`、`dstAlphaBlendFactor = ZERO`
    （保证 HDR alpha 元数据始终收敛到 1，防止"透明盖住天空后仍被判为天空"）。

---

## 7. 曝光与后续 Bloom 的预留

- `TonemapUBO.exposure.x` 是曝光乘数；已由视口相机（编辑器相机 / 游戏主相机）
  每帧接入（默认 1.0），后续可按需拓展到场景组件/自动曝光；
- `TonemapUBO.flags` 预留 tonemap 开关、天空旗标开关；调试时可通过编辑器临时关掉
  tonemap 看 HDR 原始值；
- future Bloom 插入点：`Transparent → Tonemap`。Bloom Pass 读 `Scene_HDR`，把阈值
  后的高光写入独立 bloom 缓冲或直接加到 HDR；Tonemap 之前完成合成，曝光和 tonemap
  仍在最后统一处理。

---

## 8. 实施步骤（每步可独立构建验证）

**S1 Lighting 输出 HDR + Tonemap 骨架（无透明变化）**
1. `SceneLayer` 声明 `Scene_HDR`；
2. `Lighting` 改写给 `Scene_HDR`，`deferred_lighting.frag` 去掉 ACES、sky alpha=0、
   几何 alpha=1；
3. 新增 `tonemap.vert/.frag`、`FlushTonemap`、`ConfigureTonemapPipeline`、`TonemapUBO`；
4. 在 `Lighting` 后、`Transparent` 前插入 `Tonemap`（中间态链条）；
5. `Transparent` 仍写 `ViewportColor`。

验收：不透明/天空画面与前向/原延迟逐像素基本一致；RenderDoc 可看到 `Scene_HDR`
为 RGBA16F、高光/自发光 >1.0；天空像素 alpha=0、几何 alpha=1；Tonemap 输出视口颜色
供 ImGui 采样正常。

**S2 Transparent 移入 HDR（解决 §9.3）**
1. `Transparent` 颜色附件改为 `Scene_HDR`；
2. 新增透明 HDR 片元变体并接入管线布局；
3. 调整 HDR 透明混合 alpha 因子为 `ONE / ZERO`；
4. Pass 顺序改为 `Lighting → Transparent → Tonemap`。

验收：透明物体在线性 HDR 空间合成后再 tonemap；半透明高光不再被前序 ACES 截断；
透明覆盖天空时 HDR alpha=1，被正确 tonemap；原透明排序/深度遮挡不变。

**S3 开关与回归收尾**
- 前向路径不变，`IsDeferred` 开关继续可用；HDR 链只在延迟路径生效；
- 可临时加 `m_HDR` / 编辑器开关（推荐：并入现有调试面板，默认随延迟开启）；
- 全量回归：glTF 蒙皮、MASK/双面、透明度、天空盒开关、无天空盒背景、视口 resize、
  池 LRU（HDR 资源随 resize 复用/淘汰）、RenderDoc 帧捕获对比。

---

## 9. 风险与注意

1. **RGBA16F 带宽/内存**：视口颜色从 8bit×4 变为 16F×4，约 2× 内存与带宽；走虚拟
   资源池不持有持久纹理，且当前单视口影响有限。若担忧可利用 `R16G16B16A16F` 的
   后半部分仅在需要采样时驻留，暂不做优化。
2. **天空不 tonemap 的 alpha 元数据**：必须同步维护好三类来源（sky/geometry/
   transparent）。若透明混合未把 alpha 强制为 1，透明覆盖天空的像素会被 Tonemap
   误判为天空而跳过 tonemap。
3. **clearColor 颜色空间**：现有 `clearColor` 被视为线性输入。若未来从 UI 以 sRGB
   取背景色，应在写入 HDR 前转换；本阶段不动，避免引入不确定的颜色语义。
4. **曝光默认 1.0**：Tonemap 接曝光后默认不改变画面。测试时可用 RenderDoc 的 shader
   调试确认 `exposure`/`flags` 上传正确。
5. **PBR 透明 HDR 变体**：`mesh_pbr.frag` 的 ACES 必须在 HDR 变体中移除，否则透明
   会先 tonemap 再混合（又回到 §9.3）。注意把 `.spv` 与 CMake 的 shader glob 同步，
   避免运行期用旧二进制。
6. **旧 .spv 缓存**：若 assets 下存在无对应源码的 .spv（如当前 `mesh_pbr_ibl.frag.spv`），
   新增/修改着色器后应确认 CMake 编译产物与实际源码一致；必要时清理陈旧 .spv。
7. **调试体验**：`Scene_HDR`/`Tonemap` 的 debug name 已由图/pass 链路承载，RenderDoc
   按名可查。建议在 `deferred_lighting.frag` 与 `tonemap.frag` 中把 alpha 旗标约定写成
  注释，防止后续误用。

---

## 10. 后续阶段

- **Bloom**：在 `Transparent → Tonemap` 之间插入 Bloom Pass，读 `Scene_HDR`；
- **曝光控制**：Tonemap UBO 接入相机/场景曝光参数，可选自动曝光（计算 HDR 亮度直方图
  或 Uniform Exposure）；
- **HDR 调试视图**：编辑器可切换查看 HDR 原图/exposure/tonemap 前后，便于调试；
- **MSAA / compute tonemap**：若后续上 MSAA 或多视口，可评估 compute tonemap 与
  更紧凑的 HDR 格式。
