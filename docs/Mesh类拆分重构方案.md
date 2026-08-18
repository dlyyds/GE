# Mesh 类拆分重构方案

> 状态：**阶段 1–3 已实施（2026-08-18）；阶段 4 构建验证待用户执行**
> 前置：~~`docs/glTF阶段2场景图导入实现方案.md`（阶段 2 会给 Mesh 增加工厂方法，届时骨架尽量不动）~~
> 说明：按用户要求在本拆分中先完成阶段 1–3（与阶段 2 无后续冲突：本拆分新增的 `Create(MeshData&&)` / `CreateShell`
> 正是阶段 2 `CreateFromBuffers` 的既有形态，届时直接叠加即可）。
> 目标：把 `Mesh` 从「CPU 数据容器 + GPU 资源 + 异步加载生命周期 + 文件格式解析」四合一拆开，
>       让每个类职责单一、可读性提升、释放大模型常驻内存的 CPU 顶点副本。

---

## 1. 背景与现状问题

当前 `Mesh`（`GE/include/GE/Render/Mesh.h`）一个 class 焊接了四种互不相干的职责：

| 职责 | 载体 | 成员/方法 |
|---|---|---|
| CPU 数据容器 | 格式解析产物 | `m_Vertices` / `m_Indices` / `m_SubMeshes` / `m_MaterialData`（421-424 行） |
| GPU 资源 | 可渲染资产 | `m_VertexBuffer` / `m_IndexBuffer` / `UploadToGPU` / `SetDebugName` |
| 异步加载生命周期 | 空壳→就绪状态机 | `AsyncPendingSlot` / `m_Ready` / `InstallAsyncData` / `IsReady`（**保留在 Mesh**，被动状态） |
| 加载编排 / 几何生成 | 调起加载、提交任务、内置几何 | `LoadFromFile` / `LoadFromFileAsync` / `CreateBuiltin`（**迁出 Mesh** → MeshManager） |
| 文件格式解析 | 磁盘读取 | `ParseModelData` / `ParseOBJData`（迁出 → OBJLoader / ModelLoader） |

**主要问题**：

1. **职责漂移**：一个「网格」概念驱动了四个关心面，「类名读着是资源，类里装着解析器」。
2. **CPU 顶点常驻内存**：已上传 GPU 后，`m_Vertices`/`m_Indices`（大模型可达数百 MB）仍整份留在堆上。
3. **每次加格式都要动 Mesh.h**：glTF 阶段 2 / FBX / 未来格式都往类里塞加载逻辑，越塞越臃肿。

---

## 2. 消费者盘点（拆分边界的依据）

对公开访问器逐一审计（2026-08 现状，全库 grep）：

| 访问器 | 外部消费方 | 拆分去向 |
|---|---|---|
| `GetSubMeshes()` | Scene.cpp:404、MeshManager.cpp:181、SceneHierarchyPanel.cpp:796、Renderer3D 批处理 | **保留在 Mesh**（渲染范围 + 默认材质，渲染必需） |
| `GetVertexCount()` / `GetIndexCount()` | Renderer3D:287（draw）、编辑器统计（SceneHierarchyPanel、ResourcePanel） | 保留，改为读 `MeshData` 摘要计数 |
| `GetMaterialData()` | MeshManager.cpp:182（子网格材质名匹配） | **保留在 Mesh**（或随 MeshData 摘要） |
| `GetVertices()` / `GetIndices()` | **无任何外部调用方**，仅 Mesh.cpp 内部（上传、切线） | 上传后不保留，可整体释放 |
| `GetFilePath()` / `SetFilePath()` | SceneSerializer、编辑器、MeshManager | 保留在 Mesh |
| `IsReady()` / `InstallAsyncData` / `AsyncPendingSlot` | Renderer3D:312、编辑器、MeshManager（异步链路） | 保留在 Mesh（§4：异步状态机不动） |
| `LoadFromFile`（同步） | **0 个外部调用方**（死代码，仅注释示例提及） | **删除** |
| `LoadFromFileAsync` / `CreateBuiltin` | 各 1 个调用方，都在 `MeshManager::Load`（:153 / :133） | 编排/几何生成迁入 MeshManager，Mesh 不再持有 |

