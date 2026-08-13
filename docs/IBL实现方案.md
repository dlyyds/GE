# IBL 环境光实现计划书

> 对应《PBR 实现方案》中的 **P4** 独立里程碑。目标：把 `mesh_pbr.frag` 里那句
> 常量环境光 `result = ambientColor * albedo` 替换为基于图像的光照（IBL），
> 让 PBR 物体能反射环境、粗糙度控制反射模糊度。

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
3. **BRDF LUT**——镜面 BRDF 的 2D 积分表，`(NdotV, roughness) → (scale, bias)`。

### 引擎侧已具备 / 缺失的能力

| 能力 | 状态 | 说明 |
|------|------|------|
| Compute 管线 | ✅ 已有 | `VulkanResourceCache::RequestComputePipeline` + `VulkanComputePipeline` |
| 多 mip / 多层图像 | ✅ 已有 | `VulkanImageBuilder::with_mip_levels / with_array_layers / with_flags` |
| 立方体视图 | ⚠️ 部分 | `VulkanImageView` 支持 `eCube` 类型，但 `Texture` 只建 2D 视图 |
| HDR 加载 | ⚠️ 需做 | `stb_image` 的 `stbi_loadf` 可读 Radiance `.hdr`（float），需走 float 通路 |
| `.comp` 编译 | ❌ 需改 | shader CMake 只 glob `*.vert *.frag`，需加 `*.comp` |
| 计算队列 | ⚠️ 需确认 | 需验证主队列支持 compute（图形队列通常支持） |

---

## 2. 阶段总览

| 阶段 | 主题 | 核心产出 | 依赖 |
|------|------|----------|------|
| **IBL-0** | 环境 Cubemap 资源通路 | HDR → 6 面 Cubemap（float） | 无 |
| **IBL-1** | 辐照度图 | 漫反射环境光 Cubemap | IBL-0 |
| **IBL-2** | 预滤波环境图 | 镜面反射 mip 链 Cubemap | IBL-0 |
| **IBL-3** | BRDF LUT | 2D 积分表 | 无 |
| **IBL-4** | 片元着色器接入 | `mesh_pbr.frag` split-sum 采样 | IBL-1/2/3 |
| **IBL-5** | 场景 / 编辑器接入 | `EnvironmentMap` 类 + 加载 UI + 序列化 | IBL-4 |

每步独立可验证、不破坏现有直接光路径（无 IBL 时回退常量环境光）。

---

## 3. IBL-0：环境 Cubemap 资源通路

### 目标
`.hdr` 等距柱状图（Equirectangular）→ 6 面 Cubemap，float 存储。这是后续所有
烘焙的输入。

### 关键设计

**a) HDR 加载（float 通路）**
用 `stbi_loadf` 读入 Radiance RGBE → `float*`（线性 HDR，无需 gamma）。该 float
数据**直接**上传，不经过 `Texture::LoadFromFile`（那是 RGBA8 的 LDR 通路）。

**b) Cubemap 图像创建**
新增 `EnvironmentMap` 类，内部直接管理 `VulkanImage` / `VulkanImageView`（不强行
塞进 `Texture`，因为 `Texture` 只建 2D 视图）：
```cpp
// 关键创建参数
vk::ImageCreateInfo{
    .imageType  = vk::ImageType::e2D,
    .format     = vk::Format::eR16G16B16A16Sfloat,  // HDR 需要 float
    .arrayLayers = 6,                                // 6 面
    .usage      = eTransferDst | eTransferSrc | eStorage | eSampled,
    .flags      = vk::ImageCreateFlagBits::eCubeCompatible,
};
```
每个面一个 `e2D` 面视图（烘焙时作为存储图/渲染目标写），一个 `eCube` 视图
（片元采样用）。

**c) 等距柱状图 → Cubemap 转换**
用 compute 一次 dispatch 覆盖 6 面 × 分辨率。核心是方向 → 采样坐标：
```glsl
// 面索引 → 主方向（注意 Vulkan 的坐标约定与绕序，见风险节）
vec3 dir = faceDir(gl_GlobalInvocationID, faceIndex, resolution);
vec2 uv = dirToEquirectUV(dir);   // u = atan(z,x)/2π+0.5, v = asin(y)/π+0.5
color = texture(samplerHDR, uv).rgb;
```

