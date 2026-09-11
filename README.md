# GE — 自研游戏引擎与编辑器

基于 **Vulkan** 的 C++20 游戏引擎，附带一个功能完整的场景编辑器（`GE_Editor`）。

引擎自底层封装 Vulkan（Instance / Device / Swapchain / Pipeline / DescriptorSet / VMA 内存管理…），上层提供 ECS 场景、前向与延迟两套渲染路径、PBR + IBL、级联阴影、HDR 后处理链、水面与水下效果、骨骼蒙皮与动画状态机、Jolt 物理、Lua 脚本与音频系统。

> 平台：Windows x64（MSVC）。目前仅支持 Windows，底层已按平台层（`GE/src/Platform/`）隔离。

---

## 截图

**运行时画面**（延迟渲染 + PBR / IBL + CSM 阴影 + 水体与水下后处理 + Bloom）

![运行时画面 1](docs/images/runtime-1.png)

![运行时画面 2](docs/images/runtime-2.png)

![运行时画面 3](docs/images/runtime-3.png)

**编辑器**

![编辑器 1](docs/images/editor-1.png)

![编辑器 2](docs/images/editor-2.png)

![动画状态机（ASM）节点图](docs/images/asm-graph.png)

*动画状态机（ASM）节点图*

![组件面板](docs/images/components.png)

*组件面板*

---

## 功能特性

### 渲染
- **Vulkan 1.3**：`vulkan.hpp` 绑定 + VMA 显存管理 + **动态渲染**（KHR_dynamic_rendering，无 RenderPass 对象）
- **渲染图（RenderGraph）**：pass 声明式编排，虚拟资源池 + 自动屏障/布局转换/生命周期管理
- **两套渲染路径**
  - 前向：Blinn-Phong（方向光 + 任意数量点光源 + 环境光）
  - 延迟：GBuffer（MRT 四张）→ Lighting（全屏，在线性 HDR 空间着色）→ Transparent → Tonemap
- **PBR**：Cook-Torrance 金属-粗糙度工作流，铝箔/自发光/法线/金属粗糙度贴图槽
- **IBL**：split-sum 预滤波环境光（prefilter + skybox + BRDF LUT），天空盒可与 IBL 联动
- **级联阴影（CSM）**：默认 3 级、practical split 切分、逐级 AABB 剔除、PCF 软阴影，各级分辨率/偏差独立可配
- **HDR 后处理链**：`RGBA16F` 中间缓冲 → Bloom（阈值提取 + 多级降/升采样 + 合成）→ ACES Tonemap（曝光可调）
- **水面渲染**：Gerstner 波位移、法线贴图细节、深度感知折射、岸线泡沫、水下焦散与水下后处理（雾/去饱和/暗角）
- **合批与剔除**：相同 mesh + 材质的实例合并为一次 `vkCmdDrawIndexedInstanced`（实例数据走 SSBO）；视锥剔除、阴影包围盒剔除、实体级 AABB 子树裁剪
- **透明渲染**：按 `pass → pipeline → material → mesh → depth` 排序键分区，不透明近→远吃 early-z，透明远→近正确混合
- **异步上传**：纹理/网格数据经 `AsyncUploadManager` 后台线程上传

### 场景与资源
- **ECS**：EnTT，`Scene` + 轻量 `Entity` 包装 + 20 余种组件
- **层级**：父子实体（`TransformComponent::parent`），世界矩阵每帧 DFS 更新；UUID 稳定引用，序列化重排不错乱
- **序列化**：`.scene`（YAML），编辑器保存/加载
- **模型加载**：`glTF/GLB`（tinygltf，含多 primitive / 蒙皮 / 动画）、`OBJ`（tinyobjloader）、引擎内置 `.gemesh` 二进制格式
- **纹理**：stb_image + KTX2（libktx），按路径去重缓存
- **骨骼动画**：蒙皮管线（顶点骨骼加权变形）、动画剪辑加载与混合、**动画状态机 ASM**（配套节点图编辑器）

### 其他子系统
- **物理**：Jolt Physics，固定 60Hz 步长；刚体 / 盒 / 球 / 胶囊碰撞体 / 角色控制器（CharacterVirtual）
- **脚本**：Lua 5.4 + sol2，脚本挂载在实体上，含 `PUBLIC_FIELDS` 面板可编辑公开字段
- **音频**：miniaudio（AudioContext / AudioWorld / 3D 音源）
- **编辑器 UI**：Dear ImGui（Docking + multi-viewport）+ ImGuizmo 变换 gizmo + imgui-node-editor
- **性能分析**：Tracy（可选，`build.bat` 默认开启）+ 引擎内 `GE_PROFILE_*` 宏

---

## 目录结构

