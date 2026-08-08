# Renderer3D 扩展路线图：材质系统 + 批处理

## Context

当前 `Renderer3D` 采用每个网格一个 draw call 的简单模式，没有独立的材质系统——材质属性（纹理、颜色）扁平地嵌在 `MeshComponent` 和 `DrawMesh()` 参数中。随着场景复杂度上升，两个核心问题会凸显：

1. **draw call 过多**：每个网格独立绑定顶点/索引/纹理/UBO + 独立绘制，没有任何合批
2. **材质表达能力不足**：只有一张主纹理 + 一个 tint 颜色，无法支持多纹理（法线贴图、金属粗糙度贴图等）、着色器变体、混合模式等

本路线图规划了从"简单 Blinn-Phong 单 draw 模式"演进到"材质驱动 + 多维度合批"的分阶段路径，每一步都可独立验证、不破坏现有接口。

---

## 阶段总览

| 阶段 | 主题 | 核心收益 | 改动规模 |
|------|------|----------|----------|
| **阶段 0** | 材质数据层 + 资源管理 | 建立 Material 抽象，解耦渲染参数与组件 | 中 |
| **阶段 1** | 按材质排序 + 状态分组 | 减少管线/纹理切换开销，为后续合批打基础 | 小 |
| **阶段 2** | Dynamic UBO + 同材质合批 | 同材质的多个 mesh 共享 descriptor set，draw 间仅改 dynamic offset | 中 |
| **阶段 3** | GPU Instancing（同 Mesh 同材质） | 相同 mesh + 相同材质的多个实例合并为单个 draw call | 中 |
| **阶段 4** | 着色器变体机制 + 材质特性扩展 | 支持 normal map、emissive、alpha test 等特性开关 | 大 |
| **阶段 5**（可选） | 视锥体剔除 + 前向+/延迟渲染架构 | 大规模场景性能跃升 | 大 |

---

## 阶段 0：材质数据层 + 资源管理

### 目标
建立独立的 `Material` 类，将材质属性从 `MeshComponent` / `DrawMesh` 参数中抽离出来，形成统一的材质资源抽象。

### 关键设计

#### 0.1 Material 类
**文件**：新增 `GE/include/GE/Render/Material.h` + `GE/src/Render/Material.cpp`

```cpp
class Material {
public:
    // 纹理槽位（后续可扩展）
    enum TextureSlot {
        Albedo = 0,    // 主颜色（原 BaseTexture）
        Normal,        // 法线贴图
        Emissive,      // 自发光
        // ... 后续增加 MetallicRoughness 等
        Count
    };

    // 设置 / 获取纹理
    void SetTexture(TextureSlot slot, Texture* tex);
    Texture* GetTexture(TextureSlot slot) const;

    // 材质标量参数
    void SetFloat(const std::string& name, float value);
    float GetFloat(const std::string& name) const;

    // 材质着色器类型（当前只有 BlinnPhong，后续扩展 PBR 等）
    enum class Type { BlinnPhong };
    Type GetType() const { return m_Type; }

    // 渲染状态覆盖（可选，先做默认值）
    bool  alphaTest   = false;
    bool  doubleSided = false;
    // ...

private:
    Type m_Type = Type::BlinnPhong;
    std::array<Texture*, (size_t)TextureSlot::Count> m_Textures{};
    std::unordered_map<std::string, float> m_Params;  // 或使用定长 struct 更高效
};
```

#### 0.2 MaterialManager（全局缓存）
与 `VulkanResourceCache` 类似，但针对运行时材质实例。可以简单起步：一个 `std::unordered_map<std::string, std::unique_ptr<Material>>` + 名称查找。

#### 0.3 MeshComponent 扩展
**文件**：`GE/include/GE/Scene/Components.h`

在 `MeshComponent` 中增加 `Material* MaterialPtr`（保持裸指针风格，与 `MeshPtr` / `BaseTexture` 一致）。保留 `BaseTexture` 和 `Color` 作为向后兼容的快捷方式，或标记为 deprecated，逐步迁移。

#### 0.4 DrawMesh 接口扩展
**文件**：`GE/include/GE/Render/Renderer3D.h`