**结论**：直接支撑拆分的事实——`GetVertices()`/`GetIndices()` 是「写后读一次的废弃物」，整个 CPU 数组只服务于构建期（上传 + 切线），做完就该扔；Mesh 实际需要的只是**计数 + 子网格 + 材质数据**这类轻量摘要。

---

## 3. 目标结构

### 3.1 新增 `MeshData` —— 格式解析的统一输出（纯 CPU 载荷）

```cpp
// GE/include/GE/Render/Mesh.h（或独立 MeshData.h）
struct MeshData {
    std::vector<Vertex>     vertices;      // 已量化 + 去重；构建期用，上传后释放
    std::vector<uint32_t>   indices;
    std::vector<SubMesh>    subMeshes;     // 渲染范围 + materialName
    std::vector<MaterialData> materialData; // 材质匹配用（MeshManager 读取）
};
```

- 所有格式解析器（OBJ / 未来的 glTF / FBX）**统一输出形式** `bool ParseXxx(path, MeshData& out, err)`
- 组合 vs 继承：用**聚合**（struct 持有），不用继承（Mesh 不是一种 MeshData）

### 3.2 `Mesh` 瘦身 —— 只留可渲染所需

```cpp
class Mesh {
public:
    // 工厂：都是"MeshData → Mesh"的纯转换
    static std::unique_ptr<Mesh> Create(VulkanDevice&, MeshData&&);        // 同步：切线+上传
    static std::unique_ptr<Mesh> CreateFromBuffers(MeshData&&,             // 异步 finalize：接住后台
        std::unique_ptr<VulkanBuffer> vb, std::unique_ptr<VulkanBuffer> ib);// 已上传缓冲，只组装
    static std::unique_ptr<Mesh> CreateShell(const std::string& filepath); // 造空壳+槽位（编排在 MeshManager）

    // 渲染所需
    VulkanBuffer &GetVertexBuffer(); VulkanBuffer &GetIndexBuffer();
    const std::vector<SubMesh> &GetSubMeshes() const;
    uint32_t GetVertexCount() const; uint32_t GetIndexCount() const;      // 读摘要计数
    void SetSubMeshDefaultMaterial(uint32_t, Material*);

    // 生命周期 / 序列化
    bool IsReady() const;   const std::string &GetFilePath() const; void SetFilePath(const std::string&);
    void SetDebugName(const std::string&);

private:
    // 成员：只剩 GPU 资产 + 轻量摘要 + 文件路径 + 被动异步状态
    std::unique_ptr<VulkanBuffer> m_VertexBuffer, m_IndexBuffer;
    std::vector<SubMesh>  m_SubMeshes;      // 渲染范围（含 defaultMaterial 指针）
    std::vector<MaterialData> m_MaterialData; // 材质匹配摘要（MeshManager 读）
    uint32_t m_VertexCount = 0, m_IndexCount = 0;  // 摘要计数（替代整份 CPU 数组）
    std::string m_FilePath;
    // 异步状态（IsReady / AsyncPendingSlot / 空壳机制保留在 Mesh，§4）
};
```

> 已从 Mesh 移除：`LoadFromFile`（死代码）、`LoadFromFileAsync` / `CreateBuiltin`（或其几何生成体）
> → 全部编排迁入 `MeshManager`；`Mesh` 只剩「资源 + 被动生命周期」。

**核心收益**：`m_Vertices`/`m_Indices` 两个数百 MB 的数组从 Mesh 生命周期里消失，只留两个 uint32 计数；
`MeshData` 是临时传递对象，构建完即析构释放。

### 3.3 `Mesh.cpp` 里的解析器搬家

- `ParseModelData`（分派器）+ `ParseOBJData`（tinyobj）+ `ComputeTangents` 迁出：
  - **先落目标**：`GE/src/Render/OBJLoader.cpp`（`ParseOBJData`）、各格式解析器逐步独立
  - 与阶段 2 的 `GLTFRawLoader`（已在独立文件）同构——**格式解析器一律独立成文件，Mesh.cpp 只留装配**
