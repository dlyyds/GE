# glTF 导入与骨骼动画实施方案

## Context

当前引擎的模型约定是「一个文件 = 一个 `Mesh` = 一个实体」：

- `Mesh` 是一张几何（共享顶点/索引缓冲 + `SubMesh` 索引范围），不携带任何摆放信息
- `TransformComponent` 是平铺的（`Components.h`），无 parent/child
- `MeshManager::Load(path)` 只返回一个 `Mesh*`，`MeshRendererComponent` 只指向一个 `Mesh*`

OBJ 天生是单几何，契合这个模型；glTF 则是「场景容器」——多张 mesh + node 层次 + 每 node 的局部变换 + 可选骨骼/动画。要支持 glTF 的**骨架动画 / 交互部件**（轮子转、部件动、角色走路），需要从地基开始逐层搭建，任何一层缺失都全崩。

依赖链：

```
骨骼动画
   └─ 依赖 → 蒙皮渲染（第 3 层）
           └─ 依赖 → glTF 场景图导入（第 2 层）
                   └─ 依赖 → 实体父子系统（第 1 层）
```

---

## 阶段总览

| 阶段 | 主题 | 核心收益 | 里程碑可交付 | 改动规模 |
|------|------|----------|--------------|----------|
| **阶段 1** | 实体父子系统 | Transform 层级化，实体可整体/局部联动 | ✅ 独立可交付 | 中 |
| **阶段 2** | glTF 场景图导入 | 一个 .gltf/.glb → 一棵实体树，**交互部件可独立运动** | ✅ 独立可交付 | 中 |
| **阶段 3** | 蒙皮（骨骼动画加载 + GPU 蒙皮着色） | 角色网格随骨骼形变 | 需含动画才能演示 | 大 |
| **阶段 4** | 动画播放 | 关键帧驱动骨骼 → 角色走路/循环动作 | ✅ 骨骼动画全链路完成 | 中 |

> **建议**：阶段 1 → 2 完成后停下评估。只做交互部件（轮子转、部件动）到阶段 2 即可交付，不碰顶点布局，风险低；完整骨骼动画必须走完 1→4，阶段 3 的 `Vertex`/shader/渲染是硬骨头（顶点布局破坏性改动一次，现有资产需重跑切线/量化逻辑）。

---

## 阶段 1：实体父子系统

### 目标
`TransformComponent` 获得层级能力：`parent` / `children`，`GetTransform()` 返回 `父级世界变换 × 自身局部变换`。做交互部件与骨骼动画的地基，**不碰渲染一行**。

### 关键设计

> 设计决策（已定）：
> - **层级存储** = 组件内 `parent` 向上指针（`entt::entity`，null=根）+ Scene 侧反向索引缓存 `m_ChildrenOf`（parent→children）。唯一真相在组件指针，索引是可由 O(n) 遍历重建的派生缓存，永不失同步。
> - **序列化引用** = 每实体持久化 UUID（新增 `IDComponent`），父引用落盘为父实体的 UUID 字符串；反序列化走「先全建→记 ID→句柄 →再二次遍历接 parent」的两遍流程。重排/增删实体不影响引用语义（弃用"数组索引"脆方案），并为阶段 4 glTF node↔实体对应、编辑器 undo/脚本铺路。

#### 1.1 TransformComponent 层级改造
**文件**：`GE/include/GE/Scene/Components.h`

- `TransformComponent` 增加一个 `entt::entity parent = entt::null`，其余保持纯 POD（**不加** `std::vector<Entity> children`——vector 破坏内存布局与平凡拷贝，且组件间互链是 EnTT 反模式）
- 新增 `GetWorldTransform()`：沿 `parent` 链上溯累乘 `父世界 × 局部 TRS`。EnTT 句柄带版本位，实体销毁再复用不会产生悬垂引用
- glTF 的 node transform（TRS 或 matrix）与此结构对齐：rotation 保持四元数（当前已是四元数存储）

#### 1.2 IDComponent（新组件）
**文件**：`GE/include/GE/Scene/Components.h`

- 新增 `IDComponent`，持 `std::string UUID`（创建实体时生成，运行时也可按 ID 查实体）
- 序列化时随实体落盘；不参与渲染/物理遍历

#### 1.3 Scene 反向索引 + 层级 API
**文件**：`GE/src/Scene/Scene.cpp`