新增重载：
```cpp
void DrawMesh(const glm::mat4& transform, Mesh* mesh, Material* material);
```

保留原有 `DrawMesh(transform, mesh, texture, color)` 重载，内部构造一个临时/默认材质调用新接口。

#### 0.5 Scene::OnUpdate3D 适配
**文件**：`GE/src/Scene/Scene.cpp`

在收集 `MeshComponent` 时，优先使用 `MaterialPtr`，fallback 到 `BaseTexture` + `Color` 的旧路径。

---

## 阶段 1：按材质排序 + 状态分组

> **状态：✅ 已完成**（2026-08-08）
> - `MeshInstance` 增加 `sortKey`，`DrawMesh` 时计算（管线→纹理→view 深度）
> - `EndScene` 绘制前按 `sortKey` 升序排序（不透明物体从前往后，early-z）
> - 统计 `batches3D`（按有效纹理指针分组的连续批次数），ImGui 面板显示

### 目标
在不改变 draw call 数量的前提下，通过排序减少 GPU 状态切换（管线切换、纹理绑定切换），**是后续所有合批优化的基础**。

### 关键设计

#### 1.1 排序键（Sorting Key）
为每个待绘制的 `MeshInstance` 计算一个 64-bit sorting key，高优先级字段放在高位：
```
[ 8bit pipeline_id ][ 24bit texture_hash ][ 32bit depth/z ]
```
- 高位按管线分组 → 最少的管线切换
- 中位按纹理分组 → 减少纹理绑定切换（descriptor set 缓存命中）
- 低位按深度排序 → 对透明物体从后往前，对不透明物体从前往后（early-z 优化）

#### 1.2 EndScene 中增加排序步骤
**文件**：`GE/src/Render/Renderer3D.cpp`

在 `EndScene()` 中、分配 UBO 之前，对 `m_Meshes` 按 sorting key 排序。由于当前只有一种 shader + 一种管线状态，pipeline_id 恒为 0，主要收益来自纹理分组。

#### 1.3 管线标识
从 `VulkanResourceCache::RequestGraphicsPipeline` 的返回值获取管线标识（或基于 pipeline state hash），填入 sorting key 高位。

---

## 阶段 2：Dynamic UBO + 同材质合批

> **状态：✅ 已完成**（2026-08-08）
> - 构造时 `m_VertShader->set_resource_mode("ObjectUBO", Dynamic)`，先用现有反射/DescriptorSetLayout 基础设施把 set 2 binding 0 建为 `eUniformBufferDynamic`（无需改底层）
> - `EndScene` 按有效纹理分组，每组分配一块连续 ObjectUBO 内存，偏移对齐 `minUniformBufferOffsetAlignment`，一次性上传
> - 绘制时绑定组缓冲 + 动态偏移；同组 descriptor set 复用，draw 间仅更新动态偏移
> - 验证：RenderDoc 抓帧应看到同材质 draw 之间 descriptor set handle 相同、仅 dynamic offset 变化

### 目标
将 per-object 数据（模型矩阵、颜色等）从独立 UBO 改为 Dynamic Uniform Buffer，使得**同材质的多个 mesh 共享同一个 descriptor set**，draw call 之间仅更新 dynamic offset——大幅减少 descriptor set 分配/绑定开销。

### 关键设计

#### 2.1 ObjectUBO 改为 Dynamic
**文件**：`GE/include/GE/Render/Renderer3D.h` / `.cpp`

1. 在 pipeline layout 中，将 Set 2 的 ObjectUBO 标记为 dynamic：
   ```cpp
   m_PipelineLayout->set_resource_mode("ObjectUBO", ShaderResourceMode::Dynamic);
   ```
   （需要确认 `VulkanPipelineLayout` 是否暴露此接口；如果没有，需要在 `ShaderResource` / 反射阶段增加 mode 设置能力。）

2. EndScene 中，为**同材质组**的所有 mesh 分配一块连续的 ObjectUBO 内存（每个 mesh 的偏移对齐到 `minUniformBufferOffsetAlignment`），一次性上传。

3. 每个 draw call 前通过 `vkCmdBindDescriptorSets` 的 `pDynamicOffsets` 参数传入当前 mesh 的 offset。descriptor set 本身在组内只绑定一次。