### 改动文件
| 文件 | 改动 |
|------|------|
| `GE/src/Render/EnvironmentMap.cpp`（新） | HDR 加载、cubemap 图像/视图创建、转换 dispatch |
| `GE/include/GE/Render/EnvironmentMap.h`（新） | `EnvironmentMap` 类声明 |
| `assets/shaders/glsl/ibl_equirect_to_cube.comp`（新） | 等距柱状图 → cubemap 转换 |
| `assets/shaders/glsl/CMakeLists.txt` | glob 增加 `*.comp` |

**验证**：抓帧看 6 面 cubemap 内容正确、无镜像/翻转，颜色为 HDR 浮点。

---

## 4. IBL-1：辐照度图（漫反射环境光）

### 目标
对每个输出方向 N，在半球内做余弦加权积分：
```
irradiance(N) = ∫_hemisphere L(P, ωi) · cosθ dωi
```
输出低分辨率（建议 32×32）Cubemap，`eR16G16B16A16Sfloat`，无 mip。

### 关键设计
compute dispatch：每 texel 一个方向，蒙特卡洛采样半球（固定伪随机序列，用
Hammersley）：
```glsl
vec3 irradiance = vec3(0.0);
int SAMPLE_COUNT = 512;
for (int i = 0; i < SAMPLE_COUNT; i++) {
    vec2 xi = hammersley(i, SAMPLE_COUNT);
    vec3 localDir = hemisphereSampleCosWeighted(xi);   // 余弦加权围绕 N
    vec3 worldDir = TBN * localDir;                     // TBN 由 N 构建
    irradiance += texture(samplerEnv, worldDir).rgb * cosTheta;
}
irradiance *= PI / SAMPLE_COUNT;
```

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/ibl_irradiance.comp`（新） | 辐照度卷积 dispatch |
| `GE/src/Render/EnvironmentMap.cpp` | 调用 IBL-1 管线，生成辐照度图 |

**验证**：漫反射面片在环境光下呈柔和均匀的"环境浸染"，无方向性高光。

---

## 5. IBL-2：预滤波环境图（镜面反射）

### 目标
对粗糙度 r 生成不同模糊度的镜面反射 Cubemap：`roughness = mip / (mipCount-1)`。
用 GGX 重要性采样，输出带 mip 链的 Cubemap（建议 512 底 + 5 mip）。

### 关键设计
每个 mip 级 dispatch：
```glsl
float roughness = float(mip) / float(mipCount - 1);
vec3 N = faceDir(...);
vec3 R = N;  // 反射方向（输出方向）
vec3 V = R;
for (int i = 0; i < SAMPLE_COUNT; i++) {
    vec2 xi = hammersley(i, SAMPLE_COUNT);
    vec3 H  = importanceSampleGGX(xi, N, roughness);
    vec3 L  = normalize(2.0 * dot(V, H) * H - V);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL > 0.0) {
        float D  = distributionGGX(N, H, roughness);
        float NdotH = max(dot(N, H), 0.0);
        float pdf = D * NdotH / (4.0 * NdotH) + 0.0001;
        float saTexel = 4.0 * PI / (6.0 * res * res);
        float saSample = 1.0 / (SAMPLE_COUNT * pdf + 0.0001);
        float mipLevel = 0.5 * log2(saSample / saTexel);
        prefiltered += textureLod(samplerEnv, L, mipLevel).rgb * NdotL;
        totalWeight += NdotL;
    }
}
prefiltered /= max(totalWeight, 0.001);
```
核心技巧：**采样时用 `textureLod` 按 mip 选择源纹理等级**（`saSample/saTexel`
的 solid-angle 比），消除高粗糙度下的振铃。

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/ibl_prefilter.comp`（新） | GGX 预滤波多 mip dispatch |
| `GE/src/Render/EnvironmentMap.cpp` | 逐 mip 生成预滤波图 |

**验证**：粗糙度 0 的金属球清晰反射环境，粗糙度 1 呈大面积柔光弥散。

---

## 6. IBL-3：BRDF LUT