- `Mesh.cpp` 收敛为：`MeshData → Mesh` 的装配（切线计算 → 上传 → 摘要提取）+ 被动异步状态；
  调度/分派/几何生成一律不在 Mesh 内（见 §4）
- 共享装配 `BuildMesh` 保留，但签名改为吃 `MeshData&&`

---

## 4. 异步状态机保留在 Mesh，编排迁入 MeshManager

**状态机不动**：空壳→就绪的被动部分（`AsyncPendingSlot` / `m_Ready` / `InstallAsyncData` / `IsReady`）继续长在
Mesh 里；AsyncUploadManager 三阶段、MeshManager「返回空壳→轮询注入」、渲染端 `IsReady()` 门控的时序**原样不变**。

**编排迁出**：`LoadFromFileAsync` 的整段主体（造空壳 → 组装 UploadTask → 提交 → finalize 里 `InstallAsyncData` +
`onInstalled`）移入 `MeshManager::Load` 文件分支；`Mesh` 只补一个 `CreateShell(filepath)` 负责「造空壳 + 初始化
槽位」（空壳生命周期仍在 Mesh，故必须保留这个最小工厂）。

```
MeshManager::Load(path)
  ├─ builtin → 生成几何 → Mesh::Create → 注册
  ├─ 文件   → Mesh::CreateShell（空壳） → 组装 UploadTask（decode/upload/finalize 结构与现在一致）
  │            finalize → InstallAsyncData(MeshData+缓冲) + onInstalled + 置 m_Ready
  └─ 注册缓存
```

#### 4.1 为什么必须保留 CreateShell

`Mesh` 的构造函数是私有的（`Mesh() = default` 在 `private:` 下），`AsyncPendingSlot` 成员也只有 Mesh 自己
能初始化。现在造空壳的代码藏在 `LoadFromFileAsync` 内部：

```cpp
// 现状（Mesh.cpp 内部，随 LoadFromFileAsync 一起搬走）：
auto mesh = std::unique_ptr<Mesh>(new Mesh());
mesh->m_AsyncSlot = std::make_shared<AsyncPendingSlot>();
mesh->m_AsyncSlot->target = mesh.get();      // 槽位指向自己，finalize 据此安全注入
```

编排迁入 MeshManager 后，MeshManager **不是 Mesh，调不到私有构造**。所以 Mesh 必须开一个小门，
把「造壳 + 初始化槽位」封装成公开工厂，让外部也能合法得到带好槽位的空壳：

```cpp
// Mesh.h，公开工厂
static std::unique_ptr<Mesh> CreateShell(const std::string &filepath) {
    auto mesh = std::unique_ptr<Mesh>(new Mesh());       // 私有构造，只能在 Mesh 内部写
    mesh->m_FilePath = filepath;
    mesh->m_AsyncSlot = std::make_shared<AsyncPendingSlot>();
    mesh->m_AsyncSlot->target = mesh.get();
    return mesh;                                          // 返回的空壳：IsReady()==false
}
```

然后再由 MeshManager 组装任务、注册、立刻返回给渲染端（`IsReady()` 为 false 期间渲染跳过）。
**CreateShell 不是给 Mesh 加新职责**——只是把原来藏在 `LoadFromFileAsync` 里的「造壳」动作留在它该在的
Mesh，好让搬走编排的 MeshManager 也能合法调用。这是「状态机留在 Mesh」决策的必然配套。

**与方案 B 的区别**：B 是把「空壳」概念整个删掉（完成即产出完整 Mesh）；这里空壳概念、`IsReady()` 门控、
延迟注入全部保留，只是「谁来调起加载」从 Mesh 挪到 MeshManager。**B 仍是否决状态，不排期。**

> 与阶段 2 的关系：阶段 2 只给 Mesh **增加** `Create` 重载 / `CreateFromBuffers`，它们在本拆分后仍是既有形态；
> 且 glTF 本就不走 `LoadFromFileAsync`（走 `GLTFImporter`），移除它对 glTF 无影响。
> 建议**顺序 = 阶段 2 完成 → 再做本拆分**。

---

## 5. 分阶段迁移步骤

