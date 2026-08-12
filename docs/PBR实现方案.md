# PBR 实现方案：从 Blinn-Phong 到物理渲染

## Context

当前 `Renderer3D` 使用 Blinn-Phong 光照模型（`assets/shaders/glsl/mesh.frag`），
核心是两个光照函数 `calcDirectionalLight` / `calcPointLight`，用
`pow(NdotH, shininess)` 近似高光。材质系统（`Material`）已具备纹理槽位、
标量参数、着色器类型枚举（`Type::BlinnPhong`，已预留 `PBR`）。实例化合批、
法线贴图、切线空间 TBN、每材质 UBO 均已就绪。

本方案把 Blinn-Phong **替换**为 Cook-Torrance 物理 BRDF（金属-粗糙度工作流），
不改变渲染架构，只改片元着色器的光照函数 + 补充材质数据 + 环境光。分阶段推进，
每步可独立验证、不破坏现有 Blinn-Phong 路径。

> **约定**：PBR 是新管线，与 Blinn-Phong 并行。通过 `Material::Type` 切换，
> 不删旧 shader，避免回归风险。

---

## 阶段总览

| 阶段 | 主题 | 核心收益 | 需要新资源 |
|------|------|----------|------------|
| **P0** | Cook-Torrance 核心 BRDF（直接光） | 物理正确的高光/金属/粗糙度观感 | 否 |
| **P1** | 材质类型切换 + 管线路由 | 一个引擎内 PBR / Blinn-Phong 共存 | 否 |
| **P2** | MetallicRoughness 数据通路 | 金属度/粗糙度可控（贴图 + 标量 fallback） | 可选一张贴图 |
| **P3** | 线性空间 + 色调映射 | PBR 数学正确性的前提 | 否 |
| **P4** | IBL 环境光 | 质感跃升（漫反射辐照 + 镜面预滤波 + BRDF LUT） | .hdr 环境贴图 |

P0–P3 是"直接光 PBR"，一个里程碑可完成；P4 是独立大工程，单独做。

---

## P0：Cook-Torrance 核心 BRDF（直接光）

### 目标
新增 `mesh_pbr.frag`，实现金属-粗糙度工作流的 GGX BRDF，替代 Blinn-Phong 的
`pow(NdotH, shininess)`。先只做方向光 + 点光源的直接光照，环境光用常量近似。

### 关键代码（核心 BRDF 三件套）

```glsl
const float PI = 3.14159265359;

// 菲涅尔：绝缘体 F0=0.04，金属用 albedo
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX 法线分布
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith 几何遮蔽（直接光用 k=(r+1)²/8）
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

// 单个光源贡献
vec3 calcDirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance,
                     vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

    float NDF = distributionGGX(N, H, roughness);
    float G   = geometrySmith(N, V, L, roughness);

    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic); // 金属漫反射为 0

    return (kD * albedo / PI + specular) * radiance * NdotL;
}
```