#### 2.2 同材质组的判定
以材质（纹理组合 + 管线状态）为单位分组。阶段 1 的排序已经保证同材质 mesh 连续排列，阶段 2 只需在遍历时识别组边界。

#### 2.3 预期收益
- descriptor set 数量从 `N_meshes` 降到 `N_materials`
- VulkanResourceCache / RenderFrame 的 descriptor set 缓存命中率大幅提升
- 为阶段 3 的 instancing 打好基础

---

## 阶段 3：GPU Instancing（同 Mesh 同材质）

### 目标
对"相同 mesh + 相同材质"的多个实例，使用 `vkCmdDrawIndexedInstanced` 合并为单个 draw call。这是减少 draw call 数量最直接的手段。

### 关键设计

#### 3.1 实例数据收集
在 `EndScene()` 中，按 `(mesh, material)` 为 key 分组，收集每组的 transform / color 列表。

#### 3.2 Instance Buffer
将 per-instance 数据（模型矩阵行、颜色等）存入一块 instance vertex buffer 或 storage buffer。

**方案 A（顶点属性实例化）**：
- 顶点输入增加 per-instance 属性（`VK_VERTEX_INPUT_RATE_INSTANCE`）
- 模型矩阵拆成 4 个 `vec4` 属性（mat4 需要 4 个 location）
- 优点：兼容性好，不需要 SSBO
- 缺点：顶点属性 slot 占用多

**方案 B（Storage Buffer + gl_InstanceIndex）**：
- 所有实例数据存入 SSBO
- shader 中用 `gl_InstanceIndex` 索引
- 优点：灵活、数据量大
- 缺点：需要 SSBO 支持（现代 GPU 都没问题）

**推荐**：方案 B，更符合现代渲染管线，也为后续 GPU Driven 做铺垫。

#### 3.3 着色器修改
`mesh.vert` 中增加 instance buffer 读取路径，用 `gl_InstanceIndex` 获取 per-instance 模型矩阵。

**注意**：当前着色器只有 `.spv` 没有源码，需要先找回/重建 GLSL 源文件。这是本阶段的前置工作。

#### 3.4 MeshInstance 结构调整
在 `Renderer3D` 内部维护一个 `RenderBatch` 列表：
```cpp
struct RenderBatch {
    Mesh* mesh;
    Material* material;
    uint32_t instanceCount;
    uint32_t instanceDataOffset;  // 在 instance buffer 中的偏移
};
```

---

## 阶段 4：着色器变体机制 + 材质特性扩展

### 目标
建立完整的着色器变体（Shader Variant）机制，支持通过宏定义开关材质特性（normal map、emissive、alpha test、PBR 等），使材质系统具备可扩展性。

### 关键设计

#### 4.1 ShaderVariant 增强
**文件**：`GE/include/GE/Render/VulkanBase/VulkanShaderModule.h` / `.cpp`

1. 在 `ShaderVariant` 中增加宏定义列表：
   ```cpp
   struct MacroDef {
       std::string name;
       std::string value;  // 可为空
   };
   std::vector<MacroDef> macros;
   ```

2. 实现基于 macros 的 `id` 计算（hash all name+value pairs，保证确定性）。

3. 与现有的 `runtime_array_sizes` 合并到统一的 variant id 中。

#### 4.2 GLSL 在线编译
引入 `shaderc` 或 `glslang` 作为第三方依赖，当 `ShaderSource` 是 GLSL 文本（而非 `.spv` 路径）时，在 `ShaderModule` 构造中：
1. 注入 variant 的宏定义
2. 调用 glslang/shaderc 编译为 SPIR-V
3. 后续流程与预编译路径一致

预编译 `.spv` 路径保留为 fast path（无变体、release 模式下预编译所有变体）。

#### 4.3 材质特性声明
`Material` 类增加 `GetShaderFeatures()` 方法，返回一组特性开关（bitmask 或 string set），如：
```
HAS_NORMAL_MAP, HAS_EMISSIVE_MAP, ALPHA_TEST, ...
```

#### 4.4 管线变体生成
材质根据自身特性组合出对应的 `ShaderVariant`，通过 `VulkanResourceCache` 请求对应的 shader module + pipeline layout + pipeline，天然去重。