### 目标
预积分镜面 BRDF 的 `(scale, bias)`：输入 `(NdotV, roughness)`，输出 2D 纹理
（建议 512×512，`eR16G16B16A16Sfloat`）。

### 关键设计
```glsl
float NdotV = gid.x / res + 0.5 / res;
float roughness = gid.y / res + 0.5 / res;
vec3 V  = vec3(sqrt(1 - NdotV*NdotV), 0.0, NdotV);
vec3 N  = vec3(0.0, 0.0, 1.0);
// 对积分 ∫ (F·G·V)/(...) 采样，输出 A/B，见 learnopengl 标准实现
outLUT = vec2(A, B);
```

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/ibl_brdf_lut.comp`（新） | BRDF LUT dispatch |
| `GE/src/Render/EnvironmentMap.cpp` | 生成 LUT 纹理 |

**验证**：LUT 呈对角线渐变（左上亮 → 右下暗）的标准形态。

---

## 7. IBL-4：片元着色器接入（split-sum）

### 目标
在 `mesh_pbr.frag` 中把常量环境光替换为三项 IBL 采样。

### 关键设计
```glsl
// set 1 新增绑定（在 MR 之后）
layout (set = 1, binding = 5) uniform samplerCube samplerIrradiance;
layout (set = 1, binding = 6) uniform samplerCube samplerPrefilter;
layout (set = 1, binding = 7) uniform sampler2D  samplerBrdfLUT;

// 粗糙度参与的菲涅尔：避免粗糙表面在掠射角过亮
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
           pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 替换原常量环境光段
vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
vec3 kS = F;
vec3 kD = (1.0 - kS) * (1.0 - metallic);
vec3 irradiance = texture(samplerIrradiance, N).rgb;
vec3 diffuseIBL = irradiance * albedo * kD;

vec3 R = reflect(-V, N);
vec3 prefiltered = textureLod(samplerPrefilter, R,
                              roughness * MAX_REFLECTION_LOD).rgb;
vec2 brdf = texture(samplerBrdfLUT,
                    vec2(max(dot(N, V), 0.0), roughness)).rg;
vec3 specularIBL = prefiltered * (F * brdf.x + brdf.y);

result += diffuseIBL + specularIBL;
```
需把 `F0 = mix(vec3(0.04), albedo, metallic)` 提到 main 里（现在在 `calcDirectLight`
内），供 IBL 共用。`MAX_REFLECTION_LOD` 与预滤波 mip 数一致（如 4.0）。

**接入方式**：无 IBL 时（`m_EnvironmentMap == nullptr`）回退原常量环境光路径，
保证向后兼容。

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/mesh_pbr.frag` | 加 3 个采样器 + `fresnelSchlickRoughness` + split-sum |
| `GE/include/GE/Render/Renderer3D.h` | PipelineLayout 增加 IBL 绑定、`EnvironmentMap` 成员 |
| `GE/src/Render/Renderer3D.cpp` | 绑定 IBL 描述符；无 IBL 时走旧路径 |

**验证**：金属球清晰反射环境、粗糙度控制模糊度；关掉 IBL 立即回退常量环境光。

---

## 8. IBL-5：场景 / 编辑器接入

### 目标
让 `EnvironmentMap` 成为可加载、可序列化的场景资源，编辑器能换环境贴图。

### 关键设计
1. `EnvironmentMap::LoadFromFile(device, cache, "*.hdr")`：加载 + 烘焙三件套，
   返回 `std::unique_ptr<EnvironmentMap>`（仿 `Texture::LoadFromFile` 风格）。
2. `Renderer3D` 暴露 `SetEnvironmentMap(EnvironmentMap*)`，`EndScene` 时绑定其
   描述符到 PBR 管线。
3. 编辑器（`ResourcePanel` / 材质面板逻辑）加"加载环境贴图"按钮，选中 `.hdr`
   后烘焙并应用。
4. 场景序列化保存环境贴图路径（延续 `SceneSerializer` 的纹理路径序列化方式）。