- Scene 持有 `std::unordered_map<entt::entity, std::vector<entt::entity>> m_ChildrenOf`（保插入序）
- 层级变更**只走单一入口**，同步更新组件指针 + 反向索引两处，杜绝双真相漂移：
  - `void Scene::SetParent(Entity child, Entity parent)` — 唯一改父子关系的 API
  - `Entity GetParent(Entity)` / `std::vector<Entity> GetChildren(Entity)`
  - `void RebuildChildrenIndex()` — 全量扫描重建反向索引（恢复现场/兜底）
- `DestroyEntity` 改造：沿反向索引递归销毁后代，并把自己从父实体的后代列表中移除

#### 1.4 遍历改递归 + 世界矩阵缓存
**文件**：`GE/src/Scene/Scene.cpp`，编辑器场景面板

- 实体遍历从「平铺所有实体」改为「递归走树」（根 → 子）
- 渲染前每帧按树深度先父后子一遍 DFS：`world = parentWorld × local`，缓存到 `TransformComponent` 或 Scene 侧世界矩阵表；渲染端只读世界矩阵
- `Scene::OnUpdate3D` 仍按实体收集，但世界矩阵用缓存值

#### 1.5 序列化（UUID + 两遍反序列化）
**文件**：`GE/src/Scene/SceneSerializer.cpp`

- **写**：`IDComponent` 落盘为 `Id: "..."`；`TransformComponent.parent` 落盘为**父实体的 UUID 字符串**；根实体写 `Parent: null` 或省略
- **读**（两遍）：
  1. 第一遍：创建全部实体，记 `uint32_t → 实体句柄` 映射（ID 到句柄）
  2. 第二遍：每实体若 `Parent` 非空，`SetParent(该实体, id映射[parent])` 接上
- **向后兼容**：旧场景无 `Id`/`Parent` 字段 → 生成随机 UUID、视为根节点；旧格式（节点无 Id）只在加载新格式时正常，旧文件仍可加载为平铺树

### 验收标准
- 父实体移动，子实体整体跟随；子实体局部旋转只转自己
- 保存/加载场景后层级关系不丢失
- 删除父实体级联删除子树

---

## 阶段 2：glTF 场景图导入

### 目标
扩展 `ParseModelData` 分派，新增 glTF/GLB 解析器：一个文件 → 一棵实体树（每个带 mesh 的 node 一个实体），transform 填**相对父级的局部坐标**。材质走既有 `MaterialData` / `MaterialManager` 通道。

### 关键设计

#### 2.1 解析器
**文件**：`GE/src/Render/Mesh.cpp`（新增 `ParseGLTFData`，在 `ParseModelData` 注册 `.gltf` / `.glb`）

- 库选型：**tinygltf**（C++ 单 header，与现有 tinyobjloader 同族，支持 GLB / 嵌入纹理 / 场景结构）
- primitive → `SubMesh`：各 primitive 顶点**追加**进共享数组 + 索引偏移，`material` 索引 → `MaterialData.name`
- node 树：遍历 node，带 mesh 的 node 生成实体，TRS/matrix → 局部 `TransformComponent`
- 网格复用：多个 node 引用同一 mesh 时复用同一 `Mesh*`（对应 glTF 的 wheels 复用场景，待阶段 1 层级支持后生效）

#### 2.2 材质扩展
**文件**：`GE/include/GE/Render/Mesh.h`（`MaterialData` 增字段）、`GE/src/Render/MeshManager.cpp`（`ApplyMaterialData` 增 glTF 分支）

- `MaterialData` 新增：`metallicRoughnessMap`（合并贴图 B=metallic, G=roughness）、`occlusionMap`、`normalScale`、`doubleSided`、`alphaMask`/`alphaCutoff`
- `Material::MetallicRoughness` 槽位（已存在）接合并 MR 贴图；`mesh_pbr.frag:188` 已采样该贴图，渲染端零改动
- glTF 特有坑：
  - **内嵌纹理**（GLB bufferView / data URI）不能以文件系统路径加载 → 需 `TextureManager` 增加「从内存字节加载」（复用已有 `Texture::LoadFromMemory`）或落盘临时文件
  - **baseColorFactor 是线性值**，塞进 sRGB 纯色贴图会被二次解码偏亮 → glTF 路径用 linear 贴图或 factor 先 linear→sRGB
  - **UV 约定**：glTF V 轴天然匹配 Vulkan，不要像 OBJ 那样再 `1 - v` 翻转
  - `doubleSided`/`alphaMode`（MASK 裁剪）已有 `Material` 字段承接（`Material.h` `doubleSided` / `alphaTest`）