方向光 / 点光源照搬现有循环结构，只把每个光源的 `radiance` 传入 `calcDirectLight`。

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/mesh_pbr.frag`（新） | 核心 BRDF + 直接光循环 + 常量环境光 |

**验证**：替换材质类型的物体在直接光下高光呈物理正确的"金属亮 / 绝缘体柔"差异。

---

## P1：材质类型切换 + 管线路由

### 目标
让 `Material::Type::PBR` 真正生效，一个引擎内 PBR 与 Blinn-Phong 并存。

### 关键设计
1. `Material::Type` 枚举启用 `PBR`：
   ```cpp
   enum class Type { BlinnPhong = 0, PBR = 1 };
   ```
2. `Renderer3D` 构造时加载两套 shader 模块（`m_FragShaderBP` / `m_FragShaderPBR`），
   或按 `Material::GetType()` 在 `DrawMesh` 时记录到 `MeshInstance`，`EndScene` 按
   `sortKey.pipelineId` 分组，分别绑定对应管线。
3. 复用阶段 4 路线图的 `pipelineId` 字段：PBR 物体 `pipelineId=1`，与 Blinn-Phong
   （0）自动分组，减少管线切换。

### 改动文件
| 文件 | 改动 |
|------|------|
| `GE/include/GE/Render/Material.h` | 启用 `Type::PBR` |
| `GE/include/GE/Render/Renderer3D.h` | 增加 PBR frag shader 成员、`MeshInstance` 记录类型 |
| `GE/src/Render/Renderer3D.cpp` | 加载 PBR shader、`EndScene` 按 pipelineId 路由管线 |

**验证**：同一场景混用两种材质，均正确渲染，RenderDoc 中 PBR / BP 各一条管线。

---

## P2：MetallicRoughness 数据通路

### 目标
金属度 / 粗糙度可调。首选 glTF 惯例的 `MetallicRoughness` 贴图
（B=metallic, G=roughness），无贴图时用标量 fallback。

### 关键设计
1. `Material::TextureSlot` 增加槽位：
   ```cpp
   enum TextureSlot : size_t {
       Albedo = 0,
       Normal = 1,
       Emissive = 2,
       MetallicRoughness = 3,  // B=metallic, G=roughness
       Count
   };
   ```
2. `MaterialUBO` 增加一个 `vec4`（std140，保持 16 对齐）：
   ```glsl
   layout (set = 1, binding = 2, std140) uniform MaterialUBO {
       vec4 params;   // x=shininess(弃用), y=specularStrength, z=emissiveStrength
       vec4 pbr;      // x=metallic, y=roughness
   } material;
   ```
   C++ 端 `Renderer3D.h` 的 `MaterialUBO` 同步加 `glm::vec4 pbr;`。
3. shader 采样：
   ```glsl
   vec4 mr = texture(samplerMetallicRoughness, inUV, 0.0);
   float metallic  = mr.b  * material.pbr.x;   // 贴图 × 标量系数
   float roughness = mr.g  * material.pbr.y;
   ```
   无贴图时绑定默认纹理（B=1, G=1，即标量原值），或直接回退标量。

### 改动文件
| 文件 | 改动 |
|------|------|
| `GE/include/GE/Render/Material.h` | 加 `MetallicRoughness` 槽位 |
| `GE/include/GE/Render/Renderer3D.h` | `MaterialUBO` 加 `pbr` 字段 |
| `GE/src/Render/Renderer3D.cpp` | 默认 MR 纹理、管线 layout 加 binding |
| `assets/shaders/glsl/mesh_pbr.frag` | 采样 MR 贴图 |

**验证**：金属度 0→1 物体从绝缘体渐变到全金属镜面；粗糙度调高高光越柔。

---

## P3：线性空间 + 色调映射

### 目标
PBR 数学在**线性空间**才成立。分两个动作：输入 sRGB→线性，输出做 ACES + gamma。

### 关键设计
1. **Albedo 贴图**采样后转线性（法线 / metallic / roughness 是数据，**不做** gamma）：
   ```glsl
   vec3 albedo = pow(texture(samplerColor, inUV, 0.0).rgb, vec3(2.2)) * inColor.rgb;
   ```
2. 输出前 ACES 色调映射 + gamma 编码：
   ```glsl
   vec3 aces(vec3 x) {
       return clamp((x * (2.51 * x + 0.03)) /
                    (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
   }
   outFragColor = vec4(pow(aces(result), vec3(1.0 / 2.2)), 1.0);
   ```

> **注意**：若 Blinn-Phong shader 目前不转线性，PBR 物体与其混用会亮度不一致，
> 属预期差异（PBR 更物理正确），后续可统一反迭代到 BP。

### 改动文件
| 文件 | 改动 |
|------|------|
| `assets/shaders/glsl/mesh_pbr.frag` | 线性解码 + ACES 色调映射 |

**验证**：高光处不再过曝成纯白，暗部有层次，颜色在 0~1 内不裁切。

---

## P4：IBL 环境光（独立里程碑）

### 目标
替换常量环境光，用基于图像的光照补足观感。三件套：
辐照度图（漫反射）+ 预滤波环境图（镜面）+ BRDF LUT。

### 关键设计
1. **资源**：`.hdr` 环境 Cubemap（如亮度图）。需要一个 `HDR>>Cubemap` 加载路径。
2. **离线/运行时烘焙**（三组 shader）：
   - 辐照度卷积 shader → 低分辨率 Cubemap（漫反射环境光）
   - GGX 预滤波 shader → 各粗糙度 mip 的镜面环境 Cubemap
   - BRDF 积分 shader → 2D LUT（随 roughness/NdotV 变）
3. **片元着色器接入**（split-sum 三项）：
   ```glsl
   vec3 irradiance = texture(uIrradianceMap, N).rgb;
   vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
   vec3 kD = (1.0 - F) * (1.0 - metallic);
   vec3 diffuseIBL = irradiance * albedo * kD;

   vec3 R = reflect(-V, N);
   vec3 prefiltered = textureLod(uPrefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
   vec2 brdf = texture(uBrdfLUT, vec2(max(dot(N, V), 0.0), roughness)).xy;
   vec3 specularIBL = prefiltered * (F * brdf.x + brdf.y);

   vec3 ambient = diffuseIBL + specularIBL;
   ```
   其中 `fresnelSchlickRoughness` 用粗糙度混合 F0，避免菲涅尔在粗糙表面过亮。

### 工程取舍
- **运行时 compute 烘焙**：灵活，但需三组 compute shader + 同步，工程量最大。
- **预烘焙资源加载**：用 `libktx` / `gltf-transform` 离线烘焙成 ktx2 加载，
  引擎只需加载代码，推荐先走这条。

### 改动文件（新增）
| 文件 | 改动 |
|------|------|
| `GE/src/Render/EnvironmentMap.cpp`（新） | HDR 加载 + cubemap 转换 + 烘焙管理 |
| `assets/shaders/glsl/ibl_*.comp`（新） | 辐照度 / 预滤波 / BRDF LUT 烘焙 |
| `assets/shaders/glsl/mesh_pbr.frag` | 接入 IBL 采样 |

---

## 关键文件索引

| 文件 | 改动 | 阶段 |
|------|------|------|
| `assets/shaders/glsl/mesh_pbr.frag`（新） | PBR 片元着色器 | P0, P2, P3, P4 |
| `GE/include/GE/Render/Material.h` | 启用 `Type::PBR`、加 `MetallicRoughness` 槽位 | P1, P2 |
| `GE/include/GE/Render/Renderer3D.h` | `MaterialUBO` 加 `pbr`、PBR shader 成员、`MeshInstance` 记类型 | P1, P2 |
| `GE/src/Render/Renderer3D.cpp` | 加载 PBR shader、管线路由、默认 MR 纹理 | P1, P2 |
| `GE/src/Render/EnvironmentMap.cpp`（新） | IBL 烘焙 | P4 |
| `assets/shaders/glsl/ibl_*.comp`（新） | 烘焙 shader | P4 |

---

## 验证方式

1. **P0**：PBR 材质在方向光 + 点光下高光呈物理正确差异（金属亮 / 绝缘体柔）
2. **P1**：混用 PBR 与 Blinn-Phong 材质，均正确渲染，RenderDoc 见两条管线
3. **P2**：金属度 0→1 渐变、粗糙度调高柔化高光
4. **P3**：高光不过曝、暗部有层次、无颜色裁切
5. **P4**：PBR 物体在环境光下反射环境、粗糙度控制反射模糊度

---

## 风险与注意事项

1. **线性空间**：PBR 必须线性计算，忽略会整体偏暗/过曝。法线 / metallic / roughness
   贴图是数据，禁止做 gamma。
2. **IBL 资源**：P4 依赖 `.hdr` 环境贴图，且烘焙工程量最大，务必单独里程碑。
3. **与 Blinn-Phong 共存**：两套 shader 亮度基准不同，混用时观感差异属正常，
   不强行统一。
4. **`MaterialUBO` 对齐**：加 `vec4` 后仍保持 16 字节对齐，`static_assert` 需同步验证。
5. **shaderc / glslang**：若走运行时编译变体，需引入依赖（见阶段 4 路线图）；
   当前 `.spv` 预编译路径优先，离线编译 `mesh_pbr.frag` 即可。