```
GameEngine/
├── GE/                       # 引擎核心（静态库）
│   ├── include/GE/           # 公开头文件
│   ├── src/                  # 实现
│   │   ├── Core/             # Application / Window / LayerStack / Log / Input
│   │   ├── Events/           # 事件系统
│   │   ├── Render/           # Renderer2D / Renderer3D / 资源管理
│   │   │   ├── VulkanBase/   # Vulkan 底层封装
│   │   │   ├── RenderGraph/  # 渲染图
│   │   │   └── ImGui/        # ImGui 后端
│   │   ├── Scene/            # Scene / Entity / 序列化 / glTF 导入 / ScriptEngine
│   │   ├── Physics/          # Jolt 封装
│   │   ├── Animation/        # 动画剪辑、蒙皮、状态机
│   │   ├── Audio/            # miniaudio 封装
│   │   ├── FileSystem/       # 文件系统抽象
│   │   └── Platform/Windows/ # 平台层实现
│   └── third_party/          # 全部第三方依赖（多数已 vendored）
├── GE_Editor/                # 编辑器可执行
│   └── src/
│       ├── EditorApp.cpp     # 入口，组装各 Layer
│       ├── SceneLayer.cpp    # 场景渲染 + 渲染图 pass 声明
│       ├── Panels/           # 层级 / 资源 / 渲染统计 / ASM 图面板
│       └── NodeEditorUtils/  # 节点图绘制工具
├── assets/                   # 共享资源（着色器 / 场景 / 脚本 / 字体 / 纹理）
│   └── shaders/glsl/         # GLSL 着色器，构建时由 glslc 编译为 .spv
├── tools/                    # 辅助工具（见下文）
├── docs/                     # 设计文档与实现计划书
├── CMakeLists.txt            # 主构建脚本
└── build.bat                 # MSVC + Ninja 一键构建
```

**运行时以仓库根目录为工作目录** —— 引擎按 `assets/...` 相对路径加载字体、着色器与场景。

---

## 环境要求

| 依赖 | 说明 |
|------|------|
| Windows 10/11 x64 | 唯一支持的平台 |
| Visual Studio 2022 | 需 MSVC 工具集 + Windows SDK（`build.bat` 通过 `vcvarsall.bat` 初始化） |
| Vulkan SDK | 1.3+，需在 `PATH`/`VULKAN_SDK` 中能找到 **`glslc`**（着色器编译） |
| CMake | 3.15+ |
| Ninja | 随 VS / CMake / CLion 分发 |
| Python 3 | 可选，仅生成环境 IBL 资产与合并贴图时需要 |

> 除 Vulkan SDK 外，所有第三方库均已随仓库携带（`GE/third_party/`），无需额外安装。

> ⚠️ **资源未入库**：`.gitignore` 排除了大体积资源目录（`assets/models/`、`assets/materal/`、`assets/environments/`、`assets/audio/`、`assets/HDRI/`、`*.ktx`/`*.ktx2`）。克隆后需自行准备这些资源，场景中的模型/环境引用才能正常加载；纯引擎与编辑器本身可正常构建运行。

---

## 构建

### 一键脚本（推荐）

```bat
build.bat                  :: Debug 构建（默认）
build.bat release          :: Release 构建
build.bat release rebuild  :: 清理后 Release 构建
build.bat -h               :: 查看帮助
```

脚本会自动初始化 MSVC 环境、调用 CMake 配置（Ninja 生成器）并编译，产物输出到 `bin/`。

> 首次使用请修改 `build.bat` 顶部的 `MSVC_TOOLS` 变量，指向本机 MSVC 安装路径（默认 `F:\c++budiltool`，即 `vcvarsall.bat` 与 Ninja 所在位置）。

### 手动 CMake

```bat
cmake -B bin-int-msvc -G Ninja ^
      -DCMAKE_BUILD_TYPE=Debug ^
      -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe
cmake --build bin-int-msvc
```

需在已初始化 MSVC 环境的终端中执行。

### 构建目标

| 目标 | 产物 | 说明 |
|------|------|------|
| `GE` | `GE.lib` | 引擎核心静态库 |
| `GE_Editor` | `GE_Editor.exe` | 编辑器 |
| `gemesh` | `gemesh.exe` | `.gemesh` 离线导出 CLI |
| `GEShaderCompile` / `ShaderCompile` | `*.spv` | GLSL → SPIR-V（构建时自动触发） |

---

## 运行

在**仓库根目录**下运行，否则相对路径资源加载会失败：

```bat
bin\GE_Editor.exe
```

编辑器启动后：顶部「文件」菜单新建 / 打开 / 保存场景（`assets/scenes/*.scene`），右侧层级面板增删实体与组件，资源面板浏览纹理/材质/网格，ASM 图面板编辑动画状态机。

编辑器个人偏好（相机视角、渲染开关等）写入 `editor_settings.cfg` / `editor_camera.cfg`，不入版本库。

---

## 渲染管线

延迟路径的 pass 链（前向路径退化为单个 `Scene3D` pass）：