#### 4.5 材质特性扩展（可叠加的功能）
- **Normal Mapping**：切线空间法线贴图 + 切线/副切线顶点属性
- **Emissive**：自发光贴图 + 自发光强度
- **Alpha Test / Alpha Clip**：discard 透明度低于阈值的片元
- **Vertex Color**：支持顶点色叠加
- **UV 缩放偏移**：tiling + offset

---

## 阶段 5（可选）：高级渲染架构

在前面阶段完成后，根据项目需求可继续推进：

### 5.1 视锥体剔除
- CPU 端：`BeginScene` 后对所有 mesh 做 AABB 视锥剔除
- 后期可演进到 GPU 端剔除（compute shader + indirect draw）

### 5.2 前向+ 渲染（Forward+ / Tiled Forward）
- 将点光源分块（tile-based lighting），突破 8 个点光源限制
- 适合点光源数量多但仍用前向渲染的场景

### 5.3 延迟渲染
- G-Buffer（albedo + normal + metallic/roughness + emissive）
- 几何 Pass + 光照 Pass
- 适合光源数量极多的场景，但对透明物体不友好

### 5.4 Shadow Mapping
- 方向光：级联阴影贴图（CSM）
- 点光源：Omnidirectional shadow map（立方体贴图）
- 需要独立的 shadow pass + shadow shader 变体

---

## 关键文件索引

| 文件 | 改动内容 |
|------|----------|
| `GE/include/GE/Render/Material.h`（新） | Material 类定义 |
| `GE/src/Render/Material.cpp`（新） | Material 实现 |
| `GE/include/GE/Render/Renderer3D.h` | DrawMesh 新重载、内部结构调整 |
| `GE/src/Render/Renderer3D.cpp` | 排序、合批、Dynamic UBO、instancing |
| `GE/include/GE/Scene/Components.h` | MeshComponent 增加 MaterialPtr |
| `GE/src/Scene/Scene.cpp` | OnUpdate3D 适配 MaterialPtr |
| `GE/include/GE/Render/VulkanBase/VulkanShaderModule.h` | ShaderVariant 增加宏定义 |
| `GE/src/Render/VulkanBase/VulkanShaderModule.cpp` | GLSL 在线编译支持 |
| `assets/shaders/glsl/mesh.vert` / `.frag`（新增源文件） | 着色器源码（需重建或找回） |

---

## 验证方式

每个阶段可独立验证：

1. **阶段 0**：Sandbox 中创建一个带 Material 的实体，正常渲染即通过
2. **阶段 1**：Tracy 中观察 `Renderer3D::EndScene` 耗时无明显增加（排序开销可忽略）；可以加 debug 统计信息输出分组数量
3. **阶段 2**：RenderDoc 抓帧，确认同材质的多个 draw call 之间 descriptor set handle 相同、只有 dynamic offset 变化
4. **阶段 3**：RenderDoc 抓帧，确认相同 mesh + 相同材质的 N 个实例对应 1 个 `vkCmdDrawIndexedInstanced(instanceCount=N)`
5. **阶段 4**：创建两个不同特性的材质（如一个有 normal map、一个没有），确认渲染效果正确且管线数量 = 2（而非每个材质一条）

---

## 风险与注意事项

1. **GLSL 源码缺失**：当前只有 `.spv`，阶段 3/4 需要先找回或重写着色器源码。这是最大的前置依赖。
2. **Dynamic UBO 对齐**：`minUniformBufferOffsetAlignment` 通常是 256 字节，每个 ObjectUBO（64+4+12+16 = 96 字节）会 pad 到 256，内存开销约 2.7x。若 mesh 数量极多（>10k），可考虑改用 SSBO。
3. **排序开销**：每帧对 mesh 列表排序的时间复杂度 O(n log n)，n 在万级以下基本无感。
4. **资源所有权**：当前所有资源用裸指针，需注意 Material 的生命周期管理（由 MaterialManager 持有，保证渲染期间有效）。
5. **向后兼容**：每个阶段都应保持 `DrawMesh(transform, mesh, texture, color)` 旧接口可用，逐步迁移而非一次性切换。
