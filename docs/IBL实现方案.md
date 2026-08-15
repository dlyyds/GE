# IBL 环境光实现计划书

> 对应《PBR 实现方案》中的 **P4** 独立里程碑。目标：把 `mesh_pbr.frag` 里那句
> 常量环境光 `result = ambientColor * albedo` 替换为基于图像的光照（IBL），
> 让 PBR 物体能反射环境、粗糙度控制反射模糊度。

## 0. 方案选型（2026-08-15 定稿）

**采用「全 cmgen 离线烘焙 + KTX 加载 + Filament 采样公式」路线。**
不做运行时环境切换，因此三张 IBL 图全部用 Filament 的 `cmgen` 工具在**构建期
一次性烘焙**成 `.ktx` 资产提交进仓库，引擎只负责加载，不写任何 compute 烘焙
shader。

选型理由（对比旧「运行时 compute 烘焙」方案）：

| 维度 | 离线 cmgen | 运行时 compute |
|------|:---:|:---:|
| 编辑器换环境 | ❌ 不支持 | ✅ 支持 |
| 烘焙代码量 | 0（工具代劳） | `ibl_*` 4 个 compute + dispatch |
| 模型一致性 | ✅ Filament 成套 | 需自洽 GGX 约定 |
| 坐标系适配 | ⚠️ 需转绕序 | ⚠️ 需自建约定 |
| 适用场景 | 环境固定、省代码 | 运行时换环境 |

需求确认结果：
- **不需要运行时切换环境贴图** → 全离线。
- **片元采样用 Filament 公式** → 三张图（diffuse + specular + LUT）成套采样。
- **cmgen 手动跑一次，产物入库** → 不接 CMake 自动烘焙，资产随源码版本管理。

---

## 1. Context：现状与差距

当前 `mesh_pbr.frag`（P0/P2/P3 已完成）：
- Cook-Torrance 直接光（方向光 + 点光源）已就绪，`calcDirectLight` 返回
  `(kD·albedo/π + specular) · radiance · NdotL`。
- 环境光退化为 `vec3 result = frame.ambient.rgb * frame.ambient.w * albedo;`，
  无镜面反射、无粗糙度感知。

IBL 要补的三件套（split-sum 近似）：
1. **辐照度图（Irradiance Map）**——漫反射分量的半球卷积，低分辨率 Cubemap。
2. **预滤波环境图（Prefiltered Env Map）**——镜面分量的 GGX 重要性采样，
   按粗糙度存入 mip 链。
3. **BRDF LUT**——镜面 BRDF 的 2D 积分表，`(NdotV, roughness) → DFG`。

### 引擎侧已具备 / 缺失的能力

| 能力 | 状态 | 说明 |
|------|------|------|
| KTX-Software | ✅ 已接入 | 最近一次 commit 已作为编译期依赖接入（libktx） |
| Cubemap 视图 | ⚠️ 部分 | `VulkanImageView` 支持 `eCube`，但 `Texture` 只建 2D 视图 |
| HDR/EXR 加载 | ✅ 由 cmgen 代劳 | 引擎不再直接读 HDR，只读 cmgen 产出的 `.ktx` |
| float 纹理 | ⚠️ 需做 | `.ktx` → `R16G16B16A16Sfloat` 上传通路 |
| 片元 IBL 采样 | ❌ 需做 | `mesh_pbr.frag` split-sum + Filament 公式 |

计算队列、`.comp` 编译、compute 管线等能力**不再需要**（烘焙全离线）。

---

## 2. 阶段总览

| 阶段 | 主题 | 核心产出 | 状态 |
|------|------|----------|------|
| **IBL-0** | 烘焙（离线） | 预滤波 cubemap + BRDF LUT | ✅ 完成 |
| **IBL-1** | 加载 | `EnvironmentMap` 读入两张图 | ✅ 完成 |
| **IBL-2** | 片元着色器接入 | `mesh_pbr` HAS_IBL 变体 split-sum 采样 | ✅ 完成 |
| **IBL-3** | 场景序列化 | 环境贴图路径入场景 | ⏳ 未做 |

每步独立可验证、不破坏现有直接光路径（无 IBL 时回退常量环境光）。