```
ShadowMap_C0..Cn        逐级级联阴影深度图（零颜色附件 + 深度）
        ↓
GBuffer                 MRT：Albedo / Normal / WorldPos / Emissive（仅不透明段）
        ↓
Lighting                采样 GBuffer，输出线性 HDR 到 Scene_HDR；天空盒并入此 pass
        ↓
SceneColorCopy          Scene_HDR → Scene_HDR_Base（供水面折射读取，避免自依赖）
SceneDepthCopy          不透明深度 → 可采样的 SceneDepth
        ↓
Transparent             透明物体前向 alpha 混合，写回 Scene_HDR（线性 HDR）
        ↓
UnderwaterFX            可选：相机淹没时水下雾 / 去饱和 / 暗角 / 焦散
        ↓
Bloom                   Extract → Downsample×N → Upsample×N → Composite → Scene_AfterBloom
        ↓
Tonemap                 曝光 + ACES，HDR → 视口颜色
        ↓
Scene2D                 Sprite 叠加
```

**描述符绑定约定**（前向）：

```
Set 0  Binding 0   FrameUBO（投影 / 视图 / 相机位置 / 光照）
       Binding 1   点光源 SSBO（数量无编译期上限）
Set 1  Binding 0/1/3  主纹理 / 法线贴图 / 自发光贴图
       Binding 2   MaterialUBO（shininess / metallic / roughness / 自发光因子…）
       Binding 5/6/7   IBL：prefilter / BRDF LUT / irradiance（IBL 变体）
Set 2  Binding 0   InstanceData SSBO（model + tint，按实例）
       Binding 1   JointBuffer SSBO（蒙皮关节矩阵）
```

---

## ECS 组件

`Tag` · `ID` · `Transform` · `SpriteRenderer` · `MeshRenderer` · `Camera` · `Script` · `PointLight` · `DirectionalLight` · `AmbientLight` · `Environment` · `RigidBody` · `BoxCollider` · `SphereCollider` · `CapsuleCollider` · `CharacterController` · `FollowCamera` · `BoundingBox` · `Water` · 动画与音频组件

定义见 [`GE/include/GE/Scene/Components.h`](GE/include/GE/Scene/Components.h)。

---

## Vulkan 封装层次

```
┌──────────────────────────────────────────────────┐
│  高层渲染接口  Renderer2D / Renderer3D / Material  │
├──────────────────────────────────────────────────┤
│  渲染图  RenderGraph（pass 编排 / 屏障 / 资源池）   │
├──────────────────────────────────────────────────┤
│  帧资源  VulkanRenderContext / VulkanRenderFrame   │
├──────────────────────────────────────────────────┤
│  全局资源缓存  VulkanResourceCache（去重）          │
├──────────────────────────────────────────────────┤
│  单对象封装  Buffer / Image / Pipeline / Sampler…  │
├──────────────────────────────────────────────────┤
│  核心上下文  VulkanContext / VulkanDevice          │
├──────────────────────────────────────────────────┤
│  vulkan.hpp + VMA                                 │
└──────────────────────────────────────────────────┘
```

`VulkanComputePipeline` 与 `VulkanCommandBuffer::Dispatch` 已封装，引擎内暂无消费方。

---

## 工具

| 工具 | 用途 |
|------|------|
| `tools/gemesh/gemesh_main.cpp` | `.obj` → `.gemesh` 离线烘焙 CLI，与运行时加载走同一套解析/切线/序列化逻辑 |
| `tools/gen_env.py` | 由源 HDRI 全景图生成一套 IBL 资产（`skybox.ktx2` / `prefilter.ktx` / `brdf_lut.png` / `preview.png`）到 `assets/environments/<环境名>/`。依赖 Filament `cmgen` 与 KTX `ktx` CLI，脚本顶部可配路径 |
| `tools/merge_metal_rough.py` | 合并独立的金属度 / 粗糙度单通道图，输出 glTF 约定的 `R/G/B/A` 打包图 |

环境资产目录约定：`assets/environments/<环境名>/`，其中 `brdf_lut.png` 全局共享一份。

---

## 文档

`docs/` 下按子系统存放设计文档与实现计划书，覆盖架构总览、渲染（PBR / IBL / 延迟 / HDR / Bloom / CSM / 阴影剔除 / 透明 / 水体 / 水下后处理 / 渲染图）、动画（骨骼蒙皮 / 状态机 / 节点图编辑器）、物理（角色控制器 / 编辑运行时分离）、音频、Lua 脚本、Asset 格式（`.gemesh`）等。已完成的方案归入 `docs/ok/`。

---

## 第三方依赖

| 库 | 用途 |
|----|------|
| Vulkan SDK | 图形 API（外部依赖） |
| vulkan.hpp / VMA | C++ 绑定 / 显存分配 |
| GLFW | 窗口与输入 |
| Dear ImGui · ImGuizmo · imgui-node-editor | 编辑器 UI |
| glm | 数学库 |
| EnTT | ECS |
| spdlog | 日志 |
| yaml-cpp | 场景序列化 |
| Jolt Physics | 物理 |
| tinyobjloader · tinygltf | OBJ / glTF 加载 |
| stb_image | 图像解码 |
| KTX-Software (libktx) | KTX2 纹理 |
| SPIRV-Cross | SPIR-V 反射 / 反编译 |
| Lua 5.4 · sol2 | 脚本 |
| miniaudio | 音频 |
| Tracy | 性能分析（可选） |

---

## 许可

[Apache License 2.0](LICENSE)