### 改动文件
| 文件 | 改动 |
|------|------|
| `GE_Editor/src/Panels/ResourcePanel.cpp` | 环境贴图加载入口 |
| `GE/include/GE/Scene/SceneSerializer.h` | 保存环境贴图路径 |
| `GE/src/Render/EnvironmentMap.cpp` | 加载 + 烘焙入口 |

**验证**：编辑器换 `.hdr`，场景立即更新环境反射；重开场景后自动恢复。

---

## 9. 关键文件索引

| 文件 | 改动 | 阶段 |
|------|------|------|
| `GE/include/GE/Render/EnvironmentMap.h`（新） | 环境映射类声明 | IBL-0/1/2/3/5 |
| `GE/src/Render/EnvironmentMap.cpp`（新） | HDR 加载、cubemap 创建、烘焙调度 | IBL-0/1/2/3/5 |
| `assets/shaders/glsl/ibl_equirect_to_cube.comp`（新） | 等距柱状图 → cubemap | IBL-0 |
| `assets/shaders/glsl/ibl_irradiance.comp`（新） | 漫反射辐照度卷积 | IBL-1 |
| `assets/shaders/glsl/ibl_prefilter.comp`（新） | 镜面 GGX 预滤波 | IBL-2 |
| `assets/shaders/glsl/ibl_brdf_lut.comp`（新） | BRDF LUT | IBL-3 |
| `assets/shaders/glsl/mesh_pbr.frag` | split-sum IBL 采样 | IBL-4 |
| `GE/include/GE/Render/Renderer3D.h` / `.cpp` | IBL 绑定、`SetEnvironmentMap` | IBL-4/5 |
| `assets/shaders/glsl/CMakeLists.txt` | glob 加 `*.comp` | IBL-0 |
| `GE_Editor/src/Panels/ResourcePanel.cpp` | 环境贴图加载 UI | IBL-5 |
| `GE/include/GE/Scene/SceneSerializer.h` | 环境贴图序列化 | IBL-5 |

---

## 10. 验证方式

1. **IBL-0**：RenderDoc 看 6 面 cubemap 内容正确、无镜像翻转，颜色为 HDR 浮点。
2. **IBL-1**：纯漫反射（roughness=1, metallic=0）球体呈柔和环境浸染。
3. **IBL-2**：金属球随粗糙度从清晰反射渐变到弥散。
4. **IBL-3**：LUT 呈对角线渐变标准形态。
5. **IBL-4**：PBR 物体在环境光下反射环境；关闭 IBL 立即回退常量环境光。
6. **IBL-5**：编辑器换 `.hdr` 场景更新，重开场景自动恢复。

---

## 11. 风险与注意事项

1. **坐标系与绕序（最大风险）**：Vulkan 的 Y 轴翻转 + cubemap 面绕序与
   OpenGL 不同。等距柱状图 → cubemap、以及片元采样时，必须统一约定，否则
   环境会镜像／上下颠倒。**先用一张已知朝向的 `.hdr`（如天空盒素材）验证**。
2. **`.comp` 编译**：`glslc` 需在 shader CMake glob 中加 `*.comp`，否则新增
   compute shader 不会自动编译为 `.spv`。
3. **float vs HDR 精度**：环境图务必用 `eR16G16B16A16Sfloat`；若用 RGBA8 会
   裁掉高光、导致镜面反射过曝，且 IBL-0 的 HDR 数据会丢失。
4. **计算队列**：烘焙用 compute 需确认主队列的队列族支持 `eCompute`（图形队列
   通常隐含支持）。若需专用队列，要处理队列族转移。
5. **烘焙时机**：照 `Texture::UploadPixels` 的模式，用一次性 command buffer +
   fence 在加载时阻塞烘焙，**不要**每帧重建。
6. **存储图像布局**：烘焙写 `eStorage`，采样读 `eShaderReadOnly`，需在烘焙后
   做布局转换（`image_utils::TransitionLayout`）。
7. **内存**：预滤波米链 + 辐照度 + LUT 一次性占用可观（512³ 预滤波约几 MB），
   `EnvironmentMap` 生命周期由调用方管理，避免加载多张环境图内存失控。
8. **向后兼容**：`SetEnvironmentMap(nullptr)` 应回退常量环境光，保证无环境贴图
   场景不回归。