---

## 1.5 实施记录（2026-08-15，与方案的两点偏差）

实际实现与方案原文有两处偏差，都是为了**可验证的正确性**，请留意：

1. **无独立辐照度 cubemap**：本机 cmgen（`E:\software\filament`）的
   `--ibl-irradiance` 输出为空——新版 Filament 漫反射走**运行时 SH**，不产辐照度
   cubemap。因此辐照度**复用预滤波 cubemap 的最高 mip**（粗糙度≈1 的余弦卷积近似），
   绑定到 binding 5 与 6 共用同一张图。若日后要严格的辐照度图，可单独烘焙替换
   binding 5。
2. **BRDF LUT 自生成而非 cmgen 产物**：cmgen 的 `--ibl-dfg` 只支持 png/exr/dds
   （不支持 ktx），且实测其 LUT 的 v 轴（roughness）写在**图底部**（与
   `texture(lut, vec2(NoV, roughness))` 的 Vulkan 采样方向相反），离线无法验证。
   故用 `assets/ibl/gen_brdf_lut.py` 按标准 GGX split-sum 公式自生成匹配的 LUT
   （rough=0 在 v=0、u=NoV），与片元公式 `E.x*F0 + E.y` 自洽。**LUT 通道语义仍是
   Filament 式**（R=F0 系数、G=菲涅尔尾项），只是几何项用 Schlick 而非 cmgen 的
   高度相关 Smith，属同一 split-sum 的合法变体。

着色器采用**两个变体**而非运行时 flag：`mesh_pbr.frag`（无 IBL，原样）+ 
`mesh_pbr_ibl.frag.spv`（`-DHAS_IBL`）。Renderer3D 按 `m_EnvironmentMap` 分流，
无 IBL 时 PBR 走无 IBL 变体（常量环境光），完全向后兼容。

---

## 3. IBL-0：离线烘焙（cmgen）

### 目标
用 Filament 的 `cmgen` 从源全景图（`DaySkyHDRI065B_4K_HDR.exr`）一次性烘焙
出三张 IBL 图，产物提交进仓库。

### 关键命令

```bash
cd assets/HDRI/DaySkyHDRI065B_4K
cmgen \
  -x ../../ibl \
  --extract-irradiance \
  --extract-blur \
  --size=256 \
  --format=ktx \
  DaySkyHDRI065B_4K_HDR.exr
```

参数说明：
- `--extract-irradiance`：漫反射辐照度 cubemap（建议 32×32 级的低分辨率，cmgen
  按 `--size` 自动定）。
- `--extract-blur`：GGX 预滤波镜面 mip 链 cubemap（底图 `--size`，多 mip）。
- `--brdf-lut`（可选）：BRDF LUT `brdf_lut.ktx`，与输入无关，只需生成一次。
- `--format=ktx`：输出 KTX 容器，供 libktx 加载。
- `--size`：cubemap 单面尺寸，512 为常用底图值。

### 预期产物（提交进 git）

```
assets/ibl/
├── DaySkyHDRI065B_irradiance.ktx    # 辐照度 cubemap
├── DaySkyHDRI065B_prefilter.ktx     # 预滤波 mip 链 cubemap
└── brdf_lut.ktx                     # BRDF LUT（与输入无关）
```

