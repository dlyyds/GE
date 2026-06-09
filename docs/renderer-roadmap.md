# Vulkan 渲染器开发路线图

> 本文档规划 GameEngine Vulkan 渲染器的未来开发计划。
> 项目当前状态：已实现 Vulkan 1.3 Dynamic Rendering 基础管线，Sandbox 可渲染带纹理四边形。

---

## 总体阶段划分

```
阶段一：巩固基础 ───→ 阶段二：功能完善 ───→ 阶段三：高级特性 ───→ 阶段四：工具与优化
   (当前 ~ 2月)          (2~4月)               (4~6月)               (持续)
```

---

## 阶段一：巩固基础（当前 ~ 2个月）

### 目标
补齐渲染器骨架，使其能处理实际场景而不仅仅是测试四边形。

### 1.1 渲染架构重构
- [ ] 统一 `Renderer` 类设计，区分 Forward Renderer / Deferred Renderer 接口
- [ ] 引入 `RenderCommand` / `CommandBuffer` 录制抽象，分离逻辑提交与 GPU 执行
- [ ] 梳理 `Renderer2D` 职责：当前是渲染器单例，未来应拆分为 2D 和 3D 两个子系统

### 1.2 网格与模型加载
- [ ] 集成 `assimp` 或 `tinyobjloader` 加载复杂模型
- [ ] 设计 `Model` 类（持有多 Mesh、材质索引、节点层级）
- [ ] 实现 glTF 2.0 加载管线（目前最通用的 3D 格式）
- [ ] 支持顶点缓存（Vertex Cache）和索引缓存优化

### 1.3 材质系统升级
- [ ] 当前：材质 = Pipeline + DescriptorSet，过于刚性
- [ ] 设计 `Material` 资源类：参数化材质（颜色、粗糙度、金属度、法线贴图等）
- [ ] 实现材质序列化（YAML / JSON）
- [ ] 支持运行时材质编辑（通过 ImGui Property Panel）

### 1.4 纹理系统增强
- [ ] 当前：仅支持从文件加载 `VulkanImage`
- [ ] 实现纹理管理器（`TextureCache`）—— 去重、引用计数、异步加载
- [ ] 支持立方体贴图（Cubemap）、HDR 纹理
- [ ] 支持纹理压缩格式（BCn, ASTC）

### 1.5 着色器管理系统
- [ ] 当前：从 `.spv` 文件直接加载，硬编码路径
- [ ] 设计 `ShaderLibrary`：按名称管理着色器，支持 #include 预处理
- [ ] 构建时着色器编译（glslc）集成到 CMake（当前已初步有）
- [ ] 支持着色器热重载（开发时改完立刻生效）
- [ ] 增加着色器反射（利用 SPIRV-Reflect 或 Vulkan-Hpp 反射功能解析 binding/location）

### 1.6 编辑器解封
- [ ] 修复 `GE_Editor` CMake 构建（当前被注释掉）
- [ ] 将编辑器层（`EditorLayer`）从旧的 OpenGL 风格 API 迁移到新 Vulkan API
- [ ] 实现场景视口（Scene Viewport）—— 渲染到 ImGui 纹理

---

## 阶段二：功能完善（2 ~ 4个月）

### 目标
实现现代渲染器标配功能，包括光照、阴影、后处理、多 Pass 渲染。

### 2.1 光照系统
- [ ] 设计 `Light` 组件（方向光、点光源、聚光灯）
- [ ] 实现 Forward+ / Tiled Shading 光照计算
- [ ] 在 GPU 侧使用光照缓冲区（Structured Buffer 存储所有光源）
- [ ] 支持 Editor 中放置和调整光源

### 2.2 阴影映射
- [ ] 实现方向光阴影（级联阴影映射 CSM）
- [ ] 点光源阴影（Cube Map Shadow）
- [ ] PCF 软阴影滤波
- [ ] 阴影相机剔除优化

### 2.3 延迟渲染管线 (Deferred Shading)
- [ ] G-Buffer 设计：位置、法线、反照率、粗糙度/金属度、深度
- [ ] 延迟光照 Pass
- [ ] 延迟管线与现有 Forward 管线共存，按 Mesh/Material 自动切换
- [ ] SSAO（屏幕空间环境光遮蔽）

### 2.4 后处理管线
- [ ] 设计 `PostProcessStack`：可串联多个后处理效果
- [ ] 基础效果：色调映射（HDR → LDR）、Gamma 校正
- [ ] Bloom 效果（高斯模糊 + 混合）
- [ ] FXAA / TAA 抗锯齿
- [ ] 色彩分级（Color Grading LUT）