#### 2.3 MeshManager 扩展
**文件**：`GE/src/Render/MeshManager.cpp`（或新增 glTF 资产导入器）

- `MeshManager::Load(path)` 的「一文件一 Mesh」约定扩展为「导入结果 = 多实体 + 根节点」；返回一个 `ImportedAsset`（根实体 / 实体列表），供场景层挂载
- 同步/异步两条加载路径共用 `ParseModelData`，保持现状

### 验收标准
- 导入汽车类 glTF：车身 + 轮子为独立实体，各有正确局部位置
- 轮子实体可绕自身轴旋转（阶段 1 父子 + 本阶段导入共同达成）
- 材质（PBR 金属-粗糙度 + 合并 MR 贴图 + 法线 + 自发光）正确显示
- GLB 内嵌纹理加载正常，baseColorFactor 颜色无偏亮

---

## 阶段 3：蒙皮（骨骼动画加载 + GPU 蒙皮）

### 目标
角色网格随骨骼形变。**这是全方案最硬的一步**。

### 关键设计

#### 3.1 顶点数据扩展（破坏性改动）
**文件**：`GE/include/GE/Render/Mesh.h`（`Vertex`）、顶点着色器、`ComputeTangents`/量化逻辑

- `Vertex` 增加：`int32 jointIndices[4]`（默认 -1）+ `float jointWeights[4]`（默认 0），GLB 的 JOINTS_0 / WEIGHTS_0 accessor 填入
- 顶点布局 / 顶点着色器同步改（破坏性），现有资产全部重跑一遍切线/量化逻辑
- 量化/去重 `operator==` 与 hash 需同步纳入新字段

#### 3.2 蒙皮渲染
**文件**：PBR 顶点着色器 + `Renderer3D`（UBO 扩展）

- 顶点着色器：`pos = Σ wᵢ · jointMatrixᵢ · pos`，法线同理（用 joint 矩阵的旋转部分）
- 每帧把 joint 矩阵表传 UBO；矩阵由骨架（node 树的一部分）算出，含反向绑定矩阵（inverse bind matrix）、骨骼根、蒙皮数据管理（glTF `skins`）

#### 3.3 资源接入
- GLB 的 `skins`/`joints` 节点、inverse bind matrices 从解析器剥出，接入 Scene 层管理
- 与阶段 1 的 node 层级直接对接：joint 即 node，骨骼变换就是 node 世界变换

### 验收标准
- 含骨骼的网格正确形变，未蒙皮顶点不受影响（权重为 0 的顶点等价旧行为）
- 骨骼根 / inverse bind 矩阵数据加载正确

---

## 阶段 4：动画播放

### 目标
关键帧驱动骨骼 → 角色走路 / 循环动作。glTF 提供 translation/rotation/scale 三轨道 × STEP/LINEAR/CUBICSPLINE 三插值。

### 关键设计

#### 4.1 动画数据与播放
**文件**：新增 `Animation` 数据结构 + `AnimationComponent`

- 轨道数据（target node + 通道 + 关键帧 + 采样器）+ 时间轴 + 每帧求值写回实体局部变换
- `AnimationComponent`：当前时间、播放/暂停/倍速、循环

#### 4.2 编辑器
- 资源面板动画列表 + 播放控制条（播放/暂停/时间轴）

### 验收标准
- 导入的带动画 glTF/GLB 在场景内按关键帧驱动骨骼播放
- 循环播放流畅，暂停/恢复/倍速可用

---

## 依赖与风险清单

| 项 | 说明 |
|----|------|
| 阶段 3 顶点布局破坏 | `Vertex` 增加关节字段，现有资产需重新加载；量化/去重/hash 同步更新 |
| `MeshManager.Load` 约定扩展 | 「一文件一 Mesh」改为「一文件多实体」，调用方（场景/编辑器）需适配 |
| 实体父子序列化 | 旧场景兼容 + 加载顺序二次遍历重建关系 |
| tinygltf 第三方依赖 | 新增 `GE/third_party/tinygltf/`，需接入构建脚本（build.bat） |
| 内嵌纹理 | GLB bufferView / data URI 无法走文件路径加载，需内存加载入口 |
| 颜色空间 | glTF baseColorFactor 线性 vs 现路径 sRGB 纯色贴图，需区分处理 |
| AO / 扩展材质（clearcoat 等） | 着色器尚未采样 occlusion，KHR 材质扩展暂缓 |