> **注意**：源文件是 `.exr`。cmgen 支持 EXR 输入；若该链路有问题，可先用
> 工具转 `.hdr` 再喂 cmgen。

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/ibl/*.ktx`（新，入库） | 三张烘焙产物 |

**验证**：用 KTX 工具 / RenderDoc 打开三张图，确认 cubemap 无镜像翻转、颜色为
HDR 浮点、预滤波图为 mip 链。

---

## 4. IBL-1：KTX → Cubemap 加载

### 目标
让引擎用 libktx 读入三张 `.ktx`，转成 `VulkanImage` / `VulkanImageView`（float
cubemap），供 `mesh_pbr.frag` 采样。

### 关键设计

**a) `EnvironmentMap` 类（重写为「加载」而非「烘焙」）**
内部直接管理 `VulkanImage` / `VulkanImageView`（不强行塞进 `Texture`，因为
`Texture` 只建 2D 视图）：
```cpp
struct EnvironmentMap {
    // 三张图的图像 + 视图
    vk::Image  irradianceImage;   // 32 级 cubemap
    vk::Image  prefilterImage;    // 512 底 + mip 链
    vk::Image  brdfLUTImage;      // 512×512 2D
    vk::ImageView irradianceView; // eCube
    vk::ImageView prefilterView;  // eCube（带 mip）
    vk::ImageView brdfLUTView;    // e2D
};
```

**b) libktx 读取 + 上传**
用 `ktxTexture2_CreateFromFile` / `ktxTexture_VkUpload`（或 `ktxTexture2_CreateFromStream`
+ manuall 上传），把 KTX 数据转成 Vulkan 图像：
- 格式取 `.ktx` 内嵌的 VkFormat（cmgen 一般输出 `R16G16B16A16Sfloat` 或
  `R32G32B32A32Sfloat`，需确认）。
- cubemap 三张设 `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`，预滤波图建 mip 视图。
- LUT 是普通 2D，无 cubemap 标记。

**c) 布局 / 采样准备**
KTX 上传后布局转 `eShaderReadOnly`，三张图都 `eSampled`。

### 改动文件
| 文件 | 改动 |
|------|------|
| `GE/include/GE/Render/EnvironmentMap.h`（新/重写） | `EnvironmentMap` 类声明 |
| `GE/src/Render/EnvironmentMap.cpp`（新/重写） | libktx 加载、图像/视图创建 |
| `CMakeLists.txt` | 链接 libktx（若未在 `EnvironmentMap` 侧声明） |

**验证**：RenderDoc 看三张 cubemap 内容与 cmgen 产物一致、无绕序翻转。

---

## 5. IBL-2：片元着色器接入（Filament split-sum）

### 目标
在 `mesh_pbr.frag` 中把常量环境光替换为三张图采样，采用 **Filament 成套公式**
（与 cmgen 产物配对）。

### 关键设计

```glsl
// set 1 新增绑定
layout (set = 1, binding = 5) uniform samplerCube samplerIrradiance;
layout (set = 1, binding = 6) uniform samplerCube samplerPrefilter;
layout (set = 1, binding = 7) uniform sampler2D  samplerBrdfDFG;

// Filament 的 DFG LUT 采样：dfg.x = 漫反射项，dfg.y = 镜面/菲涅尔项
vec3 dfg = texture(samplerBrdfDFG, vec2(max(dot(N, V), 0.0), roughness)).rgb;

// 漫反射分量
vec3 irradiance = texture(samplerIrradiance, N).rgb;
vec3 diffuseIBL = irradiance * albedo * kD;

// 镜面分量（reflect + 预滤波 mip）
vec3 R = reflect(-V, N);
vec3 prefiltered = textureLod(samplerPrefilter, R,
                              roughness * MAX_REFLECTION_LOD).rgb;

// Filament FIBL：用 dfg.y 还原镜面 BRDF 积分
// （具体 Fd/Fr 展开按 Filament 实现对齐 dfg 通道语义，见风险节）
vec3 specularIBL = prefiltered * FIBL(dfg, F0, roughness, NoV);

result += diffuseIBL + specularIBL;
```

要点：
- 把 `F0 = mix(vec3(0.04), albedo, metallic)` 提到 `main` 里（现在在
  `calcDirectLight` 内），供 IBL 共用。
- `MAX_REFLECTION_LOD` 与预滤波 mip 数一致（cmgen 底 `--size` 决定，如 4~5）。
- **无 IBL 时**（`m_EnvironmentMap == nullptr`）回退原常量环境光路径，向后兼容。

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/mesh_pbr.frag` | 加 3 个采样器 + `FIBL` + split-sum |
| `GE/include/GE/Render/Renderer3D.h` | PipelineLayout 增加 IBL 绑定、`EnvironmentMap` 成员 |
| `GE/src/Render/Renderer3D.cpp` | 绑定 IBL 描述符；无 IBL 走旧路径 |

**验证**：金属球清晰反射环境、粗糙度控制模糊度；关掉 IBL 立即回退常量环境光。

---

## 6. IBL-3：场景 / 编辑器接入

### 目标
让 `EnvironmentMap` 成为可加载、可序列化的场景资源（环境固定，不运行时切换）。

### 关键设计
1. `EnvironmentMap::LoadFromFile(device, cache, "ibl/*.ktx")`：加载三张烘焙产物，
   返回 `std::unique_ptr<EnvironmentMap>`（仿 `Texture::LoadFromFile` 风格）。
2. `Renderer3D` 暴露 `SetEnvironmentMap(EnvironmentMap*)`，`EndScene` 时绑定其
   描述符到 PBR 管线。
3. 场景序列化保存环境贴图路径（延续 `SceneSerializer` 的纹理路径序列化方式）。
4. **不做运行时换环境 UI**（需求已确认不需要）；资产路径写死在场景 / 资源里。

### 改动文件
| 文件 | 改动 |
|------|------|
| `GE/include/GE/Scene/SceneSerializer.h` | 保存环境贴图路径 |
| `GE/src/Render/EnvironmentMap.cpp` | 加载入口 |
| `GE_Editor/src/Panels/ResourcePanel.cpp` | （可选）展示环境贴图，无加载按钮 |

**验证**：场景加载后 PBR 物体呈正确环境反射；重开场景自动恢复。

---

## 7. 关键文件索引

| 文件 | 改动 | 阶段 |
|------|------|------|
| `assets/ibl/*.ktx`（新） | cmgen 烘焙产物，入库 | IBL-0 |
| `GE/include/GE/Render/EnvironmentMap.h`（新/重写） | 环境映射类声明 | IBL-1/3 |
| `GE/src/Render/EnvironmentMap.cpp`（新/重写） | libktx 加载、视图创建 | IBL-1/3 |
| `assets/shaders/glsl/mesh_pbr.frag` | Filament split-sum IBL 采样 | IBL-2 |
| `GE/include/GE/Render/Renderer3D.h` / `.cpp` | IBL 绑定、`SetEnvironmentMap` | IBL-2/3 |
| `GE/include/GE/Scene/SceneSerializer.h` | 环境贴图序列化 | IBL-3 |

---

## 8. 验证方式

1. **IBL-0**：KTX 工具 / RenderDoc 打开三张图，确认 cubemap 无镜像翻转、HDR 浮点、
   预滤波为 mip 链。
2. **IBL-1**：RenderDoc 看 `EnvironmentMap` 加载的三张图与 cmgen 产物一致。
3. **IBL-2**：金属球随粗糙度从清晰反射渐变到弥散；纯漫反射球呈柔和环境浸染。
4. **IBL-3**：场景加载环境反射正确；重开场景自动恢复。

---

## 9. 风险与注意事项

1. **坐标系与绕序（最大风险）**：cmgen 按 Filament 约定（右手 + Y-up）生成。
   Vulkan cubemap 面绕序与采样的 Y 轴翻转可能不一致，导致环境镜像／上下颠倒。
   **用已知朝向的 HDR 验证**，必要时做面置换或转置。
2. **EXR 输入**：源文件是 `.exr`，确认 cmgen 直接支持；否则先转 `.hdr`。
3. **格式与精度**：确认 cmgen `.ktx` 内嵌 VkFormat 与片元采样一致（应
   `R16G16B16A16Sfloat` 或 `R32G32B32A32Sfloat`）；若为 RGBA8 会丢高光、过曝。
4. **预滤波 mip 数**：片元 `MAX_REFLECTION_LOD` 必须等于预滤波实际 mip 数，
   否则 `textureLod` 越界导致异常。
5. **Filament 公式成套性**：dfg 通道语义（漫反射项 / 镜面项）必须按 Filament
   `Fd/Fr/F_IBL` 实现对齐采样，**不能**用早期 learnopengl `(F0*A+B)` 公式去读
   cmgen 的 LUT——两者通道语义不同，会偏色。
6. **向后兼容**：`SetEnvironmentMap(nullptr)` 应回退常量环境光，保证无环境贴图
   场景不回归。
7. **烘焙时机**：cmgen 手动跑一次，产物入库；`EnvironmentMap` 加载用一次性
   command buffer + fence 阻塞，**不要**每帧重建。