### 2.5 Skybox / 环境光照
- [ ] 立方体贴图加载与渲染（Skybox Pass）
- [ ] IBL（Image Based Lighting）：漫反射辐照度图 (Irradiance Map)
- [ ] 镜面反射预滤波图 (Pre-filtered Environment Map)
- [ ] BRDF LUT 生成

### 2.6 粒子系统
- [ ] GPU 粒子（Compute Shader 驱动）
- [ ] CPU 回退方案（小数量粒子）
- [ ] 粒子材质与纹理采样

---

## 阶段三：高级特性（4 ~ 6个月）

### 目标
引入高端渲染技术和性能优化，使引擎达到生产可用水平。

### 3.1 GPU Driven Rendering
- [ ] Indirect Draw（`vkCmdDrawIndexedIndirect`）
- [ ] 可见性缓冲区（Visibility Buffer）或 GPU Frustum Culling
- [ ] 基于 Mesh Shader（VK_NV_mesh_shader / VK_EXT_mesh_shader）的管线
- [ ] LOD 系统（层级细节切换）

### 3.2 Compute Shader 管线
- [ ] Compute 队列管理（与 Graphics 队列分离）
- [ ] 通用计算管线封装
- [ ] 应用：后处理、粒子更新、蒙皮动画、FFT 等

### 3.3 光线追踪 (Ray Tracing)
- [ ] 启用 `VK_KHR_ray_tracing_pipeline` 扩展
- [ ] BLAS / TLAS 构建与管理
- [ ] 简单光追反射混合管线
- [ ] 光追阴影

### 3.4 资源流式加载
- [ ] 纹理 Streaming（Mipmap 渐进加载）
- [ ] 网格 Streaming
- [ ] 多线程资源加载与 GPU Upload 队列
- [ ] 基于虚拟纹理 (Virtual Texturing) 的大世界纹理

### 3.5 多 GPU / 多线程渲染
- [ ] 渲染线程分离（主线程 → 渲染线程 → GPU）
- [ ] 多个 Command Buffer 并行录制
- [ ] 多 GPU 支持（SLI / 显存聚合，非必须，可延后）

---

## 阶段四：工具与优化（持续）

### 目标
提升开发体验、调试能力和运行时性能。

### 4.1 调试工具
- [ ] Vulkan Validation Layers 错误自动捕获与中文提示
- [ ] GPU 性能计数器查询（VK_KHR_performance_query）
- [ ] 内置 GPU 剖析 UI（每帧 Draw Call 数、显存占用、管线切换次数）
- [ ] RenderDoc / Nsight 标注（`vkCmdBeginDebugUtilsLabelEXT`）

### 4.2 性能优化
- [ ] Pipeline Cache 序列化/反序列化（加速启动）
- [ ] Descriptor Set 池化与重用
- [ ] Staging Buffer 池（上传队列复用）
- [ ] Barrier 合并与优化（减少 Image Layout Transition 开销）
- [ ] 使用 `VK_EXT_graphics_pipeline_library` 减少管线创建开销

### 4.3 渲染功能集成到 Editor
- [ ] 材质编辑器（节点图或参数面板）
- [ ] 实时着色器编辑与预览
- [ ] 渲染设置面板（分辨率、VSync、后处理开关）
- [ ] 场景 Profiling 面板

---

## 里程碑速览

| 里程碑 | 预计时间 | 核心交付 |
|--------|----------|----------|
| **M1: 渲染骨架** | 2 个月 | 模型加载、材质系统、Editor 恢复、基础光照 |
| **M2: 标准渲染器** | 4 个月 | 延迟渲染、阴影、后处理、IBL、粒子 |
| **M3: 高级渲染** | 6 个月 | GPU Driven、Compute、光追、流式加载 |
| **M4: 工具成熟** | 持续 | 调试工具、性能优化、Editor 完善 |

---

## 当前优先级建议

### 最优先（下一个 sprint）
1. **模型加载**（assimp / tinyobjloader）—— 没有模型就无法展示渲染效果
2. **Editor 解封** —— 场景视口和层级面板是开发核心工具
3. **着色器热重载** —— 大幅提升着色器开发效率
4. **材质系统重构** —— 从硬编码到参数化材质

### 次优先（下下个 sprint）
5. 方向光阴影映射
6. Skybox / IBL 环境光照
7. 后处理栈（至少 Bloom + Tone Mapping）
8. 延迟渲染 G-Buffer 管线

---

*本文档会随项目进展持续更新。每完成一个阶段，重新评估优先级并调整计划。*