### 阶段1：引入 `MeshData`，不改行为
1. 新增 `struct MeshData`，把四个 vector 成员从 Mesh 挪进它
2. `ParseModelData` / `ParseOBJData` 签名改为输出 `MeshData&`；`AsyncMeshLoadData.bucket` 内的原始数组字段替换为 `MeshData`
3. `Mesh::Create` 加 `MeshData&&` 重载；`InstallAsyncData` 改收 `MeshData&&`

### 阶段2：Mesh 成员瘦身，释放 CPU 数组
1. Mesh 内部改为持有 `m_VertexCount`/`m_IndexCount` 摘要计数，不持有原始数组
2. `GetVertices()` / `GetIndices()` 删除（无外部调用方，已核实）；`GetVertexCount()` 改读计数
3. 上传内部：`UploadToGPU` 用 `MeshData` 建缓冲后，Mesh 只回收 `m_SubMeshes`/`m_MaterialData`/计数

### 阶段3：解析器搬家 + 加载编排迁出，Mesh.cpp 归位
1. 新建 `OBJLoader.cpp`：迁入 `ParseOBJData`（含 MTL → MaterialData 捕获），与 `GLTFRawLoader` 对称
2. `ParseModelData` 分派器移入 MeshManager 或独立 `ModelLoader.cpp`（统一入口，还在）
3. `MeshManager::Load` 接管编排：文件分支改为 `Mesh::CreateShell` → 组装 UploadTask → 提交 →
   finalize 注入；builtin 分支改为内联生成几何（原 `CreateBuiltin` 体）→ `Mesh::Create` → 注册
4. 清理 `Mesh.h`：删除 `LoadFromFile`（死代码）与 `LoadFromFileAsync`；新增 `CreateShell`；
   `ParseOBJData` / `CreateBuiltin` 主体不再出现在 Mesh.cpp；`IsBuiltinPath` / `GetBuiltinType`
   判定逻辑按需移入 MeshManager 或保留为自由函数

### 阶段4：构建验证
- 全量走 `build.bat debug`，确认 OBJ 加载 / 异步加载 / 编辑器顶点统计 / 场景序列化行为不变
- 内存侧可用调试验证：加载大模型后 CPU 堆不再保留原始顶点数组

---

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| 破坏现有异步加载链路 | 状态机/时序不变；编排迁入 MeshManager 时**照搬现状结构**（decode/upload/finalize 逐字对应），唯一变的是代码所在文件 |
| 编辑器/渲染依赖原始数组 | 已核实无外部读取 `GetVertices()`/`GetIndices()`；计数与子网格访问器保留 |
| 拆完不能马上验证（用户自行构建） | 按阶段小步走，每阶段是独立可编译快照；建议仅在阶段 3 完成后整体验证一次 |
| `MeshManager::Load` 变胖 | 可控：它本就是加载的总入口，编排迁入是职责归位而非膨胀；若继续增大再抽 `ModelLoader` |
| 后续阶段 3 蒙皮需要 CPU 关节数据 | 蒙皮时 `Vertex` 增关节字段，JOINTS/WEIGHTS 属构建期数据，随 `MeshData` 临时持有即可满足，不要求 Mesh 常驻 |

---

## 7. 验收标准

- OBJ / 内置几何体加载、异步加载、场景保存加载行为与拆分前完全一致（含 `LoadFromFileAsync` 迁入
  MeshManager 后空壳/`IsReady` 时序不变）
- 编辑器网格统计（顶点/索引数）与子网格材质编辑正常
- `Mesh.h` 公开面缩减 ~1/2：去掉格式解析声明、原始数组访问器与加载/几何工厂，只剩「资源 + 摘要 + 被动异步状态」
- 加载大模型后，Mesh 生命周期内看不到整份 CPU 顶点数组常驻

---

## 8. 与现方案的衔接备忘

- 阶段 2 新增的 `BuildGLTFMesh` / `Mesh::Create(…, MeshData…)` / `CreateFromBuffers` 在拆分后是**既有形态**，
  拆分反而让 glTF 路径更顺（MeshData 正是 GLTFRawLoader 的自然输出）